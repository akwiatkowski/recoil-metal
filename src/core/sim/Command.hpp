#pragma once

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/PathService.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/TickRate.hpp"
#include "core/sim/UnitCatalog.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rm::sim {

class UnitStore;
class FeatureStore;
class Intel;
class ScriptTaskHost;
struct PlayableRect;
struct GuardWork;
struct SelfDestructWork;

/// When an input is applied relative to one simulation tick.
enum class CommandPhase : std::uint8_t {
    PreTick = 0,
    PostSpawn = 1,
};

// An order, as a value — and the single path every order takes into the sim.
//
// WHY THIS EXISTS (PLAN2.md §7 P2.5). Until now a human's click called `orderTo` directly and
// the scripted opponent called `orderRouted` directly. Two callers, two paths, and no record
// of either. That makes three things impossible at once:
//
//   1. **The success criterion.** §1.3 is "the same command log produces the same match". There
//      was no command log — the phrase named something that did not exist. A match was
//      reproducible only by replaying a hash log, which proves two runs agreed without being
//      able to say what either of them was asked to do.
//   2. **Testing the human path.** A click went through AppKit, so "does shift-click order the
//      whole selection" could only be answered by clicking. A command is a struct; a test can
//      make one.
//   3. **Authorisation.** With orders applied at the call site, nothing checked that the player
//      issuing one commands the unit. In a single-player game that is invisible; it is the
//      first thing a networked one gets wrong.
//
// A command is DATA, deliberately: it carries when, who, what and where, and it carries no
// behaviour. `applyCommand` is the behaviour, and it is one function so that a human's order
// and a script's are not merely similar but identical — the property §7 P2.5 asks to be tested.
//
// AND IT IS NOW A QUEUE TOO (§6.4, §7 P4.1). This comment used to say the opposite — "a command
// here is applied on the tick it names and is then history" — and that is no longer true: a
// command goes into the unit's `CommandQueue`, and `advanceOrders` starts the next one when the
// current finishes. The queue was built out of commands rather than instead of them, exactly as
// the note predicted.

/// What an order asks for.
///
/// A closed set, and short on purpose: only orders the engine can actually carry out live here.
/// A `Build` command names a type rather than a blueprint path, because the sim does not know
/// what a path is (`UnitCatalog`).
enum class CommandKind : std::uint8_t {
    /// Walk to a place, routed around what is impassable.
    Move = 0,
    /// Stop where you are, cancelling the route.
    Stop = 1,
    /// Walk at a place — the same as `Move` today, since attacking is what a unit does on
    /// arrival anyway. Separate because the two mean different things to a player, and
    /// collapsing them would lose the distinction in the log.
    Attack = 2,
    /// Begin something at a place. The caller turns a finished construction into a unit, so
    /// the sim records the intent and the economy does the rest.
    Build = 3,
    /// Harvest a wreck: walk into build reach and drain it into the army's store
    /// (`core/sim/Reclaim.hpp`). `target` names the FEATURE — the id spaces are separate
    /// pools, and a reclaim resolves its handle only against the feature store, so the
    /// reuse of an integer between them cannot cross the streams. The order completes when
    /// the wreck is gone, however many reclaimers emptied it.
    Reclaim = 4,
    /// Fire the unit's MANUAL weapon — the commander's OverCharge — at `target`: walk
    /// into that weapon's range, wait for the energy, fire ONCE, done. The pursuit is the
    /// attack's; the differences are the reach (the manual weapon's, not the guns'), the
    /// energy gate (`Weapon::energyRequired`, drained the tick it fires), and completion —
    /// one shot per order, whether or not the target survives it (`fireOvercharge`
    /// forgets the target on firing, which is what retires the order).
    Overcharge = 5,
    /// Walk to a place, stopping to engage visible enemies encountered on the way, then
    /// resume toward the original destination when the temporary target is gone.
    AttackMove = 6,
    /// Repeatedly walk between queued waypoints, engaging enemies by the same rule as an
    /// attack-move. The first patrol order automatically includes the unit's starting point.
    Patrol = 7,
    /// Pour this builder's BuildRate into whatever `target` is building — the guard order,
    /// on the working half of the game. The pursuit is the attack's with the BUILD reach
    /// for a weapon range: follow the target, hold beside it, and while it has an
    /// unfinished construction the assister's rate is added to it every tick
    /// (`applyAssistance`). A target with an idle queue is simply waited on — the order is
    /// a standing one and completes only when the target dies.
    Assist = 8,
    /// Toggle a factory's mobile-production repeat state. This is an authoritative action, not
    /// presentation state, because it changes what completion does to the production command.
    ToggleFactoryRepeat = 9,
    /// Restore a damaged allied unit while it remains in build reach. Unlike Assist, this is a
    /// finite repair task and does not contribute to construction.
    Repair = 10,
    /// Run a named Lua task through the native command scheduler. Core simulation owns the
    /// lifecycle and opaque serializable bytes; an injected ScriptTaskHost owns Lua itself.
    Script = 11,
    /// Escort an allied unit; combat capability and engineering capability stay independent.
    Guard = 12,
    /// Cancel a factory product or structure upgrade by stable ID. Cancelling an upgrade also
    /// removes dependent later tiers. The historical name is retained in semantic logs.
    CancelFactoryBuild = 13,
    /// Reclaim a UNIT rather than a wreck: walk into build reach and un-build it, crediting
    /// its build cost as its fraction falls (`core/sim/Reclaim.hpp`). `target` names a unit,
    /// which is what makes this a distinct kind from `Reclaim`: the kind byte IS the tag that
    /// keeps a `UnitId` and a `FeatureId` from ever being compared (the C-157 gate). Retail
    /// has one UNITCOMMAND_Reclaim for both; our numbering is already our own. Completes when
    /// the target is gone — fully reclaimed by this unit or destroyed by anything else.
    ReclaimUnit = 14,
    /// Immediate vertical-layer control; preserves the horizontal order queue.
    Dive = 15,
    /// Take a hostile unit intact: walk into build reach and convert it, funded by energy
    /// per beat (`core/sim/Capture.hpp`). `target` names a unit, like `ReclaimUnit` — the
    /// kind byte is what keeps the C-157 exemption pointed at capture rather than at
    /// salvage. Retail has one UNITCOMMAND_Capture; our numbering is already our own.
    /// Completes with a replacement-entity transfer to the captor's army; retires when
    /// the target is gone however it went.
    Capture = 16,
    /// Fire one counted MANUAL projectile — a silo's tactical or strategic missile — at
    /// `target` or at the `targetX`/`targetZ` ground zero. The pursuit is the attack's
    /// when a unit is named; a position aims where it was clicked. The differences from
    /// Overcharge are the gate (a round in the tube, not an energy bill — an empty silo
    /// HOLDS the order while the build catches up) and the completion: `fireMissiles`
    /// spends one `SiloAmmo` the tick the shot leaves and marks the order spent on the
    /// launcher itself, which no click can produce.
    MissileLaunch = 17,
    /// Pause or resume a unit's production: its construction, its silo ammunition,
    /// its enhancement work, its repair and reclaim and capture tasks, its build-power
    /// assistance, and its own income all hold while the flag is set. An authoritative
    /// action like `ToggleFactoryRepeat`, not presentation state — it changes what the
    /// economy charges for and what the command stage advances.
    ToggleProduction = 18,
    /// Cycle a producer's construction priority Normal → High → Low. Authoritative like
    /// `ToggleProduction`: the allocator serves High before Normal before Low out of
    /// whatever the tier above left, so the tier changes what a stalled tick pays for.
    /// Never queued — it acts on intake, the same beat it was issued.
    CycleBuildPriority = 19,
    /// Cycle a mobile unit's retreat threshold Off → Low → Medium → High. Same
    /// authoritative-but-unqueued shape as `ToggleProduction`: the setting itself
    /// is sim state, and `updateRetreats` is the pass that acts on it.
    CycleRetreatThreshold = 20,
    /// Board the carrier `target` names: walk to it, and while the order is at
    /// the head the carrier — idle — flies to the unit and comes down to take it
    /// aboard. The order retires when the unit attaches; a carrier that can
    /// never fit the class drops it outright, while a full one leaves it waiting
    /// (`core/sim/Transport.hpp`).
    LoadTransport = 21,
    /// Carry the cargo to `targetX`/`targetZ`, come down, and set it on the
    /// ground there. The transport-side counterpart of LoadTransport: it retires
    /// when the last child steps off, so a carrier still airborne counts as
    /// mid-order until touchdown.
    UnloadTransport = 22,
    /// A standing route: hold at the position where the order started — the
    /// beacon — and carry whatever is ordered to the beacon to `targetX`/`targetZ`,
    /// returning for the next load until the queue is cleared. Never completes on
    /// its own; the loop is the point.
    Ferry = 23,
    /// Cycle an armed unit's target focus Default → Snipe → AirOnly →
    /// EconomyOnly. Same authoritative-but-unqueued shape as
    /// `CycleRetreatThreshold`: the setting itself is sim state, and
    /// `nearestTarget` is the pass that reads it.
    CycleTargetFocus = 24,
    /// Set a producer's construction priority to the tier `priority` names —
    /// the two-state REGULAR/PRIORITY control the economy window offers, which
    /// a cycle cannot express (Normal → High → Low → Normal has no direct
    /// High → Normal edge). Same authoritative-but-unqueued shape as
    /// `CycleBuildPriority`: it acts on intake, the same beat it was issued.
    SetBuildPriority = 25,
    /// Queue one tactical-slot missile build — retail's `IssueSiloBuildTactical`
    /// (`0x006faa30`), the left-click on the silo's build button reaching the same
    /// `SiloAddBuild` the idle refill uses (`C-241`). Authoritative and unqueued like
    /// `ToggleProduction`: it mutates the silo's build queue at intake, and a silo that
    /// is full — stored plus queued at capacity — refuses it.
    SiloBuildTactical = 26,
    /// The strategic counterpart — `IssueSiloBuildNuke` (`0x006fab90`), slot 1.
    SiloBuildNuke = 27,
    /// Flip the silo's auto-mode (`SetAutoMode`, the build button's right-click): with it
    /// off an idle silo stops re-queueing itself, while already-queued and manually
    /// issued builds still run. Same intake-immediate shape as `ToggleProduction`.
    ToggleSiloAuto = 28,
    /// Toggle the unit's self-destruct countdown (`C-345`): five seconds, then the unit
    /// dies by the ordinary death path — wreck, blast, kill stats and all. Retail
    /// reaches the same behaviour through a SimCallback (`ToggleSelfDestruct` →
    /// `selfdestruct.lua`'s `StartCountdown` + `unit:Kill()`); here it is an
    /// intake-immediate command like `ToggleProduction`, and a second issue cancels the
    /// countdown, which is what "toggle" means.
    SelfDestruct = 29,
};

[[nodiscard]] constexpr bool isGuardCommand(CommandKind kind) noexcept {
    return kind == CommandKind::Assist || kind == CommandKind::Guard;
}

/// Whether a unit has any production to pause: builds, fabricates, holds a counted
/// silo, or declares the retail `RULEUTC_ProductionToggle` cap. The cap alone is not
/// the gate — only two dozen retail units declare it (fabricators, generators,
/// engineering stations), while FAF offers pause to everything that produces, which
/// is what "pausable production everywhere" means. Def-level only so the command
/// panel can answer the same question without a catalog.
[[nodiscard]] bool canPauseProduction(const unitdef::UnitDef& def) noexcept;

/// Whether a unit may carry a build-priority tier: every producer whose demand the
/// allocator can actually tier — builders, factories, and counted-projectile silos,
/// whose ammo build is bucketed by owner like any other work. Anything else would
/// hold a flag that never moves a resource.
[[nodiscard]] bool canSetBuildPriority(const unitdef::UnitDef& def) noexcept;

/// One order, from one player, on one tick.
///
/// Fixed-size and trivially copyable, which is what lets a log of them be compared byte for
/// byte and hashed the way sim state is.
struct Command {
    /// The tick this was issued on, and the tick it must be applied on.
    ///
    /// Recorded rather than implied by position in a list, because a replay has to apply
    /// commands on the right tick even when a tick has none — and because a log concatenated
    /// from two sources must still sort.
    TickIndex tick = 0;

    /// Who issued it. Checked against the unit's army: a command from a player who does not
    /// command that army is REJECTED, deterministically, rather than trusted.
    PlayerIndex player = 0;

    CommandKind kind = CommandKind::Stop;

    /// Whether this input appends behind the current order. Serialized because replay must
    /// rebuild the same queue; once accepted it is provenance and does not affect execution.
    bool queued = false;

    /// Which unit. Stale handles are ignored — a player may click a unit that died on the
    /// tick their order was issued, and that must not resolve to whoever inherited the slot.
    UnitId unit{};

    /// Where, for the kinds that have a where. Ignored by `Stop`.
    ///
    /// For an `Attack` WITH a target, this is where the target was LAST ROUTED TO — the
    /// chase in `advanceOrders` re-routes when the target strays from it and records the
    /// new goal here, so the field is the chase's own memory rather than a stale click.
    Fx targetX{};
    Fx targetZ{};

    /// Who to attack, for `Attack` — or an invalid handle, which makes the attack a plain
    /// walk-at-a-place (the old semantics, and still what attack-ground means). A valid
    /// handle is what turns the order into a PURSUIT: the unit follows the target's real
    /// position, holds when its own longest weapon reaches, and the order completes when
    /// the target dies rather than when the unit arrives anywhere. Stale generations read
    /// as dead, which is exactly right — the thing clicked no longer exists.
    UnitId target{};

    /// What to build, for `Build`. Ignored by the rest.
    UnitTypeIndex buildType = 0;

};

/// One semantic issue action, possibly addressed to several units.
///
/// Selection order is presentation state, not simulation order. `applyCommand` therefore sorts
/// this set by the complete generational handle and removes duplicates before touching a unit.
/// This is also the command-log record. A recorded issue replaces `units` with the canonical
/// accepted set, preserving the issue boundary without retaining refused recipients.
struct CommandIssue {
    TickIndex tick = 0;
    CommandPhase phase = CommandPhase::PreTick;
    CommandSource source = kInvalidCommandSource;
    CommandId id = kInvalidCommandId;
    PlayerIndex player = 0;
    CommandKind kind = CommandKind::Stop;
    bool queued = false;
    std::vector<UnitId> units;
    Fx targetX{};
    Fx targetZ{};
    UnitId target{};
    UnitTypeIndex buildType = 0;
    std::uint32_t count = 1;
    std::string scriptTask;
    std::vector<std::uint8_t> scriptData;
    /// Used only by CancelFactoryBuild; this action never becomes a queued order.
    CommandId cancelCommandId = kInvalidCommandId;
    /// Used only by SetBuildPriority: the tier to set, not a cycle step.
    BuildPriority priority = BuildPriority::Normal;
};

/// Transport-only command intake. It is deliberately absent from the state hash.
///
/// A drain orders sources ascending and preserves submission FIFO within one source. Unit order
/// is canonicalized at submission, so every producer and replay records the same semantic set.
class CommandBuffer {
public:
    /// Queues one issue, allocating a source-local ID when it has none.
    [[nodiscard]] std::optional<CommandId> submit(CommandIssue issue, UnitStore& store);

    /// Removes and returns one tick/phase batch in deterministic application order.
    [[nodiscard]] std::vector<CommandIssue> take(TickIndex tick, CommandPhase phase);

    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }
    [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }
    /// What has been submitted and not yet taken, for a caller deciding whether to submit
    /// more — the app's standing orders must not override a player's click that is still here.
    [[nodiscard]] std::span<const CommandIssue> pending() const noexcept { return pending_; }

private:
    std::vector<CommandIssue> pending_;
};

/// Immutable intent shared by every queue entry created by one issue action.
///
/// `units` is the canonical accepted set. Individual requested members may be refused because
/// they are dead, unauthorised, unroutable, or over the queue cap; those refusals do not enter
/// authoritative state or split the accepted members into separately allocated commands.
/// Pointer identity is useful for exact cross-queue edits, but addresses and ownership counts
/// are never simulation data.
struct SharedCommand {
    TickIndex tick = 0;
    CommandSource source = kInvalidCommandSource;
    CommandId id = kInvalidCommandId;
    PlayerIndex player = 0;
    CommandKind kind = CommandKind::Stop;
    bool queued = false;
    std::vector<UnitId> units;
    Fx targetX{};
    Fx targetZ{};
    UnitId target{};
    UnitTypeIndex buildType = 0;
    CommandSerial creationSerial = 0;
    std::uint32_t originalCount = 1;
    std::uint32_t remainingCount = 1;
    std::string scriptTask;
    std::vector<std::uint8_t> scriptData;
};

inline constexpr std::size_t kMaxScriptTaskNameBytes = 256;
inline constexpr std::size_t kMaxScriptTaskDataBytes = 1024 * 1024;

/// Which members of a semantic issue were accepted, in canonical unit order.
struct ApplyCommandResult {
    std::vector<UnitId> accepted;

    [[nodiscard]] explicit operator bool() const noexcept { return !accepted.empty(); }
    [[nodiscard]] bool acceptedUnit(UnitId unit) const noexcept;
};

/// Resolves each recipient's movement domain. A heterogeneous selection may legitimately send
/// a tank and an aircraft through different grids, so a grouped issue cannot use one grid.
using CommandGridForUnit = std::function<const PassabilityGrid*(UnitId)>;

/// Whether two commands are the same order. For comparing a recorded log with a replayed one.
[[nodiscard]] bool operator==(const Command& a, const Command& b) noexcept;
[[nodiscard]] bool operator==(const CommandIssue& a, const CommandIssue& b) noexcept;

/// What an order is CALLED — the one spelling, shared by the command log and the console.
///
/// It was private to `Command.cpp`, where the log writer used it and nothing else could. The
/// app then had no way to name a kind, so the interface reported orders by not reporting them.
/// Two spellings of "attack-move" would be two things to keep in step; this is one.
[[nodiscard]] const char* commandKindName(CommandKind kind) noexcept;

/// Whether a structure footprint fits the terrain without overlapping a living ground unit or
/// another construction. This is the shared answer for the placement ghost and command
/// authority; mobile factory products and upgrades do not use map placement.
[[nodiscard]] bool buildSitePlaceable(const PassabilityGrid& grid, Fx x, Fx z, Fx radiusElmos,
                                       const UnitStore& store, const UnitCatalog& catalog,
                                       std::span<const Construction> building) noexcept;

/// Applies one command, and says whether it was applied.
///
/// **THE SINGLE PATH.** A human's click and a script's decision both arrive here, which is
/// what makes "they replay through the same path" true rather than aspirational.
///
/// Returns false — without changing anything — when the command names a unit that is not
/// alive, or a player that does not command it, or a kind this build cannot carry out. Those
/// are ORDINARY, not errors: a player clicks a dying unit, a stale log names a unit that no
/// longer exists. What matters is that the rejection is deterministic, so a replay rejects
/// exactly what the original did.
///
/// `Build` IS applied now (P3), and the gap this comment used to describe is closed. A mobile
/// builder first approaches its exact retail build range; only then is the `Construction` row
/// created. Factory products and upgrades remain immediate on the builder's own pad.
///
/// It could not be before, for a reason worth keeping: `Construction::blueprintIndex` meant "an
/// index into whatever list the caller is building from", so the sim had no way to name a
/// blueprint the caller would recognise. P3 unified the two registries — an index is a
/// `UnitTypeIndex` now, which the catalog also uses — so the sim can read a type's cost and
/// build time and push a `Construction` itself.
///
/// What is still the caller's: turning a FINISHED construction into a unit. That needs a model
/// out of the VFS, which the sim genuinely cannot reach, and `TickReport::finished` is how it
/// is handed over. That division is the real one; the old one was an accident of indexing.
///
/// `building` may be null — a decorative crowd has no construction list — in which case a
/// `Build` command is refused rather than crashing.
///
/// `Command::queued` is the shift key (§7 P4.1). Without it the order REPLACES the unit's queue
/// and is started at once; with it the order goes behind whatever is already there — or CANCELS
/// a matching one, except that repeatable mobile factory products are appended.
///
/// THE TWO ARE VALIDATED DIFFERENTLY, and the asymmetry is deliberate rather than an oversight.
/// With a `PathService`, a plain ground `Move` is accepted as path intent and routed on a later
/// beat by its army's FIFO service; an unreachable result is then dropped. The null-service seam
/// retains synchronous routing for callers that do not own a complete match yet. A queued order
/// cannot be validated at all: the unit will be somewhere else by the time it starts, so a route
/// computed now would be a route from the wrong place. It is checked when it is reached, and an
/// order that cannot be started then is dropped and the next one tried (`advanceOrders`). Recoil
/// validates queued orders no earlier either.
/// `features` is where a `Reclaim` resolves its target; null refuses the kind outright,
/// which is what a scene with nothing on the ground should do.
[[nodiscard]] bool applyCommand(const Command& command, UnitStore& store,
                                const UnitCatalog& catalog, std::span<const Player> players,
                                std::span<const Army> armies, const Terrain& terrain,
                                 const PassabilityGrid& grid, TickRate rate,
                                  std::vector<Construction>* building = nullptr,
                                   EventQueue* events = nullptr,
                                   const FeatureStore* features = nullptr,
                                   PathService* pathService = nullptr,
                                   ScriptTaskHost* scriptTasks = nullptr,
                                   std::vector<SiloAmmo>* siloAmmo = nullptr,
                                   std::vector<SiloBuild>* siloQueue = nullptr,
                                   std::vector<SelfDestructWork>* selfDestructs = nullptr);

/// Applies one semantic issue to a canonicalized unit set.
///
/// Each accepted unit receives one queue entry referring to the same immutable `SharedCommand`.
/// Mutable pursuit and temporary-target state remains local to that entry. A serial is consumed
/// once when the first member accepts the issue, not once per selected unit.
/// `gridForUnit` validates the command's site. For Build, `approachGridForUnit`
/// supplies the builder's movement domain; single-grid callers may omit it.
///
/// `siloAmmo`/`siloQueue` give the silo-build kinds (`SiloBuildTactical`, `SiloBuildNuke`,
/// `ToggleSiloAuto`) the state they act on — the records for the full check and the queue
/// the accepted build lands in. Either being null refuses those kinds outright, which is
/// what a scene with no silos should do.
[[nodiscard]] ApplyCommandResult applyCommand(
    const CommandIssue& issue, UnitStore& store, const UnitCatalog& catalog,
    std::span<const Player> players, std::span<const Army> armies, const Terrain& terrain,
    const CommandGridForUnit& gridForUnit, TickRate rate,
    std::vector<Construction>* building = nullptr, EventQueue* events = nullptr,
    const FeatureStore* features = nullptr, PathService* pathService = nullptr,
    ScriptTaskHost* scriptTasks = nullptr,
    const CommandGridForUnit& approachGridForUnit = {},
    std::vector<SiloAmmo>* siloAmmo = nullptr,
    std::vector<SiloBuild>* siloQueue = nullptr,
    std::vector<SelfDestructWork>* selfDestructs = nullptr);

/// Publishes a finished asynchronous plain-move route through the command authority.
///
/// A result is ignored when its unit died or a later order replaced its command identity while
/// the search was in flight. An empty result means the service's retries are spent: a
/// transportable unit is offered an embark instead, and anything else drops the now-unroutable
/// head through normal dispatch.
[[nodiscard]] bool publishPathResult(const PathResult& result, UnitStore& store,
                                     const UnitCatalog& catalog);

/// How close a builder must stand to a site to work on a product there, centre to centre:
/// its build reach plus its own footprint plus the product's skirt (`CUnitMobileBuildTask`).
[[nodiscard]] Fx constructionReach(const UnitCatalog& catalog, UnitTypeIndex builder,
                                   UnitTypeIndex product) noexcept;

/// Where a Build order's work stands: the builder's own pad for an upgrade or a factory's
/// mobile product, the ordered position for a placed structure. Nullopt when either
/// definition is missing.
[[nodiscard]] std::optional<std::pair<Fx, Fx>> buildSiteFor(
    UnitTypeIndex buildType, Fx targetX, Fx targetZ, UnitIndex slot,
    const UnitStore& store, const UnitCatalog& catalog) noexcept;

/// A construction of `blueprint` at exactly this site, by anyone — the colleague an
/// arriving builder finds when another claimed the same deposit first, or the scaffold a
/// returning founder abandoned and was sent back to.
[[nodiscard]] Construction* constructionAtSite(std::vector<Construction>& building,
                                               UnitTypeIndex blueprint, Fx x,
                                               Fx z) noexcept;

/// Whether `work`'s builder is still building it — alive, with its queue's active head
/// the Build order this row stands for, or a guarding factory whose retained entry it
/// mirrors. A row nobody works is ORPHANED: it keeps its place and its progress, draws
/// nothing, and waits for a builder ordered onto the site to take it over.
[[nodiscard]] bool constructionWorkedOn(const Construction& work, const UnitStore& store,
                                        const UnitCatalog& catalog) noexcept;

/// Whether `slot`'s army is on the side that pays for `work` — the join/adopt check for a
/// construction already on the map, which works on a dead founder's row because the row
/// remembers its army even when its builder is gone. No alliance state (a bare test
/// store) counts as allied, the way `alliedBuilder` already reads it.
[[nodiscard]] bool constructionArmyAllied(const Construction& work, UnitIndex slot,
                                          const UnitStore& store,
                                          std::span<const Army> armies) noexcept;

/// C-183 eligibility shared by the construction prepass and guard dispatch.
[[nodiscard]] bool guardAllowsBuildAssistance(UnitIndex slot, const UnitStore& store,
    const UnitCatalog& catalog, std::span<const Construction> building,
    std::span<const Army> armies, const Intel* intel, const PlayableRect* playableRect,
    TickIndex tick = 0, TickRate rate = TickRate{});

/// Starts the next order for every unit that has finished its current one.
///
/// Returns how many orders were started, which is what a test asserts on and a caller reports.
///
/// NO `players` ARGUMENT, and that is the point: authorisation happened when the order was
/// given. A queue holds orders that were already allowed, so starting one asks about the world
/// and not about who is at the keyboard — and a player who leaves mid-match does not strand a
/// unit halfway along a route it was legitimately sent on.
///
/// COMPLETION IS DEFINED HERE because only the caller of the passes can know it. A `Move` or
/// `Attack` is finished when the unit has stopped moving; `Stop` finishes immediately, while a
/// `Build` remains current while its mobile builder approaches and until its construction
/// completes. An order that
/// cannot be started — a route that no longer exists, a target that died — is dropped and the
/// next one tried in the same tick, so a dead waypoint does not stall a route.
///
/// `gridForType` is indexed by `UnitTypeIndex` and may hold nulls: passability is a property of
/// the motion class (P3.4), so a hover tank and a bot route on different grids and a unit whose
/// grid is missing is left alone rather than routed on somebody else's.
///
/// CONSTRUCTION IS ADVANCED HERE, and that is where retail advances it: a build task
/// materialises its target and retires its own order inside the command-dispatch stage
/// (`C-142`, `C-188`), not out of an end-of-beat economy write-back. Doing it anywhere else
/// made construction a second queue-head mutation site, which is the residue `C-112` had left
/// open. The economy pass at the end of the beat still owns the BILL and the ratio the next
/// beat's progress is multiplied by (`Economy::advanceConstruction`).
///
/// `finished` collects the work that completed this tick, in builder-slot order, so the caller
/// can put the new units on the map. Null when a caller does not care.
std::size_t advanceOrders(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                           std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                           std::vector<Construction>* building = nullptr,
                           EventQueue* events = nullptr,
                            const FeatureStore* features = nullptr,
                            std::vector<Construction>* finished = nullptr,
                              PathService* pathService = nullptr,
                              std::span<const Army> armies = {},
                              const Intel* intel = nullptr,
                              const PlayableRect* playableRect = nullptr,
                              ScriptTaskHost* scriptTasks = nullptr,
                              std::vector<GuardWork>* guardWork = nullptr,
                              RandomStream* random = nullptr, TickIndex tick = 0,
                              std::span<const PassabilityGrid* const> gridForTypeSubmerged = {});

/// Updates attack-move and patrol combat after movement and intel. These orders retain their
/// waypoint while `target` temporarily names the visible hostile that interrupted the route.
void updateAggressiveOrders(UnitStore& store, const UnitCatalog& catalog,
                             std::span<const Army> armies, const Terrain& terrain,
                             std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                             const Intel* intel = nullptr,
                             const PlayableRect* playableRect = nullptr,
                             TickIndex tick = 0,
                             std::span<const PassabilityGrid* const> gridForTypeSubmerged = {});

// --- The log --------------------------------------------------------------------------
//
// Every command a match was given, in the order it was given. With the initial state, this IS
// the match: §1.3's criterion in one file.

/// Semantic issues, in tick/phase order.
///
/// A flat vector rather than a map from tick to commands: a match issues a few hundred orders
/// over ten minutes, so the whole log fits in a cache line's worth of pages, and a flat array
/// keeps the on-disk form and the in-memory form the same shape. `at()` is a binary search,
/// which for these sizes is faster than a hash and exactly reproducible.
class CommandLog {
public:
    /// Appends one issue with its canonical accepted unit set. Returns false for malformed or
    /// backwards input rather than silently producing an unreplayable artifact.
    bool record(CommandIssue issue);

    /// The issues issued in one explicit tick phase, in deterministic intake order.
    [[nodiscard]] std::span<const CommandIssue> at(TickIndex tick,
                                                   CommandPhase phase) const noexcept;

    [[nodiscard]] std::span<const CommandIssue> all() const noexcept { return issues_; }
    [[nodiscard]] std::size_t size() const noexcept { return issues_.size(); }
    [[nodiscard]] bool empty() const noexcept { return issues_.empty(); }

    /// The last tick with a command on it, or zero for an empty log. What a replay runs to.
    [[nodiscard]] TickIndex lastTick() const noexcept;

private:
    std::vector<CommandIssue> issues_;
};

/// Writes a strict, versioned text log, one semantic issue per line.
///
/// TEXT, for the same reason the hash log is text: a log you can read is a log you can
/// diff, quote in a bug report and hand-edit to reproduce something. The format is
/// Fixed-point targets are written as their RAW integers — a decimal expansion would be a lossy
/// round trip, which in a determinism artifact is the one unacceptable kind of lossy.
/// `pathFor` resolves a build's type index into its blueprint path for the log's last
/// final quoted column, or empty. Written because a TYPE INDEX is this run's private numbering — the
/// original replay attempt refused every type it had never registered — while a path names
/// content any run can resolve.
bool writeCommandLog(const CommandLog& log, const std::string& path,
                     const std::function<std::string(std::uint32_t)>& pathFor = nullptr);

/// Reads one back. Returns nothing when the file cannot be read or a line will not parse:
/// a partial log is worse than none, because it replays as a different match.
/// Legacy unversioned logs are rejected because they cannot express grouping, phase, source,
/// identity, or repeat count without guessing. `buildPaths`, when given, receives one entry per
/// issue — the blueprint path for a build, empty otherwise — index-aligned with the log.
[[nodiscard]] std::optional<CommandLog> readCommandLog(
    const std::string& path, std::vector<std::string>* buildPaths = nullptr);

} // namespace rm::sim
