#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"  // kNoArmy
#include "core/sim/Fx.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/Transform.hpp"
#include "core/map/HeightField.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

class UnitStore;

// The first thing in this engine whose state changes between frames.
//
// Units follow a route (core/sim/Pathfinding.hpp), turn at a bounded rate, hug
// the terrain, and push each other apart on arrival. What is still absent is
// everything that makes a game of it: no combat, no economy, no orders beyond
// "go there", and no reaction to being blocked — a unit whose route is occupied
// leans on whoever is in the way rather than re-routing.

/// Simulation ticks per second.
///
/// TEN, matching Supreme Commander, whose scripts hardcode it: `WaitSeconds(n)`
/// is `WaitTicks(n * 10)` (mohodata/lua/simInit.lua:37). This was 30 — Recoil's
/// `GAME_SPEED` (rts/Sim/Misc/GlobalConstants.h:52) — and the two games disagree,
/// so one of them had to be chosen.
///
/// WHY THE SLOWER ONE WINS. Nothing here needs 30: this engine has no lockstep
/// multiplayer, which is the constraint that fixed both games' rates in the first
/// place, and the renderer has never been coupled to the tick. What 10 buys is
/// that Supreme Commander's own gameplay scripts could later run on this sim
/// unmodified, and a 30 Hz sim would run every one of their timings three times
/// fast. That is not a constant to change later: by then every value tuned against
/// the rate would have to move with it, which is a far bigger job than this line.
/// See PLAN.md, "Designing for a Lua host that does not exist yet".
///
/// Content is unaffected either way, and this is the part worth being careful
/// about: both families author speeds in units PER SECOND, so nothing is rescaled
/// by the change. The one exception was a `turnrate` in Recoil frames, which is a
/// fact about BAR's authoring rather than about our rate and now has its own
/// constant in UnitDef.cpp — using this one made two different facts share a
/// number, which held only while the values agreed.
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
void resolveCollisions(UnitStore& store, const Terrain& terrain);

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

private:
    TickRate rate_{};
    float unspentSeconds_ = 0.0f;
};

} // namespace rm::sim
