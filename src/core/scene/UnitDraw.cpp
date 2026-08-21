#include "core/scene/UnitDraw.hpp"

#include <algorithm>
#include <cmath>

namespace rm {
namespace {

/// `(1 - t) * a + t * b`, and the form matters — see the header. Exact at both endpoints.
[[nodiscard]] float mix(float a, float b, float t) noexcept { return (1.0f - t) * a + t * b; }

/// A `Mag` health pair as a fraction of full.
[[nodiscard]] float fraction(sim::Mag current, sim::Mag maximum) noexcept {
    const float full = sim::magToFloat(maximum);
    return full > 0.0f ? std::clamp(sim::magToFloat(current) / full, 0.0f, 1.0f) : 1.0f;
}

/// One unit drawn where its snapshot says, with no blending.
[[nodiscard]] DrawUnit at(const sim::UnitView& view) noexcept {
    return DrawUnit{
        .id = view.id,
        .type = view.type,
        .armyIndex = view.armyIndex,
        .position = {sim::fxToFloat(view.transform.x), sim::fxToFloat(view.transform.y),
                     sim::fxToFloat(view.transform.z)},
        .rotationX = sim::radiansFromBrad(view.transform.pitch),
        .rotationY = sim::radiansFromBrad(view.transform.heading),
        .rotationZ = sim::radiansFromBrad(view.transform.roll),
        .distanceTravelledElmos = sim::fxToFloat(view.distanceTravelledElmos),
        .speedPerTick = sim::fxToFloat(view.speedPerTick),
        .healthFraction = fraction(view.health, view.maxHealth),
    };
}

/// The shortest way round between two angles, in radians, interpolated.
///
/// IN BINARY RADIANS FIRST, then converted. A unit turning through north goes from 65,000 to
/// 500 in `Brad`, and lerping those as numbers spins it the long way round at high speed — the
/// classic angle-interpolation bug. `Brad`'s difference is already the shortest arc, because
/// [-32,768, 32,767) is exactly [-half turn, +half turn) in two's complement, so the fix is to
/// do the subtraction in the wrapping type and only then leave it.
[[nodiscard]] float mixAngle(Brad from, Brad to, float t) noexcept {
    const auto delta = static_cast<std::int16_t>(static_cast<std::uint16_t>(to)
                                                 - static_cast<std::uint16_t>(from));
    const auto stepped = static_cast<Brad>(
        static_cast<std::uint16_t>(from)
        + static_cast<std::uint16_t>(static_cast<std::int16_t>(
              std::lround(static_cast<double>(delta) * static_cast<double>(t)))));
    // The endpoints are exact by construction: at t = 0 the step is 0 and at t = 1 it is the
    // whole delta, which added to `from` IS `to` in the wrapping type.
    return sim::radiansFromBrad(stepped);
}

} // namespace

void project(const sim::Snapshot& state, std::vector<DrawUnit>& out) {
    out.clear();
    out.reserve(state.units.size());
    for (const sim::UnitView& view : state.units) {
        out.push_back(at(view));
    }
}

void interpolate(const sim::Snapshot& from, const sim::Snapshot& to, float alpha,
                 std::vector<DrawUnit>& out) {
    const float t = std::clamp(alpha, 0.0f, 1.0f);

    // The endpoints are handled by the merge below rather than short-circuited, so that alpha 0
    // and alpha 1 go through exactly the same code as alpha 0.5. A short-circuit would make the
    // stated test pass while proving nothing about the path a real frame takes.
    out.clear();
    out.reserve(to.units.size());

    // A TWO-POINTER MERGE, because both snapshots are in slot order and a unit's slot does not
    // change while it lives. So this is linear and allocation-free, where matching by id
    // through a map would be a hash lookup per unit per frame.
    std::size_t before = 0;
    for (const sim::UnitView& now : to.units) {
        // Advance past anything in `from` that is no longer here — units that died, and slots
        // whose occupant was replaced (a lower generation in the same slot).
        while (before < from.units.size()
               && (from.units[before].id.index < now.id.index
                   || (from.units[before].id.index == now.id.index
                       && from.units[before].id.generation < now.id.generation))) {
            ++before;
        }

        const bool wasHere = before < from.units.size() && from.units[before].id == now.id;
        if (!wasHere) {
            // Spawned since `from`. Drawn where it is, not blended in from somewhere it never
            // was — the whole reason this is matched by id and not by index.
            out.push_back(at(now));
            continue;
        }

        const sim::UnitView& then = from.units[before];
        out.push_back(DrawUnit{
            .id = now.id,
            .type = now.type,
            .armyIndex = now.armyIndex,
            .position = {mix(sim::fxToFloat(then.transform.x), sim::fxToFloat(now.transform.x),
                             t),
                         mix(sim::fxToFloat(then.transform.y), sim::fxToFloat(now.transform.y),
                             t),
                         mix(sim::fxToFloat(then.transform.z), sim::fxToFloat(now.transform.z),
                             t)},
            .rotationX = mixAngle(then.transform.pitch, now.transform.pitch, t),
            .rotationY = mixAngle(then.transform.heading, now.transform.heading, t),
            .rotationZ = mixAngle(then.transform.roll, now.transform.roll, t),
            .distanceTravelledElmos = mix(sim::fxToFloat(then.distanceTravelledElmos),
                                          sim::fxToFloat(now.distanceTravelledElmos), t),
            .speedPerTick = sim::fxToFloat(now.speedPerTick),
            // Health is NOT interpolated toward the new value from the old — it is taken as it
            // is. A health bar that eased into a hit would show a unit at 40% when the sim had
            // already killed it, and the bar is information rather than motion.
            .healthFraction = fraction(now.health, now.maxHealth),
        });
    }
}

} // namespace rm
