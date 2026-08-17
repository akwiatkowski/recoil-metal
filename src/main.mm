#import <AppKit/AppKit.h>

#include "core/map/MapInfo.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/mesh/TerrainMesh.hpp"
#include "platform/Window.hpp"
#include "render/Renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Quits the process when the last window closes. Without a delegate the app
// lingers windowless in the Dock, which is endlessly confusing in a terminal
// workflow. (Objective-C declarations must be at global scope — the RM
// prefix is the namespacing mechanism, per Cocoa convention.)
@interface RMAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation RMAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}
@end

namespace {

// Fallback map size when no .smf is supplied: 256 squares is 2048 elmos across,
// big enough to look like terrain and small enough to build instantly.
constexpr int kDemoSquares = 256;

// Vertical range for the demo, chosen to match the relief ratio real content
// actually has. FAR measured typical SupCom relief at 3-6% of map width (report
// 06 section 8.2) and BAR maps are somewhat steeper; 400 elmos over a 2048-elmo
// map is about 20%, which reads as proper hills.
//
// The temptation is to span the format's full 4096-elmo range because the
// heightmap is 16-bit — but real maps use only a slice of that range, and using
// all of it produces vertical needles rather than terrain. Learned by looking.
constexpr float kDemoMinHeight = -40.0f;
constexpr float kDemoMaxHeight = 360.0f;

/// Loads the map named on the command line, or synthesises one if none is given.
/// Returns std::nullopt only when an explicitly requested map failed to load —
/// falling back silently there would hide the error the user needs to see.
[[nodiscard]] std::optional<rm::HeightField> resolveHeightField(int argc, const char* argv[]) {
    if (argc < 2) {
        std::printf("no map given, rendering a procedural field\n"
                    "  usage: recoil-metal <path/to/map.smf>\n");
        return rm::makeSineHills(kDemoSquares, kDemoSquares, kDemoMinHeight, kDemoMaxHeight);
    }

    const std::string path = argv[1];
    auto field = rm::smf::loadFile(path);
    if (!field) {
        std::fprintf(stderr, "failed to load \"%s\": %s\n",
                     path.c_str(), field.error().message.c_str());
        return std::nullopt;
    }

    std::printf("loaded %s: %d x %d squares (%.0f x %.0f elmos)\n",
                path.c_str(), field->squaresX, field->squaresZ,
                static_cast<double>(field->widthElmos()),
                static_cast<double>(field->depthElmos()));

    // mapinfo.lua overrides the binary header's vertical range. Skipping this
    // renders maps with an inverted header upside down — BAR's Angel Crossing
    // is one such map. See core/map/MapInfo.hpp.
    const auto infoPath = rm::mapinfo::findBesideMap(path);
    if (!infoPath) {
        std::printf("  no mapinfo.lua alongside; using the binary header range\n");
        return std::move(*field);
    }

    const auto info = rm::mapinfo::parseFile(*infoPath);
    if (!info) {
        // A mapinfo.lua we cannot read is worth saying out loud rather than
        // ignoring: the header values we fall back to may well be wrong.
        std::fprintf(stderr, "  warning: could not read %s (%s); keeping header range\n",
                     infoPath->filename().string().c_str(), info.error().message.c_str());
        return std::move(*field);
    }

    if (info->verticalRange) {
        const float headerMin = field->baseHeight;
        const float headerMax =
            field->baseHeight + field->heightScale * rm::kHeightQuantisationSteps;
        std::printf("  %s overrides header range %.1f..%.1f -> %.1f..%.1f elmos\n",
                    infoPath->filename().string().c_str(),
                    static_cast<double>(headerMin), static_cast<double>(headerMax),
                    static_cast<double>(info->verticalRange->minHeight),
                    static_cast<double>(info->verticalRange->maxHeight));
        field->setVerticalRange(info->verticalRange->minHeight,
                                info->verticalRange->maxHeight);
    }

    return std::move(*field);
}

/// Loads and assembles the ground texture for a map, if its .smt is on disk.
///
/// A missing .smt is not fatal — the engine tolerates it too, rendering those
/// tiles flat (SMFGroundTextures.cpp:147-157) — so this reports and returns
/// nullopt rather than failing the launch.
[[nodiscard]] std::optional<rm::TileAtlas> resolveAtlas(const std::filesystem::path& smfPath) {
    const auto index = rm::smf::loadTileIndexFile(smfPath);
    if (!index) {
        std::fprintf(stderr, "  no tile index: %s\n", index.error().message.c_str());
        return std::nullopt;
    }

    // mapinfo.lua may rename the tile files; the engine only honours the
    // override when the count matches (SMFGroundTextures.cpp:122-127).
    std::vector<std::string> names = index->smtFileNames;
    if (const auto infoPath = rm::mapinfo::findBesideMap(smfPath)) {
        if (const auto info = rm::mapinfo::parseFile(*infoPath)) {
            if (info->smtFileNames.size() == names.size()) {
                names = info->smtFileNames;
            }
        }
    }

    if (names.empty()) {
        std::fprintf(stderr, "  map declares no tile files\n");
        return std::nullopt;
    }
    if (names.size() > 1) {
        // Multi-file tile sets concatenate their index spaces. Nothing here
        // stops that working, but no test covers it, so say so rather than
        // quietly rendering the first file's tiles for every index.
        std::fprintf(stderr, "  warning: %zu tile files; only the first is loaded\n",
                     names.size());
    }

    // Names are relative to the map file's directory (SMFGroundTextures.cpp:138-145).
    const std::filesystem::path smtPath =
        smfPath.parent_path() / std::filesystem::path{names[0]}.filename();

    const auto tiles = rm::smt::loadFile(smtPath);
    if (!tiles) {
        std::fprintf(stderr, "  no ground texture (%s): %s\n",
                     smtPath.filename().string().c_str(), tiles.error().message.c_str());
        return std::nullopt;
    }

    auto atlas = rm::buildTileAtlas(*index, *tiles);
    if (!atlas) {
        std::fprintf(stderr, "  could not build atlas: %s\n", atlas.error().message.c_str());
        return std::nullopt;
    }

    std::printf("texture: %d x %d texels, %d tiles, %zu MiB BC1\n",
                atlas->widthTexels, atlas->heightTexels, tiles->tileCount,
                atlas->data.size() / (1024 * 1024));
    return std::move(*atlas);
}

/// Parsed --bench / --bench-offscreen arguments.
struct BenchOptions {
    bool enabled = false;
    bool offscreen = false;  ///< no window, no vsync
    std::size_t frames = 600;
    std::size_t warmup = 60;
    unsigned int width = 1920;
    unsigned int height = 1080;
    std::string csvPath;
};

/// Recognises `--bench <frames> <out.csv>` anywhere after the map path.
[[nodiscard]] BenchOptions parseBench(int argc, const char* argv[]) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg != "--bench" && arg != "--bench-offscreen") {
            continue;
        }
        options.enabled = true;
        options.offscreen = (arg == "--bench-offscreen");
        if (i + 1 < argc) {
            options.frames = static_cast<std::size_t>(std::max(1, std::atoi(argv[i + 1])));
        }
        if (i + 2 < argc) {
            options.csvPath = argv[i + 2];
        }
        break;
    }
    return options;
}

} // namespace

// Polls the renderer and quits once the benchmark has collected enough frames.
// A timer rather than a counter inside drawFrame: the display link owns the
// frame loop, and terminating from inside its callback is asking for trouble.
@interface RMBenchWatcher : NSObject
@property(nonatomic, assign) rm::Window* window;
@property(nonatomic, assign) NSUInteger targetFrames;
@property(nonatomic, copy) NSString* csvPath;
@end

@implementation RMBenchWatcher

- (void)tick:(NSTimer*)timer {
    if (self.window->recordedFrames() < self.targetFrames) {
        return;
    }
    [timer invalidate];

    const rm::bench::FrameRecorder recorder = self.window->benchmarkSnapshot();
    std::printf("%s\n", recorder.summaryLine("recoil-metal").c_str());
    std::printf("  note: cpu ms is display-paced by CAMetalDisplayLink; gpu ms is the\n"
                "        renderer's own cost and is the number worth comparing.\n");

    if (self.csvPath.length > 0) {
        const std::string csv = recorder.toCsv();
        std::ofstream out{self.csvPath.UTF8String, std::ios::binary};
        if (out) {
            out << csv;
            std::printf("  wrote %s (%zu frames)\n", self.csvPath.UTF8String,
                        recorder.recorded());
        } else {
            std::fprintf(stderr, "  could not write %s\n", self.csvPath.UTF8String);
        }
    }

    [NSApp terminate:nil];
}

@end

/// Writes a recorder's per-frame CSV, reporting rather than throwing on failure.
void writeCsv(const std::string& path, const rm::bench::FrameRecorder& recorder) {
    if (path.empty()) {
        return;
    }
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "  could not write %s\n", path.c_str());
        return;
    }
    out << recorder.toCsv();
    std::printf("  wrote %s (%zu frames)\n", path.c_str(), recorder.recorded());
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const auto field = resolveHeightField(argc, argv);
        if (!field) {
            return 1;
        }

        const rm::TerrainMesh mesh = rm::buildTerrainMesh(*field);
        std::printf("terrain: %zu vertices, %zu triangles, height %.1f..%.1f elmos\n",
                    mesh.vertices.size(), mesh.triangleCount(),
                    static_cast<double>(mesh.minY), static_cast<double>(mesh.maxY));

        const BenchOptions bench = parseBench(argc, argv);

        // --- Headless offscreen benchmark ----------------------------------
        // No NSApplication, no window, no display link, hence no vsync. This is
        // the only mode whose CPU numbers describe the renderer instead of the
        // display, so it is the one comparable against another engine.
        if (bench.enabled && bench.offscreen) {
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            if (argc >= 2) {
                if (auto atlas = resolveAtlas(argv[1])) {
                    renderer.setGroundTexture(*atlas);
                }
            }

            std::printf("offscreen benchmark: %ux%u, %zu frames (discarding %zu warmup),"
                        " %zu frames in flight, no vsync\n",
                        bench.width, bench.height, bench.frames, bench.warmup,
                        rm::Renderer::kMaxFramesInFlight);

            const rm::bench::FrameRecorder recorder =
                renderer.runOffscreenBenchmark(bench.width, bench.height, bench.frames,
                                               bench.warmup);

            std::printf("%s\n", recorder.summaryLine("recoil-metal offscreen").c_str());
            writeCsv(bench.csvPath, recorder);
            return 0;
        }

        NSApplication* app = [NSApplication sharedApplication];
        // Regular = real Dock icon and keyboard focus; without this a
        // terminal-launched app is a background agent that can't take focus.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app setDelegate:[[RMAppDelegate alloc] init]];
        [app finishLaunching];

        // 1280x720 points: comfortable debug size on a laptop screen.
        rm::Window window{1280, 720, "recoil-metal — m3: textured SMF terrain"};
        window.setTerrain(mesh);
        if (argc >= 2) {
            if (auto atlas = resolveAtlas(argv[1])) {
                window.setGroundTexture(*atlas);
            }
        }
        window.show();

        RMBenchWatcher* watcher = nil;
        if (bench.enabled) {
            std::printf("benchmarking %zu frames (discarding %zu warmup)\n",
                        bench.frames, bench.warmup);
            window.beginBenchmark(bench.warmup);

            watcher = [[RMBenchWatcher alloc] init];
            watcher.window = &window;
            watcher.targetFrames = bench.frames;
            watcher.csvPath = bench.csvPath.empty()
                                  ? @""
                                  : [NSString stringWithUTF8String:bench.csvPath.c_str()];
            [NSTimer scheduledTimerWithTimeInterval:0.1
                                             target:watcher
                                           selector:@selector(tick:)
                                           userInfo:nil
                                            repeats:YES];
        }

        [app activateIgnoringOtherApps:YES];
        [app run]; // never returns until the app quits
    }
    return 0;
}
