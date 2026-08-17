#import <AppKit/AppKit.h>

#include "core/map/MapInfo.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/Smf.hpp"
#include "core/mesh/TerrainMesh.hpp"
#include "platform/Window.hpp"

#include <cstdio>
#include <optional>
#include <string>

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
    if (const auto infoPath = rm::mapinfo::findBesideMap(path)) {
        if (const auto range = rm::mapinfo::findVerticalRangeInFile(*infoPath)) {
            const float headerMin = field->baseHeight;
            const float headerMax =
                field->baseHeight + field->heightScale * rm::kHeightQuantisationSteps;
            std::printf("  %s overrides header range %.1f..%.1f -> %.1f..%.1f elmos\n",
                        infoPath->filename().string().c_str(),
                        static_cast<double>(headerMin), static_cast<double>(headerMax),
                        static_cast<double>(range->minHeight),
                        static_cast<double>(range->maxHeight));
            field->setVerticalRange(range->minHeight, range->maxHeight);
        }
    } else {
        std::printf("  no mapinfo.lua alongside; using the binary header range\n");
    }

    return std::move(*field);
}

} // namespace

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

        NSApplication* app = [NSApplication sharedApplication];
        // Regular = real Dock icon and keyboard focus; without this a
        // terminal-launched app is a background agent that can't take focus.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app setDelegate:[[RMAppDelegate alloc] init]];
        [app finishLaunching];

        // 1280x720 points: comfortable debug size on a laptop screen.
        rm::Window window{1280, 720, "recoil-metal — m2: SMF terrain"};
        window.setTerrain(mesh);
        window.show();

        [app activateIgnoringOtherApps:YES];
        [app run]; // never returns until the app quits
    }
    return 0;
}
