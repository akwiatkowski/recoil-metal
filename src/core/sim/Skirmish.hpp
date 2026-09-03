#pragma once

#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/Intel.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

/// Implemented C-210 skirmish defeat predicates.
enum class VictoryMode : std::uint8_t {
    Assassination,
    Supremacy,
};

// One tick of a whole match, in one place.
//
// WHY THIS FILE EXISTS. Every rule a match is made of was already here and already
// tested — movement, collisions, aiming, firing, projectiles, damage, death, defeat,
// economy. What was NOT here was the order they run in. That lived in an anonymous
// namespace inside `main.mm`, which is an executable-only translation unit, so no test
// could link it and there was nothing stopping a second caller from running a different
// subset of it. One did: the windowed frame loop ticked movement and collisions and
// nothing else, so a unit in the interactive game moved perfectly and never fired a
// shot, and the whole test suite stayed green.
//
// So the assembly is the thing being moved, not the rules. Both callers — the headless
// `--march` pre-run and the frame loop — call `tickSkirmish` now, and the order of the
// passes is a fact about the sim rather than about whichever caller you are reading.
//
// WHAT IS DELIBERATELY NOT HERE. Anything that needs an asset. A finished construction
// becomes a unit on the map, which means loading a model out of the VFS, and a death
// leaves a scorch mark, which means a decal buffer. Both are the caller's work: this
// reports what happened and the caller decides what to load. That keeps the sim free of
// the VFS, which is what lets a test build a match out of two structs and a flat field.

// `SkirmishGroup` used to live here: one batch's mutable spans plus the one definition its
// whole batch shared. The passes take `UnitStore` and `UnitCatalog` now — see PLAN2.md §7
// P1.4 for why the grouping existed and what removing it changed.
/// The match-wide state one tick advances, alongside the per-batch groups.
///
/// References rather than values: a tick mutates all of it, and a Match is built fresh
/// at each call site from storage that outlives it.
struct Match {
    std::vector<Army>& armies;

    /// One economy per army, indexed by army. Empty outside a skirmish, which is what a
    /// `--units` crowd is — the economy passes then do nothing rather than being skipped
    /// by a flag.
    std::span<Economy> economies;

    std::vector<Projectile>* projectiles = nullptr;

    /// Everything under construction, all armies together. Partitioned per army inside
    /// the tick, because `tickEconomy` is documented to be given one army's work and
    /// charging the wrong one is a caller's mistake to avoid.
    std::vector<Construction>* building = nullptr;

    /// CAiSiloBuildImpl-shaped state, separate from UnitStore (`C-081`).
    std::vector<SiloAmmo>* siloAmmo = nullptr;

    /// What is on the ground that is not a unit — wrecks (§7 P6.2). Null for a scene with
    /// nothing to leave behind.
    ///
    /// The sim ADDS to it; the renderer reads it and projects decals. What used to happen is
    /// that a death appended GPU vertices to the app's scene state, so the record of what died
    /// here was a triangle list. Same reasoning as `projectiles`: caller-owned storage, sim-side
    /// authorship.
    FeatureStore* features = nullptr;

    /// Where the tick reports what happened, or null (§7 P6.1).
    ///
    /// CALLER-OWNED, APPEND-ONLY FROM HERE, and **the caller clears it**. The tick used to clear
    /// it at the top, which was wrong for a reason that took a manual check to see: the caller
    /// raises events of its own — `UnitCreated` when it spawns, `ConstructionStarted` when its
    /// scripted opponent orders a build — and some of those happen BEFORE the tick. Clearing
    /// inside the tick threw those away, so two event kinds were declared, emitted, and never
    /// once observable. A queue's lifetime belongs to whoever knows where the frame boundary is,
    /// and that is not the sim.
    ///
    /// Null for a scene with nothing listening — a `--units` crowd, a pathfinding harness, most
    /// tests — in which case every emit is one branch.
    EventQueue* events = nullptr;

    /// The grid each unit TYPE routes on, indexed by `UnitTypeIndex`. Entries may be null.
    ///
    /// PER TYPE, not one for the match, because P3.4 made passability a property of the motion
    /// class: an Aeon hover tank crosses water a Cybran bot walks around, and a queued route
    /// computed on somebody else's grid would send one of them into the sea. The caller already
    /// keys its grids on the limits, so this is a lookup table it can fill in a loop over the
    /// catalog.
    ///
    /// HERE RATHER THAN IN `tickSkirmish`'s SIGNATURE because it belongs to the match: the
    /// grids are built from the map the match is played on. Empty means queued orders are not
    /// advanced at all, which is exactly right for a `--units` crowd that cannot have any.
    std::span<const PassabilityGrid* const> passability;

    /// Optional match-owned path service. Null preserves the synchronous compatibility seam
    /// for callers that do not model a full match yet.
    PathService* pathService = nullptr;

    /// How many commanders each army STARTED with, indexed by army.
    ///
    /// The win condition needs it to tell "lost its commander" from "never had one": a
    /// decorative crowd has no commanders and must not be declared a draw on tick one.
    std::span<const int> commandersEver;

    /// The Lua victory.lua category predicate selected for this skirmish.
    VictoryMode victoryMode = VictoryMode::Assassination;

    /// The storage cap every army gets before anything it has built adds to it.
    ///
    /// Passed in rather than fixed here because it is the CALLER's starting condition —
    /// a scenario could hand out a different one — and a constant in the sim would make
    /// that a code change.
    Resources baseStorage{};

    /// What each alliance can see (ADR-037), or null for a scene with no fog of war.
    ///
    /// CALLER-OWNED like the rest of this struct, and null is a real configuration rather
    /// than an oversight: a `--units` crowd has no alliances to keep grids for, and every
    /// visibility query answers "seen" — which is the engine every scene predating this
    /// was written against.
    Intel* intel = nullptr;

    /// Immutable scenario boundary for automatic unit acquisition. Owning the optional value
    /// makes the boundary part of deterministic match configuration rather than a borrowed
    /// address into caller setup. An empty optional preserves unrestricted scenes.
    std::optional<PlayableRect> playableRect;

    /// Set once the match has been decided, so the result is announced once rather than
    /// every tick for the rest of the run.
    bool over = false;

    /// C-210: a terminal winner (or draw) must remain unchanged for fifteen seconds before
    /// it becomes GameOver. This is match state, rather than caller state, so every tick path
    /// confirms the same candidate.
    bool winnerPending = false;
    std::optional<int> pendingWinner;
    TickCount winnerStableTicks = 0;

    /// C-210: commander defeat is sampled every three seconds, not every simulation tick.
    TickCount defeatPollElapsedTicks = 0;

    /// C-210: OnDefeat clears an army's non-wall units twenty seconds later. Entries are
    /// indexed like `armies`; zero means that army has no cleanup pending.
    std::vector<TickCount> defeatCleanupRemainingTicks;
};

/// A unit's death, with everything the caller needs to mark it.
///
/// The position and radius are CARRIED rather than looked up, because retiring a unit is
/// what zeroes its radius and collapses its mesh — a caller that went back to the slot
/// afterwards would find a scorch mark of size zero at a position the sim had written
/// off. What the wreck looks like has to be sampled before the unit stops existing.
struct Death {
    /// The unit that died. Stale by the time a caller sees it — `retireDead` kills the handle
    /// once the report is built — so it is a NAME for the corpse rather than a way back to a
    /// live unit. Its slot (`ref.index`) still indexes the arrays, which is what lets the
    /// death explosion read the def and the owner of something that no longer exists.
    UnitId ref;
    std::array<Fx, 3> at{};

    /// The collision radius it had while alive, which is what sizes its wreck: a
    /// commander marks more ground than a tank.
    Fx radiusElmos{};
};

/// What one tick did, for a caller that has to react to it.
///
/// Returned rather than written into the match because these are EVENTS, and the two
/// callers react differently: the pre-run prints them, the frame loop draws them.
struct TickReport {
    std::size_t shotsFired = 0;

    /// Queued orders started this tick — a unit reaching the next waypoint of a shift-queued
    /// route, or a builder taking the next item off its list.
    ///
    /// Reported rather than kept quiet because it is the only outward sign that a queue is
    /// draining: a route that stalls looks exactly like a unit that has arrived, and this is
    /// what tells the two apart.
    std::size_t ordersStarted = 0;

    /// Who died this tick, reported exactly once each.
    ///
    /// Once, because the caller leaves a permanent scorch mark per entry, and a corpse
    /// repeated every tick would scorch the same ground forever.
    std::vector<Death> died;

    /// Death explosions set off, and the damage they dealt.
    ///
    /// BOTH, because they answer different questions: a blast that goes off and hurts
    /// nothing is the ordinary case when two commanders kill each other in the same
    /// tick, and reporting only the damage would make that look like the explosions
    /// never happened.
    std::size_t deathBlasts = 0;
    Mag deathBlastDamage{};

    /// How many armies were newly defeated this tick.
    std::size_t defeated = 0;

    /// Whether this tick ended the match, true on the ONE tick that decided it.
    bool matchEnded = false;

    /// The winning team, when the match ended with one. Empty on a draw — every army
    /// losing its commander at once is a legitimate outcome, not an error.
    std::optional<int> winner;

    /// Constructions that completed this tick, for a caller that can turn one into a
    /// unit on the map. The sim cannot: that needs a model out of the VFS.
    ///
    /// NEWLY completed, and the finished work stays in `Match::building` rather than
    /// being taken out of it. That is deliberate: a finished construction is what marks
    /// its ground as spoken for, and an opponent that scans the list for a free mass
    /// deposit would put a second extractor on top of the first the moment the first
    /// was removed.
    std::vector<Construction> finished;
};

/// Advances the whole match by one fixed tick.
///
/// THE ORDER IS THE DESIGN, and it is stated here because it is what used to be lost:
///
///  1. Movement, then collisions. A unit shoots from where it has GOT to this tick.
///  2. Aiming, then firing. An unturreted weapon may only shoot along its hull, so a
///     unit that stopped facing the wrong way is brought round first — otherwise the
///     facing gate reads as a weapon that simply does not work.
///  3. Projectiles, then the dead. A shot that lands this tick kills this tick.
///  4. Defeats and the win condition, every tick rather than at the end: an army that
///     loses its commander stops being a target from that moment, which is what
///     `hostile()` already reads, so a late check leaves a dead side fighting on.
///  5. Income recomputed from what is STANDING, then the economy charged. A structure
///     that died in step 3 takes its production with it in step 5, in the same tick.
///
/// Firing before moving would let a unit shoot from outside a range it is about to
/// enter; checking defeat before the dead are retired would miss the commander that
/// died this tick.
/// The rate is a parameter, defaulted so that the many callers who want the ordinary clock
/// need not say so. Defaulted rather than absent because a `TickRate` is cheap to construct
/// and the alternative — reading a constant inside — is the thing §5.1 forbids.
TickReport tickSkirmish(UnitStore& store, const UnitCatalog& catalog, Match& match,
                         const Terrain& terrain, TickRate rate = TickRate{}, TickIndex tick = 0);

/// Living commanders per army, indexed by army.
///
/// Exposed rather than hidden inside the tick because the caller needs the same count at
/// SETUP to fill `Match::commandersEver`, and two ways of counting the same thing is how
/// "never had one" and "lost it" get confused.
[[nodiscard]] std::vector<int> countCommanders(const UnitStore& store,
                                               const UnitCatalog& catalog,
                                               std::size_t armyCount);

} // namespace rm::sim
