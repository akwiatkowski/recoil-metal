#pragma once

#include "core/sim/Fx.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rm::sim {

// `CArmyStats` — `CArmyImpl+0x214` (`C-227`): the serialized per-army statistic store
// and one-shot threshold-trigger service, not merely score telemetry. Retail's
// serializer (`0x0071b3a0`) writes named aggregate and per-blueprint statistics plus a
// list of shared trigger objects; the evaluator (`0x00712920`) supports `>`, `>=`,
// `<`, `<=`, requires EVERY condition to pass, removes qualifying triggers before
// callback delivery, and runs from the per-army beat after tick 10. Shipped Lua uses
// it for economy hysteresis (`aibrain.lua`'s `EconLowMassStore`/`EconFullMassStore`
// ladder), scenario objectives, scoring and taunts.
//
// THE CALLBACK IS THE CALLER'S. Retail's trigger object carries a Lua function; this
// engine has no Lua host on the sim side, so `evaluateArmyStats` returns the NAMES of
// the triggers that fired — removed before the names are delivered, exactly the
// retail ordering — and whoever owns the match decides what a name means. That keeps
// the serialized state identical in shape: names, conditions and values, no code.

/// One threshold test inside a trigger: stat `stat` (optionally the per-blueprint
/// row `category` names) compared `op` against `value`. Retail's `CompareType`
/// strings, as a closed set — the evaluator supports exactly these four.
enum class StatCompare : std::uint8_t {
    GreaterThan,
    GreaterThanOrEqual,
    LessThan,
    LessThanOrEqual,
};

/// The `CompareType` spelling retail Lua passes (`'GreaterThan'`,
/// `'GreaterThanOrEqual'`, `'LessThan'`, `'LessThanOrEqual'`). Unknown names read as
/// `GreaterThanOrEqual`, which is `TriggerManager.lua`'s own default when a spec
/// leaves `CompareType` unset.
[[nodiscard]] StatCompare statCompareFromName(std::string_view name) noexcept;

struct ArmyStatCondition {
    /// The stat's name — `Economy_Ratio_Mass`, `Units_Killed`, `Enemies_Killed`.
    std::string stat;
    /// The per-blueprint row, or empty for the aggregate stat. Retail's optional
    /// `Category` argument to `SetArmyStatsTrigger` — `GetBlueprintStat`'s key.
    std::string category;
    StatCompare op = StatCompare::GreaterThanOrEqual;
    Mag value{};
};

/// One one-shot trigger: fires when EVERY condition passes, then is gone. A trigger
/// with no conditions can never qualify — retail's evaluator requires at least one
/// comparison to have something to test.
struct ArmyStatTrigger {
    /// The name Lua registered — `EconLowMassStore`, a scenario trigger name. It is
    /// also what `evaluateArmyStats` hands back: the callback's identity.
    std::string name;
    std::vector<ArmyStatCondition> conditions;
};

/// Which trigger fired, for delivery — the callback itself is the caller's.
struct ArmyStatFired {
    int army = -1;
    std::string name;
};

/// One army's statistics and pending triggers.
///
/// Sorted vectors rather than maps: a real match holds a few dozen stats and a
/// handful of triggers, the store serializes in order, and a linear scan is the
/// honest answer at this size.
struct ArmyStats {
    /// Named aggregate stats, sorted by name — `GetArmyStat(name)` reads these.
    std::vector<std::pair<std::string, Mag>> stats;
    /// Per-blueprint stats, sorted by (stat, blueprint) — `GetBlueprintStat(name,
    /// category)` reads these. The "category" is the blueprint id in retail's use.
    std::vector<std::pair<std::pair<std::string, std::string>, Mag>> blueprintStats;
    /// Pending one-shot triggers, in registration order.
    std::vector<ArmyStatTrigger> triggers;
};

/// `GetArmyStat`: the aggregate stat, or `fallback` when the army never recorded it.
[[nodiscard]] Mag armyStat(const ArmyStats& stats, std::string_view name,
                           Mag fallback = {}) noexcept;

/// `GetBlueprintStat`: the per-blueprint row, or `fallback`.
[[nodiscard]] Mag armyBlueprintStat(const ArmyStats& stats, std::string_view name,
                                    std::string_view category,
                                    Mag fallback = {}) noexcept;

/// `SetArmyStat`: write the aggregate stat, creating the row on first use.
void setArmyStat(ArmyStats& stats, std::string name, Mag value);

/// Add to the aggregate stat — the engine's own counters (`Units_Killed`,
/// `Economy_TotalConsumed_Mass`) accumulate this way.
void addArmyStat(ArmyStats& stats, const std::string& name, Mag delta);

/// Add to a per-blueprint row — `Units_Killed` under the victim's blueprint id.
void addArmyBlueprintStat(ArmyStats& stats, const std::string& name,
                          const std::string& category, Mag delta);

/// `SetArmyStatsTrigger`: register a one-shot trigger. Re-registering the same name
/// REPLACES the pending trigger — retail's hysteresis ladder re-arms the same names
/// (`EconLowMassStore` → `EconMidMassStore` → `EconFullMassStore`) and a duplicate
/// would fire the stale condition alongside the new one.
void setArmyStatsTrigger(ArmyStats& stats, ArmyStatTrigger trigger);

/// The evaluator (`0x00712920`): every condition must pass for a trigger to qualify;
/// qualifying triggers are REMOVED BEFORE the fired list is delivered — retail's
/// ordering, so a callback that re-registers the same name cannot see its own
/// trigger still pending. Returns the fired trigger names in registration order.
[[nodiscard]] std::vector<std::string> evaluateArmyStats(ArmyStats& stats);

} // namespace rm::sim
