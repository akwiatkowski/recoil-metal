#include "core/sim/Reclaim.hpp"

#include "core/sim/Combat.hpp"

#include <algorithm>
#include <optional>

namespace rm::sim {
namespace {

[[nodiscard]] const Army* armyFor(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) {
            return &army;
        }
    }
    return nullptr;
}

/// `value * work / total`, for positive `Mag` values, without a lossy intermediate ratio and
/// without overflowing the 64-bit raw representation. `work` is clamped to `total`, so the
/// result cannot exceed `value`; the long-division tail handles the remainder product one bit
/// at a time instead of constructing a 128-bit temporary C++ does not portably provide.
[[nodiscard]] Mag proportionalWork(Mag value, Mag work, Mag total) noexcept {
    if (value <= Mag{} || work <= Mag{} || total <= Mag{}) {
        return Mag{};
    }

    const std::uint64_t a = static_cast<std::uint64_t>(value.raw());
    const std::uint64_t b = static_cast<std::uint64_t>(std::min(work, total).raw());
    const std::uint64_t d = static_cast<std::uint64_t>(total.raw());
    const std::uint64_t whole = (a / d) * b;
    const std::uint64_t remainderValue = a % d;

    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        quotient *= 2;
        remainder *= 2;
        if (remainder >= d) {
            remainder -= d;
            ++quotient;
        }
        if (((b >> static_cast<unsigned>(bit)) & 1u) != 0u) {
            remainder += remainderValue;
            if (remainder >= d) {
                remainder -= d;
                ++quotient;
            }
        }
    }
    return Mag::fromRaw(static_cast<MagRaw>(whole + quotient));
}

[[nodiscard]] bool drainWreck(UnitIndex reclaimer, FeatureId id, UnitStore& store,
                              const UnitCatalog& catalog, FeatureStore& features,
                              std::span<Economy> economies) {
    Feature* wreck = features.findMutable(id);
    if (wreck == nullptr) {
        return false;
    }
    const Mag pace = catalog.rates(store.typeAt(reclaimer)).buildPerTick
                   * wreck->reclaimPerBuildRate;
    if (pace <= Mag{}) {
        return false;
    }

    const Mag massGrant = std::min(wreck->massRemaining, pace);
    const Mag energyGrant = std::min(wreck->energyRemaining, pace);
    wreck->massRemaining -= massGrant;
    wreck->energyRemaining -= energyGrant;

    const int owner = store.motion()[reclaimer].armyIndex;
    if (owner >= 0 && static_cast<std::size_t>(owner) < economies.size()) {
        economies[static_cast<std::size_t>(owner)].stored.mass += massGrant;
        economies[static_cast<std::size_t>(owner)].stored.energy += energyGrant;
    }
    if (wreck->massRemaining <= Mag{} && wreck->energyRemaining <= Mag{}) {
        features.remove(id);
    }
    return massGrant > Mag{} || energyGrant > Mag{};
}

[[nodiscard]] bool repairUnit(UnitIndex builder, UnitIndex target, UnitStore& store,
                               const UnitCatalog& catalog, Fx funded) {
    Health& health = store.health()[target];
    const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
    if (targetDef == nullptr || targetDef->buildTime <= Mag{}) {
        return false;
    }
    // Recoil `CUnit::AddBuildPower` (`Unit.cpp:2030-2049`): repair step is build work /
    // target BuildTime, and health gained is maxHealth * step.
    const Mag restored = std::min(
        health.maximum - health.current,
        proportionalWork(health.maximum, catalog.rates(store.typeAt(builder)).buildPerTick * funded,
                          targetDef->buildTime));
    if (restored <= Mag{}) {
        return false;
    }
    health.current += restored;
    return true;
}

[[nodiscard]] Resources repairDrain(UnitIndex builder, const unitdef::UnitDef& target,
                                    const UnitStore& store, const UnitCatalog& catalog) noexcept {
    return drainPerTick(Construction{
        .cost = {.mass = target.buildCostMass, .energy = target.buildCostEnergy},
        .totalBuildTime = target.buildTime,
        .buildPerTick = catalog.rates(store.typeAt(builder)).buildPerTick,
    });
}

} // namespace

Fx reclaimReach(const UnitCatalog& catalog, UnitTypeIndex type, const MoveState& reclaimer,
                const Feature& wreck) noexcept {
    return catalog.rates(type).buildReachElmos + reclaimer.radiusElmos + wreck.radiusElmos;
}

Fx repairReach(const UnitCatalog& catalog, UnitTypeIndex type, const MoveState& builder,
               const MoveState& target) noexcept {
    return catalog.rates(type).buildReachElmos + builder.radiusElmos + target.radiusElmos;
}

std::size_t harvestReclaim(UnitStore& store, const UnitCatalog& catalog,
                           FeatureStore& features, std::span<Economy> economies) {
    std::size_t harvesting = 0;

    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || !store.health()[slot].alive()) {
            continue;
        }
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || head->kind() != CommandKind::Reclaim) {
            continue;
        }
        Feature* wreck = features.findMutable(head->target());
        if (wreck == nullptr) {
            continue;  // emptied by someone else; `advanceOrders` retires the order
        }

        const UnitTypeIndex type = store.typeAt(slot);
        const Fx gap = groundDistanceElmos(positionOf(transforms[slot]), wreck->at);
        if (gap > reclaimReach(catalog, type, motion[slot], *wreck)) {
            continue;  // still walking there
        }

        if (drainWreck(slot, head->target(), store, catalog, features, economies)) {
            ++harvesting;
        }
    }

    return harvesting;
}

void collectRepairWork(const UnitStore& store, const UnitCatalog& catalog,
                       std::span<const Army> armies, std::vector<RepairWork>& out) {
    out.clear();
    for (UnitIndex builder = 0; builder < store.orders().size(); ++builder) {
        if (!store.slotAlive(builder) || !store.health()[builder].alive()) {
            continue;
        }
        const QueuedCommand* order = store.orders()[builder].active();
        if (order == nullptr || order->kind() != CommandKind::Repair || !store.alive(order->target())
            || !store.health()[order->target().index].alive()) {
            continue;
        }
        const UnitIndex target = order->target().index;
        const Health& health = store.health()[target];
        const MoveState& builderMotion = store.motion()[builder];
        const Army* owner = armyFor(builderMotion.armyIndex, armies);
        const Army* targetArmy = armyFor(store.motion()[target].armyIndex, armies);
        const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
        if (health.current >= health.maximum || owner == nullptr || targetArmy == nullptr
            || !allied(*owner, *targetArmy) || targetDef == nullptr
            || targetDef->buildTime <= Mag{}) {
            continue;
        }
        const Fx reach = repairReach(catalog, store.typeAt(builder), builderMotion,
                                     store.motion()[target]);
        const Transform& builderAt = store.transforms()[builder];
        const Fx gap = groundDistanceElmos(positionOf(builderAt),
                                           positionOf(store.transforms()[target]));
        // A repair beam retains an established contact out to twice its start range. Before
        // contact, the command is still only approaching and cannot request resources or heal.
        const bool established = order->targetX() == builderAt.x && order->targetZ() == builderAt.z;
        if (gap > (established ? reach * 2 : reach)) {
            continue;
        }
        out.push_back(RepairWork{.armyIndex = builderMotion.armyIndex,
                                 .builder = builder,
                                 .target = target,
                                 .demand = repairDrain(builder, *targetDef, store, catalog)});
    }
}

std::size_t applyRepairWork(UnitStore& store, const UnitCatalog& catalog,
                            std::span<const RepairWork> repairs) {
    std::size_t serviced = 0;
    for (const RepairWork& repair : repairs) {
        if (repair.builder >= store.slotCount() || repair.target >= store.slotCount()
            || !store.slotAlive(repair.builder) || !store.slotAlive(repair.target)) {
            continue;
        }
        if (repairUnit(repair.builder, repair.target, store, catalog, repair.funded)) {
            ++serviced;
        }
    }
    return serviced;
}

std::size_t servicePatrolBuilders(UnitStore& store, const UnitCatalog& catalog,
                                  std::span<const Army> armies, FeatureStore* features,
                                  std::span<Economy> economies) {
    std::size_t serviced = 0;

    for (UnitIndex builder = 0; builder < store.orders().size(); ++builder) {
        if (!store.slotAlive(builder) || !store.health()[builder].alive()) {
            continue;
        }
        const QueuedCommand* order = store.orders()[builder].active();
        const UnitCatalog::Rates& rates = catalog.rates(store.typeAt(builder));
        const bool patrol = order != nullptr && order->kind() == CommandKind::Patrol
                            && order->target().generation == 0;
        if (!patrol
            || rates.buildPerTick <= Mag{}) {
            continue;
        }

        const MoveState& builderMotion = store.motion()[builder];
        const Army* owner = armyFor(builderMotion.armyIndex, armies);
        const std::array<Fx, 3> from = positionOf(store.transforms()[builder]);

        std::optional<UnitIndex> repairTarget;
        Fx repairDistance{};
        for (UnitIndex target = 0; target < store.slotCount(); ++target) {
            if (target == builder || !store.slotAlive(target) || owner == nullptr) {
                continue;
            }
            Health& health = store.health()[target];
            const Army* targetArmy = armyFor(store.motion()[target].armyIndex, armies);
            const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
            if (!health.alive() || health.current >= health.maximum || targetArmy == nullptr
                || !allied(*owner, *targetArmy) || targetDef == nullptr
                || targetDef->buildTime <= Mag{}) {
                continue;
            }
            const Fx distance = groundDistanceElmos(from, positionOf(store.transforms()[target]));
            const Fx reach = repairReach(catalog, store.typeAt(builder), builderMotion,
                                         store.motion()[target]);
            if (distance <= reach && (!repairTarget || distance < repairDistance)) {
                repairTarget = target;
                repairDistance = distance;
            }
        }

        if (repairTarget) {
            if (repairUnit(builder, *repairTarget, store, catalog, kFxOne)) {
                ++serviced;
                continue;  // repair is PATROLHELPER priority 1; reclaim is priority 3
            }
        }

        if (features == nullptr) {
            continue;
        }
        std::optional<FeatureId> reclaimTarget;
        Fx reclaimDistance{};
        for (UnitIndex slot = 0; slot < features->size(); ++slot) {
            if (!features->slotAlive(slot)) {
                continue;
            }
            const Feature& wreck = features->all()[slot];
            if (wreck.massRemaining <= Mag{} && wreck.energyRemaining <= Mag{}) {
                continue;
            }
            const int army = builderMotion.armyIndex;
            if (army < 0 || static_cast<std::size_t>(army) >= economies.size()) {
                continue;
            }
            const Economy& economy = economies[static_cast<std::size_t>(army)];
            const bool canStore = (wreck.massRemaining > Mag{}
                                   && economy.stored.mass < economy.storage.mass)
                               || (wreck.energyRemaining > Mag{}
                                   && economy.stored.energy < economy.storage.energy);
            if (!canStore) {
                continue;  // an autonomous helper does not erase value into a full store
            }
            const Fx distance = groundDistanceElmos(from, wreck.at);
            if (distance <= reclaimReach(catalog, store.typeAt(builder), builderMotion, wreck)
                && (!reclaimTarget || distance < reclaimDistance)) {
                reclaimTarget = features->idAt(slot);
                reclaimDistance = distance;
            }
        }
        if (reclaimTarget
            && drainWreck(builder, *reclaimTarget, store, catalog, *features, economies)) {
            ++serviced;
        }
    }

    return serviced;
}

} // namespace rm::sim
