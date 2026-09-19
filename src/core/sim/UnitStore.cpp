#include "core/sim/UnitStore.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

namespace rm::sim {

UnitStore::UnitStore(const Snapshot& snapshot)
    : ids_(snapshot.ids),
      generations_(snapshot.generations),
      transforms_(snapshot.transforms),
      motion_(snapshot.motion),
      health_(snapshot.health),
       types_(snapshot.types),
       enhancements_(snapshot.enhancements),
       factoryRepeat_(snapshot.factoryRepeat),
       productionPaused_(snapshot.productionPaused),
       buildPriority_(snapshot.buildPriority),
       retreatThresholds_(snapshot.retreatThreshold),
       targetFocus_(snapshot.targetFocus),
       retreats_(snapshot.retreats),
       doNotTarget_(snapshot.doNotTarget),
       scriptBitsDisabled_(snapshot.scriptBitsDisabled),
       intelDisabled_(snapshot.intelDisabled),
       maintenanceActive_(snapshot.maintenanceActive),
       orders_(snapshot.transforms.size()),
       parents_(snapshot.parents),
       children_(snapshot.children),
       attachmentOffsets_(snapshot.attachmentOffsets),
        attachmentHeights_(snapshot.attachmentHeights),
        attachmentParentBones_(snapshot.attachmentParentBones),
        attachmentSelfBones_(snapshot.attachmentSelfBones),
        attachmentParentRest_(snapshot.attachmentParentRest),
        attachmentParentRestHeights_(snapshot.attachmentParentRestHeights),
        attachmentSelfRest_(snapshot.attachmentSelfRest),
       attachmentSelfRestHeights_(snapshot.attachmentSelfRestHeights),
       lifetimeRemainingTicks_(snapshot.lifetimeRemainingTicks),
       manipulators_(snapshot.manipulators),
       boneHidden_(snapshot.boneHidden),
       nextCommandSerial_(snapshot.nextCommandSerial),
        nextCommandCounters_(snapshot.nextCommandCounters) {
    std::vector<std::shared_ptr<SharedCommand>> mutableCommands;
    mutableCommands.reserve(snapshot.sharedCommands.size());
    std::vector<std::shared_ptr<const SharedCommand>> sharedCommands;
    sharedCommands.reserve(snapshot.sharedCommands.size());
    for (const SharedCommand& command : snapshot.sharedCommands) {
        auto restored = std::make_shared<SharedCommand>(command);
        sharedCommands.push_back(restored);
        mutableCommands.push_back(std::move(restored));
    }
    if (!snapshot.orders.empty() && snapshot.orders.size() != orders_.size()) {
        throw std::invalid_argument("command queue snapshot does not match unit slots");
    }
    for (std::size_t slot = 0; slot < snapshot.orders.size(); ++slot) {
        if (!orders_[slot].restore(snapshot.orders[slot], sharedCommands)) {
            throw std::invalid_argument("invalid command queue snapshot");
        }
    }
    for (const std::shared_ptr<SharedCommand>& command : mutableCommands) {
        (void)registerCommand(command);
    }
    attachmentOffsets_.resize(transforms_.size());
    const bool deriveAttachmentHeights = attachmentHeights_.empty();
    attachmentHeights_.resize(transforms_.size());
    // Bone records predate no version: older snapshots simply leave them empty, and
    // the resize below recovers the boneless default for every slot.
    attachmentParentBones_.resize(transforms_.size(), kNoBone);
    attachmentSelfBones_.resize(transforms_.size(), kNoBone);
    attachmentParentRest_.resize(transforms_.size());
    attachmentParentRestHeights_.resize(transforms_.size());
    attachmentSelfRest_.resize(transforms_.size());
    attachmentSelfRestHeights_.resize(transforms_.size());
    enhancements_.resize(transforms_.size());
    factoryRepeat_.resize(transforms_.size(), false);
    productionPaused_.resize(transforms_.size(), false);
    buildPriority_.resize(transforms_.size(), BuildPriority::Normal);
    retreatThresholds_.resize(transforms_.size(), RetreatThreshold::Off);
    targetFocus_.resize(transforms_.size(), TargetFocus::Default);
    retreats_.resize(transforms_.size());
    doNotTarget_.resize(transforms_.size(), false);
    // Older snapshots carry neither array: resize to the defaults — every
    // feature on, maintenance consuming — rather than fail the load.
    scriptBitsDisabled_.resize(transforms_.size(), 0);
    // Older snapshots carry no intel-disable array either: every type enabled
    // is the pre-C-283 default.
    intelDisabled_.resize(transforms_.size(), 0);
    maintenanceActive_.resize(transforms_.size(), true);
    // `C-360`'s mirror is derived: a restored store starts unbuffed and the first
    // tick's `syncCheatBuffs` re-stamps it from the armies.
    cheatBuffed_.resize(transforms_.size(), false);
    // Older snapshots carry no lifetime array: `kLifetimeUnset` re-arms every
    // slot from its blueprint on the next tick — a restored Othuy restarts its
    // `Lifetime` rather than failing to load (C-265).
    lifetimeRemainingTicks_.resize(transforms_.size(), kLifetimeUnset);
    // Older snapshots carry neither array: empty lists and all-shown bones are
    // the pre-manipulator defaults, so the resize recovers both.
    manipulators_.resize(transforms_.size());
    boneHidden_.resize(transforms_.size());
    for (std::size_t child = 0; child < transforms_.size(); ++child) {
        if (parents_[child]) {
            if (deriveAttachmentHeights) {
                attachmentHeights_[child] =
                    transforms_[child].y - transforms_[parents_[child]->index].y;
            }
            motion_[child].attached = true;
        }
    }
}

UnitStore::Snapshot UnitStore::snapshot() const {
    Snapshot saved{.ids = ids_.snapshot(),
                   .generations = generations_,
                   .transforms = transforms_,
                   .motion = motion_,
                   .health = health_,
                   .types = types_,
                   .factoryRepeat = factoryRepeat_,
                   .productionPaused = productionPaused_,
                   .buildPriority = buildPriority_,
                   .retreatThreshold = retreatThresholds_,
                   .targetFocus = targetFocus_,
                   .retreats = retreats_,
                   .doNotTarget = doNotTarget_,
                   .scriptBitsDisabled = scriptBitsDisabled_,
                   .intelDisabled = intelDisabled_,
                   .maintenanceActive = maintenanceActive_,
                   .parents = parents_,
                   .children = children_,
                   .attachmentOffsets = attachmentOffsets_,
                   .attachmentHeights = attachmentHeights_,
                   .attachmentParentBones = attachmentParentBones_,
                   .attachmentSelfBones = attachmentSelfBones_,
                   .attachmentParentRest = attachmentParentRest_,
                   .attachmentParentRestHeights = attachmentParentRestHeights_,
                   .attachmentSelfRest = attachmentSelfRest_,
                   .attachmentSelfRestHeights = attachmentSelfRestHeights_,
                   .lifetimeRemainingTicks = lifetimeRemainingTicks_,
                   .nextCommandSerial = nextCommandSerial_,
                   .nextCommandCounters = nextCommandCounters_};
    std::map<const SharedCommand*, std::size_t> sharedCommands;
    for (const CommandQueue& queue : orders_) {
        for (const QueuedCommand& entry : queue.entries()) {
            sharedCommands.try_emplace(&entry.payload(), sharedCommands.size());
        }
    }
    saved.sharedCommands.resize(sharedCommands.size());
    for (const auto& [command, index] : sharedCommands) {
        saved.sharedCommands[index] = *command;
    }
    saved.orders.reserve(orders_.size());
    for (const CommandQueue& queue : orders_) {
        saved.orders.push_back(queue.snapshot(sharedCommands));
    }
    saved.manipulators = manipulators_;
    saved.boneHidden = boneHidden_;
    saved.enhancements = enhancements_;
    return saved;
}

UnitId UnitStore::spawn(const Spawn& request) {
    const UnitId id = ids_.acquire();
    const auto slot = static_cast<std::size_t>(id.index);

    if (slot >= transforms_.size()) {
        // A slot the pool has never handed out before. The pool only ever grows by one, so
        // this is an append rather than a resize — asserted by construction rather than
        // checked, since `IdPool::acquire` is the only thing that produces these.
        transforms_.emplace_back();
        motion_.emplace_back();
        health_.emplace_back();
        types_.emplace_back();
        enhancements_.emplace_back();
        factoryRepeat_.emplace_back(false);
        productionPaused_.emplace_back(false);
        buildPriority_.emplace_back(BuildPriority::Normal);
        intelDisabled_.emplace_back(0);
        retreatThresholds_.emplace_back(RetreatThreshold::Off);
        targetFocus_.emplace_back(TargetFocus::Default);
        scriptBitsDisabled_.emplace_back(0);
        maintenanceActive_.emplace_back(true);
        cheatBuffed_.emplace_back(false);
        retreats_.emplace_back();
        doNotTarget_.emplace_back(false);
        orders_.emplace_back();
        parents_.emplace_back();
        children_.emplace_back();
        attachmentOffsets_.emplace_back();
        attachmentHeights_.emplace_back();
        manipulators_.emplace_back();
        boneHidden_.emplace_back();
        attachmentParentBones_.emplace_back(kNoBone);
        attachmentSelfBones_.emplace_back(kNoBone);
        attachmentParentRest_.emplace_back();
        attachmentParentRestHeights_.emplace_back();
        attachmentSelfRest_.emplace_back();
        attachmentSelfRestHeights_.emplace_back();
        lifetimeRemainingTicks_.emplace_back(kLifetimeUnset);
        generations_.emplace_back();
    }

    transforms_[slot] = request.transform;
    motion_[slot] = request.motion;
    motion_[slot].attached = false;
    health_[slot] = request.health;
    types_[slot] = request.type;
    enhancements_[slot].clear();
    factoryRepeat_[slot] = false;
    productionPaused_[slot] = false;
    intelDisabled_[slot] = 0;
    buildPriority_[slot] = BuildPriority::Normal;
    retreatThresholds_[slot] = RetreatThreshold::Off;
    targetFocus_[slot] = TargetFocus::Default;
    scriptBitsDisabled_[slot] = 0;
    cheatBuffed_[slot] = false;
    maintenanceActive_[slot] = true;
    retreats_[slot] = {};
    doNotTarget_[slot] = false;
    // CLEARED HERE rather than in `kill`, which is the tombstone rule applied to orders: a
    // corpse keeps its arrays so the death blast can read them, and a slot is only wiped when
    // something new moves in. A queue left behind would have the newcomer inherit the dead
    // unit's route — the same class of bug `UnitId`'s generation exists to prevent.
    orders_[slot].clear();
    parents_[slot].reset();
    children_[slot].clear();
    attachmentOffsets_[slot] = {};
    attachmentHeights_[slot] = {};
    attachmentParentBones_[slot] = kNoBone;
    attachmentSelfBones_[slot] = kNoBone;
    // `C-293`/`C-303`: manipulator state and bone visibility are live-unit
    // state — the newcomer starts with no manipulators and every bone shown.
    manipulators_[slot].clear();
    boneHidden_[slot].clear();
    attachmentParentRest_[slot] = {};
    attachmentParentRestHeights_[slot] = {};
    attachmentSelfRest_[slot] = {};
    attachmentSelfRestHeights_[slot] = {};
    // `C-265`: the newcomer re-arms its `Lifetime` from its own blueprint on the
    // next tick — never inheriting the corpse's remaining seconds.
    lifetimeRemainingTicks_[slot] = kLifetimeUnset;
    // And the same for who last hit the PREVIOUS occupant: `request.health` sets the fresh
    // unit's own, but a caller that leaves it unset would have the newcomer already remember
    // being shot by whoever killed its predecessor. Set from the request so an explicit value
    // still wins.
    health_[slot].lastHitBy = request.health.lastHitBy;
    generations_[slot] = id.generation;
    return id;
}

void UnitStore::reindex(Fx cellSize) { space_.rebuild(*this, cellSize); }

std::array<Fx, 2> rotateByHeading(Brad heading, std::array<Fx, 2> local) noexcept {
    const Fx c = fxCos(heading);
    const Fx s = fxSin(heading);
    return {local[0] * c + local[1] * s, local[1] * c - local[0] * s};
}

bool UnitStore::attach(UnitId parent, UnitId child) {
    return attach(parent, child, AttachBones{});
}

bool UnitStore::attach(UnitId parent, UnitId child, AttachBones bones) {
    if (!alive(parent) || !alive(child) || parent == child || parents_[child.index].has_value()) {
        return false;
    }

    for (UnitId ancestor = parent;;) {
        if (ancestor == child) {
            return false;
        }
        const std::optional<UnitId> next = parentOf(ancestor);
        if (!next) {
            break;
        }
        ancestor = *next;
    }

    parents_[child.index] = parent;
    attachmentParentBones_[child.index] = bones.parent;
    attachmentSelfBones_[child.index] = bones.self;
    attachmentParentRest_[child.index] = bones.parentRest;
    attachmentParentRestHeights_[child.index] = bones.parentRestHeight;
    attachmentSelfRest_[child.index] = bones.selfRest;
    attachmentSelfRestHeights_[child.index] = bones.selfRestHeight;
    // The stored offset is what `C-196`'s composition adds AFTER the bones: with
    // bones it is captured bone-relative, so the first propagation reproduces the
    // placement exactly and later ones follow the carrier's swing. Without bones
    // both rotations are zero and this is the historical world-axis capture.
    const std::array<Fx, 2> parentBone =
        rotateByHeading(transforms_[parent.index].heading, bones.parentRest);
    const std::array<Fx, 2> selfBone =
        rotateByHeading(transforms_[child.index].heading, bones.selfRest);
    attachmentOffsets_[child.index] = {transforms_[child.index].x - transforms_[parent.index].x
                                           - parentBone[0] + selfBone[0],
                                       transforms_[child.index].z - transforms_[parent.index].z
                                           - parentBone[1] + selfBone[1]};
    attachmentHeights_[child.index] = transforms_[child.index].y - transforms_[parent.index].y
                                      - bones.parentRestHeight + bones.selfRestHeight;
    motion_[child.index].moving = false;
    motion_[child.index].attached = true;
    children_[parent.index].push_back(child);
    // `C-244`: the carrier's air controller divides its `KLift`/`KTurn`/`KRoll`
    // gains by `(own + Σ cargo) / own`, so the sum is maintained here — the one
    // place every attachment (cargo, death detach, scripted) passes through.
    motion_[parent.index].carriedMass += motion_[child.index].unitMass;
    return true;
}

bool UnitStore::detach(UnitId child) {
    if (!alive(child) || !parents_[child.index]) {
        return false;
    }

    const UnitId parent = *parents_[child.index];
    auto& siblings = children_[parent.index];
    const auto entry = std::find(siblings.begin(), siblings.end(), child);
    if (entry != siblings.end()) {
        siblings.erase(entry);
    }
    parents_[child.index].reset();
    attachmentOffsets_[child.index] = {};
    attachmentHeights_[child.index] = {};
    attachmentParentBones_[child.index] = kNoBone;
    attachmentSelfBones_[child.index] = kNoBone;
    attachmentParentRest_[child.index] = {};
    attachmentParentRestHeights_[child.index] = {};
    attachmentSelfRest_[child.index] = {};
    attachmentSelfRestHeights_[child.index] = {};
    motion_[child.index].attached = false;
    // The mirror of `attach`'s `C-244` bookkeeping — clamped at zero so a
    // double-detach or a zero-mass child cannot push the ratio negative.
    motion_[parent.index].carriedMass =
        std::max(Fx{}, motion_[parent.index].carriedMass - motion_[child.index].unitMass);
    return true;
}

void UnitStore::setAttachmentOffset(UnitId child, std::array<Fx, 2> offset,
                                    Fx height) noexcept {
    if (!alive(child) || !parents_[child.index]) {
        return;
    }
    attachmentOffsets_[child.index] = offset;
    attachmentHeights_[child.index] = height;
}

std::optional<UnitId> UnitStore::parentOf(UnitId child) const noexcept {
    if (!alive(child)) {
        return std::nullopt;
    }
    return parents_[child.index];
}

const std::vector<UnitId>& UnitStore::childrenOf(UnitId parent) const noexcept {
    static const std::vector<UnitId> noChildren;
    return alive(parent) ? children_[parent.index] : noChildren;
}

std::array<Fx, 2> UnitStore::attachmentOffsetOf(UnitId child) const noexcept {
    return alive(child) ? attachmentOffsets_[child.index] : std::array<Fx, 2>{};
}

Fx UnitStore::attachmentHeightOf(UnitId child) const noexcept {
    return alive(child) ? attachmentHeights_[child.index] : Fx{};
}

UnitStore::AttachBones UnitStore::attachmentBonesOf(UnitId child) const noexcept {
    if (!alive(child)) {
        return AttachBones{};
    }
    return AttachBones{.parent = attachmentParentBones_[child.index],
                       .self = attachmentSelfBones_[child.index],
                       .parentRest = attachmentParentRest_[child.index],
                       .parentRestHeight = attachmentParentRestHeights_[child.index],
                       .selfRest = attachmentSelfRest_[child.index],
                       .selfRestHeight = attachmentSelfRestHeights_[child.index]};
}

void UnitStore::propagateAttachments() {
    std::function<void(UnitId)> updateChildren = [&](UnitId parent) {
        for (const UnitId child : children_[parent.index]) {
            if (!alive(child) || parents_[child.index] != parent) {
                continue;
            }
            // `C-196`'s composition with authored REST bones: the parent bone rides
            // the carrier's swing, the child bone hangs off the child's own heading,
            // and the stored offset (captured bone-relative at attach) adds last.
            // Boneless attachments rotate nothing and take the historical path.
            const std::array<Fx, 2> parentBone = rotateByHeading(
                transforms_[parent.index].heading, attachmentParentRest_[child.index]);
            const std::array<Fx, 2> selfBone = rotateByHeading(
                transforms_[child.index].heading, attachmentSelfRest_[child.index]);
            transforms_[child.index].x = transforms_[parent.index].x + parentBone[0]
                                         - selfBone[0] + attachmentOffsets_[child.index][0];
            transforms_[child.index].y = transforms_[parent.index].y
                                         + attachmentParentRestHeights_[child.index]
                                         - attachmentSelfRestHeights_[child.index]
                                         + attachmentHeights_[child.index];
            transforms_[child.index].z = transforms_[parent.index].z + parentBone[1]
                                         - selfBone[1] + attachmentOffsets_[child.index][1];
            // An attached unit rides its carrier: the movement tick skips it, so nothing else
            // ever refreshes its tilt, and a child left with the pitch and roll of the slope
            // it was picked up from would sit askew on a level deck. It takes the carrier's,
            // exactly as a bone-mounted passenger would (`C-196`). Heading stays its own —
            // the captured offsets are world-axis, and turning them is transport scope.
            transforms_[child.index].pitch = transforms_[parent.index].pitch;
            transforms_[child.index].roll = transforms_[parent.index].roll;
            updateChildren(child);
        }
    };

    for (UnitIndex slot = 0; slot < slotCount(); ++slot) {
        const UnitId unit = idAt(slot);
        if (alive(unit) && !parents_[slot]) {
            updateChildren(unit);
        }
    }
}

void UnitStore::kill(UnitId id, RandomStream* random) {
    if (!ids_.alive(id)) {
        return;
    }
    // Queue ownership is live command state, not corpse state. Releasing it here lets a shared
    // command ID expire when this was its final member; the other tombstone arrays remain.
    orders_[id.index].clear();
    orders_[id.index].clearObserver();
    intelDisabled_[id.index] = 0;
    factoryRepeat_[id.index] = false;
    productionPaused_[id.index] = false;
    buildPriority_[id.index] = BuildPriority::Normal;
    scriptBitsDisabled_[id.index] = 0;
    maintenanceActive_[id.index] = true;
    // `C-293`/`C-303`: the manipulator list and the bone mask are live-unit
    // state like the enhancements above — a corpse's pose is nobody's business,
    // and a recycled slot must not inherit either.
    manipulators_[id.index].clear();
    boneHidden_[id.index].clear();
    retreatThresholds_[id.index] = RetreatThreshold::Off;
    targetFocus_[id.index] = TargetFocus::Default;
    retreats_[id.index] = {};
    doNotTarget_[id.index] = false;
    // `C-254`: `SimUnitEnhancements[id]` is live unit state, not corpse state — a dead
    // commander's installed upgrades leave the registry with it, so a respawned or
    // recycled slot never inherits a tombstone's enhancements.
    enhancements_[id.index].clear();
    lifetimeRemainingTicks_[id.index] = kLifetimeUnset;
    (void)detach(id);
    const std::vector<UnitId> cargo = children_[id.index];
    for (const UnitId child : cargo) {
        if (attachmentParentBones_[child.index] == kTractorAttachBone) {
            // `C-381`: a tractor-claw victim is script cargo, not transport
            // cargo — `TractorWatchThread`'s teardown is `DetachAll(muzzle)`,
            // so the holder dying drops the victim ALIVE and targetable
            // again, never through the `C-197` roll below.
            (void)detach(child);
            (void)setDoNotTarget(child, false);
            continue;
        }
        if (random != nullptr) {
            // `r < 0.99` on a uint32 draw — the constant `0x00ea2c5c = 0.99f`
            // read from the image — so the top 1% of the range survives.
            const bool dies =
                random->next() < static_cast<std::uint32_t>(0.99 * 4294967296.0);
            if (!dies) {
                (void)detach(child);
                continue;
            }
        }
        kill(child, random);
    }
    children_[id.index].clear();
    ids_.release(id);
    // The arrays are deliberately left as they were — a dead unit is a tombstone, not a
    // hole (see the header). What zeroes a corpse's collision radius so it stops shoving
    // the living is `retireDead` in the tick, which is a rule about the match rather than
    // about storage, and it stays there.
    //
    // The generation mirror is NOT updated: the slot now holds a generation the pool has
    // moved past, so `idAt` returns a handle that fails `alive`, which is exactly what a
    // caller asking about an empty slot should get.
}

void UnitStore::destroy(UnitId id) {
    if (!ids_.alive(id)) {
        return;
    }
    // `C-261`/`C-265`, retail's `unit:Destroy()`: gone WITHOUT the death path.
    // The radius goes first because it is `retireDead`'s once-per-death guard —
    // zeroed here, the corpse is never reported, never wrecked, never scored.
    // The health goes next because `shootable` reads health, not the handle:
    // without this the destroyed unit stays a valid target until its slot is
    // recycled. Attached cargo is destroyed with it, recursively — a carrier's
    // load dies with the carrier here exactly as it does under `kill`.
    const UnitIndex slot = id.index;
    motion_[slot].moving = false;
    motion_[slot].speedPerTick = Fx{};
    motion_[slot].radiusElmos = Fx{};
    health_[slot].current = Mag{};
    const std::vector<UnitId> cargo = children_[slot];
    for (const UnitId child : cargo) {
        if (attachmentParentBones_[child.index] == kTractorAttachBone) {
            // `C-381`: same exemption as `kill` — `Destroy()` on the holder
            // still runs the claw's `DetachAll(muzzle)` teardown, so the
            // victim is dropped alive rather than destroyed with it.
            (void)detach(child);
            (void)setDoNotTarget(child, false);
            continue;
        }
        destroy(child);
    }
    kill(id);
}

std::optional<CommandId> UnitStore::allocateCommandId(CommandSource source) {
    if (source == kInvalidCommandSource) {
        return std::nullopt;
    }
    std::uint32_t& next = nextCommandCounters_[source];
    for (std::uint32_t tried = 0; tried <= kCommandCounterMask; ++tried) {
        const CommandId candidate = commandId(source, next);
        next = (next + 1U) & kCommandCounterMask;
        if (!commandIdLive(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool UnitStore::consumeCommandId(CommandSource source, CommandId id) {
    if (source == kInvalidCommandSource || commandSource(id) != source) {
        return false;
    }
    std::uint32_t next = nextCommandCounters_[source];
    for (std::uint32_t tried = 0; tried <= kCommandCounterMask; ++tried) {
        const CommandId candidate = commandId(source, next);
        next = (next + 1U) & kCommandCounterMask;
        if (commandIdLive(candidate)) {
            continue;
        }
        if (candidate != id) {
            return false;
        }
        nextCommandCounters_[source] = next;
        return true;
    }
    return false;
}

bool UnitStore::registerCommand(const std::shared_ptr<SharedCommand>& command) {
    if (command == nullptr || command->id == kInvalidCommandId
        || commandSource(command->id) != command->source) {
        return false;
    }
    if (commandIdLive(command->id)) {
        return false;
    }
    liveCommands_[command->id] = command;
    return true;
}

std::shared_ptr<SharedCommand> UnitStore::liveCommand(CommandId id) {
    const auto found = liveCommands_.find(id);
    if (found == liveCommands_.end()) {
        return nullptr;
    }
    std::shared_ptr<SharedCommand> command = found->second.lock();
    if (command == nullptr) {
        liveCommands_.erase(found);
    }
    return command;
}

bool UnitStore::commandIdLive(CommandId id) { return liveCommand(id) != nullptr; }

std::size_t UnitStore::liveCommandCount() {
    for (auto it = liveCommands_.begin(); it != liveCommands_.end();) {
        if (it->second.expired()) {
            it = liveCommands_.erase(it);
        } else {
            ++it;
        }
    }
    return liveCommands_.size();
}

bool UnitStore::setFactoryRepeat(UnitId unit, bool enabled) noexcept {
    if (!alive(unit)) {
        return false;
    }
    factoryRepeat_[unit.index] = enabled;
    return true;
}

bool UnitStore::factoryRepeat(UnitId unit) const noexcept {
    return alive(unit) && factoryRepeat_[unit.index];
}

bool UnitStore::setProductionPaused(UnitId unit, bool paused) noexcept {
    if (!alive(unit)) {
        return false;
    }
    productionPaused_[unit.index] = paused;
    return true;
}

bool UnitStore::productionPaused(UnitId unit) const noexcept {
    return alive(unit) && productionPaused_[unit.index];
}

void UnitStore::syncCheatBuffs(std::span<const Army> armies) noexcept {
    cheatBuffed_.resize(motion_.size(), false);
    for (UnitIndex slot = 0; slot < motion_.size(); ++slot) {
        const int owner = motion_[slot].armyIndex;
        cheatBuffed_[slot] = slotAlive(slot)
                             && owner >= 0
                             && static_cast<std::size_t>(owner) < armies.size()
                             && armies[static_cast<std::size_t>(owner)].cheatEnabled;
    }
}

bool UnitStore::setBuildPriority(UnitId unit, BuildPriority tier) noexcept {
    if (!alive(unit)) {
        return false;
    }
    buildPriority_[unit.index] = tier;
    return true;
}

BuildPriority UnitStore::buildPriority(UnitId unit) const noexcept {
    // A dead or invalid handle has no say in the allocation — its leftover work
    // records (an unfinished capture's `captor`, say) fall back to Normal.
    return alive(unit) ? buildPriority_[unit.index] : BuildPriority::Normal;
}

bool UnitStore::setRetreatThreshold(UnitId unit, RetreatThreshold threshold) noexcept {
    if (!alive(unit)) {
        return false;
    }
    retreatThresholds_[unit.index] = threshold;
    return true;
}

RetreatThreshold UnitStore::retreatThreshold(UnitId unit) const noexcept {
    // Dead men don't flinch: an invalid handle reads as Off, which is also the
    // answer the automation wants when a corpse's slot is walked.
    return alive(unit) ? retreatThresholds_[unit.index] : RetreatThreshold::Off;
}

bool UnitStore::setTargetFocus(UnitId unit, TargetFocus focus) noexcept {
    if (!alive(unit)) {
        return false;
    }
    targetFocus_[unit.index] = focus;
    return true;
}

TargetFocus UnitStore::targetFocus(UnitId unit) const noexcept {
    // A dead handle reads as Default — acquisition for a corpse never runs,
    // but the combat pass also walks slots that died mid-tick.
    return alive(unit) ? targetFocus_[unit.index] : TargetFocus::Default;
}

bool UnitStore::setDoNotTarget(UnitId unit, bool enabled) noexcept {
    if (!alive(unit)) {
        return false;
    }
    doNotTarget_[unit.index] = enabled;
    return true;
}

bool UnitStore::doNotTarget(UnitId unit) const noexcept {
    return alive(unit) && doNotTarget_[unit.index];
}

bool UnitStore::setScriptBitDisabled(UnitId unit, std::uint8_t bit,
                                     bool disabled) noexcept {
    if (!alive(unit) || bit >= 16) {
        return false;
    }
    const std::uint16_t mask = static_cast<std::uint16_t>(1u << bit);
    if (disabled) {
        scriptBitsDisabled_[unit.index] |= mask;
    } else {
        scriptBitsDisabled_[unit.index] &= static_cast<std::uint16_t>(~mask);
    }
    return true;
}

bool UnitStore::scriptBitDisabled(UnitId unit, std::uint8_t bit) const noexcept {
    return alive(unit) && scriptBitDisabledAt(unit.index, bit);
}

bool UnitStore::setIntelEnabled(UnitId unit, IntelType type, bool enabled) noexcept {
    const auto bit = static_cast<std::uint8_t>(type);
    if (!alive(unit) || bit >= kIntelTypeCount || type == IntelType::None) {
        return false;
    }
    const std::uint16_t mask = intelTypeBit(type);
    if (enabled) {
        intelDisabled_[unit.index] &= static_cast<std::uint16_t>(~mask);
    } else {
        intelDisabled_[unit.index] |= mask;
    }
    return true;
}

bool UnitStore::intelEnabled(UnitId unit, IntelType type) const noexcept {
    return alive(unit) && !intelDisabledAt(unit.index, type);
}

bool UnitStore::intelDisabledAt(UnitIndex slot, IntelType type) const noexcept {
    return (intelDisabledMaskAt(slot) & intelTypeBit(type)) != 0;
}

std::uint16_t UnitStore::intelDisabledMaskAt(UnitIndex slot) const noexcept {
    if (slot >= intelDisabled_.size()) {
        return 0;
    }
    // The explicit disables plus whatever the RULEUTC_* toggles switched off —
    // `Unit.lua`'s `OnScriptBitSet` calls `DisableUnitIntel` per type, and the
    // union is what its refcounted `IntelDisables` table converges to.
    return static_cast<std::uint16_t>(
        intelDisabled_[slot] | scriptBitIntelMask(scriptBitsDisabledMaskAt(slot)));
}

bool UnitStore::scriptBitDisabledAt(UnitIndex slot, std::uint8_t bit) const noexcept {
    return slot < scriptBitsDisabled_.size() && bit < 16
           && (scriptBitsDisabled_[slot] & static_cast<std::uint16_t>(1u << bit)) != 0;
}

bool UnitStore::setMaintenanceActive(UnitId unit, bool active) noexcept {
    if (!alive(unit)) {
        return false;
    }
    maintenanceActive_[unit.index] = active;
    return true;
}

bool UnitStore::maintenanceActive(UnitId unit) const noexcept {
    return alive(unit) && maintenanceActive_[unit.index];
}

bool UnitStore::increaseCommandCount(CommandId id, std::uint32_t amount) {
    std::shared_ptr<SharedCommand> command = liveCommand(id);
    if (command == nullptr || amount == 0
        || command->remainingCount > std::numeric_limits<std::uint32_t>::max() - amount
        || command->originalCount > std::numeric_limits<std::uint32_t>::max() - amount) {
        return false;
    }
    command->remainingCount += amount;
    command->originalCount += amount;
    return true;
}

bool UnitStore::decreaseCommandCount(CommandId id, std::uint32_t amount) {
    std::shared_ptr<SharedCommand> command = liveCommand(id);
    if (command == nullptr || amount == 0) {
        return false;
    }
    if (amount < command->remainingCount) {
        command->remainingCount -= amount;
        return true;
    }

    command->remainingCount = 0;
    for (const UnitId unit : command->units) {
        if (unit.index < orders_.size()) {
            (void)orders_[unit.index].removeExact(command.get());
        }
    }
    return true;
}

bool UnitStore::addManipulator(UnitId unit, Manipulator manipulator) noexcept {
    if (!alive(unit)) {
        return false;
    }
    std::vector<Manipulator>& list = manipulators_[unit.index];
    // `C-294`'s precedence sort, done at insert like retail's `SetPrecedence`
    // re-sort (`0x6417b0`): the tick walks the list as-is. `upper_bound` keeps
    // equal precedences in registration order — retail's list append.
    const auto at = std::upper_bound(list.begin(), list.end(), manipulator.precedence,
        [](std::int32_t precedence, const Manipulator& entry) {
            return precedence < entry.precedence;
        });
    list.insert(at, manipulator);
    return true;
}

bool UnitStore::setBoneHidden(UnitId unit, std::int32_t bone, bool hidden) noexcept {
    if (!alive(unit) || bone < 0) {
        return false;
    }
    std::vector<std::uint64_t>& mask = boneHidden_[unit.index];
    const auto word = static_cast<std::size_t>(bone) / 64;
    if (word >= mask.size()) {
        mask.resize(word + 1, 0);
    }
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(bone) % 64);
    if (hidden) {
        mask[word] |= bit;
    } else {
        mask[word] &= ~bit;
    }
    return true;
}

bool UnitStore::boneHiddenAt(UnitIndex slot, std::int32_t bone) const noexcept {
    if (bone < 0 || slot >= boneHidden_.size()) {
        return false;
    }
    const auto word = static_cast<std::size_t>(bone) / 64;
    const std::vector<std::uint64_t>& mask = boneHidden_[slot];
    return word < mask.size()
        && (mask[word] & (std::uint64_t{1} << (static_cast<std::size_t>(bone) % 64))) != 0;
}

} // namespace rm::sim
