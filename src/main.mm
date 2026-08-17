#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "core/map/MapInfo.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/model/S3o.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
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
#include <span>
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

// Where BAR's reference content lives. Used only to resolve a model's texture
// names, which S3O carries but does not contain.
constexpr const char* kBarTextureDir =
    "projects/llm/games/forged-alliance-reborn/reference/BAR/unittextures";

/// How many scattered instances by default. A single unit is well under 1% of an
/// 8192-elmo map's width and effectively invisible when framed, so seeing units
/// on a map means seeing many of them.
constexpr std::size_t kDefaultUnitCount = 800;

struct UnitScene {
    rm::Model model;
    std::vector<rm::UnitInstance> instances;
    std::optional<rm::dds::Texture> diffuse;  ///< tex1: albedo + team-colour mask
    std::optional<rm::dds::Texture> shading;  ///< tex2: self-illum + reflectivity
};

/// Loads one of a model's named textures from BAR's unittextures/.
///
/// S3O names its textures but does not carry them (FAR report 05 §5.3). A
/// missing one is reported and skipped, not fatal: the shader has a defined
/// fallback for each, and half a model's shading is better than no model.
[[nodiscard]] std::optional<rm::dds::Texture> resolveModelTexture(const std::string& name,
                                                                  const char* slot) {
    if (name.empty()) {
        return std::nullopt;
    }

    const char* home = std::getenv("HOME");
    const std::filesystem::path path =
        std::filesystem::path{home == nullptr ? "" : home} / kBarTextureDir / name;

    auto texture = rm::dds::loadFile(path);
    if (!texture) {
        std::fprintf(stderr, "  no %s texture (%s): %s\n", slot, name.c_str(),
                     texture.error().message.c_str());
        return std::nullopt;
    }

    std::printf("  %s %s: %dx%d, %d mips\n", slot, name.c_str(), texture->width,
                texture->height, texture->mipLevels);
    return std::move(*texture);
}

/// Loads a model, resolves its diffuse texture, and places instances.
///
/// Placement is start positions first when the map declares them, then a
/// deterministic scatter to make up the count — spawn points read as meaningful
/// and the scatter makes the map look inhabited.
[[nodiscard]] std::optional<UnitScene> resolveUnits(const std::filesystem::path& modelPath,
                                                   const rm::HeightField& field,
                                                   std::size_t count, float scale,
                                                   std::span<const rm::mapinfo::StartPosition> starts) {
    auto model = rm::s3o::loadFile(modelPath);
    if (!model) {
        std::fprintf(stderr, "failed to load model \"%s\": %s\n",
                     modelPath.string().c_str(), model.error().message.c_str());
        return std::nullopt;
    }

    UnitScene scene;
    std::printf("model %s: %zu bones, %zu vertices, %zu triangles, radius %.1f\n",
                model->name.c_str(), model->bones.size(), model->vertices.size(),
                model->triangleCount(), static_cast<double>(model->radius));

    // Both textures, not just the diffuse: tex2 carries the self-illumination
    // and reflectivity the model was authored with, and tex1's alpha is the
    // team-colour mask that only means something once tex2's shading is there
    // to light it.
    scene.diffuse = resolveModelTexture(model->textures[0], "diffuse");
    scene.shading = resolveModelTexture(model->textures[1], "shading");

    scene.instances = rm::atStartPositions(field, starts, scale);
    const std::size_t remaining = count > scene.instances.size()
                                      ? count - scene.instances.size()
                                      : 0;
    const auto scattered = rm::scatterOnLand(field, remaining, /*seed=*/20260817u, scale);
    scene.instances.insert(scene.instances.end(), scattered.begin(), scattered.end());

    std::printf("  %zu instances (%zu at start positions, %zu scattered)\n",
                scene.instances.size(), starts.size(), scattered.size());

    scene.model = std::move(*model);
    return scene;
}

/// Parsed --bench / --bench-offscreen arguments.
/// Parsed --units arguments.
/// Parsed --screenshot arguments.
struct ShotOptions {
    bool enabled = false;
    std::string path;
    unsigned int width = 1920;
    unsigned int height = 1080;
};

[[nodiscard]] ShotOptions parseShot(int argc, const char* argv[]) {
    ShotOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} != "--screenshot") {
            continue;
        }
        options.enabled = true;
        if (i + 1 < argc) {
            options.path = argv[i + 1];
        }
        if (i + 3 < argc) {
            const int w = std::atoi(argv[i + 2]);
            const int h = std::atoi(argv[i + 3]);
            if (w > 0 && h > 0) {
                options.width = static_cast<unsigned int>(w);
                options.height = static_cast<unsigned int>(h);
            }
        }
        break;
    }
    return options;
}

struct UnitOptions {
    std::filesystem::path modelPath;
    std::size_t count = kDefaultUnitCount;
    float scale = 1.0f;
    bool focus = false;  ///< frame the first instance instead of the whole map
};

[[nodiscard]] UnitOptions parseUnits(int argc, const char* argv[]) {
    UnitOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} == "--focus") {
            options.focus = true;
            continue;
        }
        if (std::string{argv[i]} != "--units") {
            continue;
        }
        if (i + 1 < argc) {
            options.modelPath = argv[i + 1];
        }
        if (i + 2 < argc) {
            const int parsed = std::atoi(argv[i + 2]);
            if (parsed > 0) {
                options.count = static_cast<std::size_t>(parsed);
            }
        }
        if (i + 3 < argc) {
            const double parsed = std::atof(argv[i + 3]);
            if (parsed > 0.0) {
                options.scale = static_cast<float>(parsed);
            }
        }
        break;
    }

    // --focus may appear after --units' positional arguments, so it gets its own
    // pass rather than relying on ordering.
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} == "--focus") {
            options.focus = true;
        }
    }
    return options;
}

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

/// Writes BGRA8 pixels to a PNG via ImageIO — part of the OS, so no new
/// dependency. Reports rather than throwing on failure.
bool writePng(const std::string& path, const rm::Renderer::CapturedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.bgra.empty()) {
        std::fprintf(stderr, "nothing to write to %s\n", path.c_str());
        return false;
    }

    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, image.bgra.data(),
                                 static_cast<CFIndex>(image.bgra.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);

    // The drawable is BGRA8; premultiplied-first + little-endian is how
    // CoreGraphics spells that.
    CGImageRef cgImage = CGImageCreate(
        static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height), 8, 32,
        static_cast<std::size_t>(image.width) * 4, space,
        // Cast to the bitmap-info type before OR-ing: the two constants come from
        // different enums and mixing them directly is deprecated.
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst)
            | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little),
        provider, nullptr, false,
        kCGRenderingIntentDefault);

    bool ok = false;
    if (cgImage != nullptr) {
        CFStringRef cfPath = CFStringCreateWithCString(nullptr, path.c_str(),
                                                       kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle,
                                                     false);
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
        if (destination != nullptr) {
            CGImageDestinationAddImage(destination, cgImage, nullptr);
            ok = CGImageDestinationFinalize(destination);
            CFRelease(destination);
        }
        CFRelease(url);
        CFRelease(cfPath);
        CGImageRelease(cgImage);
    }

    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);

    if (ok) {
        std::printf("wrote %s (%dx%d)\n", path.c_str(), image.width, image.height);
    } else {
        std::fprintf(stderr, "failed to encode %s\n", path.c_str());
    }
    return ok;
}

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

        // Start positions come from the same mapinfo.lua already consulted for
        // the height range, so this costs one extra parse and no new plumbing.
        std::vector<rm::mapinfo::StartPosition> starts;
        if (argc >= 2) {
            if (const auto infoPath = rm::mapinfo::findBesideMap(argv[1])) {
                if (const auto info = rm::mapinfo::parseFile(*infoPath)) {
                    starts = info->startPositions;
                }
            }
        }

        const UnitOptions unitOptions = parseUnits(argc, argv);
        std::optional<UnitScene> units;
        if (!unitOptions.modelPath.empty()) {
            units = resolveUnits(unitOptions.modelPath, *field, unitOptions.count,
                                 unitOptions.scale, starts);
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
            if (units) {
                renderer.setUnits(units->model, units->instances);
                if (units->diffuse) {
                    renderer.setUnitTexture(*units->diffuse);
                }
                if (units->shading) {
                    renderer.setUnitShadingTexture(*units->shading);
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

        // --- Headless screenshot -------------------------------------------
        // No window, so this works regardless of which Space is active — the
        // reason it exists.
        const ShotOptions shot = parseShot(argc, argv);
        if (shot.enabled) {
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            if (argc >= 2) {
                if (auto atlas = resolveAtlas(argv[1])) {
                    renderer.setGroundTexture(*atlas);
                }
            }
            if (units) {
                renderer.setUnits(units->model, units->instances);
                if (units->diffuse) {
                    renderer.setUnitTexture(*units->diffuse);
                }
                if (units->shading) {
                    renderer.setUnitShadingTexture(*units->shading);
                }
                if (unitOptions.focus && !units->instances.empty()) {
                    constexpr float kRadiiBack = 2.5f;
                    const auto& first = units->instances.front();
                    renderer.focusOn(first.position,
                                     std::max(units->model.radius, 1.0f) * kRadiiBack
                                         * unitOptions.scale);
                }
            }

            const auto image = renderer.renderToImage(shot.width, shot.height);
            return writePng(shot.path, image) ? 0 : 1;
        }

        NSApplication* app = [NSApplication sharedApplication];
        // Regular = real Dock icon and keyboard focus; without this a
        // terminal-launched app is a background agent that can't take focus.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app setDelegate:[[RMAppDelegate alloc] init]];
        [app finishLaunching];

        // 1280x720 points: comfortable debug size on a laptop screen.
        rm::Window window{1280, 720, "recoil-metal — m5: units on SMF terrain"};
        window.setTerrain(mesh);
        if (argc >= 2) {
            if (auto atlas = resolveAtlas(argv[1])) {
                window.setGroundTexture(*atlas);
            }
        }
        if (units) {
            window.setUnits(units->model, units->instances);
            if (units->diffuse) {
                window.setUnitTexture(*units->diffuse);
            }
            if (units->shading) {
                window.setUnitShadingTexture(*units->shading);
            }

            // Framing the whole map makes a unit about two pixels across, which
            // says nothing about whether it is textured correctly. --focus looks
            // at the first instance from a few model-radii out instead.
            if (unitOptions.focus && !units->instances.empty()) {
                constexpr float kRadiiBack = 2.5f;
                const auto& first = units->instances.front();
                window.focusOn(first.position,
                               std::max(units->model.radius, 1.0f) * kRadiiBack
                                   * unitOptions.scale);
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
