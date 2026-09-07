#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

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

/// Recomputes every construction's `assistPerTick` from who is currently helping: every
/// standing Assist/Guard order in reach of its builder, then every idle engineering station
/// with a project in reach. Returns how many helpers contributed this tick — the outward
/// sign the order works.
std::size_t applyAssistance(const UnitStore& store, const UnitCatalog& catalog,
                            std::vector<Construction>& building,
                            std::span<const Army> armies = {}, const Intel* intel = nullptr,
                            const PlayableRect* playableRect = nullptr);

} // namespace rm::sim
