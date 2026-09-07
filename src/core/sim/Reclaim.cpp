#include "core/sim/Reclaim.hpp"

#include "core/sim/Assist.hpp"

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

[[nodiscard]] Fx fractionOf(Mag part, Mag total) noexcept {
    if (part <= Mag{} || total <= Mag{}) {
        return Fx{};
    }
    const Mag clipped = std::min(part, total);
    return Fx::fromRaw(saturate((FxWide{clipped.raw()} << kFxFractionalBits) / total.raw()));
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
    if (pace <= Mag{} || wreck->reclaimWorkRemaining <= Mag{}
        || wreck->reclaimWorkTotal <= Mag{}) {
        return false;
    }

    const Mag applied = std::min(pace, wreck->reclaimWorkRemaining);
    const Mag fullMass = wreck->maximumMassReclaim * wreck->damageRatio;
    const Mag fullEnergy = wreck->maximumEnergyReclaim * wreck->damageRatio;
    // The final applied slice receives any fixed-point remainder. Without this, a value whose
    // ratio is not exactly representable can leave dust behind when the shared work bar reaches
    // zero, and removing the wreck would silently delete that resource.
    const bool finalSlice = applied == wreck->reclaimWorkRemaining;
    const Mag massGrant = finalSlice
        ? wreck->massRemaining
        : std::min(wreck->massRemaining,
                   proportionalWork(fullMass, applied, wreck->reclaimWorkTotal));
    const Mag energyGrant = finalSlice
        ? wreck->energyRemaining
        : std::min(wreck->energyRemaining,
                   proportionalWork(fullEnergy, applied, wreck->reclaimWorkTotal));
    wreck->massRemaining -= massGrant;
    wreck->energyRemaining -= energyGrant;
    wreck->reclaimWorkRemaining -= applied;
    wreck->reclaimFraction = fractionOf(wreck->reclaimWorkRemaining,
                                        wreck->reclaimWorkTotal);
    wreck->health = std::min(wreck->health,
                             wreck->maximumHealth * wreck->reclaimFraction);

    const int owner = store.motion()[reclaimer].armyIndex;
    if (owner >= 0 && static_cast<std::size_t>(owner) < economies.size()) {
        economies[static_cast<std::size_t>(owner)].stored.mass += massGrant;
        economies[static_cast<std::size_t>(owner)].stored.energy += energyGrant;
    }
    if (wreck->reclaimWorkRemaining <= Mag{}) {
        features.remove(id);
    }
    return applied > Mag{};
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

Mag damageFeature(FeatureStore& features, FeatureId id, Mag damage) {
    Feature* wreck = features.findMutable(id);
    if (wreck == nullptr || damage <= Mag{} || wreck->health <= Mag{}) {
        return Mag{};
    }
    const Mag applied = std::min(damage, wreck->health);
    wreck->health -= applied;
    if (wreck->health <= Mag{}) {
        features.remove(id);
        return applied;
    }
    if (wreck->maximumHealth <= Mag{}) {
        return applied;
    }

    wreck->damageRatio = fractionOf(wreck->health, wreck->maximumHealth);
    const Mag fullMass = wreck->maximumMassReclaim * wreck->damageRatio;
    const Mag fullEnergy = wreck->maximumEnergyReclaim * wreck->damageRatio;
    wreck->massRemaining = fullMass * wreck->reclaimFraction;
    wreck->energyRemaining = fullEnergy * wreck->reclaimFraction;
    wreck->reclaimWorkTotal = std::max(fullMass, fullEnergy);
    wreck->reclaimWorkRemaining = wreck->reclaimWorkTotal * wreck->reclaimFraction;
    wreck->reclaimPerBuildRate = wreck->maximumReclaimPerBuildRate / wreck->damageRatio;
    return applied;
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

std::size_t reclaimUnits(UnitStore& store, const UnitCatalog& catalog,
                         std::span<Economy> economies, EventQueue* events) {
    std::size_t reclaiming = 0;
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || !store.health()[slot].alive()) {
            continue;
        }
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || head->kind() != CommandKind::ReclaimUnit) {
            continue;
        }
        const UnitId target = head->target();
        if (!store.alive(target) || !store.health()[target.index].alive()) {
            continue;  // gone; `advanceOrders` retires the order
        }
        const UnitIndex victim = target.index;
        const unitdef::UnitDef* def = catalog.def(store.typeAt(victim));
        if (def == nullptr) {
            continue;
        }
        const Mag total = std::max(def->buildCostMass, def->buildCostEnergy);
        if (total <= Mag{}) {
            continue;
        }
        const UnitTypeIndex type = store.typeAt(slot);
        const Fx gap = groundDistanceElmos(positionOf(transforms[slot]),
                                           positionOf(transforms[victim]));
        if (gap > repairReach(catalog, type, motion[slot], motion[victim])) {
            continue;  // still walking there
        }

        // Work per tick is the reclaimer's BuildRate per SECOND: FAF's duration is
        // 0.1 × cost / BuildRate seconds for `cost` units of work, so a tick advances
        // BuildRate of them — ten times the per-tick build figure the catalog derived.
        Mag pace = catalog.rates(type).buildPerTick;
        pace *= Fx::fromInt(10);
        Health& health = store.health()[victim];
        // The work left follows the fraction, and the fraction is the health (C-099).
        const Mag remaining = proportionalWork(total, health.current, health.maximum);
        const Mag applied = std::min(pace, remaining);
        if (applied <= Mag{}) {
            continue;
        }
        // The final slice pays whatever fraction is left exactly, so fixed-point dust never
        // leaves a unit worth 0.3 mass standing.
        const bool finalSlice = applied >= remaining;
        const Mag massGrant = finalSlice
            ? proportionalWork(def->buildCostMass, health.current, health.maximum)
            : proportionalWork(def->buildCostMass, applied, total);
        const Mag energyGrant = finalSlice
            ? proportionalWork(def->buildCostEnergy, health.current, health.maximum)
            : proportionalWork(def->buildCostEnergy, applied, total);
        const int owner = motion[slot].armyIndex;
        if (owner >= 0 && static_cast<std::size_t>(owner) < economies.size()) {
            economies[static_cast<std::size_t>(owner)].stored.mass += massGrant;
            economies[static_cast<std::size_t>(owner)].stored.energy += energyGrant;
        }

        if (finalSlice) {
            // Destroyed, not killed: no kill credit, no wreck, no death explosion. Retiring
            // the corpse here (radius zero, health zero, handle released) is what keeps
            // `retireDead` from treating it as a death next tick — its `radiusElmos > 0`
            // guard is the once-per-death rule, and this unit never died.
            emit(events, Event{
                             .kind = EventKind::UnitDestroyed,
                             .unit = target,
                             .instigator = store.idAt(slot),
                             .army = motion[victim].armyIndex,
                             .at = positionOf(transforms[victim]),
                         });
            health.current = Mag{};
            motion[victim].moving = false;
            motion[victim].speedPerTick = Fx{};
            motion[victim].radiusElmos = Fx{};
            store.kill(target);
        } else {
            health.current -= proportionalWork(health.maximum, applied, total);
        }
        ++reclaiming;
    }
    return reclaiming;
}

std::vector<WorkClaim> collectUnitWorkClaims(const UnitStore& store) {
    std::vector<WorkClaim> claims;
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || !store.health()[slot].alive()) {
            continue;
        }
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || head->kind() != CommandKind::ReclaimUnit
            || !store.alive(head->target())) {
            continue;
        }
        claims.push_back(WorkClaim{.target = head->target().index,
                                   .workerArmy = motion[slot].armyIndex});
    }
    return claims;
}

std::size_t applyGuardReclaim(UnitStore& store, const UnitCatalog& catalog,
                              FeatureStore& features, std::span<Economy> economies,
                              std::span<const GuardWork> work) {
    std::size_t serviced = 0;
    for (const GuardWork& item : work) {
        if (item.kind != GuardWorkKind::Reclaim || !store.slotAlive(item.builder)
            || !store.health()[item.builder].alive()) {
            continue;
        }
        Feature* wreck = features.findMutable(item.target);
        if (wreck == nullptr) continue;
        const MoveState& motion = store.motion()[item.builder];
        if (groundDistanceElmos(positionOf(store.transforms()[item.builder]), wreck->at)
            <= reclaimReach(catalog, store.typeAt(item.builder), motion, *wreck)
            && drainWreck(item.builder, item.target, store, catalog, features, economies)) {
            ++serviced;
        }
    }
    return serviced;
}

void collectRepairWork(const UnitStore& store, const UnitCatalog& catalog,
                       std::span<const Army> armies, std::vector<RepairWork>& out,
                       std::span<const GuardWork> guardWork,
                       std::span<const Construction> building) {
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
    for (const GuardWork& item : guardWork) {
        if (item.kind != GuardWorkKind::Repair || !store.slotAlive(item.builder)
            || !store.alive(item.target) || !store.health()[item.builder].alive()) continue;
        const UnitIndex target = item.target.index;
        const MoveState& builderMotion = store.motion()[item.builder];
        const Army* owner = armyFor(builderMotion.armyIndex, armies);
        const Army* targetArmy = armyFor(store.motion()[target].armyIndex, armies);
        const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
        if (owner == nullptr || targetArmy == nullptr || !allied(*owner, *targetArmy)
            || targetDef == nullptr || targetDef->buildTime <= Mag{}
            || store.health()[target].current >= store.health()[target].maximum
            || groundDistanceElmos(positionOf(store.transforms()[item.builder]),
                                   positionOf(store.transforms()[target]))
                   > repairReach(catalog, store.typeAt(item.builder), builderMotion,
                                 store.motion()[target])) continue;
        out.push_back(RepairWork{.armyIndex = builderMotion.armyIndex,
                                 .builder = item.builder,
                                 .target = target,
                                 .demand = repairDrain(item.builder, *targetDef, store, catalog)});
    }

    // Idle engineering stations (`Assist.hpp`): construction in reach comes first, exactly as
    // the guard ladder ranks build-assist above repair; otherwise the nearest damaged ally the
    // station can reach is healed, nearest first and lowest index on a tie.
    for (UnitIndex station = 0; station < store.orders().size(); ++station) {
        if (!idleEngineeringStation(station, store, catalog)
            || stationConstructionInReach(station, store, catalog, building, armies)) {
            continue;
        }
        const MoveState& stationMotion = store.motion()[station];
        const Army* owner = armyFor(stationMotion.armyIndex, armies);
        if (owner == nullptr) continue;
        const std::array<Fx, 3> at = positionOf(store.transforms()[station]);
        std::optional<UnitIndex> nearest;
        Fx nearestGap{};
        for (UnitIndex target = 0; target < store.orders().size(); ++target) {
            if (target == station || !store.slotAlive(target) || !store.health()[target].alive()
                || store.health()[target].current >= store.health()[target].maximum) continue;
            const Army* targetArmy = armyFor(store.motion()[target].armyIndex, armies);
            const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target));
            if (targetArmy == nullptr || !allied(*owner, *targetArmy) || targetDef == nullptr
                || targetDef->buildTime <= Mag{}) continue;
            const Fx gap = groundDistanceElmos(at, positionOf(store.transforms()[target]));
            if (gap > repairReach(catalog, store.typeAt(station), stationMotion,
                                  store.motion()[target])) continue;
            if (!nearest || gap < nearestGap) {
                nearest = target;
                nearestGap = gap;
            }
        }
        if (!nearest) continue;
        const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(*nearest));
        out.push_back(RepairWork{.armyIndex = stationMotion.armyIndex,
                                 .builder = station,
                                 .target = *nearest,
                                 .demand = repairDrain(station, *targetDef, store, catalog)});
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
