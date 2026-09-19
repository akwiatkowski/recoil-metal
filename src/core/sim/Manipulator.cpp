#include "core/sim/Manipulator.hpp"

#include "core/sim/Army.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/Intel.hpp"
#include "core/sim/UnitStore.hpp"

#include <algorithm>
#include <vector>

namespace rm::sim {
namespace {

/// The stored fraction a `CStorageManipulator` slides on (`C-304`): stored over
/// capacity for the manipulator's resource, clamped to [0, 1]. Zero capacity
/// reads as empty — the pod shows nothing stored, which is the honest answer
/// when there is nowhere to put anything.
[[nodiscard]] Fx storedFraction(const Economy& economy, std::uint8_t resource) noexcept {
    const Mag stored = resource == 0 ? economy.stored.mass : economy.stored.energy;
    const Mag capacity = resource == 0 ? economy.storage.mass : economy.storage.energy;
    if (capacity <= Mag{} || stored <= Mag{}) {
        return Fx{};
    }
    return std::min(kFxOne, stored.toFx() / capacity.toFx());
}


} // namespace
void tickManipulators(UnitStore& store, std::span<const Economy> economies,
                      EventQueue* events) {
    const std::span<const MoveState> motion = store.motion();
    const std::span<std::vector<Manipulator>> lists = store.manipulators();
    (void)events;  // the collision manipulator's callbacks land with C-372

    for (UnitIndex slot = 0; slot < lists.size(); ++slot) {
        if (lists[slot].empty() || !store.slotAlive(slot)) {
            continue;
        }
        const int owner = motion[slot].armyIndex;
        // IN PRECEDENCE ORDER (`C-294`): the list is sorted at insert, so the
        // walk is the execution order retail's re-sort produces.
        for (Manipulator& manip : lists[slot]) {
            // `manip+0x4c` (`C-372`): a disabled manipulator keeps its state
            // but its update does not run — the build-open lifecycle parks the
            // builder arm this way.
            if (!manip.enabled) {
                continue;
            }
            switch (manip.kind) {
            case ManipulatorKind::StorageSlide: {
                // `C-304`: the offset is the authored travel scaled by the
                // owner's stored fraction. An army with no economy record — a
                // `--units` crowd — stores nothing, so the slide rests at zero.
                Fx fraction{};
                if (owner >= 0 && static_cast<std::size_t>(owner) < economies.size()) {
                    fraction = storedFraction(economies[static_cast<std::size_t>(owner)],
                                              manip.slideResource);
                }
                manip.slideOffset = manip.slideRange * fraction;
                break;
            }
            case ManipulatorKind::Collision:
                // `C-301`/`C-372`'s contact events land with the collision
                // slice — the latches serialize from the start, the edge
                // detection arrives with its own change.
                break;
            case ManipulatorKind::BuilderArm:
                // `C-249`'s arm angles ride `MoveState`'s turret pose like
                // every other aim state; the record's own state is the enable
                // byte, which the build-open lifecycle owns. Nothing to update.
                break;
            }
        }
    }
}

void tickEffects(UnitStore& store, std::vector<SimEmitter>* pool,
                 std::span<const Army> armies, const Intel* intel,
                 EventQueue* events) {
    if (pool == nullptr) {
        return;
    }
    // `C-298`'s attach means the record cannot outlive its bone: an emitter
    // whose unit died is retired, and the sweep runs before the spawn pass so
    // a recycled slot never inherits a dead unit's flash. Dead records are
    // erased rather than tombstoned — the pool is dense, and a dead emitter
    // has no state worth a save.
    std::erase_if(*pool, [&store](const SimEmitter& emitter) {
        return !emitter.alive || !store.alive(emitter.unit);
    });
    // `C-303`'s gate: an emitter riding a hidden bone is suppressed — retail's
    // `EmitIfVisible` — while the record stays alive for when the bone shows
    // again. Recomputed every beat because `ShowBone` can undo a hide.
    for (SimEmitter& emitter : *pool) {
        emitter.hidden = store.boneHiddenAt(emitter.unit.index, emitter.bone);
    }

    // The spawn trigger this slice implements: one emitter per shot, at the
    // muzzle bone (`CreateEmitterAtBone`, `0x677800` → `C-298`). The events are
    // collected first because the spawns emit `EffectEmitted` into the same
    // queue — iterating a vector being appended to is a use-after-invalidate.
    std::vector<Event> shots;
    if (events != nullptr) {
        for (const Event& event : events->all()) {
            if (event.kind == EventKind::WeaponFired) {
                shots.push_back(event);
            }
        }
    }
    for (const Event& shot : shots) {
        if (!store.alive(shot.unit)) {
            continue;
        }
        SimEmitter emitter{
            .unit = shot.unit,
            .bone = kMuzzleBone,
            .effect = shot.visualId,
            .alive = true,
            .hidden = store.boneHiddenAt(shot.unit.index, kMuzzleBone),
            .viewerMask = ~std::uint64_t{0},
        };
        // `C-374`'s per-army mask (`0x664ff0` → `0x665130`): bit `armyIndex`
        // set means that army's alliance sees the emitter's position. No
        // intel, or an unconfigured one, is the no-fog answer — everyone sees
        // it, which is what the all-ones default already says.
        if (intel != nullptr && intel->active()) {
            emitter.viewerMask = 0;
            for (const Army& army : armies) {
                if (army.index < 0 || army.index >= 64) {
                    continue;
                }
                if (intel->sees(army.alliance, IntelKind::Vision,
                                shot.at2[0], shot.at2[2])) {
                    emitter.viewerMask |= std::uint64_t{1} << army.index;
                }
            }
        }
        pool->push_back(emitter);
        // The emission event carries the mask (`C-374`): the scene's drain
        // reads which armies the emission is for rather than re-deriving it.
        emit(events, Event{.kind = EventKind::EffectEmitted,
                           .unit = shot.unit,
                           .army = shot.army,
                           .at = shot.at2,
                           .visualId = shot.visualId,
                           .bone = kMuzzleBone,
                           .viewerMask = emitter.viewerMask});
    }
}

} // namespace rm::sim
