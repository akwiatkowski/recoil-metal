#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

enum class GuardWorkKind : std::uint8_t { Reclaim, Repair };

/// A guard-ladder decision made during command dispatch and consumed later in the same beat.
/// It is derived, never saved: command dispatch always runs before the economy work it drives.
struct GuardWork {
    UnitIndex builder = 0;
    GuardWorkKind kind = GuardWorkKind::Reclaim;
    UnitId target{};
};

// Turning wrecks back into mass.
//
// THE MECHANIC, read from retail Lua and the native reclaim task (the value chain is cited on
// `UnitDef` and `Feature`): a builder within build reach advances one materialisation-work bar
// at `BuildRate × Feature::reclaimPerBuildRate` per second. The actual applied work fraction
// pays both mass and energy in the same proportion and lands in the army's store that tick,
// subject to the same storage cap as other income (`tickEconomy` clamps overflow).
//
// SEVERAL RECLAIMERS SHARE ONE WRECK, each at its own rate, first slot first — the last
// tick's grant is whatever is left, so the total never exceeds what the wreck held.
// `Unit.lua:909`'s comment ("can end up negative if another engineer finishes reclaiming
// the prop between us") is the original engine saying the same thing less politely.
//
// Damage can reduce a wreck's durability, value, and remaining work through `damageFeature`.
// Wreck collision, ordinary combat targeting, and the rebuild bonus remain outside this slice;
// each is named in the survey doc's gap list rather than silently absent.

/// How close a reclaimer must be, centre to centre: its build reach plus both radii —
/// the same generosity the game's own build-range overlay draws
/// (`blueprints-units.lua:250` adds the footprint to the radius for exactly this reason).
[[nodiscard]] Fx reclaimReach(const UnitCatalog& catalog, UnitTypeIndex type,
                               const MoveState& reclaimer, const Feature& wreck) noexcept;

/// The initial build-distance reach for repairing another unit, centre to centre.
[[nodiscard]] Fx repairReach(const UnitCatalog& catalog, UnitTypeIndex type,
                              const MoveState& builder, const MoveState& target) noexcept;

/// Damages a wreck and recalculates its reclaim value from the immutable maximums, exactly as
/// `wreckage.lua::DoTakeDamage`; a lethal hit removes it. Returns health actually removed.
Mag damageFeature(FeatureStore& features, FeatureId id, Mag damage);

/// One tick of every reclaim order in the store: drains wrecks, credits economies,
/// removes what is emptied. Returns how many units actually harvested this tick — the
/// outward sign a reclaim is progressing, for the report and for tests.
///
/// Run AFTER movement (a reclaimer harvests from where it ended the tick) and BEFORE
/// `tickEconomy` (so the cap applies to this tick's haul in this tick).
std::size_t harvestReclaim(UnitStore& store, const UnitCatalog& catalog,
                           FeatureStore& features, std::span<Economy> economies);

/// Applies reclaim-copy decisions made by the guard ladder. Manual reclaim has already run;
/// patrol work runs afterwards.
std::size_t applyGuardReclaim(UnitStore& store, const UnitCatalog& catalog,
                              FeatureStore& features, std::span<Economy> economies,
                              std::span<const GuardWork> work);

/// Collects every explicit repair that is actively holding its target into economy requests.
/// The caller awards these alongside construction and upkeep, then passes the same records to
/// `applyRepairWork` so healing uses exactly the allocation ratio that paid for it.
void collectRepairWork(const UnitStore& store, const UnitCatalog& catalog,
                       std::span<const Army> armies, std::vector<RepairWork>& out,
                       std::span<const GuardWork> guardWork = {});

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
