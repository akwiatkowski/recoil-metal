#include "app/Run.hpp"


#include "app/RunShared.hpp"
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
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

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
                    // Bounded by the slot list, not the instance list: a projectile-mesh
                    // batch draws instances that belong to no unit slot.
                    for (std::size_t i = 0; i < units.drawSlotOf[batch].size() && made < rings;
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
                        appendIntelRings(vertices, map->field, units, slot);
                        appendRallyLine(vertices, map->field, units, slot);
                        // The order queue in a capture too: a headless run cannot hold
                        // shift, so the capture draws what the modifier would — the
                        // route here, its predicted times with the HUD below.
                        rm::app::appendOrderRoute(vertices, map->field, units, slot,
                                                  ground);
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
            // TEMP DEBUG: pip census for the radar investigation; remove after.
            units.refreshViewerContacts();
            int seen = 0, radarB = 0, sonarB = 0;
            for (const auto& c : units.contactScratch) {
                if (c.kind == rm::sim::ContactKind::Seen) ++seen;
                else if (c.kind == rm::sim::ContactKind::Radar) ++radarB;
                else if (c.kind == rm::sim::ContactKind::Sonar) ++sonarB;
            }
            int alive0 = 0, alive1 = 0;
            for (rm::UnitIndex s = 0; s < units.store.slotCount(); ++s) {
                if (!units.store.slotAlive(s)) continue;
                const int a = units.store.motion()[s].armyIndex;
                if (a == 0) ++alive0; else if (a == 1) ++alive1;
            }
            std::printf("DEBUG pips=%zu deposits=%zu snapshot=%zu seen=%d radar=%d sonar=%d "
                        "alive0=%d alive1=%d viewer=%d\n",
                        pips.size(), units.resourceDeposits.size(), units.snapshotCurrent.size(),
                        seen, radarB, sonarB, alive0, alive1, units.viewingAlliance());
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
            // The same tier strip the live panel applies: a menu spanning more than one
            // tier draws tabs and defaults to the builder's own, else the lowest.
            std::uint32_t shotTierMask = rm::ui::buildTiersPresent(shotOptions);
            int shotTier = 0;
            if ((shotTierMask & (shotTierMask - 1u)) != 0) {
                int builderTech = 0;
                if (const rm::unitdef::UnitDef* builderDef = units.catalog.def(
                        units.store.typeAt(shotWho.builder.index))) {
                    builderTech = rm::unitdef::techOf(*builderDef);
                }
                shotTier = builderTech >= 1 && (shotTierMask & (1u << builderTech)) != 0
                               ? builderTech
                               : std::countr_zero(shotTierMask);
                shotOptions = rm::ui::buildOptionsForTier(shotOptions, shotTier);
            } else {
                shotTierMask = 0;
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
            // The selection's ACTIVE ORDER, for acceptance of orders the panels do not show:
            // a guard or a move has no card, so a headless run needs this line to prove one
            // is still standing after the replayed commands and the simulated seconds.
            if (!capturedSelection.empty() && units.store.alive(capturedSelection.front())) {
                const rm::sim::QueuedCommand* head =
                    units.store.orders()[capturedSelection.front().index].active();
                const rm::sim::Transform& where =
                    units.store.transforms()[capturedSelection.front().index];
                std::printf("  hud-order: unit=%u:%u kind=%s target=%u:%u at=%.0f,%.0f\n",
                            capturedSelection.front().index, capturedSelection.front().generation,
                            head != nullptr ? rm::sim::commandKindName(head->kind()) : "none",
                            head != nullptr ? head->target().index : 0u,
                            head != nullptr ? head->target().generation : 0u,
                            static_cast<double>(rm::sim::fxToFloat(where.x)),
                            static_cast<double>(rm::sim::fxToFloat(where.z)));
            }
            std::vector<const rm::unitdef::UnitDef*> shotCommandSelection;
            for (const rm::sim::UnitId id : capturedSelection) {
                if (units.store.alive(id)) {
                    shotCommandSelection.push_back(
                        units.catalog.def(units.store.typeAt(id.index)));
                }
            }
            std::vector<rm::sim::SiloAmmo> shotSilos;
            for (const rm::sim::SiloAmmo& record : units.siloAmmo) {
                if (std::ranges::find(capturedSelection, record.owner)
                    != capturedSelection.end()) {
                    shotSilos.push_back(record);
                }
            }
            const rm::ui::CommandPage shotCommandPage =
                rm::ui::commandPage(shotCommandSelection, shotSilos, units.siloQueue);
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

            // The predicted order times the capture stands in for shift on. Paired
            // with the route decals above, a queued `--march` wears its ETA.
            std::size_t shotOrderTimes = 0;
            for (const rm::sim::UnitId id : capturedSelection) {
                if (units.store.alive(id)) {
                    shotOrderTimes += rm::app::appendOrderTimes(
                        hud, units, renderer.camera(), map->field,
                        renderer.labelFont(), shotViewport, id.index);
                }
            }
            if (shotOrderTimes > 0) {
                std::printf("  order-times: %zu\n", shotOrderTimes);
            }

            // The reclaim overlay stands in for a builder under the cursor: a capture
            // cannot arm one, so a staged `--wreck` beside a `--select`-ed builder
            // wears the same field totals the live overlay draws.
            if (std::any_of(capturedSelection.begin(), capturedSelection.end(),
                            [&](rm::sim::UnitId id) {
                                const rm::unitdef::UnitDef* def =
                                    units.store.alive(id)
                                        ? units.catalog.def(units.store.typeAt(id.index))
                                        : nullptr;
                                return def != nullptr && def->isBuilder();
                            })) {
                const std::size_t reclaimLabels = rm::app::appendReclaimLabels(
                    hud, units, renderer.camera(), map->field,
                    renderer.labelFont(), shotViewport);
                if (reclaimLabels > 0) {
                    std::printf("  reclaim labels: %zu\n", reclaimLabels);
                }
            }

            // THE CONSTRUCTION SITES IN A CAPTURE TOO, for the reason the HUD is here: a
            // screenshot is how this project verifies anything, and an effect only visible in
            // a live window cannot be checked at all. The upload afterwards is not optional —
            // this is where the product's model is loaded, and a batch created after the last
            // `setUnits` draws nothing.
            std::vector<rm::Renderer::ConstructionDraw> shotSites;
            rm::app::gatherConstructions(units, content, map->field, shotSites);
            if (!shotSites.empty()) {
                renderer.setUnits(units.textures.all(), units.textures.srgbFlags(), units.batches);
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
            units.projectileTrails.append(shotParticles, units.weaponVisuals, 0.0f,
                renderer.camera().elmosPerPoint(rm::kIconReferenceHeightPoints),
                [&](const std::array<float, 3>& at) {
                    return units.visibleToViewer(rm::sim::fxFromFloat(at[0]),
                                                 rm::sim::fxFromFloat(at[2]));
                });
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
                        // The ribbon, over a bending path behind the bolt: eight ticks of an
                        // arc rising and falling, so a straight strip and a ribbon cannot be
                        // confused in the capture.
                        rm::ProjectileTrails ribbons;
                        std::vector<rm::sim::Projectile> path(1);
                        path[0].visualId = examples[i].second;
                        constexpr int kArcSamples = 8;      // recorded positions along the arc
                        constexpr float kArcStepElmos = 8.0f;
                        const auto arc = [&](int sample) {
                            const float t = static_cast<float>(sample) / kArcSamples;
                            return std::array<rm::sim::Fx,3>{
                                rm::sim::fxFromFloat(from[0] - kArcStepElmos * (kArcSamples - sample)),
                                rm::sim::fxFromFloat(from[1] + 6.0f * std::sin(t * 3.14159265f)),
                                rm::sim::fxFromFloat(z)};
                        };
                        path[0].visualOrigin = arc(0);
                        for (int sample = 1; sample <= kArcSamples; ++sample) {
                            path[0].position = arc(sample);
                            path[0].velocity = {rm::sim::fxFromFloat(kArcStepElmos), {}, {}};
                            ribbons.update(path, units.weaponVisuals, 0.1f);
                        }
                        const auto before = shotParticles.size();
                        ribbons.append(shotParticles, units.weaponVisuals, 0.0f, 1.0f);
                        std::printf("weapon gallery ribbon: %s, %zu segments\n", examples[i].second,
                            shotParticles.size() - before);
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
                if (hasFlag(argc, argv, "--impact-gallery")) {
                    // A seventh row: the FALLBACK vocabulary — what a shot whose weapon
                    // authored no visuals still fires with. Flash plus halo at the
                    // muzzle, the beam chain to the strike, a tracer in flight.
                    const float z = centre.z + 3.5f*16.0f;
                    const auto at = [&](float x) {
                        return std::array<rm::sim::Fx,3>{rm::sim::fxFromFloat(x),
                            rm::sim::fxFromFloat(centre.y), rm::sim::fxFromFloat(z)};
                    };
                    const std::array<rm::sim::Event,2> unauthored{{
                        {.kind=rm::sim::EventKind::WeaponFired, .at2=at(centre.x-25),
                         .visualId="none:NoWeapon",
                         .visualDirection={rm::sim::Fx::fromInt(1),{}, {}}},
                        {.kind=rm::sim::EventKind::BeamFired, .at=at(centre.x+25),
                         .at2=at(centre.x-5), .visualId="none:NoBeam",
                         .visualDirection={rm::sim::Fx::fromInt(1),{}, {}}},
                    }};
                    rm::CombatEffectState fallback;
                    const auto first = shotParticles.size();
                    rm::emitCombatEffects(shotParticles, unauthored, &units.weaponVisuals, &fallback);
                    for (auto p=first; p<shotParticles.size(); ++p) shotParticles[p].age += 0.05f;
                    rm::sim::Projectile flying;
                    flying.position = at(centre.x+50);
                    flying.velocity = {rm::sim::fxFromFloat(40.0f), {}, {}};
                    flying.visualId = "none:NoTracer";
                    rm::appendProjectiles(shotParticles, {&flying, 1}, 0.0f, 1.0f,
                                          &units.weaponVisuals);
                    const auto screen = rm::worldToScreen(renderer.camera(),
                        simd_make_float3(centre.x-95.0f, centre.y, z),
                        shotViewport.hudExtent().width, shotViewport.hudExtent().height);
                    if (screen) (void)rm::text::appendText(hud.label, renderer.labelFont().glyphs,
                        "unauthored (fallback)", (*screen)[0], (*screen)[1], {1,1,1,1});
                    std::printf("impact gallery: unauthored weapon, beam and tracer\n");
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
                                         shotWho.name, shotWho.role, shotTierMask, shotTier);
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
                        renderer.setUnits(units.textures.all(), units.textures.srgbFlags(), units.batches);
                        const auto typeIndex = static_cast<std::size_t>(*type);
                        const auto snapped = snapBuildSite(units, *type, {gx, gz});
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
                        renderer.setBuildGrid(true);
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
                        // The live ghost's adjacency labels, so a headless capture can
                        // see the numbers a player would. The connection lines are
                        // ground decals, which this path never uploads — labels carry
                        // the information on their own.
                        const auto preview = rm::app::appendAdjacencyPreview(
                            hud, units, renderer.camera(), map->field,
                            renderer.labelFont(), shotViewport, *type, {gx, gz});
                        for (const auto& link : preview.links) {
                            std::printf("  adjacency link: slot %u%s%s\n", link.slot,
                                        link.toGhost.any() ? " receives" : "",
                                        link.fromGhost.any() ? " grants" : "");
                        }
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
                            ? rm::ui::commandInspector(shotCommandPage, *shotHoveredCommand,
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
                shotCommandPage, shotHoveredCommand);

            if (shotProduction) {
                rm::ui::appendProductionPanel(hud, renderer.labelFont(), renderer.readoutFont(),
                    shotTheme, rm::ui::productionPanelRect(shotFrame), *shotProduction);
                std::printf("  production panel: %zu rows, repeat=%s\n",
                    shotProduction->queue.size(), shotProduction->repeat ? "on" : "off");
            }

            // THE ECONOMY WINDOW under `--econ-window`, for the same reason `--select`
            // exists: an overlay only a key press opens can never reach a screenshot,
            // and a screenshot is how this project verifies anything. The budget
            // stands untouched — all fabricators on — and no row is focused.
            if (hasFlag(argc, argv, "--econ-window")) {
                rm::ui::EconomyWindowView econView;
                rm::app::gatherEconomyWindow(units, capturedSelection, -1.0f,
                                             std::nullopt, econView);
                const rm::ui::Rect econRect =
                    rm::ui::economyWindowRect(shotFrame, econView.rows.size());
                rm::ui::appendEconomyWindow(hud, renderer.labelFont(),
                                            renderer.readoutFont(), shotTheme, econRect,
                                            econView);
                std::printf("  economy window: %zu row(s), %zu fabricator(s), funded %.0f%%\n",
                            econView.rows.size(), econView.fabricators.size(),
                            static_cast<double>(econView.fundedFraction * 100.0f));
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
            renderer.setUnits(units.textures.all(), units.textures.srgbFlags(), units.batches);
            units.applyFog(renderer);
            renderer.setProps(props.textures.all(), props.textures.srgbFlags(), props.batches);
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
            const auto pixelsWide = static_cast<unsigned int>(std::lround(static_cast<float>(bench.width) * shot.backing));
            const auto pixelsHigh = static_cast<unsigned int>(std::lround(static_cast<float>(bench.height) * shot.backing));
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
            renderer.setUnits(units.textures.all(), units.textures.srgbFlags(), units.batches);
            units.applyFog(renderer);
            renderer.setProps(props.textures.all(), props.textures.srgbFlags(), props.batches);
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
                static_cast<unsigned int>(std::lround(static_cast<float>(shot.width) * shot.backing));
            const auto pixelsHigh =
                static_cast<unsigned int>(std::lround(static_cast<float>(shot.height) * shot.backing));
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

} // namespace rm::app
