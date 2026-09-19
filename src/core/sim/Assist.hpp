#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

// Lending a build arm: the Assist order's per-tick effect.
//
// THE MECHANIC. A builder with an `Assist` order standing within build reach of its
// target adds its own BuildRate to the target's UNFINISHED construction — the oldest one,
// which is the one the target is working on. The construction then advances and DRAINS at
// the combined rate (`Construction::effectiveBuildPerTick`), so help costs resources
// faster in exchange for time, exactly as a second engineer on a build does in the game.
//
// RECOMPUTED EVERY TICK, from orders and positions the hash already covers: a helper that
// walks away, dies, or is re-tasked stops contributing the same tick, with no
// subscription bookkeeping to forget. A target with an idle queue is simply waited
// beside — the order is a standing one (`advanceOrders` keeps it while the target lives).

// ENGINEERING STATIONS HELP WITHOUT BEING TOLD. FA's `ENGINEERSTATION` units (the UEF Kennel,
// the Cybran Hive) cannot move and take no Guard order (`RULEUCC_Guard = false` in their
// blueprints), so the only way they can ever lend their BuildRate is on their own initiative.
// An idle station — alive, no order of its own — adds its rate to the NEAREST unfinished
// allied construction it can reach whose founder is alive and still on its Build order, judged
// with the same rule a mobile builder's build task uses (`constructionReach`); with nothing to
// build in reach it repairs the nearest damaged ally instead (`collectRepairWork`). Build before
// repair is the C-183 guard ladder's order.
//
// THE TWO SCANS RUN AT DIFFERENT POINTS OF THE TICK: assistance is judged before `advanceOrders`
// (with the dispatch stage, where retail's assisters work), repair after it. A construction that
// starts or finishes inside `advanceOrders` can therefore make a station do both, or neither,
// for that one tick. Deterministic and replay-stable — both read state the hash covers — and
// accepted rather than fused, because fusing them would move one of the passes off retail's
// stage.
//
// The player's decision (2026-09-07) was "assist and repair in reach", which is what a Kennel
// left alone does in a retail base once its pods have a target; retail additionally lets a
// player order the station to a specific project, which the ordinary Assist order still does.

/// An alive `ENGINEERSTATION` with build power and an empty order queue.
[[nodiscard]] bool idleEngineeringStation(UnitIndex slot, const UnitStore& store,
                                          const UnitCatalog& catalog) noexcept;

/// The nearest unfinished allied construction an idle station can reach from where it
/// stands, as an index into `building`; nothing when it has no project in range. Ties break
/// on the lower index, so the answer is replay-stable.
[[nodiscard]] std::optional<std::size_t>
stationConstructionInReach(UnitIndex slot, const UnitStore& store, const UnitCatalog& catalog,
                           std::span<const Construction> building,
                           std::span<const Army> armies) noexcept;

/// One helper's contribution this tick: `helper` lent its rate to `building[work]`.
///
/// PRESENTATION STATE — the same facts the rate itself is recomputed from, kept so the
/// HUD can draw a build stream from the unit actually helping rather than guessing who
/// might be. Like `assistPerTick` it is rewritten every tick: a helper that walks away
/// or dies drops out of the list the same beat it stops contributing.
struct AssistLink {
    UnitId helper{};
    /// Index into the `building` span `applyAssistance` was given — a HINT, not an
    /// identity. A row reaped after this pass (a dead upgrade's scaffold leaves the
    /// list later in the same tick) shifts every index after it, so a consumer must
    /// confirm the row still is the site `position` names before drawing.
    std::size_t work = 0;
    /// The site the helper lent its rate to — the link's identity across a `building`
    /// vector that may be shortened before the HUD reads it. Two constructions cannot
    /// share a cell, so the position is unambiguous.
    std::array<Fx, 3> position{};
    /// The shared target's fraction complete, reported as the helper's own work
    /// progress (`C-017`/`C-098`): retail's builder-side `Unit+0xd8` mirror is
    /// not a per-builder counter but the target's `GetFractionComplete` read
    /// through the helper — every assister on one project shows the same
    /// number, which is exactly what the HUD's progress bar over each engineer
    /// displays. Recomputed with the link each tick.
    Fx fraction{};
};

/// One assistant's silo-build contribution this tick — `C-083`'s
/// `SiloAssistWithResource` (`0x005d5b00`), driven from the *assistant's*
/// `CEconRequest` rather than the silo's `CEconomyEvent`. The demand is the
/// silo tick's cost scaled by `assistantRate / siloRate`, so a faster helper
/// asks for — and a funded grant advances — SEVERAL production ticks in one
/// call. Recomputed every tick like `assistPerTick`; never serialized.
struct SiloAssistWork {
    /// The silo being helped — `SiloAmmo::owner`.
    UnitId silo{};
    /// Who is helping — the unit the charge is recorded against.
    UnitId assistant{};
    /// This tick's request: `costPerTick × assistantRate / siloRate`.
    Resources demand;
};


/// Recomputes every construction's `assistPerTick` from who is currently helping: every
/// standing Assist/Guard order in reach of its builder, then every idle engineering station
/// with a project in reach. Returns how many helpers contributed this tick — the outward
/// sign the order works. `links`, when given, is cleared and refilled with one entry per
/// helper — presentation output, excluded from saves and state hashes.
std::size_t applyAssistance(const UnitStore& store, const UnitCatalog& catalog,
                            std::vector<Construction>& building,
                            std::span<const Army> armies = {}, const Intel* intel = nullptr,
                            const PlayableRect* playableRect = nullptr,
                            TickIndex tick = 0, TickRate rate = TickRate{},
                            std::vector<AssistLink>* links = nullptr,
                            std::span<const SiloAmmo> siloAmmo = {},
                            std::span<const SiloBuild> siloQueue = {},
                            std::vector<SiloAssistWork>* siloAssists = nullptr);

/// The unit at the end of `start`'s guard chain — `C-183`'s transitive walk
/// through `Unit+0x4e0`, the same field retail's roll-off follows (`C-189`).
/// Returns `start` when it guards nothing, and `start` again on a cycle: a
/// malformed chain resolves to its own entry point rather than an arbitrary
/// member, which is the honest answer for a rally lookup (the unit's own
/// list) and for assistance (no founder).
[[nodiscard]] UnitId terminalGuardTarget(UnitId start, const UnitStore& store) noexcept;

} // namespace rm::sim
