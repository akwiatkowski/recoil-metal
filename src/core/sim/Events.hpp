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
#include <string>
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

/// `UnitStore.hpp`'s intel enum, forward-declared so `Event::intelType` can carry
/// it as a byte without this header dragging the store in.
enum class IntelType : std::uint8_t;

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
    /// For upgrades, instigator is the replaced unit handle.
    UnitFinished,
    /// A unit took damage. `amount` is the post-armour/shield amount before health clips it, so
    /// an overkill reports the full incoming blow rather than only the health it removed
    /// (retail `DealDamage`, C-111).
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
    /// An active bubble absorbed damage. `unit` is its generator and `amount` was absorbed.
    ShieldDamaged,
    /// A bubble reached zero power and stopped intercepting damage.
    ShieldCollapsed,
    /// A collapsed or partially depleted bubble returned to full power.
    ShieldRestored,
    /// A build order was accepted and a construction created.
    ConstructionStarted,
    /// A construction completed. The unit it becomes is a separate `UnitFinished`, because the
    /// sim cannot make one — that needs a model out of the VFS.
    ConstructionFinished,
    /// A construction's fraction crossed a quarter boundary — retail's
    /// `OnBuildProgress`/`OnBeingBuiltProgress` thresholds at 25/50/75%
    /// (`C-102`). `amount` is the boundary crossed (0.25, 0.5 or 0.75), `at`
    /// the site, `instigator` the builder. One event per boundary per beat —
    /// a sacrifice or a fast assist can cross two at once and reports both.
    ConstructionProgress,
    /// A unit reached a new veterancy level. `amount` is the level it reached, 1 through 5,
    /// as a whole number — the one kind whose amount is a count rather than a quantity of
    /// health, because there is nowhere else for it to go and a separate field used by one
    /// kind would cost every event eight bytes.
    UnitVeteranPromoted,
    /// An army lost its last commander.
    TeamDefeated,
    /// The match ended. `army` is the winning alliance, or `kNoArmy` for a draw.
    GameOver,
    /// `C-372`'s terrain-contact callback (`OnAnimTerrainCollision`,
    /// `0xe71308`): a collision manipulator's volume touched the ground — in
    /// the bounded slice, the unit came to rest on a surface (a landing
    /// aircraft, a unit placed on the map). `at` is where it touched.
    AnimTerrainCollision,
    /// The leaving edge (`OnNotAnimTerrainCollision`, `0xe712ec`): the unit
    /// stopped resting on a surface — a takeoff, or a unit lifted onto a
    /// carrier.
    AnimTerrainCollisionEnd,
    /// `C-301`'s unit-contact callback (`OnAnimCollision`, `0xe71320` — the
    /// footfall path into `Unit.lua:2289`): the unit's collision volume
    /// touched another unit's. `at` is where the touched unit stands.
    AnimCollision,
    /// A sim-owned emitter emitted for its viewers (`C-296`/`C-374`):
    /// `unit` is the emitter's carrier, `at` the attach point, `bone` the
    /// bone index it rides, `viewerMask` the per-army visibility bits, and
    /// `visualId` the effect name. Emitted once per spawn — the record itself
    /// lives in `Match::effects`.
    EffectEmitted,
    /// `C-125`: a unit's horizontal motion state changed — started or stopped
    /// moving. `motionHorz` carries retail's `MotionHorzEvent` value
    /// (`enum_registrations.tsv`: Cruise 0, TopSpeed 1, Stopping 2, Stopped 3);
    /// only Cruise and Stopped are reachable — the movers have no acceleration
    /// model, so TopSpeed/Stopping have no transition to hang on.
    MotionHorz,
    /// `C-125`: a unit's vertical motion state changed. `motionVert` carries
    /// retail's `MotionVertEvent` value (Up 0, Top 1, Hover 2, Down 3, Bottom 4);
    /// Hover is unmodelled — the air layer has no hover state, so a flyer is
    /// climbing, cruising at altitude, descending, or on the ground.
    MotionVert,
    /// `C-125`: a unit's turn state changed. `motionTurn` carries retail's
    /// `MotionTurnEvent` value (Straight 0, Turn 1, SharpTurn 2); a quarter-turn
    /// or more of heading error is SharpTurn, anything less a Turn.
    MotionTurn,
    /// `C-125`: a unit's coarse motion state changed — attached to a transport
    /// or detached from one. `motionState` carries retail's `MotionState` value
    /// (None 0, Attached 1, Ballistic 2, Crashed 3); Ballistic and Crashed are
    /// unmodelled — nothing here flies a ballistic arc or a crash trajectory.
    MotionState,
    /// `C-282`: one of a unit's intel bits changed for one army — the per-bit
    /// edge behind `OnIntelChange(blip, type, val)`. `unit` is the blip's unit,
    /// `army` the army whose picture changed, `intelType` the bit that flipped
    /// (Radar/Sonar/Omi/LOSNow — the four `CIntel::Update` writes), `intelValue`
    /// its new state, `at` the unit's position.
    IntelChanged,
    /// `C-282`: a unit appeared on an army's picture for the first time — the
    /// edge behind `OnDetectedBy(army)`. `unit` is the detected unit, `army` the
    /// detecting army, `at` where it was first seen.
    DetectedBy,
    /// `C-237` (`0x0060B942`): a capture task activated on a target — the
    /// target-side `OnStartBeingCaptured`. `unit` is the target, `instigator`
    /// the captor. Fires when the captor first comes in reach, which is when
    /// retail's task flips `UNITSTATE_BeingCaptured` on.
    StartBeingCaptured,
    /// `C-237` (`0x0060B968`): the captor-side `OnStartCapture`, emitted
    /// immediately after `StartBeingCaptured` — retail's start order is
    /// target-then-captor. `unit` is the captor, `instigator` the target.
    StartCapture,
    /// `C-237` (`0x0060B822`): the captor-side `OnStopCapture` at completion —
    /// FIRST of the completion triple. `unit` is the captor, `instigator` the
    /// target. Retail's dead-target preamble exit passes the captor as the arg;
    /// that corner is unread in the bounded slice.
    StopCapture,
    /// `C-237` (`0x0060B84A`): the target-side `OnStopBeingCaptured`, second of
    /// the completion triple. `unit` is the target, `instigator` the captor.
    StopBeingCaptured,
    /// `C-237` (`0x0060B870`): the target-side `OnCaptured`, last of the
    /// completion triple — retail's Lua body then performs the ownership
    /// transfer, which is why this precedes `UnitCreated` for the replacement.
    /// `unit` is the target, `instigator` the captor.
    Captured,
    /// `C-237` (spec §"Deactivation reports failure"): a capture task
    /// deactivated on a LIVE target — the captor-side `OnFailedCapture`.
    /// `unit` is the captor, `instigator` the target.
    FailedCapture,
    /// `C-237`: the target-side `OnFailedBeingCaptured`, emitted with
    /// `FailedCapture`. `unit` is the target, `instigator` the captor.
    FailedBeingCaptured,
    /// `C-361`: an `ArmyStats` trigger fired — the sim-side record behind
    /// `brain:OnStatsTrigger`. `army` is the owning army, `statName` the name
    /// Lua registered.
    ArmyStatTriggered,
    /// `C-361`: an army hit its unit cap — the sim-side record behind
    /// `brain:OnUnitCapLimitReached`. `army` is the capped army.
    UnitCapLimitReached,
    /// `C-346` (`SimPing.lua:19`): a `SpawnPing` SimCallback placed a ping —
    /// `army` is the owner, `at` the location, `text` the ping kind (`data.Mesh`),
    /// `markerId` the assigned marker slot or -1 for a timed ping. The event IS
    /// the contract: no UI consumes it here, allied brains observe it through the
    /// callback surface (`DoPingCallbacks`).
    PingSpawned,
    /// `C-346` (`SimPing.lua:106`): an `UpdateMarker` SimCallback mutated a
    /// marker — `army` the owner, `markerId` the slot, `markerAction` the verb
    /// (delete/move/rename/renew), `at` the new location on a move, `text` the
    /// new name on a rename.
    MarkerUpdated,
    /// `C-344` (`SessionSendChatMessage`): a chat line or taunt crossed the
    /// session channel — `army` the sender, `to` the recipient (`kChatAll`,
    /// `kChatAllies`, or an army index), `text` the message, `tauntIndex` the
    /// taunt-table row or -1 for plain text.
    ChatMessage,
    /// `C-344` (`build_templates.lua:89`): a shared build template crossed the
    /// same channel — `army` the sender, `to` the recipient army, `text` the
    /// serialized template table.
    TemplateShared,
};

/// `UpdateMarker`'s action word (`SimPing.lua:106`), as a byte on the event and
/// the marker record. `Renew` is retail's re-sync request — it mutates nothing
/// and only re-sends, so it is an event action with no state transition.
enum class MarkerAction : std::uint8_t {
    None = 0,
    Delete = 1,
    Move = 2,
    Rename = 3,
    Renew = 4,
};

/// Retail's native projectile impact classifier (`C-124`, `C-170`).
///
/// Values are explicit because Lua receives these through the engine's name table, and the
/// pending projectile state is hashed. `Invalid` is only the reset value; an emitted impact is
/// always one of the other eleven.
enum class ImpactType : std::uint8_t {
    Invalid = 0,
    Terrain = 1,
    Water = 2,
    Air = 3,
    Underwater = 4,
    Projectile = 5,
    ProjectileUnderwater = 6,
    Prop = 7,
    Shield = 8,
    Unit = 9,
    UnitAir = 10,
    UnitUnderwater = 11,
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

    /// UnitFinished's construction founder. Separate from instigator, which carries
    /// the replaced handle for upgrade selection handoff. AI manager inheritance
    /// follows this builder's current owner, including ordinary factory production.
    UnitId builder{};

    /// Whose it is. The owning army for the unit kinds, the defeated army for `TeamDefeated`,
    /// the winning alliance for `GameOver`.
    int army = kNoArmy;

    /// How much, where the kind has an amount: incoming post-mitigation damage for
    /// `UnitDamaged`.
    Mag amount{};

    /// Where it happened. Carried rather than looked up, because for a death the unit's slot is
    /// about to be recycled and for an impact there is no unit at all.
    std::array<Fx, 3> at{};

    /// The OTHER end, for the kinds that are a line rather than a point — the muzzle of a
    /// `BeamFired`, or initial muzzle position for `WeaponFired`.
    std::array<Fx, 3> at2{};

    /// Native impact classification for `ProjectileImpact`; `Invalid` for every other event.
    ImpactType impactType = ImpactType::Invalid;

    /// Presentation metadata copied before a shooter can die or its slot be reused.
    std::string visualId;
    Fx visualDuration{};
    std::array<Fx,3> visualDirection{};
    /// WeaponFired diagnostic: actual initial velocity in elmos/tick, before
    /// gravity or guidance. Kept separate from the authored effect direction.
    std::array<Fx,3> launchVelocity{};
    /// WeaponFired diagnostic: the bore line at fire — posed trunnion to
    /// muzzle, world elmos — so the trial-aim metric can compare the barrel
    /// with the launch WITHOUT a drawn frame (the headless `--play` pre-run
    /// poses no instances). Zero for an unmuzzled weapon, which has no bore.
    std::array<Fx,3> visualBarrel{};

    /// The bone an `AnimCollision`-family or `EffectEmitted` event names —
    /// `kNoBone` (-1) when the kind has no bone. A literal rather than the
    /// constant so this header stays free of `Manipulator.hpp`.
    std::int32_t bone = -1;

    /// `C-374`'s per-army visibility mask on `EffectEmitted`: bit `armyIndex`
    /// set means that army may see the emission. All-ones for every other
    /// kind and for a match with no fog of war.
    std::uint64_t viewerMask = ~std::uint64_t{0};
    /// `C-125` motion payloads: retail's `MotionHorzEvent`/`MotionVertEvent`/
    /// `MotionTurnEvent`/`MotionState` values for the matching `Motion*` kinds,
    /// `0xff` for every other kind and for the never-emitted states. Kept as
    /// bytes rather than four enum classes because the values are a wire format
    /// — Lua receives the integer, not a name.
    std::uint8_t motionHorz = 0xff;
    std::uint8_t motionVert = 0xff;
    std::uint8_t motionTurn = 0xff;
    std::uint8_t motionState = 0xff;

    /// `C-282` intel payloads: which bit flipped (`IntelType`, stored as its
    /// byte so this header needs only the forward declaration) and its new
    /// state, for `IntelChanged`. `IntelType::None`/false for other kinds.
    std::uint8_t intelType = 0;
    bool intelValue = false;

    /// `C-361`: the name Lua registered for an `ArmyStatTriggered` event.
    /// A string because the trigger name is Lua's, not an enum the sim owns.
    std::string statName;

    /// `C-344`/`C-346` callback payloads: the recipient for `ChatMessage` and
    /// `TemplateShared` (`kChatAll`/`kChatAllies` or an army index; `kNoArmy`
    /// elsewhere), the marker slot for `PingSpawned`/`MarkerUpdated` (-1 for a
    /// timed ping), the taunt-table row for a taunt `ChatMessage` (-1 for plain
    /// text), and the `UpdateMarker` verb for `MarkerUpdated`.
    int to = kNoArmy;
    int markerId = -1;
    int tauntIndex = -1;
    MarkerAction markerAction = MarkerAction::None;

    /// The text half of the callback kinds: the chat line for `ChatMessage`,
    /// the serialized template for `TemplateShared`, the ping kind for
    /// `PingSpawned`, the new name for a rename `MarkerUpdated`. A string like
    /// `statName` because the payload is Lua's, not an enum the sim owns.
    std::string text;
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
