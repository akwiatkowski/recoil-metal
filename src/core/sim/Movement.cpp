#include "core/sim/Movement.hpp"

#include "core/sim/Pathfinding.hpp"
#include "core/sim/UnitStore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include <vector>

namespace {

using rm::sim::Fx;

/// The shortest way round, from one angle to another, as a SIGNED number of binary radians.
///
/// This function used to be six lines of `fmod` and two comparisons. In `Brad` it is a
/// subtraction and a cast, and that is not a micro-optimisation — it is the reason angles are
/// binary radians at all. A turn is 65,536, so `to - from` in 16 bits wraps into
/// [-32,768, 32,767], which IS [-half turn, +half turn): the shortest way round falls out of
/// two's complement with no wrapping step to get wrong and no modulo by an irrational to
/// accumulate error.
[[nodiscard]] std::int32_t shortestTurn(rm::Brad from, rm::Brad to) noexcept {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(to)
                                     - static_cast<std::uint16_t>(from));
}

/// One elmo, the sample distance for the terrain gradient.
inline constexpr Fx kOneElmo = Fx::fromInt(1);

} // namespace

namespace rm::sim {

Fx arrivalRadius(TickRate rate) noexcept {
    // Half a heightmap square is the finest the terrain itself resolves, so aiming tighter
    // asks for precision the ground does not have. And it must exceed ONE TICK OF TRAVEL, or a
    // unit steps past the goal every tick and never lands inside the radius at all.
    //
    // Which bound binds depends on the rate, which is exactly why this is a function now: at
    // 30 Hz a tick is 2.9 elmos and the first bound wins; at 10 Hz it is 8.7 and the second
    // does. The old constant wrote the 10 Hz answer down and would have silently stopped doing
    // its job at any other rate.
    return std::max(Fx::fromInt(kSquareSize) * Fx::fromRatio(1, 2),
                    rate.perTick(kDefaultSpeedElmosPerSecond));
}

MoveState defaultMotion(TickRate rate) noexcept {
    MoveState state;
    state.speedPerTick = rate.perTick(kDefaultSpeedElmosPerSecond);
    state.turnPerTick = rate.bradPerTick(kDefaultTurnRateRadiansPerSecond);
    return state;
}

void orderTo(MoveState& state, const Terrain& terrain, Fx x, Fx z) noexcept {
    const Fx width = Fx::fromInt(terrain.field().squaresX * kSquareSize);
    const Fx depth = Fx::fromInt(terrain.field().squaresZ * kSquareSize);
    state.destinationX = std::clamp(x, Fx{}, width);
    state.destinationZ = std::clamp(z, Fx{}, depth);
    state.moving = true;

    // A direct order supersedes a route. Without this the unit would reach the
    // new destination and then carry on with whatever it was doing before.
    state.path.clear();
    state.pathIndex = 0;
}

void orderAlongPath(MoveState& state, std::span<const std::array<Fx, 2>> path) {
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

void tick(std::span<Transform> transforms, std::span<MoveState> motion,
          const Terrain& terrain) noexcept {
    const std::size_t count = std::min(transforms.size(), motion.size());

    const Fx width = Fx::fromInt(terrain.field().squaresX * kSquareSize);
    const Fx depth = Fx::fromInt(terrain.field().squaresZ * kSquareSize);

    // The arrival radius depends on the default travel per tick, and the speeds are already
    // per tick — so it is derived from the fastest thing here rather than from a rate the
    // pass no longer sees. One tick of THIS unit's travel is the bound that matters.
    const Fx halfSquare = Fx::fromInt(kSquareSize) * Fx::fromRatio(1, 2);

    for (std::size_t i = 0; i < count; ++i) {
        MoveState& state = motion[i];
        if (!state.moving || state.attached) {
            continue;
        }

        Transform& unit = transforms[i];

        const Fx dx = state.destinationX - unit.x;
        const Fx dz = state.destinationZ - unit.z;

        // Bearing and distance from one CORDIC pass rather than two: the pass needs both, and
        // vectoring mode produces both.
        const Polar toTarget = fxPolar(dx, dz);
        const Fx distance = toTarget.length;

        // Reached this waypoint. The threshold is the radius or one tick of
        // travel, whichever is larger: a unit fast enough to cross the radius in
        // a single tick would otherwise step past, turn round, step past again,
        // and jitter there for the rest of the game.
        //
        // An intermediate waypoint uses the loose radius, so a unit rounds a
        // corner rather than driving into each cell centre; the last one uses
        // the tight radius, because that is where it was actually sent.
        const bool onFinalWaypoint = state.pathIndex + 1 >= state.path.size();
        const Fx radius = onFinalWaypoint ? std::max(halfSquare, state.speedPerTick)
                                          : kWaypointRadius;
        const Fx travel = state.speedPerTick;

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

        // Turn toward the destination, but no faster than the unit can. The bearing already
        // came out of `fxPolar` above, measured from +Z toward +X — the engine's convention,
        // which the function's argument order enforces rather than a comment.
        const std::int32_t error = shortestTurn(unit.heading, toTarget.bearing);
        const std::int32_t maxTurn = state.turnPerTick;
        unit.heading = static_cast<Brad>(static_cast<std::uint16_t>(unit.heading)
                                         + static_cast<std::uint16_t>(
                                             std::clamp(error, -maxTurn, maxTurn)));

        // Forward speed falls off with how badly the unit is still pointed the
        // wrong way, reaching zero at 90 degrees off. This is what makes a unit
        // pivot roughly in place before setting off, rather than driving away
        // at full speed and arcing back — and it needs no arbitrary "turn until
        // aligned" threshold, because the cosine already is one.
        const auto remaining = static_cast<Brad>(shortestTurn(unit.heading, toTarget.bearing));
        const Fx alignment = std::max(Fx{}, fxCos(remaining));

        // Never step past the destination, however fast the unit is.
        const Fx step = std::min(travel * alignment, distance);

        const Fx previousX = unit.x;
        const Fx previousZ = unit.z;

        unit.x += fxSin(unit.heading) * step;
        unit.z += fxCos(unit.heading) * step;

        // The destination is already on the map, but the arc taken to reach it
        // need not be — a unit pivoting near a border can swing outside it.
        unit.x = std::clamp(unit.x, Fx{}, width);
        unit.z = std::clamp(unit.z, Fx{}, depth);

        // Measured after the clamp, so a unit pressed against the border stops
        // striding instead of walking on the spot forever. Horizontal only: a
        // walk cycle is paced by ground covered, not by height climbed.
        state.distanceTravelledElmos += fxHypot(unit.x - previousX, unit.z - previousZ);

        if (state.surfaceWater && terrain.hasWater()) {
            unit.y = terrain.waterLevel();
        } else if (!state.airborne) {
            // Ground behavior stays exactly where it was: moving units update height here;
            // idle units retain their established Y and only refresh slope below.
            unit.y = terrain.heightAt(unit.x, unit.z);
        }
    }

    // Place every unit on its layer — NOT just the ones that
    // moved. A scene of scattered units has ordered none of them, and gating
    // this on movement would leave all of them sticking out horizontally on
    // their hillsides, which is the whole thing alignment exists to fix.
    //
    // A separate pass rather than a line in the loop above, because it applies
    // to a different set: the loop moves what is moving, this tilts everything.
    for (std::size_t i = 0; i < count; ++i) {
        if (motion[i].airborne || motion[i].surfaceWater) {
            placeOnMotionLayer(transforms[i], motion[i], terrain);
            continue;
        }
        Transform& unit = transforms[i];
        const std::array<Brad, 2> align =
            slopeAlignment(terrain, unit.x, unit.z, unit.heading);
        unit.pitch = align[0];
        unit.roll = align[1];
    }
}

int TickClock::advance(float seconds) noexcept {
    // Time never runs backwards, but a clock read across a sleep or a change of
    // timebase can say it did. Banking a negative would swallow the next real
    // frames' worth of ticks.
    if (seconds > 0.0f) {
        unspentSeconds_ += seconds;
    }

    // The tick length comes from the rate this clock was built with. `secondsPerTick` is
    // legitimate here and nowhere in the sim proper: converting wall time to ticks is exactly
    // this class's job, and wall time is what the display deals in.
    const float tickSeconds = rate_.secondsPerTick();
    const auto cap = static_cast<int>(maxTicksPerAdvance());
    const auto whole = static_cast<int>(unspentSeconds_ / tickSeconds);
    const int ticks = std::clamp(whole, 0, cap);
    unspentSeconds_ -= static_cast<float>(ticks) * tickSeconds;

    // Past the cap the surplus is dropped rather than carried, or the backlog
    // simply reappears next frame and the stall never clears.
    if (whole > cap) {
        unspentSeconds_ = 0.0f;
    }

    return ticks;
}

std::array<Brad, 2> slopeAlignment(const Terrain& terrain, Fx x, Fx z, Brad yaw) noexcept {
    // Surface y = h(x, z). The unnormalised normal is (-dh/dx, 1, -dh/dz).
    // A fixed 1-elmo sample distance is small compared to an 8-elmo square and
    // large enough not to drown in quantisation.
    const Fx dx = (terrain.heightAt(x + kOneElmo, z) - terrain.heightAt(x - kOneElmo, z))
                  * Fx::fromRatio(1, 2);
    const Fx dz = (terrain.heightAt(x, z + kOneElmo) - terrain.heightAt(x, z - kOneElmo))
                  * Fx::fromRatio(1, 2);

    const Fx normalLength = fxSqrt(dx * dx + kFxOne + dz * dz);
    if (normalLength <= Fx{}) {
        return {{Brad{0}, Brad{0}}};
    }
    const Fx nx = -dx / normalLength;
    const Fx ny = kFxOne / normalLength;
    const Fx nz = -dz / normalLength;

    // Transform the world normal into the unit's local frame by undoing yaw.
    const Fx c = fxCos(yaw);
    const Fx s = fxSin(yaw);
    const Fx nxLocal = nx * c - nz * s;
    const Fx nyLocal = ny;
    const Fx nzLocal = nx * s + nz * c;

    // Roll then pitch in the local frame maps local +Y toward the local normal. `fxAsin`
    // saturates outside [-1, 1] rather than being undefined, which is what a vertical wall
    // produces — so the clamp the float version needed is now the function's own contract.
    const auto roll = static_cast<Brad>(-static_cast<std::int32_t>(fxAsin(nxLocal)));

    // Near a vertical wall the roll approaches a quarter turn, its cosine approaches zero, and
    // the pitch is meaningless. The float version tested `cosRoll > 1e-4f`; the fixed-point
    // equivalent is a few steps of the type, since anything smaller is not representable.
    const Fx cosRoll = fxCos(roll);
    const Brad pitch = cosRoll > Fx::fromRaw(4) ? fxBearing(nzLocal, nyLocal) : Brad{0};

    return {{pitch, roll}};
}

void placeOnMotionLayer(Transform& transform, const MoveState& state,
                        const Terrain& terrain) noexcept {
    if (state.surfaceWater && terrain.hasWater()) {
        transform.y = terrain.waterLevel();
        transform.pitch = Brad{0};
        transform.roll = Brad{0};
        return;
    }
    transform.y = terrain.heightAt(transform.x, transform.z);
    if (state.airborne) {
        transform.y += kAirClearanceElmos;
        transform.pitch = Brad{0};
        transform.roll = Brad{0};
        return;
    }
    const std::array<Brad, 2> align =
        slopeAlignment(terrain, transform.x, transform.z, transform.heading);
    transform.pitch = align[0];
    transform.roll = align[1];
}

void resolveCollisions(UnitStore& store, const Terrain& terrain,
                       std::span<const PassabilityGrid* const> gridForType) {
    const std::span<Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::size_t count = std::min(transforms.size(), motion.size());
    if (count < 2) {
        return;
    }

    // The largest radius decides how far a unit has to look: two units overlap only if they are
    // within the sum of their radii, and that sum is at most twice the largest.
    Fx largestRadius{};
    for (std::size_t i = 0; i < count; ++i) {
        largestRadius = std::max(largestRadius, motion[i].radiusElmos);
    }
    if (largestRadius <= Fx{}) {
        return;  // nothing here occupies any space
    }
    const Fx reach = largestRadius * 2;

    // One buffer for the whole pass. The grid's own answer buffer is overwritten by the next
    // query, so a copy is needed — and one copy that grows to the largest neighbourhood is the
    // whole allocation cost of the pass.
    std::vector<UnitIndex> neighbours;

    // ASCENDING SLOT ORDER, resolving each pair as it is found.
    //
    // This used to walk a `std::unordered_map` of buckets built here, every tick, and for each
    // unit it visited the nine surrounding cells in (dz, dx) order — so the ORDER pairs were
    // resolved in depended on how the cells happened to be laid out around each unit. The pass
    // accumulates pushes, so that order is part of the answer, and a comment here claimed the
    // result did not depend on the layout. It did.
    //
    // Now the store's index answers the query and returns slots in ascending order, so a pair
    // is resolved in slot order regardless of where the two units are on the map. That is a
    // different sequence from the old one — the match plays out differently, deliberately —
    // and it is the sequence the old comment was describing.
    for (std::size_t a = 0; a < count; ++a) {
        const Fx radiusA = motion[a].radiusElmos;
        if (radiusA <= Fx{}) {
            continue;
        }
        Transform& unitA = transforms[a];

        // Read into a local copy, because the next query overwrites the grid's answer buffer
        // and the loop below can trigger one — `fxHypot` does not, but a reader has no way to
        // know that from here, and a dangling span is not a bug worth leaving available.
        const std::span<const UnitIndex> near =
            store.space().within(unitA.x, unitA.z, reach);
        neighbours.assign(near.begin(), near.end());

        for (const UnitIndex b : neighbours) {
            // Each pair once, and never a unit against itself.
            if (b <= a) {
                continue;
            }
            if (b >= count) {
                continue;
            }

            const Fx radiusB = motion[b].radiusElmos;
            if (radiusB <= Fx{} || motion[a].airborne != motion[b].airborne
                || motion[a].surfaceWater != motion[b].surfaceWater) {
                continue;
            }
            Transform& unitB = transforms[b];

            Fx dxWorld = unitB.x - unitA.x;
            Fx dzWorld = unitB.z - unitA.z;
            const Fx wanted = radiusA + radiusB;
            Fx distance = fxHypot(dxWorld, dzWorld);

            if (distance >= wanted) {
                continue;
            }

            const bool movableA = motion[a].speedPerTick > Fx{};
            const bool movableB = motion[b].speedPerTick > Fx{};
            if (!movableA && !movableB) {
                continue;
            }

            // Exactly coincident, which a rally order produces the moment two units are given
            // the same destination. There is no direction to separate along, so one is invented
            // from the pair's indices — deterministic, and different for each pair, so a stack
            // fans out instead of picking one axis and forming a line.
            if (distance <= Fx::fromRaw(4)) {
                const auto angle = static_cast<Brad>((a * 7919 + b * 104729) % 65536);
                dxWorld = fxCos(angle);
                dzWorld = fxSin(angle);
                distance = kFxOne;
            }

            // Mobile peers share the overlap. An immobile structure is a fixed obstacle, so the
            // mobile unit takes the whole displacement instead of sliding the building away.
            const Fx overlap = wanted - distance;
            const Fx pushA = movableA ? (movableB ? overlap * Fx::fromRatio(1, 2) : overlap)
                                      : Fx{};
            const Fx pushB = movableB ? (movableA ? overlap * Fx::fromRatio(1, 2) : overlap)
                                      : Fx{};
            const Fx nx = dxWorld / distance;
            const Fx nz = dzWorld / distance;

            const auto siteAllowed = [&](std::size_t slot, Fx x, Fx z) {
                if (motion[slot].airborne) {
                    return true;
                }
                const auto type = static_cast<std::size_t>(
                    store.typeAt(static_cast<UnitIndex>(slot)));
                const PassabilityGrid* grid =
                    type < gridForType.size() ? gridForType[type] : nullptr;
                return grid == nullptr
                    || sitePlaceable(*grid, x, z, motion[slot].radiusElmos);
            };

            const Fx oldAX = unitA.x;
            const Fx oldAZ = unitA.z;
            unitA.x -= nx * pushA;
            unitA.z -= nz * pushA;
            if (!siteAllowed(a, unitA.x, unitA.z)) {
                unitA.x = oldAX;
                unitA.z = oldAZ;
            }

            const Fx oldBX = unitB.x;
            const Fx oldBZ = unitB.z;
            unitB.x += nx * pushB;
            unitB.z += nz * pushB;
            if (!siteAllowed(b, unitB.x, unitB.z)) {
                unitB.x = oldBX;
                unitB.z = oldBZ;
            }
        }
    }

    // Put everyone back on the map and on its movement layer. Done once at the end rather than per
    // push, since a unit may be moved by several neighbours.
    const Fx width = Fx::fromInt(terrain.field().squaresX * kSquareSize);
    const Fx depth = Fx::fromInt(terrain.field().squaresZ * kSquareSize);
    for (std::size_t i = 0; i < count; ++i) {
        if (motion[i].radiusElmos <= Fx{}) {
            continue;
        }
        Transform& unit = transforms[i];
        unit.x = std::clamp(unit.x, Fx{}, width);
        unit.z = std::clamp(unit.z, Fx{}, depth);
        if (motion[i].airborne || motion[i].surfaceWater) {
            placeOnMotionLayer(unit, motion[i], terrain);
        } else {
            unit.y = terrain.heightAt(unit.x, unit.z);
        }
    }
}

} // namespace rm::sim
