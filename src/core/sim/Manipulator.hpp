#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rm::sim {

class UnitStore;
class Intel;
struct Army;
struct Economy;
class EventQueue;
class UnitCatalog;
struct Construction;

/// No bone: an attachment sits at its carrier's origin. Namespace scope (rather
/// than a class constant) so nested record initializers may name it.
inline constexpr std::int32_t kNoBone = -1;

// Sim-side animation manipulators — retail's `IAniManipulator` list.
//
// WHY THIS EXISTS (C-293, WP-41). Retail serializes eleven manipulator classes
// plus `MotorFallDown` as per-unit sim state, ticked once per sim tick from
// `Unit::MotionTick` via `CAniActor::UpdateManipulators` (`0x641550`, sole
// caller `0x6afb65`). They are NOT presentation: the pose they write is what a
// save restores and a replay hashes, which is why they live in `core/sim` and
// not `core/scene`.
//
// THE BOUNDED SLICE. This engine owns no skeleton — bones reach the sim only as
// indices and rest offsets (see `UnitStore::AttachBones`) — so each manipulator
// is a flat record rather than a pose writer. What each kind keeps is the state
// retail serializes: the storage slide's current offset, the collision
// manipulator's contact latches. `BoneVisibility` is deliberately NOT a kind:
// retail's `Unit::HideBone`/`ShowBone` write the pose's flag map directly
// (`C-303`, `0x6d820b`), so the mask lives on the store beside this list rather
// than inside a record in it.

/// Which manipulator a record is. Flat like `EventKind`: the record is
/// fixed-size and trivially copyable so a per-unit list of them serializes and
/// hashes like the rest of sim state.
enum class ManipulatorKind : std::uint8_t {
    /// `CStorageManipulator` (`C-304`): slides a bone as a function of the
    /// owner's stored-resource fraction — the storage pods that fill and empty
    /// visually with MASS/ENERGY. `slideOffset` is the serialized pose output.
    StorageSlide,
    /// `CCollisionManipulator` (`C-301`/`C-372`): fires the anim-collision
    /// callbacks when the unit's collision volume touches terrain or another
    /// unit. `inTerrainContact`/`inUnitContact` are the serialized latches the
    /// edge detection is computed against.
    Collision,
    /// `CBuilderArmManipulator` (`C-249`): the two-axis yaw/pitch rig a factory
    /// aims at its build site. The record carries the enable byte; the arm's
    /// own angles ride `MoveState`'s turret pose like every other aim state.
    BuilderArm,
};

/// One manipulator on one unit.
///
/// FLAT, with fields only some kinds use — the same shape `Event` takes and for
/// the same reason: a queue of them serializes and hashes without a variant's
/// largest-case cost in every slot. Which fields each kind fills is documented
/// on the kind above.
struct Manipulator {
    ManipulatorKind kind = ManipulatorKind::Collision;

    /// Execution order within the unit's list (`C-294`): retail's
    /// `IAniManipulator::SetPrecedence` (`0x642610`) writes `manip+0x58` and
    /// re-sorts the actor's list. The store keeps the list sorted on insert so
    /// the tick walks it in precedence order without re-sorting per beat.
    /// Foot-plant registers at 10 in retail (`Unit.lua:2665`); collision and
    /// storage ride the default 0, and the builder arm sits at 5.
    std::int32_t precedence = 0;

    /// Retail's enabled byte (`manip+0x4c`, `C-372`): a disabled manipulator
    /// stays in the list and keeps its state but its update does not run —
    /// `ManipulatorUpdate` is gated on it at `0x6415eb`. The build-open
    /// lifecycle toggles the builder arm through this.
    bool enabled = true;

    // --- StorageSlide ---------------------------------------------------------
    /// Which resource drives the slide: 0 = mass, 1 = energy — the
    /// `U4EconResource` selector retail stores at `manip+0xa8`.
    std::uint8_t slideResource = 0;
    /// The bone the slide moves.
    std::int32_t slideBone = kNoBone;
    /// The full travel, in elmos: the offset is `slideRange × stored/capacity`.
    Fx slideRange{};
    /// The serialized pose output — this tick's computed offset.
    Fx slideOffset{};

    // --- Collision ------------------------------------------------------------
    /// The contact latches the edge events are fired against (`C-372`'s
    /// `manip+0x84`/`+0x85` gates). Serialized, so a save mid-contact does not
    /// re-fire the entry callback on restore.
    bool inTerrainContact = false;
    bool inUnitContact = false;
};

/// One live emitter in the sim's effect manager — the bounded slice of retail's
/// `CEffectManagerImpl` (`C-296`), a `Sim` member at `+0x8C0` ticked in
/// `Sim::AdvanceBeat` and serialized through `SerEffects`.
///
/// MATCH-OWNED like the projectile list, for the same reason: the sim authors
/// the records, the caller owns the storage (`Match::effects`), and a scene
/// with no effects passes null. Emitters attach to bones sim-side
/// (`CreateEmitterAtBone`, `0x677800` → `C-298`) and die with their unit.
struct SimEmitter {
    /// The unit whose bone this rides. A handle, not a slot: the pool outlives
    /// the unit by at most a tick, and a stale handle must not attach a dead
    /// unit's muzzle flash to the slot's next occupant.
    UnitId unit{};
    /// The bone index it attaches to. `kMuzzleBone` for the weapon-fire
    /// trigger this slice implements.
    std::int32_t bone = kNoBone;
    /// Which effect, as authored content — the emitter's blueprint name. A
    /// string like `Event::visualId`: the sim names it, the scene resolves it.
    std::string effect;
    /// Live flag rather than erase-on-death: the pool is serialized, and a
    /// tombstone keeps the record's slot stable for the tick it died on. The
    /// tick sweeps dead records after reporting them.
    bool alive = true;
    /// Whether the bone it rides is currently hidden (`C-303`): a hidden
    /// attachment point suppresses emission — retail's `EmitIfVisible` gate —
    /// while the record itself stays alive.
    bool hidden = false;
    /// `C-374`'s per-army visibility mask, stamped at spawn: bit `armyIndex`
    /// set means that army could see the emitter's position when it was
    /// created. Retail builds the mask per client (`0x664ff0` → `0x665130`);
    /// ours is computed once and carried, because the sim has no local client.
    std::uint64_t viewerMask = ~std::uint64_t{0};
};

/// The bone index a weapon-fire emitter attaches to. The sim owns no skeleton,
/// so the muzzle is a conventional index — the same one `boneHiddenAt` answers
/// for — rather than a resolved name.
inline constexpr std::int32_t kMuzzleBone = 0;

/// The manipulator tick — retail's `CAniActor::UpdateManipulators` beat
/// (`C-293`), run once per sim tick from the motion stage.
///
/// Walks every live unit's list IN PRECEDENCE ORDER (`C-294`) and updates each
/// enabled record: storage slides recompute their offset from the owner's
/// stored fraction (`C-304`), collision manipulators compare their latches
/// against the post-movement world and emit the anim-collision events on the
/// transitions (`C-301`/`C-372`).
/// PLACEMENT: after movement, collisions and congestion — the contact tests
/// read where units ENDED the tick, which is the same rule the intel stamp
/// follows. `economies` may be empty (a `--units` crowd): storage slides then
/// hold their offset at zero, the honest reading of "nothing stored".
void tickManipulators(UnitStore& store, std::span<const Economy> economies,
                      EventQueue* events);

/// The builder-arm lifecycle — `C-249`'s two halves that the tick can own
/// without a skeleton. Retail creates the `CBuilderArmManipulator` in
/// `SetupBuildBones` only when all three `General.BuildBones` references
/// exist (`builderArm.exists()`), and the factory's build-open animation
/// disables it while the bay opens and re-enables it when the arm may aim.
/// With no animation timeline the honest proxy for "the bay is open" is
/// "the unit has live construction work": a builder named on an unfinished
/// `Construction` row has its arm enabled; idle it is parked disabled.
/// Runs before `tickManipulators` so a freshly parked arm does not update.
void syncBuilderArms(UnitStore& store, const UnitCatalog& catalog,
                     std::span<const Construction> building);


/// The effect-manager beat — retail's `Sim+0x8C0` tick in `AdvanceBeat`
/// (`C-296`, `0x65fda0`).
///
/// Two jobs, in order: sweep the pool — emitters whose unit died are retired
/// (`C-298`'s attach means the record cannot outlive its bone) — then spawn
/// from this tick's `WeaponFired` events, one emitter per shot at the muzzle
/// bone, stamping `C-374`'s viewer mask from `intel` (null or inactive intel
/// means everyone sees it, the no-fog answer). Each spawn reports an
/// `EffectEmitted` event carrying the mask, so the scene's drain knows which
/// armies the emission is for.
///
/// `pool` is caller-owned (`Match::effects`); null means the scene keeps no
/// effect state and the whole beat is a branch, like `Match::projectiles`.
void tickEffects(UnitStore& store, std::vector<SimEmitter>* pool,
                 std::span<const Army> armies, const Intel* intel,
                 EventQueue* events);

} // namespace rm::sim
