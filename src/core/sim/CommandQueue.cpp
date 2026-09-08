#include "core/sim/CommandQueue.hpp"
#include "core/sim/UnitCatalog.hpp"

#include <algorithm>

namespace rm::sim {
namespace {

/// Whether two positional orders point at close enough the same place.
///
/// Ground distance, and SQUARED on both sides so nothing takes a root: the comparison is the
/// only thing wanted and `fxHypot` would cost a CORDIC sweep per queued order per click. `Mag`
/// for the products because two coordinates 8,192 elmos apart square past `Fx`'s ceiling.
template <typename Order>
[[nodiscard]] bool withinCancelDistance(const Order& a, const Order& b) noexcept {
    const Fx dx = a.targetX - b.targetX;
    const Fx dz = a.targetZ - b.targetZ;
    const Mag squared = Mag::fromRaw(static_cast<MagRaw>(dx.raw()) * dx.raw())
                        + Mag::fromRaw(static_cast<MagRaw>(dz.raw()) * dz.raw());
    const Mag limit = Mag::fromRaw(static_cast<MagRaw>(kCancelDistance.raw())
                                   * kCancelDistance.raw());
    return squared < limit;
}

template <typename Order>
[[nodiscard]] bool sameOrderValue(const Order& a, const Order& b,
                                 const UnitCatalog* catalog = nullptr) noexcept {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case CommandKind::Dive:
    case CommandKind::Stop:
    case CommandKind::ToggleFactoryRepeat:
    case CommandKind::CancelFactoryBuild:
        return false;
    case CommandKind::Move:
    case CommandKind::AttackMove:
    case CommandKind::Patrol:
        return withinCancelDistance(a, b);
    case CommandKind::Attack:
        if (a.target.generation != 0 || b.target.generation != 0) {
            return a.target == b.target;
        }
        return withinCancelDistance(a, b);
    case CommandKind::Build: {
        if (a.buildType != b.buildType) return false;
        // Recoil CommandAI.cpp:GetCancelQueued uses the building's footprint here.
        // A fixed 17-elmo movement radius cancels distinct adjacent T1 generators.
        // Intake has already snapped both sites onto the build grid. Without a
        // catalog, only identical sites prove a duplicate; never guess a footprint.
        const auto* def = catalog ? catalog->def(a.buildType) : nullptr;
        if (!def) return a.targetX == b.targetX && a.targetZ == b.targetZ;
        const Fx halfX = Fx::fromInt(std::max(1,def->footprintSquaresX)*kSquareSize/2);
        const Fx halfZ = Fx::fromInt(std::max(1,def->footprintSquaresZ)*kSquareSize/2);
        const Fx dx = a.targetX-b.targetX, dz = a.targetZ-b.targetZ;
        return dx >= -halfX && dx <= halfX && dz >= -halfZ && dz <= halfZ;
    }
    case CommandKind::Reclaim:
    case CommandKind::ReclaimUnit:
    case CommandKind::Overcharge:
    case CommandKind::Assist:
    case CommandKind::Guard:
    case CommandKind::Repair:
        return a.target == b.target;
    case CommandKind::Script:
        // Script command data is opaque. Issuing it twice means two invocations, not a
        // shift-click cancellation gesture whose equality the core could safely infer.
        return false;
    }
    return false;
}

[[nodiscard]] std::shared_ptr<const SharedCommand> standalonePayload(const Command& command) {
    return std::make_shared<const SharedCommand>(SharedCommand{
        .tick = command.tick,
        .player = command.player,
        .kind = command.kind,
        .queued = command.queued,
        .units = {command.unit},
        .targetX = command.targetX,
        .targetZ = command.targetZ,
        .target = command.target,
        .buildType = command.buildType,
    });
}

[[nodiscard]] QueuedCommand standaloneEntry(const Command& command) {
    return QueuedCommand{command.unit, standalonePayload(command)};
}

} // namespace

void CommandQueue::notify(CommandQueueStatus status, const QueuedCommand* command) const {
    if (!observer_) {
        return;
    }
    observer_(CommandQueueChange{
                  .status = status,
                  .id = command != nullptr ? command->payload().id : kInvalidCommandId,
                  .kind = command != nullptr ? command->kind() : CommandKind::Stop,
              },
              *this);
}

void CommandQueue::eraseAt(std::size_t index, bool abortHead) {
    if (index >= queue_.size()) {
        return;
    }
    QueuedCommand& removing = queue_[index];
    if (removing.kind() == CommandKind::Script && removing.scriptState().created
        && scriptTasks_ != nullptr) {
        scriptTasks_->onDestroy(removing.unit(), removing.payload().scriptTask,
                                removing.payload().scriptData, removing.scriptState());
    }
    const QueuedCommand removed = removing;
    if (abortHead && index == 0) {
        notify(CommandQueueStatus::Aborted, &removed);
        activeSerial_.reset();
    }
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
    notify(CommandQueueStatus::Removed, &removed);
}

bool sameOrder(const Command& a, const Command& b) noexcept {
    return sameOrderValue(a, b);
}

CommandQueue::Result CommandQueue::give(const Command& command, bool queued) {
    return give(standaloneEntry(command), queued);
}

CommandQueue::Result CommandQueue::give(QueuedCommand command, bool queued,
                                       const UnitCatalog* catalog) {
    if (!queued) {
        // A plain order forgets everything. Recoil clears before appending
        // (`CommandAI.cpp:998-1011`); the cancel rules below then never fire, because there is
        // nothing left to match against.
        clear();
        queue_.push_back(std::move(command));
        notify(CommandQueueStatus::Inserted, &queue_.back());
        return Result::Replaced;
    }

    // FROM THE BACK, which is Recoil's direction (`GetCancelQueued` walks `q.end()` down to
    // `q.begin()`). It matters: shift-clicking the same place twice in a row should take away
    // the one just added, not the identical one from earlier in a patrol route.
    for (std::size_t behind = queue_.size(); behind > 0; --behind) {
        const std::size_t at = behind - 1;
        if (!sameOrderValue(command.payload(), queue_[at].payload(), catalog)) {
            continue;
        }
        const bool wasCurrent = at == 0;
        eraseAt(at, true);
        // ONE match only, and then stop looking — Recoil's "only delete one non-build order"
        // (`:1400`). Worth being honest about: it is currently indistinguishable from removing
        // ALL matches, because `give` is the only way into the queue and it cancels a match
        // rather than adding one, so a duplicate can never accumulate. It is kept because the
        // moment something other than a player click can enqueue — a patrol that crosses
        // itself, a formation move — the two stop being the same act, and that is a bad time
        // to find out the loop was unbounded.
        return wasCurrent ? Result::CancelledCurrent : Result::Cancelled;
    }

    // A patrol queue rotates, so its oldest command is not necessarily at the front. Retail
    // inserts a newly added waypoint at the end of the CURRENT lap: immediately before that
    // oldest command, unless the lap already starts at the head. The immutable serial matters
    // when several clicks landed on the same tick.
    if (command.kind() == CommandKind::Patrol && queue_.size() >= 2
        && queue_.front().kind() == CommandKind::Patrol) {
        const auto oldest = std::min_element(
            queue_.begin(), queue_.end(), [](const QueuedCommand& a, const QueuedCommand& b) {
                return a.payload().creationSerial < b.payload().creationSerial;
            });
        if (oldest != queue_.begin()) {
            const std::size_t at = static_cast<std::size_t>(oldest - queue_.begin());
            queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(at), std::move(command));
            notify(CommandQueueStatus::Inserted, &queue_[at]);
            return Result::Appended;
        }
    }

    queue_.push_back(std::move(command));
    notify(CommandQueueStatus::Inserted, &queue_.back());
    return Result::Appended;
}

const QueuedCommand* CommandQueue::finish() {
    if (!queue_.empty()) {
        eraseAt(0, false);
    }
    activeSerial_.reset();
    return current();
}

const QueuedCommand* CommandQueue::abort() {
    if (!queue_.empty()) {
        eraseAt(0, true);
    }
    activeSerial_.reset();
    return current();
}

const QueuedCommand* CommandQueue::cycle() {
    if (!queue_.empty()) {
        queue_.push_back(queue_.front());
        queue_.pop_front();
        notify(CommandQueueStatus::Reordered, &queue_.back());
    }
    activeSerial_.reset();
    return current();
}

std::size_t CommandQueue::cycleExact(const SharedCommand* command) {
    if (command == nullptr) {
        return 0;
    }
    const auto found = std::ranges::find_if(queue_, [command](const QueuedCommand& entry) {
        return &entry.payload() == command;
    });
    if (found == queue_.end()) {
        return 0;
    }
    const bool movedActiveHead = found == queue_.begin() && queue_.size() > 1
                              && activeSerial_ == found->payload().creationSerial;
    const QueuedCommand moved = *found;
    queue_.erase(found);
    queue_.push_back(moved);
    notify(CommandQueueStatus::Reordered, &queue_.back());
    if (movedActiveHead) {
        activeSerial_.reset();
    }
    return 1;
}

void CommandQueue::append(QueuedCommand command) {
    queue_.push_back(std::move(command));
    notify(CommandQueueStatus::Inserted, &queue_.back());
}

CommandQueue::Snapshot CommandQueue::snapshot(
    const std::map<const SharedCommand*, std::size_t>& sharedCommands) const {
    Snapshot result{.activeSerial = activeSerial_};
    result.entries.reserve(queue_.size());
    for (const QueuedCommand& entry : queue_) {
        const auto found = sharedCommands.find(&entry.payload());
        if (found != sharedCommands.end()) {
            result.entries.push_back(
                {.execution = entry.snapshot(), .sharedCommand = found->second});
        }
    }
    return result;
}

bool CommandQueue::restore(
    const Snapshot& snapshot, std::span<const std::shared_ptr<const SharedCommand>> sharedCommands) {
    std::deque<QueuedCommand> restored;
    for (const SnapshotEntry& entry : snapshot.entries) {
        if (entry.sharedCommand >= sharedCommands.size() || !sharedCommands[entry.sharedCommand]) {
            return false;
        }
        restored.emplace_back(entry.execution, sharedCommands[entry.sharedCommand]);
    }
    queue_ = std::move(restored);
    activeSerial_ = snapshot.activeSerial;
    return true;
}

void CommandQueue::remove(CommandKind kind) {
    const bool removesCurrent = current() != nullptr && current()->kind() == kind;
    for (std::size_t after = queue_.size(); after > 0; --after) {
        const std::size_t at = after - 1;
        if (queue_[at].kind() == kind) {
            eraseAt(at, removesCurrent);
        }
    }
}

std::size_t CommandQueue::removeExact(const SharedCommand* command) {
    if (command == nullptr) {
        return 0;
    }
    std::size_t removed = 0;
    for (std::size_t after = queue_.size(); after > 0; --after) {
        const std::size_t at = after - 1;
        if (&queue_[at].payload() == command) {
            eraseAt(at, true);
            ++removed;
        }
    }
    return removed;
}

void CommandQueue::clear() {
    notify(CommandQueueStatus::Cleared);
    while (!queue_.empty()) {
        eraseAt(queue_.size() - 1, true);
    }
    activeSerial_.reset();
}

std::vector<Command> CommandQueue::all() const {
    std::vector<Command> result;
    result.reserve(queue_.size());
    for (const QueuedCommand& entry : queue_) {
        result.push_back(entry.asCommand());
    }
    return result;
}

void CommandQueue::append(Command command) {
    queue_.push_back(standaloneEntry(command));
    notify(CommandQueueStatus::Inserted, &queue_.back());
}

} // namespace rm::sim
