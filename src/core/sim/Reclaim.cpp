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

} // namespace

Fx reclaimReach(const UnitCatalog& catalog, UnitTypeIndex type, const MoveState& reclaimer,
                const Feature& wreck) noexcept {
    return catalog.rates(type).buildReachElmos + reclaimer.radiusElmos + wreck.radiusElmos;
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
        const Command* head = orders[slot].active();
        if (head == nullptr || head->kind != CommandKind::Reclaim) {
            continue;
        }
        Feature* wreck = features.findMutable(head->target);
        if (wreck == nullptr) {
            continue;  // emptied by someone else; `advanceOrders` retires the order
        }

        const UnitTypeIndex type = store.typeAt(slot);
        const Fx gap = groundDistanceElmos(positionOf(transforms[slot]), wreck->at);
        if (gap > reclaimReach(catalog, type, motion[slot], *wreck)) {
            continue;  // still walking there
        }

        if (drainWreck(slot, head->target, store, catalog, features, economies)) {
            ++harvesting;
        }
    }

    return harvesting;
}

std::size_t servicePatrolBuilders(UnitStore& store, const UnitCatalog& catalog,
                                  std::span<const Army> armies, FeatureStore* features,
                                  std::span<Economy> economies) {
    std::size_t serviced = 0;

    for (UnitIndex builder = 0; builder < store.orders().size(); ++builder) {
        if (!store.slotAlive(builder) || !store.health()[builder].alive()) {
            continue;
        }
        const Command* order = store.orders()[builder].active();
        const UnitCatalog::Rates& rates = catalog.rates(store.typeAt(builder));
        if (order == nullptr || order->kind != CommandKind::Patrol
            || order->target.generation != 0
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
            const Fx reach = rates.buildReachElmos + builderMotion.radiusElmos
                           + store.motion()[target].radiusElmos;
            if (distance <= reach && (!repairTarget || distance < repairDistance)) {
                repairTarget = target;
                repairDistance = distance;
            }
        }

        if (repairTarget) {
            Health& health = store.health()[*repairTarget];
            const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(*repairTarget));
            // Recoil `CUnit::AddBuildPower` (`Unit.cpp:2030-2049`): repair step is
            // build work / target BuildTime, and health gained is maxHealth * step.
            // FA's native engine uses the same build-work progression; its Lua layer
            // only applies the resource discount, deferred to explicit assist here.
            const Mag restored = std::min(
                health.maximum - health.current,
                proportionalWork(health.maximum, rates.buildPerTick, targetDef->buildTime));
            if (restored > Mag{}) {
                health.current += restored;
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
