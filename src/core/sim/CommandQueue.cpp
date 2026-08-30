#include "core/sim/CommandQueue.hpp"

#include <algorithm>

namespace rm::sim {
namespace {

/// Whether two positional orders point at close enough the same place.
///
/// Ground distance, and SQUARED on both sides so nothing takes a root: the comparison is the
/// only thing wanted and `fxHypot` would cost a CORDIC sweep per queued order per click. `Mag`
/// for the products because two coordinates 8,192 elmos apart square past `Fx`'s ceiling.
[[nodiscard]] bool withinCancelDistance(const Command& a, const Command& b) noexcept {
    const Fx dx = a.targetX - b.targetX;
    const Fx dz = a.targetZ - b.targetZ;
    const Mag squared = Mag::fromRaw(static_cast<MagRaw>(dx.raw()) * dx.raw())
                        + Mag::fromRaw(static_cast<MagRaw>(dz.raw()) * dz.raw());
    const Mag limit = Mag::fromRaw(static_cast<MagRaw>(kCancelDistance.raw())
                                   * kCancelDistance.raw());
    return squared < limit;
}

} // namespace

bool sameOrder(const Command& a, const Command& b) noexcept {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case CommandKind::Stop:
        // No target, nothing to compare, and nothing sensible to do with a match if there
        // were one. Recoil arrives here too: its predicate wants one or three parameters.
        return false;
    case CommandKind::Move:
    case CommandKind::AttackMove:
    case CommandKind::Patrol:
        return withinCancelDistance(a, b);
    case CommandKind::Attack:
        // A TARGETED attack matches on the target, not the ground: the chase rewrites its
        // own targetX/Z as it pursues, so position comparison would never cancel — and
        // "attack that unit" clicked twice plainly names the same order however far the
        // unit has walked. A ground attack (no target) keeps the distance rule.
        if (a.target.generation != 0 || b.target.generation != 0) {
            return a.target == b.target;
        }
        return withinCancelDistance(a, b);
    case CommandKind::Build:
        return a.buildType == b.buildType && withinCancelDistance(a, b);
    case CommandKind::Reclaim:
        // Two reclaims of one wreck are one order, however the clicks landed — the handle
        // names the wreck the way a targeted attack's names its victim.
        return a.target == b.target;
    case CommandKind::Overcharge:
        // Same rule: the target names the order.
        return a.target == b.target;
    case CommandKind::Assist:
        return a.target == b.target;
    }
    return false;
}

CommandQueue::Result CommandQueue::give(const Command& command, bool queued) {
    if (!queued) {
        // A plain order forgets everything. Recoil clears before appending
        // (`CommandAI.cpp:998-1011`); the cancel rules below then never fire, because there is
        // nothing left to match against.
        queue_.clear();
        activeSerial_.reset();
        queue_.push_back(command);
        return Result::Replaced;
    }

    // FROM THE BACK, which is Recoil's direction (`GetCancelQueued` walks `q.end()` down to
    // `q.begin()`). It matters: shift-clicking the same place twice in a row should take away
    // the one just added, not the identical one from earlier in a patrol route.
    for (std::size_t behind = queue_.size(); behind > 0; --behind) {
        const std::size_t at = behind - 1;
        if (!sameOrder(command, queue_[at])) {
            continue;
        }
        const bool wasCurrent = at == 0;
        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(at));
        if (wasCurrent) {
            activeSerial_.reset();
        }
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
    if (command.kind == CommandKind::Patrol && queue_.size() >= 2
        && queue_.front().kind == CommandKind::Patrol) {
        const auto oldest = std::min_element(
            queue_.begin(), queue_.end(), [](const Command& a, const Command& b) {
                return a.creationSerial < b.creationSerial;
            });
        if (oldest != queue_.begin()) {
            queue_.insert(oldest, command);
            return Result::Appended;
        }
    }

    queue_.push_back(command);
    return Result::Appended;
}

const Command* CommandQueue::finish() {
    if (!queue_.empty()) {
        queue_.pop_front();
    }
    activeSerial_.reset();
    return current();
}

const Command* CommandQueue::cycle() {
    if (!queue_.empty()) {
        queue_.push_back(queue_.front());
        queue_.pop_front();
    }
    activeSerial_.reset();
    return current();
}

void CommandQueue::remove(CommandKind kind) {
    const bool removesCurrent = current() != nullptr && current()->kind == kind;
    std::erase_if(queue_, [kind](const Command& command) { return command.kind == kind; });
    if (removesCurrent) {
        activeSerial_.reset();
    }
}

void CommandQueue::clear() noexcept {
    queue_.clear();
    activeSerial_.reset();
}

std::vector<Command> CommandQueue::all() const {
    return std::vector<Command>{queue_.begin(), queue_.end()};
}

} // namespace rm::sim
