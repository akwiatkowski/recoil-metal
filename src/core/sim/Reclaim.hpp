#pragma once

#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <optional>
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

/// Collects every explicit repair that is actively holding its target into economy requests,
/// then every guard's repair pick, then every idle engineering station's: a station with no
/// construction in reach (`stationConstructionInReach`, which is why `building` is passed)
/// heals the nearest damaged allied unit within its repair reach.
/// The caller awards these alongside construction and upkeep, then passes the same records to
/// `applyRepairWork` so healing uses exactly the allocation ratio that paid for it.
void collectRepairWork(const UnitStore& store, const UnitCatalog& catalog,
                       std::span<const Army> armies, std::vector<RepairWork>& out,
                       std::span<const GuardWork> guardWork = {},
                       std::span<const Construction> building = {});

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

// Reclaiming a LIVING UNIT (`CommandKind::ReclaimUnit`).
//
// THE MECHANIC. Retail's `CUnitReclaimTask::TaskTick` (C-147) asks the target's Lua
// `GetReclaimCosts` for a duration and both costs, converts the duration to ticks, then each
// beat drives the target's `Materialize` with one negative progress step and credits the
// army by the fraction actually applied. FAF's `Unit.lua:4024 GetReclaimCosts` for a unit:
// duration = 0.1 × max(BuildCostEnergy, BuildCostMass) / BuildRate seconds, returning the
// full build costs — so work per tick is the reclaimer's BuildRate per second, the total
// work is the larger build cost, and a fully reclaimed unit pays its whole build cost. The
// unit's fraction falls with the work (C-099: no health floor on a decrease), which here is
// its health. At zero it is destroyed WITHOUT a wreck (`Unit.lua:819 OnReclaimed` calls
// `Destroy`, not the kill path that leaves wreckage).
//
// NOT READ FROM RETAIL: whether that Lua formula matches the native one bit for bit (the
// claim table has the native mechanism, not the unit-cost formula), the reclaimer's
// `ReclaimTimeMultiplier` (a script field, taken as 1), and reclaiming one's own units.

/// One tick of every ReclaimUnit order in reach: un-builds the target, credits the reclaimer's
/// army, and destroys a fully reclaimed unit without a wreck. Run in the economy step with
/// `harvestReclaim`. Returns how many reclaimers made progress.
std::size_t reclaimUnits(UnitStore& store, const UnitCatalog& catalog,
                         std::span<Economy> economies, EventQueue* events);

/// The units being un-built this tick and by whose side, for C-157's targeting exemption.
/// Derived from the active ReclaimUnit queue heads after dispatch; never saved.
[[nodiscard]] std::vector<WorkClaim> collectUnitWorkClaims(const UnitStore& store);

/// `value * work / total`, for positive `Mag` values, without a lossy intermediate ratio and
/// without overflowing the 64-bit raw representation. Shared with capture budgeting, which
/// spreads a build-energy cost over a work-tick budget the same way.
[[nodiscard]] Mag proportionalWork(Mag value, Mag work, Mag total) noexcept;

// THE REBUILD BONUS (C-148, `LookForStructureRebuilder` `0x005fd970`; applied by C-191,
// `Helper::SetTarget` `0x005fc65f`). When a structure begins construction, retail finds the
// NEAREST `RECLAIMABLE` prop inside the new building's footprint rect — built from the
// blueprint's footprint bytes `bp[0xd8]`/`bp[0xd9]` — reads the winner's `AssociatedBP`
// (the blueprint id of the unit that died, `Unit.lua:1137`), and `stricmp`s it against the
// new blueprint's `Economy.RebuildBonusIds` (`bp+0x518`, our `UnitDef::rebuildBonusIds`).
// On a match the wreck is CONSUMED at rebuild start (C-190 passes it to a reclaim-family
// function) and the build gets a HEAD START, not a refund: `task+0x5C` holds
// `wreck's Entity+0xD8 × GetRebuildBonus()`, where `Entity+0xD8` is the wreck's fraction
// complete (C-133) and `GetRebuildBonus` is a hardcoded 0.5 (`Unit.lua:2723-2726`,
// "everything re-built is 50% complete to begin with"). `Helper::SetTarget` then applies it
// once via `Materialize(helper+0x1c)`.

/// What a matching wreck under a new structure is worth: which feature, and the fraction of
/// the new build already complete — `wreck fraction × 0.5`, so a whole wreck halves the
/// build and a half-reclaimed one quarters it.
struct RebuildBonus {
    FeatureId wreck{};
    Fx headStart{};
};

/// The rebuild bonus a structure of `product` placed at (`siteX`, `siteZ`) earns, if any.
/// Searches every live feature whose centre lies inside the footprint rect — half-extents
/// `footprintSquaresX/Z` ogrids, 8 elmos each — keeps the nearest by squared distance, and
/// matches only that one's type name against `product.rebuildBonusIds`, case-insensitively
/// like retail's `stricmp` (the corpus spells the ids lowercase, `'xsb2108'`, while
/// `UnitDef::name` keeps the file's casing). Nullopt when nothing matches: the build then
/// starts at zero and the wreck stays.
[[nodiscard]] std::optional<RebuildBonus> findRebuildBonus(
    const FeatureStore& features, const UnitCatalog& catalog,
    const unitdef::UnitDef& product, Fx siteX, Fx siteZ) noexcept;

} // namespace rm::sim
