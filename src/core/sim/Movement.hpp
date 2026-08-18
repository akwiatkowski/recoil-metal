#pragma once

#include "core/map/HeightField.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <span>

namespace rm::sim {

// The first thing in this engine whose state changes between frames.
//
// It is movement and nothing else: units walk straight lines to where they are
// told, and pass through each other and through cliffs on the way. Pathfinding
// and collision are the sim proper, which the README deliberately declines to
// plan until the renderer has earned it. What lives here is the smallest thing
// that makes an order visible on screen.

/// Simulation ticks per second.
///
/// 30, matching Recoil's own `GAME_SPEED` (rts/Sim/Misc/GlobalConstants.h:52).
/// Worth matching exactly rather than picking something convenient: every unit
/// speed and turn rate in the content is authored against this rate, so any
/// other value silently rescales the whole game's movement.
inline constexpr int kTicksPerSecond = 30;
inline constexpr float kTickSeconds = 1.0f / static_cast<float>(kTicksPerSecond);

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
/// Four elmos is half a heightmap square — the finest the terrain itself
/// resolves, so aiming tighter than this asks for precision the ground does not
/// have. It also has to exceed one tick of travel or a unit steps past the goal
/// and turns back forever; the tick guards that case explicitly rather than
/// relying on this constant to cover every speed.
inline constexpr float kArrivalRadiusElmos = 4.0f;

// What a unit is doing. UnitInstance carries what the GPU needs — position,
// yaw, scale, colour — and this carries what the sim needs and the GPU must
// never see. Kept as a parallel array rather than folded into UnitInstance
// because that struct's layout is pinned by a static_assert and read verbatim
// by the vertex shader.
struct MoveState {
    float destinationX = 0.0f;
    float destinationZ = 0.0f;
    bool moving = false;

    float speedElmosPerSecond = kDefaultSpeedElmosPerSecond;
    float turnRateRadiansPerSecond = kDefaultTurnRateRadiansPerSecond;
};

/// Orders a unit to a world position, clamped onto the map.
///
/// Clamped here rather than in the tick so that an order is a fact about a
/// reachable place the moment it is given — a destination off the map would
/// otherwise leave a unit pressed against the border with `moving` stuck true
/// forever.
void orderTo(MoveState& state, const HeightField& field, float x, float z) noexcept;

/// Advances every unit by one fixed tick.
///
/// The two spans are parallel: instance i is driven by motion i. A short motion
/// span leaves the trailing instances alone rather than reading past its end.
///
/// noexcept and allocation-free: this runs inside the frame loop.
void tick(std::span<UnitInstance> instances, std::span<MoveState> motion,
          const HeightField& field) noexcept;

// Turns elapsed wall-clock time into whole fixed ticks.
//
// The sim runs at a fixed rate and the display does not — 60 Hz, 120 Hz, or
// whatever a frame took while a window was being dragged. Stepping by the frame
// delta instead would make the result depend on the frame rate, which is the
// difference between a screenshot that reproduces and one that does not.
class TickClock {
public:
    /// Banks `seconds` of wall time and returns how many ticks to run now.
    [[nodiscard]] int advance(float seconds) noexcept;

    /// Ticks a single advance will ever ask for.
    ///
    /// Without a cap, a stall — a breakpoint, a slow asset load, a closed lid —
    /// hands over minutes of banked time and the frame loop tries to catch up
    /// thousands of ticks at once, which reads as a hang. Losing time after a
    /// stall is the better failure. Five is a tenth of a second of catch-up per
    /// frame: enough to ride out ordinary hitches, far too few to spiral.
    static constexpr int kMaxTicksPerAdvance = 5;

private:
    float unspentSeconds_ = 0.0f;
};

} // namespace rm::sim
