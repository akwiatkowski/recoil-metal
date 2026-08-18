#include "core/sim/Movement.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0f * kPi;

/// Wraps an angle difference into [-pi, pi] — the shortest way round.
///
/// Without this a unit facing 0.1 rad and asked to face 6.2 turns almost all
/// the way round the long way instead of a tenth of a radian back.
[[nodiscard]] float shortestAngleTo(float from, float to) noexcept {
    float delta = std::fmod(to - from, kTwoPi);
    if (delta > kPi) {
        delta -= kTwoPi;
    } else if (delta < -kPi) {
        delta += kTwoPi;
    }
    return delta;
}

} // namespace

namespace rm::sim {

void orderTo(MoveState& state, const HeightField& field, float x, float z) noexcept {
    state.destinationX = std::clamp(x, 0.0f, field.widthElmos());
    state.destinationZ = std::clamp(z, 0.0f, field.depthElmos());
    state.moving = true;

    // A direct order supersedes a route. Without this the unit would reach the
    // new destination and then carry on with whatever it was doing before.
    state.path.clear();
    state.pathIndex = 0;
}

void orderAlongPath(MoveState& state, std::span<const std::array<float, 2>> path) {
    state.path.assign(path.begin(), path.end());
    state.pathIndex = 0;

    if (state.path.empty()) {
        state.moving = false;
        return;
    }

    state.destinationX = state.path.front()[0];
    state.destinationZ = state.path.front()[1];
    state.moving = true;
}

void tick(std::span<UnitInstance> instances, std::span<MoveState> motion,
          const HeightField& field) noexcept {
    const std::size_t count = std::min(instances.size(), motion.size());

    for (std::size_t i = 0; i < count; ++i) {
        MoveState& state = motion[i];
        if (!state.moving) {
            continue;
        }

        UnitInstance& unit = instances[i];

        const float dx = state.destinationX - unit.position[0];
        const float dz = state.destinationZ - unit.position[2];
        const float distance = std::hypot(dx, dz);

        // Reached this waypoint. The threshold is the radius or one tick of
        // travel, whichever is larger: a unit fast enough to cross the radius in
        // a single tick would otherwise step past, turn round, step past again,
        // and jitter there for the rest of the game.
        //
        // An intermediate waypoint uses the loose radius, so a unit rounds a
        // corner rather than driving into each cell centre; the last one uses
        // the tight radius, because that is where it was actually sent.
        const bool onFinalWaypoint = state.pathIndex + 1 >= state.path.size();
        const float radius = onFinalWaypoint ? kArrivalRadiusElmos : kWaypointRadiusElmos;
        const float travel = state.speedElmosPerSecond * kTickSeconds;

        if (distance <= std::max(radius, travel)) {
            if (!onFinalWaypoint) {
                ++state.pathIndex;
                state.destinationX = state.path[state.pathIndex][0];
                state.destinationZ = state.path[state.pathIndex][1];
                continue;  // aim at the next one on the following tick
            }

            state.moving = false;
            state.path.clear();
            state.pathIndex = 0;
            continue;
        }

        // Turn toward the destination, but no faster than the unit can.
        // atan2(dx, dz), not the usual atan2(z, x): the vertex shader maps a
        // model's local +Z to (sin yaw, cos yaw), so yaw is measured from +Z
        // toward +X. Swapping the arguments compiles, runs, and renders every
        // unit walking sideways.
        const float desired = std::atan2(dx, dz);
        const float error = shortestAngleTo(unit.rotationY, desired);
        const float maxTurn = state.turnRateRadiansPerSecond * kTickSeconds;
        unit.rotationY += std::clamp(error, -maxTurn, maxTurn);

        // Forward speed falls off with how badly the unit is still pointed the
        // wrong way, reaching zero at 90 degrees off. This is what makes a unit
        // pivot roughly in place before setting off, rather than driving away
        // at full speed and arcing back — and it needs no arbitrary "turn until
        // aligned" threshold, because the cosine already is one.
        const float remaining = shortestAngleTo(unit.rotationY, desired);
        const float alignment = std::max(0.0f, std::cos(remaining));

        // Never step past the destination, however fast the unit is.
        const float step = std::min(travel * alignment, distance);

        const float previousX = unit.position[0];
        const float previousZ = unit.position[2];

        unit.position[0] += std::sin(unit.rotationY) * step;
        unit.position[2] += std::cos(unit.rotationY) * step;

        // The destination is already on the map, but the arc taken to reach it
        // need not be — a unit pivoting near a border can swing outside it.
        unit.position[0] = std::clamp(unit.position[0], 0.0f, field.widthElmos());
        unit.position[2] = std::clamp(unit.position[2], 0.0f, field.depthElmos());

        // Measured after the clamp, so a unit pressed against the border stops
        // striding instead of walking on the spot forever. Horizontal only: a
        // walk cycle is paced by ground covered, not by height climbed.
        state.distanceTravelledElmos +=
            std::hypot(unit.position[0] - previousX, unit.position[2] - previousZ);

        // Sit on the ground. Interpolated, so crossing a square does not pop —
        // that is the whole reason HeightField::heightAtWorld exists.
        unit.position[1] = field.heightAtWorld(unit.position[0], unit.position[2]);
    }
}

int TickClock::advance(float seconds) noexcept {
    // Time never runs backwards, but a clock read across a sleep or a change of
    // timebase can say it did. Banking a negative would swallow the next real
    // frames' worth of ticks.
    if (seconds > 0.0f) {
        unspentSeconds_ += seconds;
    }

    const auto whole = static_cast<int>(unspentSeconds_ / kTickSeconds);
    const int ticks = std::clamp(whole, 0, kMaxTicksPerAdvance);
    unspentSeconds_ -= static_cast<float>(ticks) * kTickSeconds;

    // Past the cap the surplus is dropped rather than carried, or the backlog
    // simply reappears next frame and the stall never clears.
    if (whole > kMaxTicksPerAdvance) {
        unspentSeconds_ = 0.0f;
    }

    return ticks;
}

} // namespace rm::sim
