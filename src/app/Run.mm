#include "app/Run.hpp"

#include "core/audio/CueEvents.hpp"
#include "core/audio/Xwb.hpp"
#include "core/audio/Cues.hpp"
#include "platform/Audio.hpp"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include "core/sim/Adjacency.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/StateHash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

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

namespace rm::app {

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

namespace {

// The session's fields, named the way the code that used to be inside `main` named them. A
// Every name is `[[maybe_unused]]`: a benchmark reads no start positions and a screenshot
// opens no window, so a shared session is by nature partly unread by each of its users.
// `using`-style unpacking rather than a rename sweep: the bodies below are the same statements
// they were, which is what makes this move checkable by comparing a screenshot.
#define RM_UNPACK_SESSION(s)                                                                   \
    [[maybe_unused]] const int argc = (s).argc;                                                 \
    [[maybe_unused]] const char** argv = (s).argv;                                              \
    [[maybe_unused]] const LoadedMap* map = &(s).map;                                                           \
    [[maybe_unused]] UnitScene& units = (s).units;                                                              \
    [[maybe_unused]] const PropScene& props = (s).props;                                                        \
    [[maybe_unused]] PassabilitySet& passability = (s).passability;                                              \
    [[maybe_unused]] const rm::vfs::Vfs& content = (s).content;                                                 \
    [[maybe_unused]] const rm::Settings& settings = (s).settings;                                               \
    [[maybe_unused]] const rm::TerrainMesh& mesh = (s).mesh;                                                    \
    [[maybe_unused]] const std::span<const rm::mapinfo::StartPosition> starts = (s).starts;                      \
    [[maybe_unused]] std::vector<rm::Particle>& marchDust = (s).marchDust;                                      \
    [[maybe_unused]] const ShotOptions shot = (s).shot;                                                          \
    [[maybe_unused]] const BenchOptions bench = (s).bench;                                                       \
    [[maybe_unused]] const MarchOptions marchOptions = (s).march;                                                \
    [[maybe_unused]] const LookOptions look = (s).look;                                                          \
    [[maybe_unused]] const float focus = (s).focus;                                                              \
    [[maybe_unused]] const float animationTime = (s).animationTime;                                              \
    [[maybe_unused]] const std::size_t propInstances = (s).propInstances;

void appendVisibleWreckDecals(std::vector<rm::DecalVertex>& out, const UnitScene& units) {
    for (std::size_t i = 0; i + 2 < units.wreckDecals.size(); i += 3) {
        const rm::DecalVertex& first = units.wreckDecals[i];
        if (units.visibleToViewer(rm::sim::fxFromFloat(first.position[0]),
                                  rm::sim::fxFromFloat(first.position[2]))) {
            out.insert(out.end(), units.wreckDecals.begin() + static_cast<std::ptrdiff_t>(i),
                       units.wreckDecals.begin() + static_cast<std::ptrdiff_t>(i + 3));
        }
    }
}

void gatherVisibleProjectiles(std::vector<rm::sim::Projectile>& out, const UnitScene& units,
                              float alpha = 0.0f) {
    out.clear();
    for (const rm::sim::Projectile& projectile : units.projectiles) {
        const rm::sim::Fx x = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[0])
            + rm::sim::fxToFloat(projectile.velocity[0]) * alpha);
        const rm::sim::Fx z = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[2])
            + rm::sim::fxToFloat(projectile.velocity[2]) * alpha);
        if (units.visibleToViewer(x, z)) {
            out.push_back(projectile);
        }
    }
}

void appendVisibleParticles(std::vector<rm::Particle>& out, std::span<const rm::Particle> source,
                            const UnitScene& units) {
    for (const rm::Particle& particle : source) {
        if (units.visibleToViewer(rm::sim::fxFromFloat(particle.origin[0]),
                                  rm::sim::fxFromFloat(particle.origin[2]))) {
            out.push_back(particle);
        }
    }
}

void gatherVisibleEvents(std::vector<rm::sim::Event>& out, const UnitScene& units) {
    out.clear();
    units.refreshViewerContacts();
    for (const rm::sim::Event& event : units.events.all()) {
        const bool matchEvent = event.kind == rm::sim::EventKind::TeamDefeated
                             || event.kind == rm::sim::EventKind::GameOver;
        const bool alliedUnitEvent = event.kind != rm::sim::EventKind::ProjectileImpact
                                  && units.alliedWithViewer(event.army);
        if (matchEvent || alliedUnitEvent || units.visibleToViewer(event.unit)
            || units.visibleToViewer(event.at[0], event.at[2])) {
            out.push_back(event);
        }
    }
}

} // namespace

int runOffscreenBenchmark(const Session& session) {
    RM_UNPACK_SESSION(session)
        // --- Headless offscreen benchmark ----------------------------------
        // No NSApplication, no window, no display link, hence no vsync. This is
        // the only mode whose CPU numbers describe the renderer instead of the
        // display, so it is the one comparable against another engine.
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            applyGround(renderer, *map);
            renderer.setUnits(units.textures.all(), units.batches);
            units.applyFog(renderer);
            renderer.setProps(props.textures.all(), props.batches);
            renderer.setAnimationTime(animationTime);
            renderer.setReflections(settings.reflections);
            renderer.setStratumNormals(settings.stratumNormals);
            renderer.setRefraction(settings.refraction);
            // The march's dust, so a benchmark measures the same scene a capture
            // shows rather than one without particles in it — plus the icons, for the
            // units the camera has left too small to read.
            std::vector<rm::Particle> captureParticles;
            captureParticles.assign(marchDust.begin(), marchDust.end());
            appendSceneIcons(captureParticles, units, renderer.camera());
            renderer.setParticles(captureParticles);
            // --focus works here too, so the benchmark can measure a close
            // camera as well as a whole-map one. They are different workloads:
            // anything that culls to what the camera sees is invisible at full
            // zoom and everything up close.
            if (focus > 0.0f) {
                focusOnFirstUnit(renderer, units, focus);
            }

            std::printf("offscreen benchmark: %ux%u, %zu frames (discarding %zu warmup),"
                        " %zu frames in flight, no vsync\n",
                        bench.width, bench.height, bench.frames, bench.warmup,
                        rm::Renderer::kMaxFramesInFlight);
            std::printf("  %s\n", describeQuality(settings, propInstances, marchDust.size()).c_str());

            const rm::bench::FrameRecorder recorder =
                renderer.runOffscreenBenchmark(bench.width, bench.height, bench.frames,
                                               bench.warmup);

            std::printf("%s\n", recorder.summaryLine("recoil-metal offscreen").c_str());
            writeCsv(bench.csvPath, recorder);
    return 0;
}

// --- Headless screenshot -------------------------------------------------------------------
// No window, so this works regardless of which Space is active — the reason it exists.
int runScreenshot(const Session& session) {
    RM_UNPACK_SESSION(session)
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            applyGround(renderer, *map);
            renderer.setUnits(units.textures.all(), units.batches);
            units.applyFog(renderer);
            renderer.setProps(props.textures.all(), props.batches);
            renderer.setAnimationTime(animationTime);
            renderer.setReflections(settings.reflections);
            renderer.setStratumNormals(settings.stratumNormals);
            renderer.setRefraction(settings.refraction);
            if (focus > 0.0f) {
                focusOnFirstUnit(renderer, units, focus);
            }
            if (look.enabled) {
                renderer.focusOn({look.x, map->field.heightAtWorld(look.x, look.z), look.z},
                                 look.radiusElmos);
            }

            // Selection rings need a selection, and a headless run has no
            // clicks. `--select N` rings the first N units so that what a
            // click produces can be captured and compared between builds —
            // otherwise the one piece of interface this renderer draws is the
            // one thing no screenshot can show.
            // The ground decals for a capture. WRECKS FIRST and unconditionally, because
            // they are a fact about the battlefield rather than a piece of interface — the
            // rest of this block used to be gated on `--select`, which meant a scorch mark
            // only appeared in a screenshot that also happened to be ringing units.
            std::vector<rm::sim::UnitId> capturedSelection;
            std::vector<rm::DecalVertex> vertices;
            appendVisibleWreckDecals(vertices, units);
            {
                const std::size_t rings = parseCount(argc, argv, "--select");
                std::vector<rm::SelectionEntry> captured;
                // The same selection as UnitIds, so the BUILD PANEL can be captured too. The
                // comment above says why this matters: interface that only exists under a click
                // is interface no screenshot can compare between builds, and the build panel is
                // the largest piece of it.
                capturedSelection.clear();
                std::size_t made = 0;
                for (std::size_t batch = 0; batch < units.drawScratch.size() && made < rings;
                     ++batch) {
                    for (std::size_t i = 0; i < units.drawScratch[batch].size() && made < rings;
                         ++i, ++made) {
                        const rm::UnitIndex slot = units.drawSlotOf[batch][i];
                        const rm::sim::Transform& at = units.store.transforms()[slot];
                        const std::array<float, 3> ground{rm::sim::fxToFloat(at.x),
                                                          rm::sim::fxToFloat(at.y),
                                                          rm::sim::fxToFloat(at.z)};
                        rm::appendSelectionRing(
                            vertices, map->field, ground,
                            rm::sim::fxToFloat(units.store.motion()[slot].radiusElmos)
                                * kSelectionRingMargin,
                            kSelectionRingColour);
                        // The range ring in a capture too, or the feature is unverifiable —
                        // same reasoning and same arithmetic as the windowed loop's.
                        if (const rm::unitdef::UnitDef* def =
                                units.catalog.def(units.store.typeAt(slot))) {
                            rm::sim::Fx reach{};
                            for (const rm::unitdef::Weapon& weapon : def->weapons) {
                                if (weapon.fires() && weapon.maxRange > reach) {
                                    reach = weapon.maxRange;
                                }
                            }
                            if (reach > rm::sim::Fx{}) {
                                rm::appendSelectionRing(vertices, map->field, ground,
                                                        rm::sim::fxToFloat(reach),
                                                        kRangeRingColour,
                                                        kRangeRingThicknessElmos);
                            }
                        }
                        captured.push_back(rm::SelectionEntry{batch, i});
                        capturedSelection.push_back(units.store.idAt(slot));
                    }
                }
                // No beginFrame: that acquires a frames-in-flight slot which
                // only drawFrame releases, and a capture goes through
                // renderToImage instead. Without it the slot stays 0, which is
                // the same slot encodeScene will read — consistent, because
                // nothing here is pipelined.
                // An order marker too, where --march sent them. A right-click
                // cannot be screenshotted any more than a left-click can, and
                // --march already names the destination, so the flag that orders
                // the move is also the one that marks it.
                if (marchOptions.enabled && marchOptions.orderAll) {
                    rm::appendOrderMarker(vertices, map->field,
                                          {{marchOptions.x, 0.0f, marchOptions.z}},
                                          kOrderMarkerColour, /*age=*/0.0f);
                }

                renderer.setGroundDecals(vertices);
                renderer.setSelection(captured);
                std::printf("  selected %zu units for the capture\n", made);
            }

            // The dust the march raised, if there was one. Nothing else in a
            // headless capture can produce a particle: dust records movement, and
            // only --march moves anything without a window.
            // The march's dust, plus icons for whatever the camera has left too small to
            // read. The SCREENSHOT path, which is the one that has to show them: a feature
            // only visible in a live window cannot be verified, and AGENT.md asks for a
            // screenshot or it did not happen.
            // Filled AFTER the icon atlas below decides which types the strategic layer
            // draws — the squares are the fallback for the glyphless, and appending them
            // first would draw both.
            std::vector<rm::Particle> shotParticles;
            appendVisibleParticles(shotParticles, marchDust, units);

            // The HUD in a capture too. A screenshot is how this project verifies anything,
            // and an interface only visible in a live window cannot be checked at all.
            rm::ui::Geometry hud;
            rm::ui::build(hud, renderer.labelFont(), renderer.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, marchOptions.seconds),
                          static_cast<float>(shot.width),
                          static_cast<float>(shot.height));

            // Health bars in a capture too, for the usual reason: a battle screenshot is
            // the one place a damaged unit reliably exists to verify them against.
            appendHealthBars(hud, units, renderer.camera(), renderer.labelFont(),
                             static_cast<float>(shot.width),
                             static_cast<float>(shot.height));

            // THE MINIMAP IN A CAPTURE TOO, for the same reason the rest of the HUD is here: a
            // screenshot is how this project verifies anything, and an interface only visible in
            // a live window cannot be checked at all. Adding it to the frame loop alone is how
            // the first version of P7.4 looked correct and produced an empty corner in every
            // capture.
            std::vector<rm::ui::MinimapPip> pips;
            std::vector<std::array<float, 2>> view;
            appendMinimapPips(pips, units);
            appendViewFootprint(view, renderer.camera(), map->field,
                                static_cast<float>(shot.width), static_cast<float>(shot.height));
            const rm::ui::MinimapLayout shotMinimap = rm::ui::minimapLayout(
                static_cast<float>(shot.width), static_cast<float>(shot.height));
            const bool shotPreview = map->preview.width > 0;
            if (shotPreview) {
                renderer.setMinimapRect(shotMinimap.x + shotMinimap.inset,
                                        shotMinimap.y + shotMinimap.inset,
                                        shotMinimap.size - shotMinimap.inset * 2.0f,
                                        shotMinimap.size - shotMinimap.inset * 2.0f);
            }
            rm::ui::appendMinimap(hud, renderer.labelFont(), hudThemeFor(units), shotMinimap,
                                  map->field.widthElmos(), map->field.depthElmos(), pips, view,
                                  !shotPreview);

            // The build panel and the roster, for whatever `--select` ringed. This is what
            // makes either verifiable at all: a headless run has no clicks, so `--select` is
            // the only way interface that appears on selection reaches a screenshot.
            //
            // NOT SCOPED TO THE BUILD PANEL, which the first version was: the icons were packed
            // inside `if (!shotOptions.empty())`, so a selection of tanks — nothing that builds
            // — drew a roster with no pictures in it.
            std::vector<rm::ui::BuildOption> shotOptions;
            std::vector<rm::ui::RosterTile> shotRoster;
            rm::app::BuildSelection shotWho;
            gatherRoster(units, capturedSelection, shotRoster);
            gatherBuildOptions(units, capturedSelection, hudThemeFor(units), shotOptions,
                               shotWho);

            // Packed unconditionally now: the strategic glyphs exist with nothing selected
            // at all, which is precisely the far-zoom capture that shows them.
            rm::app::ensureStrategicIconArt(units, content);
            std::size_t shotStrategicBase = 0;
            renderer.setIconAtlas(
                rm::app::packInterfaceIcons(content, shotOptions, shotRoster,
                                            units.strategicIconArt, &shotStrategicBase));
            std::vector<std::optional<rm::app::StrategicIconRef>> shotRefs;
            rm::app::buildStrategicIconRefs(units, shotStrategicBase, shotRefs);
            rm::app::appendStrategicIcons(hud, units, renderer.camera(),
                                          static_cast<float>(shot.width),
                                          static_cast<float>(shot.height), shotRefs);
            rm::app::appendContactBlips(hud, units, renderer.camera(), map->field,
                                        renderer.labelFont(), static_cast<float>(shot.width),
                                        static_cast<float>(shot.height));
            appendSceneIcons(shotParticles, units, renderer.camera(), shotRefs);
            // The shots in flight at the captured tick — the reason a battle screenshot
            // finally shows the battle. No trails headless: the capture has no aging
            // particle list for them to fade through.
            std::vector<rm::sim::Projectile> shotProjectiles;
            gatherVisibleProjectiles(shotProjectiles, units);
            rm::appendProjectiles(shotParticles, shotProjectiles, 0.0f,
                                  renderer.camera().elmosPerPoint(
                                      rm::kIconReferenceHeightPoints));
            renderer.setParticles(shotParticles);

            if (!shotOptions.empty()) {
                // `--hover N` lights the Nth option (1-based) and draws its info card, for
                // the same reason `--select` exists: a headless run has no cursor, and
                // interface that appears only under one cannot reach a screenshot.
                const std::size_t hoverAt = parseCount(argc, argv, "--hover");
                const std::optional<std::size_t> shotHovered =
                    hoverAt > 0 && hoverAt <= shotOptions.size()
                        ? std::optional<std::size_t>{hoverAt - 1}
                        : std::nullopt;
                const rm::ui::BuildPanelLayout shotPanel =
                    rm::ui::buildPanelLayout(shotMinimap, shotOptions.size());
                rm::ui::appendBuildPanel(hud, renderer.labelFont(), renderer.readoutFont(),
                                         hudThemeFor(units), shotPanel, shotOptions,
                                         shotHovered, shotWho.name, shotWho.role);
                if (shotHovered) {
                    const rm::ui::InfoCard card =
                        rm::ui::buildOptionCard(shotOptions[*shotHovered]);
                    const float cardHeight = rm::ui::infoCardHeight(
                        renderer.labelFont().lineHeight, card.rows.size());
                    rm::ui::appendInfoCard(hud, renderer.labelFont(), renderer.readoutFont(),
                                           hudThemeFor(units), shotPanel.x,
                                           shotPanel.y - cardHeight, shotPanel.width, card);
                }
                std::printf("  build panel: %zu options for %s\n", shotOptions.size(),
                            shotWho.name.c_str());

                // `--ghost X Z`: the silhouette of the hovered option (or the first) at a
                // world point, for the reason --select and --hover exist — the ghost only
                // appears under a cursor, and a headless run has none.
                for (int i = 1; i + 2 < argc; ++i) {
                    if (std::string{argv[i]} != "--ghost") {
                        continue;
                    }
                    const float gx = static_cast<float>(std::atof(argv[i + 1]));
                    const float gz = static_cast<float>(std::atof(argv[i + 2]));
                    const std::size_t option = shotHovered.value_or(0);
                    const std::string path =
                        rm::data::RosterEntry{.id = shotOptions[option].id}.path();
                    const std::optional<rm::UnitTypeIndex> type =
                        ensureDrawableType(units, content, path);
                    const std::size_t batch =
                        type ? units.batchOf(*type) : UnitScene::kNoBatch;
                    if (batch != UnitScene::kNoBatch) {
                        // The batch grew after the upload above, so upload again — the
                        // windowed loop's growth check, done by hand.
                        renderer.setUnits(units.textures.all(), units.batches);
                        const auto typeIndex = static_cast<std::size_t>(*type);
                        renderer.setGhost(
                            batch,
                            rm::UnitInstance{
                                .position = {{gx, map->field.heightAtWorld(gx, gz), gz}},
                                .rotationY = 0.0f,
                                .scale = typeIndex < units.typeScale.size()
                                             ? units.typeScale[typeIndex]
                                             : 1.0f,
                            },
                            kBuildGhostColour);
                        std::printf("  ghost: %s at %.0f, %.0f\n",
                                    shotOptions[option].id.c_str(),
                                    static_cast<double>(gx), static_cast<double>(gz));
                    }
                    break;
                }
            }

            if (!shotRoster.empty()) {
                rm::ui::appendRoster(hud, renderer.labelFont(), renderer.readoutFont(),
                                     hudThemeFor(units),
                                     rm::ui::rosterLayout(static_cast<float>(shot.width),
                                                          static_cast<float>(shot.height),
                                                          shotRoster.size()),
                                     shotRoster, std::nullopt);
                std::printf("  roster: %zu type(s) selected\n", shotRoster.size());
            }

            renderer.setHud(hud.label, hud.readout, hud.image, hud.worldImage);

            const auto image = renderer.renderToImage(shot.width, shot.height);
            return writePng(shot.path, image) ? 0 : 1;
}

// --- The game -------------------------------------------------------------------------------
int runWindowed(const Session& session) {
    RM_UNPACK_SESSION(session)
        NSApplication* app = [NSApplication sharedApplication];
        // Regular = real Dock icon and keyboard focus; without this a
        // terminal-launched app is a background agent that can't take focus.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app setDelegate:[[RMAppDelegate alloc] init]];
        [app finishLaunching];

        // 1280x720 points: comfortable debug size on a laptop screen.
        rm::Window window{1280, 720, "recoil-metal — m8: movable units"};

        // SOUND, windowed only: the headless paths are captures and a capture is silent.
        // The mixer lives on the stack beside the window; the output pulls from it on the
        // audio thread and a machine with no device just plays the match mute — the same
        // degradation `--mute` asks for on purpose.
        rm::audio::Mixer mixer;
        rm::audio::Output audioOutput;
        if (hasFlag(session.argc, session.argv, "--mute")) {
            mixer.setMasterGain(0.0f);
        } else {
            // `--volume 0..100`: the master gain, for tuning the mix by ear without a
            // rebuild. Absent means the mixer's own default.
            if (const std::size_t volume = parseCount(session.argc, session.argv, "--volume");
                volume > 0) {
                mixer.setMasterGain(static_cast<float>(std::min<std::size_t>(volume, 100))
                                    / 100.0f);
            }
            (void)audioOutput.start(mixer);
        }

        // The game's own booms: FA ships its audio as loose wave banks beside gamedata
        // (`<install>/sounds`), all plain PCM (see core/audio/Xwb.hpp). Absent — a
        // procedural map, a missing drive — the synthesised cues carry on.
        std::optional<rm::audio::WaveBank> explosionBank;
        std::optional<rm::audio::WaveBank> impactBank;
        for (int i = 1; i + 1 < session.argc; ++i) {
            if (std::string_view{session.argv[i]} == "--gamedata") {
                const std::filesystem::path sounds =
                    std::filesystem::path{session.argv[i + 1]}.parent_path() / "sounds";
                explosionBank = rm::audio::loadWaveBank(sounds / "Explosions.xwb");
                impactBank = rm::audio::loadWaveBank(sounds / "Impacts.xwb");
                if (explosionBank) {
                    std::printf("audio: %zu explosion(s), %zu impact(s) from the game's own"
                                " banks\n",
                                explosionBank->entries.size(),
                                impactBank ? impactBank->entries.size() : 0);
                }
                break;
            }
        }
        window.setTerrain(mesh);
        applyGround(window, *map);
        window.setProps(props.textures.all(), props.batches);

        // Only the windowed path steps the sim, so only it can pace a walk
        // cycle by distance. The headless paths — screenshots and benchmarks —
        // stay on the renderer's clock, which is what makes `--time` mean
        // something there and keeps a captured frame reproducible.
        for (rm::UnitBatch& batch : units.batches) {
            batch.animationDrivenByInstance = true;
        }
        window.setUnits(units.textures.all(), units.batches);
        if (focus > 0.0f) {
            focusOnFirstUnit(window, units, focus);
        }

        // The planar reflection is half the frame for something Fresnel largely
        // hides at this camera angle, so it is a setting rather than a fact.
        // `r` flips it live, which is the only way to judge whether it is worth
        // its cost — a side-by-side of two runs cannot show the difference
        // moving.
        window.setReflections(settings.reflections);
        window.setStratumNormals(settings.stratumNormals);
        window.setRefraction(settings.refraction);
        // --- Click to move -------------------------------------------------
        // Left selects the unit under the cursor, right orders the selection to
        // the ground under it. Both arrive as a world ray; what it hit is
        // decided here, where the map and the units are.
        //
        // Captured by reference: everything named outlives the window, which is
        // destroyed at the end of this scope before any of them.
        // Which units the user has selected. Shift- or Command-click adds or
        // removes one; a plain click replaces the set with the unit under the
        // cursor. Right-click orders every selected unit to the ground under it.
        //
        // Identity only — nothing about a selected unit is drawn differently, so
        // there is nothing per-unit to remember and put back. The rings are
        // rebuilt from this list every frame.
        // Handles, not (batch, instance) pairs: a selection has to survive a unit dying
        // and the gather renumbering what is drawn. Converted to `SelectionEntry` at draw
        // time, which is the only place the pair means anything.
        std::vector<rm::sim::UnitId> selected;
        std::vector<rm::SelectionEntry> selectionScratch;
        rm::sim::TickClock clock;

        // CONTROL GROUPS: ten saved selections. Ctrl+digit files the current selection under
        // that digit; a bare digit recalls it, pruned of the dead at recall rather than
        // eagerly — a group is a note about intent, and the note outliving some of its units
        // is normal. An empty recall is a no-op rather than a deselect: fat-fingering '4'
        // must not throw away the army under the cursor.
        std::array<std::vector<rm::sim::UnitId>, 10> controlGroups;
        std::optional<rm::sim::CommandKind> armedGroundOrder;

        window.onKey([&window, &selected, &controlGroups, &units, &armedGroundOrder](char key) {
            if (key == 'r') {
                const bool enabled = !window.reflectionsEnabled();
                window.setReflections(enabled);
                std::printf("reflections %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'n') {
                const bool enabled = !window.stratumNormalsEnabled();
                window.setStratumNormals(enabled);
                std::printf("stratum normals %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'f') {
                const bool enabled = !window.refractionEnabled();
                window.setRefraction(enabled);
                std::printf("water refraction %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'o') {
                const bool visible = !window.propsVisible();
                window.setPropsVisible(visible);
                std::printf("props %s\n", visible ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'a' && window.shiftHeldNow()) {
                armedGroundOrder = rm::sim::CommandKind::AttackMove;
                std::printf("attack-move armed: right-click a destination\n");
            } else if (key == 'p') {
                armedGroundOrder = rm::sim::CommandKind::Patrol;
                std::printf("patrol armed: right-click a destination\n");
            } else if (key >= '0' && key <= '9') {
                // The digit arrives with its modifiers stripped (Window.mm's charFor), so
                // whether this is "set" or "recall" is polled from the live modifier state
                // at the moment the key lands — the one question that distinction needs.
                auto& group = controlGroups[static_cast<std::size_t>(key - '0')];
                if (window.controlHeldNow()) {
                    group = selected;
                    std::printf("group %c: %zu unit(s) set\n", key, group.size());
                } else {
                    std::erase_if(group, [&units](rm::sim::UnitId id) {
                        return !units.store.alive(id);
                    });
                    if (!group.empty()) {
                        selected = group;
                    }
                }
            }
        });

        // THE BUILD PANEL (BAR-styled, `core/ui/BuildPanel.hpp`). Scratch kept outside the loop
        // for the same reason every other scratch here is: a frame should not allocate.
        //
        // DECLARED UP HERE, above `onClick`, rather than beside the frame callback that fills
        // it, because BOTH need it: the frame draws the panel and the click has to know whether
        // it landed on one. What a click hits is the panel as last DRAWN, which is what the
        // player was looking at when they pressed the button — so the two reading one vector is
        // the correct coupling rather than a shortcut.
        std::vector<rm::ui::BuildOption> buildOptions;
        rm::app::BuildSelection buildWho;

        // WHAT THE PLAYER PICKED OFF THE TRAY, if anything — an index into `buildOptions`.
        //
        // ARMED RATHER THAN IMMEDIATE, which is how both reference games do it and is not
        // merely convention: a structure needs a PLACE, and the panel cannot know one. So a
        // cell click arms, the next ground click places, and right-click or Escape disarms.
        // Held here beside the options it indexes, and cleared whenever they change — an index
        // into a list that has been rebuilt is a different building.
        std::optional<std::size_t> armedOption;

        // WHOSE MENU THE ICON ATLAS WAS PACKED FOR. Keyed on the builder rather than on the
        // option list, because the list is rebuilt every frame and compares equal every frame —
        // packing on inequality would mean packing never, and packing unconditionally would
        // mean 15 VFS reads and a 512x512 upload per frame for a picture that has not changed.
        // A different builder is the only thing that changes the set.
        rm::sim::UnitId iconsPackedFor{};

        /// The slot each option's icon went into, kept because `gatherBuildOptions` REBUILDS
        /// the option list every frame and a fresh `BuildOption` has no slot. Repacking to
        /// recover them would be 15 archive reads a frame for pictures that have not moved, so
        /// the answer is cached and reapplied instead.
        std::vector<std::optional<std::size_t>> iconSlots;

        /// The selection, grouped by type — Track 0's UI-2. Rebuilt every frame from the store,
        /// so a unit dying leaves its tile's count one lower without anything having to notice.
        std::vector<rm::ui::RosterTile> rosterTiles;
        std::vector<std::optional<std::size_t>> rosterSlots;

        /// How many units the roster's icons were packed for. The tiles are grouped by TYPE, so
        /// this changes only when the selection gains or loses a type — but the count is the
        /// cheap conservative key, and repacking on a unit's death costs one frame of archive
        /// reads rather than a stale picture.
        std::size_t rosterPackedFor = static_cast<std::size_t>(-1);

        // The strategic layer's per-type icon table, rebuilt with every pack — the slots
        // move with the tray's and roster's counts. `typesPackedFor` starts impossible so
        // the FIRST frame packs: unlike the tray, the strategic icons exist with nothing
        // selected at all.
        std::vector<std::optional<rm::app::StrategicIconRef>> strategicRefs;
        std::size_t typesPackedFor = static_cast<std::size_t>(-1);

        // The caller-side tick, the same one `march()` drives. Built here rather than in
        // the frame callback because a match is decided on one tick and stays decided, and
        // the opponents remember what they have already started.
        MatchRunner runner =
            makeMatchRunner(units, map->field, passability, content, starts, map->markers);
        // Match time in TICKS, for pacing the opponents' decisions. Counted rather than
        // read off `matchSeconds`, so a dropped frame cannot skip a decision or run one
        // twice.
        //
        // Seeded from the pre-run, because `--play` simulates N seconds headless before
        // the window opens and the window then continues the SAME match: an opponent
        // announcing its attack at 0.0s in a match already 500 seconds old reads as a bug
        // in the opponent. Only the logging sees this number — the sim counts its own
        // ticks.
        int matchTicks =
            marchOptions.enabled
                ? static_cast<int>(gAppTickRate.ticks(rm::sim::seconds(marchOptions.seconds)))
                : 0;

        // Where the last few orders landed, and when. Markers expire on their own
        // (GroundDecals.hpp), so this only ever grows to the number of orders
        // given inside kOrderMarkerSecondsToLive — a handful at a click a second,
        // and bounded below by nothing needing to be cleaned up on a schedule.
        struct OrderMark {
            std::array<float, 3> position{};
            float age = 0.0f;
        };
        std::vector<OrderMark> orderMarks;

        // Refusals, one per unit that reported no route — the red cross the printf used to
        // be. Same aging discipline as the order marks; drawn beside them each frame.
        std::vector<OrderMark> noRouteMarks;

        // Dust. Particles live across frames — that is the point of them — so
        // this list persists and is aged rather than rebuilt, unlike the decals.
        // The emitter list IS rebuilt each frame, because it is a view of where
        // the units are now.
        std::vector<rm::Particle> particles;
        std::vector<rm::DustEmitter> dustEmitters;
        std::vector<rm::sim::Event> visibleEvents;
        std::vector<rm::sim::Projectile> visibleProjectiles;
        float dustDebt = 0.0f;
        float ambientDebt = 0.0f;
        // A fixed seed, so a scene is the same every run: the same reason the unit
        // scatter takes one.
        std::uint32_t dustSeed = 0x51ED27u;

        // Reused across frames so that rebuilding the rings costs no
        // allocation — the capacity settles after the first large selection.
        std::vector<rm::DecalVertex> decalVertices;

        // The radius a build ghost is drawn at, and the footprint `sitePlaceable` checks.
        //
        // FROM THE BLUEPRINT once the type is registered — a factory is not an extractor — and
        // this is only the fallback for the moment before it resolves.
        constexpr float kGhostFallbackRadiusElmos = 4.0f;

        /// The armed option's blueprint path, or empty. Derived from the id rather than carried
        /// alongside it, so the two cannot disagree — `RosterEntry::path` is the one place the
        /// corpus's `/units/<ID>/<ID>_unit.bp` layout is written down.
        const auto armedPath = [&]() -> std::string {
            if (!armedOption || *armedOption >= buildOptions.size()) {
                return {};
            }
            return rm::data::RosterEntry{.id = buildOptions[*armedOption].id}.path();
        };

        /// The armed option's collision radius, for the ghost and the footprint test.
        const auto armedRadius = [&]() -> float {
            const std::string path = armedPath();
            if (path.empty()) {
                return kGhostFallbackRadiusElmos;
            }
            const std::optional<rm::UnitTypeIndex> type =
                resolveBuildable(units, content, path);
            const rm::unitdef::UnitDef* def = type ? units.catalog.def(*type) : nullptr;
            return def != nullptr && def->collisionRadiusElmos > 0.0f
                       ? def->collisionRadiusElmos
                       : kGhostFallbackRadiusElmos;
        };

        /// Whether the armed build may stand at a world point — the ghost's colour, and the
        /// same question the click asks before it orders anything.
        const auto armedPlaceable = [&](std::array<float, 2> at) -> bool {
            if (!armedOption) {
                return false;
            }
            const auto type =
                static_cast<std::size_t>(units.store.typeAt(buildWho.builder.index));
            const rm::sim::PassabilityGrid& grid = passability.gridFor(
                units.maxSlopeDegrees[type], units.maxWaterDepthElmos[type]);
            return rm::sim::sitePlaceable(grid, rm::sim::fxFromFloat(at[0]),
                                          rm::sim::fxFromFloat(at[1]),
                                          rm::sim::fxFromFloat(armedRadius()));
        };

        /// Orders the armed build at a world point, through the one order path, and disarms.
        ///
        /// DISARMS WHETHER OR NOT IT TOOK. A refused placement that stayed armed would leave
        /// the player clicking at a spot that will never work, with the ghost saying so and
        /// nothing else happening — better to put the tool down and let them pick it up again.
        const auto placeArmedBuild = [&](simd_float3 at) {
            const std::string path = armedPath();
            const std::optional<rm::UnitTypeIndex> type =
                path.empty() ? std::nullopt : resolveBuildable(units, content, path);
            if (!type || !units.store.alive(buildWho.builder)) {
                armedOption.reset();
                return;
            }
            const auto builderType =
                static_cast<std::size_t>(units.store.typeAt(buildWho.builder.index));
            const rm::sim::PassabilityGrid& grid = passability.gridFor(
                units.maxSlopeDegrees[builderType], units.maxWaterDepthElmos[builderType]);

            if (armedPlaceable({at.x, at.z})
                && issueBuild(units, grid, map->field, buildWho.builder,
                              playerDriving(units, units.playerArmy),
                              static_cast<rm::TickIndex>(matchTicks), *type,
                              rm::sim::fxFromFloat(at.x), rm::sim::fxFromFloat(at.z))) {
                std::printf("build: %s at %.0f, %.0f\n", buildOptions[*armedOption].id.c_str(),
                            static_cast<double>(at.x), static_cast<double>(at.z));
            }
            armedOption.reset();
        };

        window.onClick([&](const rm::Ray& ray, rm::MouseButton button,
                           rm::MouseModifiers mods) {
            // THE MINIMAP FIRST, because it is in front of the world (§7 P7.4). A click on the
            // panel is about the panel; without this check the ray under it would also select
            // whatever unit happens to be behind the minimap, which is the single most
            // irritating bug an overlay can have.
            // Orders the whole selection to a world point — the right button's meaning,
            // whether the point came from a ray into the world or a click on the minimap.
            // One lambda so the two entrances cannot drift; the marker, the per-unit grids
            // and the queue reporting are the same statements they were.
            const auto orderSelectionTo = [&](simd_float3 ground, bool queue,
                                               std::optional<rm::sim::UnitId> target =
                                                   std::nullopt,
                                               rm::sim::CommandKind groundKind =
                                                   rm::sim::CommandKind::Move) {
                orderMarks.push_back(OrderMark{
                    .position = {ground.x, ground.y, ground.z},
                    .age = 0.0f,
                });

                std::size_t failed = 0;
                for (const rm::sim::UnitId sel : selected) {
                    if (!units.store.alive(sel)) {
                        continue;  // selected, then killed before the order was given
                    }
                    // Each unit routes on the map ITS limits see. Two units given the same
                    // order can legitimately get different answers, and one of them can be
                    // "no route" while the other walks off.
                    const auto type = static_cast<std::size_t>(units.store.typeAt(sel.index));
                    const rm::sim::PassabilityGrid& grid = passability.gridFor(
                        units.maxSlopeDegrees[type], units.maxWaterDepthElmos[type]);
                    // ⌘-RIGHT-CLICK ON AN ENEMY IS AN OVERCHARGE, for the units that carry
                    // a manual weapon; the rest of the selection attacks as it would have.
                    // The escort keeps escorting while the commander spends the store.
                    const rm::unitdef::UnitDef* selDef =
                        units.catalog.def(units.store.typeAt(sel.index));
                    float shotCost = 0.0f;  // the manual weapon's EnergyRequired, or zero
                    if (selDef != nullptr) {
                        for (const rm::unitdef::Weapon& weapon : selDef->weapons) {
                            if (weapon.manuallyFired()) {
                                shotCost = rm::sim::magToFloat(weapon.energyRequired);
                                break;
                            }
                        }
                    }
                    const bool wantOvercharge = target && mods.command && shotCost > 0.0f;
                    bool took = false;
                    if (wantOvercharge) {
                        took = issueOvercharge(
                            units, grid, map->field, sel,
                            playerDriving(units, units.playerArmy),
                            static_cast<rm::TickIndex>(matchTicks), *target,
                            rm::sim::fxFromFloat(ground.x), rm::sim::fxFromFloat(ground.z), queue);
                    } else if (target) {
                        took = issueAttack(units, grid, map->field, sel,
                                           playerDriving(units, units.playerArmy),
                                           static_cast<rm::TickIndex>(matchTicks), *target,
                                           rm::sim::fxFromFloat(ground.x),
                                           rm::sim::fxFromFloat(ground.z), queue);
                    } else {
                        took = issueMove(units, grid, map->field, sel,
                                         playerDriving(units, units.playerArmy),
                                         static_cast<rm::TickIndex>(matchTicks),
                                         rm::sim::fxFromFloat(ground.x),
                                         rm::sim::fxFromFloat(ground.z), queue, groundKind);
                    }
                    if (took && wantOvercharge && units.playerArmy >= 0
                        && static_cast<std::size_t>(units.playerArmy)
                               < units.economies.size()) {
                        // The one piece of feedback the world does not show: whether the
                        // shot leaves now or waits for the bar to fill.
                        const float banked = rm::sim::magToFloat(
                            units.economies[static_cast<std::size_t>(units.playerArmy)]
                                .stored.energy);
                        if (banked >= shotCost) {
                            std::printf("overcharge: firing (%.0f energy banked)\n",
                                        static_cast<double>(banked));
                        } else {
                            std::printf("overcharge: holding until charged (%.0f of %.0f "
                                        "energy)\n",
                                        static_cast<double>(banked),
                                        static_cast<double>(shotCost));
                        }
                    }
                    if (!took) {
                        ++failed;
                        // The refusal, ON the refusing unit: the destination already has
                        // its marker, and the question a failure raises is which of the
                        // selected are not coming. This replaces a printf nobody looked at
                        // in the one moment it mattered.
                        const rm::sim::Transform& at = units.store.transforms()[sel.index];
                        noRouteMarks.push_back(OrderMark{
                            .position = {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                         rm::sim::fxToFloat(at.z)},
                            .age = 0.0f,
                        });
                    }
                }
                if (failed > 0) {
                    std::printf("no route there for %zu of %zu units\n", failed,
                                selected.size());
                } else if (queue) {
                    // Only for a queued order, and only the length: this is the one piece of
                    // feedback the world does not already show.
                    const rm::sim::UnitId first = selected.front();
                    if (units.store.alive(first)) {
                        std::printf("queued: %zu order(s) for the first of %zu selected\n",
                                    units.store.orders()[first.index].size(),
                                    selected.size());
                    }
                }
            };

            const rm::ui::MinimapLayout minimap =
                rm::ui::minimapLayout(static_cast<float>(window.width()),
                                      static_cast<float>(window.height()));
            if (rm::ui::insideMinimap(minimap, mods.pointX, mods.pointY)) {
                const std::array<float, 2> where =
                    rm::ui::minimapToWorld(minimap, map->field.widthElmos(),
                                           map->field.depthElmos(), mods.pointX, mods.pointY);
                const simd_float3 ground = simd_make_float3(
                    where[0], map->field.heightAtWorld(where[0], where[1]), where[1]);

                // THE RIGHT BUTTON MEANS THE SAME THING ON THE MAP AS IN THE WORLD: go
                // there. Ordering across the map without swinging the camera off the fight
                // is most of what a minimap order is for.
                if (button == rm::MouseButton::Right) {
                    if (!selected.empty()) {
                        orderSelectionTo(ground, mods.shift, std::nullopt,
                                         armedGroundOrder.value_or(
                                             rm::sim::CommandKind::Move));
                        armedGroundOrder.reset();
                    }
                    return;
                }

                // Left: jump, keeping the camera's distance and angles — a minimap click
                // moves where you are looking, not how. Height sampled from the terrain so
                // the target sits on the ground rather than at y = 0.
                window.camera().target = ground;
                return;
            }

            // THE BUILD PANEL, for the minimap's reason above and with a sharper edge to it.
            // The panel appears only while a builder is selected, so a click falling through it
            // reached the ground, cleared the selection, and took the panel away with it — the
            // button vanished under the cursor that pressed it. An overlay that punishes you for
            // aiming at it is worse than no overlay.
            //
            // SWALLOWED WHETHER OR NOT IT HIT A CELL: the gutters and the header are part of the
            // panel, and a click landing in one is still a click on the interface rather than on
            // the world behind it.
            //
            // What it does not yet do is ACT on the cell. That needs the build path routed
            // through `applyCommand` — a behaviour change, not a guard — so the two are separate
            // jobs and this is the one that stops the bleeding.
            if (!buildOptions.empty()) {
                const rm::ui::BuildPanelLayout panel =
                    rm::ui::buildPanelLayout(minimap, buildOptions.size());
                if (rm::ui::insideBuildPanel(panel, mods.pointX, mods.pointY)) {
                    // A cell ARMS the build; the gutters and header swallow and do nothing.
                    // Right-click anywhere on the panel disarms, so the way out is where the
                    // way in was.
                    const std::optional<std::size_t> cell = rm::ui::buildOptionAt(
                        panel, buildOptions.size(), mods.pointX, mods.pointY);
                    if (button == rm::MouseButton::Right) {
                        armedOption.reset();
                    } else if (cell && buildOptions[*cell].affordable
                               && buildWho.role == "factory") {
                        // A FACTORY CELL BUILDS AT ONCE — there is no place to pick, the
                        // factory IS the place, so arming a ghost would be a step with no
                        // decision in it. The site is one step off the floor, so the
                        // finished unit stands beside its factory rather than inside it.
                        if (units.store.alive(buildWho.builder)) {
                            const std::string path =
                                rm::data::RosterEntry{.id = buildOptions[*cell].id}.path();
                            const std::optional<rm::UnitTypeIndex> type =
                                resolveBuildable(units, content, path);
                            const auto factoryType = static_cast<std::size_t>(
                                units.store.typeAt(buildWho.builder.index));
                            const rm::sim::PassabilityGrid& grid = passability.gridFor(
                                units.maxSlopeDegrees[factoryType],
                                units.maxWaterDepthElmos[factoryType]);
                            const rm::sim::Transform& at =
                                units.store.transforms()[buildWho.builder.index];
                            const rm::sim::Fx rollOff =
                                units.store.motion()[buildWho.builder.index].radiusElmos * 2;
                            if (type
                                && issueBuild(units, grid, map->field, buildWho.builder,
                                              playerDriving(units, units.playerArmy),
                                              static_cast<rm::TickIndex>(matchTicks), *type,
                                              at.x, at.z + rollOff)) {
                                std::printf("factory: %s queued\n",
                                            buildOptions[*cell].id.c_str());
                            }
                        }
                    } else if (cell && buildOptions[*cell].affordable) {
                        armedOption = cell;
                    } else if (cell) {
                        // UNAFFORDABLE ARMS NOTHING, and the cell is still drawn — "not yet" is
                        // the information. Arming it would leave a ghost the player cannot
                        // place and no way to learn why.
                        std::printf("cannot afford %s (%.0f mass)\n",
                                    buildOptions[*cell].id.c_str(),
                                    static_cast<double>(buildOptions[*cell].massCost));
                    }
                    return;
                }
            }

            // THE ROSTER, for the build panel's reason: it is in front of the world, and a
            // click that fell through would order the very units the roster is describing to
            // walk to wherever happens to be behind it.
            //
            // AND IT IS A CONTROL, not only a readout: clicking a tile filters the selection
            // to that type — the composition panel becomes the way to peel the engineers out
            // of a battle group — and a modifier inverts it, dropping the type instead. Both
            // reference games bind tile clicks this way. The gutters and header still just
            // swallow: a miss near a button must not become the wrong button.
            if (!rosterTiles.empty()) {
                const rm::ui::RosterLayout roster =
                    rm::ui::rosterLayout(static_cast<float>(window.width()),
                                         static_cast<float>(window.height()),
                                         rosterTiles.size());
                if (rm::ui::insideRoster(roster, mods.pointX, mods.pointY)) {
                    const std::optional<std::size_t> tile =
                        rm::ui::rosterTileAt(roster, mods.pointX, mods.pointY);
                    if (tile && button == rm::MouseButton::Left) {
                        const std::string& id = rosterTiles[*tile].id;
                        const bool drop = mods.shift || mods.command || mods.control;
                        std::erase_if(selected, [&](rm::sim::UnitId unit) {
                            if (!units.store.alive(unit)) {
                                return true;  // housekeeping the next gather would do anyway
                            }
                            const rm::unitdef::UnitDef* def =
                                units.catalog.def(units.store.typeAt(unit.index));
                            const bool matches = def != nullptr && def->name == id;
                            return drop ? matches : !matches;
                        });
                    }
                    return;
                }
            }

            // A GROUND CLICK WHILE ARMED PLACES, and nothing else happens — it does not also
            // select, which is the same reasoning as the panel guard above: the click had a
            // meaning and it was not "pick a unit".
            if (armedOption && button == rm::MouseButton::Left) {
                const std::optional<simd_float3> at = rm::pickGround(ray, map->field);
                if (!at) {
                    return;  // the sky, or past the edge — the order simply does not happen
                }
                placeArmedBuild(*at);
                return;
            }
            if (armedOption && button == rm::MouseButton::Right) {
                armedOption.reset();  // right-click cancels, as it cancels everything else
                return;
            }

            if (button == rm::MouseButton::Left) {
                // What the click MEANS is decided in core/scene/Selection.hpp,
                // where it can be tested. Nothing is left to do here: the units
                // themselves are not repainted, and the rings are rebuilt from
                // this list by the frame callback.
                const bool addToSet = mods.shift || mods.command || mods.control;
                const std::optional<rm::sim::UnitId> pick = pickAcrossBatches(ray, units);

                // DOUBLE-CLICK WIDENS TO THE TYPE, on screen: every one of the player's
                // units of the clicked type whose position projects into the viewport.
                // On screen rather than map-wide because that is what both reference games
                // do, and because "everything like this, everywhere" silently commits units
                // the player cannot see. The first click of the pair selected the unit
                // normally; this refines it, so a double-click on empty ground still means
                // what a single click there meant.
                if (pick && mods.clicks >= 2) {
                    const rm::UnitTypeIndex wanted = units.store.typeAt(pick->index);
                    const float w = static_cast<float>(window.width());
                    const float h = static_cast<float>(window.height());
                    std::vector<rm::sim::UnitId> ofType;
                    for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                        if (!units.store.slotAlive(slot)
                            || units.store.typeAt(slot) != wanted
                            || units.armyOf(slot) != units.playerArmy) {
                            continue;
                        }
                        const rm::sim::Transform& at = units.store.transforms()[slot];
                        const auto screen = rm::worldToScreen(
                            window.camera(),
                            simd_make_float3(rm::sim::fxToFloat(at.x),
                                             rm::sim::fxToFloat(at.y),
                                             rm::sim::fxToFloat(at.z)),
                            w, h);
                        if (screen && (*screen)[0] >= 0.0f && (*screen)[0] <= w
                            && (*screen)[1] >= 0.0f && (*screen)[1] <= h) {
                            ofType.push_back(units.store.idAt(slot));
                        }
                    }
                    selected = rm::applyBand<rm::sim::UnitId>(selected, ofType, addToSet);
                    return;
                }

                selected = rm::applyClick<rm::sim::UnitId>(selected, pick, addToSet);
                return;
            }

            if (selected.empty()) {
                return;  // an order with nothing selected is not an error
            }

            // AN ATTACK OR A MOVE, decided by what the right button landed on. Picking is
            // done WITHOUT the army filter that selection uses — the point here is to find
            // an enemy, which is exactly what selection excludes.
            //
            // What this is not: a tracked attack order. The selection is sent to where the
            // target IS, and the automatic targeting does the shooting once in range. A
            // target that walks away is therefore not chased, which is the honest
            // difference between "attack move" and "attack that unit", and the next thing
            // here.
            std::optional<simd_float3> ground;
            const std::optional<rm::sim::UnitId> hit = pickAnyBatch(ray, units);
            const bool isAttack = hit && units.playerArmy != rm::sim::kNoArmy
                                && hostileTo(units, units.playerArmy, *hit);

            if (isAttack) {
                const rm::sim::Transform& at = units.store.transforms()[hit->index];
                ground = simd_make_float3(rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                          rm::sim::fxToFloat(at.z));
            } else {
                ground = rm::pickGround(ray, map->field);
            }
            if (!ground) {
                return;  // clicked the sky, or past the edge of the map
            }

            // A WRECK UNDER THE CLICK MAKES IT A RECLAIM — for the builders in the
            // selection; everyone else walks there. An enemy unit beats a wreck (the pick
            // above already decided that), and a wreck beats plain ground. The hit disc is
            // the mark the player can actually SEE — the decal's radius, not the sim's —
            // with a floor so a tiny unit's wreck is still clickable.
            if (!isAttack && !armedGroundOrder) {
                std::optional<rm::sim::FeatureId> wreck;
                float wreckGap = 0.0f;
                for (rm::UnitIndex slot = 0; slot < units.features.size(); ++slot) {
                    if (!units.features.slotAlive(slot)) {
                        continue;
                    }
                    const rm::sim::Feature& candidate = units.features.all()[slot];
                    if (!units.visibleToViewer(candidate.at[0], candidate.at[2])) {
                        continue;  // an unseen wreck cannot turn a blind move into an oracle
                    }
                    if (candidate.massRemaining <= rm::sim::Mag{}
                        && candidate.energyRemaining <= rm::sim::Mag{}) {
                        continue;  // a bare scorch is not an order target
                    }
                    const float dx = ground->x - rm::sim::fxToFloat(candidate.at[0]);
                    const float dz = ground->z - rm::sim::fxToFloat(candidate.at[2]);
                    const float gap = std::sqrt(dx * dx + dz * dz);
                    const float disc =
                        std::max(6.0f, rm::sim::fxToFloat(candidate.radiusElmos)
                                           * rm::kWreckMarkRadiusFactor);
                    if (gap <= disc && (!wreck || gap < wreckGap)) {
                        wreck = units.features.idAt(slot);
                        wreckGap = gap;
                    }
                }
                if (wreck) {
                    const rm::sim::Feature* found = units.features.find(*wreck);
                    orderMarks.push_back(OrderMark{
                        .position = {rm::sim::fxToFloat(found->at[0]),
                                     rm::sim::fxToFloat(found->at[1]),
                                     rm::sim::fxToFloat(found->at[2])},
                        .age = 0.0f,
                    });
                    std::size_t reclaiming = 0;
                    for (const rm::sim::UnitId sel : selected) {
                        if (!units.store.alive(sel)) {
                            continue;
                        }
                        const auto type =
                            static_cast<std::size_t>(units.store.typeAt(sel.index));
                        const rm::sim::PassabilityGrid& grid = passability.gridFor(
                            units.maxSlopeDegrees[type], units.maxWaterDepthElmos[type]);
                        const rm::unitdef::UnitDef* def =
                            units.catalog.def(units.store.typeAt(sel.index));
                        const bool builder = def != nullptr && def->isBuilder();
                        const bool took =
                            builder ? issueReclaim(units, grid, map->field, sel,
                                                   playerDriving(units, units.playerArmy),
                                                   static_cast<rm::TickIndex>(matchTicks),
                                                   *wreck, mods.shift)
                                    : issueMove(units, grid, map->field, sel,
                                                playerDriving(units, units.playerArmy),
                                                static_cast<rm::TickIndex>(matchTicks),
                                                found->at[0], found->at[2], mods.shift);
                        if (took && builder) {
                            ++reclaiming;
                        }
                    }
                    if (reclaiming > 0) {
                        std::printf("reclaim: %zu builder(s) on the wreck (%.0f mass)\n",
                                    reclaiming,
                                    static_cast<double>(
                                        rm::sim::magToFloat(found->massRemaining)));
                    }
                    return;
                }
            }

            // Marked before the routing is attempted (inside orderSelectionTo), and
            // deliberately: the mark answers "did that click land, and where", which is
            // true even if every unit then reports no route. SHIFT QUEUES IT (§7 P4.1) —
            // the same modifier adds to the selection on the left button and appends to
            // the order queue on the right, as every RTS this engine reads content from.
            //
            // An attack carries the TARGET'S HANDLE, which is what makes it a pursuit
            // rather than a walk to where the target used to be (`advanceOrders`' chase).
            orderSelectionTo(*ground, mods.shift,
                             isAttack && !armedGroundOrder
                                 ? hit
                                 : std::optional<rm::sim::UnitId>{},
                             armedGroundOrder.value_or(rm::sim::CommandKind::Move));
            armedGroundOrder.reset();
        });

        // The overhead view the camera returns to when space is released. Captured at the
        // start, which is the whole-map fit `OrbitCamera::frame` computed on load, so
        // "normal view" means the view the app opened with rather than a hardcoded angle.
        const float restPitch = window.camera().pitch;
        const float restYaw = window.camera().yaw;

        window.onKeyState([&window, restPitch, restYaw](char key, bool pressed) {
            // Releasing space snaps the camera back overhead. Holding it is what allows a
            // drag to rotate at all (Window.mm), so this is the other half of that mode:
            // look around while you hold, and you are put back when you let go, which
            // means a glance never costs you your bearings.
            if (key == ' ' && !pressed) {
                rm::OrbitCamera& camera = window.camera();
                camera.pitch = restPitch;
                camera.yaw = restYaw;
            }
        });

        // Scratch for the icon pass, held outside the frame callback so a frame costs no
        // allocation — the same reason the dust emitters are.
        std::vector<rm::Particle> iconScratch;
        rm::ui::Geometry hudScratch;

        // The minimap's per-frame scratch, kept out here so a frame allocates nothing.
        std::vector<rm::ui::MinimapPip> minimapPips;
        std::vector<std::array<float, 2>> minimapView;
        float matchSeconds = 0.0f;

        // The left button's state LAST frame, for the band-select's falling edge — the
        // release is detected by polling, the same way the drag itself is.
        bool leftWasHeld = false;
        std::vector<rm::sim::UnitId> bandScratch;

        window.onFrame([&](float elapsed) {
            // WASD pans the map, every frame rather than per keypress: a pan driven by
            // key EVENTS moves in jerks the length of the auto-repeat interval, and stops
            // dead the moment the repeat lapses.
            //
            // The step is scaled by the frustum's width at the target, the same quantity a
            // mouse pan uses, so the speed feels the same zoomed in as out — there is no
            // sensitivity constant here to get wrong. A second and a half to cross the
            // visible width.
            {
                const float across =
                    window.camera().elmosPerPoint(kPanReferenceHeightPoints)
                    * kPanReferenceHeightPoints;
                const float step = across * elapsed / kSecondsToCrossTheView;

                float right = 0.0f;
                float forward = 0.0f;
                if (window.keyHeld('a')) { right -= step; }
                if (window.keyHeld('d')) { right += step; }
                if (window.keyHeld('w')) { forward += step; }
                if (window.keyHeld('s')) { forward -= step; }
                if (right != 0.0f || forward != 0.0f) {
                    window.camera().pan(right, forward);
                }
            }

            matchSeconds += elapsed;
            const int ticks = clock.advance(elapsed);

            // How many batches the renderer currently knows about. A finished
            // construction spawns its unit into a NEW batch when nothing of that
            // blueprint stands yet, and `setInstances` silently drops a batch index it
            // was never given (Renderer.hpp) — so without noticing the growth here, a
            // building would exist in the sim, fight, earn, and never be drawn.
            const std::size_t batchesBefore = units.batches.size();

            // THE SAME TICK the headless pre-run makes. This loop used to call
            // `sim::tick` and `resolveCollisions` and nothing else, so a unit in the
            // interactive game moved perfectly and never fired a shot — while the whole
            // suite stayed green, because every rule was tested and the assembly of them
            // was not. Both callers now go through `advanceMatch`, which is what makes
            // "the same game" checkable rather than remembered.
            for (int i = 0; i < ticks; ++i) {
                const rm::sim::TickReport report =
                    advanceMatch(runner, matchTicks, static_cast<float>(matchTicks)
                                                         * gAppTickRate.secondsPerTick());
                ++matchTicks;

                // The tick's combat, as particles — read HERE because an event is a
                // per-tick notification (Events.hpp): the next advanceMatch clears the
                // queue, so this tick's shots are visible now or never.
                gatherVisibleEvents(visibleEvents, units);
                rm::emitCombatEffects(particles, visibleEvents);

                // ...and as SOUND, from the same per-tick queue for the same reason. The
                // listener rides the camera every tick, so panning follows the view.
                mixer.setListener(window.camera().target.x, window.camera().target.z,
                                  window.camera().distance);
                rm::audio::playForEvents(mixer, visibleEvents,
                                         explosionBank ? &*explosionBank : nullptr,
                                         impactBank ? &*impactBank : nullptr);

                // ...and the arcs' smoke, one puff per shell per tick — the emission rate
                // is the sim's own, so the trail spacing is a tick of travel (ProjectileFx).
                gatherVisibleProjectiles(visibleProjectiles, units);
                rm::emitProjectileTrails(particles, visibleProjectiles);

                // The match, announced once. The frame loop draws the fight rather than
                // narrating it, so this is the one thing worth saying out loud — and only
                // when there is a match to decide, the same guard the pre-run uses.
                if (report.matchEnded && !units.armies.empty()) {
                    if (report.winner) {
                        std::printf("team %d WINS\n", *report.winner);
                    } else {
                        std::printf("a DRAW: every army lost its commander\n");
                    }
                    std::fflush(stdout);
                }
            }

            // The gather builds every instance from the last two SNAPSHOTS, blended by how far
            // into the next tick the clock's banked time reaches (§7 P7.2). That fraction was
            // already being computed — `TickClock::advance` banks the remainder — so exposing
            // it as `alpha()` is what lets a 10 Hz sim draw continuously at 120 Hz rather than
            // teleporting twelve times a step.
            //
            // Walk-cycle phase included, and interpolated too: a leg that stepped at the tick
            // rate would slide as badly as a body that did.
            const simd_float3 cameraEye = window.camera().eye();
            const std::array<float, 3> lodEye{cameraEye.x, cameraEye.y, cameraEye.z};
            units.gatherForDrawing(clock.alpha(), &lodEye);

            // Re-upload when the match built something new. Only on growth, which is a
            // handful of times in a whole match — this walks every model and texture, so
            // doing it per frame would cost what it costs to load the scene.
            //
            // AFTER the gather, and that ordering is load-bearing: `setUnits` sizes each
            // batch's instance buffer from what its span holds, and a batch created this
            // tick holds nothing until the gather fills it. Uploading first gives the new
            // model a capacity of zero, and a unit type the player just built never draws.
            if (units.batches.size() != batchesBefore) {
                for (std::size_t b = batchesBefore; b < units.batches.size(); ++b) {
                    units.batches[b].animationDrivenByInstance = true;
                }
                window.setUnits(units.textures.all(), units.batches);
            }

            for (std::size_t batch = 0; batch < units.drawScratch.size(); ++batch) {
                window.setInstances(batch, units.drawScratch[batch]);
            }

            // The fog, from the same grid that decided which of those instances exist.
            units.applyFog(window);

            // Dust behind whatever is moving. After the sim, so a puff is born
            // where the unit has got to rather than where it started the frame.
            //
            // The emitters are gathered from the motion state the sim just wrote,
            // which is also where the speed threshold gets its answer — a unit
            // being jostled by a crowd is moving in position but not in speed, and
            // should not smoke.
            dustEmitters.clear();
            for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                if (!units.store.slotAlive(slot)) {
                    continue;  // a wreck does not kick up dust
                }
                if (!units.visibleToViewer(units.store.idAt(slot))) {
                    continue;
                }
                const rm::sim::MoveState& motion = units.store.motion()[slot];
                const rm::sim::Transform& at = units.store.transforms()[slot];
                dustEmitters.push_back(rm::DustEmitter{
                    .position = {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                 rm::sim::fxToFloat(at.z)},
                    .moving = motion.moving,
                    // The dust threshold is stated per second, so the per-tick speed converts
                    // back — a display concern reading a sim value, which is what
                    // `secondsPerTick` exists for.
                    .topSpeedElmosPerSecond =
                        rm::sim::fxToFloat(motion.speedPerTick)
                        * static_cast<float>(gAppTickRate.ticksPerSecond()),
                    .radiusElmos = rm::sim::fxToFloat(motion.radiusElmos),
                });
            }
            rm::advanceParticles(particles, elapsed);
            rm::emitDust(particles, dustEmitters, map->field, elapsed, dustDebt, dustSeed);
            // ...and the map's own ambient effects, which run whether anything moves
            // or not: a map's lava does not stop steaming because nobody is looking.
            rm::emitAmbient(particles, props.ambient, elapsed, ambientDebt, dustSeed);

            // ICONS for whatever has shrunk past reading. Appended to the particle list
            // rather than drawn by a pass of their own, because an icon IS a stationary
            // camera-facing quad and that is what the particle pipeline already draws
            // (core/scene/UnitIcons.hpp). Built into a scratch copy so the icons do not
            // accumulate in the list the dust ages through.
            iconScratch.clear();
            appendVisibleParticles(iconScratch, particles, units);
            // With LAST pack's refs: the types the strategic layer draws keep their squares
            // out of the particle list. One frame after a fresh type appears, both tables
            // agree; in between it shows the square, which is the fallback anyway.
            appendSceneIcons(iconScratch, units, window.camera(), strategicRefs);
            // The shots in flight, extrapolated by the frame's tick fraction — rebuilt per
            // frame like the icons, into the same scratch, aging never.
            gatherVisibleProjectiles(visibleProjectiles, units, clock.alpha());
            rm::appendProjectiles(iconScratch, visibleProjectiles, clock.alpha(),
                                  window.camera().elmosPerPoint(
                                      rm::kIconReferenceHeightPoints));
            window.setParticles(iconScratch);

            hudScratch.clear();
            rm::ui::build(hudScratch, window.labelFont(), window.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, matchSeconds),
                          static_cast<float>(window.width()),
                          static_cast<float>(window.height()));

            // Health over the units that need it: damaged, and close enough to be units
            // rather than icons. Absence is what "fine" looks like (Interface.hpp).
            appendHealthBars(hudScratch, units, window.camera(), window.labelFont(),
                             static_cast<float>(window.width()),
                             static_cast<float>(window.height()));

            // The strategic layer: the game's own glyphs where units are too small to read,
            // in the army's colour, under all the chrome (Geometry::worldImage).
            appendStrategicIcons(hudScratch, units, window.camera(),
                                 static_cast<float>(window.width()),
                                 static_cast<float>(window.height()), strategicRefs);
            appendContactBlips(hudScratch, units, window.camera(), map->field,
                               window.labelFont(), static_cast<float>(window.width()),
                               static_cast<float>(window.height()));

            // THE MINIMAP (§7 P7.4), appended to the same geometry the HUD builds — it is
            // rectangles in screen space, which is what `text::appendRect` already draws, so it
            // needs no pipeline of its own. That is the other half of "nearly free".
            appendMinimapPips(minimapPips, units);
            appendViewFootprint(minimapView, window.camera(), map->field,
                                static_cast<float>(window.width()),
                                static_cast<float>(window.height()));
            const rm::ui::MinimapLayout minimap =
                rm::ui::minimapLayout(static_cast<float>(window.width()),
                                      static_cast<float>(window.height()));
            // The preview under the panel, inset by the border so the chrome frames it. The
            // panel then draws everything BUT its own fill, so the picture shows through.
            const bool hasPreview = map->preview.width > 0;
            if (hasPreview) {
                window.setMinimapRect(minimap.x + minimap.inset, minimap.y + minimap.inset,
                                      minimap.size - minimap.inset * 2.0f,
                                      minimap.size - minimap.inset * 2.0f);
            }
            rm::ui::appendMinimap(hudScratch, window.labelFont(), hudThemeFor(units), minimap,
                                  map->field.widthElmos(), map->field.depthElmos(), minimapPips,
                                  minimapView, !hasPreview);

            // What the selection can build, above the minimap — the bottom-left control block
            // Beyond All Reason arranges the same way. Absent entirely when nothing selected
            // builds, rather than an empty frame asking to be explained.
            rm::app::gatherBuildOptions(units, selected, hudThemeFor(units), buildOptions,
                                        buildWho);
            // AN INDEX INTO A LIST THAT HAS BEEN REBUILT IS A DIFFERENT BUILDING. Deselecting,
            // or selecting a different builder, must not leave cell 4 armed and meaning
            // something else — so the arming is dropped whenever the list it points into can no
            // longer be trusted to be the same list.
            if (armedOption && *armedOption >= buildOptions.size()) {
                armedOption.reset();
            }

            rm::app::gatherRoster(units, selected, rosterTiles);

            // The icons for BOTH panels, in one atlas: packed when either set changes, and
            // reapplied from the cache otherwise. Reapplied rather than repacked because the
            // option list and the tile list are rebuilt every frame and a fresh entry has no
            // slot — repacking to recover them would be two dozen archive reads a frame for
            // pictures that have not moved.
            if (buildWho.builder != iconsPackedFor || rosterTiles.size() != rosterPackedFor
                || units.catalog.size() != typesPackedFor) {
                iconsPackedFor = buildWho.builder;
                rosterPackedFor = rosterTiles.size();
                typesPackedFor = units.catalog.size();
                // Any glyph a newly registered type names is fetched before the pack, so a
                // unit type first seen this frame gets its icon in this atlas rather than
                // a square until the next selection change.
                rm::app::ensureStrategicIconArt(units, content);
                std::size_t strategicBase = 0;
                window.setIconAtlas(rm::app::packInterfaceIcons(
                    content, buildOptions, rosterTiles, units.strategicIconArt,
                    &strategicBase));
                rm::app::buildStrategicIconRefs(units, strategicBase, strategicRefs);
                iconSlots.clear();
                for (const rm::ui::BuildOption& option : buildOptions) {
                    iconSlots.push_back(option.iconSlot);
                }
                rosterSlots.clear();
                for (const rm::ui::RosterTile& tile : rosterTiles) {
                    rosterSlots.push_back(tile.iconSlot);
                }
            } else {
                for (std::size_t i = 0; i < buildOptions.size() && i < iconSlots.size(); ++i) {
                    buildOptions[i].iconSlot = iconSlots[i];
                }
                for (std::size_t i = 0; i < rosterTiles.size() && i < rosterSlots.size(); ++i) {
                    rosterTiles[i].iconSlot = rosterSlots[i];
                }
            }
            if (!buildOptions.empty()) {
                const rm::ui::BuildPanelLayout panel =
                    rm::ui::buildPanelLayout(minimap, buildOptions.size());

                // The lit cell under the cursor, which is most of what makes a grid of squares
                // read as BUTTONS rather than as a readout. Polled once here rather than
                // tracked through a mouseMoved handler — see `Window::cursor`.
                const std::array<float, 2> at = window.cursor();
                std::optional<std::size_t> hovered =
                    rm::ui::buildOptionAt(panel, buildOptions.size(), at[0], at[1]);
                // THE ARMED CELL STAYS LIT while the cursor is out over the map, which is
                // exactly when the player needs to be told what they are about to place. A
                // hover wins over it, so moving back onto the tray reads normally.
                if (!hovered && armedOption) {
                    hovered = armedOption;
                }

                rm::ui::appendBuildPanel(hudScratch, window.labelFont(), window.readoutFont(),
                                         hudThemeFor(units), panel, buildOptions, hovered,
                                         buildWho.name, buildWho.role);

                // The hover card, docked above the tray — the block grows upward one more
                // step: minimap, tray, card. A fixed slot rather than a pointer-chasing
                // tooltip; the reasons are on `InfoCard`.
                if (hovered && *hovered < buildOptions.size()) {
                    const rm::ui::InfoCard card =
                        rm::ui::buildOptionCard(buildOptions[*hovered]);
                    const float cardHeight = rm::ui::infoCardHeight(
                        window.labelFont().lineHeight, card.rows.size());
                    rm::ui::appendInfoCard(hudScratch, window.labelFont(),
                                           window.readoutFont(), hudThemeFor(units), panel.x,
                                           panel.y - cardHeight, panel.width, card);
                }
            }

            // The roster, bottom centre. After the tray so both are in one buffer; they do not
            // overlap, so the order between them is arbitrary and stated only to be stable.
            if (!rosterTiles.empty()) {
                const rm::ui::RosterLayout roster =
                    rm::ui::rosterLayout(static_cast<float>(window.width()),
                                         static_cast<float>(window.height()),
                                         rosterTiles.size());
                const std::array<float, 2> at = window.cursor();
                const std::optional<std::size_t> overTile =
                    rm::ui::rosterTileAt(roster, at[0], at[1]);
                rm::ui::appendRoster(hudScratch, window.labelFont(), window.readoutFont(),
                                     hudThemeFor(units), roster, rosterTiles, overTile);

                // The tile's card, above the roster — same fitting as the tray's.
                if (overTile && *overTile < rosterTiles.size()) {
                    const rm::ui::InfoCard card =
                        rm::ui::rosterTileCard(rosterTiles[*overTile]);
                    const float cardHeight = rm::ui::infoCardHeight(
                        window.labelFont().lineHeight, card.rows.size());
                    rm::ui::appendInfoCard(hudScratch, window.labelFont(),
                                           window.readoutFont(), hudThemeFor(units), roster.x,
                                           roster.y - cardHeight, roster.width, card);
                }
            }

            // --- The band box, and the minimap's drag-to-pan --------------------------
            // Both are DERIVED FROM POLLED STATE — is the left button down, where did the
            // press begin, where is the cursor now — rather than from drag events, because
            // the interface is rebuilt per frame and a drag is a per-frame fact. The press
            // origin decides which gesture this is: on the minimap it pans the view, on the
            // world it draws a band, on a panel (or while a build is armed) it is neither.
            {
                const float w = static_cast<float>(window.width());
                const float h = static_cast<float>(window.height());
                const bool held = window.leftMouseHeld();
                const std::array<float, 2> origin = window.dragOrigin();
                const std::array<float, 2> at = window.cursor();

                // Strictly above the click slop (3 points, backing-scaled) so a release can
                // never be both a click and a band: between the two thresholds is a small
                // dead zone, which is the safe side of the ambiguity.
                constexpr float kBandSlopPx = 8.0f;
                const bool traveled = std::abs(at[0] - origin[0]) + std::abs(at[1] - origin[1])
                                      > kBandSlopPx;

                const bool onMinimap = rm::ui::insideMinimap(minimap, origin[0], origin[1]);
                const bool onPanel =
                    (!buildOptions.empty()
                     && rm::ui::insideBuildPanel(
                         rm::ui::buildPanelLayout(minimap, buildOptions.size()), origin[0],
                         origin[1]))
                    || (!rosterTiles.empty()
                        && rm::ui::insideRoster(
                            rm::ui::rosterLayout(w, h, rosterTiles.size()), origin[0],
                            origin[1]));

                if (held && onMinimap) {
                    // Drag-to-pan: the ground under the finger, continuously. The same
                    // projection the click-jump uses, at frame rate, and clamped to the map
                    // by minimapToWorld — dragging past the letterbox pins to the edge.
                    const std::array<float, 2> where = rm::ui::minimapToWorld(
                        minimap, map->field.widthElmos(), map->field.depthElmos(), at[0],
                        at[1]);
                    window.camera().target = simd_make_float3(
                        where[0], map->field.heightAtWorld(where[0], where[1]), where[1]);
                }

                const bool worldBand = !onMinimap && !onPanel && !armedOption && traveled;
                if (held && worldBand) {
                    // The box: a whisper of fill so the caught area reads, and a hairline
                    // in the lit edge so the bounds are exact. Interface, not effect — the
                    // same vocabulary as every panel border.
                    const rm::ui::Theme theme = hudThemeFor(units);
                    const float left = std::min(origin[0], at[0]);
                    const float top = std::min(origin[1], at[1]);
                    const float wide = std::abs(at[0] - origin[0]);
                    const float tall = std::abs(at[1] - origin[1]);
                    rm::text::appendRect(hudScratch.label, window.labelFont(), left, top,
                                         wide, tall, rm::ui::fade(theme.edgeLit, 0.10f));
                    rm::text::appendRect(hudScratch.label, window.labelFont(), left, top,
                                         wide, 1.0f, theme.edgeLit);
                    rm::text::appendRect(hudScratch.label, window.labelFont(), left,
                                         top + tall - 1.0f, wide, 1.0f, theme.edgeLit);
                    rm::text::appendRect(hudScratch.label, window.labelFont(), left, top,
                                         1.0f, tall, theme.edgeLit);
                    rm::text::appendRect(hudScratch.label, window.labelFont(),
                                         left + wide - 1.0f, top, 1.0f, tall, theme.edgeLit);
                }

                if (leftWasHeld && !held && worldBand) {
                    // Release: everything of the player's whose position projects into the
                    // box. Position rather than silhouette — a unit is its instance's point
                    // here, exactly as it is for a click's pick radius.
                    const float left = std::min(origin[0], at[0]);
                    const float right = std::max(origin[0], at[0]);
                    const float top = std::min(origin[1], at[1]);
                    const float bottom = std::max(origin[1], at[1]);
                    bandScratch.clear();
                    for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                        if (!units.store.slotAlive(slot)
                            || units.armyOf(slot) != units.playerArmy) {
                            continue;
                        }
                        const rm::sim::Transform& tr = units.store.transforms()[slot];
                        const auto screen = rm::worldToScreen(
                            window.camera(),
                            simd_make_float3(rm::sim::fxToFloat(tr.x),
                                             rm::sim::fxToFloat(tr.y),
                                             rm::sim::fxToFloat(tr.z)),
                            w, h);
                        if (screen && (*screen)[0] >= left && (*screen)[0] <= right
                            && (*screen)[1] >= top && (*screen)[1] <= bottom) {
                            bandScratch.push_back(units.store.idAt(slot));
                        }
                    }
                    selected = rm::applyBand<rm::sim::UnitId>(selected, bandScratch,
                                                              window.shiftHeldNow());
                }
                leftWasHeld = held;
            }

            window.setHud(hudScratch.label, hudScratch.readout, hudScratch.image,
                          hudScratch.worldImage);

            // Rings under whatever is selected, rebuilt from scratch every
            // frame. Cheap — a selection is tens of units and each ring is 192
            // vertices of arithmetic — and it is the only way a ring can follow
            // a unit that is walking, which is the whole point of drawing one.
            //
            // The buffer is reused rather than reallocated so that a frame
            // costs no heap traffic; it is declared outside this lambda for
            // exactly that reason.
            // WRECKS FIRST, so the rings and markers a player is reading sit on top of the
            // scorch rather than under it. They are copied in rather than rebuilt: a wreck
            // is permanent and there is nothing to recompute.
            decalVertices.clear();
            appendVisibleWreckDecals(decalVertices, units);
            // Dead selections draw nothing rather than being pruned here: a frame is not
            // where a selection changes, and a ring under a wreck is the bug this avoids.
            for (const rm::sim::UnitId sel : selected) {
                if (!units.store.alive(sel)) {
                    continue;
                }
                const rm::sim::Transform& at = units.store.transforms()[sel.index];
                const std::array<float, 3> ground{rm::sim::fxToFloat(at.x),
                                                  rm::sim::fxToFloat(at.y),
                                                  rm::sim::fxToFloat(at.z)};
                rm::appendSelectionRing(
                    decalVertices, map->field, ground,
                    rm::sim::fxToFloat(units.store.motion()[sel.index].radiusElmos)
                        * kSelectionRingMargin,
                    kSelectionRingColour);

                // THE RANGE RING: the longest firing weapon's reach, only while selected.
                // What a player is deciding with a selection is where to send it, and "from
                // where can it hurt things" is the radius that decision is made against.
                // The longest range rather than one ring per weapon: a triple-barrelled
                // unit's rings differ by metres and draw as one smeared band.
                const rm::unitdef::UnitDef* def =
                    units.catalog.def(units.store.typeAt(sel.index));
                if (def != nullptr) {
                    rm::sim::Fx reach{};
                    for (const rm::unitdef::Weapon& weapon : def->weapons) {
                        if (weapon.fires() && weapon.maxRange > reach) {
                            reach = weapon.maxRange;
                        }
                    }
                    if (reach > rm::sim::Fx{}) {
                        rm::appendSelectionRing(decalVertices, map->field, ground,
                                                rm::sim::fxToFloat(reach), kRangeRingColour,
                                                kRangeRingThicknessElmos);
                    }
                }

                // THE ORDER QUEUE, drawn in the world for a selected unit: a line from the
                // unit through every queued destination, a diamond at each node — and the
                // build orders' nodes in the ghost's cyan, because that node will become a
                // building and its colour should say so before the fact. Only while
                // selected: forty queues at once is a map of spaghetti, and the question
                // "where is THIS unit going" is asked of a selection.
                {
                    const std::deque<rm::sim::Command>& queue =
                        units.store.orders()[sel.index].orders();
                    std::array<float, 2> from{ground[0], ground[2]};
                    for (const rm::sim::Command& order : queue) {
                        if (order.kind == rm::sim::CommandKind::Stop) {
                            continue;  // a stop has no destination to draw a line to
                        }
                        const std::array<float, 2> to{rm::sim::fxToFloat(order.targetX),
                                                      rm::sim::fxToFloat(order.targetZ)};
                        appendGroundSegment(decalVertices, map->field, from, to,
                                            kQueueLineColour, kQueueLineWidthElmos);
                        appendGroundNode(decalVertices, map->field, to,
                                         order.kind == rm::sim::CommandKind::Build
                                             ? kBuildGhostColour
                                             : kQueueNodeColour,
                                         kQueueNodeHalfElmos);
                        from = to;
                    }
                }
            }

            // ...and the BUILD GHOST, wherever the cursor is pointing while a cell is armed.
            //
            // A RING RATHER THAN A MODEL, and it is a real difference from what both reference
            // games draw. A ghost mesh needs the blueprint's model resolved and uploaded for
            // something that may never be built, and the question a player is actually asking
            // is "may it go HERE" — which is a footprint and a yes or no, both of which a ring
            // says. The model is the nicer answer and it is not the load-bearing one.
            //
            // The colour IS the answer: `sitePlaceable` over every cell the footprint touches,
            // so the ring goes red against a cliff before the click rather than after. It is
            // the same call the placement makes, which is what stops the ghost and the order
            // disagreeing about the same spot.
            if (armedOption) {
                const rm::Ray under = rm::screenRay(
                    window.camera(), window.cursor()[0],
                    static_cast<float>(window.height()) - window.cursor()[1],
                    static_cast<float>(window.width()), static_cast<float>(window.height()));
                const std::optional<simd_float3> at = rm::pickGround(under, map->field);
                if (at) {
                    const bool ok = armedPlaceable({at->x, at->z});
                    rm::appendSelectionRing(decalVertices, map->field, {at->x, at->y, at->z},
                                            armedRadius() * kSelectionRingMargin,
                                            ok ? kBuildGhostColour : kBuildGhostBlockedColour);

                    // THE ADJACENCY PREVIEW: a link line from the ghost to every standing
                    // structure whose skirt the armed one would touch here — the bonus
                    // shown BEFORE the mass is spent, because a bonus the player cannot
                    // see at placement time is a mechanic that does not exist
                    // (`core/sim/Adjacency.hpp`). Drawn whenever the skirts would touch,
                    // like the game's own preview, which links every touching structure
                    // without asking whether a buff lands (`gamemain.lua:916-920`).
                    if (const std::optional<rm::UnitTypeIndex> armedType =
                            armedPath().empty()
                                ? std::nullopt
                                : resolveBuildable(units, content, armedPath())) {
                        const rm::sim::UnitCatalog::AdjacencyInfo& mine =
                            units.catalog.adjacency(*armedType);
                        if (mine.participates()) {
                            for (rm::UnitIndex slot = 0; slot < units.store.slotCount();
                                 ++slot) {
                                if (!units.store.slotAlive(slot)
                                    || units.armyOf(slot) != units.playerArmy) {
                                    continue;
                                }
                                const rm::sim::UnitCatalog::AdjacencyInfo& theirs =
                                    units.catalog.adjacency(units.store.typeAt(slot));
                                if (!theirs.participates()) {
                                    continue;
                                }
                                const rm::sim::Transform& t =
                                    units.store.transforms()[slot];
                                if (!rm::sim::skirtsShareEdge(
                                        rm::sim::fxFromFloat(at->x),
                                        rm::sim::fxFromFloat(at->z), mine.skirtHalfXElmos,
                                        mine.skirtHalfZElmos, t.x, t.z,
                                        theirs.skirtHalfXElmos, theirs.skirtHalfZElmos)) {
                                    continue;
                                }
                                rm::appendGroundSegment(
                                    decalVertices, map->field, {at->x, at->z},
                                    {rm::sim::fxToFloat(t.x), rm::sim::fxToFloat(t.z)},
                                    kBuildGhostColour, 1.5f);
                            }
                        }
                    }

                    // THE SILHOUETTE over the ring: the armed blueprint's own model at the
                    // cursor, in the same two colours the ring speaks. The ring stays — it
                    // is the sim's actual test (`sitePlaceable` checks a DISC of the
                    // collision radius, so a rectangle would promise a precision the sim
                    // does not check) — and the model says WHAT would stand here, which no
                    // circle can.
                    //
                    // `ensureDrawableType` is a map lookup after the first call; the first
                    // call loads the model and grows `units.batches`, which next frame's
                    // growth check turns into the re-upload the ghost draw then finds. The
                    // renderer draws nothing for a batch it has not been given yet, so the
                    // one-frame gap is invisible rather than wrong.
                    const std::string path = armedPath();
                    const std::optional<rm::UnitTypeIndex> ghostType =
                        path.empty() ? std::nullopt
                                     : ensureDrawableType(units, content, path);
                    const std::size_t ghostBatch =
                        ghostType ? units.batchOf(*ghostType) : UnitScene::kNoBatch;
                    if (ghostBatch != UnitScene::kNoBatch) {
                        const auto typeIndex = static_cast<std::size_t>(*ghostType);
                        window.setGhost(
                            ghostBatch,
                            rm::UnitInstance{
                                .position = {{at->x, at->y, at->z}},
                                .rotationY = 0.0f,  // spawns face north; so does the promise
                                .scale = typeIndex < units.typeScale.size()
                                             ? units.typeScale[typeIndex]
                                             : 1.0f,
                            },
                            ok ? kBuildGhostColour : kBuildGhostBlockedColour);
                    } else {
                        window.clearGhost();
                    }
                }
                if (!at) {
                    window.clearGhost();  // the sky promises nothing
                }
            } else {
                window.clearGhost();
            }

            // ...and a marker wherever an order was given recently. Aged by the
            // frame's own elapsed time rather than a wall clock, so a marker
            // fades over the same span whatever the frame rate — and by the same
            // reasoning that paces the walk cycles by distance covered.
            for (OrderMark& mark : orderMarks) {
                mark.age += elapsed;
                rm::appendOrderMarker(decalVertices, map->field, mark.position,
                                      kOrderMarkerColour, mark.age);
            }
            for (OrderMark& mark : noRouteMarks) {
                mark.age += elapsed;
                rm::appendNoRouteMarker(decalVertices, map->field, mark.position, mark.age);
            }
            // Dropped once they have nothing left to draw. The append above is
            // silent past its lifetime, so this is housekeeping rather than
            // correctness — without it the list grows for as long as the app runs.
            std::erase_if(orderMarks, [](const OrderMark& mark) {
                return mark.age >= rm::kOrderMarkerSecondsToLive;
            });
            std::erase_if(noRouteMarks, [](const OrderMark& mark) {
                return mark.age >= rm::kOrderMarkerSecondsToLive;
            });

            window.setGroundDecals(decalVertices);
            // ...and an outline around each selected unit, which is what a ring
            // cannot do at a low camera angle where the units hide their own rings.
            // The renderer wants (batch, instance) — where the unit ended up in THIS frame's
            // gather. That is a projection of the selection, not the selection itself, which
            // is why it is built here and not stored.
            selectionScratch.clear();
            for (const rm::sim::UnitId sel : selected) {
                if (const std::optional<rm::SelectionEntry> where = units.drawnAt(sel)) {
                    selectionScratch.push_back(*where);
                }
            }
            window.setSelection(selectionScratch);
        });

        window.show();

        RMBenchWatcher* watcher = nil;
        if (bench.enabled) {
            std::printf("benchmarking %zu frames (discarding %zu warmup)\n  %s\n",
                        bench.frames, bench.warmup,
                        describeQuality(settings, propInstances, marchDust.size()).c_str());
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
    return 0;
}

} // namespace rm::app
