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
       factoryRepeat_(snapshot.factoryRepeat),
       doNotTarget_(snapshot.doNotTarget),
       orders_(snapshot.transforms.size()),
       parents_(snapshot.parents),
       children_(snapshot.children),
       attachmentOffsets_(snapshot.attachmentOffsets),
        attachmentHeights_(snapshot.attachmentHeights),
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
    factoryRepeat_.resize(transforms_.size(), false);
    doNotTarget_.resize(transforms_.size(), false);
    attachmentOffsets_.resize(transforms_.size());
    const bool deriveAttachmentHeights = attachmentHeights_.empty();
    attachmentHeights_.resize(transforms_.size());
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
                   .doNotTarget = doNotTarget_,
                   .parents = parents_,
                   .children = children_,
                   .attachmentOffsets = attachmentOffsets_,
                   .attachmentHeights = attachmentHeights_,
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
        factoryRepeat_.emplace_back(false);
        doNotTarget_.emplace_back(false);
        orders_.emplace_back();
        parents_.emplace_back();
        children_.emplace_back();
        attachmentOffsets_.emplace_back();
        attachmentHeights_.emplace_back();
        generations_.emplace_back();
    }

    transforms_[slot] = request.transform;
    motion_[slot] = request.motion;
    motion_[slot].attached = false;
    health_[slot] = request.health;
    types_[slot] = request.type;
    factoryRepeat_[slot] = false;
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
    // And the same for who last hit the PREVIOUS occupant: `request.health` sets the fresh
    // unit's own, but a caller that leaves it unset would have the newcomer already remember
    // being shot by whoever killed its predecessor. Set from the request so an explicit value
    // still wins.
    health_[slot].lastHitBy = request.health.lastHitBy;
    generations_[slot] = id.generation;
    return id;
}

void UnitStore::reindex(Fx cellSize) { space_.rebuild(*this, cellSize); }

bool UnitStore::attach(UnitId parent, UnitId child) {
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
    attachmentOffsets_[child.index] = {transforms_[child.index].x - transforms_[parent.index].x,
                                        transforms_[child.index].z - transforms_[parent.index].z};
    attachmentHeights_[child.index] = transforms_[child.index].y - transforms_[parent.index].y;
    motion_[child.index].moving = false;
    motion_[child.index].attached = true;
    children_[parent.index].push_back(child);
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
    motion_[child.index].attached = false;
    return true;
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

void UnitStore::propagateAttachments() {
    std::function<void(UnitId)> updateChildren = [&](UnitId parent) {
        for (const UnitId child : children_[parent.index]) {
            if (!alive(child) || parents_[child.index] != parent) {
                continue;
            }
            transforms_[child.index].x = transforms_[parent.index].x + attachmentOffsets_[child.index][0];
            transforms_[child.index].y = transforms_[parent.index].y + attachmentHeights_[child.index];
            transforms_[child.index].z = transforms_[parent.index].z + attachmentOffsets_[child.index][1];
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

void UnitStore::kill(UnitId id) {
    if (!ids_.alive(id)) {
        return;
    }
    // Queue ownership is live command state, not corpse state. Releasing it here lets a shared
    // command ID expire when this was its final member; the other tombstone arrays remain.
    orders_[id.index].clear();
    orders_[id.index].clearObserver();
    factoryRepeat_[id.index] = false;
    doNotTarget_[id.index] = false;
    (void)detach(id);
    for (const UnitId child : children_[id.index]) {
        if (child.index < parents_.size() && parents_[child.index] == id) {
            parents_[child.index].reset();
            attachmentOffsets_[child.index] = {};
            attachmentHeights_[child.index] = {};
            motion_[child.index].attached = false;
        }
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

} // namespace rm::sim
