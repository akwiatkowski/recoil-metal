#pragma once

#include "core/Types.hpp"
#include "core/sim/CommandQueue.hpp"
#include "core/sim/Health.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Manipulator.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/SpatialGrid.hpp"
#include "core/sim/Transform.hpp"
#include "core/sim/RandomStream.hpp"

#include <cstddef>
#include <array>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rm::sim {

/// `C-265`'s "not yet armed" marker for `UnitStore::lifetimeRemainingTicks`:
/// the tick's lifetime pass writes the blueprint's `Lifetime` here on first
/// sight of the slot. `std::numeric_limits<TickCount>::max()` rather than a
/// second flag array — a unit can never legitimately outlive it.
inline constexpr TickCount kLifetimeUnset = std::numeric_limits<TickCount>::max();

/// A retreating unit's live bookkeeping: where it broke off, and which builder it
/// ran to. `active` is what separates "owes the front a return trip" from a slot
/// that never flinched.
struct RetreatState {
    bool active = false;
    Fx returnX{}, returnZ{};
    Fx toX{}, toZ{};
};

/// Retail's `INTEL_` enum (`enum_registrations.tsv`, `0x00510f60`): the type
/// argument `Entity:EnableIntel`/`DisableIntel`/`IsIntelEnabled` dispatch on
/// (`0x00694B60`, `0x00694C56`, `C-283`). Numbering is retail's — types 0–8 are
/// the grid-backed senses and fields `CIntel` holds handles for, types ≥9 the
/// self-intel statuses kept as (enabled, active) byte pairs on the unit's
/// attributes object (`entity+0x1E0`: Jammer `+0x24`, Spoof `+0x26`, Cloak
/// `+0x28`, RadarStealth `+0x2A`, SonarStealth `+0x2C`).
enum class IntelType : std::uint8_t {
    None = 0,
    Vision = 1,
    WaterVision = 2,
    Radar = 3,
    Sonar = 4,
    Omni = 5,
    RadarStealthField = 6,
    SonarStealthField = 7,
    CloakField = 8,
    Jammer = 9,
    Spoof = 10,
    Cloak = 11,
    RadarStealth = 12,
    SonarStealth = 13,
};

inline constexpr std::size_t kIntelTypeCount = 14;

/// One type's bit in a per-unit intel mask.
[[nodiscard]] constexpr std::uint16_t intelTypeBit(IntelType type) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(type));
}

/// The intel types `RULEUTC_IntelToggle` (script bit 3) switches — every
/// non-vision type, verbatim from `Unit.lua`'s `OnScriptBitSet` (lines
/// 318-332): both stealths, both fields, sonar, omni, cloak, cloak field,
/// spoof, jammer, radar. Vision is not in the group; retail's intel toggle
/// leaves the eyeball on.
inline constexpr std::uint16_t kIntelToggleMask =
    static_cast<std::uint16_t>(intelTypeBit(IntelType::Radar)
                               | intelTypeBit(IntelType::Sonar)
                               | intelTypeBit(IntelType::Omni)
                               | intelTypeBit(IntelType::RadarStealthField)
                               | intelTypeBit(IntelType::SonarStealthField)
                               | intelTypeBit(IntelType::CloakField)
                               | intelTypeBit(IntelType::Jammer)
                               | intelTypeBit(IntelType::Spoof)
                               | intelTypeBit(IntelType::Cloak)
                               | intelTypeBit(IntelType::RadarStealth)
                               | intelTypeBit(IntelType::SonarStealth));

/// The intel types `RULEUTC_StealthToggle` (script bit 5) switches — the
/// stealth family only (`Unit.lua` lines 337-341).
inline constexpr std::uint16_t kStealthToggleMask =
    static_cast<std::uint16_t>(intelTypeBit(IntelType::RadarStealth)
                               | intelTypeBit(IntelType::RadarStealthField)
                               | intelTypeBit(IntelType::SonarStealth)
                               | intelTypeBit(IntelType::SonarStealthField));

/// The intel-type mask a `RULEUTC_*` disabled mask implies — the Lua mapping
/// flattened: bit 2 jams, bit 3 the whole non-vision suite, bit 5 the stealth
/// family, bit 8 the cloak. Bits 0/1/4/6/7 carry no intel type.
[[nodiscard]] constexpr std::uint16_t scriptBitIntelMask(
    std::uint16_t scriptBits) noexcept {
    std::uint16_t mask = 0;
    if ((scriptBits & (1u << 2)) != 0) {
        mask = static_cast<std::uint16_t>(mask | intelTypeBit(IntelType::Jammer));
    }
    if ((scriptBits & (1u << 3)) != 0) {
        mask = static_cast<std::uint16_t>(mask | kIntelToggleMask);
    }
    if ((scriptBits & (1u << 5)) != 0) {
        mask = static_cast<std::uint16_t>(mask | kStealthToggleMask);
    }
    if ((scriptBits & (1u << 8)) != 0) {
        mask = static_cast<std::uint16_t>(mask | intelTypeBit(IntelType::Cloak));
    }
    return mask;
}

// Every unit in a match, in one place.
//
// WHY THIS EXISTS. Unit state is currently three parallel deques of per-batch vectors in an
// anonymous namespace in `main.mm` — `instances`, `motion`, `health` — that must stay
// index-locked, addressed as `[batch][instance]` where a batch is one instanced draw call.
// So "which unit is this" is answered by "which draw call, and which slot of its instance
// buffer", and a unit list cannot be walked without knowing the render batching (PLAN2.md
// §1.1). This replaces that.
//
// STRUCTURE OF ARRAYS, NOT AN ARRAY OF STRUCTS, and this is a deliberate deviation from the
// `Unit` sketch in PLAN2.md §2. Three reasons, in order of weight:
//
//   1. The passes already take spans of exactly these arrays, so the migration in P1.4 is a
//      change of WHICH span rather than a rewrite of every pass. That is the difference
//      between a phase that can be verified step by step and one that cannot.
//   2. The arrays are what the renderer's upload wants: a gather over one contiguous array
//      per frame rather than a strided walk over an array of structs. (This reason used to
//      read "keeping `UnitInstance` contiguous means the upload stays a memcpy". P2.2 ended
//      that: the store holds `Transform` now, the instance is built at draw time, and the
//      gather it was trying to avoid is the gather P1.4 introduced anyway.)
//   3. It matches how the sim already treats death, which is the property the golden replay
//      log depends on — see below.
//
// An `Unit` view over these arrays is the right shape for callers that want to talk about
// one unit, and is cheap to add on top. It is not needed yet.
//
// DEATH IS A TOMBSTONE, NOT A REMOVAL. `retireDead` (Skirmish.cpp) zeroes a dead unit's
// radius and leaves it where it is; nothing is ever erased and no unit ever moves slot. That
// is not an accident to be tidied up — it is what makes iteration order stable, and the
// replay hash is order-sensitive on purpose (PLAN2.md §7 P1.0). A store that compacted on
// death would reorder the survivors, change every subsequent hash, and turn the one tool
// that makes this phase safe into noise. So: slots are permanent for the life of the match,
// `IdPool` decides which are live, and a pass skips the dead.
//
// The store owns no definitions. A unit carries a `UnitTypeIndex` and the catalog holds the
// rest, so the store never touches the VFS and a test can build one from two structs.
class UnitStore {
public:
    /// What a new unit needs. Grouped rather than passed as eight arguments because the
    /// order of eight floats is exactly the kind of thing a caller gets silently wrong.
    struct Spawn {
        UnitTypeIndex type = 0;
        Transform transform{};
        MoveState motion{};
        Health health{};
    };

    /// State required to resume the unit slots without changing their identities or the next
    /// allocator result. The spatial index is derived and rebuilt after restoring.
    struct Snapshot {
        IdPool::Snapshot ids;
        std::vector<Generation> generations;
        std::vector<Transform> transforms;
        std::vector<MoveState> motion;
        std::vector<Health> health;
        std::vector<UnitTypeIndex> types;
        std::vector<bool> factoryRepeat;
        std::vector<bool> productionPaused;
        std::vector<BuildPriority> buildPriority;
        std::vector<RetreatThreshold> retreatThreshold;
        std::vector<TargetFocus> targetFocus;
        std::vector<RetreatState> retreats;
        std::vector<bool> doNotTarget;
        std::vector<std::uint16_t> scriptBitsDisabled;
        /// `C-283`'s per-slot intel-disable masks (see `setIntelEnabled`) —
        /// the explicit `EnableIntel`/`DisableIntel` writes only; the
        /// `RULEUTC_*` toggles' contribution is derived from
        /// `scriptBitsDisabled` at read time.
        std::vector<std::uint16_t> intelDisabled;
        std::vector<bool> maintenanceActive;
        std::vector<std::optional<UnitId>> parents;
        std::vector<std::vector<UnitId>> children;
        std::vector<std::array<Fx, 2>> attachmentOffsets;
        std::vector<Fx> attachmentHeights;
        std::vector<std::int32_t> attachmentParentBones;
        std::vector<std::int32_t> attachmentSelfBones;
        std::vector<std::array<Fx, 2>> attachmentParentRest;
        std::vector<Fx> attachmentParentRestHeights;
        std::vector<std::array<Fx, 2>> attachmentSelfRest;
        std::vector<Fx> attachmentSelfRestHeights;
        /// `C-265`'s per-slot lifetime countdowns (see `lifetimeRemainingTicks`).
        std::vector<TickCount> lifetimeRemainingTicks;
        CommandSerial nextCommandSerial = 0;
        std::array<std::uint32_t, kInvalidCommandSource> nextCommandCounters{};
        std::vector<SharedCommand> sharedCommands;
        std::vector<CommandQueue::Snapshot> orders;
        /// `C-293`'s per-slot manipulator lists (see `manipulators`) and
        /// `C-303`'s per-slot bone-visibility masks (see `setBoneHidden`), both
        /// from SaveState v44.
        std::vector<std::vector<Manipulator>> manipulators;
        std::vector<std::vector<std::uint64_t>> boneHidden;
        std::vector<std::map<std::string, std::string>> enhancements;
    };

    UnitStore() = default;
    explicit UnitStore(const Snapshot& snapshot);

    [[nodiscard]] Snapshot snapshot() const;

    /// Adds a unit and returns its handle.
    ///
    /// Reuses a dead unit's slot when one is free, which is what keeps a long match from
    /// growing an array per unit it has ever had. The handle is distinct from every handle
    /// ever issued for that slot (`IdPool`).
    [[nodiscard]] UnitId spawn(const Spawn& request);

    /// Marks a unit dead. Its slot stays put and its arrays keep their last values — see
    /// the note on tombstones above. Killing an already-dead unit does nothing.
    ///
    /// `random`, when given, is the sim stream `C-197`'s transport rule draws
    /// from: each attached cargo child rolls `r < 0.99` to die with the
    /// carrier, and the survivors detach where it fell. Without a stream —
    /// callers outside the match tick, which have no deterministic draw to
    /// offer — cargo dies unconditionally, the pre-`C-197` behaviour.
    void kill(UnitId id, RandomStream* random = nullptr);

    /// Retail's `unit:Destroy()` (`C-261`, `C-265`): the unit leaves WITHOUT the
    /// death path — no wreck, no `UnitDestroyed` event, no kill credit, no death
    /// weapon — which is what a crab egg hatching and an Othuy expiring both do.
    ///
    /// `kill` alone is NOT this: it releases the handle but leaves health and
    /// collision radius standing, so the corpse stays targetable and keeps
    /// shoving the living until its slot is recycled. Destroy zeroes the radius
    /// FIRST — that is `retireDead`'s once-per-death guard, so the corpse is
    /// never reported — then the health, then releases the handle. Attached
    /// cargo is destroyed with it, the way a carrier's load dies with the
    /// carrier.
    void destroy(UnitId id);

    [[nodiscard]] bool alive(UnitId id) const noexcept { return ids_.alive(id); }

    /// What a handle names right now — the script-object lifecycle seam (WP-03, C-044 to
    /// C-046). Retail has two meanings of "destroyed": a unit whose death is queued but whose
    /// native object still exists, which scripts may keep reading, and a unit whose object
    /// pointer has been nulled, which raises "Game object has been destroyed". The tombstone
    /// store already has both moments: health reaches zero in the tick's combat phase and the
    /// handle is released in `retireDead` at the tick's end. This names them.
    enum class HandleState : std::uint8_t {
        Alive,      ///< live unit, every field meaningful
        Destroyed,  ///< dead this tick, slot still resolvable for final-state reads (C-046)
        Stale,      ///< handle released; the slot may already belong to someone else (C-045)
    };
    struct Resolved {
        UnitIndex slot = 0;
        HandleState state = HandleState::Stale;
    };
    /// Resolved FRESH on every call and never cached, which is C-044's rule: the answer can
    /// change within a tick, and a cached slot would follow the slot's next occupant.
    [[nodiscard]] Resolved resolve(UnitId id) const noexcept {
        if (!ids_.alive(id)) {
            return Resolved{.slot = id.index, .state = HandleState::Stale};
        }
        const bool living = id.index < health_.size() && health_[id.index].alive();
        return Resolved{.slot = id.index,
                        .state = living ? HandleState::Alive : HandleState::Destroyed};
    }

    /// `C-195`'s per-child record: the bone index on the parent (`+0x08`) and on
    /// the child (`+0x0C`), plus each bone's authored REST offset from its unit's
    /// origin. Rest offsets are static model data resolved by the caller (the sim
    /// owns no skeleton); the default rides the carrier's origin exactly
    /// like the historical offset-only attachment.
    struct AttachBones {
        std::int32_t parent = kNoBone;
        std::int32_t self = kNoBone;
        std::array<Fx, 2> parentRest{};
        Fx parentRestHeight{};
        std::array<Fx, 2> selfRest{};
        Fx selfRestHeight{};
    };

    /// The `AttachBones::parent` value a tractor claw's victim carries
    /// (`C-381`, `aeonweapons.lua` `ADFTractorClaw.TractorThread`). Retail's
    /// `AttachBoneTo(-1, unit, muzzle)` is a real bone attach, but the sim owns
    /// no skeleton — the sentinel marks the attachment as script cargo rather
    /// than transport cargo, which is what `kill`/`destroy` read to drop the
    /// victim ALIVE instead of rolling it through `C-197`'s 99% cascade.
    /// `kNoBone - 1` so it can never collide with a resolved bone index.
    static constexpr std::int32_t kTractorAttachBone = kNoBone - 1;

    /// Moves an attached child's stored offset — the slider half of the
    /// tractor claw (`C-381`): `CreateSlider(unit, muzzle)` retracts the bone
    /// the victim hangs from, which in boneless sim terms is this offset
    /// shrinking toward zero. `propagateAttachments` publishes the new world
    /// position on the next pass. No-op on a unit with no attachment.
    void setAttachmentOffset(UnitId child, std::array<Fx, 2> offset, Fx height) noexcept;

    /// Attaches a live child to a live parent. A child has exactly one parent, and an
    /// attachment may not introduce a cycle.
    [[nodiscard]] bool attach(UnitId parent, UnitId child);
    /// Bone-indexed form (`C-195`): rest offsets are static model data resolved by
    /// the caller, since the sim owns no skeleton.
    [[nodiscard]] bool attach(UnitId parent, UnitId child, AttachBones bones);

    /// Removes a live child's attachment, if it has one.
    [[nodiscard]] bool detach(UnitId child);

    [[nodiscard]] std::optional<UnitId> parentOf(UnitId child) const noexcept;
    [[nodiscard]] const std::vector<UnitId>& childrenOf(UnitId parent) const noexcept;
    [[nodiscard]] std::array<Fx, 2> attachmentOffsetOf(UnitId child) const noexcept;
    [[nodiscard]] Fx attachmentHeightOf(UnitId child) const noexcept;
    [[nodiscard]] AttachBones attachmentBonesOf(UnitId child) const noexcept;


    /// Updates attached children from their parents' current transforms. The hierarchy is
    /// traversed parent before child so an attached chain receives one coherent transform.
    void propagateAttachments();

    /// Whether the unit in this slot is live. The form a pass wants, since a pass walks
    /// slots rather than carrying handles.
    [[nodiscard]] bool slotAlive(UnitIndex slot) const noexcept {
        return slot < generations_.size() && ids_.alive(UnitId{slot, generations_[slot]});
    }

    /// The handle currently occupying a slot. Stale-safe: for an empty slot the generation
    /// will not match, so `alive()` on the result is false.
    [[nodiscard]] UnitId idAt(UnitIndex slot) const noexcept {
        return slot < generations_.size() ? UnitId{slot, generations_[slot]} : UnitId{};
    }

    // --- The arrays, for the passes ------------------------------------------
    //
    // All the same length, all indexed by slot, all sparse. Mutable where a pass writes and
    // const where it reads, which is the same split `CombatGroup` and `SkirmishGroup`
    // already make and for the same reason.

    [[nodiscard]] std::span<Transform> transforms() noexcept { return transforms_; }
    [[nodiscard]] std::span<const Transform> transforms() const noexcept {
        return transforms_;
    }
    [[nodiscard]] std::span<MoveState> motion() noexcept { return motion_; }
    [[nodiscard]] std::span<const MoveState> motion() const noexcept { return motion_; }
    [[nodiscard]] std::span<Health> health() noexcept { return health_; }
    [[nodiscard]] std::span<const Health> health() const noexcept { return health_; }
    [[nodiscard]] std::span<const UnitTypeIndex> types() const noexcept { return types_; }

    /// Completed enhancements by authored slot, independent for every unit instance.
    [[nodiscard]] auto enhancements() noexcept { return std::span{enhancements_}; }
    [[nodiscard]] auto enhancements() const noexcept { return std::span{enhancements_}; }

    /// The orders each unit still has to carry out (PLAN2.md §6.4, §7 P4.1).
    ///
    /// ON THE UNIT, which is where the plan puts it and where Recoil puts it too — the
    /// alternative, one table keyed by handle, would need clearing on death and would make
    /// "walk every unit's queue" a hash lookup per slot in a pass that already has the slot.
    /// A dead unit's queue is cleared immediately because its shared ownership determines
    /// command-ID liveness. Position, motion, health, and type remain tombstone state.
    [[nodiscard]] std::span<CommandQueue> orders() noexcept { return orders_; }
    [[nodiscard]] std::span<const CommandQueue> orders() const noexcept { return orders_; }

    /// Assigns the immutable creation clock carried by an accepted command. Match-global, as in
    /// retail: recycling a unit slot or clearing one queue must not restart the clock.
    [[nodiscard]] CommandSerial allocateCommandSerial() noexcept {
        return nextCommandSerial_++;
    }
    [[nodiscard]] CommandSerial nextCommandSerial() const noexcept { return nextCommandSerial_; }

    /// Allocates the next source-tagged ID not currently owned by any queue.
    [[nodiscard]] std::optional<CommandId> allocateCommandId(CommandSource source);

    /// Consumes the next source-local ID while replaying an explicit semantic issue. The ID
    /// must be exactly what live allocation would have produced, so replay reconstructs and
    /// validates the hashed allocator state instead of merely injecting an identity.
    [[nodiscard]] bool consumeCommandId(CommandSource source, CommandId id);

    /// Registers an accepted shared command weakly. Duplicate live IDs are refused.
    [[nodiscard]] bool registerCommand(const std::shared_ptr<SharedCommand>& command);
    [[nodiscard]] bool commandIdLive(CommandId id);
    [[nodiscard]] std::shared_ptr<SharedCommand> liveCommand(CommandId id);
    [[nodiscard]] std::size_t liveCommandCount();

    /// Per-live-unit factory repeat execution state. Command dispatch reads this only when a
    /// mobile factory product completes; semantic command intake owns the choice to change it.
    [[nodiscard]] bool setFactoryRepeat(UnitId unit, bool enabled) noexcept;
    [[nodiscard]] bool factoryRepeat(UnitId unit) const noexcept;

    /// Per-live-unit production pause. The ONE authoritative flag: every economy consumer —
    /// construction, silo ammunition, enhancements, repair, capture, reclaim, assistance
    /// and the unit's own income — reads it (directly or through a per-tick mirror on the
    /// work record). Semantic command intake owns the choice to change it.
    [[nodiscard]] bool setProductionPaused(UnitId unit, bool paused) noexcept;
    [[nodiscard]] bool productionPaused(UnitId unit) const noexcept;

    /// Per-slot mirror of `Army::cheatEnabled` (C-360), refreshed once a tick by
    /// `syncCheatBuffs`. Retail's `CheatBuildRate` is a buff ON THE UNIT
    /// (`aiutilities.lua:1776`, `Buff.ApplyBuff(unit, 'CheatBuildRate')`), and the
    /// build-rate readers — `effectiveBuildPerTick` — see the store but not the
    /// army list, so the buff's per-unit shape is kept here rather than threading
    /// armies through a dozen signatures. Derived state: not saved, not hashed —
    /// the first tick after a restore re-stamps it from the armies.
    [[nodiscard]] bool cheatBuffedAt(UnitIndex slot) const noexcept {
        return slot < cheatBuffed_.size() && cheatBuffed_[slot];
    }

    /// Re-stamps `cheatBuffed_` from the armies: every live unit of a cheating
    /// army carries the buff, which is also `Unit.lua:209`'s OnCreate rule — a
    /// unit spawned after the flag was set is buffed because its army is.
    void syncCheatBuffs(std::span<const Army> armies) noexcept;

    /// Per-live-unit construction priority. The allocator serves High before Normal before
    /// Low out of whatever the tier above left; the flag is authoritative like
    /// `productionPaused`, so a stalled match under the same log allocates identically.
    /// `buildPriorities` exposes the slot-indexed span the economy pass consumes.
    [[nodiscard]] bool setBuildPriority(UnitId unit, BuildPriority tier) noexcept;
    [[nodiscard]] BuildPriority buildPriority(UnitId unit) const noexcept;
    [[nodiscard]] std::span<const BuildPriority> buildPriorities() const noexcept {
        return buildPriority_;
    }
    [[nodiscard]] bool setRetreatThreshold(UnitId unit, RetreatThreshold threshold) noexcept;
    [[nodiscard]] RetreatThreshold retreatThreshold(UnitId unit) const noexcept;
    [[nodiscard]] std::span<const RetreatThreshold> retreatThresholds() const noexcept {
        return retreatThresholds_;
    }
    /// The per-unit target filter (#15809): narrows or re-orders what
    /// `nearestTarget` acquires, inside the weapon's authored priorities.
    /// `targetFocuses` exposes the slot-indexed span the combat pass consumes.
    [[nodiscard]] bool setTargetFocus(UnitId unit, TargetFocus focus) noexcept;
    [[nodiscard]] TargetFocus targetFocus(UnitId unit) const noexcept;
    [[nodiscard]] std::span<const TargetFocus> targetFocuses() const noexcept {
        return targetFocus_;
    }
    /// The live bookkeeping — mutable because the retreat automation pass writes it.
    [[nodiscard]] std::span<RetreatState> retreats() noexcept { return retreats_; }
    [[nodiscard]] std::span<const RetreatState> retreats() const noexcept { return retreats_; }

    /// Controls automatic acquisition only; explicit target orders remain authoritative.
    [[nodiscard]] bool setDoNotTarget(UnitId unit, bool enabled) noexcept;
    [[nodiscard]] bool doNotTarget(UnitId unit) const noexcept;

    /// Per-live-unit script-bit disabled mask, retail's `RULEUTC_*` toggles
    /// (`C-350`, `Unit.lua` `OnScriptBitSet`/`OnScriptBitClear`). A SET bit means
    /// the feature is OFF — the uniform reading the sim needs, since retail's own
    /// bit polarity is inconsistent (shield set = on, intel set = off). Bit
    /// numbers are retail's: 0 shield, 2 jammer, 3 intel, 5 stealth, 8 cloak.
    /// Bits 1 (weapon — a retail no-op), 4 (production — `productionPaused`),
    /// 6 (generic pause) and 7 (special) are not stored.
    [[nodiscard]] bool setScriptBitDisabled(UnitId unit, std::uint8_t bit,
                                            bool disabled) noexcept;
    [[nodiscard]] bool scriptBitDisabled(UnitId unit, std::uint8_t bit) const noexcept;
    /// The whole mask, for the state hash — per-bit reads cannot express it.
    [[nodiscard]] std::uint16_t scriptBitsDisabledMaskAt(UnitIndex slot) const noexcept {
        return slot < scriptBitsDisabled_.size() ? scriptBitsDisabled_[slot] : 0;
    }

    /// Per-live-unit intel enable state, retail's (enabled, active) byte pairs
    /// (`C-283`, `0x00694B60`/`0x00694C56`). `setIntelEnabled` is the
    /// `EnableIntel`/`DisableIntel` dispatch: it flips the ENABLED byte of one
    /// `IntelType`. The effective disabled mask a slot reports is the union of
    /// these explicit writes and the `RULEUTC_*` toggles' `scriptBitIntelMask`
    /// — the same answer retail's refcounted `IntelDisables` table converges
    /// to, since overlapping groups (bit 3's all-intel vs bit 5's stealth) can
    /// only ever leave a type disabled.
    [[nodiscard]] bool setIntelEnabled(UnitId unit, IntelType type,
                                       bool enabled) noexcept;
    /// `IsIntelEnabled`: the enabled byte — false while either an explicit
    /// disable or a `RULEUTC_*` toggle covers the type.
    [[nodiscard]] bool intelEnabled(UnitId unit, IntelType type) const noexcept;
    /// The slot-indexed per-type read the intel pass uses.
    [[nodiscard]] bool intelDisabledAt(UnitIndex slot, IntelType type) const noexcept;
    /// The whole EFFECTIVE mask (explicit ∪ script bits), for the intel pass's
    /// placement key — a toggle that changes what a unit emits must re-stamp it.
    [[nodiscard]] std::uint16_t intelDisabledMaskAt(UnitIndex slot) const noexcept;
    /// The explicit mask alone — what a save serializes, since the script-bit
    /// half is already carried by `scriptBitsDisabled`.
    [[nodiscard]] std::uint16_t intelDisabledExplicitMaskAt(
        UnitIndex slot) const noexcept {
        return slot < intelDisabled_.size() ? intelDisabled_[slot] : 0;
    }
    /// The slot-indexed read the hot loops use — `Intel::update` and the shield
    /// passes walk slots, not handles.
    [[nodiscard]] bool scriptBitDisabledAt(UnitIndex slot, std::uint8_t bit) const noexcept;

    /// Per-live-unit maintenance-consumption flag, retail's
    /// `SetMaintenanceConsumption{Active,Inactive}` — last writer wins, so a unit
    /// with two toggles draws upkeep by whichever it touched LAST, not by whether
    /// anything remains on. Defaults to active; the toggle command owns changes.
    [[nodiscard]] bool setMaintenanceActive(UnitId unit, bool active) noexcept;
    [[nodiscard]] bool maintenanceActive(UnitId unit) const noexcept;

    /// `C-265`'s per-slot self-destruct timer, retail's blueprint `Lifetime`
    /// (the Othuy's 30 s). Slot-indexed like the other pass-facing arrays.
    /// `kLifetimeUnset` means "not yet armed": the tick's lifetime pass arms it
    /// from the type's `UnitDef::lifetimeSeconds` on first sight, so every spawn
    /// path — scene placement, finished construction, death-spawn — gets the
    /// countdown without the caller knowing it exists. Zero means "no lifetime".
    [[nodiscard]] std::span<TickCount> lifetimeRemainingTicks() noexcept {
        return lifetimeRemainingTicks_;
    }
    [[nodiscard]] std::span<const TickCount> lifetimeRemainingTicks() const noexcept {
        return lifetimeRemainingTicks_;
    }

    /// `C-293`'s per-unit manipulator list — retail's `IAniManipulator` chain,
    /// ticked from `Unit::MotionTick` via `CAniActor::UpdateManipulators`
    /// (`0x641550`). The list is kept sorted by `Manipulator::precedence`
    /// (`C-294`, `SetPrecedence` at `0x642610` re-sorts `0x6417b0`), so the
    /// manipulator tick walks it in execution order. Slot-indexed like the
    /// other pass-facing arrays; the mutable span is what `tickManipulators`
    /// writes through.
    [[nodiscard]] std::span<std::vector<Manipulator>> manipulators() noexcept {
        return manipulators_;
    }
    [[nodiscard]] std::span<const std::vector<Manipulator>> manipulators() const noexcept {
        return manipulators_;
    }
    /// Registers a manipulator on a live unit, inserting at its precedence —
    /// retail's `SetPrecedence` re-sort. Stable within one precedence, like
    /// retail's list append.
    [[nodiscard]] bool addManipulator(UnitId unit, Manipulator manipulator) noexcept;

    /// `C-303`'s bone-visibility mask — retail's `CAniPoseBone+0x48` flag map,
    /// written by `Unit::HideBone`/`ShowBone` (`0x6d820b`). One bit per bone,
    /// a SET bit meaning HIDDEN; the word vector grows to the highest bone
    /// touched rather than fixing a skeleton size the sim does not own.
    /// Serialized sim state, not a render flag: it saves, loads and hashes.
    [[nodiscard]] bool setBoneHidden(UnitId unit, std::int32_t bone,
                                     bool hidden) noexcept;
    /// The slot-indexed read consumers use — emitter attachment points ask
    /// "is the bone I ride hidden" without resolving a handle.
    [[nodiscard]] bool boneHiddenAt(UnitIndex slot, std::int32_t bone) const noexcept;
    /// The whole mask, for the state hash and serialization.
    [[nodiscard]] std::span<const std::uint64_t> boneHiddenMaskAt(
        UnitIndex slot) const noexcept {
        return slot < boneHidden_.size() ? std::span<const std::uint64_t>{boneHidden_[slot]}
                                         : std::span<const std::uint64_t>{};
    }

    /// Shared repeat/count operations. Exhaustion removes this exact object from every member
    /// queue, matching retail's cross-queue `DecreaseCommandCount` path.
    [[nodiscard]] bool increaseCommandCount(CommandId id, std::uint32_t amount = 1);
    [[nodiscard]] bool decreaseCommandCount(CommandId id, std::uint32_t amount = 1);

    [[nodiscard]] std::uint32_t nextCommandCounter(CommandSource source) const noexcept {
        return source < nextCommandCounters_.size() ? nextCommandCounters_[source] : 0;
    }

    // --- The spatial index (PLAN2.md §6.5, §7 P5.2) ---------------------------
    //
    // HERE RATHER THAN THREADED THROUGH SIX SIGNATURES. `nearestTarget`, `nearestStruck`,
    // `damageArea`, `aimAtTargets` and `resolveCollisions` all ask "which units are near this
    // place", and all five already take the store. An index derived from the store's own
    // positions belongs with them — the alternative was a `SpatialGrid&` parameter on every
    // pass and on every one of their forty-odd test call sites, for no gain in clarity.
    //
    // IT IS NOT SELF-MAINTAINING, and that is the price. A pass that moves units invalidates
    // it, so `reindex` is called explicitly at the points in the tick where positions have
    // settled — `tickSkirmish` owns that, the same way it owns the pass order. A query against
    // a stale index answers about where units WERE, which is a wrong answer rather than a
    // crash, so the rebuild points are documented where they happen.

    /// Rebuilds the spatial index from the current positions.
    ///
    /// Idempotent and cheap: one pass over the slots and a sort. Called twice per tick — once
    /// before collisions and once after, because collisions move things.
    void reindex(Fx cellSize);

    /// The index. Const because a query is a read — the answer buffer inside is a cache.
    [[nodiscard]] const SpatialGrid& space() const noexcept { return space_; }

    /// Slots that exist, live or dead. The length of every array above.
    [[nodiscard]] std::size_t slotCount() const noexcept { return transforms_.size(); }

    /// Units currently alive.
    [[nodiscard]] std::size_t liveCount() const noexcept { return ids_.liveCount(); }

    /// The type of the unit in a slot, or 0 for a slot that has never held one. Bounds
    /// checked because a pass indexing past the end is a bug worth failing loudly on rather
    /// than reading whatever is next in memory.
    [[nodiscard]] UnitTypeIndex typeAt(UnitIndex slot) const noexcept {
        return slot < types_.size() ? types_[slot] : UnitTypeIndex{0};
    }

private:
    IdPool ids_;

    /// The generation currently in each slot, mirrored from the pool so that a slot can be
    /// turned back into a handle without asking the pool for its internals.
    std::vector<Generation> generations_;

    /// The sim's authority on where units are. `UnitInstance` — the GPU's layout — is built
    /// from these at draw time and never read back; see `core/sim/Transform.hpp`.
    std::vector<Transform> transforms_;
    std::vector<MoveState> motion_;
    std::vector<Health> health_;
    std::vector<UnitTypeIndex> types_;
    std::vector<std::map<std::string, std::string>> enhancements_;
    std::vector<bool> factoryRepeat_;
    std::vector<bool> productionPaused_;
    std::vector<BuildPriority> buildPriority_;
    std::vector<RetreatThreshold> retreatThresholds_;
    std::vector<TargetFocus> targetFocus_;
    std::vector<RetreatState> retreats_;
    std::vector<bool> doNotTarget_;
    /// Retail's nine script bits fit a u16; a set bit means that feature is OFF.
    std::vector<std::uint16_t> scriptBitsDisabled_;
    /// `C-283`'s per-slot intel-disable masks — a set bit means that
    /// `IntelType` is OFF via an explicit `EnableIntel`/`DisableIntel` write.
    /// The `RULEUTC_*` toggles' contribution is derived on read through
    /// `scriptBitIntelMask`, so this array only ever holds the explicit half.
    std::vector<std::uint16_t> intelDisabled_;
    /// `SetMaintenanceConsumption*` — last writer wins; defaults to active.
    std::vector<bool> maintenanceActive_;
    /// `C-360`'s per-slot cheat-buff mirror — see `cheatBuffedAt`.
    std::vector<bool> cheatBuffed_;

    std::vector<CommandQueue> orders_;
    std::vector<std::optional<UnitId>> parents_;
    std::vector<std::vector<UnitId>> children_;
    std::vector<std::array<Fx, 2>> attachmentOffsets_;
    std::vector<Fx> attachmentHeights_;
    std::vector<std::int32_t> attachmentParentBones_;
    std::vector<std::int32_t> attachmentSelfBones_;
    std::vector<std::array<Fx, 2>> attachmentParentRest_;
    std::vector<Fx> attachmentParentRestHeights_;
    std::vector<std::array<Fx, 2>> attachmentSelfRest_;
    std::vector<Fx> attachmentSelfRestHeights_;

    /// `C-265`: ticks until the unit `Destroy()`s itself — the Othuy's
    /// `Lifetime`. `kLifetimeUnset` until the tick's lifetime pass arms it from
    /// the blueprint; zero for a unit with no lifetime.
    std::vector<TickCount> lifetimeRemainingTicks_;

    /// `C-293`'s manipulator lists, one per slot — see `manipulators()`.
    std::vector<std::vector<Manipulator>> manipulators_;
    /// `C-303`'s bone-visibility masks, one word-vector per slot — see
    /// `setBoneHidden`. A set bit means the bone is hidden.
    std::vector<std::vector<std::uint64_t>> boneHidden_;

    CommandSerial nextCommandSerial_ = 0;
    std::array<std::uint32_t, kInvalidCommandSource> nextCommandCounters_{};
    std::map<CommandId, std::weak_ptr<SharedCommand>> liveCommands_;

    /// Not parallel to the arrays above: a sorted index INTO them, rebuilt by `reindex`.
    SpatialGrid space_;
};

/// Rotates a model-space X/Z offset into world axes by a heading — the same
/// composition `attach`/`propagateAttachments` apply to bone rest offsets.
/// Free-standing so the transport attach path can pre-place cargo exactly
/// where the bone will hold it.
[[nodiscard]] std::array<Fx, 2> rotateByHeading(Brad heading,
                                                std::array<Fx, 2> local) noexcept;

} // namespace rm::sim
