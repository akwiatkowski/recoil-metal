#pragma once

#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/TickRate.hpp"

#include <array>
#include <cstddef>

namespace rm::sim {

// Veterancy: what a unit earns by killing things, and what that is worth.
//
// WHY THESE EXACT NUMBERS. Every constant and every rule below is read off the retail
// Forged Alliance corpus rather than invented or copied from FAF, which rewrote this
// system wholesale. The evidence is recorded as claims `C-024`, `C-028` and `C-029` in
// `docs/fa-exe-analysis-plan.md`; the sources are the shipped
// `lua/sim/Unit.lua` (`AddKills`, `CheckVeteranLevel`, `SetVeteranLevel`, `OnKilledUnit`),
// `lua/game.lua:11-17` (`VeteranDefault`), `lua/sim/BuffDefinitions.lua`
// (`VeterancyHealth1-5`, `VeterancyRegen1-5`) and `lua/sim/Buff.lua` (`BuffCalculate` and
// the `MaxHealth`/`Regen` arms of `ApplyBuff`).
//
// THREE THINGS RETAIL DOES THAT ARE EASY TO GET WRONG, and which the tests pin:
//
//   1. **Levels do not compound.** Retail recomputes every buff from the BLUEPRINT base
//      value, and the veterancy buffs declare `Stacks = 'REPLACE'`, so only the current
//      level's buff is ever applied. Level 3 health is `base x 1.3`. It is not
//      `base x 1.1 x 1.2 x 1.3`, which would be 1.716 and is what an incremental
//      implementation produces.
//   2. **Promotion heals.** `Buff.lua` raises the maximum and then, absent `DoNoFill`,
//      calls `AdjustHealth(unit, val - oldmax)` — so a unit gains the full increase as
//      current health too. Veterancy is a mid-fight survivability spike, not a bigger bar
//      that fills later.
//   3. **Weapon damage does not change.** Retail's per-weapon veterancy buff is present but
//      COMMENTED OUT in `Unit.lua`, under `# TODO: Enable per weapon buffs again`. A
//      veteran hits no harder; it only lives longer. Anyone porting FAF behaviour will
//      assume otherwise.

/// A unit's earned veterancy. Small and trivially copyable — the state hash walks it.
struct Veterancy {
    /// Cumulative confirmed kills. Retail keeps this as the unit's `KILLS` stat.
    int kills = 0;

    /// 0 through `kMaxVeteranLevel`.
    int level = 0;
};

/// Retail FA has five levels (`table.getsize` over `VeteranDefault`).
inline constexpr int kMaxVeteranLevel = 5;

/// Kills needed to REACH each level. Index `i` holds the threshold for level `i + 1`.
using VeterancyThresholds = std::array<int, kMaxVeteranLevel>;

/// `Game.VeteranDefault` (`lua/game.lua:11-17`) — the FALLBACK, and only the fallback.
///
/// 193 of the 568 shipped unit blueprints state their own `Veteran` table, and they are not
/// small adjustments: an interceptor promotes at 2 kills and a T2 gunship at 6, against this
/// table's 25. Using the default for everything would make veterancy roughly an order of
/// magnitude too rare across most of the combat roster, which is why `UnitDef` carries a
/// per-type table and this is what a type falls back to when its blueprint states none.
inline constexpr VeterancyThresholds kVeterancyKillThresholds{25, 100, 250, 500, 1000};

/// Flat regeneration ADDED at each level, in health per second. Index `i` is level `i + 1`.
using VeterancyRegen = VeterancyThresholds;

/// `VeterancyRegen1-5`'s `Add` values (`Mult` is 1) — again the FALLBACK only.
///
/// 192 of the 568 shipped blueprints restate these under `Buffs.Regen`, and the values differ:
/// an interceptor gets 1 per level and a Cybran T3 bomber 3, against this table's 2. Retail
/// applies the default buff and then the blueprint one, and both carry `BuffType =
/// 'VETERANCYREGEN'` with `Stacks = 'REPLACE'` — so the blueprint value replaces rather than
/// adds to the default. Whole numbers, so this stays out of `check_no_sim_floats`'s way and
/// `veteranRegenPerTick` can divide exactly.
inline constexpr VeterancyRegen kVeterancyRegenPerSecond{2, 4, 6, 8, 10};

/// Clamps a level into range. Defensive rather than decorative: the level is used to index
/// two tables, and a corrupt or out-of-range value would be a read out of bounds.
[[nodiscard]] constexpr int clampVeteranLevel(int level) noexcept {
    return level < 0 ? 0 : (level > kMaxVeteranLevel ? kMaxVeteranLevel : level);
}

/// A unit's maximum health at this level, computed from the BLUEPRINT base.
///
/// Taking the base rather than the current maximum is the whole of point 1 above: it is what
/// makes the levels replace each other instead of stacking.
///
/// SCALED AS AN EXACT RATIO on the raw fixed-point value, rather than by multiplying by an
/// `Fx` multiplier. `VeterancyHealth<L>` states `Mult = 1 + L/10`, and 1.1 is not
/// representable in binary: `Fx::fromRatio(11, 10)` is 18022/16384, so scaling 1,000 health
/// through it yields 1,099.976 rather than 1,100. Multiplying the raw value by `10 + L` and
/// then dividing by 10 keeps the arithmetic in whole numbers and lands on the exact figure —
/// which matters because this feeds the state hash, and because "veteran health is 0.002%
/// short" is the kind of discrepancy nobody would ever track down.
///
/// The intermediate cannot overflow: the largest `MaxHealth` in the corpus is 5,000,000,
/// which is a raw value of about 8.2e10, and fifteen times that is 1.2e12 against `int64_t`'s
/// 9.2e18.
[[nodiscard]] constexpr Mag veterancyMaxHealth(Mag blueprintMaxHealth, int level) noexcept {
    const MagRaw scaled =
        blueprintMaxHealth.raw() * (10 + clampVeteranLevel(level)) / 10;
    return Mag::fromRaw(scaled);
}

/// The regeneration a veteran of this level adds, in health per TICK.
///
/// Divided out in whole numbers rather than going through `TickRate::magPerTick`, which takes
/// a per-second `float`. Two reasons, and the second is the real one: the bonus depends on the
/// unit's level, so unlike every other rate it cannot be converted once at content load and
/// must be derived inside the tick — and a float conversion in the tick is exactly what the
/// fixed-point rule exists to prevent.
///
/// ROUNDED TO NEAREST, not truncated, because that is the contract every other rate conversion
/// here states (`TickRate::ticks`, `magFromFloat`). The difference is one part in 16,384 — at
/// level 2 and 10 Hz, truncation gives 6,553 raw where rounding gives 6,554 — which is far too
/// small to see and far too easy to leave inconsistent. Two conversions of the same rate that
/// disagree in the last bit are a divergence source, and this value feeds the state hash.
[[nodiscard]] inline Mag veteranRegenPerTick(TickRate rate, int level,
                                            const VeterancyRegen& perLevel) noexcept {
    const int clamped = clampVeteranLevel(level);
    if (clamped == 0) {
        return Mag{};
    }
    const int perSecond = perLevel[static_cast<std::size_t>(clamped - 1)];
    if (perSecond <= 0) {
        return Mag{};
    }
    const MagRaw ticks = static_cast<MagRaw>(rate.ticksPerSecond());
    return Mag::fromRaw((Mag::fromInt(perSecond).raw() + ticks / 2) / ticks);
}

/// The level a unit with this many kills has earned, against its own threshold table.
///
/// Derived from the total rather than accumulated step by step, so it cannot drift and it
/// gives the same answer whether the kills arrived one at a time or in a batch. Retail's two
/// paths — `CheckVeteranLevel`, which promotes at most one level, and `AddKills`, which loops
/// — agree with this whenever a single kill cannot cross two thresholds. That holds for the
/// default table and for every shipped override, whose levels are evenly spaced.
///
/// A non-positive threshold is treated as absent and ends the ladder, so a blueprint stating
/// only `Level1` and `Level2` caps at 2 rather than promoting on a zero.
[[nodiscard]] constexpr int veterancyLevelFor(int kills,
                                              const VeterancyThresholds& thresholds) noexcept {
    int level = 0;
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        if (thresholds[i] <= 0) {
            break;
        }
        if (kills >= thresholds[i]) {
            level = static_cast<int>(i) + 1;
        }
    }
    return level;
}

// Forward-declared rather than included: this header is pulled in by `Health.hpp`, which the
// store itself includes, so including the store here would close a cycle. `TickRate` and
// `UnitId` are taken by value and so must be complete, which is why those two are included.
class EventQueue;
class UnitCatalog;
class UnitStore;

/// Credits one kill to `killer` and promotes it if that crossed a threshold.
///
/// Called once per death from `retireDead`, which is where retail calls `OnKilledUnit` on the
/// instigator — before the death weapon fires and before the wreck is made. Returns whether a
/// promotion happened, which is what a caller would count; the health change is applied here.
///
/// A killer that is already dead earns nothing and this is not an error: `lastHitBy` outlives
/// its unit on purpose, so a duel where both sides die on the same tick reaches here with a
/// stale handle routinely.
bool creditKill(UnitStore& store, const UnitCatalog& catalog, UnitId killer,
                EventQueue* events);

/// Heals every damaged unit by its regeneration rate for one tick.
///
/// Hull regeneration only — bubbles are `tickShields`, which has a delay and a recharge that a
/// hull does not. A unit at full health, or with no `Defense.RegenRate` and no veterancy, is
/// skipped rather than healed by zero.
void tickRegeneration(UnitStore& store, const UnitCatalog& catalog, TickRate rate);

} // namespace rm::sim
