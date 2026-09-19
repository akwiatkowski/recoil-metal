#include "core/sim/Capture.hpp"

#include "core/sim/Command.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/map/HeightField.hpp"
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

Fx captureEdgeDistance(const UnitCatalog& catalog, UnitTypeIndex captorType,
                       UnitTypeIndex targetType, Fx centreGap) noexcept {
    // `C-250`: the retail preamble subtracts each side's u8 footprint — the
    // larger of `Footprint.SizeX`/`SizeZ`, whole — from the centre distance.
    // `footprintSquaresX/Z` are those same ogrid counts (derived from
    // `Physics.SizeX/Z` when the blueprint states no Footprint block), so the
    // subtraction is `squares × kSquareSize` elmos per side.
    const auto footprintElmos = [&catalog](UnitTypeIndex type) {
        const unitdef::UnitDef* def = catalog.def(type);
        if (def == nullptr) {
            return Fx{};
        }
        return Fx::fromInt(std::max(def->footprintSquaresX, def->footprintSquaresZ)
                           * kSquareSize);
    };
    return centreGap - footprintElmos(captorType) - footprintElmos(targetType);
}


void syncCaptureWork(const UnitStore& store, const UnitCatalog& catalog,
                     std::span<const Army> armies, std::vector<CaptureWork>& captures,
                     TickRate rate, EventQueue* events) {
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
                // `C-237` (spec §"Deactivation reports failure"): an active task
                // ending on a LIVE target fires `OnFailedBeingCaptured`/
                // `OnFailedCapture` — the order cancelled, the target turned
                // uncapturable, or the captor walked off. A dead target's arm is
                // a no-op, which is why a completed transfer fires none.
                if (found->inReach && store.alive(found->target)) {
                    const Transform& at = transforms[found->target.index];
                    emit(events, Event{.kind = EventKind::FailedBeingCaptured,
                                       .unit = found->target,
                                       .instigator = store.idAt(captor),
                                       .army = motion[found->target.index].armyIndex,
                                       .at = {at.x, at.y, at.z}});
                    emit(events, Event{.kind = EventKind::FailedCapture,
                                       .unit = store.idAt(captor),
                                       .instigator = found->target,
                                       .army = motion[captor].armyIndex,
                                       .at = {at.x, at.y, at.z}});
                }
                captures.erase(found);
            }
            continue;
        }
        // Only an in-reach captor draws demand: the entry persists while its head
        // stays on the same target (progress is kept while walking back into
        const UnitId target = head->target();
        const unitdef::UnitDef* targetDef = catalog.def(store.typeAt(target.index));
        const Fx gap = groundDistanceElmos(positionOf(transforms[captor]),
                                           positionOf(transforms[target.index]));
        // `C-250`: work admits inside a 10-ogrid footprint-edge gap — wider
        // than the 5-ogrid approach stop, so a captor still closing banks
        // funded progress on the way in, exactly like the retail task.
        const bool inReach =
            captureEdgeDistance(catalog, store.typeAt(captor), store.typeAt(target.index),
                                gap) <= kCaptureWorkEdgeElmos;
        // `C-237` (`0x0060B942`-`0x0060B968`): a task ACTIVATES when its captor
        // comes in reach — retail flips `UNITSTATE_BeingCaptured` and bumps the
        // target's `+0x690` count there — and activation is what fires the start
        // pair: target `OnStartBeingCaptured` first, then captor `OnStartCapture`.
        // `inReach` is the activation flag's persistent form, so the edge is the
        // transition into it; a restored task with `inReach` already set does not
        // re-fire, which is the saved-game behaviour for free.
        const bool wasInReach = found != captures.end() && found->inReach;
        if (inReach && !wasInReach) {
            const Transform& at = transforms[target.index];
            emit(events, Event{.kind = EventKind::StartBeingCaptured,
                               .unit = target,
                               .instigator = store.idAt(captor),
                               .army = motion[target.index].armyIndex,
                               .at = {at.x, at.y, at.z}});
            emit(events, Event{.kind = EventKind::StartCapture,
                               .unit = store.idAt(captor),
                               .instigator = target,
                               .army = motion[captor].armyIndex,
                               .at = {at.x, at.y, at.z}});
        }
        if (!inReach && wasInReach && store.alive(found->target)) {
            // Deactivation on a LIVE target reports failure, not stop (spec
            // §"Deactivation reports failure"): `OnFailedBeingCaptured` on the
            // target, `OnFailedCapture` on the captor. A dead target's arm is a
            // no-op — which is why a completed transfer's destruction fires none.
            const Transform& at = transforms[found->target.index];
            emit(events, Event{.kind = EventKind::FailedBeingCaptured,
                               .unit = found->target,
                               .instigator = store.idAt(captor),
                               .army = motion[found->target.index].armyIndex,
                               .at = {at.x, at.y, at.z}});
            emit(events, Event{.kind = EventKind::FailedCapture,
                               .unit = store.idAt(captor),
                               .instigator = found->target,
                               .army = motion[captor].armyIndex,
                               .at = {at.x, at.y, at.z}});
        }
        // A production-paused captor asks for nothing, like one out of reach: the
        // task and its progress survive, but no funded beat accumulates under it.
        const bool funded = inReach && !store.productionPaused(store.idAt(captor));
        if (found != captures.end() && found->target == target) {
            found->armyIndex = motion[captor].armyIndex;
            found->inReach = inReach;
            found->demand = funded && targetDef != nullptr
                ? captureDemand(targetDef->buildCostEnergy, found->workTicks)
                : Resources{};
            continue;
        }
        if (found != captures.end()) {
            // Retargeting deactivates the old task on its old target — the same
            // live-target failure pair as any other deactivation.
            if (found->inReach && store.alive(found->target)) {
                const Transform& at = transforms[found->target.index];
                emit(events, Event{.kind = EventKind::FailedBeingCaptured,
                                   .unit = found->target,
                                   .instigator = store.idAt(captor),
                                   .army = motion[found->target.index].armyIndex,
                                   .at = {at.x, at.y, at.z}});
                emit(events, Event{.kind = EventKind::FailedCapture,
                                   .unit = store.idAt(captor),
                                   .instigator = found->target,
                                   .army = motion[captor].armyIndex,
                                   .at = {at.x, at.y, at.z}});
            }
            captures.erase(found);
        }
        // A fresh task: budget from the target's blueprint and the captor's
        // effective rate, like a construction records its own at order time.
        // Retargeting starts over rather than inheriting progress.
        const Mag ratePerTick = effectiveBuildPerTick(store, catalog, captor);
        const int budget = targetDef != nullptr
            ? captureWorkTicks(targetDef->buildTime, ratePerTick, ticksPerSecond)
            : 1;
        const Resources demand = funded && targetDef != nullptr
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
    // Captors gone from the queue entirely (dead or reordered) leave orphans —
    // the same live-target failure pair as the in-loop deactivations.
    std::erase_if(captures, [&](const CaptureWork& work) {
        bool orphan = work.captor >= orders.size();
        if (!orphan) {
            const QueuedCommand* head = orders[work.captor].active();
            orphan = head == nullptr || head->kind() != CommandKind::Capture
                || !store.alive(head->target()) || head->target() != work.target;
        }
        if (orphan && work.inReach && store.alive(work.target)) {
            const Transform& at = transforms[work.target.index];
            emit(events, Event{.kind = EventKind::FailedBeingCaptured,
                               .unit = work.target,
                               .instigator = store.idAt(work.captor),
                               .army = motion[work.target.index].armyIndex,
                               .at = {at.x, at.y, at.z}});
            emit(events, Event{.kind = EventKind::FailedCapture,
                               .unit = store.idAt(work.captor),
                               .instigator = work.target,
                               .army = work.armyIndex,
                               .at = {at.x, at.y, at.z}});
        }
        return orphan;
    });
}

std::size_t applyCaptureWork(UnitStore& store, std::vector<CaptureWork>& captures,
                             EventQueue* events, std::span<SiloAmmo> siloAmmo,
                             std::span<EnhancementWork> enhancements) {
    std::size_t capturing = 0;
    for (std::size_t i = 0; i < captures.size();) {
        CaptureWork& work = captures[i];
        const bool live = work.captor < store.slotCount() && store.slotAlive(work.captor)
            && store.health()[work.captor].alive() && store.alive(work.target)
            && store.health()[work.target.index].alive();
        if (!live) {
            // `C-237` (spec §"Deactivation reports failure"): teardown on a dead
            // captor fires the failure pair on a LIVE target; a dead target's
            // arm is a no-op — which is why a completed transfer fires none.
            if (work.inReach && store.alive(work.target)
                && store.health()[work.target.index].alive()) {
                const Transform& at = store.transforms()[work.target.index];
                emit(events, Event{.kind = EventKind::FailedBeingCaptured,
                                   .unit = work.target,
                                   .instigator = store.idAt(work.captor),
                                   .army = store.motion()[work.target.index].armyIndex,
                                   .at = {at.x, at.y, at.z}});
                emit(events, Event{.kind = EventKind::FailedCapture,
                                   .unit = store.idAt(work.captor),
                                   .instigator = work.target,
                                   .army = work.armyIndex,
                                   .at = {at.x, at.y, at.z}});
            }
            captures.erase(captures.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // funded" by the allocator, so the ratio alone cannot tell waiting apart
        // from free.
        if (!work.inReach || work.funded < kFxOne || work.workTicks <= 0) {
            ++i;
            continue;
        }
        // `C-243`: progress adds the TARGET's active-captor count, not one — retail's
        // `Unit+0x690`, incremented when a capture task activates on the target and
        // decremented when it deactivates. Every in-reach task on the same target is
        // active, so two captors on one victim each bank two per funded beat and the
        // capture lands in half the time.
        int activeCaptors = 0;
        for (const CaptureWork& other : captures) {
            if (other.target == work.target && other.inReach) {
                ++activeCaptors;
            }
        }
        ++capturing;
        work.progress = std::min(work.workTicks, work.progress + std::max(1, activeCaptors));
        if (work.progress < work.workTicks) {
            ++i;
            continue;
        }
        // `C-237` (`0x0060B822`-`0x0060B870`): the completion triple, in retail's
        // order — captor `OnStopCapture`, target `OnStopBeingCaptured`, then
        // target `OnCaptured`, whose Lua body performs the transfer. The native
        // transfer below stands in for that body, so `Captured` precedes the
        // replacement's `UnitCreated` exactly as retail's does.
        {
            const Transform& at = store.transforms()[work.target.index];
            emit(events, Event{.kind = EventKind::StopCapture,
                               .unit = store.idAt(work.captor),
                               .instigator = work.target,
                               .army = work.armyIndex,
                               .at = {at.x, at.y, at.z}});
            emit(events, Event{.kind = EventKind::StopBeingCaptured,
                               .unit = work.target,
                               .instigator = store.idAt(work.captor),
                               .army = store.motion()[work.target.index].armyIndex,
                               .at = {at.x, at.y, at.z}});
            emit(events, Event{.kind = EventKind::Captured,
                               .unit = work.target,
                               .instigator = store.idAt(work.captor),
                               .army = store.motion()[work.target.index].armyIndex,
                               .at = {at.x, at.y, at.z}});
        }
        const UnitId replacement =
            transferUnitArmy(store, work.target, work.armyIndex, events, siloAmmo,
                             enhancements);
        (void)replacement;
        captures.erase(captures.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return capturing;
}

UnitId transferUnitArmy(UnitStore& store, UnitId unit, int armyIndex,
                        EventQueue* events, std::span<SiloAmmo> siloAmmo,
                        std::span<EnhancementWork> enhancements) {
    if (!store.alive(unit)) {
        return UnitId{};
    }
    // Replacement-entity transfer, not an in-place rewrite: the old unit leaves
    // silently (no wreck, no kill credit — nothing died) and a replacement of
    // the same type, transform and health stands under the new army. The
    // giver's orders die with the handle: every unit-target order pointing at
    // it reads stale, which is exactly what retires spent orders everywhere.
    const UnitIndex victim = unit.index;
    const UnitTypeIndex type = store.typeAt(victim);
    const Transform transform = store.transforms()[victim];
    const Health health = store.health()[victim];
    MoveState motion = store.motion()[victim];
    // `SimUtils.lua:68-132` restores more than the health record after
    // `ChangeUnitArmy`: the installed enhancement names (re-run through
    // `CreateEnhancement`) and the shield on/off toggle. The spawn clears
    // both, so they are snapshotted here and restored on the replacement —
    // the health record above already carries shield health, veterancy
    // and fuel.
    const auto installedEnhancements = store.enhancements()[victim];
    const bool shieldToggledOff = store.scriptBitDisabledAt(victim, 0);
    motion.armyIndex = armyIndex;
    motion.moving = false;
    motion.path.clear();
    store.kill(unit);
    const UnitId replacement = store.spawn(UnitStore::Spawn{
        .type = type,
        .transform = transform,
        .motion = motion,
        // `C-240`/`SimUtils.lua:68-132`: the replacement keeps the WHOLE
        // health record — shield state, reload/burst clocks, automatic
        // targets, veterancy — not just current/maximum.
        .health = health,
    });
    store.enhancements()[replacement.index] = installedEnhancements;
    if (shieldToggledOff) {
        (void)store.setScriptBitDisabled(replacement, 0, true);
    }
    // And the match-side records that keyed on the old handle follow it:
    // Lua restores silo ammo and in-flight enhancement work onto the
    // replacement, so a transferred silo keeps its stockpile and a
    // half-built enhancement keeps its progress.
    for (SiloAmmo& ammo : siloAmmo) {
        if (ammo.owner == unit) {
            ammo.owner = replacement;
        }
    }
    for (EnhancementWork& work_ : enhancements) {
        if (work_.owner == unit) {
            work_.owner = replacement;
        }
    }
    emit(events, Event{.kind = EventKind::UnitCreated,
                       .unit = replacement,
                       .army = motion.armyIndex,
                       .at = positionOf(transform)});
    return replacement;
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
