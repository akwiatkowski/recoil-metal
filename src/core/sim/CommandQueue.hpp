#pragma once

#include "core/sim/Command.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace rm::sim {

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

/// A unit's order list.
class CommandQueue {
public:
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

    /// The queue head, or null when the unit has no orders.
    ///
    /// The head can be waiting for the next command-dispatch beat after a cyclic rotation. Use
    /// `active()` when a simulation pass needs the order the unit is actually carrying out.
    [[nodiscard]] const Command* current() const noexcept {
        return queue_.empty() ? nullptr : &queue_.front();
    }

    /// The same head, writable — for the chase, which records where it last routed to in
    /// the order's own targetX/Z. Only the head: orders behind it are promises not yet
    /// started, and nothing may rewrite a promise.
    [[nodiscard]] Command* currentMutable() noexcept {
        return queue_.empty() ? nullptr : &queue_.front();
    }

    /// The head only when it has been dispatched and started.
    [[nodiscard]] const Command* active() const noexcept {
        const Command* head = current();
        return head != nullptr && activeSerial_ == head->creationSerial ? head : nullptr;
    }

    [[nodiscard]] Command* activeMutable() noexcept {
        Command* head = currentMutable();
        return head != nullptr && activeSerial_ == head->creationSerial ? head : nullptr;
    }

    /// Marks the current head as started by the command-dispatch stage.
    void markCurrentActive() noexcept {
        const Command* head = current();
        activeSerial_ = head != nullptr ? std::optional{head->creationSerial} : std::nullopt;
    }

    /// The current order is done. Drops it and returns the next, or null.
    ///
    /// Named for what the CALLER knows — that the unit arrived, or its target died — because
    /// the queue cannot tell: whether an order is complete is a fact about the world, and the
    /// queue holds no world.
    const Command* finish();

    /// Moves the completed head to the back and returns the next waypoint. Patrol uses this
    /// instead of finishing, so its two endpoints remain a loop rather than being consumed.
    const Command* cycle();

    /// Adds an engine-generated order without player cancellation rules. Used only for the
    /// starting-point waypoint paired with a player's first patrol destination.
    void append(Command command) { queue_.push_back(std::move(command)); }

    /// Removes every order of one kind. A patrol with fewer than two points is no loop, so
    /// cancelling either endpoint dissolves its remaining synthetic half through this path.
    void remove(CommandKind kind);

    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }

    /// Every order, current first. For drawing the queue in the world and for the state hash.
    ///
    /// A COPY into a vector rather than a span, because a `deque` is not contiguous. The
    /// callers are the hash (once per tick per unit) and the renderer (for a selected unit),
    /// so a copy of a handful of small structs is the cheaper thing to get right.
    [[nodiscard]] std::vector<Command> all() const;

    /// Every order, without copying — for the state hash, which reads and discards.
    [[nodiscard]] const std::deque<Command>& orders() const noexcept { return queue_; }

private:
    /// A DEQUE, because the two operations are push-back and pop-front and Recoil's own
    /// cancel path also pushes to the front. A vector would make every completed order an
    /// O(n) erase from the head.
    std::deque<Command> queue_;

    /// Identity of the command whose execution state is live. A rotated or newly exposed head
    /// deliberately has no active serial until the next dispatch step starts it.
    std::optional<CommandSerial> activeSerial_;
};

} // namespace rm::sim
