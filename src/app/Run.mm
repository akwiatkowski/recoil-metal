#include "app/Run.hpp"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

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
            std::vector<rm::DecalVertex> vertices{units.wreckDecals.begin(),
                                                  units.wreckDecals.end()};
            {
                const std::size_t rings = parseCount(argc, argv, "--select");
                std::vector<rm::SelectionEntry> captured;
                std::size_t made = 0;
                for (std::size_t batch = 0; batch < units.drawScratch.size() && made < rings;
                     ++batch) {
                    for (std::size_t i = 0; i < units.drawScratch[batch].size() && made < rings;
                         ++i, ++made) {
                        const rm::UnitIndex slot = units.drawSlotOf[batch][i];
                        const rm::sim::Transform& at = units.store.transforms()[slot];
                        rm::appendSelectionRing(
                            vertices, map->field,
                            {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                             rm::sim::fxToFloat(at.z)},
                            rm::sim::fxToFloat(units.store.motion()[slot].radiusElmos)
                                * kSelectionRingMargin,
                            kSelectionRingColour);
                        captured.push_back(rm::SelectionEntry{batch, i});
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
            std::vector<rm::Particle> shotParticles{marchDust.begin(), marchDust.end()};
            appendSceneIcons(shotParticles, units, renderer.camera());
            renderer.setParticles(shotParticles);

            // The HUD in a capture too. A screenshot is how this project verifies anything,
            // and an interface only visible in a live window cannot be checked at all.
            rm::ui::Geometry hud;
            rm::ui::build(hud, renderer.labelFont(), renderer.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, marchOptions.seconds),
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
            rm::ui::appendMinimap(hud, renderer.labelFont(), hudThemeFor(units),
                                  rm::ui::minimapLayout(static_cast<float>(shot.width),
                                                        static_cast<float>(shot.height)),
                                  map->field.widthElmos(), map->field.depthElmos(), pips, view);

            renderer.setHud(hud.label, hud.readout);

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
        window.onKey([&window](char key) {
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
            } else if (key == 'p') {
                const bool visible = !window.propsVisible();
                window.setPropsVisible(visible);
                std::printf("props %s\n", visible ? "on" : "off");
                std::fflush(stdout);
            }
        });

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

        // Dust. Particles live across frames — that is the point of them — so
        // this list persists and is aged rather than rebuilt, unlike the decals.
        // The emitter list IS rebuilt each frame, because it is a view of where
        // the units are now.
        std::vector<rm::Particle> particles;
        std::vector<rm::DustEmitter> dustEmitters;
        float dustDebt = 0.0f;
        float ambientDebt = 0.0f;
        // A fixed seed, so a scene is the same every run: the same reason the unit
        // scatter takes one.
        std::uint32_t dustSeed = 0x51ED27u;

        // Reused across frames so that rebuilding the rings costs no
        // allocation — the capacity settles after the first large selection.
        std::vector<rm::DecalVertex> decalVertices;

        window.onClick([&](const rm::Ray& ray, rm::MouseButton button,
                           rm::MouseModifiers mods) {
            // THE MINIMAP FIRST, because it is in front of the world (§7 P7.4). A click on the
            // panel is about the panel; without this check the ray under it would also select
            // whatever unit happens to be behind the minimap, which is the single most
            // irritating bug an overlay can have.
            const rm::ui::MinimapLayout minimap =
                rm::ui::minimapLayout(static_cast<float>(window.width()),
                                      static_cast<float>(window.height()));
            if (rm::ui::insideMinimap(minimap, mods.pointX, mods.pointY)) {
                const std::array<float, 2> where =
                    rm::ui::minimapToWorld(minimap, map->field.widthElmos(),
                                           map->field.depthElmos(), mods.pointX, mods.pointY);
                // Jump, keeping the camera's distance and angles — a minimap click moves where
                // you are looking, not how. Height sampled from the terrain so the target sits
                // on the ground rather than at y = 0, which on a hill would look like a zoom.
                window.camera().target = simd_make_float3(
                    where[0], map->field.heightAtWorld(where[0], where[1]), where[1]);
                return;
            }

            if (button == rm::MouseButton::Left) {
                // What the click MEANS is decided in core/scene/Selection.hpp,
                // where it can be tested. Nothing is left to do here: the units
                // themselves are not repainted, and the rings are rebuilt from
                // this list by the frame callback.
                const bool addToSet = mods.shift || mods.command || mods.control;
                selected = rm::applyClick<rm::sim::UnitId>(
                    selected, pickAcrossBatches(ray, units), addToSet);
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

            // Marked before the routing is attempted, and deliberately: the mark
            // answers "did that click land, and where", which is true even if
            // every unit then reports no route. A marker that appeared only on
            // success would leave the player unsure whether the click registered.
            orderMarks.push_back(OrderMark{
                .position = {ground->x, ground->y, ground->z},
                .age = 0.0f,
            });

            std::size_t failed = 0;
            for (const rm::sim::UnitId sel : selected) {
                if (!units.store.alive(sel)) {
                    continue;  // selected, then killed before the order was given
                }
                // Each unit routes on the map ITS limits see. Two units given
                // the same order can legitimately get different answers, and
                // one of them can be "no route" while the other walks off.
                const auto type = static_cast<std::size_t>(units.store.typeAt(sel.index));
                const rm::sim::PassabilityGrid& grid =
                    passability.gridFor(units.maxSlopeDegrees[type],
                                        units.maxWaterDepthElmos[type]);
                // SHIFT QUEUES IT (§7 P4.1). The same modifier adds to the selection on the
                // left button and appends to the order queue on the right, which is what
                // every RTS this engine reads content from does.
                if (!issueMove(units, grid, map->field, sel,
                               playerDriving(units, units.playerArmy),
                               static_cast<rm::TickIndex>(matchTicks),
                               rm::sim::fxFromFloat(ground->x),
                               rm::sim::fxFromFloat(ground->z), mods.shift)) {
                    ++failed;
                }
            }
            if (failed > 0) {
                std::printf("no route there for %zu of %zu units\n", failed, selected.size());
            } else if (mods.shift) {
                // Only for a queued order, and only the length: this is the one piece of
                // feedback the world does not already show. A plain order is legible from the
                // unit turning; a queued one looks like nothing happened until the unit gets
                // there. Drawing the queue in the world is UI-3's job.
                const rm::sim::UnitId first = selected.front();
                if (units.store.alive(first)) {
                    std::printf("queued: %zu order(s) for the first of %zu selected\n",
                                units.store.orders()[first.index].size(), selected.size());
                }
            }
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
            units.gatherForDrawing(clock.alpha());

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
            iconScratch.assign(particles.begin(), particles.end());
            appendSceneIcons(iconScratch, units, window.camera());
            window.setParticles(iconScratch);

            hudScratch.clear();
            rm::ui::build(hudScratch, window.labelFont(), window.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, matchSeconds),
                          static_cast<float>(window.width()),
                          static_cast<float>(window.height()));

            // THE MINIMAP (§7 P7.4), appended to the same geometry the HUD builds — it is
            // rectangles in screen space, which is what `text::appendRect` already draws, so it
            // needs no pipeline of its own. That is the other half of "nearly free".
            appendMinimapPips(minimapPips, units);
            appendViewFootprint(minimapView, window.camera(), map->field,
                                static_cast<float>(window.width()),
                                static_cast<float>(window.height()));
            rm::ui::appendMinimap(hudScratch, window.labelFont(), hudThemeFor(units),
                                  rm::ui::minimapLayout(static_cast<float>(window.width()),
                                                        static_cast<float>(window.height())),
                                  map->field.widthElmos(), map->field.depthElmos(), minimapPips,
                                  minimapView);

            window.setHud(hudScratch.label, hudScratch.readout);

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
            decalVertices.assign(units.wreckDecals.begin(), units.wreckDecals.end());
            // Dead selections draw nothing rather than being pruned here: a frame is not
            // where a selection changes, and a ring under a wreck is the bug this avoids.
            for (const rm::sim::UnitId sel : selected) {
                if (!units.store.alive(sel)) {
                    continue;
                }
                const rm::sim::Transform& at = units.store.transforms()[sel.index];
                rm::appendSelectionRing(
                    decalVertices, map->field,
                    {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                     rm::sim::fxToFloat(at.z)},
                    rm::sim::fxToFloat(units.store.motion()[sel.index].radiusElmos)
                        * kSelectionRingMargin,
                    kSelectionRingColour);
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
            // Dropped once they have nothing left to draw. The append above is
            // silent past its lifetime, so this is housekeeping rather than
            // correctness — without it the list grows for as long as the app runs.
            std::erase_if(orderMarks, [](const OrderMark& mark) {
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
