#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"  // kNoArmy
#include "core/sim/Fx.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/Transform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

class UnitStore;
struct PassabilityGrid;

// The first thing in this engine whose state changes between frames.
//
// Units follow a route (core/sim/Pathfinding.hpp), turn at a bounded rate, hug
// the terrain, and push each other apart on arrival. What is still absent is
// everything that makes a game of it: no combat, no economy, no orders beyond
// "go there", and no reaction to being blocked — a unit whose route is occupied
// leans on whoever is in the way rather than re-routing.

/// The DEFAULT simulation rate, in ticks per second. **Not the rate** — that is `TickRate`,
/// and it is configurable from 5 to 50 Hz (PLAN2.md §5.1, D2).
///
/// TEN, because the content this engine imports is Supreme Commander-authored and its own
/// scripts are written against that clock: `WaitSeconds(n)` is `WaitTicks(n * 10)`
/// (`mohodata/lua/simInit.lua:37`). Recoil's `GAME_SPEED` is 30
/// (`rts/Sim/Misc/GlobalConstants.h:52`); the two games disagree, and a default has to be
/// one of them.
///
/// **THIS COMMENT USED TO ARGUE SOMETHING THE PROJECT HAS SINCE DECIDED AGAINST**, and the
/// correction is left visible rather than quietly deleted because the old argument is
/// persuasive and someone will reconstruct it. It said 10 Hz was chosen so that "Supreme
/// Commander's own gameplay scripts could later run on this sim unmodified", and that "this
/// is not a constant to change later".
///
/// Both halves are now wrong. **Running anyone's scripts is a stated non-goal** (§1.1: "you
/// promise import, not compatibility" — no existing Lua runs, and D6 puts unit behaviour in
/// C++ with no script host at all). And **it is exactly a constant that changes**: D2 made
/// the rate a value with a `--tick-rate` flag and a four-rate test behind it, which is what
/// retired the argument. PLAN2 §1.1 names this file as the live justification it was
/// retiring.
///
/// What survives is the part that was always true and is the reason a default is safe at
/// all: **both content families author speeds PER SECOND**, so changing the rate rescales
/// nothing. The one exception was a `turnrate` in Recoil frames — a fact about BAR's
/// authoring rather than about our clock — which now has its own constant in `UnitDef.cpp`.
///
/// A caller wanting the rate should take a `TickRate`. This is here for the handful that
/// convert a per-tick figure back for display.
inline constexpr int kTicksPerSecond = 10;
// `kTickSeconds` used to be here. It is GONE, which is PLAN2.md §5.1's "kTickSeconds leaves
// sim math" — the last sim caller disappeared when the economy went per-tick and the movement
// pass took per-tick speeds. What the renderer and the HUD clock want is
// `TickRate::secondsPerTick()`, which says whose concern it is in its name.
//
// `kTicksPerSecond` above survives as the DEFAULT rate's value, for the handful of callers
// that convert a per-tick figure back for display. It is a rate, not a duration in ticks, so
// `tools/check_no_tick_literals.sh` exempts it.

/// Default ground speed, in elmos per second.
///
/// 87 is BAR's Pawn (units/ArmBots/armpw.lua:27), an ordinary light bot — a
/// representative number rather than a guessed one. Recoil's modern `speed`
/// field is already elmos/second; only the legacy `maxVelocity` is per frame
/// (rts/Sim/Units/UnitDef.cpp:442-443). Per-unit speeds belong to unit defs,
/// which this engine does not read yet.
inline constexpr float kDefaultSpeedElmosPerSecond = 87.0f;

/// Default turn rate, in radians per second.
///
/// The same Pawn's `turnrate` of 1214.4 (armpw.lua:31), converted out of
/// Recoil's units: turn rates are authored in circle divisions per frame, where
/// a full circle is SPRING_CIRCLE_DIVS = 65536 (rts/System/SpringMath.h:16-17).
/// So 1214.4 / 65536 * 2*pi * 30 = 3.49 rad/s, a little under 200 degrees a
/// second.
inline constexpr float kDefaultTurnRateRadiansPerSecond = 3.4934f;

/// How close counts as arrived, in elmos.
///
/// TWO requirements, and which one binds depends on the tick rate, so it is
/// written as the larger rather than as whichever happened to win.
///
/// Half a heightmap square, four elmos, is the finest the terrain itself
/// resolves, so aiming tighter asks for precision the ground does not have.
/// And it must exceed ONE TICK OF TRAVEL, or a unit steps past the goal every
/// tick and never lands inside the radius at all.
///
/// At 30 ticks a second the first bound won: a tick was 2.9 elmos and the
/// constant was a flat 4. At 10 it is 8.7, so the second binds, and hard-coding
/// the 4 would have left every ordered unit stopping on the tick's overshoot
/// guard instead of on arrival — a constant that had quietly stopped doing its
/// job. Exactly the kind of value PLAN.md warns grows up around a tick rate.
/// Now DERIVED, because it depends on the rate and §5.1 forbids writing that dependency as a
/// number. `arrivalRadius(rate)` is the larger of half a square and one tick of default travel.
[[nodiscard]] Fx arrivalRadius(TickRate rate) noexcept;

/// Default collision radius, in elmos — see MoveState::radiusElmos. Rate-independent, so it
/// stays a constant.
inline constexpr Fx kDefaultRadius = Fx::fromInt(16);

/// Simplified aircraft clearance above local terrain, in elmos.
///
/// Recoil's `AAirMoveType::wantedHeight` defaults to 80 (`AAirMoveType.h:75`). The real air
/// movers vary altitude, take off and land; this slice keeps that sourced cruise clearance
/// fixed, which is enough to cross terrain without inventing a flight model.
inline constexpr Fx kAirClearanceElmos = Fx::fromInt(80);

// What a unit is DOING, as opposed to where it is. `Transform` carries position and
// orientation; this carries the order, the derived rates and the bookkeeping.
//
// Two structs rather than one because they have different readers: every pass touches the
// transform, and only the movement and collision passes touch this. Keeping them apart is
// what lets the draw gather read one array and ignore the other.

struct MoveState {
    /// Who owns this unit. See kNoArmy.
    int armyIndex = kNoArmy;

    Fx destinationX{};
    Fx destinationZ{};
    bool moving = false;

    /// Attached units receive their transform from their parent rather than advancing their own
    /// movement path. Attachment does not remove them from collision or other sim passes.
    bool attached = false;

    /// This unit occupies the air movement layer. Derived from its immutable `UnitDef` when
    /// spawned; stored here because movement and collision are hot span passes with no catalog.
    bool airborne = false;

    /// This unit floats on the map's water plane. Mutually exclusive with `airborne`; surface
    /// ships route on the inverse water grid and do not inherit seabed height or slope.
    bool surfaceWater = false;

    /// PER TICK, both of them, derived once from the authored per-second figures (§5.1).
    ///
    /// They default to ZERO rather than to the constants above, and that is deliberate: a
    /// per-tick default would have to name a rate, which is the one thing the rule forbids.
    /// `defaultMotion(rate)` builds a `MoveState` with the authored defaults converted, and is
    /// what a caller that wants "an ordinary unit" should use.
    Fx speedPerTick{};

    /// Binary radians per tick. SIGNED and wider than `Brad`: a rate fast enough to wrap a
    /// full turn in one tick would be indistinguishable from standing still in the wrapping
    /// type, and clamping the turn against the remaining error needs the true magnitude.
    std::int32_t turnPerTick = 0;

    /// How much room this unit takes up, in elmos. Zero opts out of collision
    /// entirely, which is what a marker or a decorative instance wants.
    ///
    /// The default is BAR's Pawn again: `footprintx = 2`, doubled by the
    /// engine's footprint scale to 4 squares, half of which is 16 elmos.
    Fx radiusElmos = kDefaultRadius;

    // Ground distance covered since this unit was created, in elmos. Only ever
    // increases.
    //
    // This is what a walk cycle should be driven by. Wall time cannot do the
    // job: a unit pivoting on the spot or standing still would keep striding,
    // which is the same foot-sliding artefact as a mismatched playback rate,
    // just more obvious. Distance also handles the cases nothing else does —
    // slowing into a turn, stopping on arrival — without any special casing,
    // because a unit that covers no ground advances no legs.
    //
    // Kept here rather than derived from position because a straight line from
    // the spawn is not the distance walked: a unit that goes out and comes back
    // has covered twice what its displacement says.
    Fx distanceTravelledElmos{};

    // The route still to walk, as world (x, z) waypoints, and how far along it
    // the unit is. Empty for a unit heading straight at a point.
    //
    // Held per unit rather than shared because two units ordered to the same
    // place start from different corners of the map. A vector per unit is
    // cheap here — this never reaches the GPU, and the instance data that does
    // stays exactly as tightly packed as it was.
    std::vector<std::array<Fx, 2>> path;
    std::size_t pathIndex = 0;

    /// The accepted route's start cell fixes C-177's staggered revalidation phase. A zero width
    /// means this motion was not admitted through the match-owned path service.
    int pathPhaseStartX = 0;
    int pathPhaseStartZ = 0;
    int pathPhaseCellsX = 0;

    // --- winged flight (`C-221`) ------------------------------------------------
    //
    // Retail's air mover is a disjoint path keyed on the blueprint, not a variant of the
    // ground one — and ours is too, gated on `canFly` below. Ground units carry these
    // fields at zero forever; they hash to nothing (the hash skips non-flyers) and save
    // through the v11 air sidecar rather than the motion record.

    /// Whether this unit may leave the ground. Set once at spawn from the definition;
    /// the movement pass has no catalog, so everything authored it needs arrives here.
    bool canFly = false;

    /// Retail's vertical events (`C-222`/`C-223`): Bottom = on the ground, Up = climbing
    /// out, Top = cruising, Down = coming in. Hover belongs to transports and is deferred.
    /// Spawned airborne (the status quo ante); landing and takeoff move it from there.
    enum class AirState : std::uint8_t { Bottom, Up, Top, Down };
    AirState airState = AirState::Bottom;

    /// Elmos per SECOND, all three axes. Retail's trapezoid (`C-221`) only balances in
    /// per-second units — per-tick velocity would fly every leg at a tenth of its
    /// authored speed — so the desired vector below is built from the per-second cruise
    /// speed, never from `speedPerTick`.
    std::array<Fx, 3> velocity{};

    /// The altitude the lift law is chasing, in elmos. Slews toward terrain plus clearance
    /// at `airLift × 0.1` per tick upward, half that downward (`C-221`).
    Fx altitudeRef{};

    /// Remaining fuel as a 0..1 ratio. Drains `1 / (FuelUseTime × 10)` per tick while
    /// climbing, cruising or descending and refills at the same rate on the ground
    /// (`C-223`); clamped at both ends, with no native consequence at zero — every
    /// consequence retail has is Lua. Spawned full.
    Fx fuelRatio = Fx::fromInt(1);

    /// Idle ticks since this flyer last had an order. Past `idleLandThreshold` a cruising
    /// flyer commits to landing (`floor(AutoLandTime × 10)` — `C-222`'s auto-land timer).
    std::uint32_t idleTicks = 0;

    /// Ticks of idleness that trigger auto-land. `AutoLandTime <= 0` reads as never:
    /// a zero threshold would ground everything the tick after spawn. Open edge, noted.
    std::uint32_t idleLandThreshold = std::numeric_limits<std::uint32_t>::max();

    /// Per-tick proportional approach rates, converted once at spawn from the authored
    /// `Air.KMove` / `Air.KLift` (`C-221`). The exact gain schedule is unread — these stand
    /// in for it, and the ledger says so.
    Fx airApproachGain{};
    Fx airLiftGain{};

    /// The climb authority (`Air.LiftFactor`, `C-221`) in elmos per SECOND, matching the
    /// velocity state below. Below half max airspeed the lift cap goes negative and the
    /// aircraft cannot climb at all.
    Fx airLiftFactor{};

    /// Cruise speed in elmos per SECOND (`Air.MaxAirspeed`, converted once at spawn).
    /// Separate from `speedPerTick` because the winged integrator's velocity is
    /// per-second (`C-221`'s trapezoid only balances in those units) while the waypoint
    /// logic thinks per tick.
    Fx airMaxSpeedElmosPerSec{};

    /// per-second (`C-221`'s trapezoid only balances in those units) while the waypoint
    /// logic thinks per tick.

    /// Fuel ratio spent per tick while climbing, cruising or descending
    /// (`1 / (FuelUseTime × 10)`, `C-223`), converted once at spawn.
    Fx fuelDrainPerTick{};
};

/// Sends a unit along a route, aiming it at the first waypoint.
///
/// An empty path stops the unit rather than leaving it heading wherever it was:
/// "no route exists" and "walk to where you already are" are the same answer,
/// and both mean stay put.
void orderAlongPath(MoveState& state, std::span<const std::array<Fx, 2>> path);

// `CollisionGroup` used to live here, with a multi-group overload of `resolveCollisions`:
// instances were held per model, one array each, so separating a mixed crowd meant handing
// the pass every batch at once. The store is one flat array, so the single-span form below is
// the whole story.

/// Pushes overlapping units apart, once.
///
/// Separate from `tick` on purpose, and for two reasons. Collision is the only
/// pairwise thing here — every other rule is per unit — and it is the only part
/// that needs scratch space, so keeping it out leaves `tick` noexcept and
/// allocation-free. Callers run it after ticking.
///
/// One pass relieves overlap rather than resolving it: a pile settles over
/// several ticks, which is both cheaper and steadier than solving a crowd
/// exactly and having it explode apart in a single frame. Units keep their
/// order, so the result is the same every run.
///
/// This is separation, not physics — no momentum, no friction, and a unit
/// being pushed does not push back on whatever is driving it. That is enough
/// to stop a rally point from being a stack of models in the same spot, which
/// is the visible lie it exists to fix.
///
/// TAKES THE STORE now, not two spans (§7 P5.2). It needs the store's spatial index — it used
/// to build a `std::unordered_map` of buckets of its own, every tick, with its own cell size —
/// and taking the store is what every other pass already does. The spans were the last
/// leftover of the batch era, when a pass was handed one array per instanced draw.
///
/// **The index must be current**: `UnitStore::reindex` before this, since the pass reads
/// positions that `tick` has just changed.
void resolveCollisions(
    UnitStore& store, const Terrain& terrain,
    std::span<const PassabilityGrid* const> gridForType = {});

/// How close counts as reaching an intermediate waypoint, in elmos.
///
/// Far looser than arriving: a waypoint is a hint about which way to go, not a
/// place to stand. Half a pathfinding cell lets a unit round a corner in a
/// smooth arc instead of driving to each cell centre and pivoting there, and it
/// costs nothing in accuracy because the FINAL waypoint still uses the tight
/// radius.
inline constexpr Fx kWaypointRadius = Fx::fromInt(32);

/// Pitch (rotationX) and roll (rotationZ) that align a unit's up axis with the
/// terrain normal under its feet.
///
/// The unit's yaw is preserved: the slope is expressed in the unit's local
/// frame so a unit facing any direction plants both feet on the same slope.
/// Returns {rotationX, rotationZ} in radians.
[[nodiscard]] std::array<Brad, 2> slopeAlignment(const Terrain& terrain, Fx x, Fx z,
                                                 Brad yaw) noexcept;

/// Orders a unit to a world position, clamped onto the map.
///
/// Clamped here rather than in the tick so that an order is a fact about a
/// reachable place the moment it is given — a destination off the map would
/// otherwise leave a unit pressed against the border with `moving` stuck true
/// forever.
void orderTo(MoveState& state, const Terrain& terrain, Fx x, Fx z) noexcept;

/// Places one transform on its movement layer after X/Z changes. Ground units follow terrain
/// and its slope; aircraft keep fixed clearance and remain level.
void placeOnMotionLayer(Transform& transform, const MoveState& state,
                        const Terrain& terrain) noexcept;

/// Advances every unit by one fixed tick.
///
/// The two spans are parallel: instance i is driven by motion i. A short motion
/// span leaves the trailing instances alone rather than reading past its end.
///
/// noexcept and allocation-free: this runs inside the frame loop.
void tick(std::span<Transform> transforms, std::span<MoveState> motion,
          const Terrain& terrain) noexcept;

/// A `MoveState` with the authored default speed and turn rate, converted for this clock.
///
/// The replacement for defaulting those fields in the struct: the defaults are authored per
/// second (`kDefaultSpeedElmosPerSecond`, `kDefaultTurnRateRadiansPerSecond`) and only a rate
/// can turn them into per-tick amounts.
[[nodiscard]] MoveState defaultMotion(TickRate rate) noexcept;

// Turns elapsed wall-clock time into whole fixed ticks.
//
// The sim runs at a fixed rate and the display does not — 60 Hz, 120 Hz, or
// whatever a frame took while a window was being dragged. Stepping by the frame
// delta instead would make the result depend on the frame rate, which is the
// difference between a screenshot that reproduces and one that does not.
class TickClock {
public:
    /// The rate is a value the clock is built with, not a constant it reads. That is what
    /// lets the same code bridge a 5 Hz sim and a 50 Hz one — and what lets one process run
    /// both, which is P2.3's multi-rate test.
    explicit TickClock(TickRate rate) : rate_(rate) {}
    TickClock() = default;

    /// Banks `seconds` of wall time and returns how many ticks to run now.
    [[nodiscard]] int advance(float seconds) noexcept;

    /// The most catch-up a single advance will ever ask for, AS A DURATION.
    ///
    /// Without a cap, a stall — a breakpoint, a slow asset load, a closed lid — hands over
    /// minutes of banked time and the frame loop tries to catch up thousands of ticks at
    /// once, which reads as a hang. Losing time after a stall is the better failure.
    ///
    /// This was `kMaxTicksPerAdvance = 5`, which PLAN2 §5.1 names as one of the two constants
    /// the rule exists to delete: five ticks is half a second at 10 Hz and a tenth of one at
    /// 50 Hz, so raising the rate would silently have made the engine five times less
    /// tolerant of a hitch. Authored in seconds, the tolerance is the same however fast the
    /// sim runs. (The comment that used to sit here said "a tenth of a second", which was
    /// wrong about its own constant — five ticks at 10 Hz is half a second. Worth recording:
    /// the rate was in a comment, and the comment was mistaken.)
    static constexpr Seconds kMaxCatchUp = Seconds{0.5f};

    /// How many ticks that is, at this clock's rate.
    [[nodiscard]] TickCount maxTicksPerAdvance() const noexcept {
        return rate_.ticks(kMaxCatchUp);
    }

    /// How far into the next tick the banked time reaches, 0..1.
    ///
    /// THE INTERPOLATION ALPHA (§7 P7.2), and it was already being computed here — `advance`
    /// banks the remainder and this is what the remainder means. Exposing it is what lets the
    /// renderer draw between two snapshots instead of on top of the newer one; without it the
    /// frame loop would have to keep a second copy of the same accumulator and the two would
    /// drift.
    ///
    /// **Unsynced by construction**: it is wall time, which the sim never sees. A float here is
    /// correct rather than tolerated.
    [[nodiscard]] float alpha() const noexcept {
        const float perTick = rate_.secondsPerTick();
        return perTick > 0.0f ? std::clamp(unspentSeconds_ / perTick, 0.0f, 1.0f) : 0.0f;
    }

private:
    TickRate rate_{};
    float unspentSeconds_ = 0.0f;
};

} // namespace rm::sim
