#pragma once

#include "core/unit/UnitDef.hpp"

#include <optional>
#include <string_view>

namespace rm::data {

// What ground a MOTION CLASS can cross — not what a unitdef says about placing a building.
//
// WHY THIS EXISTS, AND WHAT IT CORRECTS (PLAN2.md §7 P3.4). Passability was keyed on
// `UnitDef::maxSlopeDegrees` and `UnitDef::maxWaterDepthElmos`, read straight off the blueprint.
// Two cited reports say that is the wrong field:
//
//   `01-unitdefs-movedefs.md` §3.2, on `maxSlope`:  "**buildings only**: converted to
//   `maxHeightDif = 40 * tan(rad)`, the terraform/placement tolerance. Mobile ground units take
//   slope from the **MoveDef**" (`UnitDef.cpp:423-427`, `GameHelper.cpp:1211,1633-1635`).
//
//   And on `minWaterDepth`/`maxWaterDepth`: "**buildings only** for placement. Mobile ground
//   units use MoveDef depth" (`UnitDef.cpp:429-430`, `GameHelper.cpp:1602-1620`).
//
//   `recoil-engine-map.md` §5 states it as a gotcha: "for mobile units the MoveDef overrides the
//   unitdef's `maxSlope`/`maxWaterDepth` entirely — those apply only to building placement".
//
// So the old keying asked a building-placement question and used the answer for routing. It
// happened to work because the corpus's structures state limits and its mobile units mostly do
// not — a mobile unit with no stated depth got `kDefaultMaxWaterDepthElmos`, which is a guess
// standing in for a fact that was available all along.
//
// `ADR-027` already says passability comes from motion class. This makes it true.
//
// THE NUMBERS ARE RECOIL'S OWN DEFAULTS, cited from `01 §7`'s MoveDef table:
//   - `maxSlope`: **60°** for Tank and KBot, **15°** for Hover (`MoveDefHandler.cpp:84-95`,
//     `:233,238`), clamped to [0,60].
//   - `maxWaterDepth`: **1e6** — effectively unlimited — for Tank, KBot and Hover
//     (`MoveDefHandler.cpp:217`, `:232,237`).
//
// Recoil's Tank default of "any depth" is not what Supreme Commander means by a tank, so the
// mapping below is OURS where the two games disagree, and says so per line. That is D8: copy the
// functionality, not the numbers, when the numbers describe a different game.

/// What one motion class can cross.
struct MoveDef {
    /// The steepest ground it will climb, in degrees.
    float maxSlopeDegrees = 0.0f;

    /// The deepest water it will enter, in elmos. Zero for something that will not get wet.
    float maxWaterDepthElmos = 0.0f;

    /// Whether the ground grid is the right tool at all. False for air, ships and submarines —
    /// the first needs no grid and the last two need its inverse, water deep enough rather than
    /// shallow enough.
    bool usesGroundGrid = true;
};

/// The MoveDef for a motion class.
///
/// A FUNCTION over a closed enum rather than a table loaded from data, and that is a deliberate
/// stopping point: `MotionType` comes from the corpus's own `RULEUMT_*` strings, so the set of
/// classes is content-defined and already read from content. What each class can cross is a
/// small, stable table that has never needed to vary per mod — and making it a data file would
/// add a load path, a schema and a failure mode for eight rows. If a mod ever needs its own, this
/// is one function to redirect.
[[nodiscard]] MoveDef moveDefFor(unitdef::MotionType motion) noexcept;

/// The MoveDef a UNIT routes by.
///
/// THE CORRECTION, in one function. A mobile unit gets its class's limits and its own
/// `maxSlopeDegrees`/`maxWaterDepthElmos` are ignored — those govern where a BUILDING may be
/// placed. An immobile one has no MoveDef at all, and asking for one is a caller bug: nothing
/// routes a building.
[[nodiscard]] MoveDef moveDefFor(const unitdef::UnitDef& def) noexcept;

/// Whether this unit can be in water at all — what the manual check in §7 P3.4 walks across a bay.
///
/// All four factions' ACUs are `RULEUMT_Amphibious` (`14-blueprint-census.md §9.1`), so
/// amphibious movement is a fixed cost of playing the game rather than a later feature: the very
/// first unit every match spawns needs it.
[[nodiscard]] bool canCrossWater(const unitdef::UnitDef& def) noexcept;

} // namespace rm::data
