#include "core/sim/Movement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include <vector>

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

    // Tilt every unit onto the ground underneath it — NOT just the ones that
    // moved. A scene of scattered units has ordered none of them, and gating
    // this on movement would leave all of them sticking out horizontally on
    // their hillsides, which is the whole thing alignment exists to fix.
    //
    // A separate pass rather than a line in the loop above, because it applies
    // to a different set: the loop moves what is moving, this tilts everything.
    for (std::size_t i = 0; i < count; ++i) {
        UnitInstance& unit = instances[i];
        const std::array<float, 2> align =
            slopeAlignment(field, unit.position[0], unit.position[2], unit.rotationY);
        unit.rotationX = align[0];
        unit.rotationZ = align[1];
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

std::array<float, 2> slopeAlignment(const HeightField& field, float x, float z,
                                    float yaw) noexcept {
    // Surface y = h(x, z). The unnormalised normal is (-dh/dx, 1, -dh/dz).
    // A fixed 1-elmo sample distance is small compared to an 8-elmo square and
    // large enough not to drown in quantisation.
    constexpr float kSampleDistance = 1.0f;
    const float dx = (field.heightAtWorld(x + kSampleDistance, z)
                      - field.heightAtWorld(x - kSampleDistance, z))
                   / (2.0f * kSampleDistance);
    const float dz = (field.heightAtWorld(x, z + kSampleDistance)
                      - field.heightAtWorld(x, z - kSampleDistance))
                   / (2.0f * kSampleDistance);

    const float normalLength = std::sqrt(dx * dx + 1.0f + dz * dz);
    if (!(normalLength > 0.0f)) {
        return {{0.0f, 0.0f}};
    }
    const float nx = -dx / normalLength;
    const float ny = 1.0f / normalLength;
    const float nz = -dz / normalLength;

    // Transform the world normal into the unit's local frame by undoing yaw.
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    const float nxLocal = nx * c - nz * s;
    const float nyLocal = ny;
    const float nzLocal = nx * s + nz * c;

    // Roll (rotationZ) then pitch (rotationX) in the local frame maps local +Y
    // toward the local normal. Clamp asin input for vertical walls.
    const float roll = -std::asin(std::clamp(nxLocal, -1.0f, 1.0f));
    const float cosRoll = std::cos(roll);
    const float pitch = cosRoll > 1e-4f ? std::atan2(nzLocal, nyLocal) : 0.0f;

    return {{pitch, roll}};
}

void resolveCollisions(std::span<UnitInstance> instances, std::span<const MoveState> motion,
                       const HeightField& field) {
    const std::size_t count = std::min(instances.size(), motion.size());
    if (count < 2) {
        return;
    }

    // A uniform grid over the map, so a unit only tests the neighbours that
    // could actually reach it. Without one this is every pair against every
    // other, which is fine for the forty units a demo places and quadratic for
    // the hundreds a rally order gathers.
    float largestRadius = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        largestRadius = std::max(largestRadius, motion[i].radiusElmos);
    }
    if (largestRadius <= 0.0f) {
        return;  // nothing here occupies any space
    }

    // Cells two radii across: any pair that overlaps is then either in the same
    // cell or in touching ones, so eight neighbours is the whole search.
    const float cellSize = largestRadius * 2.0f;
    const auto cellOf = [cellSize](float value) {
        return static_cast<int>(std::floor(value / cellSize));
    };

    // Keyed on the packed cell coordinates. A map rather than a dense grid
    // because a crowd occupies a handful of cells out of the tens of thousands
    // a map has, and the dense version would cost more to clear than to search.
    std::unordered_map<std::int64_t, std::vector<std::size_t>> buckets;
    buckets.reserve(count);

    const auto key = [](int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    };

    for (std::size_t i = 0; i < count; ++i) {
        if (motion[i].radiusElmos <= 0.0f) {
            continue;
        }
        buckets[key(cellOf(instances[i].position[0]), cellOf(instances[i].position[2]))]
            .push_back(i);
    }

    // Ascending index order, resolving each pair as it is found, so the result
    // does not depend on how the buckets happened to be laid out.
    for (std::size_t a = 0; a < count; ++a) {
        const float radiusA = motion[a].radiusElmos;
        if (radiusA <= 0.0f) {
            continue;
        }

        const int cx = cellOf(instances[a].position[0]);
        const int cz = cellOf(instances[a].position[2]);

        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto bucket = buckets.find(key(cx + dx, cz + dz));
                if (bucket == buckets.end()) {
                    continue;
                }

                for (const std::size_t b : bucket->second) {
                    // Each pair once, and never a unit against itself.
                    if (b <= a) {
                        continue;
                    }

                    const float radiusB = motion[b].radiusElmos;
                    if (radiusB <= 0.0f) {
                        continue;
                    }

                    float dxWorld = instances[b].position[0] - instances[a].position[0];
                    float dzWorld = instances[b].position[2] - instances[a].position[2];
                    const float wanted = radiusA + radiusB;
                    float distance = std::hypot(dxWorld, dzWorld);

                    if (distance >= wanted) {
                        continue;
                    }

                    // Exactly coincident, which a rally order produces the
                    // moment two units are given the same destination. There is
                    // no direction to separate along, so one is invented from
                    // the pair's indices — deterministic, and different for
                    // each pair, so a stack fans out instead of picking one
                    // axis and forming a line.
                    if (distance < 1e-4f) {
                        const auto spread = static_cast<float>((a * 7 + b * 13) % 360);
                        const float angle = spread * (std::numbers::pi_v<float> / 180.0f);
                        dxWorld = std::cos(angle);
                        dzWorld = std::sin(angle);
                        distance = 1.0f;
                    }

                    // Half the overlap each: neither unit outranks the other,
                    // and moving only one would let a unit under orders shove
                    // its way through a crowd untouched.
                    const float push = (wanted - distance) * 0.5f;
                    const float nx = dxWorld / distance;
                    const float nz = dzWorld / distance;

                    instances[a].position[0] -= nx * push;
                    instances[a].position[2] -= nz * push;
                    instances[b].position[0] += nx * push;
                    instances[b].position[2] += nz * push;
                }
            }
        }
    }

    // Put everyone back on the map and on the ground. Done once at the end
    // rather than per push, since a unit may be moved by several neighbours.
    for (std::size_t i = 0; i < count; ++i) {
        if (motion[i].radiusElmos <= 0.0f) {
            continue;
        }
        UnitInstance& unit = instances[i];
        unit.position[0] = std::clamp(unit.position[0], 0.0f, field.widthElmos());
        unit.position[2] = std::clamp(unit.position[2], 0.0f, field.depthElmos());
        unit.position[1] = field.heightAtWorld(unit.position[0], unit.position[2]);
    }
}

} // namespace rm::sim
