#pragma once

// Running a match: the scripted opponents, the tick the callers share, and the headless pre-run.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5, and this is the layer §10's warning was really
// about — "the spawn logic, the extractor ordering, the passability cache keying — those encode
// decisions that took work". They are here, and they came across with their comments intact
// because the reasoning is not recoverable from the code.
//
// WHAT IS AND IS NOT HERE. `advanceMatch` is the CALLER's side of a tick: it paces the
// opponents, calls `sim::tickSkirmish`, and does the work the sim reports back — spawning what
// finished, marking what died, publishing the snapshot. The tick ORDER is not here; that is
// `core/sim/Skirmish.cpp`, and `tools/check_sim_boundary.sh` fails the build if this file ever
// starts calling the passes directly. That check is the reason this split is safe to make: the
// one thing that must not drift between two callers cannot.

#include "app/Cli.hpp"
#include "app/FafAi.hpp"
#include "app/Opponent.hpp"
#include "app/SceneBuild.hpp"

#include "core/map/ScenarioSave.hpp"
#include "core/scene/Particles.hpp"
#include "core/sim/BuildOrder.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/Skirmish.hpp"

#include <array>
#include <memory>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace rm::app {

/// What one army has ON THE MAP, gathered for the scripted opponent's decisions —
/// and for the economy, which recomputes income from this rather than accumulating
/// it, so a structure that dies takes its production with it.
struct Standing {
    bool commanderAlive = false;
    std::array<rm::sim::Fx, 3> commanderPosition{};
    float commanderBuildRate = 0.0f;

    /// WHICH commander, not just where it is (§7 P6.1). A build order is issued BY a unit, and
    /// the sim needs the builder's handle to check it can build, charge the right army and name
    /// the instigator in a `ConstructionStarted` event.
    rm::sim::UnitId commander{};

    std::size_t extractors = 0;
    std::size_t powerGenerators = 0;
    std::size_t factories = 0;
    std::array<rm::sim::Fx, 3> factoryPosition{};
    float factoryBuildRate = 0.0f;

    /// The factory that builds this army's units, for the same reason.
    rm::sim::UnitId factory{};

    std::vector<rm::sim::UnitId> tanks;
};

/// Orders every unit in the scene to a point, and runs the sim for a while.
///
/// `dust` collects the particles the march raised, so that a headless capture can
/// show a trail. Emitted DURING the ticks rather than at the end, which is the only
/// way to get one: dust marks where a unit has been, and a scene sampled after the
/// walk knows only where everything ended up.
// Everything one tick of a match is, from the CALLER's side.
//
// WHY THIS EXISTS. `rm::sim::tickSkirmish` owns the order the sim's own passes run in.
// This owns the order the caller's work runs in around them: the opponents decide before
// the tick, and afterwards come the two things the sim cannot do for itself — mark a
// wreck, and turn a finished construction into a unit on the map, which needs a model out
// of the VFS.
//
// It is ONE type used by both callers — the headless `--march`/`--play` pre-run and the
// windowed frame loop — because the alternative was two hand-rolled loops and they
// diverged. The windowed one ticked movement and collisions only, so a unit in the
// interactive game moved perfectly and never fired a shot, and the whole test suite stayed
// green because every rule was tested and the ASSEMBLY was not. Skirmish.cpp fixed that for
// the sim's passes; this fixes it for the caller's.
//
// What deliberately stays with each caller, because it is presentation rather than rules:
// PRINTING (the pre-run narrates the match, the frame loop draws it) and PARTICLE timing
// (the pre-run ages dust by the fixed tick, the frame loop by the frame it just drew).
// Neither changes what the match does.
//
// References rather than values, the same shape and for the same reason as
// `rm::sim::Match`: a runner is built at a call site from storage that outlives it.
struct MatchRunner {
    UnitScene& scene;
    const rm::HeightField& field;
    PassabilitySet& passability;
    const rm::vfs::Vfs& content;
    std::span<const rm::mapinfo::StartPosition> starts;
    std::span<const rm::scenario::Marker> markers;

    /// One opponent per army, behind the port (ADR-038); a human player's slot holds a
    /// `ScriptedOpponent` that is simply never asked to think.
    std::vector<std::unique_ptr<rm::ai::Opponent>> scripts;

    /// A recorded command log to play back instead of letting anything think — §1.3's
    /// "same log, same match". When set, the caller has emptied `scripts` and the spawn
    /// roll-off stands down too: every one of their orders is already IN the log, and a
    /// path that regenerated them would issue each twice. Points at storage the caller
    /// owns for the run.
    const rm::sim::CommandLog* replay = nullptr;

    /// The log's blueprint-path column, index-aligned with `replay`'s commands. A build's
    /// recorded type index was the ORIGINAL run's private numbering; the path is what this
    /// run resolves into its own catalog before applying.
    const std::vector<std::string>* replayPaths = nullptr;

    /// The FAF sandbox, when `--ai-faf` seats FAF opponents — one VM shared by all of them
    /// (FafAi's one-per-match rule), owned here because the opponents hold references into
    /// it and the runner is what outlives them. Null on the scripted path.
    std::unique_ptr<rm::ai::FafAi> fafSandbox;

    /// Built once and kept, because `over` has to survive between ticks — a match is
    /// decided on one tick and stays decided.
    rm::sim::Match match;

    /// The grid each unit TYPE routes on, indexed by `UnitTypeIndex`, for the sim's own
    /// advancing of queued orders (§7 P4.1).
    ///
    /// KEPT HERE rather than in the scene because it is a cache of pointers into
    /// `PassabilitySet`, which is the runner's. Refilled at the top of every tick: the catalog
    /// grows when a construction finishes and introduces a type nothing had spawned yet, and a
    /// table built once at the start would have a hole exactly where the new unit is.
    std::vector<const rm::sim::PassabilityGrid*> gridForType;

    /// Running totals, for the callers that report them at the end.
    std::size_t shotsFired = 0;
    std::size_t completedBuilds = 0;

    /// Units destroyed, accumulated from the tick's death reports.
    ///
    /// NOT counted by scanning the store at the end, which is what this used to do and what
    /// the flat store quietly broke: a corpse's slot is reused by the next spawn, so a scan
    /// for "slots whose health is zero" undercounts every death whose slot got recycled. The
    /// golden match reported 21 that way against 24 scorch marks — and the marks were right,
    /// because they are accumulated from the same reports as this. A total that only ever
    /// goes up cannot be undone by storage reusing a slot.
    std::size_t unitsDestroyed = 0;
    bool matchOver = false;
};

/// Advances the match by one fixed tick, and does the work the sim reports back.
///
/// `tickIndex` paces the opponents' decisions; `now` is passed through to them for their
/// own logging and reaches nothing in the sim, which counts in ticks and not in seconds.
/// `--print-events`: dump every event the sim raises, as it raises it.
///
/// §7 P6.1's stated manual check, and the cheapest possible consumer of the queue — which is the
/// point of it being the manual check. If this reads clearly, the seam is the right shape; if it
/// needs to reach back into the store to make sense of an event, the event is missing a field.
///
/// A file-scope flag rather than a parameter threaded through `advanceMatch`, for the same
/// reason `gAppTickRate` is one and with the same accounting: it is read once per tick in the
/// app, it changes nothing in the sim, and P7.5 moves this code out of `main.mm` entirely.
extern bool gPrintEvents;

/// `--ai-faf`: seat FAF opponents (app/FafOpponent.hpp) instead of the scripted ones, for
/// every non-player army. A file-scope flag for gPrintEvents' reasons, plus one of its own:
/// the seating happens inside `makeMatchRunner`, which both the headless pre-run and the
/// windowed loop call, and threading a parameter through both call chains would touch every
/// caller to carry one bit that changes no signature's meaning.
extern bool gFafOpponents;

/// `--ai-log`: narrate the FAF opponents — every decision with the corpus builder that
/// fired it, and the corpus's own LOG/WARN lines (normally counted and discarded). The
/// analysis view of a match: `starts`/`completes` say what happened, this says why.
extern bool gFafLog;

// --- What the match layer does ------------------------------------------------------------

[[nodiscard]] bool issueMove(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                              rm::PlayerIndex player, rm::TickIndex tick, rm::sim::Fx toX,
                              rm::sim::Fx toZ, bool queued = false,
                              rm::sim::CommandKind kind = rm::sim::CommandKind::Move,
                              rm::sim::CommandPhase phase = rm::sim::CommandPhase::PreTick);
[[nodiscard]] bool issueMove(UnitScene& scene, rm::sim::UnitId unit, rm::PlayerIndex player,
                             rm::TickIndex tick, rm::sim::Fx toX, rm::sim::Fx toZ,
                             bool queued = false,
                             rm::sim::CommandKind kind = rm::sim::CommandKind::Move,
                             rm::sim::CommandPhase phase = rm::sim::CommandPhase::PreTick);

/// issueMove's sibling for a TARGETED attack: the handle turns the order into a pursuit —
/// the unit follows the target's real position, holds at its own longest weapon's reach,
/// and the order completes when the target dies. toX/toZ are where the target is right now.
[[nodiscard]] bool issueAttack(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::sim::UnitId target, rm::sim::Fx toX, rm::sim::Fx toZ,
                                bool queued = false);

/// issueAttack with the commander's manual weapon: walk into ITS range, wait for the
/// energy, fire once, done. Refused for a unit with no manual weapon.
[[nodiscard]] bool issueOvercharge(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                    rm::PlayerIndex player, rm::TickIndex tick,
                                    rm::sim::UnitId target, rm::sim::Fx toX, rm::sim::Fx toZ,
                                    bool queued = false);

/// The guard order: `unit` lends its BuildRate to whatever `target` is building, standing
/// at build reach and following it (`core/sim/Assist.hpp`). Refused unless both units are
/// distinct builders in the same army, exactly as the sim refuses it.
[[nodiscard]] bool issueAssist(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::sim::UnitId target, bool queued = false);

/// issueAttack's sibling for a wreck: the FEATURE handle rides in `target`, the builder
/// walks into reach and the harvest drains it (`core/sim/Reclaim.hpp`). Refused for a
/// non-builder and for a wreck with nothing in it, exactly as the sim refuses them.
[[nodiscard]] bool issueReclaim(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                 rm::PlayerIndex player, rm::TickIndex tick,
                                 rm::sim::FeatureId wreck, bool queued = false);

[[nodiscard]] rm::PlayerIndex playerDriving(const UnitScene& scene, int army);

[[nodiscard]] Standing standingFor(const UnitScene& scene, int army);

[[nodiscard]] std::optional<std::array<rm::sim::Fx, 3>> nearestEnemyCommander(
    const UnitScene& scene, int army, const std::array<rm::sim::Fx, 3>& from);

[[nodiscard]] const rm::scenario::Marker* nearestFreeDeposit(
    const UnitScene& scene, std::span<const rm::scenario::Marker> markers,
    const std::array<rm::sim::Fx, 3>& from);

/// Drives every opponent through the port (ADR-038): each one observes, thinks, and hands
/// back decisions that this applies. Which implementation plays which army is settled at match
/// setup, so this loop never asks what kind of opponent it is holding.
void runOpponents(UnitScene& scene, const rm::vfs::Vfs& content, const rm::HeightField& field,
                   std::span<const rm::mapinfo::StartPosition> starts,
                  std::span<const rm::scenario::Marker> markers,
                  std::vector<std::unique_ptr<rm::ai::Opponent>>& scripts, float elapsedSeconds,
                  rm::TickIndex tickIndex);

[[nodiscard]] MatchRunner makeMatchRunner(UnitScene& scene, const rm::HeightField& field,
                                          PassabilitySet& passability,
                                          const rm::vfs::Vfs& content,
                                          std::span<const rm::mapinfo::StartPosition> starts,
                                          std::span<const rm::scenario::Marker> markers);

void printEvents(const rm::sim::EventQueue& events, float now);

rm::sim::TickReport advanceMatch(MatchRunner& runner, int tickIndex, float now);

void march(UnitScene& scene, const rm::HeightField& field, PassabilitySet& passability,
           const MarchOptions& options, std::span<const rm::AmbientEmitter> ambient,
           std::vector<rm::Particle>& dust, const rm::vfs::Vfs& content,
           std::span<const rm::mapinfo::StartPosition> starts,
           std::span<const rm::scenario::Marker> markers);

} // namespace rm::app
