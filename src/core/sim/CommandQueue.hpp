#pragma once

#include "core/sim/Command.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rm::sim {

enum class CommandQueueStatus : std::uint8_t {
    Inserted,
    Removed,
    Cleared,
    Aborted,
    Reordered,
};

struct CommandQueueChange {
    CommandQueueStatus status = CommandQueueStatus::Inserted;
    CommandId id = kInvalidCommandId;
    CommandKind kind = CommandKind::Stop;
};

// The orders a unit still has to carry out.
//
// WHY IT IS A DEQUE OF COMMANDS AND NOT A CLASS HIERARCHY (PLAN2.md §6.4). Recoil's
// `Sim/Units/CommandAI/CommandAI.h` is `CCommandAI` → `CMobileCAI` →
// `CBuilderCAI`/`CFactoryCAI`/`CAirCAI`, with virtual dispatch, a static command-description
// cache, and a `CObject` death-dependence graph — `AddDeathDependence`, `DependentDied`,
// `ClearCommandDependencies`.
//
// Most of that weight exists because a queued command holds a raw POINTER to its target unit
// or feature, so the engine has to be told when a target dies or the pointer dangles. Our
// `Command` holds a `UnitId`, so a dead target is simply a handle that fails to resolve when
// the order is started — and the entire dependence graph evaporates. Domain specialisation
// (mobile vs factory vs builder) is a property of the unit's TYPE, read from the catalog, not
// a subclass of its order list.
//
// WHAT IS WORTH COPYING, THOUGH, and it is the reason this file is not just a `std::deque`:
// the cancel-queued rules. `WillCancelQueued`/`GetCancelQueued`/`CancelCommands`
// (`CommandAI.cpp:1305-1400`) encode behaviour players feel in their hands — shift-clicking a
// waypoint you already queued REMOVES it rather than queueing a second one — and it is worth
// copying rather than re-deriving from what feels right.

/// How far apart two positional orders may be and still count as the same order, in elmos.
///
/// Recoil's own `COMMAND_CANCEL_DIST = 17.0f` (`CommandAI.cpp:49`), and it transfers because of
/// what it MEANS rather than because both games use the same units: it is about one unit's own
/// width, which is the distance at which a player clicking twice meant one place. Measured
/// against the corpus we actually load, the FA blueprints' median `SizeX` is 2.3 ogrids — 18.4
/// elmos — so Recoil's figure lands within 8% of a median FA unit's width. That is D8 the
/// right way round: the number survives the move because it describes a footprint and not a
/// speed.
inline constexpr Fx kCancelDistance = Fx::fromInt(17);

/// Whether two commands are THE SAME ORDER, for cancellation purposes.
///
/// Recoil's `GetCancelQueued` predicate, reduced to the kinds this engine has:
///
///   - Different kinds never match. Recoil also matches `CMD_ATTACK` against a one-parameter
///     `CMD_FIGHT`; we have no fight order, so that case has nothing to say here.
///   - `Stop` never matches anything, including another `Stop`. Recoil reaches the same answer
///     structurally — its predicate needs one or three parameters and a stop has none — and it
///     is the right answer: stopping twice is not a request to un-stop.
///   - Positional movement orders match within `kCancelDistance`; targeted attacks match by
///     handle.
///   - `Build` additionally requires the SAME BLUEPRINT. Recoil compares build footprints for
///     overlap; comparing the type as well is stricter in the one direction that matters,
///     since two different buildings queued on the same spot are a plan rather than a
///     duplicate.
[[nodiscard]] bool sameOrder(const Command& a, const Command& b) noexcept;

/// How many orders one unit may already be holding and still take another.
///
/// Retail's cap, read instruction by instruction at `0x006f7e30`–`0x006f7e48` inside
/// `Sim::IssueCommand`'s per-unit body (`C-214`, `C-231`). It divides the command vector's
/// byte span by 8 and compares against `0x1f4`:
///
///     movl 0x10(%edi), %ecx     ; begin — a null begin skips the test entirely
///     movl 0x14(%edi), %eax     ; end
///     subl %ecx, %eax
///     sarl $0x3, %eax           ; 8-byte WeakPtr entries
///     cmpl $0x1f4, %eax         ; 500
///     jbe  accept               ; <= 500 takes the order
///     testb %bl, %bl            ; the incoming command's "clear the queue" flag
///     je   reject               ; not clearing, and over the cap: this unit is skipped
///
/// So the comparison is STRICTLY GREATER: a queue holding exactly 500 still accepts, and the
/// ceiling a player can reach is 501. `C-214` recorded the number and not the boundary, and
/// the boundary is the part a test can fail on.
///
/// A clearing order — every unqueued click, and every `Stop` — bypasses it, which is what
/// keeps a unit that has been shift-clicked to the cap from becoming uncommandable.
inline constexpr std::size_t kCommandQueueCap = 500;

/// One unit's mutable execution of a shared immutable command.
class QueuedCommand {
public:
    struct Snapshot {
        UnitId unit{};
        Fx targetX{};
        Fx targetZ{};
        UnitId target{};
        std::optional<std::array<Fx, 2>> patrolOrigin;
        bool returningToPatrolOrigin = false;
    };

    QueuedCommand(UnitId unit, std::shared_ptr<const SharedCommand> payload)
        : unit_(unit), targetX_(payload->targetX), targetZ_(payload->targetZ),
          target_(payload->target), payload_(std::move(payload)) {}
    QueuedCommand(Snapshot snapshot, std::shared_ptr<const SharedCommand> payload)
        : unit_(snapshot.unit), targetX_(snapshot.targetX), targetZ_(snapshot.targetZ),
          target_(snapshot.target), payload_(std::move(payload)),
          patrolOrigin_(std::move(snapshot.patrolOrigin)),
          returningToPatrolOrigin_(snapshot.returningToPatrolOrigin) {}

    [[nodiscard]] Snapshot snapshot() const {
        return {.unit = unit_,
                .targetX = targetX_,
                .targetZ = targetZ_,
                .target = target_,
                .patrolOrigin = patrolOrigin_,
                .returningToPatrolOrigin = returningToPatrolOrigin_};
    }

    [[nodiscard]] const SharedCommand& payload() const noexcept { return *payload_; }
    [[nodiscard]] UnitId unit() const noexcept { return unit_; }
    [[nodiscard]] CommandKind kind() const noexcept { return payload_->kind; }
    [[nodiscard]] Fx targetX() const noexcept { return targetX_; }
    [[nodiscard]] Fx targetZ() const noexcept { return targetZ_; }
    [[nodiscard]] UnitId target() const noexcept { return target_; }
    [[nodiscard]] UnitTypeIndex buildType() const noexcept { return payload_->buildType; }

    void setTargetPosition(Fx x, Fx z) noexcept {
        targetX_ = x;
        targetZ_ = z;
    }
    void setTarget(UnitId target) noexcept { target_ = target; }

    /// A one-unit compatibility snapshot for logs, UI, and tests. It is a copy; mutating it
    /// cannot change either queue execution or immutable issued intent.
    [[nodiscard]] Command asCommand() const noexcept {
        return Command{
            .tick = payload_->tick,
            .player = payload_->player,
            .kind = payload_->kind,
            .queued = payload_->queued,
            .unit = unit_,
            .targetX = targetX_,
            .targetZ = targetZ_,
            .target = target_,
            .buildType = payload_->buildType,
        };
    }

    [[nodiscard]] const std::optional<std::array<Fx, 2>>& patrolOrigin() const noexcept {
        return patrolOrigin_;
    }
    [[nodiscard]] bool returningToPatrolOrigin() const noexcept {
        return returningToPatrolOrigin_;
    }

    void setPatrolOrigin(std::array<Fx, 2> origin) noexcept { patrolOrigin_ = origin; }
    void routeToPatrolOrigin() noexcept {
        if (!patrolOrigin_) {
            return;
        }
        targetX_ = (*patrolOrigin_)[0];
        targetZ_ = (*patrolOrigin_)[1];
        target_ = UnitId{};
        returningToPatrolOrigin_ = true;
    }
    void restorePatrolDestination() noexcept {
        targetX_ = payload_->targetX;
        targetZ_ = payload_->targetZ;
        target_ = payload_->target;
        returningToPatrolOrigin_ = false;
    }

private:
    UnitId unit_{};
    Fx targetX_{};
    Fx targetZ_{};
    UnitId target_{};
    std::shared_ptr<const SharedCommand> payload_;
    std::optional<std::array<Fx, 2>> patrolOrigin_;
    bool returningToPatrolOrigin_ = false;
};

/// A unit's order list.
class CommandQueue {
public:
    using Observer = std::function<void(const CommandQueueChange&, const CommandQueue&)>;

    struct SnapshotEntry {
        QueuedCommand::Snapshot execution;
        std::size_t sharedCommand = 0;
    };
    struct Snapshot {
        std::vector<SnapshotEntry> entries;
        std::optional<CommandSerial> activeSerial;
    };

    /// What giving an order did, because a caller shows different feedback for each.
    enum class Result : std::uint8_t {
        /// The queue was cleared and this order is now the only one. A plain order.
        Replaced,
        /// Appended behind what was already there. A shift-order.
        Appended,
        /// This order matched one already queued, so THAT ONE WAS REMOVED and this one was
        /// not added. The shift-click-to-un-queue behaviour.
        Cancelled,
        /// Cancelled the order the unit is CURRENTLY carrying out, so it should stop as well
        /// as forget. Distinguished from `Cancelled` because the caller has to interrupt
        /// what the unit is doing, which Recoil does by pushing a stop to the front.
        CancelledCurrent,
    };

    /// Gives an order.
    ///
    /// `queued` is the shift key. Without it the order REPLACES everything: Recoil clears the
    /// whole queue before appending (`CommandAI.cpp:998-1011`), which is why a right-click
    /// makes a unit forget its route rather than adding to it.
    ///
    /// With it, the cancel rules run first. If the new order matches something queued, that
    /// match is removed and the new order is dropped — one match only, which is Recoil's
    /// "only delete one non-build order" (`:1400`). This is deliberately checked BEFORE
    /// appending, because the two are alternatives: a shift-click on a waypoint is either
    /// adding it or taking it away.
    Result give(const Command& command, bool queued);
    Result give(QueuedCommand command, bool queued);

    /// Installs a synchronous read-only observer. Queue status is notification, not simulation
    /// state, so the callback itself is neither serialized nor hashed.
    void setObserver(Observer observer) { observer_ = std::move(observer); }
    void clearObserver() { observer_ = nullptr; }

    [[nodiscard]] const QueuedCommand* currentEntry() const noexcept {
        return queue_.empty() ? nullptr : &queue_.front();
    }
    [[nodiscard]] QueuedCommand* currentEntryMutable() noexcept {
        return queue_.empty() ? nullptr : &queue_.front();
    }

    /// The queue head, or null when the unit has no orders.
    ///
    /// The head can be waiting for the next command-dispatch beat after a cyclic rotation. Use
    /// `active()` when a simulation pass needs the order the unit is actually carrying out.
    [[nodiscard]] const QueuedCommand* current() const noexcept { return currentEntry(); }

    /// The same head, writable — for the chase, which records where it last routed to in
    /// the order's own targetX/Z. Only the head: orders behind it are promises not yet
    /// started, and nothing may rewrite a promise.
    [[nodiscard]] QueuedCommand* currentMutable() noexcept { return currentEntryMutable(); }

    /// The head only when it has been dispatched and started.
    [[nodiscard]] const QueuedCommand* activeEntry() const noexcept {
        const QueuedCommand* head = currentEntry();
        return head != nullptr && activeSerial_ == head->payload().creationSerial ? head
                                                                                  : nullptr;
    }

    [[nodiscard]] QueuedCommand* activeEntryMutable() noexcept {
        QueuedCommand* head = currentEntryMutable();
        return head != nullptr && activeSerial_ == head->payload().creationSerial ? head
                                                                                  : nullptr;
    }

    [[nodiscard]] const QueuedCommand* active() const noexcept { return activeEntry(); }

    [[nodiscard]] QueuedCommand* activeMutable() noexcept { return activeEntryMutable(); }

    /// Marks the current head as started by the command-dispatch stage.
    void markCurrentActive() noexcept {
        const QueuedCommand* head = currentEntry();
        activeSerial_ = head != nullptr ? std::optional{head->payload().creationSerial}
                                        : std::nullopt;
    }

    /// Leaves the same head in place but makes its next leg wait for the next dispatch beat.
    /// A patrol's hidden origin uses this between destination and return legs.
    void deactivateCurrent() noexcept { activeSerial_.reset(); }

    /// The current order is done. Drops it and returns the next, or null.
    ///
    /// Named for what the CALLER knows — that the unit arrived, or its target died — because
    /// the queue cannot tell: whether an order is complete is a fact about the world, and the
    /// queue holds no world.
    const QueuedCommand* finish();

    /// Moves the completed head to the back and returns the next waypoint. Patrol uses this
    /// instead of finishing, so its two endpoints remain a loop rather than being consumed.
    const QueuedCommand* cycle();

    /// Moves the entry owning this exact shared command to the back. Factory guard uses this
    /// for a repeated build selected behind the guarded factory's active order.
    [[nodiscard]] std::size_t cycleExact(const SharedCommand* command);

    /// Adds an engine-generated order without player cancellation rules. Used only for the
    /// starting-point waypoint paired with a player's first patrol destination.
    void append(Command command);
    void append(QueuedCommand command);

    /// Removes every order of one kind. A patrol with fewer than two points is no loop, so
    /// cancelling either endpoint dissolves its remaining synthetic half through this path.
    void remove(CommandKind kind);

    /// Removes entries owning this exact shared command, never value-equal neighbors.
    [[nodiscard]] std::size_t removeExact(const SharedCommand* command);

    void clear();

    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }

    /// Whether retail would refuse another order that does not clear the queue.
    [[nodiscard]] bool atCapacity() const noexcept { return queue_.size() > kCommandQueueCap; }

    /// Every order, current first. For drawing the queue in the world and for the state hash.
    ///
    /// A COPY into a vector rather than a span, because a `deque` is not contiguous. The
    /// callers are the hash (once per tick per unit) and the renderer (for a selected unit),
    /// so a copy of a handful of small structs is the cheaper thing to get right.
    [[nodiscard]] std::vector<Command> all() const;

    /// Every order, without copying — for the state hash, which reads and discards.
    [[nodiscard]] const std::deque<QueuedCommand>& entries() const noexcept { return queue_; }

    [[nodiscard]] Snapshot snapshot(
        const std::map<const SharedCommand*, std::size_t>& sharedCommands) const;
    [[nodiscard]] bool restore(const Snapshot& snapshot,
                               std::span<const std::shared_ptr<const SharedCommand>> sharedCommands);

private:
    void notify(CommandQueueStatus status, const QueuedCommand* command = nullptr) const;
    void eraseAt(std::size_t index, bool abortHead);

    /// A DEQUE, because the two operations are push-back and pop-front and Recoil's own
    /// cancel path also pushes to the front. A vector would make every completed order an
    /// O(n) erase from the head.
    std::deque<QueuedCommand> queue_;

    /// Identity of the command whose execution state is live. A rotated or newly exposed head
    /// deliberately has no active serial until the next dispatch step starts it.
    std::optional<CommandSerial> activeSerial_;
    Observer observer_;
};

} // namespace rm::sim
