#include "app/Interface.hpp"

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
/// The player's own units are drawn a point bigger. A minimap's job is to say where the
/// important things are, and every pip the same size says only "units".
void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene) {
    out.clear();
    out.reserve(scene.snapshotCurrent.size());
    for (const rm::sim::UnitView& unit : scene.snapshotCurrent.units) {
        const int owner = unit.armyIndex;
        out.push_back(rm::ui::MinimapPip{
            .worldX = rm::sim::fxToFloat(unit.transform.x),
            .worldZ = rm::sim::fxToFloat(unit.transform.z),
            .colour = owner >= 0 && static_cast<std::size_t>(owner) < scene.armies.size()
                          ? rm::teamColour(static_cast<std::size_t>(owner))
                          : rm::kTeamColours[0],
            .size = owner == scene.playerArmy ? 3.0f : 2.0f,
        });
    }
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

/// Reads the scene into the state the interface reports.
///
/// A translation step rather than the interface reaching into the scene, so `ui::build` takes
/// a small struct it can be tested against and knows nothing about batches, health arrays or
/// deques.
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

} // namespace rm::app
