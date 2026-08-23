#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <span>

namespace rm::sim {

// Turning wrecks back into mass.
//
// THE MECHANIC, read from the game's own Lua (the value chain is cited on `UnitDef` and
// `Feature`): a builder within its build reach of a wreck drains it continuously, at
// `BuildRate × Feature::reclaimPerBuildRate` value per second — 5 × BuildRate for every
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

/// One tick of every reclaim order in the store: drains wrecks, credits economies,
/// removes what is emptied. Returns how many units actually harvested this tick — the
/// outward sign a reclaim is progressing, for the report and for tests.
///
/// Run AFTER movement (a reclaimer harvests from where it ended the tick) and BEFORE
/// `tickEconomy` (so the cap applies to this tick's haul in this tick).
std::size_t harvestReclaim(UnitStore& store, const UnitCatalog& catalog,
                           FeatureStore& features, std::span<Economy> economies);

/// One tick of PATROLHELPER work for builders whose current order is `Patrol`.
///
/// The builder does not leave its route or create an internal order: it repairs the nearest
/// damaged allied unit already inside build reach, otherwise reclaims the nearest wreck there.
/// A temporary combat target suppresses service, and explicit reclaim orders run first.
/// Returns how many builders did useful work. Repair is intentionally free until explicit
/// engineer assist generalises construction and repair resource consumption together.
std::size_t servicePatrolBuilders(UnitStore& store, const UnitCatalog& catalog,
                                  std::span<const Army> armies, FeatureStore* features,
                                  std::span<Economy> economies);

} // namespace rm::sim
