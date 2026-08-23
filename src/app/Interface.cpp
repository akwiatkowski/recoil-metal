#include "app/Interface.hpp"

#include "core/ui/IconAtlas.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <cmath>

namespace rm::app {

/// The minimap's pips, from the snapshot the renderer is already drawing.
///
/// FROM THE SNAPSHOT, which is the whole reason §7 calls P7.4 "nearly free once a `Map` object
/// and a snapshot exist": before P7.1 the only list of where units were was the renderer's
/// per-batch instance arrays, and a minimap would have needed a second walk over game state that
/// nothing owned.
///
/// A radar contact's colour: deliberately not anybody's team colour. See below.
constexpr rm::ui::Colour kBlipColour{0.85f, 0.85f, 0.55f, 0.75f};

/// The blips: what radar and sonar know and sight does not.
///
/// A PIP OF ITS OWN COLOUR AND A SMALLER SIZE, not the owner's team colour — because a blip
/// is a position without an identity, and painting it in the enemy's colour would be telling
/// the player something radar did not say. Whose it is, and what it is, are exactly the two
/// facts a contact withholds (`sim::Contact`).
void appendMinimapBlips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene) {
    std::vector<rm::sim::Contact>& scratch = scene.contactScratch;
    const int viewer = scene.viewingAlliance();
    if (viewer == UnitScene::kNoAlliance) {
        return;  // an observer sees units, not guesses about them
    }

    rm::sim::contactsFor(viewer, scene.store, scene.catalog, scene.armies, scene.intel,
                         scene.snapshotCurrent.tick, scratch);

    for (const rm::sim::Contact& contact : scratch) {
        if (!contact.isBlip()) {
            continue;  // already plotted, in its own colour, where it really is
        }
        out.push_back(rm::ui::MinimapPip{
            .worldX = rm::sim::fxToFloat(contact.x),
            .worldZ = rm::sim::fxToFloat(contact.z),
            .colour = kBlipColour,
            .size = 2.0f,
        });
    }
}

/// The player's own units are drawn a point bigger. A minimap's job is to say where the
/// important things are, and every pip the same size says only "units".
///

void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene) {
    out.clear();
    out.reserve(scene.snapshotCurrent.size());

    const int viewer = scene.viewingAlliance();

    for (const rm::sim::UnitView& unit : scene.snapshotCurrent.units) {
        const int owner = unit.armyIndex;
        const float worldX = rm::sim::fxToFloat(unit.transform.x);
        const float worldZ = rm::sim::fxToFloat(unit.transform.z);

        // WHAT THE MINIMAP SHOWS IS WHAT THE SCREEN SHOWS. A minimap that plotted every unit
        // would make the fog decorative: a player could read the enemy's whole position off
        // the corner of the display and never look at the map.
        if (!scene.visibleToViewer(viewer, owner, worldX, worldZ)) {
            continue;
        }

        out.push_back(rm::ui::MinimapPip{
            .worldX = worldX,
            .worldZ = worldZ,
            .colour = owner >= 0 && static_cast<std::size_t>(owner) < scene.armies.size()
                          ? rm::teamColour(static_cast<std::size_t>(owner))
                          : rm::kTeamColours[0],
            .size = owner == scene.playerArmy ? 3.0f : 2.0f,
        });
    }

    appendMinimapBlips(out, scene);
}


/// The four ground points the viewport's corners see, for the minimap's view outline.
///
/// THE SAME RAY-TO-GROUND PICK A RIGHT-CLICK USES, so the outline is exact rather than estimated
/// from the camera's distance and pitch. A corner looking past the map's edge or at the sky
/// yields nothing, and the caller draws no outline rather than a wrong one — which is the honest
/// answer for a camera that is not looking at the ground.
void appendViewFootprint(std::vector<std::array<float, 2>>& out, const rm::OrbitCamera& camera,
                         const rm::HeightField& field, float width, float height) {
    out.clear();
    // Clockwise from the top-left, so consecutive pairs are the edges of the shape.
    const std::array<std::array<float, 2>, 4> corners{
        {{0.0f, height}, {width, height}, {width, 0.0f}, {0.0f, 0.0f}}};
    for (const std::array<float, 2>& corner : corners) {
        const rm::Ray ray = rm::screenRay(camera, corner[0], corner[1], width, height);
        const std::optional<simd_float3> ground = rm::pickGround(ray, field);
        if (!ground) {
            out.clear();  // all four or none: a partial outline is a wrong one
            return;
        }
        out.push_back({ground->x, ground->z});
    }
}

/// Reads the scene into the list the build panel draws.
///
/// A TRANSLATION STEP rather than the panel reaching into the scene, which is what lets
/// `core/ui/BuildPanel.hpp` be tested over a `std::span<const BuildOption>` and know nothing
/// about stores, catalogs, rosters or deques. The two halves are tested separately for the
/// same reason they are separate: `tests/test_build_panel.cpp` asks where a cell is,
/// `tests/test_build_options.cpp` asks what belongs in it.
void gatherBuildOptions(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                        const rm::ui::Theme& theme, std::vector<rm::ui::BuildOption>& out,
                        BuildSelection& who) {
    out.clear();
    who = BuildSelection{};
    if (selection.empty() || scene.roster.size() == 0) {
        return;
    }

    // FROM THE ROSTER, NOT FROM `BuildTree` OVER `scene.definitions`, and the first version got
    // this wrong in a way only a screenshot caught. `scene.definitions` holds the types that
    // have been REGISTERED — one spawns, or `resolveBuildable` resolves one for a build order —
    // so a commander's menu came back with a single entry: the extractor the opening had just
    // asked for. Everything a player has not built yet is, by definition, exactly what a build
    // menu is for.
    //
    // `Roster` is built from the whole blueprint corpus at content load, and `Roster::all` says
    // in its own comment that it exists for "a caller listing options rather than picking one —
    // the build tray, eventually". This is that caller.
    static constexpr std::array<rm::unitdef::Role, 6> kStructureRoles{
        rm::unitdef::Role::Extractor, rm::unitdef::Role::Energy, rm::unitdef::Role::Factory,
        rm::unitdef::Role::Storage,   rm::unitdef::Role::Defence, rm::unitdef::Role::Radar,
    };

    float storedMass = 0.0f;
    if (scene.playerArmy != rm::sim::kNoArmy
        && static_cast<std::size_t>(scene.playerArmy) < scene.economies.size()) {
        storedMass = rm::sim::magToFloat(
            scene.economies[static_cast<std::size_t>(scene.playerArmy)].stored.mass);
    }

    for (const rm::sim::UnitId id : selection) {
        if (!scene.store.alive(id)) {
            continue;
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(id.index));
        if (def == nullptr) {
            continue;
        }
        const rm::unitdef::Role role = rm::unitdef::roleOf(*def);
        // A COMMANDER, AN ENGINEER — or a FACTORY. The first two build STRUCTURES, answered
        // by role; a factory builds MOBILE units, answered by its own `BuildableCategory`
        // expression, which is the game's statement of what rolls off this floor and needs
        // no taxonomy of ours. Anything else in the selection offers nothing.
        const bool buildsStructures =
            role == rm::unitdef::Role::Commander || role == rm::unitdef::Role::Builder;
        const bool isFactory = role == rm::unitdef::Role::Factory;
        if (!buildsStructures && !isFactory) {
            continue;
        }

        const int army = scene.armyOf(id.index);
        if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
            continue;
        }
        const rm::sim::Faction faction = scene.armies[static_cast<std::size_t>(army)].faction;

        who = BuildSelection{.builder = id,
                             .name = def->name,
                             .role = std::string{rm::unitdef::roleName(role)}};

        if (isFactory) {
            for (const rm::data::RosterEntry& entry :
                 scene.roster.buildableBy(faction, def->buildableCategory)) {
                // TIER ONE ONLY, the structure tray's rule for the structure tray's reason.
                if (entry.tech > 1) {
                    continue;
                }
                const float mass = rm::sim::magToFloat(entry.costMass);
                const float seconds =
                    def->buildRate > 0.0f
                        ? rm::sim::magToFloat(entry.buildTime) / def->buildRate
                        : 0.0f;
                out.push_back(rm::ui::BuildOption{
                    .id = entry.id,
                    .name = entry.description,
                    .massCost = mass,
                    .energyCost = rm::sim::magToFloat(entry.costEnergy),
                    .buildSeconds = seconds,
                    .health = rm::sim::magToFloat(entry.health),
                    .affordable = mass <= storedMass,
                    .tint = rm::ui::tierTint(theme, entry.tech),
                });
            }
            return;  // the first builder decides — see the header
        }
        for (const rm::unitdef::Role wanted : kStructureRoles) {
            for (const rm::data::RosterEntry& entry : scene.roster.all(faction, wanted)) {
                // TIER ONE ONLY, for now. A commander can build a T1 structure of each kind, and
                // the higher tiers need an upgraded engineer this engine does not yet model —
                // listing them would offer a player something no order could satisfy, which is
                // worse than a short menu.
                if (entry.tech > 1) {
                    continue;
                }
                const float mass = rm::sim::magToFloat(entry.costMass);
                // Seconds at THIS builder's rate — the blueprint states work, the builder
                // states work per second, and the player is only ever told the quotient.
                const float seconds =
                    def->buildRate > 0.0f
                        ? rm::sim::magToFloat(entry.buildTime) / def->buildRate
                        : 0.0f;
                out.push_back(rm::ui::BuildOption{
                    .id = entry.id,
                    .name = entry.description,
                    .massCost = mass,
                    .energyCost = rm::sim::magToFloat(entry.costEnergy),
                    .buildSeconds = seconds,
                    .health = rm::sim::magToFloat(entry.health),
                    .affordable = mass <= storedMass,
                    .tint = rm::ui::tierTint(theme, entry.tech),
                });
            }
        }
        return;  // the first builder decides — see the header
    }
}

[[nodiscard]] rm::ui::MatchState hudStateFrom(const UnitScene& scene, float elapsedSeconds) {
    rm::ui::MatchState state;
    state.elapsedSeconds = elapsedSeconds;
    state.armiesTotal = scene.armies.size();
    state.armiesLeft = rm::sim::survivorCount(scene.armies);

    for (const rm::sim::Health& one : scene.store.health()) {
        if (one.alive()) {
            ++state.unitsAlive;
        }
    }

    if (scene.playerArmy != rm::sim::kNoArmy
        && static_cast<std::size_t>(scene.playerArmy) < scene.economies.size()) {
        const rm::sim::Economy& mine =
            scene.economies[static_cast<std::size_t>(scene.playerArmy)];

        // The HUD reads in floats and per SECOND — it is a display, and a player thinks in
        // seconds. This is the sim-to-renderer half of the float boundary (`Fx.hpp`).
        const auto perSecond = [](rm::sim::Mag perTick) {
            return rm::sim::magToFloat(perTick)
                   * static_cast<float>(gAppTickRate.ticksPerSecond());
        };
        state.mass = rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.mass),
                                   .capacity = rm::sim::magToFloat(mine.storage.mass),
                                   .incomePerSecond = perSecond(mine.incomePerTick.mass),
                                   .drainPerSecond = perSecond(mine.upkeepPerTick.mass)};
        state.energy = rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.energy),
                                     .capacity = rm::sim::magToFloat(mine.storage.energy),
                                     .incomePerSecond = perSecond(mine.incomePerTick.energy),
                                     .drainPerSecond = perSecond(mine.upkeepPerTick.energy)};
        state.fundedFraction = rm::sim::fxToFloat(mine.fundedFraction);
    }

    if (!scene.armies.empty() && state.armiesLeft <= 1) {
        const std::optional<int> winner = rm::sim::winningAlliance(scene.armies);
        state.outcome = winner ? rm::ui::MatchState::Outcome::Win
                               : rm::ui::MatchState::Outcome::Draw;
        state.winningTeam = winner.value_or(0);
    }
    return state;
}

/// The livery the interface wears: the player's own faction, or the neutral cyan when the scene
/// has no armies in it.
bool gFafSkin = false;

namespace {
/// What packInterfaceIcons packed for the skin, for hudThemeFor to attach. File-scope
/// because the two run on opposite sides of the atlas upload and share nothing else.
rm::ui::PanelSkin gPackedSkin;
} // namespace

[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene) {
    rm::ui::Theme theme = rm::ui::neutralTheme();
    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy) {
            theme = rm::ui::themeFor(army.faction);
            break;
        }
    }
    // The skin rides whatever livery won: the chrome is the game's, the accents stay the
    // faction's.
    theme.skin = gPackedSkin;
    return theme;
}

/// Appends an icon for every unit in the scene too small to read, at the camera's current
/// zoom. See core/scene/UnitIcons.hpp.
///
/// Shared by the windowed loop and the capture paths, because a screenshot that did not show
/// the icons would make the feature unverifiable — and AGENT.md asks for a screenshot or it
/// did not happen.
void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera,
                      std::span<const std::optional<StrategicIconRef>> refs) {
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    std::vector<float> radii;
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        radii.clear();
        radii.reserve(scene.drawSlotOf[batch].size());
        for (const rm::UnitIndex slot : scene.drawSlotOf[batch]) {
            // A type the strategic layer drew is passed a zero radius, which
            // `appendUnitIcons` reads as "skip" — the square is the fallback for the
            // glyphless, not a backing plate for everyone.
            const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
            const bool drawnAsGlyph = type < refs.size() && refs[type].has_value();
            radii.push_back(drawnAsGlyph
                                ? 0.0f
                                : rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos));
        }
        (void)rm::appendUnitIcons(into, scene.drawScratch[batch], radii, elmosPerPoint);
    }
}

void appendStrategicIcons(rm::ui::Geometry& out, const UnitScene& scene,
                          const rm::OrbitCamera& camera, float width, float height,
                          std::span<const std::optional<StrategicIconRef>> refs) {
    if (refs.empty() || !(width > 0.0f) || !(height > 0.0f)) {
        return;
    }
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    if (!(elmosPerPoint > 0.0f)) {
        return;
    }

    // The glyphs ship at their reading size for a ~1000-point view; scaling by the actual
    // viewport keeps them the same fraction of the screen on every window, exactly as the
    // square fallback's point size behaves.
    const float scale = height / rm::kIconReferenceHeightPoints;

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        for (std::size_t i = 0; i < scene.drawScratch[batch].size()
                                && i < scene.drawSlotOf[batch].size(); ++i) {
            const rm::UnitIndex slot = scene.drawSlotOf[batch][i];
            const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
            if (type >= refs.size() || !refs[type]) {
                continue;  // no glyph: the square fallback owns this one
            }
            const float radius = rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos);
            if (radius <= 0.0f
                || rm::apparentPoints(radius, elmosPerPoint) >= rm::kIconThresholdPoints) {
                continue;  // the mesh still reads; a glyph over it would be noise on a tank
            }

            const rm::UnitInstance& unit = scene.drawScratch[batch][i];
            const auto screen = rm::worldToScreen(
                camera,
                simd_make_float3(unit.position[0], unit.position[1], unit.position[2]),
                width, height);
            if (!screen) {
                continue;
            }

            const StrategicIconRef& ref = *refs[type];
            const float w = static_cast<float>(ref.width) * scale;
            const float h = static_cast<float>(ref.height) * scale;
            const float x0 = (*screen)[0] - w * 0.5f;
            const float y0 = (*screen)[1] - h * 0.5f;
            const float x1 = x0 + w;
            const float y1 = y0 + h;

            // The glyph is white artwork; the army's colour arrives as the tint the image
            // shader multiplies in — the same identity story the pips and the old squares
            // tell, now wearing a shape.
            const rm::ui::IconUv uv = rm::ui::iconUvSized(ref.slot, ref.width, ref.height);
            const std::array<float, 4> tint{unit.teamColour[0], unit.teamColour[1],
                                            unit.teamColour[2], 0.95f};
            out.worldImage.push_back({{x0, y0}, {uv.u0, uv.v0}, tint});
            out.worldImage.push_back({{x1, y0}, {uv.u1, uv.v0}, tint});
            out.worldImage.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.worldImage.push_back({{x0, y0}, {uv.u0, uv.v0}, tint});
            out.worldImage.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.worldImage.push_back({{x0, y1}, {uv.u0, uv.v1}, tint});
        }
    }
}

void appendHealthBars(rm::ui::Geometry& out, const UnitScene& scene,
                      const rm::OrbitCamera& camera, const rm::text::Font& font, float width,
                      float height) {
    if (!font.usable() || !(width > 0.0f) || !(height > 0.0f)) {
        return;
    }

    // The icon threshold, inverted: icons take over when a unit shrinks past reading
    // (UnitIcons.hpp), and the bar stops where they start — one regime per zoom, never both.
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    if (!(elmosPerPoint > 0.0f)) {
        return;
    }

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        for (std::size_t i = 0; i < scene.drawScratch[batch].size()
                                && i < scene.drawSlotOf[batch].size(); ++i) {
            const rm::UnitIndex slot = scene.drawSlotOf[batch][i];
            const rm::sim::Health& hp = scene.store.health()[slot];
            if (!hp.alive() || hp.current >= hp.maximum) {
                continue;  // the absence of a bar is what "fine" looks like
            }

            const float radiusElmos =
                rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos);
            const float radiusPoints = radiusElmos / elmosPerPoint;
            if (radiusPoints * 2.0f < rm::kIconThresholdPoints) {
                continue;  // icon territory: the strategic layer owns this zoom
            }

            const rm::UnitInstance& unit = scene.drawScratch[batch][i];
            const auto screen = rm::worldToScreen(
                camera,
                simd_make_float3(unit.position[0], unit.position[1], unit.position[2]),
                width, height);
            if (!screen) {
                continue;
            }

            // Sized with the unit but clamped: a bar narrower than ~16 points is unreadable
            // and one wider than ~48 reads as interface chrome rather than a unit's.
            const float barWidth = std::clamp(radiusPoints * 2.2f, 16.0f, 48.0f);
            const float barHeight = 3.0f;
            const float x = (*screen)[0] - barWidth * 0.5f;
            const float y = (*screen)[1] - radiusPoints - 8.0f;

            const float fill =
                rm::sim::magToFloat(hp.current) / rm::sim::magToFloat(hp.maximum);
            // The roster underbar's own thresholds — the two must not disagree about how
            // bad the same number is.
            const rm::ui::Colour bar = fill > 0.6f ? rm::ui::kGain
                                       : (fill > 0.3f ? rm::ui::kWarn : rm::ui::kLoss);

            rm::text::appendRect(out.label, font, x, y, barWidth, barHeight,
                                 rm::ui::Colour{{0.0f, 0.0f, 0.0f, 0.55f}});
            rm::text::appendRect(out.label, font, x, y, barWidth * std::clamp(fill, 0.0f, 1.0f),
                                 barHeight, bar);
        }
    }
}

/// The unit nearest the ray across every batch, IGNORING who owns it.
///
/// What a right-click wants: an order aimed at an enemy has to be able to find one, and
/// the selection pick deliberately cannot.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAnyBatch(const rm::Ray& ray,
                                                          const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    // Against what is DRAWN, because that is what the ray can see — a dead unit is not in
    // the gather and so cannot be clicked. The hit comes back as a (batch, index) pair and
    // is turned into a handle, which is what everything downstream now speaks.
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }
        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = scene.unitDrawnAt(batch, *hit);
    }
    return best;
}

/// Whether `army` may shoot what `entry` points at.
[[nodiscard]] bool hostileTo(const UnitScene& scene, int army, rm::sim::UnitId id) {
    const int theirs = scene.armyOf(id.index);
    if (army == rm::sim::kNoArmy || theirs == rm::sim::kNoArmy) {
        return false;
    }
    const auto find = [&scene](int index) -> const rm::sim::Army* {
        for (const rm::sim::Army& candidate : scene.armies) {
            if (candidate.index == index) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const rm::sim::Army* mine = find(army);
    const rm::sim::Army* target = find(theirs);
    return mine != nullptr && target != nullptr && rm::sim::hostile(*mine, *target);
}

/// The unit nearest the ray across every batch, or nothing.
///
/// pickUnit searches one array at a time because that is how the instances are
/// held — one per model. Comparing its winners across batches is what makes the
/// nearest unit on SCREEN win, rather than the nearest one in whichever model
/// happened to load first.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAcrossBatches(const rm::Ray& ray,
                                                              const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }

        // A unit belongs to whoever the sim says, and only the player's own may be
        // selected. Filtered HERE rather than inside pickUnit, which is pure over
        // instances and has no business knowing about armies.
        //
        // Note what this deliberately does NOT do: it does not skip an enemy and keep
        // looking behind it. Clicking an enemy selects nothing, because that is what
        // was clicked — the alternative reaches through the thing under the cursor to
        // grab something else, which is worse than a null answer.
        const std::optional<rm::sim::UnitId> id = scene.unitDrawnAt(batch, *hit);
        if (!id) {
            continue;
        }
        if (scene.playerArmy != rm::sim::kNoArmy
            && scene.armyOf(id->index) != scene.playerArmy) {
            continue;
        }

        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = id;
    }

    return best;
}


/// The archive path for a unit's icon, derived from its id.
///
/// ONE PLACE, because two panels need it and a path spelled twice is a path that can be spelled
/// differently. The corpus's own layout, and the same reasoning `RosterEntry::path` gives for
/// deriving rather than storing.
[[nodiscard]] std::string iconPathFor(const std::string& id) {
    return "/textures/ui/common/icons/units/" + id + "_icon.dds";
}

/// One unit's icon out of the archives, or an empty texture.
///
/// EMPTY IS THE ORDINARY ANSWER and it is not an error: `UEB5208` ships no icon at all, measured
/// over `textures.scd`. `packIcons` skips an empty input and leaves its slot blank, which is
/// what keeps the packing positional.
[[nodiscard]] rm::dds::Texture iconFor(const rm::vfs::Vfs& content, const std::string& id) {
    const auto bytes = content.read(iconPathFor(id));
    if (!bytes) {
        return {};
    }
    auto icon = rm::dds::load(*bytes);
    return icon ? std::move(*icon) : rm::dds::Texture{};
}

void ensureStrategicIconArt(UnitScene& scene, const rm::vfs::Vfs& content) {
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(type));
        if (def == nullptr || def->strategicIcon.empty()
            || scene.strategicIconMissing.contains(def->strategicIcon)) {
            continue;
        }
        const auto cached = std::find_if(
            scene.strategicIconArt.begin(), scene.strategicIconArt.end(),
            [&](const auto& entry) { return entry.first == def->strategicIcon; });
        if (cached != scene.strategicIconArt.end()) {
            continue;
        }

        // The `_rest` state: the glyph at rest is the map's own layer; `_over` and
        // `_selected` belong to a hover-and-selection story the strategic layer does not
        // tell yet.
        const std::string path = "/textures/ui/common/game/strategicicons/"
                                 + def->strategicIcon + "_rest.dds";
        const auto bytes = content.read(path);
        bool loaded = false;
        if (bytes) {
            if (auto icon = rm::dds::load(*bytes); icon && !icon->data.empty()) {
                scene.strategicIconArt.emplace_back(def->strategicIcon, std::move(*icon));
                loaded = true;
            }
        }
        if (!loaded) {
            scene.strategicIconMissing.insert(def->strategicIcon);
        }
    }
}

void buildStrategicIconRefs(const UnitScene& scene, std::size_t base,
                            std::vector<std::optional<StrategicIconRef>>& out) {
    out.assign(scene.catalog.size(), std::nullopt);
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(type));
        if (def == nullptr || def->strategicIcon.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < scene.strategicIconArt.size(); ++i) {
            const auto& [name, art] = scene.strategicIconArt[i];
            if (name == def->strategicIcon) {
                out[type] = StrategicIconRef{
                    .slot = base + i, .width = art.width, .height = art.height};
                break;
            }
        }
    }
}

rm::dds::Texture packInterfaceIcons(const rm::vfs::Vfs& content,
                                    std::vector<rm::ui::BuildOption>& options,
                                    std::vector<rm::ui::RosterTile>& tiles,
                                    std::span<const std::pair<std::string, rm::dds::Texture>>
                                        strategic,
                                    std::size_t* strategicBase) {
    std::vector<rm::dds::Texture> icons;
    icons.reserve(options.size() + tiles.size() + strategic.size());

    // ONE NUMBERING ACROSS BOTH PANELS. A slot is an index into `icons`, and `packIcons` places
    // by that index, so appending the roster's after the tray's is all "one atlas" needs to be
    // true — there is no second base to add and get wrong.
    for (rm::ui::BuildOption& option : options) {
        option.iconSlot.reset();
        rm::dds::Texture icon = iconFor(content, option.id);
        if (!icon.data.empty()) {
            option.iconSlot = icons.size();
        }
        icons.push_back(std::move(icon));
    }
    for (rm::ui::RosterTile& tile : tiles) {
        tile.iconSlot.reset();
        rm::dds::Texture icon = iconFor(content, tile.id);
        if (!icon.data.empty()) {
            tile.iconSlot = icons.size();
        }
        icons.push_back(std::move(icon));
    }

    // The strategic glyphs after both panels' icons, in the cache's own order — so `base +
    // cache index` is the slot, and the caller needs nothing but the base to map them.
    if (strategicBase != nullptr) {
        *strategicBase = icons.size();
    }
    for (const auto& [name, art] : strategic) {
        icons.push_back(art);  // a copy per pack; a glyph is ~100 bytes of BC3 blocks
    }

    // The skin's nine slices, last, when `--ui faf` asked for the game's own chrome. The
    // atlas is the ride every icon already takes; nine more squares cost nothing and spare
    // the renderer a second texture bind it has no slot for.
    gPackedSkin = rm::ui::PanelSkin{};
    std::array<std::size_t, 9> skinSlots{};
    std::array<std::array<int, 2>, 9> skinSizes{};
    bool skinComplete = gFafSkin;
    if (gFafSkin) {
        constexpr std::array<const char*, 9> kPieces{
            "generic_brd_ul",      "generic_brd_horz_um", "generic_brd_ur",
            "generic_brd_vert_l",  "generic_brd_m",       "generic_brd_vert_r",
            "generic_brd_ll",      "generic_brd_horz_lm", "generic_brd_lr"};
        for (std::size_t piece = 0; piece < kPieces.size(); ++piece) {
            const std::string path = std::string{"/textures/ui/common/game/generic_brd/"}
                                     + kPieces[piece] + ".dds";
            const std::optional<std::vector<std::byte>> bytes = content.read(path);
            const auto art = bytes ? rm::dds::load(*bytes)
                                   : std::expected<rm::dds::Texture, rm::MapError>{
                                         std::unexpect, rm::MapError{}};
            if (!art || art->width == 0) {
                skinComplete = false;
                break;
            }
            skinSlots[piece] = icons.size();
            skinSizes[piece] = {static_cast<int>(art->width), static_cast<int>(art->height)};
            icons.push_back(*art);
        }
    }

    rm::dds::Texture atlas = rm::ui::packIcons(icons);
    if (skinComplete && !atlas.data.empty()) {
        for (std::size_t piece = 0; piece < 9; ++piece) {
            gPackedSkin.uv[piece] = rm::ui::iconUvSized(skinSlots[piece],
                                                        skinSizes[piece][0],
                                                        skinSizes[piece][1]);
            gPackedSkin.size[piece] = {static_cast<float>(skinSizes[piece][0]),
                                       static_cast<float>(skinSizes[piece][1])};
        }
        gPackedSkin.active = true;
        std::printf("  ui: FAF chrome packed (generic_brd, nine slices)\n");
    }
    if (atlas.data.empty()) {
        // Nothing packed: no cell may point into an empty atlas, or it draws whatever the
        // sampler makes of a texture that is not bound.
        for (rm::ui::BuildOption& option : options) {
            option.iconSlot.reset();
        }
        for (rm::ui::RosterTile& tile : tiles) {
            tile.iconSlot.reset();
        }
    }
    return atlas;
}

void gatherRoster(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                  std::vector<rm::ui::RosterTile>& out) {
    out.clear();

    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::vector<float> health;
    std::vector<float> maxHealth;
    ids.reserve(selection.size());
    names.reserve(selection.size());
    health.reserve(selection.size());
    maxHealth.reserve(selection.size());

    for (const rm::sim::UnitId id : selection) {
        if (!scene.store.alive(id)) {
            continue;  // a selection outlives the units in it
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(id.index));
        if (def == nullptr) {
            continue;
        }
        const rm::sim::Health& hp = scene.store.health()[id.index];
        ids.push_back(def->name);
        names.push_back(def->description);
        health.push_back(rm::sim::magToFloat(hp.current));
        maxHealth.push_back(rm::sim::magToFloat(hp.maximum));
    }

    out = rm::ui::groupSelection(ids, health, maxHealth, names);
}

} // namespace rm::app
