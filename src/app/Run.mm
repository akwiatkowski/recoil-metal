#include "app/Run.hpp"

#include "core/log/Log.hpp"

#include "core/audio/CueEvents.hpp"
#include "core/audio/Xwb.hpp"
#include "core/audio/Cues.hpp"
#include "platform/Audio.hpp"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include "core/sim/Adjacency.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/StateHash.hpp"
#include "core/ui/CommandPanel.hpp"
#include "core/ui/PanelPages.hpp"

#include <algorithm>
#include <cctype>
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
    const rm::ui::UiViewport viewport = self.window->uiViewport();
    const rm::ui::Extent extent = viewport.logicalExtent;
    std::printf("%s\n", recorder.summaryLine("recoil-metal").c_str());
    std::printf("  measured viewport: %.0f x %.0f points at %.2fx backing\n",
                static_cast<double>(extent.width), static_cast<double>(extent.height),
                static_cast<double>(viewport.backingScale));
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
            rm::log::writef(rm::log::Level::Error, "benchmark", "could not write %s",
                            self.csvPath.UTF8String);
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
        rm::log::writef(rm::log::Level::Error, "capture", "nothing to write to %s",
                        path.c_str());
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
        rm::log::writef(rm::log::Level::Error, "capture", "failed to encode %s",
                        path.c_str());
    }
    return ok;
}

namespace {

/// The roster's tile list as one number: FNV-1a 64-bit over each tile's blueprint id.
///
/// This is the icon atlas's repack key for the selection panel, and it has to answer "is
/// this the same GROUP OF TYPES the atlas was packed for" — which the tile count cannot,
/// because two selections of different units group into equal-length lists. Ids are what
/// the atlas actually drew; counts and health are per-frame state no icon depends on.
[[nodiscard]] std::uint64_t rosterKeyFor(const std::vector<rm::ui::RosterTile>& tiles) {
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    std::uint64_t key = kFnvOffset;
    for (const rm::ui::RosterTile& tile : tiles) {
        for (const char c : tile.id) {
            key = (key ^ static_cast<std::uint8_t>(c)) * kFnvPrime;
        }
        key = (key ^ static_cast<std::uint8_t>('/')) * kFnvPrime;  // tile separator
    }
    return key;
}

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
        const float projectileAlpha =
            projectile.pendingImpact == rm::sim::ImpactType::Invalid ? alpha : 0.0f;
        const rm::sim::Fx x = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[0])
            + rm::sim::fxToFloat(projectile.velocity[0]) * projectileAlpha);
        const rm::sim::Fx z = rm::sim::fxFromFloat(
            rm::sim::fxToFloat(projectile.position[2])
            + rm::sim::fxToFloat(projectile.velocity[2]) * projectileAlpha);
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
            out.push_back(units.combatVisualEvent(event));
        }
    }
}

} // namespace

// --- The headless interface ------------------------------------------------------------------
//
// Everything a capture shows that a live window would draw under a cursor: selection and range
// rings, the order marker, the HUD with its panels, the minimap, strategic icons, contact blips,
// construction sites, projectiles, the hovered card and the placement ghost. ONE FUNCTION for
// the screenshot and the offscreen benchmark, so the benchmark's HUD cost is the cost of the
// interface the screenshot proves, not a second approximation of it.
void composeHeadlessInterface(rm::Renderer& renderer, const Session& session,
                              const rm::ui::UiViewport& viewport) {
    RM_UNPACK_SESSION(session)
    renderer.setWeaponMaterials(units.weaponVisuals.materials);
    const bool weaponGallery = hasFlag(argc, argv, "--weapon-gallery");
    if (weaponGallery) {
        const float x = map->field.widthElmos() * 0.5f;
        const float z = map->field.depthElmos() * 0.5f;
        renderer.focusOn({x, map->field.heightAtWorld(x,z) + 100.0f, z}, 170.0f);
        renderer.camera().pitch = 1.4f;
    }
            // Selection rings need a selection, and a headless run has no
            // clicks. `--select N` rings the first N units and `--select-type ID`
            // rings the first unit with that blueprint id, so that what a
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
            appendResourceDeposits(vertices, units, map->field);
            {
                const std::string_view selectedType = parseSelectType(argc, argv);
                const std::size_t rings =
                    selectedType.empty() ? parseCount(argc, argv, "--select") : 1;
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
                         ++i) {
                        const rm::UnitIndex slot = units.drawSlotOf[batch][i];
                        if (units.playerArmy != rm::sim::kNoArmy
                            && units.armyOf(slot) != units.playerArmy) {
                            continue; // Headless player selection obeys ownership, like clicking.
                        }
                        if (!selectedType.empty()) {
                            const rm::unitdef::UnitDef* def =
                                units.catalog.def(units.store.typeAt(slot));
                            if (def == nullptr || def->name != selectedType) {
                                continue;
                            }
                        }
                        const rm::sim::Transform& at = units.store.transforms()[slot];
                        const std::array<float, 3> ground{rm::sim::fxToFloat(at.x),
                                                          rm::sim::fxToFloat(at.y),
                                                          rm::sim::fxToFloat(at.z)};
                        appendUnitSelection(
                            vertices, map->field, ground,
                            rm::sim::fxToFloat(units.store.motion()[slot].radiusElmos)
                                * kSelectionRingMargin,
                            session.uiProfile);
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
                        // Where the ringed unit stands, so a script can place a ghost or aim a
                        // second capture beside it without guessing the map's start positions.
                        std::printf("    ring %zu at (%.0f, %.0f)\n", made + 1,
                                    static_cast<double>(ground[0]),
                                    static_cast<double>(ground[2]));
                        ++made;
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

                // Interface decals are load-bearing; shield shells are presentation and may be
                // truncated by the renderer's fixed frame buffer only after the UI is complete.
                vertices.insert(vertices.end(), units.shieldScratch.begin(),
                                units.shieldScratch.end());

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
            // THE CAPTURE SCALES LIKE THE WINDOW DOES, from the same viewport contract, or a
            // capture would stop being evidence about the interface the player sees.
            const rm::ui::UiViewport& shotViewport = viewport;
            renderer.setUiViewport(shotViewport);
            const rm::ui::FrameLayout shotFrame = rm::ui::frameLayout(shotViewport);
            const rm::ui::Theme baseTheme = hudThemeFor(units, session.uiProfile);

            // Health bars in a capture too, for the usual reason: a battle screenshot is
            // the one place a damaged unit reliably exists to verify them against.
            appendHealthBars(hud, units, renderer.camera(), renderer.labelFont(), shotViewport);
            const auto shotProgressBars = appendConstructionBars(hud, units, renderer.camera(),
                map->field, renderer.labelFont(), shotViewport, capturedSelection);
            std::printf("  construction bars: %zu\n", shotProgressBars);

            // THE MINIMAP IN A CAPTURE TOO, for the same reason the rest of the HUD is here: a
            // screenshot is how this project verifies anything, and an interface only visible in
            // a live window cannot be checked at all. Adding it to the frame loop alone is how
            // the first version of P7.4 looked correct and produced an empty corner in every
            // capture.
            std::vector<rm::ui::MinimapPip> pips;
            std::vector<std::array<float, 2>> view;
            appendMinimapPips(pips, units);
            appendViewFootprint(view, renderer.camera(), map->field, shotViewport);
            const rm::ui::MinimapLayout shotMinimap = rm::ui::minimapLayout(shotFrame);
            const rm::ui::MinimapProjection shotMinimapProjection =
                rm::ui::minimapProjection(shotMinimap, map->field.widthElmos(),
                                           map->field.depthElmos());
            const bool shotPreview = map->preview.width > 0;
            if (shotPreview) {
                const rm::ui::Rect& contentRect = shotMinimapProjection.content;
                renderer.setMinimapRect(contentRect.x, contentRect.y, contentRect.width,
                                        contentRect.height);
            }
            // The build panel and the roster, for whatever `--select` ringed. This is what
            // makes either verifiable at all: a headless run has no clicks, so `--select` is
            // the only way interface that appears on selection reaches a screenshot.
            //
            // NOT SCOPED TO THE BUILD PANEL, which the first version was: the icons were packed
            // inside `if (!shotOptions.empty())`, so a selection of tanks — nothing that builds
            // — drew a roster with no pictures in it.
            std::vector<rm::ui::BuildOption> shotOptions;
            std::vector<rm::ui::RosterTile> shotRoster;
            std::vector<rm::sim::UnitId> shotBuilders;
            rm::app::BuildSelection shotWho;
            gatherRoster(units, capturedSelection, shotRoster);
            gatherBuilderCandidates(units, capturedSelection, shotBuilders);
            gatherBuildOptions(units, activeBuilderFor(shotBuilders), baseTheme, shotOptions,
                               shotWho);
            for (const auto& option : shotOptions) {
                std::printf("  hud-build-option: id=%s upgrade=%d queued-upgrade=%d\n",
                    option.id.c_str(), option.upgrade, option.queuedUpgrade);
            }
            // Pair pixels with facts from the same selection, so a visually plausible idle
            // capture cannot pass an active-construction regression.
            const auto shotWork = constructionCard(units, activeBuilderFor(shotBuilders));
            const auto shotProduction = gatherProduction(units, activeBuilderFor(shotBuilders));
            std::uint64_t queuedProducts = 0;
            if (shotProduction) {
                for (const auto& entry : shotProduction->queue) queuedProducts += entry.count;
            }
            std::printf("  hud-state: selected=%zu types=%zu work=%s progress=%d flow=%s queued=%llu\n",
                capturedSelection.size(), shotRoster.size(), shotWork ? shotWork->title.c_str() : "NONE",
                shotWork ? static_cast<int>(*shotWork->progress * 100.0f) : 0,
                shotWork ? shotWork->rows.back().value.c_str() : "NONE",
                static_cast<unsigned long long>(queuedProducts));
            std::vector<const rm::unitdef::UnitDef*> shotCommandSelection;
            for (const rm::sim::UnitId id : capturedSelection) {
                if (units.store.alive(id)) {
                    shotCommandSelection.push_back(
                        units.catalog.def(units.store.typeAt(id.index)));
                }
            }
            const rm::ui::CommandAvailability shotCommandAvailable =
                rm::ui::commandAvailability(shotCommandSelection);
            const std::size_t hoverAt = parseCount(argc, argv, "--hover");
            const std::optional<std::size_t> shotHovered =
                hoverAt > 0 && hoverAt <= shotOptions.size()
                    ? std::optional<std::size_t>{hoverAt - 1}
                    : std::nullopt;
            const std::size_t shotCapacity =
                shotFrame.buildColumns * static_cast<std::size_t>(rm::ui::kBuildRows);
            const std::size_t shotPage =
                shotHovered && shotCapacity > 0 ? *shotHovered / shotCapacity : 0;
            const rm::ui::BuildPanelLayout shotPanel =
                rm::ui::buildPanelLayout(shotFrame, shotOptions.size(), shotPage);
            const rm::ui::RosterLayout shotRosterLayout =
                rm::ui::rosterLayout(shotFrame, shotRoster.size());

            // Packed unconditionally now: the strategic glyphs exist with nothing selected
            // at all, which is precisely the far-zoom capture that shows them.
            rm::app::ensureStrategicIconArt(units, content);
            std::size_t shotStrategicBase = 0;
            rm::app::PackedInterfaceAtlas shotAtlas = rm::app::packInterfaceIcons(
                content, shotOptions, shotRoster,
                {.first = shotPanel.first, .count = shotPanel.shown},
                {.first = shotRosterLayout.first, .count = shotRosterLayout.shown},
                session.uiProfile, units.strategicIconArt, &shotStrategicBase);
            renderer.setIconAtlas(shotAtlas.texture);
            const rm::ui::Theme shotTheme =
                hudThemeFor(units, session.uiProfile, shotAtlas.skin);
            rm::ui::build(hud, renderer.labelFont(), renderer.readoutFont(), shotTheme,
                          hudStateFrom(units, marchOptions.seconds, session.uiProfile), shotFrame);
            rm::ui::appendMinimap(hud, renderer.labelFont(), shotTheme, shotMinimap,
                                  map->field.widthElmos(), map->field.depthElmos(), pips, view,
                                  !shotPreview);
            std::vector<std::optional<rm::app::StrategicIconRef>> shotRefs;
            rm::app::buildStrategicIconRefs(units, shotStrategicBase, shotRefs);
            rm::app::appendStrategicIcons(hud, units, renderer.camera(), shotViewport, shotRefs);
            rm::app::appendContactBlips(hud, units, renderer.camera(), map->field,
                                        renderer.labelFont(), shotViewport);
            appendSceneIcons(shotParticles, units, renderer.camera(), shotRefs);

            // THE CONSTRUCTION SITES IN A CAPTURE TOO, for the reason the HUD is here: a
            // screenshot is how this project verifies anything, and an effect only visible in
            // a live window cannot be checked at all. The upload afterwards is not optional —
            // this is where the product's model is loaded, and a batch created after the last
            // `setUnits` draws nothing.
            std::vector<rm::Renderer::ConstructionDraw> shotSites;
            rm::app::gatherConstructions(units, content, map->field, shotSites);
            if (!shotSites.empty()) {
                renderer.setUnits(units.textures.all(), units.batches);
                renderer.setConstructions(shotSites);
                renderer.setConstructionTime(marchOptions.seconds);
                rm::app::appendConstructionEffects(vertices, shotParticles, units, map->field,
                                                   marchOptions.seconds);
                // The decals were pushed above; pushing them again replaces that list with
                // this longer one, which is what `setGroundDecals` means.
                renderer.setGroundDecals(vertices);
                std::printf("  building: %zu site(s) under construction\n", shotSites.size());
                for (const rm::Renderer::ConstructionDraw& site : shotSites) {
                    // Where and how far along, because a capture is how this is checked and a
                    // site that drew nothing looks exactly like a site that was never there.
                    std::printf("    at (%.0f, %.0f), %.0f%% of %.0f elmos tall\n",
                                static_cast<double>(site.instance.position[0]),
                                static_cast<double>(site.instance.position[2]),
                                static_cast<double>(site.progress * 100.0f),
                                static_cast<double>(site.heightElmos));
                }
            }
            // The shots in flight at the captured tick — the reason a battle screenshot
            // finally shows the battle. No trails headless: the capture has no aging
            // particle list for them to fade through.
            std::vector<rm::sim::Projectile> shotProjectiles;
            gatherVisibleProjectiles(shotProjectiles, units);
            rm::appendProjectiles(shotParticles, shotProjectiles, 0.0f,
                                  renderer.camera().elmosPerPoint(
                                      rm::kIconReferenceHeightPoints), &units.weaponVisuals);
            if (weaponGallery) {
                shotParticles.clear();
                const std::array<std::pair<const char*, const char*>, 6> examples{{
                    {"UEF Gauss", "/projectiles/TDFGauss01/TDFGauss01_proj.bp"},
                    {"Aeon Disruptor", "/projectiles/ADFDisruptor01/ADFDisruptor01_proj.bp"},
                    {"Cybran heavy laser", "/projectiles/CDFLaserHeavy01/CDFLaserHeavy01_proj.bp"},
                    {"Seraphim Oh cannon", "/projectiles/SDFOhCannon01/SDFOhCannon01_proj.bp"},
                    {"Cybran particle beam", "URB2301:MainGun"},
                    {"Cybran microwave beam", "URL0402:MainGun"},
                }};
                const auto centre = renderer.camera().target;
                for (std::size_t i=0; i<examples.size(); ++i) {
                    const float z = centre.z + (static_cast<float>(i)-2.5f)*16.0f;
                    const bool beam = i >= 4;
                    const std::array<float,3> from{centre.x + (beam ? -25.0f : 25.0f), centre.y, z};
                    const std::array<float,3> to{centre.x+65.0f, centre.y, z};
                    const auto first = shotParticles.size();
                    if (!beam && hasFlag(argc, argv, "--impact-gallery")) {
                        const std::array<const char*,4> weapons{"UEL0201:MainGun", "UAL0201:MainGun",
                            "URL0107:LaserArms", "XSL0201:MainGun"};
                        const auto at = [&](float x) {
                            return std::array<rm::sim::Fx,3>{rm::sim::fxFromFloat(x),
                                rm::sim::fxFromFloat(centre.y),rm::sim::fxFromFloat(z)};
                        };
                        const std::array<rm::sim::Event,3> events{{
                            {.kind=rm::sim::EventKind::WeaponFired, .at2=at(centre.x-25),
                             .visualId=weapons[i], .visualDirection={rm::sim::Fx::fromInt(1),{}, {}}},
                            {.kind=rm::sim::EventKind::ProjectileImpact, .at=at(centre.x+25),
                             .impactType=rm::sim::ImpactType::Terrain, .visualId=examples[i].second},
                            {.kind=rm::sim::EventKind::ProjectileImpact, .at=at(centre.x+65),
                             .impactType=rm::sim::ImpactType::Unit, .visualId=examples[i].second},
                        }};
                        rm::CombatEffectState effects;
                        rm::emitCombatEffects(shotParticles, events, &units.weaponVisuals, &effects);
                        // Inspect the flash fifty milliseconds after creation, as a live frame
                        // does between simulation ticks; zero-age ramps can be transparent.
                        for (auto p=first; p<shotParticles.size(); ++p) shotParticles[p].age += 0.05f;
                        rm::emitCombatEffects(shotParticles, {}, &units.weaponVisuals, &effects, 0.05f);
                        std::printf("impact gallery: %s muzzle, land and unit hits\n", weapons[i]);
                    } else {
                      rm::appendWeaponVisual(shotParticles, units.weaponVisuals, examples[i].second,
                        from, to, 1.0f, beam, 0.6f, false);
                    if (beam) {
                        for (auto p=first; p<shotParticles.size(); ++p) shotParticles[p].age = 0.2f;
                    } else {
                        for (const auto id : units.weaponVisuals.find(examples[i].second))
                            rm::emitWeaponParticles(shotParticles, units.weaponVisuals.materials[id],
                                id, from, from, 0, 0.2f, id+1);
                    }
                    }
                    const auto screen = rm::worldToScreen(renderer.camera(),
                        simd_make_float3(centre.x-95.0f, centre.y,z),
                        shotViewport.hudExtent().width, shotViewport.hudExtent().height);
                    if (screen) (void)rm::text::appendText(hud.label, renderer.labelFont().glyphs,
                        examples[i].first, (*screen)[0], (*screen)[1], {1,1,1,1});
                    std::printf("weapon gallery: %s, %zu layers\n", examples[i].second,
                        units.weaponVisuals.find(examples[i].second).size());
                }
                // Exercise the scene-copy path using an original refracting emitter too.
                const auto refract = std::ranges::find_if(units.weaponVisuals.materials,
                    [](const auto& material) { return material.blend == rm::EffectBlend::Refract; });
                if (refract != units.weaponVisuals.materials.end()) {
                    const auto id = static_cast<std::uint32_t>(refract-units.weaponVisuals.materials.begin());
                    std::uint32_t seed = id+1;
                    auto particle = rm::makeWeaponParticle(*refract, id, 0.5f,
                        {centre.x+25, centre.y, centre.z+56}, seed);
                    particle.age = particle.lifetime*0.5f;
                    shotParticles.push_back(particle);
                    std::printf("weapon gallery refraction: %s\n", refract->emitter.c_str());
                }
            }
            renderer.setParticles(shotParticles);

            if (!shotOptions.empty()) {
                // `--hover N` lights the Nth option (1-based) and draws its info card, for
                // the same reason `--select` exists: a headless run has no cursor, and
                // interface that appears only under one cannot reach a screenshot.
                rm::ui::appendBuildPanel(hud, renderer.labelFont(), renderer.readoutFont(),
                                         shotTheme, shotPanel, shotOptions, shotHovered,
                                         shotWho.name, shotWho.role);
                std::printf("  build panel: %zu options for %s\n", shotOptions.size(),
                            shotWho.name.c_str());

                // `--ghost X Z`: the silhouette of the hovered option (or the first) at a
                // world point, for the reason --select and --hover exist — the ghost only
                // appears under a cursor, and a headless run has none.
                for (int i = 1; i + 2 < argc; ++i) {
                    if (std::string{argv[i]} != "--ghost") {
                        continue;
                    }
                    float gx = static_cast<float>(std::atof(argv[i + 1]));
                    float gz = static_cast<float>(std::atof(argv[i + 2]));
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
                        const auto snapped = snapResourceSite(units, *type, {gx, gz});
                        gx = snapped[0];
                        gz = snapped[1];
                        const auto& grid = passability.gridForBuild(units, typeIndex,
                            static_cast<std::size_t>(units.store.typeAt(shotWho.builder.index)));
                        const auto* def = units.catalog.def(*type);
                        const bool placeable = units.terrain(map->field).resourceSitePlaceable(
                            def->buildRestriction, rm::sim::fxFromFloat(gx), rm::sim::fxFromFloat(gz))
                            && rm::sim::buildSitePlaceable(grid, rm::sim::fxFromFloat(gx),
                                rm::sim::fxFromFloat(gz), rm::sim::fxFromFloat(def->collisionRadiusElmos),
                                units.store, units.catalog, units.building);
                        renderer.setGhost(
                            batch,
                            rm::UnitInstance{
                                .position = {{gx, map->field.heightAtWorld(gx, gz), gz}},
                                // The facing the building will actually have, from the one
                                // authority all three previews and the spawn share.
                                .rotationY = rm::sim::radiansFromBrad(rm::app::structureFacing(
                                    map->field, rm::sim::fxFromFloat(gx),
                                    rm::sim::fxFromFloat(gz))),
                                .scale = typeIndex < units.typeScale.size()
                                             ? units.typeScale[typeIndex]
                                             : 1.0f,
                            },
                            placeable ? kBuildGhostColour : kBuildGhostBlockedColour);
                        std::printf("  ghost: %s at %.0f, %.0f\n",
                                    shotOptions[option].id.c_str(),
                                    static_cast<double>(gx), static_cast<double>(gz));
                    }
                    break;
                }
            }

            const std::size_t hoverCommand = parseCount(argc, argv, "--hover-command");
            const std::optional<std::size_t> shotHoveredCommand =
                hoverCommand > 0 && hoverCommand <= rm::ui::kCommandSlots
                    ? std::optional<std::size_t>{hoverCommand - 1} : std::nullopt;
            if (!shotRoster.empty()) {
                const rm::ui::InfoCard inspector =
                    shotHovered && *shotHovered < shotOptions.size()
                        ? rm::ui::buildOptionCard(shotOptions[*shotHovered], session.uiProfile)
                        : shotHoveredCommand
                            ? rm::ui::commandCard(rm::ui::kCommandDescriptors[*shotHoveredCommand],
                                shotCommandSelection)
                            : selectedUnitCard(units, shotRoster.front(), activeBuilderFor(shotBuilders));
                if (shotHoveredCommand) {
                    std::printf("  command inspector: %s\n", inspector.rows.front().value.c_str());
                }
                rm::ui::appendRoster(hud, renderer.labelFont(), renderer.readoutFont(),
                                     shotTheme,
                                     shotRosterLayout,
                                     shotRoster, std::nullopt, &inspector);
                std::printf("  roster: %zu type(s) selected\n", shotRoster.size());
            }

            rm::ui::appendCommandRack(
                hud, renderer.labelFont(), renderer.readoutFont(), shotTheme,
                rm::ui::commandRackLayout(shotFrame, !capturedSelection.empty()),
                shotCommandAvailable, shotHoveredCommand);

            if (shotProduction) {
                rm::ui::appendProductionPanel(hud, renderer.labelFont(), renderer.readoutFont(),
                    shotTheme, rm::ui::productionPanelRect(shotFrame), *shotProduction);
                std::printf("  production panel: %zu rows, repeat=%s\n",
                    shotProduction->queue.size(), shotProduction->repeat ? "on" : "off");
            }

            renderer.setHud(hud);
            const rm::ui::UiCapacityReport& hudCapacity = renderer.uiCapacityReport();
            std::printf("  hud vertices (uploaded/submitted/capacity):");
            for (std::size_t index = 0; index < rm::ui::kUiLayerCount; ++index) {
                const auto layer = static_cast<rm::ui::UiLayer>(index);
                const rm::ui::UiLayerUsage& usage = hudCapacity.layers[index];
                const std::string_view name = rm::ui::uiLayerName(layer);
                std::printf(" %.*s=%zu/%zu/%zu", static_cast<int>(name.size()), name.data(),
                            usage.uploaded, usage.submitted, usage.capacity);
            }
            std::printf("\n");
}

int runOffscreenBenchmark(const Session& session) {
    RM_UNPACK_SESSION(session)
        // --- Headless offscreen benchmark ----------------------------------
        // No NSApplication, no window, no display link, hence no vsync. This is
        // the only mode whose CPU numbers describe the renderer instead of the
        // display, so it is the one comparable against another engine.
            rm::Renderer renderer{nullptr};
            renderer.setUiEffects(session.uiEffects.level);
            renderer.setTerrain(mesh);
            applyGround(renderer, *map);
            renderer.setUnits(units.textures.all(), units.batches);
            units.applyFog(renderer);
            renderer.setProps(props.textures.all(), props.batches);
            renderer.setAnimationTime(animationTime);
            renderer.setReflections(settings.reflections);
            renderer.setStratumNormals(settings.stratumNormals);
            renderer.setRefraction(settings.refraction);
            // --focus works here too, so the benchmark can measure a close
            // camera as well as a whole-map one. They are different workloads:
            // anything that culls to what the camera sees is invisible at full
            // zoom and everything up close.
            if (focus > 0.0f) {
                focusOnFirstUnit(renderer, units, focus);
            }
            if (look.enabled) {
                renderer.focusOn({look.x, map->field.heightAtWorld(look.x, look.z), look.z},
                                 look.radiusElmos);
            }

            // `--bench-hud`: the interface too, exactly as a screenshot composes it, so the
            // frame time includes the HUD a player sees. OPT-IN, because every published
            // number in docs/benchmark-m4.md is world-only and must stay comparable. The
            // benchmark size is the logical size and `--backing` scales the pixels, as for
            // a capture.
            const bool withHud = hasFlag(argc, argv, "--bench-hud");
            const auto pixelsWide = static_cast<unsigned int>(std::lround(bench.width * shot.backing));
            const auto pixelsHigh = static_cast<unsigned int>(std::lround(bench.height * shot.backing));
            const rm::ui::UiViewport benchViewport = rm::ui::UiViewport::full(
                static_cast<float>(bench.width), static_cast<float>(bench.height),
                shot.backing, session.uiScale);
            composeHeadlessInterface(renderer, session, benchViewport);
            if (!withHud) {
                // SAME COMPOSITION, MINUS THE HUD. In particular, both variants now make the
                // same strategic-glyph/fallback-square decision and carry the same particles,
                // construction effects and world overlays. Clearing after composition leaves
                // one measured difference: the 2D interface draw itself.
                renderer.setMinimapRect(0.0f, 0.0f, 0.0f, 0.0f);
                renderer.setHud({});
            }

            std::printf("offscreen benchmark: %ux%u points at backing %.2f -> %ux%u pixels,"
                        " %s, %zu frames (discarding %zu warmup), %zu frames in flight, no vsync\n",
                        bench.width, bench.height, static_cast<double>(shot.backing), pixelsWide,
                        pixelsHigh, withHud ? "with HUD" : "world only", bench.frames,
                        bench.warmup, rm::Renderer::kMaxFramesInFlight);
            std::printf("  %s\n", describeQuality(settings, propInstances, marchDust.size()).c_str());

            const rm::bench::FrameRecorder recorder =
                renderer.runOffscreenBenchmark(pixelsWide, pixelsHigh, bench.frames,
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
            renderer.setUiEffects(session.uiEffects.level);
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

            // `--screenshot W H` is the LOGICAL size and `--backing S` the display scale it
            // stands in for: the interface lays out in W×H points, fonts rasterise at S, and
            // the image is W·S × H·S pixels — the same contract a Retina window gives the
            // renderer, so a 2x capture is evidence about a 2x display.
            const rm::ui::UiViewport shotViewport = rm::ui::UiViewport::full(
                static_cast<float>(shot.width), static_cast<float>(shot.height), shot.backing,
                session.uiScale);
            const auto pixelsWide =
                static_cast<unsigned int>(std::lround(shot.width * shot.backing));
            const auto pixelsHigh =
                static_cast<unsigned int>(std::lround(shot.height * shot.backing));
            std::printf("  capture: %ux%u points at backing %.2f -> %ux%u pixels, hud scale %.3f\n",
                        shot.width, shot.height, static_cast<double>(shot.backing), pixelsWide,
                        pixelsHigh, static_cast<double>(shotViewport.hudScale()));
            composeHeadlessInterface(renderer, session, shotViewport);


            // WHAT REACHED THE GPU, against what the sim holds. The two disagreeing is the
            // signature of a dropped instance, and it used to be invisible: a batch capped at
            // its upload-time count drew one tank while the sim ran twenty, and every test
            // stayed green because nothing links the renderer. Fog legitimately hides units
            // from a seated player, so this is a report rather than an assertion.
            std::size_t liveUnits = 0;
            for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                if (units.store.slotAlive(slot)) {
                    ++liveUnits;
                }
            }
            std::printf("  units: %zu instance(s) drawn, %zu alive in the sim\n",
                        renderer.drawnUnitInstances(), liveUnits);

            const auto image = renderer.renderToImage(pixelsWide, pixelsHigh);
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

        rm::Window window{static_cast<int>(session.window.width),
                          static_cast<int>(session.window.height),
                          "recoil-metal — m8: movable units", session.window.fullscreen};
        window.setWeaponMaterials(units.weaponVisuals.materials);
        const bool inputAcceptance = !session.window.inputAcceptancePath.empty();
        if (inputAcceptance) {
            window.setSimulatedBacking(session.window.simulatedBacking);
        }
        const bool systemReducesTransparency =
            [NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceTransparency;
        const rm::ui::EffectsLevel uiEffects =
            rm::ui::resolveEffects(session.uiEffects, systemReducesTransparency);
        window.setUiEffects(uiEffects);
        std::printf("interface effects: %.*s%s\n",
                    static_cast<int>(rm::ui::effectsLevelName(uiEffects).size()),
                    rm::ui::effectsLevelName(uiEffects).data(),
                    systemReducesTransparency && !session.uiEffects.explicitOverride
                      ? " (macOS Reduced Transparency)"
                      : "");

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
        // The player's requested multiplier on automatic magnification, before the first frame
        // lays anything out — the faces are rasterised for the resulting fit-capped scale.
        window.setUserHudScale(session.uiScale);
        const rm::ui::UiViewport openingViewport = window.uiViewport();
        const rm::ui::Extent openingHud = openingViewport.hudExtent();
        std::printf("interface: %.2fx (%.0f x %.0f points of layout in a %u x %u window)\n",
                    static_cast<double>(openingViewport.hudScale()),
                    static_cast<double>(openingHud.width), static_cast<double>(openingHud.height),
                    window.width(), window.height());

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
        std::optional<rm::sim::CommandKind> armedCommand;

        // The overhead view the camera returns to when space is released. Captured at the start,
        // when `OrbitCamera::frame` has fitted the whole map.
        const float restPitch = window.camera().pitch;
        const float restYaw = window.camera().yaw;

        window.onKey([&window, &selected, &controlGroups, &units, &armedCommand, restPitch,
                      restYaw](rm::KeyEvent event) {
            if (event.phase == rm::KeyPhase::Release) {
                if (event.key == rm::Key::Space) {
                    // A held-space glance never costs the player their overhead bearings.
                    rm::OrbitCamera& camera = window.camera();
                    camera.pitch = restPitch;
                    camera.yaw = restYaw;
                }
                return;
            }

            if (event.key == rm::Key::R) {
                const bool enabled = !window.reflectionsEnabled();
                window.setReflections(enabled);
                std::printf("reflections %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (event.key == rm::Key::N) {
                const bool enabled = !window.stratumNormalsEnabled();
                window.setStratumNormals(enabled);
                std::printf("stratum normals %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (event.key == rm::Key::F) {
                const bool enabled = !window.refractionEnabled();
                window.setRefraction(enabled);
                std::printf("water refraction %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (event.key == rm::Key::O) {
                const bool visible = !window.propsVisible();
                window.setPropsVisible(visible);
                std::printf("props %s\n", visible ? "on" : "off");
                std::fflush(stdout);
            } else if (event.key == rm::Key::A && event.modifiers.shift) {
                armedCommand = rm::sim::CommandKind::AttackMove;
                std::printf("attack-move armed: right-click a destination\n");
            } else if (event.key == rm::Key::P) {
                armedCommand = rm::sim::CommandKind::Patrol;
                std::printf("patrol armed: right-click a destination\n");
            } else if (const std::optional<std::size_t> digit = rm::digitForKey(event.key)) {
                auto& group = controlGroups[*digit];
                if (event.modifiers.control) {
                    group = selected;
                    std::printf("group %zu: %zu unit(s) set\n", *digit, group.size());
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
        std::vector<rm::sim::UnitId> builderCandidates;
        rm::sim::UnitId activeBuilder{};

        // WHAT THE PLAYER PICKED OFF THE TRAY, if anything — an index into `buildOptions`.
        //
        // ARMED RATHER THAN IMMEDIATE, which is how both reference games do it and is not
        // merely convention: a structure needs a PLACE, and the panel cannot know one. So a
        // cell click arms, the next ground click places, and right-click or Escape disarms.
        // Held here beside the options it indexes, and cleared whenever they change — an index
        // into a list that has been rebuilt is a different building.
        std::optional<std::size_t> armedOption;
        rm::ui::PanelPages panelPages;
        std::size_t productionPage = 0;
        rm::ui::CommandAvailability commandAvailable{};
        std::vector<const rm::unitdef::UnitDef*> commandSelection;

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

        /// WHAT THE ROSTER'S ICONS WERE PACKED FOR: an FNV-1a over the tiles' blueprint ids,
        /// from `rosterKeyFor` below. It used to be the tile COUNT, which is not an identity:
        /// two selections of different unit types group into equal-length tile lists, and the
        /// positional slot reapplication then showed the previous selection's icon until some
        /// count happened to change. Ids are what the atlas actually drew.
        std::uint64_t rosterPackedKey = 0;
        std::size_t buildPagePacked = static_cast<std::size_t>(-1);
        std::size_t rosterPagePacked = static_cast<std::size_t>(-1);

        // The strategic layer's per-type icon table, rebuilt with every pack — the slots
        // move with the tray's and roster's counts. `typesPackedFor` starts impossible so
        // the FIRST frame packs: unlike the tray, the strategic icons exist with nothing
        // selected at all.
        std::vector<std::optional<rm::app::StrategicIconRef>> strategicRefs;
        std::size_t typesPackedFor = static_cast<std::size_t>(-1);
        rm::ui::PanelSkin interfaceSkin;

        // The caller-side tick, the same one `march()` drives. Built here rather than in
        // the frame callback because a match is decided on one tick and stays decided, and
        // the opponents remember what they have already started.
        MatchRunner runner =
            makeMatchRunner(units, map->field, passability, content, starts, map->markers,
                            rm::sim::PlayableRect{
                                .minX = {},
                                .maxX = rm::sim::fxFromFloat(map->field.widthElmos()),
                                .minZ = {},
                                .maxZ = rm::sim::fxFromFloat(map->field.depthElmos()),
                            });
        // Acceptance fixtures keep normal economy, construction, movement and roll-off ticks;
        // opponents are silent so an unrelated attack cannot destroy the controls under test.
        if (inputAcceptance) runner.scripts.clear();
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

        // A player order should show the first completed movement tick immediately instead of
        // spending another 100 ms blending toward it. Keep each unit current until the following
        // tick, where normal interpolation starts exactly at that first position and cannot snap
        // backwards. This is presentation state only; commands and snapshots remain unchanged.
        std::vector<rm::CurrentUnitProjection> responsiveDraws;
        std::vector<rm::sim::UnitId> responsiveDrawScratch;

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
            if (!armedOption || !units.store.alive(buildWho.builder)) {
                return false;
            }
            const std::string path = armedPath();
            const std::optional<rm::UnitTypeIndex> targetType =
                path.empty() ? std::nullopt : resolveBuildable(units, content, path);
            if (!targetType) {
                return false;
            }
            const auto builderType =
                static_cast<std::size_t>(units.store.typeAt(buildWho.builder.index));
            const rm::sim::PassabilityGrid& grid = passability.gridForBuild(
                units, static_cast<std::size_t>(*targetType), builderType);
            if (!units.terrain(map->field).resourceSitePlaceable(
                    units.catalog.def(*targetType)->buildRestriction,
                    rm::sim::fxFromFloat(at[0]), rm::sim::fxFromFloat(at[1]))) return false;
            return rm::sim::buildSitePlaceable(
                grid, rm::sim::fxFromFloat(at[0]), rm::sim::fxFromFloat(at[1]),
                rm::sim::fxFromFloat(armedRadius()), units.store, units.catalog,
                units.building);
        };

        /// Shift appends work and keeps placement armed for another site.
        const auto placeArmedBuild = [&](simd_float3 at, bool queued) {
            const std::string path = armedPath();
            const std::optional<rm::UnitTypeIndex> type =
                path.empty() ? std::nullopt : resolveBuildable(units, content, path);
            if (!type || !units.store.alive(buildWho.builder)) {
                armedOption.reset();
                return;
            }
            const auto snapped = snapResourceSite(units, *type, {at.x, at.z});
            at.x = snapped[0];
            at.z = snapped[1];
            // THE PLACEMENT REPORTS EITHER WAY. Until construction has a body in the world
            // (nothing exists at the site until the work completes) this line is the ONLY
            // sign a build was ordered at all — so a refusal being silent meant a player
            // could not tell "the site is bad" from "the button does nothing".
            const std::string& id = buildOptions[*armedOption].id;
            const std::string& what =
                buildOptions[*armedOption].name.empty() ? id : buildOptions[*armedOption].name;
            if (!armedPlaceable({at.x, at.z})) {
                std::printf("build refused: %s does not fit at (%.0f, %.0f)\n", what.c_str(),
                            static_cast<double>(at.x), static_cast<double>(at.z));
            } else if (issueBuild(units, buildWho.builder,
                                  playerDriving(units, units.playerArmy),
                                  static_cast<rm::TickIndex>(matchTicks), *type,
                                  rm::sim::fxFromFloat(at.x), rm::sim::fxFromFloat(at.z), queued)) {
                std::printf("build: %s %s at (%.0f, %.0f)\n", what.c_str(),
                            queued ? "queued" : "ordered",
                            static_cast<double>(at.x), static_cast<double>(at.z));
            } else {
                std::printf("build refused: %s was not accepted at (%.0f, %.0f)\n",
                            what.c_str(), static_cast<double>(at.x),
                            static_cast<double>(at.z));
            }
            std::fflush(stdout);
            if (!queued) armedOption.reset();
        };

        window.onClick([&](const rm::Ray& ray, rm::MouseButton button,
                           rm::MouseModifiers mods) {
            const rm::ui::UiViewport clickViewport = window.uiViewport();
            const std::array<float, 2> hudPoint =
                clickViewport.toHud({mods.pointX, mods.pointY});
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

                std::vector<rm::sim::UnitId> ordinary;
                std::vector<rm::sim::UnitId> overcharging;
                float shotCost = 0.0f;
                for (const rm::sim::UnitId sel : selected) {
                    if (!units.store.alive(sel)) {
                        continue;  // selected, then killed before the order was given
                    }
                    // ⌘-RIGHT-CLICK ON AN ENEMY IS AN OVERCHARGE, for the units that carry
                    // a manual weapon; the rest of the selection attacks as it would have.
                    // The escort keeps escorting while the commander spends the store.
                    const rm::unitdef::UnitDef* selDef =
                        units.catalog.def(units.store.typeAt(sel.index));
                    float unitShotCost = 0.0f;
                    if (selDef != nullptr) {
                        for (const rm::unitdef::Weapon& weapon : selDef->weapons) {
                            if (weapon.manuallyFired()) {
                                unitShotCost = rm::sim::magToFloat(weapon.energyRequired);
                                break;
                            }
                        }
                    }
                    if (target && mods.command && unitShotCost > 0.0f) {
                        overcharging.push_back(sel);
                        shotCost = unitShotCost;
                    } else {
                        ordinary.push_back(sel);
                    }
                }
                const rm::PlayerIndex player = playerDriving(units, units.playerArmy);
                const rm::TickIndex tick = static_cast<rm::TickIndex>(matchTicks);
                std::size_t submitted = 0;
                if (!overcharging.empty()
                    && issueOvercharge(units, overcharging, player, tick, *target,
                                       rm::sim::fxFromFloat(ground.x),
                                       rm::sim::fxFromFloat(ground.z), queue)) {
                    submitted += overcharging.size();
                }
                const bool ordinarySubmitted = ordinary.empty()
                    || (target
                            ? issueAttack(units, ordinary, player, tick, *target,
                                          rm::sim::fxFromFloat(ground.x),
                                          rm::sim::fxFromFloat(ground.z), queue)
                            : issueMove(units, ordinary, player, tick,
                                        rm::sim::fxFromFloat(ground.x),
                                        rm::sim::fxFromFloat(ground.z), queue, groundKind));
                if (ordinarySubmitted) {
                    submitted += ordinary.size();
                }
                if (!overcharging.empty() && units.playerArmy >= 0
                    && static_cast<std::size_t>(units.playerArmy) < units.economies.size()) {
                    const float banked = rm::sim::magToFloat(
                        units.economies[static_cast<std::size_t>(units.playerArmy)].stored.energy);
                    if (banked >= shotCost) {
                        std::printf("overcharge: submitted (%.0f energy banked)\n",
                                    static_cast<double>(banked));
                    } else {
                        std::printf("overcharge: submitted (%.0f of %.0f energy)\n",
                                    static_cast<double>(banked), static_cast<double>(shotCost));
                    }
                }
                // EVERY ORDER SAYS WHAT HAPPENED, not only the ones that went wrong. The old
                // version printed on a refusal and stayed silent on success, which is exactly
                // backwards for the question a player actually asks — "did that click do
                // anything at all?" — because silence is also what a click that never reached
                // this lambda produces. One line per order makes the two distinguishable from
                // the console alone, which is the only instrument a windowed session has.
                std::printf("order: %s%s — %zu of %zu unit(s) submitted at (%.0f, %.0f)\n",
                            rm::sim::commandKindName(
                                target ? rm::sim::CommandKind::Attack : groundKind),
                            queue ? " (queued)" : "", submitted, selected.size(),
                            static_cast<double>(ground.x), static_cast<double>(ground.z));
                std::fflush(stdout);
            };

            // A CLICK THE INTERFACE ATE SAYS SO. The deck spans most of the screen's bottom
            // edge at every profile, and a right-click landing on it is swallowed by design:
            // the world behind a panel is not what the player aimed at. What was NOT by
            // design is that it was swallowed in SILENCE — which is indistinguishable from an
            // order that was refused, an order that never arrived, and a unit that is merely
            // pivoting on the spot before setting off. One line tells the three apart.
            const auto swallowedByPanel = [&](const char* panel) {
                if (button == rm::MouseButton::Right && !selected.empty()) {
                    std::printf("order ignored: that click was on the %s panel, not the "
                                "world\n",
                                panel);
                    std::fflush(stdout);
                }
            };

            // Convert AppKit's logical click exactly once, beside the hit tests that consume it.
            const rm::ui::FrameLayout frame = rm::ui::frameLayout(clickViewport);
            const rm::ui::MinimapLayout minimap = rm::ui::minimapLayout(frame);
            if (rm::ui::insideMinimap(minimap, hudPoint[0], hudPoint[1])) {
                if (armedCommand && *armedCommand != rm::sim::CommandKind::Move
                    && *armedCommand != rm::sim::CommandKind::AttackMove
                    && *armedCommand != rm::sim::CommandKind::Patrol) {
                    rm::log::write(rm::log::Level::Info, "orders",
                                   "targeted command needs a world unit, not the minimap");
                    return;
                }
                const std::optional<std::array<float, 2>> where =
                    rm::ui::minimapToWorld(minimap, map->field.widthElmos(),
                                           map->field.depthElmos(), hudPoint[0], hudPoint[1]);
                if (!where) {
                    return;  // panel letterbox: swallowed, but not clamped onto the map edge
                }
                const simd_float3 ground = simd_make_float3(
                    (*where)[0], map->field.heightAtWorld((*where)[0], (*where)[1]), (*where)[1]);

                // THE RIGHT BUTTON MEANS THE SAME THING ON THE MAP AS IN THE WORLD: go
                // there. Ordering across the map without swinging the camera off the fight
                // is most of what a minimap order is for.
                if (button == rm::MouseButton::Right) {
                    if (!selected.empty()) {
                        orderSelectionTo(ground, mods.shift, std::nullopt,
                                         armedCommand.value_or(
                                             rm::sim::CommandKind::Move));
                        armedCommand.reset();
                    }
                    return;
                }

                // Left: jump, keeping the camera's distance and angles — a minimap click
                // moves where you are looking, not how. Height sampled from the terrain so
                // the target sits on the ground rather than at y = 0.
                window.camera().target = ground;
                return;
            }

            const auto production = gatherProduction(units, activeBuilder);
            const auto productionRect = rm::ui::productionPanelRect(frame);
            if (production && productionRect.contains(hudPoint[0], hudPoint[1])) {
                if (button == rm::MouseButton::Left) {
                    if (const auto step = rm::ui::productionPageStepAt(
                            productionRect, *production, hudPoint[0], hudPoint[1], productionPage)) {
                        if (*step < 0) --productionPage;
                        else ++productionPage;
                    } else {
                        (void)submitProductionControl(units, activeBuilder,
                            playerDriving(units, units.playerArmy),
                            static_cast<rm::TickIndex>(matchTicks), frame, hudPoint[0], hudPoint[1],
                            productionPage);
                    }
                }
                armedCommand.reset();
                armedOption.reset();
                swallowedByPanel("production");
                return;
            }

            // The command rack owns the complete bottom-right rectangle. Implemented commands
            // keep stable FA positions; disabled and unimplemented cells still swallow input so
            // a miss on the instrument never becomes an order to the world behind it.
            const rm::ui::CommandRackLayout commandRack =
                rm::ui::commandRackLayout(frame, !selected.empty());
            if (rm::ui::insideCommandRack(commandRack, hudPoint[0], hudPoint[1])) {
                const std::optional<std::size_t> slot =
                    rm::ui::commandSlotAt(commandRack, hudPoint[0], hudPoint[1]);
                if (button == rm::MouseButton::Right) {
                    armedCommand.reset();
                } else if (slot && commandAvailable[*slot]
                           && rm::ui::kCommandDescriptors[*slot].kind) {
                    const rm::sim::CommandKind kind =
                        *rm::ui::kCommandDescriptors[*slot].kind;
                    if (kind == rm::sim::CommandKind::Stop) {
                        (void)submitCommand(units, rm::sim::CommandIssue{
                            .tick = static_cast<rm::TickIndex>(matchTicks),
                            .phase = rm::sim::CommandPhase::PreTick,
                            .source = static_cast<rm::CommandSource>(
                                playerDriving(units, units.playerArmy)),
                            .player = playerDriving(units, units.playerArmy),
                            .kind = kind,
                            .units = selected,
                        });
                        armedCommand.reset();
                    } else {
                        armedCommand = kind;
                    }
                }
                swallowedByPanel("command");
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
                std::size_t& buildPage =
                    panelPages.build(units.store.typeAt(buildWho.builder.index));
                const rm::ui::BuildPanelLayout panel =
                    rm::ui::buildPanelLayout(frame, buildOptions.size(), buildPage);
                if (rm::ui::insideBuildPanel(panel, hudPoint[0], hudPoint[1])) {
                    // A cell ARMS the build; the gutters and header swallow and do nothing.
                    // Right-click anywhere on the panel disarms, so the way out is where the
                    // way in was.
                    const std::optional<std::size_t> cell = rm::ui::buildOptionAt(
                        panel, buildOptions.size(), hudPoint[0], hudPoint[1]);
                    if (button == rm::MouseButton::Right) {
                        if (!armedOption) {
                            swallowedByPanel("build");  // nothing to disarm: it was just eaten
                        }
                        armedOption.reset();
                    } else if (const std::optional<int> step = rm::ui::buildPageStepAt(
                                   panel, hudPoint[0], hudPoint[1])) {
                        if (*step < 0 && buildPage > 0) {
                            --buildPage;
                        } else if (*step > 0 && buildPage + 1 < panel.pages) {
                            ++buildPage;
                        }
                    } else if (cell
                               && rm::ui::buildOptionAction(buildOptions[*cell], buildWho.role)
                                      == rm::ui::BuildOptionAction::SubmitAtBuilder) {
                        // BUILT AT ONCE, with no site to pick, for two different reasons that
                        // reach the same place. A FACTORY is the place — arming a ghost for its
                        // products would be a step with no decision in it. An UPGRADE has no
                        // site at all: `startCommand` overrides whatever the order says with
                        // the builder's own position, because a factory does not upgrade into
                        // a field.
                        const auto& option = buildOptions[*cell];
                        const auto& what = option.name.empty() ? option.id : option.name;
                        if (submitBuildOption(units, content, buildWho.builder,
                                playerDriving(units, units.playerArmy),
                                static_cast<rm::TickIndex>(matchTicks), option, mods.shift)) {
                            std::printf("%s: %s\n", option.upgrade
                                ? (option.queuedUpgrade ? "upgrade queued" : "upgrade started")
                                : "factory queued", what.c_str());
                        } else {
                            std::printf("build refused: %s\n", what.c_str());
                        }
                        std::fflush(stdout);
                    } else if (cell) {
                        armedOption = cell;
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
                std::size_t& rosterPage = panelPages.roster();
                const rm::ui::RosterLayout roster =
                    rm::ui::rosterLayout(frame, rosterTiles.size(), rosterPage);
                if (rm::ui::insideRoster(roster, hudPoint[0], hudPoint[1])) {
                    const std::optional<std::size_t> tile =
                        rm::ui::rosterTileAt(roster, hudPoint[0], hudPoint[1]);
                    if (button == rm::MouseButton::Left) {
                        if (const std::optional<int> step = rm::ui::rosterPageStepAt(
                                roster, hudPoint[0], hudPoint[1])) {
                            if (*step < 0 && rosterPage > 0) {
                                --rosterPage;
                            } else if (*step > 0 && rosterPage + 1 < roster.pages) {
                                ++rosterPage;
                            }
                            return;
                        }
                    }
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
                    swallowedByPanel("selection");
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
                placeArmedBuild(*at, mods.shift);
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
                    const float w = clickViewport.logicalExtent.width;
                    const float h = clickViewport.logicalExtent.height;
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
                // Not an error, and worth saying anyway: "nothing selected" and "the order
                // was refused" look identical from the far side of the screen.
                std::printf("order ignored: nothing is selected\n");
                std::fflush(stdout);
                return;
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
                // The sky, or past the edge of the map. Silence here reads as a broken
                // button, because the click did land somewhere as far as the player is
                // concerned — the horizon looks like ground until you are told it is not.
                std::printf("order ignored: that ray missed the map\n");
                std::fflush(stdout);
                return;
            }

            // Commands armed from the rack make the next right-click explicit. They reuse the
            // same submission helpers as contextual right-clicks; the rack changes intent, not
            // the simulation path.
            if (armedCommand == rm::sim::CommandKind::Attack
                || armedCommand == rm::sim::CommandKind::Overcharge) {
                if (!isAttack || !hit) {
                    rm::log::write(rm::log::Level::Info, "orders",
                                   "attack command needs a hostile unit target");
                    return;
                }
                const rm::sim::Transform& at = units.store.transforms()[hit->index];
                const rm::PlayerIndex player = playerDriving(units, units.playerArmy);
                const rm::TickIndex tick = static_cast<rm::TickIndex>(matchTicks);
                if (*armedCommand == rm::sim::CommandKind::Overcharge) {
                    std::vector<rm::sim::UnitId> manual;
                    for (const rm::sim::UnitId id : selected) {
                        if (!units.store.alive(id)) {
                            continue;
                        }
                        const rm::unitdef::UnitDef* def =
                            units.catalog.def(units.store.typeAt(id.index));
                        if (def != nullptr
                            && std::ranges::any_of(
                                def->weapons, [](const rm::unitdef::Weapon& weapon) {
                                    return weapon.manuallyFired()
                                        && weapon.energyRequired > rm::sim::Mag{};
                                })) {
                            manual.push_back(id);
                        }
                    }
                    (void)issueOvercharge(units, manual, player, tick, *hit, at.x, at.z,
                                          mods.shift);
                } else {
                    (void)issueAttack(units, selected, player, tick, *hit, at.x, at.z,
                                      mods.shift);
                }
                armedCommand.reset();
                return;
            }

            // A RIGHT-CLICK ON A DAMAGED ALLY IS REPAIR. Builders receive the targeted repair
            // order; the rest of a mixed selection moves there. This precedes Assist because a
            // damaged builder is still a repair target, not an instruction to guard it.
            if (armedCommand == rm::sim::CommandKind::Guard) {
                if (!hit || !units.store.alive(*hit)
                    || !alliedTo(units, units.playerArmy, *hit)) {
                    rm::log::write(rm::log::Level::Info, "orders", "guard needs a living allied unit");
                    return;
                }
                std::vector<rm::sim::UnitId> guards;
                for (const auto id : selected) {
                    if (!units.store.alive(id) || id == *hit) continue;
                    const rm::unitdef::UnitDef* def = units.catalog.def(units.store.typeAt(id.index));
                    const std::array<const rm::unitdef::UnitDef*, 1> one{def};
                    if (rm::ui::commandAvailability(one)[5]) guards.push_back(id);
                }
                if (!guards.empty()) {
                    (void)issueGuard(units, guards, playerDriving(units, units.playerArmy),
                                     static_cast<rm::TickIndex>(matchTicks), *hit, mods.shift);
                    armedCommand.reset();
                }
                return;
            }
            const bool explicitRepair = armedCommand == rm::sim::CommandKind::Repair;
            if (!isAttack && (!armedCommand || explicitRepair) && hit
                && units.playerArmy != rm::sim::kNoArmy
                && alliedTo(units, units.playerArmy, *hit)
                && (explicitRepair || units.store.health()[hit->index].current
                                          < units.store.health()[hit->index].maximum)) {
                std::vector<rm::sim::UnitId> builders;
                std::vector<rm::sim::UnitId> movers;
                for (const rm::sim::UnitId sel : selected) {
                    if (!units.store.alive(sel) || sel == *hit) {
                        continue;
                    }
                    const rm::unitdef::UnitDef* def =
                        units.catalog.def(units.store.typeAt(sel.index));
                    (def != nullptr && def->isBuilder() ? builders : movers).push_back(sel);
                }
                const rm::PlayerIndex player = playerDriving(units, units.playerArmy);
                const rm::TickIndex tick = static_cast<rm::TickIndex>(matchTicks);
                const rm::sim::Transform& at = units.store.transforms()[hit->index];
                const bool repairing = builders.empty()
                    || issueRepair(units, builders, player, tick, *hit, mods.shift);
                (void)(movers.empty()
                           || issueMove(units, movers, player, tick, at.x, at.z, mods.shift));
                if (repairing && !builders.empty()) {
                    std::printf("repair: %zu builder(s) submitted\n", builders.size());
                }
                if (explicitRepair) {
                    armedCommand.reset();
                }
                return;
            }

            // A RIGHT-CLICK ON YOUR OWN BUILDER IS AN ASSIST — the guard order. Field builders
            // lend rate; immobile factories mirror compatible queued production; everyone else
            // just walks over. Beaten by an enemy under the click (that is an attack) and
            // beating a wreck and plain ground.
            const bool explicitAssist = armedCommand == rm::sim::CommandKind::Assist;
            if (!isAttack && (!armedCommand || explicitAssist) && hit
                && units.playerArmy != rm::sim::kNoArmy
                && units.armyOf(hit->index) == units.playerArmy) {
                const rm::unitdef::UnitDef* targetDef =
                    units.catalog.def(units.store.typeAt(hit->index));
                if (targetDef != nullptr && targetDef->isBuilder()) {
                    std::vector<rm::sim::UnitId> builders;
                    std::vector<rm::sim::UnitId> movers;
                    for (const rm::sim::UnitId sel : selected) {
                        if (!units.store.alive(sel) || sel == *hit) {
                            continue;  // a unit cannot assist itself
                        }
                        const rm::unitdef::UnitDef* def =
                            units.catalog.def(units.store.typeAt(sel.index));
                        (def != nullptr && def->isBuilder() ? builders : movers).push_back(sel);
                    }
                    const rm::PlayerIndex player = playerDriving(units, units.playerArmy);
                    const rm::TickIndex tick = static_cast<rm::TickIndex>(matchTicks);
                    const rm::sim::Transform& at = units.store.transforms()[hit->index];
                    const bool assisting = builders.empty()
                        || issueAssist(units, builders, player, tick, *hit, mods.shift);
                    (void)(movers.empty()
                               || issueMove(units, movers, player, tick, at.x, at.z, mods.shift));
                    if (assisting && !builders.empty()) {
                        std::printf("assist: %zu builder(s) submitted for %s\n", builders.size(),
                                    targetDef->name.c_str());
                    }
                    if (explicitAssist) {
                        armedCommand.reset();
                    }
                    return;
                }
            }

            // A WRECK UNDER THE CLICK MAKES IT A RECLAIM — for the builders in the
            // selection; everyone else walks there. An enemy unit beats a wreck (the pick
            // above already decided that), and a wreck beats plain ground. The hit disc is
            // the mark the player can actually SEE — the decal's radius, not the sim's —
            // with a floor so a tiny unit's wreck is still clickable.
            const bool explicitReclaim = armedCommand == rm::sim::CommandKind::Reclaim;
            if (!isAttack && (!armedCommand || explicitReclaim)) {
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
                    std::vector<rm::sim::UnitId> builders;
                    std::vector<rm::sim::UnitId> movers;
                    for (const rm::sim::UnitId sel : selected) {
                        if (!units.store.alive(sel)) {
                            continue;
                        }
                        const rm::unitdef::UnitDef* def =
                            units.catalog.def(units.store.typeAt(sel.index));
                        (def != nullptr && def->isBuilder() ? builders : movers).push_back(sel);
                    }
                    const rm::PlayerIndex player = playerDriving(units, units.playerArmy);
                    const rm::TickIndex tick = static_cast<rm::TickIndex>(matchTicks);
                    const bool reclaiming = builders.empty()
                        || issueReclaim(units, builders, player, tick, *wreck, mods.shift);
                    (void)(movers.empty()
                               || issueMove(units, movers, player, tick, found->at[0], found->at[2],
                                            mods.shift));
                    if (reclaiming && !builders.empty()) {
                        std::printf("reclaim: %zu builder(s) submitted (%.0f mass)\n",
                                    builders.size(),
                                    static_cast<double>(
                                        rm::sim::magToFloat(found->massRemaining)));
                    }
                    if (explicitReclaim) {
                        armedCommand.reset();
                    }
                    return;
                }
            }

            if (armedCommand == rm::sim::CommandKind::Repair
                || armedCommand == rm::sim::CommandKind::Guard
                || armedCommand == rm::sim::CommandKind::Assist
                || armedCommand == rm::sim::CommandKind::Reclaim) {
                rm::log::write(rm::log::Level::Info, "orders",
                               "the armed command cannot use that target");
                return;
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
                             isAttack && !armedCommand
                                 ? hit
                                 : std::optional<rm::sim::UnitId>{},
                             armedCommand.value_or(rm::sim::CommandKind::Move));
            armedCommand.reset();
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

        // HOW MANY BATCHES THE RENDERER HAS BEEN GIVEN — held ACROSS frames, which is the
        // whole point. This was a local captured at the top of each frame and compared at the
        // bottom, so a batch created LATER in the frame than that comparison — which is
        // exactly when the build ghost creates one, since it resolves its model at draw
        // time — was never uploaded at all: the next frame's "before" already counted it, so
        // the growth it represented had silently happened between two equal numbers. The
        // silhouette therefore drew nothing for any blueprint the player had not already
        // built, which is most of the tray.
        // This frame's construction sites, rebuilt each frame like every other draw list.
        std::vector<rm::Renderer::ConstructionDraw> constructionDraws;

        std::size_t uploadedBatches = 0;
        const auto uploadNewBatches = [&] {
            if (units.batches.size() == uploadedBatches) {
                return;
            }
            for (std::size_t b = uploadedBatches; b < units.batches.size(); ++b) {
                units.batches[b].animationDrivenByInstance = true;
            }
            window.setUnits(units.textures.all(), units.batches);
            uploadedBatches = units.batches.size();
        };

        std::size_t acceptanceFrames = 0;
        bool acceptanceFastForward = false;
        int acceptanceResult = inputAcceptance ? 1 : 0;
        window.onFrame([&](float elapsed) {
            if (inputAcceptance) {
                // Only wall-clock pacing changes; every simulated beat still uses advanceMatch.
                elapsed = gAppTickRate.secondsPerTick() * (acceptanceFastForward ? 20.0f : 1.0f);
            }
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
                if (window.keyHeld(rm::Key::A)) { right -= step; }
                if (window.keyHeld(rm::Key::D)) { right += step; }
                if (window.keyHeld(rm::Key::W)) { forward += step; }
                if (window.keyHeld(rm::Key::S)) { forward -= step; }
                if (right != 0.0f || forward != 0.0f) {
                    window.camera().pan(right, forward);
                }
            }

            matchSeconds += elapsed;
            const int pacedTicks = clock.advance(elapsed);
            const int ticks = inputAcceptance ? (acceptanceFastForward ? 20 : 1) : pacedTicks;

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
                followUpgradeSelection(units, selected);
                ++matchTicks;

                // The tick's combat, as particles — read HERE because an event is a
                // per-tick notification (Events.hpp): the next advanceMatch clears the
                // queue, so this tick's shots are visible now or never.
                gatherVisibleEvents(visibleEvents, units);
                units.updateCombatAttachments();
                rm::emitCombatEffects(particles, visibleEvents, &units.weaponVisuals,
                    &units.combatEffectState, gAppTickRate.secondsPerTick());

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
                rm::emitProjectileTrails(particles, visibleProjectiles, &units.weaponVisuals,
                    gAppTickRate.secondsPerTick());

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
            std::erase_if(responsiveDraws, [&](const rm::CurrentUnitProjection& draw) {
                return draw.throughTick < units.snapshotCurrent.tick
                       || !units.store.alive(draw.id);
            });
            responsiveDrawScratch.clear();
            for (const rm::CurrentUnitProjection& draw : responsiveDraws) {
                if (draw.activeAt(units.snapshotCurrent.tick)) {
                    responsiveDrawScratch.push_back(draw.id);
                }
            }
            units.gatherForDrawing(clock.alpha(), &lodEye, responsiveDrawScratch, elapsed);

            // WHAT IS BEING BUILT, as something to look at. Before the upload below, because
            // this is where a blueprint's model comes into existence: a construction is the
            // first moment a type needs DRAWING rather than merely simulating, and the batch
            // it creates has to reach the GPU in the same frame — which is the bug the ghost
            // spent its whole life on.
            rm::app::gatherConstructions(units, content, map->field, constructionDraws);
            window.setConstructions(constructionDraws);
            window.setConstructionTime(matchSeconds);

            // Re-upload when the match built something new. Only on growth, which is a
            // handful of times in a whole match — this walks every model and texture, so
            // doing it per frame would cost what it costs to load the scene.
            //
            // AFTER the gather, and that ordering is load-bearing: `setUnits` sizes each
            // batch's instance buffer from what its span holds, and a batch created this
            // tick holds nothing until the gather fills it. Uploading first gives the new
            // model a capacity of one — which `setInstances` now grows rather than clips,
            // so the ordering is a nicety here and a correctness rule for the poses.
            uploadNewBatches();

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
                                      rm::kIconReferenceHeightPoints), &units.weaponVisuals);
            window.setParticles(iconScratch);

            hudScratch.clear();
            // EVERY VERTEX BELOW IS IN THE HUD'S DESIGN SPACE, chrome and world-projected
            // overlays alike. They share one vertex stream and therefore one viewport, so
            // mixing the window's real size in here would place the health bars and strategic
            // icons a scale factor away from the units they belong to.
            const rm::ui::UiViewport viewport = window.uiViewport();
            const rm::ui::Extent hudExtent = viewport.hudExtent();
            const rm::ui::FrameLayout frame = rm::ui::frameLayout(viewport);
            const std::array<float, 2> logicalCursor = window.cursor();
            const std::array<float, 2> hudCursor = viewport.toHud(logicalCursor);
            const rm::ui::Theme baseTheme = hudThemeFor(units, session.uiProfile);

            // Health over the units that need it: damaged, and close enough to be units
            // rather than icons. Absence is what "fine" looks like (Interface.hpp).
            appendHealthBars(hudScratch, units, window.camera(), window.labelFont(), viewport);
            appendConstructionBars(hudScratch, units, window.camera(), map->field,
                window.labelFont(), viewport, selected, hudCursor);

            // The strategic layer: the game's own glyphs where units are too small to read,
            // in the army's colour, under all the chrome (Geometry::worldOverlay).
            appendStrategicIcons(hudScratch, units, window.camera(), viewport, strategicRefs);
            appendContactBlips(hudScratch, units, window.camera(), map->field,
                               window.labelFont(), viewport);

            // THE MINIMAP (§7 P7.4), appended to the same geometry the HUD builds — it is
            // rectangles in screen space, which is what `text::appendRect` already draws, so it
            // needs no pipeline of its own. That is the other half of "nearly free".
            appendMinimapPips(minimapPips, units);
            appendViewFootprint(minimapView, window.camera(), map->field, viewport);
            const rm::ui::MinimapLayout minimap = rm::ui::minimapLayout(frame);
            const rm::ui::MinimapProjection minimapProjection =
                rm::ui::minimapProjection(minimap, map->field.widthElmos(),
                                           map->field.depthElmos());
            // The preview under the panel, inset by the border so the chrome frames it. The
            // panel then draws everything BUT its own fill, so the picture shows through.
            const bool hasPreview = map->preview.width > 0;
            if (hasPreview) {
                const rm::ui::Rect& contentRect = minimapProjection.content;
                window.setMinimapRect(contentRect.x, contentRect.y, contentRect.width,
                                      contentRect.height);
            }
            // What the selection can build, above the minimap — the bottom-left control block
            // Beyond All Reason arranges the same way. Absent entirely when nothing selected
            // builds, rather than an empty frame asking to be explained.
            rm::app::gatherBuilderCandidates(units, selected, builderCandidates);
            const auto previousBuilder = activeBuilder;
            activeBuilder = rm::app::activeBuilderFor(builderCandidates, activeBuilder);
            if (activeBuilder != previousBuilder) productionPage = 0;
            rm::app::gatherBuildOptions(units, activeBuilder, baseTheme, buildOptions, buildWho);
            // AN INDEX INTO A LIST THAT HAS BEEN REBUILT IS A DIFFERENT BUILDING. Deselecting,
            // or selecting a different builder, must not leave cell 4 armed and meaning
            // something else — so the arming is dropped whenever the list it points into can no
            // longer be trusted to be the same list.
            if (armedOption && *armedOption >= buildOptions.size()) {
                armedOption.reset();
            }
            if (buildWho.builder != iconsPackedFor) {
                armedOption.reset();
            }

            rm::app::gatherRoster(units, selected, rosterTiles);
            commandSelection.clear();
            for (const rm::sim::UnitId id : selected) {
                if (units.store.alive(id)) {
                    commandSelection.push_back(
                        units.catalog.def(units.store.typeAt(id.index)));
                }
            }
            commandAvailable = rm::ui::commandAvailability(commandSelection);
            // Advance page ownership with the tiles, not with input. A control-group key can
            // change `selected` between display callbacks; until this rebuild, clicks must keep
            // addressing the roster that is still visible rather than page zero of a future one.
            panelPages.showRoster(selected);
            rm::ui::BuildPanelLayout buildPanel;
            if (!buildOptions.empty()) {
                std::size_t& buildPage =
                    panelPages.build(units.store.typeAt(buildWho.builder.index));
                buildPanel = rm::ui::buildPanelLayout(frame, buildOptions.size(), buildPage);
                buildPage = buildPanel.page;
            }
            std::size_t& visibleRosterPage = panelPages.roster();
            const rm::ui::RosterLayout roster =
                rm::ui::rosterLayout(frame, rosterTiles.size(), visibleRosterPage);
            visibleRosterPage = roster.page;

            // The icons for BOTH panels, in one atlas: packed when either set changes, and
            // reapplied from the cache otherwise. Reapplied rather than repacked because the
            // option list and the tile list are rebuilt every frame and a fresh entry has no
            // slot — repacking to recover them would be two dozen archive reads a frame for
            // pictures that have not moved.
            if (buildWho.builder != iconsPackedFor || rosterPackedKey != rosterKeyFor(rosterTiles)
                || units.catalog.size() != typesPackedFor || buildPagePacked != buildPanel.page
                || rosterPagePacked != roster.page) {
                iconsPackedFor = buildWho.builder;
                rosterPackedKey = rosterKeyFor(rosterTiles);
                typesPackedFor = units.catalog.size();
                buildPagePacked = buildPanel.page;
                rosterPagePacked = roster.page;
                // Any glyph a newly registered type names is fetched before the pack, so a
                // unit type first seen this frame gets its icon in this atlas rather than
                // a square until the next selection change.
                rm::app::ensureStrategicIconArt(units, content);
                std::size_t strategicBase = 0;
                rm::app::PackedInterfaceAtlas packed = rm::app::packInterfaceIcons(
                    content, buildOptions, rosterTiles,
                    {.first = buildPanel.first, .count = buildPanel.shown},
                    {.first = roster.first, .count = roster.shown}, session.uiProfile,
                    units.strategicIconArt, &strategicBase);
                window.setIconAtlas(packed.texture);
                interfaceSkin = packed.skin;
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
            const rm::ui::Theme theme =
                hudThemeFor(units, session.uiProfile, interfaceSkin);
            rm::ui::build(hudScratch, window.labelFont(), window.readoutFont(), theme,
                          hudStateFrom(units, matchSeconds, session.uiProfile), frame);
            rm::ui::appendMinimap(hudScratch, window.labelFont(), theme, minimap,
                                  map->field.widthElmos(), map->field.depthElmos(), minimapPips,
                                  minimapView, !hasPreview);
            std::optional<std::size_t> overBuild;
            if (!buildOptions.empty()) {
                // The lit cell under the cursor, which is most of what makes a grid of squares
                // read as BUTTONS rather than as a readout. Polled once here rather than
                // tracked through a mouseMoved handler — see `Window::cursor`.
                overBuild = rm::ui::buildOptionAt(buildPanel, buildOptions.size(), hudCursor[0],
                                                  hudCursor[1]);
                // THE ARMED CELL STAYS LIT while the cursor is out over the map, which is
                // exactly when the player needs to be told what they are about to place. A
                // hover wins over it, so moving back onto the tray reads normally.
                std::optional<std::size_t> lit = overBuild;
                if (!lit && armedOption) {
                    lit = armedOption;
                }

                rm::ui::appendBuildPanel(hudScratch, window.labelFont(), window.readoutFont(),
                                         theme, buildPanel, buildOptions, lit,
                                         buildWho.name, buildWho.role);
            }

            const rm::ui::CommandRackLayout commandRack =
                rm::ui::commandRackLayout(frame, !selected.empty());
            const std::optional<std::size_t> overCommand =
                rm::ui::commandSlotAt(commandRack, hudCursor[0], hudCursor[1]);
            rm::ui::appendCommandRack(hudScratch, window.labelFont(), window.readoutFont(),
                                      theme, commandRack, commandAvailable, overCommand,
                                      armedCommand);

            const auto production = gatherProduction(units, activeBuilder);
            const auto productionRect = rm::ui::productionPanelRect(frame);
            if (production) {
                productionPage = rm::ui::productionPage(
                    productionRect, production->queue.size(), productionPage).page;
                rm::ui::appendProductionPanel(hudScratch, window.labelFont(), window.readoutFont(),
                    theme, productionRect, *production, productionPage);
            }

            // The roster, bottom centre. After the tray so both are in one buffer; they do not
            // overlap, so the order between them is arbitrary and stated only to be stable.
            if (!rosterTiles.empty()) {
                const std::optional<std::size_t> overTile =
                    rm::ui::rosterTileAt(roster, hudCursor[0], hudCursor[1]);

                rm::ui::InfoCard inspector;
                if (armedOption && *armedOption < buildOptions.size()) {
                    inspector =
                        rm::ui::buildOptionCard(buildOptions[*armedOption], session.uiProfile);
                } else if (armedCommand) {
                    const auto found = std::ranges::find_if(
                        rm::ui::kCommandDescriptors,
                        [&](const rm::ui::CommandDescriptor& descriptor) {
                            return descriptor.kind == armedCommand;
                        });
                    if (found != rm::ui::kCommandDescriptors.end()) {
                        inspector = rm::ui::commandCard(*found, commandSelection, true);
                    }
                } else if (overBuild && *overBuild < buildOptions.size()) {
                    inspector =
                        rm::ui::buildOptionCard(buildOptions[*overBuild], session.uiProfile);
                } else if (overCommand && *overCommand < rm::ui::kCommandSlots) {
                    inspector = rm::ui::commandCard(
                        rm::ui::kCommandDescriptors[*overCommand],
                        commandSelection);
                } else if (overTile && *overTile < rosterTiles.size()) {
                    inspector = selectedUnitCard(units, rosterTiles[*overTile], activeBuilder);
                } else {
                    inspector = selectedUnitCard(units, rosterTiles.front(), activeBuilder);
                }
                rm::ui::appendRoster(hudScratch, window.labelFont(), window.readoutFont(),
                                     theme, roster, rosterTiles, overTile,
                                     &inspector);
            }

            // --- The band box, and the minimap's drag-to-pan --------------------------
            // Both are DERIVED FROM POLLED STATE — is the left button down, where did the
            // press begin, where is the cursor now — rather than from drag events, because
            // the interface is rebuilt per frame and a drag is a per-frame fact. The press
            // origin decides which gesture this is: on the minimap it pans the view, on the
            // world it draws a band, on a panel (or while a build is armed) it is neither.
            {
                // ALL OF THIS IS ONE SPACE, the HUD's. The box is drawn into the HUD's vertex
                // stream, the panel tests read the HUD's rectangles, and the units are caught
                // by projecting them into the same space — three things that must agree, and
                // would not if the projection used the window's real size while the box used
                // the design space the renderer magnifies.
                const float w = hudExtent.width;
                const float h = hudExtent.height;
                const bool held = window.leftMouseHeld();
                const std::array<float, 2> logicalOrigin = window.dragOrigin();
                const std::array<float, 2> origin = viewport.toHud(logicalOrigin);
                const std::array<float, 2> at = hudCursor;

                // Strictly above the click slop (3 AppKit points) so a release can
                // never be both a click and a band: between the two thresholds is a small
                // dead zone, which is the safe side of the ambiguity. Measure before HUD
                // conversion so --ui-scale cannot change the logical-point drag threshold.
                constexpr float kBandSlopPoints = 8.0f;
                const bool traveled =
                    std::abs(logicalCursor[0] - logicalOrigin[0])
                      + std::abs(logicalCursor[1] - logicalOrigin[1])
                    > kBandSlopPoints;

                const bool onMinimap = rm::ui::insideMinimap(minimap, origin[0], origin[1]);
                const std::size_t buildPage =
                    buildOptions.empty()
                        ? 0
                        : panelPages.build(units.store.typeAt(buildWho.builder.index));
                const std::size_t rosterPage = panelPages.roster();
                const bool onPanel =
                    (production && productionRect.contains(origin[0], origin[1]))
                    || (!buildOptions.empty()
                      && rm::ui::insideBuildPanel(
                          rm::ui::buildPanelLayout(frame, buildOptions.size(), buildPage), origin[0],
                          origin[1]))
                    || (!rosterTiles.empty()
                        && rm::ui::insideRoster(
                            rm::ui::rosterLayout(frame, rosterTiles.size(), rosterPage), origin[0],
                            origin[1]))
                    || rm::ui::insideCommandRack(
                        rm::ui::commandRackLayout(frame, !selected.empty()), origin[0], origin[1]);

                if (held && onMinimap) {
                    // Drag-to-pan: the ground under the finger, continuously. The same
                    // projection the click-jump uses, at frame rate. Letterbox space is not a
                    // map coordinate, so dragging through it leaves the camera where it was.
                    const std::optional<std::array<float, 2>> where = rm::ui::minimapToWorld(
                        minimap, map->field.widthElmos(), map->field.depthElmos(), at[0],
                        at[1]);
                    if (where) {
                        window.camera().target = simd_make_float3(
                            (*where)[0], map->field.heightAtWorld((*where)[0], (*where)[1]),
                            (*where)[1]);
                    }
                }

                const bool worldBand = !onMinimap && !onPanel && !armedOption && !armedCommand
                                    && traveled;
                if (held && worldBand) {
                    // The box: a whisper of fill so the caught area reads, and a hairline
                    // in the lit edge so the bounds are exact. Interface, not effect — the
                    // same vocabulary as every panel border.
                    const float left = std::min(origin[0], at[0]);
                    const float top = std::min(origin[1], at[1]);
                    const float wide = std::abs(at[0] - origin[0]);
                    const float tall = std::abs(at[1] - origin[1]);
                    rm::text::appendRect(hudScratch.worldOverlay.solid, window.labelFont(), left,
                                         top,
                                         wide, tall, rm::ui::fade(theme.edgeLit, 0.10f));
                    rm::text::appendRect(hudScratch.worldOverlay.solid, window.labelFont(), left,
                                         top,
                                         wide, 1.0f, theme.edgeLit);
                    rm::text::appendRect(hudScratch.worldOverlay.solid, window.labelFont(), left,
                                         top + tall - 1.0f, wide, 1.0f, theme.edgeLit);
                    rm::text::appendRect(hudScratch.worldOverlay.solid, window.labelFont(), left,
                                         top,
                                         1.0f, tall, theme.edgeLit);
                    rm::text::appendRect(hudScratch.worldOverlay.solid, window.labelFont(),
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

            window.setHud(hudScratch);

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
            appendResourceDeposits(decalVertices, units, map->field);
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
                appendUnitSelection(
                    decalVertices, map->field, ground,
                    rm::sim::fxToFloat(units.store.motion()[sel.index].radiusElmos)
                        * kSelectionRingMargin,
                    session.uiProfile);

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
                    const std::deque<rm::sim::QueuedCommand>& queue =
                        units.store.orders()[sel.index].entries();
                    std::array<float, 2> from{ground[0], ground[2]};
                    for (const rm::sim::QueuedCommand& order : queue) {
                        if (order.kind() == rm::sim::CommandKind::Stop) {
                            continue;  // a stop has no destination to draw a line to
                        }
                        const std::array<float, 2> to{rm::sim::fxToFloat(order.targetX()),
                                                      rm::sim::fxToFloat(order.targetZ())};
                        appendGroundSegment(decalVertices, map->field, from, to,
                                            kQueueLineColour, kQueueLineWidthElmos);
                        appendGroundNode(decalVertices, map->field, to,
                                         order.kind() == rm::sim::CommandKind::Build
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
                const std::array<float, 2> cursor = window.cursor();
                const rm::Ray under = rm::screenRay(
                    window.camera(), cursor[0], viewport.logicalExtent.height - cursor[1],
                    viewport.logicalExtent.width, viewport.logicalExtent.height);
                std::optional<simd_float3> at = rm::pickGround(under, map->field);
                if (at) {
                    if (const auto type = resolveBuildable(units, content, armedPath())) {
                        const auto snapped = snapResourceSite(units, *type, {at->x, at->z});
                        at->x = snapped[0];
                        at->z = snapped[1];
                        at->y = map->field.heightAtWorld(at->x, at->z);
                    }
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
                                        rm::sim::fxFromFloat(at->x)
                                            + mine.skirtCentreOffsetXElmos,
                                        rm::sim::fxFromFloat(at->z)
                                            + mine.skirtCentreOffsetZElmos,
                                        mine.skirtHalfXElmos, mine.skirtHalfZElmos,
                                        t.x + theirs.skirtCentreOffsetXElmos,
                                        t.z + theirs.skirtCentreOffsetZElmos,
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
                    // call loads the model and grows `units.batches`. THE UPLOAD HAPPENS
                    // HERE, immediately after, rather than being left to a growth check that
                    // has already run this frame and will see no growth on the next one —
                    // which is why the silhouette used to draw nothing for any blueprint the
                    // player had not already built.
                    const std::string path = armedPath();
                    const std::optional<rm::UnitTypeIndex> ghostType =
                        path.empty() ? std::nullopt
                                     : ensureDrawableType(units, content, path);
                    uploadNewBatches();
                    const std::size_t ghostBatch =
                        ghostType ? units.batchOf(*ghostType) : UnitScene::kNoBatch;
                    if (ghostBatch != UnitScene::kNoBatch) {
                        const auto typeIndex = static_cast<std::size_t>(*ghostType);
                        window.setGhost(
                            ghostBatch,
                            rm::UnitInstance{
                                .position = {{at->x, at->y, at->z}},
                                // The promise faces where the spawn will: the silhouette
                                // asks the same authority the completion path does.
                                .rotationY = rm::sim::radiansFromBrad(rm::app::structureFacing(
                                    map->field, rm::sim::fxFromFloat(at->x),
                                    rm::sim::fxFromFloat(at->z))),
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

            // The pads under the sites and the streams feeding them. Into the SAME two lists
            // the rest of the frame's effects use — a build stream is a line of motes and a pad
            // is a ring, and both already have a pass.
            //
            // Before the shields for the reason the shields are last: if the fixed decal
            // buffer fills, a pad marking work in progress is worth more than a distant dome.
            rm::app::appendConstructionEffects(decalVertices, iconScratch, units, map->field,
                                               matchSeconds);
            window.setParticles(iconScratch);

            // Preserve all gameplay UI when the renderer's fixed decal buffer fills. Domes are
            // deliberately last: losing distant shield shells is preferable to losing a build
            // ghost, selection ring, route, or refused-order marker.
            decalVertices.insert(decalVertices.end(), units.shieldScratch.begin(),
                                 units.shieldScratch.end());

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
            ++acceptanceFrames;
        });

        window.show(inputAcceptance);

        // This driver lives at the application boundary because the assertion is specifically
        // that AppKit selection and the DRAWN widgets reach the match. Calling issueBuild here
        // would bypass exactly the integration this acceptance run must exercise.
        int inputStage = -1;
        int stageStarted = matchTicks;
        std::size_t observedFrame = 0;
        std::size_t productIndex = 0;
        std::vector<std::string> inputProducts;
        rm::sim::UnitId inputFactory{}, inputProduct{}, inputAttackTarget{};
        std::string inputGenerator;
        rm::sim::Transform moveStarted{};
        std::vector<rm::sim::UnitId> beforeProduct;
        std::vector<rm::sim::UnitId> beforeGenerator;
        std::size_t pageClicks = 0;
        bool engineerPageProbe = false;
        bool productionProbeDone = false;
        std::size_t queuedProbeEntries = 0;
        std::vector<rm::CommandId> productionProbeIds;
        struct ExpectedInputCommand {
            rm::sim::CommandKind kind;
            rm::sim::UnitId unit;
            bool queued;
            std::size_t logStart;
        };
        std::optional<ExpectedInputCommand> expectedInputCommand;
        const auto expectInputCommand = [&](rm::sim::CommandKind kind, rm::sim::UnitId unit,
                                             bool queued = false) {
            expectedInputCommand = ExpectedInputCommand{kind, unit, queued, units.commands.size()};
        };
        const auto uppercase = [](std::string value) {
            for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return value;
        };
        const auto inputCheck = [](bool condition, const std::string& problem) {
            if (!condition) throw std::runtime_error{problem};
        };
        const auto inputWorldClick = [&](rm::sim::Fx x, rm::sim::Fx z,
                                         rm::MouseButton button, bool shift = false,
                                         std::optional<float> height = {}) {
            const float wx = rm::sim::fxToFloat(x), wz = rm::sim::fxToFloat(z);
            const auto point = rm::worldToScreen(window.camera(),
                simd_make_float3(wx, height.value_or(map->field.heightAtWorld(wx, wz)), wz),
                static_cast<float>(window.width()), static_cast<float>(window.height()));
            inputCheck(point.has_value(), "world target is behind the camera");
            const auto viewport = window.uiViewport();
            const auto hud = viewport.toHud(*point);
            const auto frame = rm::ui::frameLayout(viewport);
            inputCheck((*point)[0] > 0 && (*point)[0] < window.width()
                && (*point)[1] > 0 && (*point)[1] < window.height()
                && !frame.commands.contains(hud[0], hud[1])
                && !frame.build.contains(hud[0], hud[1]), "world target overlaps HUD or window edge");
            window.sendMouseClick((*point)[0], (*point)[1], button, shift);
        };
        std::optional<rm::sim::UnitId> pendingInputSelection;
        const auto inputSelect = [&](rm::sim::UnitId id) {
            inputCheck(units.store.alive(id), "selection target died");
            const auto& at = units.store.transforms()[id.index];
            if (pendingInputSelection != id) {
                window.focusOn({rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                rm::sim::fxToFloat(at.z)}, 420.0f);
                pendingInputSelection = id;
                return false; // Present the new camera and spawned unit before clicking it.
            }
            inputWorldClick(at.x, at.z, rm::MouseButton::Left, false, rm::sim::fxToFloat(at.y));
            std::string picked = "none";
            if (!selected.empty() && units.store.alive(selected.front())) {
                const auto* definition = units.catalog.def(units.store.typeAt(selected.front().index));
                picked = definition != nullptr ? definition->name : "unknown type";
                picked += " slot " + std::to_string(selected.front().index);
            }
            inputCheck(selected.size() == 1 && selected.front() == id,
                       "native world click did not select slot " + std::to_string(id.index)
                           + "; picked " + picked);
            pendingInputSelection.reset();
            return true;
        };
        const auto inputHudClick = [&](float x, float y, rm::MouseButton button = rm::MouseButton::Left) {
            const float scale = window.uiViewport().hudScale();
            window.sendMouseClick(x * scale, y * scale, button);
        };
        const auto inputCommandClick = [&](std::size_t slot, rm::MouseButton button = rm::MouseButton::Left) {
            const auto rack = rm::ui::commandRackLayout(rm::ui::frameLayout(window.uiViewport()), true);
            const auto at = rm::ui::commandCellOrigin(rack, slot);
            inputHudClick(at[0] + rack.cellWidth / 2, at[1] + rack.cellHeight / 2, button);
        };
        const auto inputBuildClick = [&](const std::string& name) {
            const auto wanted = std::find_if(buildOptions.begin(), buildOptions.end(),
                [&](const auto& option) { return uppercase(option.id) == name; });
            inputCheck(wanted != buildOptions.end(), "missing build tray option " + name);
            const auto index = static_cast<std::size_t>(wanted - buildOptions.begin());
            const auto layout = rm::ui::buildPanelLayout(rm::ui::frameLayout(window.uiViewport()),
                buildOptions.size(), panelPages.build(units.store.typeAt(buildWho.builder.index)));
            if (index < layout.first || index >= layout.first + layout.shown) {
                const float right = layout.x + layout.width - rm::ui::kBuildPadding;
                inputHudClick(right - (index < layout.first ? 33.0f : 11.0f),
                              layout.y + rm::ui::kBuildHeader / 2);
                ++pageClicks;
                return false; // Wait until the changed page is drawn before its next click.
            }
            const auto cell = rm::ui::buildCellOrigin(layout, index - layout.first);
            inputHudClick(cell[0] + layout.cellWidth / 2, cell[1] + layout.cellHeight / 2);
            return true;
        };
        const auto inputLiveUnits = [&] {
            std::vector<rm::sim::UnitId> ids;
            for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                if (units.store.slotAlive(slot)) ids.push_back(units.store.idAt(slot));
            }
            return ids;
        };
        const auto inputFindSpawn = [&](const std::string& name,
                                         std::span<const rm::sim::UnitId> before) {
            // A completed build can reuse a dead slot. Compare the full handle, not slot count.
            for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                if (!units.store.slotAlive(slot)) continue;
                const auto id = units.store.idAt(slot);
                if (std::ranges::find(before, id) != before.end()) continue;
                const auto* def = units.catalog.def(units.store.typeAt(slot));
                if (def && uppercase(def->name) == name) return id;
            }
            return rm::sim::UnitId{};
        };
        const auto nextInputStage = [&](int stage) {
            inputStage = stage;
            stageStarted = matchTicks;
            acceptanceFastForward = stage == 2 || stage == 9;
        };
        std::function<void(NSTimer*)> inputTick = [&](NSTimer* timer) {
            if (acceptanceFrames == observedFrame) return;
            observedFrame = acceptanceFrames;
            try {
                inputCheck(matchTicks - stageStarted < 12000 && acceptanceFrames < 12000,
                           "timeout at input stage " + std::to_string(inputStage));
                if (expectedInputCommand) {
                    const auto& expected = *expectedInputCommand;
                    std::size_t accepted = 0;
                    const auto log = units.commands.all();
                    for (std::size_t i = expected.logStart; i < log.size(); ++i) {
                        if (log[i].kind == expected.kind && log[i].queued == expected.queued
                            && std::ranges::find(log[i].units, expected.unit) != log[i].units.end()) {
                            ++accepted;
                        }
                    }
                    inputCheck(accepted == 1, "native input did not dispatch exactly one accepted command");
                    expectedInputCommand.reset();
                }
                if (inputStage == -1) {
                    // Exercise the inactive-window path deliberately: switching to another
                    // app previously made AppKit silently discard the helper's left clicks.
                    [app deactivate];
                    nextInputStage(0);
                } else if (inputStage == 0) {
                    // Launch activation can finish after the first display callback. Wait
                    // for AppKit's state change before testing input, under the normal timeout.
                    if (app.isActive) {
                        [app deactivate];
                        return;
                    }
                    for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                        if (!units.store.slotAlive(slot)) continue;
                        const auto* def = units.catalog.def(units.store.typeAt(slot));
                        if (!def) continue;
                        const std::string factory = uppercase(def->name);
                        if (factory != "UEB0101" && factory != "UAB0101"
                            && factory != "URB0101" && factory != "XSB0101") continue;
                        if (units.armyOf(slot) != units.playerArmy) continue;
                        // --units seats fixtures at map starts, including the ACUs' starts.
                        // Choose an unobstructed factory; a coincident ACU is a different click target.
                        const auto& at = units.store.transforms()[slot];
                        bool crowded = false;
                        for (rm::UnitIndex other = 0; other < units.store.slotCount(); ++other) {
                            if (other == slot || !units.store.slotAlive(other)) continue;
                            const auto& neighbour = units.store.transforms()[other];
                            if (rm::sim::fxHypot(at.x - neighbour.x, at.z - neighbour.z)
                                < rm::sim::fxFromFloat(2 * rm::kDefaultPickRadiusElmos)) {
                                crowded = true;
                                break;
                            }
                        }
                        if (crowded) continue;
                        inputFactory = units.store.idAt(slot);
                        const std::string prefix = factory.substr(0, 2) + "L";
                        for (const char* suffix : {"0101", "0103", "0104", "0105"}) {
                            inputProducts.push_back(prefix + suffix);
                        }
                        if (factory != "XSB0101") inputProducts.push_back(prefix + "0106");
                        inputProducts.push_back(prefix + (factory == "URB0101" ? "0107" : "0201"));
                        inputGenerator = factory.substr(0, 3) + "1101";
                        break;
                    }
                    inputCheck(inputFactory.generation != 0, "supply a retail T1 land factory with --units");
                    inputCheck(units.playerArmy >= 0 && !units.economies.empty(), "input acceptance requires --skirmish");
                    const auto viewport = window.uiViewport();
                    std::printf("input acceptance: %.0fx%.0f logical points, simulated %.1fx backing, %zu products\n",
                        viewport.logicalExtent.width, viewport.logicalExtent.height,
                        viewport.backingScale, inputProducts.size());
                    nextInputStage(12);
                } else if (inputStage == 1) {
                    inputCheck(activeBuilder == inputFactory, "factory selection did not expose its build tray");
                    beforeProduct = inputLiveUnits();
                    const auto before = units.commandInput.size();
                    if (inputBuildClick(inputProducts[productIndex])) {
                        inputCheck(units.commandInput.size() == before + 1,
                            "factory cell did not submit exactly one Build");
                        expectInputCommand(rm::sim::CommandKind::Build, inputFactory, true);
                        nextInputStage(2);
                    }
                } else if (inputStage == 2) {
                    inputProduct = inputFindSpawn(inputProducts[productIndex], beforeProduct);
                    if (inputProduct.generation == 0) return;
                    if (!inputSelect(inputProduct)) return;
                    nextInputStage(3);
                } else if (inputStage == 3) {
                    inputCheck(commandAvailable[1], "produced unit has no reachable Move control");
                    moveStarted = units.store.transforms()[inputProduct.index];
                    const auto before = units.commandInput.size();
                    inputCommandClick(1);
                    inputCheck(armedCommand == rm::sim::CommandKind::Move, "Move widget did not arm");
                    inputWorldClick(moveStarted.x + rm::sim::Fx::fromInt(80), moveStarted.z,
                                    rm::MouseButton::Right);
                    inputCheck(units.commandInput.size() == before + 1,
                        "native right click did not dispatch exactly one Move");
                    expectInputCommand(rm::sim::CommandKind::Move, inputProduct);
                    nextInputStage(4);
                } else if (inputStage == 4) {
                    const auto& at = units.store.transforms()[inputProduct.index];
                    if (rm::sim::fxHypot(at.x - moveStarted.x, at.z - moveStarted.z)
                        < rm::sim::Fx::fromInt(12)) return;
                    const auto before = units.commandInput.size();
                    inputWorldClick(moveStarted.x + rm::sim::Fx::fromInt(80),
                                    moveStarted.z + rm::sim::Fx::fromInt(48), rm::MouseButton::Right, true);
                    inputCheck(units.commandInput.size() == before + 1,
                               "Shift-right did not append exactly one queued order on release");
                    expectInputCommand(rm::sim::CommandKind::Move, inputProduct, true);
                    nextInputStage(5);
                } else if (inputStage == 5) {
                    inputCheck(units.store.orders()[inputProduct.index].size() == 2,
                               "Shift-right replaced the active route instead of appending a waypoint");
                    const auto before = units.commandInput.size();
                    inputCommandClick(6); // Unimplemented cell must swallow left and right input.
                    inputCommandClick(6, rm::MouseButton::Right);
                    inputCheck(units.commandInput.size() == before && selected.size() == 1
                        && selected.front() == inputProduct, "disabled rack cell leaked input into world");
                    inputCommandClick(4);
                    inputCheck(units.commandInput.size() == before + 1,
                        "Stop widget did not submit exactly one Stop; before=" + std::to_string(before)
                        + " after=" + std::to_string(units.commandInput.size())
                        + " tick=" + std::to_string(matchTicks));
                    expectInputCommand(rm::sim::CommandKind::Stop, inputProduct);
                    nextInputStage(6);
                } else if (inputStage == 6) {
                    inputCheck(units.store.alive(inputProduct), "produced unit died before the Guard check");
                    inputCheck(!units.store.motion()[inputProduct.index].moving
                        && units.store.orders()[inputProduct.index].active() == nullptr,
                        "Stop did not clear movement and queued orders");
                    inputCheck(commandAvailable[5], "produced unit has no reachable Guard control");
                    inputCommandClick(5);
                    inputCheck(armedCommand == rm::sim::CommandKind::Guard, "Guard widget did not arm");
                    const auto& guarded = units.store.transforms()[inputFactory.index];
                    inputWorldClick(guarded.x, guarded.z, rm::MouseButton::Right, false,
                                    rm::sim::fxToFloat(guarded.y));
                    expectInputCommand(rm::sim::CommandKind::Guard, inputProduct);
                    nextInputStage(19);
                } else if (inputStage == 19) {
                    const auto* guard = units.store.orders()[inputProduct.index].active();
                    inputCheck(guard != nullptr && guard->kind() == rm::sim::CommandKind::Guard
                        && guard->target() == inputFactory, "native Guard did not retain the selected ally");
                    inputCommandClick(4);
                    expectInputCommand(rm::sim::CommandKind::Stop, inputProduct);
                    nextInputStage(20);
                } else if (inputStage == 20) {
                    inputCheck(units.store.orders()[inputProduct.index].empty(), "Stop did not cancel Guard");
                    std::printf("input acceptance: %s native Guard target and Stop PASS\n",
                                inputProducts[productIndex].c_str());
                    if (commandAvailable[2]) {
                        const auto* def = units.catalog.def(units.store.typeAt(inputProduct.index));
                        const bool targetsGround = std::ranges::any_of(def->weapons,
                            [](const auto& weapon) { return weapon.fires() && weapon.canTarget(false); });
                        const auto at = units.store.transforms()[inputProduct.index];
                        // Fixture setup only: place an unarmed enemy in the weapon's target
                        // layer. The test issues Attack exclusively through the drawn rack
                        // and a native world click after the target has been rendered.
                        const auto target = spawnUnit(units, content, map->field,
                            targetsGround ? "/units/UEB1101/UEB1101_unit.bp"
                                          : "/units/UEA0101/UEA0101_unit.bp",
                            {rm::sim::fxToFloat(at.x) + 100, 0, rm::sim::fxToFloat(at.z) + 48},
                            units.armies.at(1), 0);
                        inputCheck(target.has_value(), "cannot spawn native Attack target fixture");
                        inputAttackTarget = *target;
                        nextInputStage(21);
                    } else {
                        const auto before = units.commandInput.size();
                        const auto armedBefore = armedCommand;
                        inputCommandClick(2);
                        inputCheck(units.commandInput.size() == before && armedCommand == armedBefore,
                                   "unavailable Attack widget accepted input");
                        std::printf("input acceptance: %s native Attack unavailable PASS\n",
                                    inputProducts[productIndex].c_str());
                        nextInputStage(inputProducts[productIndex].ends_with("0105") ? 7 : 10);
                    }
                } else if (inputStage == 21) {
                    inputCheck(units.store.alive(inputAttackTarget), "Attack fixture died before input");
                    inputCommandClick(2);
                    inputCheck(armedCommand == rm::sim::CommandKind::Attack, "Attack widget did not arm");
                    const auto& at = units.store.transforms()[inputAttackTarget.index];
                    inputWorldClick(at.x, at.z, rm::MouseButton::Right, false, rm::sim::fxToFloat(at.y));
                    expectInputCommand(rm::sim::CommandKind::Attack, inputProduct);
                    nextInputStage(22);
                } else if (inputStage == 22) {
                    const auto* attack = units.store.orders()[inputProduct.index].active();
                    inputCheck(attack != nullptr && attack->kind() == rm::sim::CommandKind::Attack
                        && attack->target() == inputAttackTarget,
                        "native Attack did not retain the selected enemy handle");
                    inputCheck(writePng(session.window.inputAcceptancePath + "."
                        + inputProducts[productIndex] + ".attack.png", window.capture()),
                        "Attack capture write failed");
                    inputCommandClick(4);
                    expectInputCommand(rm::sim::CommandKind::Stop, inputProduct);
                    nextInputStage(23);
                } else if (inputStage == 23) {
                    inputCheck(units.store.orders()[inputProduct.index].empty(), "Stop did not cancel Attack");
                    std::printf("input acceptance: %s native Attack target and Stop PASS\n",
                                inputProducts[productIndex].c_str());
                    // Remove the fixture so it cannot distract the next product's Guard
                    // check or obstruct the engineer's placement. This is not combat proof.
                    units.store.kill(inputAttackTarget);
                    nextInputStage(inputProducts[productIndex].ends_with("0105") ? 7 : 10);
                } else if (inputStage == 7) {
                    inputCheck(activeBuilder == inputProduct, "engineer selection did not expose construction tray");
                    if (!engineerPageProbe) {
                        engineerPageProbe = true;
                        const auto panel = rm::ui::buildPanelLayout(rm::ui::frameLayout(window.uiViewport()),
                            buildOptions.size(), panelPages.build(units.store.typeAt(inputProduct.index)));
                        if (panel.pages > 1) {
                            inputHudClick(panel.x + panel.width - rm::ui::kBuildPadding - 11.0f,
                                          panel.y + rm::ui::kBuildHeader / 2);
                            ++pageClicks;
                            return;
                        }
                    }
                    if (inputBuildClick(inputGenerator)) {
                        inputCheck(armedOption.has_value(), "generator build cell did not arm placement");
                        nextInputStage(8);
                    }
                } else if (inputStage == 8) {
                    const auto& at = units.store.transforms()[inputProduct.index];
                    beforeGenerator = inputLiveUnits();
                    bool placed = false;
                    for (float dx : {48.0f, -48.0f, 72.0f, -72.0f}) {
                        for (float dz : {0.0f, 48.0f, -48.0f}) {
                            const float x = rm::sim::fxToFloat(at.x) + dx;
                            const float z = rm::sim::fxToFloat(at.z) + dz;
                            if (!armedPlaceable({x, z})) continue;
                            const auto before = units.commandInput.size();
                            inputWorldClick(rm::sim::fxFromFloat(x), rm::sim::fxFromFloat(z), rm::MouseButton::Left);
                            inputCheck(units.commandInput.size() == before + 1,
                                "world placement did not submit exactly one Build");
                            expectInputCommand(rm::sim::CommandKind::Build, inputProduct);
                            placed = true;
                            break;
                        }
                        if (placed) break;
                    }
                    inputCheck(placed, "fixture has no visible placeable generator site");
                    nextInputStage(9);
                } else if (inputStage == 9) {
                    if (inputFindSpawn(inputGenerator, beforeGenerator).generation == 0) return;
                    std::printf("input acceptance: %s placed and completed %s\n",
                                inputProducts[productIndex].c_str(), inputGenerator.c_str());
                    nextInputStage(10);
                } else if (inputStage == 10) {
                    std::printf("input acceptance: %s production, selection, Move, Shift queue, Stop, input swallowing PASS\n",
                                inputProducts[productIndex].c_str());
                    if (++productIndex < inputProducts.size()) {
                        nextInputStage(12);
                    } else nextInputStage(11);
                } else if (inputStage == 12) {
                    if (inputSelect(inputFactory)) nextInputStage(productionProbeDone ? 1 : 13);
                } else if (inputStage >= 13 && inputStage <= 18) {
                    const auto rect = rm::ui::productionPanelRect(rm::ui::frameLayout(window.uiViewport()));
                    const auto click = [&](const rm::ui::Rect& button) {
                        inputHudClick(button.x + button.width / 2, button.y + button.height / 2);
                    };
                    const auto production = gatherProduction(units, inputFactory);
                    inputCheck(production.has_value(), "factory production panel is missing");
                    if (inputStage == 13) {
                        // Fill two pages through real tray clicks before testing a pending row.
                        if (queuedProbeEntries <= rm::ui::productionRowsFor(rect)) {
                            if (inputBuildClick(inputProducts.back())) {
                                ++queuedProbeEntries;
                                expectInputCommand(rm::sim::CommandKind::Build, inputFactory, true);
                            }
                            return;
                        }
                        inputCheck(production->queue.size() == queuedProbeEntries,
                                   "production entries merged or finished before the cancellation probe");
                        for (const auto& row : production->queue) productionProbeIds.push_back(row.commandId);
                        click(rm::ui::productionPageButtonRect(rect, true));
                        nextInputStage(14);
                    } else if (inputStage == 14) {
                        const auto page = rm::ui::productionPage(rect, production->queue.size(), productionPage);
                        inputCheck(page.page == 1 && page.shown == 1, "next-page widget did not expose the pending row");
                        inputCheck(writePng(session.window.inputAcceptancePath + ".queue.png", window.capture()),
                                   "queue capture write failed");
                        click(rm::ui::productionCancelRect(rect, 0));
                        expectInputCommand(rm::sim::CommandKind::CancelFactoryBuild, inputFactory);
                        productionProbeIds.pop_back();
                        nextInputStage(15);
                    } else if (inputStage == 15 || inputStage == 17) {
                        inputCheck(std::ranges::equal(production->queue, productionProbeIds, {},
                            &rm::ui::ProductionEntry::commandId), "cancellation removed a different production entry");
                        inputCheck(productionPage == 0, "production page did not clamp after deletion");
                        if (inputStage == 15) {
                            nextInputStage(16);
                        } else {
                            click(rm::ui::productionClearRect(rect));
                            expectInputCommand(rm::sim::CommandKind::Stop, inputFactory);
                            nextInputStage(18);
                        }
                    } else if (inputStage == 16) {
                        click(rm::ui::productionCancelRect(rect, 0));
                        expectInputCommand(rm::sim::CommandKind::CancelFactoryBuild, inputFactory);
                        productionProbeIds.erase(productionProbeIds.begin());
                        nextInputStage(17);
                    } else {
                        inputCheck(production->queue.empty() && !production->building,
                                   "Clear Queue did not stop the remaining production");
                        productionProbeDone = true;
                        std::printf("input acceptance: production pagination, pending/active cancellation, Clear Queue PASS\n");
                        nextInputStage(1);
                    }
                } else {
                    inputCheck(writePng(session.window.inputAcceptancePath, window.capture()), "capture write failed");
                    acceptanceResult = 0;
                    std::printf("input acceptance: PASS %zu products, %zu page clicks, %d normal match ticks\n",
                                inputProducts.size(), pageClicks, matchTicks);
                    [timer invalidate];
                    window.stop();
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "input acceptance: FAIL stage=%d: %s\n", inputStage, error.what());
                if (!writePng(session.window.inputAcceptancePath, window.capture())) {
                    std::fprintf(stderr, "input acceptance: failure capture could not be written\n");
                }
                [timer invalidate];
                window.stop();
            }
            std::fflush(stdout);
        };
        if (inputAcceptance) {
            [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer* timer) {
                inputTick(timer);
            }];
        }

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

        if (!inputAcceptance) [app activateIgnoringOtherApps:YES];
        [app run]; // never returns until the app quits
    return acceptanceResult;
}

} // namespace rm::app
