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
        // A COMMANDER OR AN ENGINEER, which is what the goal asks for and also what the roster
        // can answer honestly: those two build STRUCTURES, and a structure's role is a fact the
        // roster already indexes. A factory builds mobile units, which is a different query
        // against the same roster and a separate panel's worth of work.
        if (role != rm::unitdef::Role::Commander && role != rm::unitdef::Role::Builder) {
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
[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene) {
    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy) {
            return rm::ui::themeFor(army.faction);
        }
    }
    return rm::ui::neutralTheme();
}

/// Appends an icon for every unit in the scene too small to read, at the camera's current
/// zoom. See core/scene/UnitIcons.hpp.
///
/// Shared by the windowed loop and the capture paths, because a screenshot that did not show
/// the icons would make the feature unverifiable — and AGENT.md asks for a screenshot or it
/// did not happen.
void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera) {
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    std::vector<float> radii;
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        radii.clear();
        radii.reserve(scene.drawSlotOf[batch].size());
        for (const rm::UnitIndex slot : scene.drawSlotOf[batch]) {
            radii.push_back(rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos));
        }
        (void)rm::appendUnitIcons(into, scene.drawScratch[batch], radii, elmosPerPoint);
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

rm::dds::Texture packInterfaceIcons(const rm::vfs::Vfs& content,
                                    std::vector<rm::ui::BuildOption>& options,
                                    std::vector<rm::ui::RosterTile>& tiles) {
    std::vector<rm::dds::Texture> icons;
    icons.reserve(options.size() + tiles.size());

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

    rm::dds::Texture atlas = rm::ui::packIcons(icons);
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
