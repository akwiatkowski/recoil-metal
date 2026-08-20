#include "core/sim/Movement.hpp"

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
        if (!state.moving) {
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

        // Sit on the ground. Interpolated, so crossing a square does not pop.
        unit.y = terrain.heightAt(unit.x, unit.z);
    }

    // Tilt every unit onto the ground underneath it — NOT just the ones that
    // moved. A scene of scattered units has ordered none of them, and gating
    // this on movement would leave all of them sticking out horizontally on
    // their hillsides, which is the whole thing alignment exists to fix.
    //
    // A separate pass rather than a line in the loop above, because it applies
    // to a different set: the loop moves what is moving, this tilts everything.
    for (std::size_t i = 0; i < count; ++i) {
        Transform& unit = transforms[i];
        const std::array<Brad, 2> align = slopeAlignment(terrain, unit.x, unit.z, unit.heading);
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

void resolveCollisions(std::span<Transform> transforms, std::span<const MoveState> motion,
                       const Terrain& terrain) {
    // One flat index space, which is now what the caller hands over rather than something
    // assembled here: a unit's neighbours are all the units near it, not all the units near
    // it OF THE SAME MODEL. There used to be a `CollisionGroup` overload taking one span per
    // batch and flattening them, because storage was per model — with a flat store there is
    // nothing to flatten.
    struct Entry {
        Transform* unit;
        Fx radius;
    };

    std::vector<Entry> units;
    {
        const std::size_t n = std::min(transforms.size(), motion.size());
        units.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            units.push_back(Entry{&transforms[i], motion[i].radiusElmos});
        }
    }

    const std::size_t count = units.size();
    if (count < 2) {
        return;
    }

    // A uniform grid over the map, so a unit only tests the neighbours that
    // could actually reach it. Without one this is every pair against every
    // other, which is fine for the forty units a demo places and quadratic for
    // the hundreds a rally order gathers.
    Fx largestRadius{};
    for (std::size_t i = 0; i < count; ++i) {
        largestRadius = std::max(largestRadius, units[i].radius);
    }
    if (largestRadius <= Fx{}) {
        return;  // nothing here occupies any space
    }

    // Cells two radii across: any pair that overlaps is then either in the same
    // cell or in touching ones, so eight neighbours is the whole search.
    const Fx cellSize = largestRadius * 2;
    const auto cellOf = [cellSize](Fx value) { return (value / cellSize).floorToInt(); };

    // Keyed on the packed cell coordinates. A map rather than a dense grid
    // because a crowd occupies a handful of cells out of the tens of thousands
    // a map has, and the dense version would cost more to clear than to search.
    std::unordered_map<std::int64_t, std::vector<std::size_t>> buckets;
    buckets.reserve(count);

    const auto key = [](int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    };

    for (std::size_t i = 0; i < count; ++i) {
        if (units[i].radius <= Fx{}) {
            continue;
        }
        buckets[key(cellOf(units[i].unit->x), cellOf(units[i].unit->z))].push_back(i);
    }

    // Ascending index order, resolving each pair as it is found, so the result
    // does not depend on how the buckets happened to be laid out.
    for (std::size_t a = 0; a < count; ++a) {
        const Fx radiusA = units[a].radius;
        if (radiusA <= Fx{}) {
            continue;
        }
        Transform& unitA = *units[a].unit;

        const int cx = cellOf(unitA.x);
        const int cz = cellOf(unitA.z);

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

                    const Fx radiusB = units[b].radius;
                    if (radiusB <= Fx{}) {
                        continue;
                    }
                    Transform& unitB = *units[b].unit;

                    Fx dxWorld = unitB.x - unitA.x;
                    Fx dzWorld = unitB.z - unitA.z;
                    const Fx wanted = radiusA + radiusB;
                    Fx distance = fxHypot(dxWorld, dzWorld);

                    if (distance >= wanted) {
                        continue;
                    }

                    // Exactly coincident, which a rally order produces the
                    // moment two units are given the same destination. There is
                    // no direction to separate along, so one is invented from
                    // the pair's indices — deterministic, and different for
                    // each pair, so a stack fans out instead of picking one
                    // axis and forming a line.
                    if (distance <= Fx::fromRaw(4)) {
                        // The invented direction is now in binary radians directly, which is
                        // both simpler and finer than the old 360 whole degrees: the spread
                        // covers the full circle at the angle type's own resolution.
                        const auto angle = static_cast<Brad>((a * 7919 + b * 104729) % 65536);
                        dxWorld = fxCos(angle);
                        dzWorld = fxSin(angle);
                        distance = kFxOne;
                    }

                    // Half the overlap each: neither unit outranks the other,
                    // and moving only one would let a unit under orders shove
                    // its way through a crowd untouched.
                    const Fx push = (wanted - distance) * Fx::fromRatio(1, 2);
                    const Fx nx = dxWorld / distance;
                    const Fx nz = dzWorld / distance;

                    unitA.x -= nx * push;
                    unitA.z -= nz * push;
                    unitB.x += nx * push;
                    unitB.z += nz * push;
                }
            }
        }
    }

    // Put everyone back on the map and on the ground. Done once at the end
    // rather than per push, since a unit may be moved by several neighbours.
    const Fx width = Fx::fromInt(terrain.field().squaresX * kSquareSize);
    const Fx depth = Fx::fromInt(terrain.field().squaresZ * kSquareSize);
    for (std::size_t i = 0; i < count; ++i) {
        if (units[i].radius <= Fx{}) {
            continue;
        }
        Transform& unit = *units[i].unit;
        unit.x = std::clamp(unit.x, Fx{}, width);
        unit.z = std::clamp(unit.z, Fx{}, depth);
        unit.y = terrain.heightAt(unit.x, unit.z);
    }
}

} // namespace rm::sim
