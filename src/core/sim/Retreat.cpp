#include "core/sim/Retreat.hpp"

#include "core/sim/Combat.hpp"   // groundDistanceElmos, positionOf
#include "core/sim/CommandQueue.hpp"
#include "core/sim/Reclaim.hpp"  // repairReach
#include "core/unit/UnitDef.hpp"

namespace rm::sim {

namespace {

/// The walk the automation writes. Built like a player's unshifted click: it
/// replaces the queue rather than appending to it, and `tick` is provenance only —
/// the order never entered a command log.
[[nodiscard]] Command retreatMove(UnitId unit, Fx x, Fx z) noexcept {
    Command move;
    move.kind = CommandKind::Move;
    move.unit = unit;
    move.targetX = x;
    move.targetZ = z;
    return move;
}

/// Whether the queue's head is the move this pass wrote — the check that lets a
/// healed unit take its return trip without stomping a player's newer orders.
[[nodiscard]] bool headIsTheRetreat(const CommandQueue& queue,
                                    const RetreatState& state) noexcept {
    const QueuedCommand* head = queue.current();
    return head != nullptr && head->kind() == CommandKind::Move
           && head->targetX() == state.toX && head->targetZ() == state.toZ;
}

/// The nearest allied unit that can hold a wrench: a builder or a factory, on the
/// same alliance, not the patient itself. `kNotFound` when nobody can — a unit
/// with no mechanic does not run, because there is nowhere to run TO.
[[nodiscard]] std::optional<UnitId> nearestMechanic(UnitIndex slot, UnitStore& store,
                                                   const UnitCatalog& catalog,
                                                   std::span<const Army> armies,
                                                   int alliance) noexcept {
    const std::array<Fx, 3> from = positionOf(store.transforms()[slot]);
    std::optional<UnitId> nearest;
    Fx nearestDistance{};
    for (UnitIndex candidate = 0; candidate < store.slotCount(); ++candidate) {
        if (candidate == slot || !store.slotAlive(candidate)
            || store.motion()[candidate].armyIndex == kNoArmy) {
            continue;
        }
        const int theirs = [&] {
            for (const Army& army : armies) {
                if (army.index == store.motion()[candidate].armyIndex) {
                    return army.alliance;
                }
            }
            return -1;
        }();
        if (theirs != alliance) {
            continue;
        }
        const unitdef::UnitDef* def = catalog.def(store.typeAt(candidate));
        if (def == nullptr || !(def->isBuilder() || def->hasCategory("FACTORY"))) {
            continue;
        }
        const Fx distance =
            groundDistanceElmos(from, positionOf(store.transforms()[candidate]));
        if (!nearest || distance < nearestDistance) {
            nearest = store.idAt(candidate);
            nearestDistance = distance;
        }
    }
    return nearest;
}

} // namespace

void updateRetreats(UnitStore& store, const UnitCatalog& catalog,
                    std::span<const Army> armies) noexcept {
    const auto allianceOf = [armies](int index) noexcept {
        for (const Army& army : armies) {
            if (army.index == index) {
                return army.alliance;
            }
        }
        return -1;
    };
    const std::span<RetreatState> retreats = store.retreats();
    const std::span<const RetreatThreshold> thresholds = store.retreatThresholds();
    // Arrived is "close enough that another move order would fidget, not walk": the
    // unit parks beside its mechanic and waits for the hull to fill.
    const Fx arrived = Fx::fromInt(8);

    for (UnitIndex slot = 0; slot < store.orders().size(); ++slot) {
        RetreatState& state = retreats[slot];
        if (!state.active && thresholds[slot] == RetreatThreshold::Off) {
            continue;
        }
        if (!store.slotAlive(slot)) {
            state = {};
            continue;
        }
        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || !def->isMobile() || store.motion()[slot].attached) {
            continue;  // a building cannot walk home; a cargo cannot drive itself
        }

        const Health& hull = store.health()[slot];
        const int percent = retreatThresholdPercent(thresholds[slot]);
        const bool hurt = percent > 0 && hull.maximum.raw() > 0
                          && hull.current.raw() * 100 < hull.maximum.raw() * percent;
        // A threshold cycled back to Off ends a retreat in flight the same way a full
        // hull does — the unit owes the front its return trip, nothing more. The
        // alternative is a parked hulk the player already told to stop running.
        const bool whole = hull.current.raw() >= hull.maximum.raw()
                           || thresholds[slot] == RetreatThreshold::Off;
        CommandQueue& queue = store.orders()[slot];
        const UnitId unit = store.idAt(slot);
        const std::array<Fx, 3> here = positionOf(store.transforms()[slot]);

        if (!state.active) {
            if (!hurt) {
                continue;
            }
            const std::optional<UnitId> mechanic =
                nearestMechanic(slot, store, catalog, armies,
                                allianceOf(store.motion()[slot].armyIndex));
            if (!mechanic) {
                continue;
            }
            const std::array<Fx, 3> there =
                positionOf(store.transforms()[mechanic->index]);
            state = RetreatState{.active = true, .returnX = here[0], .returnZ = here[2],
                                 .toX = there[0], .toZ = there[2]};
            (void)queue.give(retreatMove(unit, state.toX, state.toZ), false);
            continue;
        }

        if (whole) {
            // Whole again: the front it owes is `return`. Take the trip only when the
            // queue is either empty or still carrying this pass's own move — a newer
            // order means the player is driving, and the flag simply clears.
            if (queue.current() == nullptr || headIsTheRetreat(queue, state)) {
                const RetreatState run = state;
                (void)queue.give(retreatMove(unit, run.returnX, run.returnZ), false);
            }
            state = {};
            continue;
        }

        // Still hurt. A queue head that is not this pass's move is the player taking
        // the wheel — automation never fights them, so the flag simply clears. Only
        // an actually EMPTY queue re-arms the run home, and only while the unit is
        // still short of its mechanic: re-ordering a parked unit is a fidget, not a
        // walk.
        const QueuedCommand* head = queue.current();
        if (head != nullptr && !headIsTheRetreat(queue, state)) {
            state = {};
            continue;
        }
        // "Parked" is measured against the mechanic's own repair reach — the ring the
        // walk can actually stop on, since a footprint blocks the last few elmos to
        // the centre the move was aimed at. `nearestMechanic` runs again because the
        // state keeps coordinates, not the handle — the same unit answers while it
        // still stands.
        const std::optional<UnitId> mechanic =
            nearestMechanic(slot, store, catalog, armies,
                            allianceOf(store.motion()[slot].armyIndex));
        const Fx parking = mechanic
            ? std::max(arrived,
                       repairReach(catalog, store.typeAt(mechanic->index),
                                   store.motion()[mechanic->index],
                                   store.motion()[slot]))
            : arrived;
        if (groundDistanceElmos(here, {state.toX, Fx{}, state.toZ}) > parking) {
            // En route — or the run home was finished or interrupted and the queue
            // is empty, in which case it is re-armed.
            if (head == nullptr) {
                (void)queue.give(retreatMove(unit, state.toX, state.toZ), false);
            }
            continue;
        }
        // Parked — nothing heals it but the mechanic, and only while the mechanic's
        // own queue is idle. A factory mid-build keeps its work; the patient waits
        // rather than cancel production it interrupted nothing to reach.
        if (mechanic && store.orders()[mechanic->index].current() == nullptr) {
            Command repair;
            repair.kind = CommandKind::Repair;
            repair.unit = *mechanic;
            repair.target = unit;
            repair.targetX = here[0];
            repair.targetZ = here[2];
            (void)store.orders()[mechanic->index].give(repair, false);
        }
    }
}

} // namespace rm::sim
