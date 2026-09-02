#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// Turning wrecks back into mass.
//
// THE MECHANIC, read from the game's own Lua (the value chain is cited on `UnitDef` and
// `Feature`): a builder within its build reach of a wreck drains it continuously, at
// `BuildRate × Feature::reclaimPerBuildRate` value per second — 10 × BuildRate for every
// wreck in the corpus — and the income lands in the army's store the tick it is earned,
// subject to the same storage cap as every other income (`tickEconomy` clamps; overflow
// is lost, exactly as extraction over a full store is).
//
// SEVERAL RECLAIMERS SHARE ONE WRECK, each at its own rate, first slot first — the last
// tick's grant is whatever is left, so the total never exceeds what the wreck held.
// `Unit.lua:909`'s comment ("can end up negative if another engineer finishes reclaiming
// the prop between us") is the original engine saying the same thing less politely.
//
// WHAT IS DELIBERATELY NOT HERE: FA's overkill and fraction-complete scaling of the
// wreck's value (needs state the death report does not carry), damage to wrecks reducing
// what is left (`wreckage.lua:44` — wrecks are not targetable here yet), and the rebuild
// bonus. Each is named in the survey doc's gap list rather than silently absent.

/// How close a reclaimer must be, centre to centre: its build reach plus both radii —
/// the same generosity the game's own build-range overlay draws
/// (`blueprints-units.lua:250` adds the footprint to the radius for exactly this reason).
[[nodiscard]] Fx reclaimReach(const UnitCatalog& catalog, UnitTypeIndex type,
                               const MoveState& reclaimer, const Feature& wreck) noexcept;

/// The initial build-distance reach for repairing another unit, centre to centre.
[[nodiscard]] Fx repairReach(const UnitCatalog& catalog, UnitTypeIndex type,
                              const MoveState& builder, const MoveState& target) noexcept;

/// One tick of every reclaim order in the store: drains wrecks, credits economies,
/// removes what is emptied. Returns how many units actually harvested this tick — the
/// outward sign a reclaim is progressing, for the report and for tests.
///
/// Run AFTER movement (a reclaimer harvests from where it ended the tick) and BEFORE
/// `tickEconomy` (so the cap applies to this tick's haul in this tick).
std::size_t harvestReclaim(UnitStore& store, const UnitCatalog& catalog,
                           FeatureStore& features, std::span<Economy> economies);

/// Collects every explicit repair that is actively holding its target into economy requests.
/// The caller awards these alongside construction and upkeep, then passes the same records to
/// `applyRepairWork` so healing uses exactly the allocation ratio that paid for it.
void collectRepairWork(const UnitStore& store, const UnitCatalog& catalog,
                       std::span<const Army> armies, std::vector<RepairWork>& out);

/// Applies repair healing after its requests have been awarded by `tickEconomy`.
std::size_t applyRepairWork(UnitStore& store, const UnitCatalog& catalog,
                            std::span<const RepairWork> repairs);

/// One tick of patrol-helper work for builders.
///
/// Patrol leaves its route intact while it repairs the nearest damaged allied unit already inside
/// build reach, otherwise reclaims the nearest wreck there. An explicit Repair works only on its
/// named target and holds through its two-range hysteresis. A temporary combat target suppresses
/// patrol service, and explicit reclaim orders run first.
/// Returns how many builders did useful work. Patrol help remains the existing free autonomous
/// convenience; explicit repair is collected separately so its demand joins the economy pass.
std::size_t servicePatrolBuilders(UnitStore& store, const UnitCatalog& catalog,
                                  std::span<const Army> armies, FeatureStore* features,
                                  std::span<Economy> economies);

} // namespace rm::sim
