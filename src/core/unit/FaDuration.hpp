#pragma once

#include "core/sim/TickRate.hpp"

namespace rm::unitdef {

// What a Forged Alliance duration ACTUALLY lasts — the +1-tick bias, corrected once at import.
//
// WHY THIS EXISTS (PLAN2.md §7 P3.5, from `11-fa-sim-layer.md §2.1`). FA's sim runs at 10 Hz and
// every Lua-timed duration in it goes through one function, `lua/simInit.lua:33-39`:
//
//     function WaitSeconds(n)
//         if n <= 0.1 then
//             WaitTicks(1)
//             return
//         end
//         WaitTicks(n * 10 + 1)
//     end
//
// `WaitTicks` is `coroutine.yield` (`:30`), so that `+ 1` is not a rounding convenience — it is
// a whole extra tick. **`WaitSeconds(1.0)` waits 1.1 seconds.** Every Lua-timed duration in the
// game is therefore *quantised to 100 ms and inflated by 100 ms*, and the balance was tuned
// against the inflated figure rather than against the number in the file.
//
// So a converter that reads `MuzzleSalvoDelay = 0.2` and emits 0.2 seconds is systematically
// 100 ms fast — for this field, 33% fast. The report calls it "a correctness item for FAR that
// no report has flagged", and puts the cost at *"every weapon's DPS is wrong by up to 50% on
// burst weapons"*.
//
// THE PART THAT IS EASY TO GET WRONG, and worth stating before the function: **this does not
// apply to everything with a duration in it.** `11 §3.6` splits the weapon fields by who
// enforces them, and the split is not intuitive:
//
//   - **Engine-timed, NOT corrected:** `RateOfFire` (`engine/Sim/UnitWeapon.lua:150`), firing
//     tolerance, turret arcs and speeds, target priorities. The engine's own clock runs these
//     with exact frame arithmetic and no Lua coroutine is involved.
//   - **Lua-timed, corrected:** `MuzzleSalvoDelay`, `MuzzleChargeDelay`, `RackSalvoChargeTime`,
//     `RackSalvoReloadTime`, `RackReloadTimeout`, shield recharge, build-effect timings.
//
// `RateOfFire` being on the *engine* side is the counter-intuitive half. A reading that
// "corrects" it would make every weapon in the game 100 ms slow — the same bug this file fixes,
// with the sign flipped and applied to the field that matters most.
//
// AND IT IS RATE-INDEPENDENT. This is a fact about how FA's numbers were AUTHORED, not about how
// fast our sim ticks: the content was balanced against a 10 Hz Lua coroutine that no longer
// exists here. So the correction happens once, at import, in seconds — before `TickRate` ever
// sees the value, and identically at 5, 10, 20 and 50 Hz.

/// The duration FA's `WaitSeconds` actually waits, given the figure a blueprint states.
///
/// Faithful to `simInit.lua` including its edge: `WaitSeconds(0)` really does wait one tick, so
/// this returns 0.1 for zero. A field the game guards with `if x > 0` — `MuzzleSalvoDelay` is
/// guarded at `DefaultProjectileWeapon.lua:1130` — must therefore be checked by the CALLER
/// before being corrected, because for those a stated zero means "no wait happens at all"
/// rather than "a wait of zero". Putting that guard in here instead would be wrong for every
/// unguarded field, and there are more of those.
///
/// A pure function on `Seconds`, so it composes ahead of `TickRate::ticks` and cannot be
/// confused with a tick count (§5.1).
[[nodiscard]] sim::Seconds faWaitSeconds(sim::Seconds authored) noexcept;

} // namespace rm::unitdef
