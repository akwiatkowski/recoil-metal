#include "core/sim/StateHash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace rm::sim {
namespace {

// FNV-1a, 64-bit. Chosen for being short enough to read in one sitting and having no
// tuning constants to get subtly wrong — the properties that matter here are that it is
// deterministic and that it is OURS, not that it is fast or cryptographic. A faster hash
// would save time we are not short of; a library hash would be a dependency whose version
// silently changes what "the same match" means.
inline constexpr StateHash kOffsetBasis = 14695981039346656037ULL;
inline constexpr StateHash kPrime = 1099511628211ULL;

void feed(StateHash& h, std::uint64_t value) noexcept {
    // Byte at a time, low to high, so the result does not depend on the host's endianness.
    // The cross-architecture claim is the whole point; hashing a `std::uint64_t`'s bytes in
    // memory order would make arm64 and x86-64 disagree on identical state.
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFULL;
        h *= kPrime;
    }
}

// `feed(StateHash&, float)` USED TO BE HERE, and its absence is the result rather than a
// tidy-up (PLAN2.md §7 P10.0).
//
// It hashed a float's bit pattern, normalising negative zero, because `Construction::position`
// was a `std::array<float, 3>` — the last float in sim state, and one the float ban could not
// see because it arrived through a `fxToFloat` call rather than a declaration. With that field
// fixed point, nothing hashed is a float any more, and `-Wunused-function` says so at every
// build: **re-introducing a float into hashed state now fails to compile until someone puts
// this function back, which is a deliberate act rather than an oversight.**
//
// That is a stronger guarantee than the grep in `check_no_sim_floats.sh` gives, and it is worth
// keeping. If you are here because you need to hash a float: do not. Convert at the boundary.

void feed(StateHash& h, int value) noexcept {
    feed(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

void feed(StateHash& h, std::size_t value) noexcept {
    feed(h, static_cast<std::uint64_t>(value));
}

void feed(StateHash& h, bool value) noexcept { feed(h, value ? 1ULL : 0ULL); }

void feedBytes(StateHash& h, std::span<const std::uint8_t> bytes) noexcept {
    feed(h, bytes.size());
    for (const std::uint8_t byte : bytes) {
        feed(h, static_cast<std::uint64_t>(byte));
    }
}

void feedText(StateHash& h, std::string_view text) noexcept {
    feed(h, text.size());
    for (const char character : text) {
        feed(h, static_cast<std::uint64_t>(static_cast<unsigned char>(character)));
    }
}

/// Fixed-point values are fed as their RAW INTEGERS, which is the whole point of having them:
/// there is no bit pattern to normalise and no tolerance to worry about, because two runs that
/// agree agree exactly.
void feed(StateHash& h, Fx value) noexcept {
    feed(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.raw())));
}

void feed(StateHash& h, Mag value) noexcept {
    feed(h, static_cast<std::uint64_t>(value.raw()));
}

void feed(StateHash& h, const std::array<Fx, 3>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
    feed(h, v[2]);
}

void feed(StateHash& h, const std::array<Fx, 2>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
}

/// An angle. Fed as its raw 16 bits — there is no wrapping to normalise, because the type
/// cannot hold an unwrapped angle.
void feed(StateHash& h, Brad value) noexcept { feed(h, static_cast<std::uint64_t>(value)); }

void feed(StateHash& h, const Resources& r) noexcept {
    feed(h, r.mass);
    feed(h, r.energy);
}

// A unit's position and orientation.
//
// The presentation exclusions this used to carry — `animationPhase` and `teamColour` — are
// gone, and not because the rule changed: the store no longer holds them. `UnitInstance` is
// built at draw time now (P2.2), so the fields that could have caused a false divergence are
// not reachable from here at all. That paragraph in the header is history rather than a
// caveat, which is the outcome it predicted.
void feedTransform(StateHash& h, const Transform& unit) noexcept {
    feed(h, unit.x);
    feed(h, unit.y);
    feed(h, unit.z);
    feed(h, unit.heading);
    feed(h, unit.pitch);
    feed(h, unit.roll);
}

void feedMotion(StateHash& h, const MoveState& motion) noexcept {
    feed(h, motion.armyIndex);
    feed(h, motion.destinationX);
    feed(h, motion.destinationZ);
    feed(h, motion.moving);
    if (motion.attached) {
        feed(h, motion.attached);
    }
    // Derived from the already-hashed unit type, but still execution state: feeding only the
    // true case catches a broken spawn invariant without changing every historical ground hash.
    if (motion.airborne) {
        feed(h, motion.airborne);
    }
    if (motion.surfaceWater) {
        feed(h, motion.surfaceWater);
    }
    if (motion.submersible) {
        feed(h, motion.submersible);
        feed(h, motion.submerged);
        feed(h, motion.diveTargetSubmerged);
        feed(h, motion.submarineOffset);
        feed(h, motion.submarineElevation);
        feed(h, motion.divePerTick);
    }
    if (motion.hovering) {
        feed(h, motion.hovering);
        feed(h, motion.hoverElevation);
    }
    feed(h, motion.speedPerTick);
    feed(h, motion.turnPerTick);
    feed(h, motion.radiusElmos);
    feed(h, motion.distanceTravelledElmos);
    // The route still to walk. Fed with its length, so a unit that has consumed a waypoint
    // differs from one that has not even when the remaining waypoints coincide.
    feed(h, motion.path.size());
    for (const std::array<Fx, 2>& waypoint : motion.path) {
        feed(h, waypoint);
    }
    feed(h, motion.pathIndex);
    // Congestion detection is execution state, fed only once it exists — a unit
    // walking freely carries a primed anchor and a zero count that say nothing
    // the position does not, and leaving them unfed keeps a quiet match's
    // digest exactly what it was before the pass existed.
    if (motion.blockedTicks != 0 || motion.yielding) {
        feed(h, motion.blockedTicks);
        feed(h, motion.yielding);
        feed(h, motion.lastGoalDistance);
    }
    // The measured step a `LeadTarget` weapon reads — execution state that
    // changes aim, fed only once nonzero so a match of standing armies keeps
    // the digest it had before the field existed.
    if (motion.stepX != Fx{} || motion.stepZ != Fx{}) {
        feed(h, motion.stepX);
        feed(h, motion.stepZ);
    }
    // The live turret pose. Fed always: a traversing ring is gameplay state
    // (fire gates on it), and zeros for the unturreted keep their digests
    // moving exactly as before only until the first traverse — after which a
    // changed digest is the truth, not churn.
    feed(h, static_cast<std::uint64_t>(motion.turretYaw));
    feed(h, static_cast<std::uint64_t>(motion.turretPitch));
    // The dual manipulator's own angles — the second arm's pose is gameplay
    // state the same way: its muzzle is where half the shots leave.
    feed(h, static_cast<std::uint64_t>(motion.turretYaw2));
    feed(h, static_cast<std::uint64_t>(motion.turretPitch2));
    // The dual manipulator's barrel-on-trigger bit — gameplay state: which
    // muzzle the next shot leaves depends on it.
    feed(h, static_cast<std::uint64_t>(motion.turretMuzzlePhase));
    if (motion.pathPhaseCellsX != 0) {
        feed(h, motion.pathPhaseStartX);
        feed(h, motion.pathPhaseStartZ);
        feed(h, motion.pathPhaseCellsX);
    }
    // Winged-flight execution state, gated on capability: ground units carry zeros here
    // that must not move their digests. Gains and thresholds are spawn-derived from the
    // already-hashed type, so only the live values feed (`C-221`).
    if (motion.canFly) {
        feed(h, motion.canFly);
        feed(h, static_cast<std::uint64_t>(motion.airState));
        feed(h, static_cast<std::uint64_t>(motion.airCombatState));
        feed(h, motion.airCombatDeadline);
        feed(h, static_cast<std::uint64_t>(motion.airSustainedTicks));
        feed(h, motion.airYawVelocity);
        feed(h, motion.velocity);
        feed(h, motion.altitudeRef);
        feed(h, motion.fuelRatio);
        feed(h, static_cast<std::size_t>(motion.idleTicks));
    }
}

void feedHealth(StateHash& h, const Health& health) noexcept {
    feed(h, health.current);
    feed(h, health.maximum);
    // Preserve every historical unshielded hash while covering all new execution state.
    if (health.shield.maximum > Mag{}) {
        feed(h, true);
        feed(h, health.shield.current);
        feed(h, health.shield.maximum);
        feed(h, static_cast<std::size_t>(health.shield.regenDelayRemaining));
        feed(h, static_cast<std::size_t>(health.shield.rechargeRemaining));
        // A manual re-enable recharges without refilling — the flag decides what
        // the next completed recharge does, so it hashes when set. Default-true
        // stays silent, keeping the pre-toggle stream intact.
        if (!health.shield.rechargeRestoresFull) {
            feed(h, false);
        }
    }
    feed(h, health.reloadRemaining.size());
    for (const int remaining : health.reloadRemaining) {
        feed(h, remaining);
    }

    // Where each weapon is WITHIN its burst. Fed for the same reason the reload is: two units
    // with identical health and identical reloads but different burst positions will fire
    // different numbers of shots over the next second, so a hash that could not tell them apart
    // would report a match as identical while it diverged (§7 P3.5).
    feed(h, health.burstRemaining.size());
    for (const int remaining : health.burstRemaining) {
        feed(h, remaining);
    }

    if (!health.automaticTargets.empty()) {
        feed(h, health.automaticTargets.size());
        for (const UnitId target : health.automaticTargets) {
            feed(h, static_cast<std::size_t>(target.index));
            feed(h, static_cast<std::size_t>(target.generation));
        }
    }

    // WHO LAST HIT IT. State, not provenance — unlike a command's issuing player, this decides
    // something: it is the instigator a `UnitDestroyed` event names, and a consumer that awards
    // a kill, plays a sound or scores a match reads it. Two runs that disagree about who is
    // shooting whom have diverged, whatever the health totals say.
    feed(h, static_cast<std::size_t>(health.lastHitBy.index));
    feed(h, static_cast<std::size_t>(health.lastHitBy.generation));

    // VETERANCY, and only when the unit has any — which keeps every historical hash of a match
    // whose units never promoted byte-identical, the same trick the unshielded case above uses.
    // Fed because it decides things: two units with equal health but different levels have
    // different maxima and heal at different rates, so they diverge from the next tick on.
    if (health.veterancy.kills != 0 || health.veterancy.level != 0) {
        feed(h, true);
        feed(h, health.veterancy.kills);
        feed(h, health.veterancy.level);
    }
}

void feedSharedCommand(StateHash& h, const SharedCommand& command) noexcept {
    feed(h, static_cast<std::uint64_t>(command.id));
    feed(h, static_cast<std::uint8_t>(command.kind));
    feed(h, static_cast<std::size_t>(command.buildType));
    feed(h, static_cast<std::uint64_t>(command.creationSerial));
    feed(h, command.targetX);
    feed(h, command.targetZ);
    if (command.target.generation != 0) {
        feed(h, static_cast<std::size_t>(command.target.index));
        feed(h, static_cast<std::size_t>(command.target.generation));
    }
    feed(h, command.units.size());
    for (const UnitId unit : command.units) {
        feed(h, static_cast<std::size_t>(unit.index));
        feed(h, static_cast<std::size_t>(unit.generation));
    }
    feed(h, static_cast<std::uint64_t>(command.originalCount));
    feed(h, static_cast<std::uint64_t>(command.remainingCount));
    if (command.kind == CommandKind::Script) {
        feedText(h, command.scriptTask);
        feedBytes(h, command.scriptData);
    }
}

/// Shared command intent, exactly once per deterministic ID rather than once per queue owner.
void feedSharedCommands(StateHash& h, std::span<const CommandQueue> queues) {
    std::map<CommandId, const SharedCommand*> shared;
    for (const CommandQueue& queue : queues) {
        for (const QueuedCommand& entry : queue.entries()) {
            if (entry.payload().id != kInvalidCommandId) {
                shared.try_emplace(entry.payload().id, &entry.payload());
            }
        }
    }
    feed(h, shared.size());
    for (const auto& [id, command] : shared) {
        (void)id;
        feedSharedCommand(h, *command);
    }
}

/// A unit's outstanding orders (§7 P4.1).
///
/// FED, because a queue is sim state in the strongest sense: two units standing in the same
/// place with the same health will end up in different places if one of them has three
/// waypoints left and the other none. A hash blind to the queue would call a match identical
/// right up to the tick the routes diverged, which is the failure mode the whole harness exists
/// to prevent — it would report the divergence late, at the movement, rather than at its cause.
///
/// Field by field rather than as a struct, on the same rule as everything else here: `Command`
/// is trivially copyable but its padding is unspecified, and hashing padding would make the
/// result depend on the compiler.
///
/// AND NOT EVERY FIELD. `tick` and `player` are PROVENANCE — when the order was given and who
/// gave it — and neither can change what the unit does with it. Feeding them would make the
/// hash disagree about two matches that will play out identically, which is the same category
/// of mistake as hashing a screen position. It would also make §7 P4.2's test impossible to
/// state: "the scripted opponent's order and a synthetic click produce identical hashes" is
/// only true if the hash is blind to which of them issued it. The `CommandLog` keeps both, and
/// that is where provenance belongs.
void feedOrders(StateHash& h, const CommandQueue& orders) noexcept {
    feed(h, orders.size());
    const QueuedCommand* active = orders.activeEntry();
    feed(h, static_cast<std::uint8_t>(active != nullptr));
    if (active != nullptr) {
        // A newly exposed cyclic head starts next beat. Hashing this identity reports a
        // divergence at the queue transition rather than one tick later when movement differs.
        feed(h, static_cast<std::uint64_t>(active->payload().id));
        if (active->payload().id == kInvalidCommandId) {
            feed(h, static_cast<std::uint64_t>(active->payload().creationSerial));
        }
    }
    for (const QueuedCommand& entry : orders.entries()) {
        const SharedCommand& command = entry.payload();
        feed(h, static_cast<std::uint64_t>(command.id));
        // Direct queue tests may build an entry outside authoritative intake. Keep those
        // distinguishable without pretending their invalid ID is shared identity.
        if (command.id == kInvalidCommandId) {
            feedSharedCommand(h, command);
        }
        feed(h, static_cast<std::size_t>(entry.unit().index));
        feed(h, static_cast<std::size_t>(entry.unit().generation));
        feed(h, entry.targetX());
        feed(h, entry.targetZ());
        // Invalid means "no target" and predates targeted commands. Feeding nothing for it
        // preserves old hashes; a real generational handle joins the hash at acquisition.
        if (entry.target().generation != 0) {
            feed(h, static_cast<std::size_t>(entry.target().index));
            feed(h, static_cast<std::size_t>(entry.target().generation));
        }
        if (entry.patrolOrigin()) {
            // The hidden origin changes the route even while the visible destination is equal.
            // Feed it only when present so non-patrol historical hashes retain their byte shape.
            feed(h, true);
            feed(h, (*entry.patrolOrigin())[0]);
            feed(h, (*entry.patrolOrigin())[1]);
            feed(h, entry.returningToPatrolOrigin());
        }
        if (command.kind == CommandKind::Script) {
            const ScriptTaskState& state = entry.scriptState();
            feed(h, state.created);
            feed(h, state.suspended);
            feed(h, static_cast<std::uint64_t>(state.sleepBeats));
            feed(h, state.aiResult);
            feedBytes(h, state.opaque);
        }
    }
}

void feedPathRequest(StateHash& h, const PathRequest& request) noexcept {
    feed(h, static_cast<std::size_t>(request.unit.index));
    feed(h, static_cast<std::size_t>(request.unit.generation));
    feed(h, static_cast<std::uint64_t>(request.command));
    feed(h, request.army);
    feed(h, request.fromX);
    feed(h, request.fromZ);
    feed(h, request.targetX);
    feed(h, request.targetZ);
}

void feedFlowField(StateHash& h, const FlowField& field) noexcept {
    feed(h, field.goalX());
    feed(h, field.goalZ());
    feed(h, field.costs().size());
    for (const Fx cost : field.costs()) {
        feed(h, cost);
    }
    feed(h, field.next().size());
    for (const int next : field.next()) {
        feed(h, next);
    }
    feed(h, field.closed().size());
    for (const std::uint8_t closed : field.closed()) {
        feed(h, static_cast<std::uint64_t>(closed));
    }
    feed(h, field.explored().size());
    for (const std::uint8_t touched : field.explored()) {
        feed(h, static_cast<std::uint64_t>(touched));
    }
    feed(h, field.open().size());
    for (const PathSearchNode& node : field.open()) {
        feed(h, node.f);
        feed(h, node.cell);
    }
}

void feedActiveField(StateHash& h, const PathService::ActiveField& active) noexcept {
    feed(h, active.gridFingerprint);
    feed(h, active.goalX);
    feed(h, active.goalZ);
    feed(h, active.startCell);
    feedFlowField(h, *active.field);
}

void feedPathService(StateHash& h, const PathService& service) noexcept {
    const auto& admissions = service.admissions();
    const auto& pending = service.pending();
    const auto& activeRequests = service.activeRequests();
    const auto& activeFields = service.activeFields();
    const auto& retryWaits = service.retryWaits();
    const auto& failureCounts = service.failureCounts();
    feed(h, service.serviceBeats());
    feed(h, admissions.size());
    for (const auto& armyAdmissions : admissions) {
        feed(h, armyAdmissions.size());
        for (const PathRequest& request : armyAdmissions) {
            feedPathRequest(h, request);
        }
    }
    feed(h, pending.size());
    for (std::size_t army = 0; army < pending.size(); ++army) {
        feed(h, pending[army].size());
        for (const PathRequest& request : pending[army]) {
            feedPathRequest(h, request);
        }
        feed(h, activeRequests[army].has_value());
        if (activeRequests[army]) {
            feedPathRequest(h, *activeRequests[army]);
        }
        feed(h, activeFields[army].has_value());
        if (activeFields[army]) {
            feedActiveField(h, *activeFields[army]);
        }
        feed(h, retryWaits[army]);
        feed(h, failureCounts[army]);
    }
}

} // namespace

StateHash hashMatch(const UnitStore& store, const Match& match) {
    StateHash h = kOffsetBasis;
    // MT19937's next state_size outputs determine its future sequence. Hash a copy
    // to include both state and cursor without locale-dependent text serialization.
    auto random = match.random.snapshot();
    for (std::size_t i = 0; i < RandomStream::Snapshot::state_size; ++i) {
        feed(h, static_cast<std::uint64_t>(random()));
    }

    // Slot count first, so a store that grew differs even if the new slot is empty — a spawn
    // that produced nothing is still a different match.
    feed(h, store.slotCount());
    // The next accepted command consumes this value. It is match state even while no live
    // command currently exposes it, and clearing or recycling a queue must not rewind it.
    feed(h, static_cast<std::uint64_t>(store.nextCommandSerial()));
    for (std::uint16_t source = 0; source < kInvalidCommandSource; ++source) {
        feed(h, static_cast<std::uint64_t>(
                    store.nextCommandCounter(static_cast<CommandSource>(source))));
    }

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();
    const std::span<const CommandQueue> orders = store.orders();
    feedSharedCommands(h, orders);

    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        feedTransform(h, transforms[slot]);
        feedMotion(h, motion[slot]);
        feedHealth(h, healths[slot]);
        feedOrders(h, orders[slot]);
        if (!store.enhancements()[slot].empty()) {
            feedText(h, "installed-enhancements");
            feed(h, store.enhancements()[slot].size());
            for (const auto& [position, name] : store.enhancements()[slot]) {
                feedText(h, position);
                feedText(h, name);
            }
        }
        // The type, and who is in the slot. The type stands in for the definition — a def is
        // content loaded from disk and identical across two runs of the same log by
        // construction, so hashing its contents would fingerprint the install rather than the
        // match. The generation says whether this is the same occupant.
        feed(h, static_cast<std::size_t>(store.typeAt(slot)));
        feed(h, static_cast<std::size_t>(store.idAt(slot).generation));
        // Factory repeat changes what a completed mobile production order does. Keep the
        // all-disabled stream intact, while a live enabled factory marks authoritative state.
        if (store.factoryRepeat(store.idAt(slot))) {
            feed(h, true);
        }
        // A production hold is authoritative like repeat: it changes what the economy
        // charges and what the command stage advances, so a paused unit must hash
        // differently from a running one.
        if (store.productionPaused(store.idAt(slot))) {
            feed(h, std::uint8_t{3});
        }
        // The build-priority tier decides what a stalled allocator pays first, so two
        // runs that disagree on it pay different work. Normal stays silent — the
        // all-Normal stream is the pre-tier stream.
        if (const BuildPriority tier = store.buildPriority(store.idAt(slot));
            tier != BuildPriority::Normal) {
            feed(h, std::uint8_t{4});
            feed(h, static_cast<std::uint8_t>(tier));
        }
        // The retreat threshold decides whether a hurt unit abandons the fight, and
        // an in-flight retreat is owed a return — both change what the next tick does,
        // so both hash. Off and not-retreating stay silent, keeping the pre-retreat
        // stream intact for matches that never arm it.
        if (const RetreatThreshold threshold = store.retreatThreshold(store.idAt(slot));
            threshold != RetreatThreshold::Off) {
            feed(h, std::uint8_t{5});
            feed(h, static_cast<std::uint8_t>(threshold));
        }
        if (const RetreatState& retreat = store.retreats()[slot]; retreat.active) {
            feed(h, std::uint8_t{6});
            feed(h, retreat.returnX.raw());
            feed(h, retreat.returnZ.raw());
            feed(h, retreat.toX.raw());
            feed(h, retreat.toZ.raw());
        }
        // The target focus decides what the guns acquire next tick — it changes
        // the outcome the same way a threshold does. Default stays silent so a
        // match that never cycles it hashes the pre-focus stream.
        if (const TargetFocus focus = store.targetFocus(store.idAt(slot));
            focus != TargetFocus::Default) {
            feed(h, std::uint8_t{7});
            feed(h, static_cast<std::uint8_t>(focus));
        }
        if (store.doNotTarget(store.idAt(slot))) {
            feed(h, std::uint8_t{2});
        }
        // The script-bit toggles change what the unit does next tick — a disabled
        // shield stops absorbing, disabled intel stops emitting — so a set mask
        // hashes. The maintenance flag is last-writer-wins state, not derivable
        // from the mask, and it gates upkeep: it hashes too. All-off/active
        // stays silent, keeping the pre-toggle stream intact.
        if (const std::uint16_t bits = store.scriptBitsDisabledMaskAt(slot);
            bits != 0) {
            feed(h, std::uint8_t{8});
            feed(h, bits);
        }
        if (!store.maintenanceActive(store.idAt(slot))) {
            feed(h, std::uint8_t{9});
        }
        // `C-283`'s explicit intel-disable mask (`EnableIntel`/`DisableIntel`)
        // changes what the unit emits next pass — a disabled radar stops
        // covering, a disabled jammer stops lying — so a set mask hashes.
        // The RULEUTC_* half is already covered by tag 8 above; only the
        // explicit mask is fed here, and all-enabled stays silent.
        if (const std::uint16_t intelMask = store.intelDisabledExplicitMaskAt(slot);
            intelMask != 0) {
            feed(h, std::uint8_t{12});
            feed(h, intelMask);
        }
        // `C-293`'s manipulator list and `C-303`'s bone mask are serialized sim
        // state — a storage slide's offset decides what the pose does next
        // tick, and a hidden bone suppresses what attaches to it. Empty list
        // and all-shown mask stay silent, keeping the pre-manipulator stream.
        if (!store.manipulators()[slot].empty()) {
            feed(h, std::uint8_t{10});
            feed(h, store.manipulators()[slot].size());
            for (const Manipulator& manip : store.manipulators()[slot]) {
                feed(h, static_cast<std::uint8_t>(manip.kind));
                feed(h, manip.precedence);
                feed(h, manip.enabled);
                feed(h, manip.slideResource);
                feed(h, manip.slideBone);
                feed(h, manip.slideRange);
                feed(h, manip.slideOffset);
                feed(h, manip.inTerrainContact);
                feed(h, manip.inUnitContact);
            }
        }
        if (const std::span<const std::uint64_t> mask = store.boneHiddenMaskAt(slot);
            std::ranges::any_of(mask, [](std::uint64_t word) { return word != 0; })) {
            feed(h, std::uint8_t{11});
            feed(h, mask.size());
            for (const std::uint64_t word : mask) {
                feed(h, word);
            }
        }
        // WHETHER THE SLOT IS OCCUPIED, which the generation cannot say on its own. Death is
        // a tombstone: `kill` leaves every array untouched and deliberately does NOT advance
        // the generation mirror, because a stale mirror is what makes a dead unit's handle
        // fail `alive`. So without this, a death changed nothing here — and a match that had
        // lost a unit hashed identically to one that had not. In practice the values usually
        // moved too (`retireDead` zeroes the radius, and health reached zero to get there),
        // which is why this went unnoticed until a test killed a unit at full health.
        feed(h, store.slotAlive(slot));
    }

    bool hasAttachments = false;
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        hasAttachments = hasAttachments || store.parentOf(store.idAt(slot)).has_value();
    }
    // Preserve the C-176 stream for attachment-free matches. Once an attachment exists, hash
    // both reciprocal views: child order is observable state and must be replay-stable too.
    if (hasAttachments) {
        feed(h, true);
        for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
            const UnitId unit = store.idAt(slot);
            const std::optional<UnitId> parent = store.parentOf(unit);
            feed(h, parent.has_value());
            if (parent) {
                feed(h, static_cast<std::size_t>(parent->index));
                feed(h, static_cast<std::size_t>(parent->generation));
                feed(h, store.attachmentOffsetOf(unit));
                feed(h, store.attachmentHeightOf(unit));
                const UnitStore::AttachBones bones = store.attachmentBonesOf(unit);
                feed(h, bones.parent);
                feed(h, bones.self);
                feed(h, bones.parentRest);
                feed(h, bones.parentRestHeight);
                feed(h, bones.selfRest);
                feed(h, bones.selfRestHeight);
            }
            const std::vector<UnitId>& children = store.childrenOf(unit);
            feed(h, children.size());
            for (const UnitId child : children) {
                feed(h, static_cast<std::size_t>(child.index));
                feed(h, static_cast<std::size_t>(child.generation));
            }
        }
    }

    // The armies. There is no `colour` to skip any more (§7 P6.3): a palette entry
    // is not a fact about the match.
    feed(h, match.armies.size());
    for (const Army& army : match.armies) {
        feed(h, army.index);
        feed(h, static_cast<int>(army.faction));
        feed(h, army.alliance);
        feed(h, army.defeated);
    }
    // `OfferingDraw` joins the stream only once some army has offered, so a
    // match that never touches the mechanic hashes exactly as before.
    if (std::ranges::any_of(match.armies,
                          [](const Army& army) { return army.offeringDraw; })) {
        for (const Army& army : match.armies) {
            feed(h, army.offeringDraw);
        }
    }
    // `UnitCap` joins the stream only once some army's ceiling differs from the
    // engine's own 500 (`0x008e8035`), so a match on the default hashes exactly
    // as before. `unitCostTotal` is derived — the units it sums are already fed.
    if (std::ranges::any_of(match.armies, [](const Army& army) {
            return army.unitCap != Fx::fromInt(500);
        })) {
        for (const Army& army : match.armies) {
            feed(h, army.unitCap.raw());
        }
    }
    // `cheatEnabled` (C-360) joins the stream only once some army cheats, so a
    // match with no AIx hashes exactly as before.
    if (std::ranges::any_of(match.armies,
                          [](const Army& army) { return army.cheatEnabled; })) {
        for (const Army& army : match.armies) {
            feed(h, army.cheatEnabled);
        }
    }
    // `RequestingAlliedVictory` (C-346) joins the stream only once some army has
    // asked for the shared win, so a match that never touches diplomacy hashes
    // exactly as before. `resourceSharing` stays out: it gates nothing in the
    // sim — retail's flag is read by the UI's sharing controls, not by
    // `GiveResourcesToPlayer`.
    if (std::ranges::any_of(match.armies,
                          [](const Army& army) { return army.requestingAlliedVictory; })) {
        for (const Army& army : match.armies) {
            feed(h, army.requestingAlliedVictory);
        }
    }


    if (match.pathService != nullptr) {
        // Preserve the pre-service byte stream for compatibility seams that do not own one.
        // A present service is authoritative execution state and therefore marks and extends it.
        feed(h, true);
        feedPathService(h, *match.pathService);
    }

    feed(h, match.economies.size());
    for (const Economy& economy : match.economies) {
        feed(h, economy.stored);
        feed(h, economy.storage);
        feed(h, economy.incomePerTick);
        feed(h, economy.upkeepPerTick);
        feed(h, economy.fundedFraction);
        // The two bucket ratios and which resource bound them (`C-159`), plus upkeep's share
        // of the carry-forward. `massIsBinding` decides which ratio each consumer is granted
        // at, so it changes behaviour and belongs here.
        feed(h, economy.multiResourceFunded);
        feed(h, economy.singleResourceFunded);
        feed(h, economy.massIsBinding);
        feed(h, economy.upkeepAllocated);
        // The allied gift in flight. It carries between ticks and is spent next tick, so it
        // is live state by the same argument as the carry-forward above — and the sharing
        // flag decides whether it is ever produced.
        feed(h, economy.sharedIn);
        feed(h, economy.sharesOverflow);
    }

    // WHAT EACH SIDE CAN SEE (ADR-037). Hashed rather than treated as a derived cache, and
    // the reason is that it is not one: sight decides who may be shot at, so two runs whose
    // grids differ are two runs that will fight differently a tick later. Catching that here
    // names the tick it happened on instead of the tick it became visible in the outcome.
    //
    // The COUNTS, not just whether a square is covered. A square seen by two units and a
    // square seen by one are the same to a shooter and different to the sim — the second is
    // one death away from being dark — and a hash that could not tell them apart would miss
    // a divergence in who is standing where.
    //
    // Both styles feed the same field, so switching `VisionStyle` invalidates a recorded
    // hash log. That is correct: it is a different match.
    feed(h, match.intel != nullptr);
    if (match.intel != nullptr) {
        feed(h, static_cast<int>(match.intel->style()));
        feed(h, match.intel->alliances());
        for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
            for (std::size_t kind = 0; kind < kIntelKindCount; ++kind) {
                const IntelGrid& grid =
                    match.intel->grid(static_cast<int>(alliance), static_cast<IntelKind>(kind));
                feed(h, grid.counts().size());
                for (const std::uint16_t count : grid.counts()) {
                    feed(h, count);
                }
            }
            // The hidden family after the senses, same discipline: derived state, hashed so
            // a divergence in stealth-field stamping is caught by the same gate as one in
            // sight. Growing the format invalidates recorded logs, correctly — the golden
            // was re-recorded when these arrived.
            for (std::size_t kind = 0; kind < kHiddenKindCount; ++kind) {
                const IntelGrid& grid = match.intel->hiddenGrid(
                    static_cast<int>(alliance), static_cast<HiddenKind>(kind));
                feed(h, grid.counts().size());
                for (const std::uint16_t count : grid.counts()) {
                    feed(h, count);
                }
            }
        }

        // Retained radar knowledge is not a presentation cache: a dead source's last-known
        // blip changes what this alliance knows. Preserve older empty-intel hash streams, but
        // once any such knowledge exists feed every owning alliance and every field.
        bool hasRetainedRadarContacts = false;
        for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
            hasRetainedRadarContacts = hasRetainedRadarContacts
                                    || !match.intel->retainedRadarContacts(
                                            static_cast<int>(alliance)).empty();
        }
        if (hasRetainedRadarContacts) {
            feed(h, true);
            for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
                const auto contacts =
                    match.intel->retainedRadarContacts(static_cast<int>(alliance));
                feed(h, contacts.size());
                for (const RetainedRadarContact& contact : contacts) {
                    feed(h, static_cast<std::size_t>(contact.unit.index));
                    feed(h, static_cast<std::size_t>(contact.unit.generation));
                    feed(h, contact.x);
                    feed(h, contact.z);
                    feed(h, contact.maybeDead);
                    feed(h, static_cast<std::size_t>(contact.deadTicks));
                }
            }
        }

        // C-158's visual-identification latch changes automatic target ranking after a unit
        // leaves sight, so it is live simulation state rather than a UI contact cache.
        bool hasSeenEver = false;
        for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
            for (const UnitId unit : match.intel->seenEver(static_cast<int>(alliance))) {
                hasSeenEver = hasSeenEver || unit.generation != 0;
            }
        }
        if (hasSeenEver) {
            feed(h, true);
            for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
                const auto known = match.intel->seenEver(static_cast<int>(alliance));
                feed(h, known.size());
                for (const UnitId unit : known) {
                    feed(h, static_cast<std::size_t>(unit.index));
                    feed(h, static_cast<std::size_t>(unit.generation));
                }
            }
        }

        // C-284's brownout recovery count decides when a starved army's senses come back,
        // so it is live state like the latches above. Fed unconditionally — the vector is
        // empty until an `update` with economies runs, and an empty span still feeds its
        // size, which distinguishes "no economy" from "recovered".
        feed(h, match.intel->intelRecovery().size());
        for (const TickCount ticks : match.intel->intelRecovery()) {
            feed(h, static_cast<std::uint64_t>(ticks));
        }
    }

    // Shots in flight. Nullable because a decorative crowd has no projectile list, and
    // "no list" has to hash differently from "an empty list" — the first is a scene with no
    // combat, the second a match between two ticks of it.
    feed(h, match.projectiles != nullptr);
    if (match.projectiles != nullptr) {
        feed(h, match.projectiles->size());
        for (const Projectile& shot : *match.projectiles) {
            feed(h, shot.position);
            feed(h, shot.velocity);
            if (shot.guidanceTarget != UnitId{} || shot.turnPerTick != 0) {
                feedText(h, "guidance");
                feed(h, static_cast<std::size_t>(shot.guidanceTarget.index));
                feed(h, static_cast<std::size_t>(shot.guidanceTarget.generation));
                feed(h, shot.turnPerTick);
                feed(h, shot.accelerationPerTickSquared);
                feed(h, shot.maxSpeedPerTick);
            }
            // THE WHOLE TABLE, field by field. A profile is trivially copyable but its padding
            // is unspecified, so it is fed like every other struct here rather than as bytes.
            feed(h, shot.damage.base);
            feed(h, static_cast<std::size_t>(shot.damage.overrideCount));
            for (std::uint8_t i = 0; i < shot.damage.overrideCount; ++i) {
                feed(h, static_cast<std::size_t>(shot.damage.overrideArmor[i]));
                feed(h, shot.damage.overrideDamage[i]);
            }
            feed(h, shot.damageRadiusElmos);
            // Surface-only is the historical FA projectile state. Keep its byte stream stable,
            // while every other mask carries a marker and value that distinguish future rules.
            if (shot.targetLayers != unitdef::TargetLayerMask::Surface) {
                feed(h, true);
                feed(h, static_cast<int>(shot.targetLayers));
            }
            feed(h, shot.firedByArmy);
            if (shot.interceptor) {
                feed(h, true);
            }
            // `C-204`'s water pair rides the shot like the interceptor flag:
            // launch-time state the blueprint resolved once, fed only when set
            // so ordinary shots keep the byte stream they were blessed with.
            if (shot.stayUnderwater || shot.destroyOnWater) {
                feed(h, true);
                feed(h, shot.stayUnderwater);
                feed(h, shot.destroyOnWater);
            }
            // The damage pool, only when the blueprint authored one — otherwise the
            // golden duel's shots keep the byte stream they were blessed with (`C-087`).
            if (shot.maxHealth > Mag{}) {
                feed(h, true);
                feed(h, shot.maxHealth);
                feed(h, shot.health);
            }
            // Same conditional treatment for categories: only resolved missile shots
            // carry any, so ordinary fire keeps its existing digest (`C-088`).
            if (!shot.categories.empty()) {
                feed(h, true);
                feed(h, shot.categories.size());
                for (const std::string& tag : shot.categories) {
                    feed(h, tag.size());
                    for (const char c : tag) {
                        feed(h, static_cast<std::size_t>(c));
                    }
                }
            }
            // The shooter's identity is live execution state: at impact it becomes
            // `lastHitBy`, which decides kill attribution. Two runs differing only here
            // hash identically for the whole flight — up to thirty seconds — before the
            // divergence surfaces one hop downstream. Fed like every other UnitId.
            feed(h, static_cast<std::size_t>(shot.firedBy.index));
            feed(h, static_cast<std::size_t>(shot.firedBy.generation));
            feed(h, static_cast<int>(shot.arc));
            feed(h, shot.ticksRemaining);
            feed(h, static_cast<int>(shot.pendingImpact));
            feed(h, static_cast<std::size_t>(shot.impactTarget.index));
            feed(h, static_cast<std::size_t>(shot.impactTarget.generation));
            // `C-171`'s weave and `C-088`'s friendly-fire flag, conditional like
            // the water pair: a shot carrying either diverges from one that
            // does not, and an ordinary shot keeps its byte stream.
            if (shot.zigZagAmplitudeElmos > Fx{} || shot.friendlyFire) {
                feed(h, true);
                feed(h, shot.zigZagAmplitudeElmos);
                feed(h, shot.zigZagPeriodTicks);
                feed(h, shot.zigZagNextRoll);
                feed(h, shot.zigZagOffsetX);
                feed(h, shot.zigZagOffsetY);
                feed(h, shot.zigZagOffsetZ);
                feed(h, shot.zigZagApplied);
                feed(h, shot.aimPoint);
                feed(h, shot.friendlyFire);
            }
        }
    }

    feed(h, match.building != nullptr);
    if (match.building != nullptr) {
        feed(h, match.building->size());
        for (const Construction& work : *match.building) {
            feed(h, work.armyIndex);
            feed(h, work.position);
            feed(h, work.cost);
            feed(h, work.buildTimeRemaining);
            feed(h, work.totalBuildTime);
            feed(h, work.buildPerTick);
            feed(h, work.blueprintIndex);
            feed(h, static_cast<std::uint64_t>(work.upgradeOf.index));
            feed(h, static_cast<std::uint64_t>(work.upgradeOf.generation));
            // The founder and the help. `builder` is real state (set at order time);
            // `assistPerTick` is derived each tick from hashed orders and positions, so
            // feeding it costs nothing and catches a divergence in the assist scan itself.
            feed(h, static_cast<std::uint64_t>(work.builder.index));
            feed(h, static_cast<std::uint64_t>(work.builder.generation));
            feed(h, work.assistPerTick);
            // THE CARRY-FORWARD AND THE CACHED RATIO (`C-159`, `C-162`). Both are live sim
            // state, not derived: `allocated` is a residue that survives into next tick's
            // outstanding demand, and `fundedLastTick` is what next tick's progress is
            // multiplied by. Leaving them out would let two runs diverge in build speed while
            // the fingerprint said they agreed — the exact failure this log exists to catch.
            feed(h, work.allocated);
            feed(h, work.fundedLastTick);
            if (work.retainedCommandId != kInvalidCommandId) {
                feed(h, static_cast<std::uint64_t>(work.retainedCommandId));
            }
        }
    }

    feed(h, match.captures != nullptr);
    if (match.captures != nullptr) {
        feed(h, match.captures->size());
        for (const CaptureWork& work : *match.captures) {
            feed(h, work.armyIndex);
            feed(h, static_cast<std::uint64_t>(work.captor));
            feed(h, static_cast<std::uint64_t>(work.target.index));
            feed(h, static_cast<std::uint64_t>(work.target.generation));
            feed(h, static_cast<std::uint64_t>(work.workTicks));
            feed(h, static_cast<std::uint64_t>(work.progress));
            feed(h, work.demand);
            // The per-tick award, like construction's cached ratio: live state the
            // next progress step reads, so a divergence in the allocator would
            // otherwise pass the fingerprint while changing capture speed.
            feed(h, work.funded);
            feed(h, work.inReach);
        }
    }

    feed(h, match.siloAmmo != nullptr);
    if (match.siloAmmo != nullptr) {
        feed(h, match.siloAmmo->size());
        for (const SiloAmmo& ammo : *match.siloAmmo) {
            feed(h, static_cast<std::size_t>(ammo.owner.index));
            feed(h, static_cast<std::size_t>(ammo.owner.generation));
            feed(h, ammo.weapon);
            feed(h, static_cast<std::size_t>(ammo.slot));
            feed(h, ammo.stored);
            feed(h, ammo.capacity);
            feed(h, static_cast<std::size_t>(ammo.totalTicks));
            feed(h, static_cast<std::size_t>(ammo.elapsedTicks));
            feed(h, ammo.costPerTick);
            feed(h, ammo.delivered);
            feed(h, ammo.autoBuild);
        }
    }

    // The build queues, same presence-then-contents shape: what a silo is ABOUT to build
    // changes the next tick's demand, so two matches differing only in a pending nuke must
    // hash apart now rather than when it lands.
    feed(h, match.siloQueue != nullptr);
    if (match.siloQueue != nullptr) {
        feed(h, match.siloQueue->size());
        for (const SiloBuild& entry : *match.siloQueue) {
            feed(h, static_cast<std::size_t>(entry.owner.index));
            feed(h, static_cast<std::size_t>(entry.owner.generation));
            feed(h, static_cast<std::size_t>(entry.slot));
        }
    }

    // Empty enhancement storage preserves hashes of matches without upgrade tasks.
    if (match.enhancements != nullptr && !match.enhancements->empty()) {
        feedText(h, "enhancement-work");
        feed(h, match.enhancements->size());
        for (const EnhancementWork& work : *match.enhancements) {
            feed(h, static_cast<std::size_t>(work.owner.index));
            feed(h, static_cast<std::size_t>(work.owner.generation));
            feedText(h, work.name);
            feed(h, work.cost);
            feed(h, work.totalBuildTime);
            feed(h, work.buildTimeRemaining);
            feed(h, work.buildPerTick);
            feed(h, work.allocated);
            feed(h, work.fundedLastTick);
            feed(h, work.paused);
        }
    }

    // Redirector state, same presence-then-contents shape as silo ammo: a match with no
    // redirectors folds one flag, so scenes without them keep their existing digest.
    feed(h, match.redirects != nullptr);
    if (match.redirects != nullptr) {
        feed(h, match.redirects->size());
        for (const MissileRedirect& redirect : *match.redirects) {
            feed(h, static_cast<std::size_t>(redirect.owner.index));
            feed(h, static_cast<std::size_t>(redirect.owner.generation));
            feed(h, redirect.radiusElmos);
            feed(h, redirect.cooldownTicks);
            feed(h, redirect.remaining);
        }
    }

    // THE WRECKS, since reclaim reads them back into a rule — the moment FeatureStore.hpp's
    // old note said they would have to be here. How much of a wreck is LEFT depends on who
    // reclaimed it and when, which no unit's row records; two matches that differ only in a
    // half-drained wreck must hash apart on the tick they diverged, not when the mass gets
    // spent. Slot liveness and generation for the same reason the units feed them: a
    // reclaimed-and-replaced slot is a different occupant.
    feed(h, match.features != nullptr);
    if (match.features != nullptr) {
        feed(h, match.features->size());
        const std::span<const Feature> features = match.features->all();
        for (UnitIndex slot = 0; slot < features.size(); ++slot) {
            const Feature& wreck = features[slot];
            feed(h, wreck.at);
            feed(h, wreck.radiusElmos);
            feed(h, static_cast<std::size_t>(wreck.fromType));
            feed(h, wreck.armyIndex);
            feed(h, wreck.health);
            feed(h, wreck.maximumHealth);
            feed(h, wreck.maximumMassReclaim);
            feed(h, wreck.maximumEnergyReclaim);
            feed(h, wreck.massRemaining);
            feed(h, wreck.energyRemaining);
            feed(h, wreck.reclaimWorkRemaining);
            feed(h, wreck.reclaimWorkTotal);
            feed(h, wreck.reclaimFraction);
            feed(h, wreck.damageRatio);
            feed(h, wreck.maximumReclaimPerBuildRate);
            feed(h, wreck.reclaimPerBuildRate);
            feed(h, match.features->slotAlive(slot));
            feed(h, static_cast<std::size_t>(match.features->idAt(slot).generation));
        }
    }

    feed(h, match.commandersEver.size());
    for (const int ever : match.commandersEver) {
        feed(h, ever);
    }
    // The selected C-210 predicate changes which future poll defeats an army.
    feed(h, static_cast<std::uint8_t>(match.victoryMode));
    feed(h, match.baseStorage);
    // The playable rectangle changes which units automatic orders may acquire. It is immutable
    // configuration, but therefore still authoritative: two runs with different bounds are
    // different matches before the first candidate is considered.
    feed(h, match.playableRect.has_value());
    if (match.playableRect) {
        feed(h, match.playableRect->minX);
        feed(h, match.playableRect->maxX);
        feed(h, match.playableRect->minZ);
        feed(h, match.playableRect->maxZ);
    }
    feed(h, match.over);
    // A pending result changes which future tick emits GameOver, so it is as authoritative as
    // the terminal flag itself. The empty optional distinguishes a draw from a named alliance.
    feed(h, match.winnerPending);
    feed(h, match.pendingWinner.has_value());
    if (match.pendingWinner) {
        feed(h, *match.pendingWinner);
    }
    // `potentialWinners` is the survivor set the pending verdict was computed
    // from; it decides whether the fifteen seconds restarts, so it is
    // authority while a verdict is pending and meaningless otherwise.
    if (match.winnerPending) {
        feed(h, match.pendingSurvivorMask);
    }
    feed(h, static_cast<std::uint64_t>(match.winnerStableTicks));
    // C-210's poll phase and delayed cleanups decide future defeats and deaths, so they are
    // authority state rather than implementation detail.
    feed(h, static_cast<std::uint64_t>(match.defeatPollElapsedTicks));
    feed(h, match.defeatCleanupRemainingTicks.size());
    for (const TickCount remaining : match.defeatCleanupRemainingTicks) {
        feed(h, static_cast<std::uint64_t>(remaining));
    }

    // `C-263`'s deficit-covering rates: a Paragon's recomputed override decides
    // next tick's income, so two matches differing only in it must hash apart
    // now. Only NONZERO entries feed — the vector is lazily sized to slotCount,
    // so an all-zero table and an empty one are the same state, and a restored
    // match must hash identically before its first recompute.
    if (match.productionOverrides != nullptr) {
        std::size_t live = 0;
        for (const Resources& value : *match.productionOverrides) {
            live += (value.mass > Mag{} || value.energy > Mag{}) ? 1 : 0;
        }
        if (live > 0) {
            feedText(h, "production-overrides");
            feed(h, live);
            for (UnitIndex slot = 0; slot < match.productionOverrides->size(); ++slot) {
                const Resources& value = (*match.productionOverrides)[slot];
                if (value.mass > Mag{} || value.energy > Mag{}) {
                    feed(h, static_cast<std::size_t>(slot));
                    feed(h, value);
                }
            }
        }
    }

    // `C-296`'s effect pool: an emitter's unit, bone and viewer mask decide
    // what the next beat emits and for whom, so two matches differing only in
    // live effects must hash apart. Present-only like `pathService` — a scene
    // with no pool feeds nothing, keeping the pre-effects stream.
    if (match.effects != nullptr) {
        feed(h, true);
        feed(h, match.effects->size());
        for (const SimEmitter& emitter : *match.effects) {
            feed(h, static_cast<std::size_t>(emitter.unit.index));
            feed(h, static_cast<std::size_t>(emitter.unit.generation));
            feed(h, emitter.bone);
            feedText(h, emitter.effect);
            feed(h, emitter.alive);
            feed(h, emitter.hidden);
            feed(h, emitter.viewerMask);
        }
    }

    // `C-289`'s terrain-type journal: a `SetTerrainTypeRect` write or a live
    // tarmac stamp changes what pathing and placement answer, so two matches
    // differing only in the journal must hash apart. Present-only like
    // `effects` — a scene with no type grid feeds nothing.
    if (match.terrainTypes != nullptr) {
        feed(h, true);
        const auto& journal = match.terrainTypes->journal();
        feed(h, journal.size());
        for (const TerrainStamp& stamp : journal) {
            feed(h, static_cast<std::size_t>(stamp.owner.index));
            feed(h, static_cast<std::size_t>(stamp.owner.generation));
            feed(h, stamp.x0);
            feed(h, stamp.z0);
            feed(h, stamp.x1);
            feed(h, stamp.z1);
            feed(h, static_cast<std::uint64_t>(stamp.type));
        }
    }

    return h;
}

} // namespace rm::sim
