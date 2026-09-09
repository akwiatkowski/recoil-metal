#include "core/sim/Capture.hpp"

#include "core/sim/Command.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Reclaim.hpp"

#include <algorithm>

namespace rm::sim {
namespace {

// Mirrors validReclaimUnit's alliance reading: hostile, never own or allied, with the
// same no-alliance-state compatibility seam for direct dispatch.
[[nodiscard]] bool hostilePair(UnitIndex captor, UnitId target, const UnitStore& store,
                               std::span<const Army> armies) noexcept {
    if (armies.empty()) {
        return true;
    }
    const int owner = store.motion()[captor].armyIndex;
    const int targetOwner = store.motion()[target.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [targetOwner](const Army& army) {
        return army.index == targetOwner;
    });
    return mine != armies.end() && theirs != armies.end() && !allied(*mine, *theirs);
}

} // namespace

bool capturableTarget(UnitIndex captor, UnitId target, const UnitStore& store,
                      const UnitCatalog& catalog, std::span<const Army> armies) noexcept {
    if (!store.alive(target) || !store.health()[target.index].alive()
        || target.index == captor) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(captor));
    const unitdef::UnitDef* victim = catalog.def(store.typeAt(target.index));
    if (builder == nullptr || victim == nullptr || !builder->isBuilder()
        || !builder->hasCategory("CAPTURE")) {
        return false;
    }
    // Commanders are immune: transferring one would hand over the Assassination
    // count, and retail destroys rather than converts them. Experimentals stay
    // capturable — only slowly, through the same cost formula as everything else.
    if (victim->hasCategory("COMMAND")) {
        return false;
    }
    const MoveState& targetMotion = store.motion()[target.index];
    if (targetMotion.airborne
        || (targetMotion.submersible && targetMotion.submerged)
        || targetMotion.attached || !store.childrenOf(target).empty()) {
        return false;
    }
    return hostilePair(captor, target, store, armies);
}

int captureWorkTicks(Mag targetBuildTime, Mag ratePerTick,
                     std::uint32_t ticksPerSecond) noexcept {
    if (ratePerTick <= Mag{} || ticksPerSecond == 0) {
        return 1;
    }
    // The retail progress contract in fixed point: seconds is the target's build
    // time (or 10 when it states none) over the captor's per-second rate, halved;
    // the budget is that many seconds of ticks, at least one.
    const Mag effectiveTime = targetBuildTime > Mag{} ? targetBuildTime : Mag::fromInt(10);
    const Mag ratePerSecond = ratePerTick * Fx::fromInt(static_cast<std::int32_t>(ticksPerSecond));
    const Mag seconds =
        proportionalWork(effectiveTime, Mag::fromInt(1), ratePerSecond + ratePerSecond);
    const int ticks =
        (seconds.toFx() * Fx::fromInt(static_cast<std::int32_t>(ticksPerSecond))).floorToInt();
    return std::max(1, ticks);
}

Resources captureDemand(Mag targetBuildEnergy, int workTicks) noexcept {
    if (workTicks <= 0) {
        return Resources{};
    }
    return Resources{.mass = Mag{},
                     .energy = proportionalWork(targetBuildEnergy, Mag::fromInt(1),
                                                Mag::fromInt(workTicks))};
}

void syncCaptureWork(const UnitStore& store, const UnitCatalog& catalog,
                     std::span<const Army> armies, std::vector<CaptureWork>& captures,
                     TickRate rate) {
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::uint32_t ticksPerSecond = rate.ticksPerSecond();

    for (UnitIndex captor = 0; captor < orders.size(); ++captor) {
        if (!store.slotAlive(captor) || !store.health()[captor].alive()) {
            continue;
        }
        const QueuedCommand* head = orders[captor].active();
        const bool capturing = head != nullptr && head->kind() == CommandKind::Capture
            && store.alive(head->target()) && store.health()[head->target().index].alive()
            && capturableTarget(captor, head->target(), store, catalog, armies);
        auto found = std::ranges::find_if(captures, [captor](const CaptureWork& work) {
            return work.captor == captor;
        });
        if (!capturing) {
            if (found != captures.end()) {
                captures.erase(found);
            }
            continue;
        }
        // Only an in-reach captor draws demand: the entry persists while its head
        // stays on the same target (progress is kept while walking back into
        // reach), but unfunded beats advance nothing.
        const UnitId target = head->target();
        const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target.index));
        const Fx gap = groundDistanceElmos(positionOf(transforms[captor]),
                                           positionOf(transforms[target.index]));
        const bool inReach =
            gap <= repairReach(catalog, store.typeAt(captor), motion[captor],
                               motion[target.index]);
        if (found != captures.end() && found->target == target) {
            found->armyIndex = motion[captor].armyIndex;
            found->inReach = inReach;
            found->demand = inReach && targetDef != nullptr
                ? captureDemand(targetDef->buildCostEnergy, found->workTicks)
                : Resources{};
            continue;
        }
        if (found != captures.end()) {
            captures.erase(found);
        }
        // A fresh task: budget from the target's blueprint and the captor's
        // effective rate, like a construction records its own at order time.
        // Retargeting starts over rather than inheriting progress.
        const Mag ratePerTick = effectiveBuildPerTick(store, catalog, captor);
        const int budget = targetDef != nullptr
            ? captureWorkTicks(targetDef->buildTime, ratePerTick, ticksPerSecond)
            : 1;
        const Resources demand = inReach && targetDef != nullptr
            ? captureDemand(targetDef->buildCostEnergy, budget)
            : Resources{};
        captures.push_back(CaptureWork{.armyIndex = motion[captor].armyIndex,
                                       .captor = captor,
                                       .target = target,
                                       .workTicks = budget,
                                       .progress = 0,
                                       .demand = demand,
                                       .funded = Fx{},
                                       .inReach = inReach});
    }
    // Captors gone from the queue entirely (dead or reordered) leave orphans.
    std::erase_if(captures, [&](const CaptureWork& work) {
        if (work.captor >= orders.size()) {
            return true;
        }
        const QueuedCommand* head = orders[work.captor].active();
        return head == nullptr || head->kind() != CommandKind::Capture
            || !store.alive(head->target()) || head->target() != work.target;
    });
}

std::size_t applyCaptureWork(UnitStore& store, std::vector<CaptureWork>& captures,
                             EventQueue* events) {
    std::size_t capturing = 0;
    for (std::size_t i = 0; i < captures.size();) {
        CaptureWork& work = captures[i];
        const bool live = work.captor < store.slotCount() && store.slotAlive(work.captor)
            && store.health()[work.captor].alive() && store.alive(work.target)
            && store.health()[work.target.index].alive();
        if (!live) {
            captures.erase(captures.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // All-or-nothing per beat, like the retail task: a partially funded beat
        // advances nothing, and each funded in-reach beat adds one per active
        // captor. Reach gates explicitly: a zero demand is trivially "fully
        // funded" by the allocator, so the ratio alone cannot tell waiting apart
        // from free.
        if (!work.inReach || work.funded < kFxOne || work.workTicks <= 0) {
            ++i;
            continue;
        }
        ++capturing;
        work.progress = std::min(work.workTicks, work.progress + 1);
        if (work.progress < work.workTicks) {
            ++i;
            continue;
        }
        // Replacement-entity transfer, not an in-place rewrite: the old unit leaves
        // silently (no wreck, no kill credit — nothing died) and a replacement of
        // the same type, transform and health stands under the captor's army. The
        // captor's order retires on its own next dispatch: its target handle is now
        // stale, which is exactly what retires every other spent unit-target order.
        const UnitIndex victim = work.target.index;
        const UnitTypeIndex type = store.typeAt(victim);
        const Transform transform = store.transforms()[victim];
        const Health health = store.health()[victim];
        MoveState motion = store.motion()[victim];
        motion.armyIndex = work.armyIndex;
        motion.moving = false;
        motion.path.clear();
        motion.pathIndex = 0;
        store.kill(work.target);
        const UnitId replacement = store.spawn(UnitStore::Spawn{
            .type = type,
            .transform = transform,
            .motion = motion,
            .health = Health{.current = health.current, .maximum = health.maximum},
        });
        emit(events, Event{.kind = EventKind::UnitCreated,
                           .unit = replacement,
                           .army = motion.armyIndex,
                           .at = positionOf(transform)});
        captures.erase(captures.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return capturing;
}

std::vector<WorkClaim> collectCaptureClaims(const UnitStore& store) {
    std::vector<WorkClaim> claims;
    const std::span<const CommandQueue> orders = store.orders();
    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || !store.health()[slot].alive()) {
            continue;
        }
        const QueuedCommand* head = orders[slot].active();
        if (head == nullptr || head->kind() != CommandKind::Capture
            || !store.alive(head->target())) {
            continue;
        }
        claims.push_back(WorkClaim{.target = head->target().index,
                                   .workerArmy = motion[slot].armyIndex});
    }
    return claims;
}

} // namespace rm::sim
