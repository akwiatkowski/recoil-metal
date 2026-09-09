#pragma once

#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// Taking a hostile unit intact (`CommandKind::Capture`).
//
// THE MECHANIC, read from the retail capture task (`CUnitCaptureTask::TaskTick`
// `0x0060aeb0`, `capture-implementation-spec.md`): one engineer capturing one
// completed, unattached ordinary enemy land unit or structure, with funded
// progress and replacement-entity transfer.
//
// Progress contract, for a single captor and a target with no attached units:
//   seconds   = ((target buildTime, or 10 when non-positive) / captor rate per second) / 2
//   workTicks = truncate(max(1, seconds * ticksPerSecond))
//   energy    = target buildCostEnergy, all of it, at energy / workTicks per beat
// Only a fully funded beat advances progress, by one per active captor; partial
// allocation does nothing. Completion kills the target silently (no wreck, no kill
// credit — a factory becoming its T2 self did not die either) and spawns a
// replacement of the same type, transform and health under the captor's army.
//
// What this slice deliberately excludes: attached parents/children, enhancements,
// fuel, silo ammunition and shields on the target (retail snapshots and restores
// them through Lua); concurrent captors beyond one progress bar each (each task
// keeps its own costs and progress, so two captors simply race); COMMAND-category
// targets (commanders are immune, and transferring one would break the
// Assassination victory count); airborne and submerged targets (a land captor can
// never stand in build reach of them).
struct CaptureWork {
    int armyIndex = kNoArmy;
    UnitIndex captor = 0;
    UnitId target{};
    int workTicks = 0;
    int progress = 0;
    Resources demand{};
    Fx funded{};
    // Set every sync: only an in-reach captor draws demand, and only an in-reach
    // task advances. A zero demand is trivially "fully funded" by the allocator,
    // so reach must gate progress explicitly rather than through the ratio.
    bool inReach = false;
};
/// Whether `captor` may start or keep capturing `target`: a live builder with the
/// authored CAPTURE category, and a live hostile completed unit that is neither
/// self, a commander, airborne, submerged, nor attached. Alliance is judged the
/// same way as unit reclaim — hostile, never own or allied — with the same
/// no-alliance-state compatibility seam.
[[nodiscard]] bool capturableTarget(UnitIndex captor, UnitId target, const UnitStore& store,
                                   const UnitCatalog& catalog,
                                   std::span<const Army> armies) noexcept;

/// The funded work budget for one capture, in ticks, from the retail progress
/// contract above. The rate is the captor's effective build rate per tick; the
/// budget is per second, so it scales with the tick rate rather than assuming one.
[[nodiscard]] int captureWorkTicks(Mag targetBuildTime, Mag ratePerTick,
                                   std::uint32_t ticksPerSecond) noexcept;

/// The per-beat energy demand for one capture: the target's whole build energy
/// cost spread evenly over its work budget. Mass costs nothing to capture.
[[nodiscard]] Resources captureDemand(Mag targetBuildEnergy, int workTicks) noexcept;
/// Reconciles the persistent capture list with this tick's active Capture heads:
/// drops entries whose order retired, retargeted or whose target died, and opens
/// entries for new captures with computed budgets. An entry persists while its head
/// stays on the same target, even walking there — but only an in-reach captor draws
/// demand, so only funded in-reach beats advance progress.

void syncCaptureWork(const UnitStore& store, const UnitCatalog& catalog,
                     std::span<const Army> armies, std::vector<CaptureWork>& captures,
                     TickRate rate);

/// One tick of funded captures: fully funded beats advance progress by one per
/// active captor, capped at the budget, and a finished capture transfers the
/// target to the captor's army. Returns how many captors made progress — the
/// outward sign a capture is advancing, for the report and for tests.
///
/// Run AFTER the economy pass awarded this tick's funding, like repair work.
std::size_t applyCaptureWork(UnitStore& store, std::vector<CaptureWork>& captures,
                             EventQueue* events);

/// The units being captured this tick and by whose side, for C-157's targeting
/// exemption: own guns do not shoot what own engineers are taking. Same shape as
/// the reclaim claims, so the same candidate loop honours both.
[[nodiscard]] std::vector<WorkClaim> collectCaptureClaims(const UnitStore& store);

} // namespace rm::sim
