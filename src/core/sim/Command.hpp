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
};

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
                                   ScriptTaskHost* scriptTasks = nullptr);

/// Applies one semantic issue to a canonicalized unit set.
///
/// Each accepted unit receives one queue entry referring to the same immutable `SharedCommand`.
/// Mutable pursuit and temporary-target state remains local to that entry. A serial is consumed
/// once when the first member accepts the issue, not once per selected unit.
[[nodiscard]] ApplyCommandResult applyCommand(
    const CommandIssue& issue, UnitStore& store, const UnitCatalog& catalog,
    std::span<const Player> players, std::span<const Army> armies, const Terrain& terrain,
    const CommandGridForUnit& gridForUnit, TickRate rate,
    std::vector<Construction>* building = nullptr, EventQueue* events = nullptr,
    const FeatureStore* features = nullptr, PathService* pathService = nullptr,
    ScriptTaskHost* scriptTasks = nullptr);

/// Publishes a finished asynchronous plain-move route through the command authority.
///
/// A result is ignored when its unit died or a later order replaced its command identity while
/// the search was in flight. An empty result remains unpublished so normal dispatch drops the
/// now-unroutable head.
[[nodiscard]] bool publishPathResult(const PathResult& result, UnitStore& store);

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
                              ScriptTaskHost* scriptTasks = nullptr);

/// Updates attack-move and patrol combat after movement and intel. These orders retain their
/// waypoint while `target` temporarily names the visible hostile that interrupted the route.
void updateAggressiveOrders(UnitStore& store, const UnitCatalog& catalog,
                             std::span<const Army> armies, const Terrain& terrain,
                             std::span<const PassabilityGrid* const> gridForType, TickRate rate,
                             const Intel* intel = nullptr,
                             const PlayableRect* playableRect = nullptr);

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
