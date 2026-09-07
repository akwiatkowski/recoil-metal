#include "core/sim/Assist.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"

#include <algorithm>

namespace rm::sim {

namespace {

[[nodiscard]] const Army* armyWithIndex(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) {
            return &army;
        }
    }
    return nullptr;
}

} // namespace

bool idleEngineeringStation(UnitIndex slot, const UnitStore& store,
                            const UnitCatalog& catalog) noexcept {
    if (!store.slotAlive(slot) || !store.health()[slot].alive()) {
        return false;
    }
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    return def != nullptr && def->hasCategory("ENGINEERSTATION")
        && catalog.rates(store.typeAt(slot)).buildPerTick > Mag{}
        && store.orders()[slot].active() == nullptr;
}

std::optional<std::size_t>
stationConstructionInReach(UnitIndex slot, const UnitStore& store, const UnitCatalog& catalog,
                           std::span<const Construction> building,
                           std::span<const Army> armies) noexcept {
    const Army* owner = armyWithIndex(store.motion()[slot].armyIndex, armies);
    if (owner == nullptr) {
        return std::nullopt;  // an army the match does not know cannot be anyone's ally
    }
    const std::array<Fx, 3> at = positionOf(store.transforms()[slot]);
    std::optional<std::size_t> nearest;
    Fx nearestGap{};
    for (std::size_t i = 0; i < building.size(); ++i) {
        const Construction& work = building[i];
        const Army* payer = armyWithIndex(work.armyIndex, armies);
        if (work.finished() || payer == nullptr || !allied(*owner, *payer)) {
            continue;
        }
        const Fx gap = groundDistanceElmos(at, work.position);
        if (gap > constructionReach(catalog, store.typeAt(slot),
                                    static_cast<UnitTypeIndex>(work.blueprintIndex))) {
            continue;
        }
        if (!nearest || gap < nearestGap) {
            nearest = i;
            nearestGap = gap;
        }
    }
    return nearest;
}

std::size_t applyAssistance(const UnitStore& store, const UnitCatalog& catalog,
                            std::vector<Construction>& building, std::span<const Army> armies,
                            const Intel* intel, const PlayableRect* playableRect) {
    // Cleared first, unconditionally: last tick's help is not this tick's fact.
    for (Construction& work : building) {
        work.assistPerTick = Mag{};
    }
    if (building.empty()) {
        return 0;
    }

    std::size_t helping = 0;
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || !isGuardCommand(head->kind())
            || !store.alive(head->target())) {
            continue;
        }
        if (!guardAllowsBuildAssistance(slot, store, catalog, building, armies, intel, playableRect)) {
            continue;
        }

        const unitdef::UnitDef* assister = catalog.def(store.typeAt(slot));
        if (assister != nullptr && assister->hasCategory("FACTORY") && !assister->isMobile()) {
            continue;  // factory guard mirrors queued production in command dispatch
        }

        const Mag rate = catalog.rates(store.typeAt(slot)).buildPerTick;
        if (rate <= Mag{}) {
            continue;
        }

        // C-183 follows Unit+0x4e0 (the active guard target) until it reaches the builder
        // actually doing the work. A cycle is malformed but legal to issue one edge at a time;
        // it contributes nothing instead of choosing an arbitrary member as the founder.
        UnitId founder = head->target();
        std::vector<UnitId> visited;
        bool cyclic = false;
        while (store.alive(founder)) {
            if (std::ranges::find(visited, founder) != visited.end()) {
                cyclic = true;
                break;
            }
            visited.push_back(founder);
            const QueuedCommand* guarded = orders[founder.index].active();
            if (guarded == nullptr || !isGuardCommand(guarded->kind())
                || !store.alive(guarded->target())) {
                break;
            }
            founder = guarded->target();
        }
        if (cyclic || !store.alive(founder)) {
            continue;
        }

        // In reach of the resolved builder, not of the site. Intermediate guards follow the
        // next unit in the chain, so a stretched chain cannot lend build power at a distance.
        const Fx reach = catalog.rates(store.typeAt(slot)).buildReachElmos
                       + motion[slot].radiusElmos + motion[founder.index].radiusElmos;
        if (groundDistanceElmos(positionOf(transforms[slot]),
                                positionOf(transforms[founder.index]))
            > reach) {
            continue;  // still walking over
        }

        // The OLDEST unfinished construction this target founded — the one it is working
        // on. First match in list order, which is creation order, so every assister of one
        // target picks the same work and the answer is replay-stable.
        for (Construction& work : building) {
            if (work.finished() || !(work.builder == founder)) {
                continue;
            }
            work.assistPerTick += rate;
            ++helping;
            break;
        }
    }

    // Idle engineering stations, after the ordered helpers: a station with a standing Assist
    // order of its own has already been counted above and is not idle.
    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!idleEngineeringStation(slot, store, catalog)) {
            continue;
        }
        const std::optional<std::size_t> project =
            stationConstructionInReach(slot, store, catalog, building, armies);
        if (!project) {
            continue;
        }
        building[*project].assistPerTick += catalog.rates(store.typeAt(slot)).buildPerTick;
        ++helping;
    }

    return helping;
}

} // namespace rm::sim
