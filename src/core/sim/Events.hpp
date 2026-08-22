#pragma once

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rm::sim {

// What happened this tick, as data — the sim's outward voice.
//
// WHY THIS EXISTS (PLAN2.md §7 P6.1). Effects are currently wired straight into the caller's
// tick loop: a death appends GPU decal vertices to game state, a shot spawns dust from
// `main.mm`, a construction finishing is read out of `TickReport` and turned into a unit. So
// every effect is a special case at the call site, and there is no way for anything else — a
// sound layer, a HUD ticker, a replay annotator, eventually a Lua host — to learn that a unit
// died without the tick loop being edited to tell it.
//
// An event queue is the seam. `04 §4` catalogues Recoil's ~130 gadget call-ins, and the point
// of that document for us is not the count: it is that a shipping engine's entire scripting
// surface is *notifications of things the sim did*. Building the notification list first is what
// makes the sim's own passes stop knowing about decals.
//
// OUR SUBSET IS TEN OF THE ELEVEN §7 P6.1 NAMES, and the omission is deliberate rather than
// an oversight: `UnitTaken` (`LuaHandle.cpp:1253`) fires before a unit changes team, and nothing
// in this engine transfers ownership — there is no capture, no gifting and no reclaim. Declaring
// a kind nothing can emit would put a branch in every consumer for a case that cannot arise.
// It goes in when a mechanic needs it.
//
// WHAT IS NOT MODELLED, and worth knowing before comparing against `04 §4.2`. Recoil's
// `UnitDestroyed` passes an attacker TRIPLE (id, def, team) and nils all three when the attacker
// is not visible to the receiver (`LuaUtils.cpp:1956-1968`). That is a vision rule, and this
// engine has no vision — every event is fully informed. When intel lands, filtering happens on
// the way out of this queue rather than by making the queue itself lossy.

/// What kind of thing happened.
///
/// A flat enum rather than a variant: an event is fixed-size and trivially copyable, which is
/// what lets a queue of them be hashed the way sim state is and written to a log beside the
/// command log.
enum class EventKind : std::uint8_t {
    /// A unit came into existence. Emitted by whoever spawns — the sim never spawns units, so
    /// this is the one kind the CALLER raises (see `EventQueue::emit`).
    UnitCreated,
    /// A unit finished being built, as opposed to being placed at match start.
    UnitFinished,
    /// A unit took damage. `amount` is what was actually dealt, not what was thrown — a shot
    /// that overkills reports the health it removed, so the numbers sum to what died.
    UnitDamaged,
    /// A unit died. `instigator` is the unit that dealt the fatal blow, which may itself be
    /// dead by now — it is a name for the killer, not a way back to a live unit.
    UnitDestroyed,
    /// A weapon fired. `unit` is the shooter.
    WeaponFired,
    /// A beam weapon delivered a pulse: damage arrived the same tick, nothing flew.
    /// `unit` is the shooter, `at` the STRUCK point and `at2` the muzzle — both carried
    /// because the beam IS the line between them, and by the next tick either end may be
    /// a recycled slot.
    BeamFired,
    /// A projectile reached something, or the ground.
    ProjectileImpact,
    /// A build order was accepted and a construction created.
    ConstructionStarted,
    /// A construction completed. The unit it becomes is a separate `UnitFinished`, because the
    /// sim cannot make one — that needs a model out of the VFS.
    ConstructionFinished,
    /// An army lost its last commander.
    TeamDefeated,
    /// The match ended. `army` is the winning alliance, or `kNoArmy` for a draw.
    GameOver,
};

[[nodiscard]] std::string_view eventKindName(EventKind kind) noexcept;

/// One thing that happened.
///
/// FLAT, with fields that only some kinds use, rather than a variant or a hierarchy. Three
/// reasons: it is trivially copyable so a queue of them hashes and serialises like the rest of
/// sim state; a consumer switches on `kind` and reads the fields that kind documents, which is
/// the same shape `Command` has; and a variant would put the size of the largest case in every
/// slot anyway. Which fields each kind fills is documented on the kind above.
struct Event {
    EventKind kind = EventKind::UnitCreated;

    /// Who it happened to. The shooter for `WeaponFired`; unset for the match-level kinds.
    UnitId unit{};

    /// Who did it, where that is a unit — the killer for `UnitDestroyed`, the shooter for
    /// `UnitDamaged`, the builder for `ConstructionStarted`. Unset when nothing did it.
    UnitId instigator{};

    /// Whose it is. The owning army for the unit kinds, the defeated army for `TeamDefeated`,
    /// the winning alliance for `GameOver`.
    int army = kNoArmy;

    /// How much, where the kind has an amount: damage dealt for `UnitDamaged`.
    Mag amount{};

    /// Where it happened. Carried rather than looked up, because for a death the unit's slot is
    /// about to be recycled and for an impact there is no unit at all.
    std::array<Fx, 3> at{};

    /// The OTHER end, for the kinds that are a line rather than a point — the muzzle of a
    /// `BeamFired`. Zero for everything else.
    std::array<Fx, 3> at2{};
};

[[nodiscard]] bool operator==(const Event& a, const Event& b) noexcept;

/// What happened this tick.
///
/// AN EVENT IS A NOTIFICATION, NOT A FACT ABOUT THE WORLD: a consumer reads the tick's events
/// during that tick or misses them, and a consumer that needs history keeps its own. That is
/// what Recoil's call-ins do — a gadget is handed the event and nothing keeps it — and it is
/// what stops the queue growing without bound over a ten-minute match.
///
/// CALLER-OWNED, like the projectile list: `Match` holds a pointer, so a scene with nothing
/// listening passes null and the sim's emit calls cost a branch. Not a global — two sims must be
/// able to exist in one process (§5.4).
///
/// --------------------------------------------------------------------------------------
/// THE LIFETIME IS A FRAME, AND `beginFrame` IS THE ONLY WAY TO ADVANCE ONE (§7 P10.7).
/// --------------------------------------------------------------------------------------
///
/// This class used to expose `clear()`, and the contract lived in prose split across two
/// headers: this one said the queue is cleared every tick, `Skirmish.hpp` said the CALLER does
/// the clearing and not the tick. Both were accurate and together they were a trap, because
/// the thing they described has no name in the code — so the only way to know where the
/// boundary is was to read both notes and believe them.
///
/// **It had already gone wrong once.** The tick cleared at the top, which threw away the
/// events the caller raises BEFORE it — `UnitCreated` when it spawns a unit, and
/// `ConstructionStarted` when its scripted opponent orders a build. Two event kinds were
/// declared, emitted, and never once observable, and nothing failed: a lost notification looks
/// exactly like a notification nobody sent.
///
/// Two changes close that:
///
///   1. **The frame has a name and a number.** `beginFrame(tick)` replaces `clear()`, so the
///      boundary is a call with an argument rather than a convention, and `frame()` lets a
///      consumer ask whether what it is reading belongs to the tick it thinks it is on.
///      Staleness becomes detectable instead of silent.
///
///   2. **Advancing is IDEMPOTENT, which is what actually kills the bug.** `beginFrame(t)`
///      clears only when `t` differs from the frame already open. So a second caller — a tick
///      pass, a subsystem, anything — that begins the frame it is already in destroys nothing.
///      Had the tick called `beginFrame` instead of `clear`, the original defect would have
///      been a no-op rather than two invisible event kinds.
///
/// The first `beginFrame` always clears, whatever tick it names, so a queue reused for a
/// second match does not carry the first one's last tick into it — two matches both starting
/// at tick 0 is the ordinary case, not a corner one.
class EventQueue {
public:
    void emit(const Event& event) { events_.push_back(event); }

    /// Opens the frame for `tick`, discarding the previous frame's events.
    ///
    /// Called by whoever owns the frame boundary — which is the CALLER, not the tick, because
    /// the caller emits before `tickSkirmish` runs and after it returns. Idempotent within a
    /// tick: see the note above, where that property is the whole point.
    void beginFrame(TickIndex tick) noexcept {
        if (!started_ || tick != frame_) {
            events_.clear();
            frame_ = tick;
            started_ = true;
        }
    }

    /// The tick these events belong to.
    ///
    /// For a consumer that wants to assert it is reading the current tick rather than trusting
    /// that someone advanced the frame. Zero and meaningless before the first `beginFrame`,
    /// which `started()` distinguishes.
    [[nodiscard]] TickIndex frame() const noexcept { return frame_; }

    /// Whether any frame has been opened. False for a queue a test emits into directly, which
    /// is a legitimate use — a unit test for one pass has no frames.
    [[nodiscard]] bool started() const noexcept { return started_; }

    /// The tick's events, in the order the passes raised them — which is the tick order, so
    /// a damage event always precedes the death it caused.
    [[nodiscard]] std::span<const Event> all() const noexcept { return events_; }

    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }

    /// How many of one kind. What a test asserts on, and the reason `UnitDestroyed` being
    /// emitted *exactly once* per death is checkable rather than merely likely.
    [[nodiscard]] std::size_t count(EventKind kind) const noexcept;

    // `clear()` USED TO BE HERE, and its absence is the point rather than a tidy-up: it was
    // the second way to advance a frame, and the one with no argument to get visibly wrong.
    // A test that wants an empty queue makes a new one.

private:
    /// Capacity is kept across ticks, so a match's steady state allocates nothing here.
    std::vector<Event> events_;

    /// The tick the events in `events_` belong to. Meaningless until `started_`.
    TickIndex frame_ = 0;

    /// Whether `beginFrame` has ever run. Distinguishes "frame 0" from "no frame yet", which
    /// is what makes the first advance always clear.
    bool started_ = false;
};

/// Emits `event` if there is anywhere to put it.
///
/// A free function so that every emit site is one line and none of them repeats the null check.
/// The check is real: a `--units` crowd, a test that only wants the store, and the pathfinding
/// harness all run the tick with no listener.
inline void emit(EventQueue* queue, const Event& event) {
    if (queue != nullptr) {
        queue->emit(event);
    }
}

} // namespace rm::sim
