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

namespace {

// The vertical events of the winged mover (`C-222`/`C-223`): state transitions and fuel.
// Runs before the waypoint logic so a takeoff or landing commit takes effect the same tick
// the order (or its absence) arrives. The altitude reference slews inside the beat, where
// the look-ahead that feeds it is computed.
void tickAirState(MoveState& state) noexcept {
    using AirState = MoveState::AirState;
    // Recharge while grounded runs at the drain rate — its exact rate is open, and with
    // no native consequence at zero either choice is behaviourally identical today.
    if (state.airState == AirState::Bottom) {
        state.fuelRatio =
            std::min(Fx::fromInt(1), state.fuelRatio + state.fuelDrainPerTick);
        state.idleTicks = 0;
        if (!state.moving) {
            return;
        }
        // A fresh order commits to takeoff. `airborne` flips the first beat the lift law
        // raises the unit, which with retail's law (`C-245`) is this same tick: a slow
        // flyer below half its elevation climbs straight up before it moves forward.
        state.airState = AirState::Up;
        return;
    }
    if (state.moving) {
        state.idleTicks = 0;
        if (state.airState == AirState::Down) {
            state.airState = AirState::Up;
        }
    } else if (state.airState == AirState::Top) {
        // The auto-land timer (`floor(AutoLandTime × 10)` idle ticks, `C-222`).
        if (++state.idleTicks >= state.idleLandThreshold) {
            state.airState = AirState::Down;
            state.idleTicks = 0;
        }
    } else if (state.airState == AirState::Up) {
        // An order cancelled mid-climb comes back down rather than idling Up.
        state.airState = AirState::Down;
    }
    // Fuel drains whenever off the ground and the clamp is the whole native story
    // (`C-223`): no speed penalty, no crash, every consequence Lua.
    state.fuelRatio = std::max(Fx{}, state.fuelRatio - state.fuelDrainPerTick);
}

// The retail step and half-step (`C-221`: `dt = 0.1` at `0x00E4CEA4`, `0.05` at
// `0x00EA2BA0`) — the projectile's two constants, reused by the aircraft.
constexpr Fx kAirDt = Fx::fromRatio(1, 10);
constexpr Fx kTrapezoidHalfStep = Fx::fromRatio(1, 20);

/// How far ahead a flyer looks for terrain, in seconds of cruise (`C-246`: `5.0` at
/// `0x00E4D960`, times `MaxAirspeed` times the unit's speed multiplier, which is 1 here).
constexpr Fx kLookAheadSeconds = Fx::fromInt(5);

/// The look-ahead's second, nearer scan is weighed at `1.5` (`0x00E4DA50`) and the
/// horizontal hold-back it produces bottoms out at `0.2` (`0x00EA2C48`) (`C-246`).
constexpr Fx kNearScanWeight = Fx::fromRatio(3, 2);
constexpr Fx kLookAheadFloor = Fx::fromRatio(1, 5);

/// Below this fraction of cruise speed (`0.08` at `0x00EA2CBC`) a flyer under half its
/// elevation holds its horizontal desire back in proportion to how high it has got
/// (`C-245`): the vertical lift-off before the forward run.
constexpr Fx kTakeoffSpeedFraction = Fx::fromRatio(2, 25);

// One winged beat (`C-221`, `C-244`–`C-246`), velocity in elmos per SECOND — the only
// units the formula balances in:
//
//   desired  = towards the destination at min(distance, cruise), held back by the terrain
//              look-ahead and the takeoff gate
//   a_xz     = KMove × desired − damp(desired) × v
//   a_y      = KLift × lift − KLiftDamping × vy
//   v       += a × 0.1;   pos += (v_old + v_new) × 0.05
//
// Retail hands the controller's force to a physics body that also applies gravity; the
// controller subtracts the same gravity first, so the two cancel and only the
// acceleration above survives — which is what is integrated here. Touchdown and
// takeoff-completion are the caller's.
void integrateAir(Transform& unit, MoveState& state, const Terrain& terrain) noexcept {
    using AirState = MoveState::AirState;
    const Fx cruise = state.airMaxSpeedElmosPerSec;
    const bool landing = state.airState == AirState::Down;

    // --- desired horizontal velocity: at the destination, at cruise or the distance ---
    const Fx dx = state.destinationX - unit.x;
    const Fx dz = state.destinationZ - unit.z;
    const Fx distance = fxHypot(dx, dz);
    const Fx elevationRef = landing
        ? wingedLandingElevation(state.airElevation, state.airElevationAdjustment, distance)
        : state.airElevation + state.airElevationAdjustment;
    Fx desiredX{};
    Fx desiredZ{};
    // Retail keeps steering toward its landing site after the command has retired. Without
    // that approach the aircraft can stop inside the ordinary arrival radius but never enter
    // the final half-elmo stage that drops the elevation target to the deck.
    const bool approaching = state.moving || landing;
    if (approaching && distance > Fx{}) {
        const Fx speed = std::min(distance, cruise);
        desiredX = dx / distance * speed;
        desiredZ = dz / distance * speed;
    }

    // --- terrain look-ahead (`C-246`) ---
    //
    // The highest surface within five seconds of cruise (or the destination, if nearer)
    // is what the altitude reference chases. When the climb it demands exceeds a second
    // of lift authority, a nearer scan at half the reach decides how much horizontal
    // desire to give up so the aircraft can climb: the fraction of the half-reach left
    // after the nearer climb, floored, squared.
    // Idle, retail's target sits under the aircraft and the reach collapses to the point
    // sample; a stale destination must not choose the cell for a flyer going nowhere.
    const Fx reach = approaching ? std::min(cruise * kLookAheadSeconds, distance) : Fx{};
    const Fx aheadHeight = terrain.maxSurfaceHeightNear(unit.x, unit.z, reach);
    const Fx climbAhead = std::max(Fx{}, aheadHeight - unit.y);
    if (climbAhead > state.airLiftFactor && reach > Fx::fromInt(kSquareSize)) {
        const Fx halfReach = reach * Fx::fromRatio(1, 2);
        const Fx nearHeight =
            terrain.maxSurfaceHeightNear(unit.x, unit.z, halfReach) * kNearScanWeight;
        const Fx climbNear = std::max(Fx{}, nearHeight - unit.y);
        const Fx factor = std::max(kLookAheadFloor, (halfReach - climbNear) / halfReach);
        desiredX = desiredX * factor * factor;
        desiredZ = desiredZ * factor * factor;
    }

    // Outside landing, the reference slews at `LiftFactor × 0.1` per tick upward and half
    // that downward (`C-221`). Landing uses C-222's separate category-dependent clamp.
    const Fx target = aheadHeight + elevationRef;
    const Fx slewUp = state.airLiftFactor * kAirDt;
    if (target > state.altitudeRef) {
        state.altitudeRef = std::min(target, state.altitudeRef + slewUp);
    } else if (landing) {
        state.altitudeRef =
            wingedLandingReference(state.altitudeRef, target, state.airTransportation);
    } else {
        state.altitudeRef =
            std::max(target, state.altitudeRef - slewUp * Fx::fromRatio(1, 2));
    }

    // --- the lift law and the takeoff gate (`C-245`) ---
    const Fx surface = terrain.surfaceHeightAt(unit.x, unit.z);
    const Fx heightAbove = unit.y - surface;
    const std::array<Fx, 3> old = state.velocity;
    const Fx horizontalSpeed = fxHypot(old[0], old[2]);
    const Fx speedRatio = cruise > Fx{} ? horizontalSpeed / cruise : Fx{};
    const Fx lift = wingedLift(state.altitudeRef - unit.y, speedRatio, state.airLiftFactor,
                               heightAbove, elevationRef);
    // The gate reads the whole velocity, climb included: the lift-off itself is what
    // first carries a flyer past the threshold and frees its forward desire.
    const Fx speed = fxHypot(horizontalSpeed, old[1]);
    if (elevationRef > Fx{} && elevationRef * Fx::fromRatio(1, 2) > heightAbove
        && speed < cruise * kTakeoffSpeedFraction) {
        // Clamped at the deck: our reference slews slower than a flyer travels, so a
        // cliff can put the surface above the unit for a beat, and retail's unclamped
        // ratio would then REVERSE the desire. Holding still is the honest reading.
        const Fx gate =
            std::min(Fx::fromInt(1), std::max(Fx{}, heightAbove) / elevationRef);
        desiredX *= gate;
        desiredZ *= gate;
    }

    // --- the controller (`C-244`) and the trapezoid (`C-221`) ---
    const Fx desiredLength = fxHypot(fxHypot(desiredX, desiredZ), lift);
    const Fx damp = airDampingFactor(state.airKMove, state.airKMoveDamping, desiredLength);
    const Fx ax = state.airKMove * desiredX - damp * old[0];
    const Fx ay = state.airKLift * lift - state.airKLiftDamping * old[1];
    const Fx az = state.airKMove * desiredZ - damp * old[2];
    state.velocity[0] += ax * kAirDt;
    state.velocity[1] += ay * kAirDt;
    state.velocity[2] += az * kAirDt;
    unit.x += (old[0] + state.velocity[0]) * kTrapezoidHalfStep;
    unit.y += (old[1] + state.velocity[1]) * kTrapezoidHalfStep;
    unit.z += (old[2] + state.velocity[2]) * kTrapezoidHalfStep;
}

}  // namespace

Fx airDampingFactor(Fx kMove, Fx kMoveDamping, Fx desiredLength) noexcept {
    const Fx s = std::max(Fx::fromInt(1), std::min(desiredLength, kMove));
    if (kMove <= s) {
        return kMove;
    }
    return std::min(kMove / s, kMoveDamping);
}

Fx wingedLandingElevation(Fx elevation, Fx adjustment, Fx distanceToSite) noexcept {
    constexpr Fx kFinalApproach = Fx::fromRatio(1, 2);
    if (distanceToSite <= kFinalApproach) {
        return Fx{};
    }
    return (elevation + adjustment) * Fx::fromRatio(1, 2);
}

Fx wingedLandingReference(Fx current, Fx target, bool transportation) noexcept {
    const Fx error = target - current;
    if (error >= Fx{}) {
        return target;
    }
    const Fx floor = transportation ? -Fx::fromInt(3) : -Fx::fromRatio(1, 4);
    const Fx step = transportation ? error : error * Fx::fromRatio(1, 2);
    return current + std::max(step, floor);
}

Fx wingedLift(Fx need, Fx speedRatio, Fx liftFactor, Fx heightAbove, Fx elevationRef) noexcept {
    const Fx cap = (speedRatio - Fx::fromRatio(1, 2)) * liftFactor;
    if (cap > Fx{}) {
        return std::min(need, cap);
    }
    const Fx half = elevationRef * Fx::fromRatio(1, 2);
    if (half > heightAbove) {
        return half - heightAbove;
    }
    return cap;
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
        // The vertical events run for every flyer, grounded ones included: a parked
        // aircraft recharges (`C-223`) and a fresh order commits it to takeoff, both
        // before the idle skip below would otherwise pass it over.
        if (state.canFly && !state.attached) {
            tickAirState(state);
        }
        // A flyer off the ground stays in the tick after its orders end: it must land,
        // hold, or keep flying on its velocity rather than freeze mid-air.
        const bool flying =
            state.canFly && state.airState != MoveState::AirState::Bottom;
        if ((!state.moving && !flying) || state.attached) {
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

        // Whether this tick completes a journey, as opposed to loitering where a previous
        // one ended: only a fresh arrival commits a flyer to landing (`C-222`).
        const bool wasMoving = state.moving;
        // A flyer off the deck keeps flying through arrival waypoints: stopping dead
        // would freeze a descent (or a climb) the same tick it was committed, because
        // the `continue` below skips the air branch that performs it.
        const bool flyThrough =
            state.canFly && state.airborne && state.airState != MoveState::AirState::Bottom;
        if (distance <= std::max(radius, travel)) {
            if (!onFinalWaypoint) {
                ++state.pathIndex;
                state.destinationX = state.path[state.pathIndex][0];
                state.destinationZ = state.path[state.pathIndex][1];
                if (!flyThrough) {
                    continue;  // aim at the next one on the following tick
                }
            } else {
                state.moving = false;
                state.path.clear();
                state.pathIndex = 0;
                // Arrival is a landing commit for a flyer (`C-222`): it descends from here
                // rather than stopping dead at cruise altitude.
                if (wasMoving) {
                    state.airState = MoveState::AirState::Down;
                }
                if (!flyThrough) {
                    continue;
                }
            }
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

        if (state.canFly
            && (state.airborne || state.airState == MoveState::AirState::Up
                || state.airState == MoveState::AirState::Down)) {
            // Winged flight (`C-221`) instead of the ground stride below: explicit
            // velocity integrated trapezoidally, with the lift law owning the vertical.
            // A committed takeoff enters from the deck and a grounded descent still
            // enters so touchdown can complete it. The surface — water where the ground
            // is drowned — is what a flyer leaves and lands on (`C-222`).
            integrateAir(unit, state, terrain);
            unit.x = std::clamp(unit.x, Fx{}, width);
            unit.z = std::clamp(unit.z, Fx{}, depth);
            const Fx surface = terrain.surfaceHeightAt(unit.x, unit.z);
            if (!state.airborne && unit.y > surface) {
                state.airborne = true;
            } else if (!state.airborne) {
                unit.y = surface;
            }
            if (state.airState == MoveState::AirState::Up && unit.y >= state.altitudeRef) {
                unit.y = state.altitudeRef;
                state.airState = MoveState::AirState::Top;
            }
            if (state.airState == MoveState::AirState::Down && unit.y <= surface) {
                unit.y = surface;
                state.airState = MoveState::AirState::Bottom;
                state.airborne = false;
                state.moving = false;
                state.velocity = {};
                state.idleTicks = 0;
            }
            continue;
        }

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
        // A flyer off the deck owns its altitude AND attitude in the winged integrator
        // above — this pass would otherwise teleport every climb back to clearance
        // height at the end of the same tick that earned it.
        if (motion[i].canFly && motion[i].airborne) {
            continue;
        }
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
    // A flyer's altitude belongs to the winged integrator above, not to this pass —
    // assigning clearance here would teleport every climb back to level flight.
    // Non-flyer airborne units (synthetic test fixtures) keep the old behavior.
    if (state.airborne && !state.canFly) {
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
