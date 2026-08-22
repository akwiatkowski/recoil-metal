#pragma once

// Everything in one scene: the units, their definitions, their batches, and the projection that
// turns a tick into instances.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5. This is the layer above `app/Content.hpp` — it
// knows what a unit is and what an army is, and it still knows nothing about a window.
//
// §10 WARNED ABOUT THIS ONE: "`main.mm` holds real knowledge, not just mess — the spawn logic,
// the extractor ordering, the passability cache keying — those encode decisions that took work.
// P7.5 moves them; the move has to be read rather than mechanical." So the comments came with
// the code unchanged, and where the move itself needed a decision (the two globals below) it is
// recorded here rather than left as a diff.

#include "app/Content.hpp"

#include "core/data/Opening.hpp"
#include "core/data/Roster.hpp"
#include "core/scene/GroundDecals.hpp"
#include "core/scene/Selection.hpp"
#include "core/scene/UnitBatch.hpp"
#include "core/scene/UnitDraw.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/Snapshot.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitDef.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rm::app {

// THE TWO FILE-SCOPE MUTABLES, and moving them made them `extern` rather than removing them.
//
// That is a real decision and §7 P2.3 already argued it: 21 sites convert a content rate
// through `gAppTickRate`, and threading a parameter to all of them is work this phase throws
// away — the sim itself holds no such thing (`check_no_sim_globals.sh` enforces that), which is
// what lets the four-rate test run four rates in one process. They are the app's, they are set
// once from argv before anything reads them, and they are declared here so that the files that
// read them do so through a header rather than by luck.

/// The rate this run of the app steps its sim at, from `--tick-rate`.
///
/// **A MUTABLE FILE-SCOPE VALUE, AND THAT IS A COMPROMISE.** It is stated plainly rather than
/// hidden: `core/sim` may hold no such thing (PLAN2 §5.4, enforced by
/// `tools/check_no_sim_globals.sh`) and does not — a `TickRate` is a value passed to the passes
/// that need it, which is what makes the multi-rate test able to run four rates in one process.
/// This is the APP, which the check does not cover.
///
/// Why it is here anyway: twenty-one sites in this file convert a content rate, and threading a
/// parameter to all of them would be work thrown away when P7.5 moves the spawn logic, the
/// scripted opponent and the draw gather out of `main.mm` altogether. At that point each of
/// them takes the rate from whatever owns it and this disappears.
///
/// Set ONCE, by `setAppTickRate`, before anything reads it. Not a `const` because a
/// command-line flag has to be able to change it; not a settable-at-any-time knob because a
/// match whose rate changed halfway would be incoherent.
extern rm::sim::TickRate gAppTickRate;

/// Whether the renderer blends the last two snapshots (§7 P7.2). `--no-interpolate` clears it.
///
/// File-scope for the same reason `gAppTickRate` is, and with the same accounting: it is read
/// once per scene build, it changes nothing in the sim, and P7.5 moves this code out.
extern bool gInterpolate;

// Everything the renderer needs to draw units, owned in one place.
//
// models and instances are held in deques, NOT vectors: the batches point into
// them, and a vector that reallocates while later models load would leave every
// batch built so far dangling. A deque never moves what it already holds.
struct UnitScene {
    std::deque<rm::Model> models;
    std::deque<rm::sca::Animation> animations;
    std::vector<rm::UnitBatch> batches;
    TextureRegistry textures;

    // EVERY UNIT, flat, addressed by handle. Was three parallel deques of per-batch
    // vectors — `instances[batch][i]`, `motion[batch][i]`, `health[batch][i]` — which made
    // a unit's identity its position in a draw call (PLAN2.md §1.1).
    rm::sim::UnitStore store;

    // What each unit TYPE is. A batch is exactly one unit type, so a type index and a batch
    // index are THE SAME NUMBER and deliberately so: it keeps the render batching and the
    // sim's type numbering in step without a mapping table, and `batches[type]` is how a
    // unit reaches its model.
    rm::sim::UnitCatalog catalog;

    // Per TYPE now rather than per batch — the same numbers, since the two indices coincide.
    // Used to pick which passability grid routes a unit: a slope one climbs is a wall to
    // another.
    std::vector<float> maxSlopeDegrees;
    std::vector<float> maxWaterDepthElmos;

    /// How much to scale each type's mesh by, from its blueprint's `meshToElmos`.
    ///
    /// Per TYPE, and here rather than in the store, because a scale is presentation: the sim
    /// has a collision radius and does not care how big the model that represents it is. It
    /// moved out of `UnitInstance` when the store stopped holding one (P2.2), and this is
    /// where the draw projection reads it.
    std::vector<float> typeScale;

    /// The last two ticks, as the renderer may see them (§7 P7.1).
    ///
    /// TWO, because interpolation needs both. `publish` rotates them: what was current becomes
    /// previous and a fresh snapshot is taken. A frame then draws somewhere between the two,
    /// which is what turns a 10 Hz sim into continuous motion on a 120 Hz screen.
    rm::sim::Snapshot snapshotPrevious;
    rm::sim::Snapshot snapshotCurrent;

    /// Where each unit is being DRAWN this frame — interpolated, float, derived, never read
    /// back. Kept between frames so the steady state allocates nothing.
    std::vector<rm::DrawUnit> drawUnits;

    /// Whether to interpolate at all. `--no-interpolate` turns it off, which is what a golden
    /// screenshot wants: a capture of tick N should be tick N rather than a blend that depends
    /// on when the frame happened to land.
    bool interpolate = true;

    // Scratch for drawing: one contiguous instance array per batch, refilled from the store
    // each frame. The store is flat and the GPU wants a run per model, so somebody has to
    // gather — this is P7's snapshot in embryo, arriving here because the renderer's upload
    // has to stay a memcpy.
    std::vector<std::vector<rm::UnitInstance>> drawScratch;

    // Where each live unit ended up in `drawScratch`, by slot: which batch, and which index
    // within it. Empty for a slot that is dead or was never filled. This is what turns a
    // handle back into something the renderer can outline.
    std::vector<rm::SelectionEntry> drawIndexOf;

    // The reverse: which slot each drawn instance came from, parallel to `drawScratch`.
    // Picking happens against what is DRAWN — that is what the ray can see — and this is
    // what turns the hit back into a unit the sim knows about.
    std::vector<std::vector<rm::UnitIndex>> drawSlotOf;

    // The sides in the match, empty outside a skirmish. Held with the scene rather
    // than beside it because every question that needs an army — may I select this,
    // may I shoot that, who banks the mass — starts from a unit.
    std::vector<rm::sim::Army> armies;

    /// What each faction fields, by role — so the opponent can ask for "a T1 extractor"
    /// instead of naming `/units/UEB1103/UEB1103_unit.bp` (P3.3).
    rm::data::Roster roster;

    /// The opponent's plan, from `data/opening.lua`. Falls back to `defaultOpening()`, which
    /// states the same thing in C++ so a build with no data directory still plays.
    rm::data::Opening opening = rm::data::defaultOpening();

    /// Every order this match has been given (P2.5).
    ///
    /// With the initial state this IS the match — §1.3's "the same command log produces the
    /// same match" naming something that exists rather than something intended. Written by
    /// `--command-log`.
    rm::sim::CommandLog commands;

    /// Who is participating, and which army each drives (P2.4).
    ///
    /// This replaces nothing yet — `playerArmy` below is still what the mouse and the HUD
    /// read — and that is deliberate: the level now EXISTS and is populated, so the code that
    /// needs it can start using it, but rewriting selection and the HUD to walk a player list
    /// is P2.5's job, where input becomes a command source with a player attached.
    std::vector<rm::sim::Player> players;

    /// The army the mouse belongs to. Only its units may be selected.
    ///
    /// Derived from `players` — the human's army, or `kNoArmy` when nobody is at the keyboard.
    /// Kept as a field because it is read in the frame loop and deriving it per frame would be
    /// a search for a value that cannot change mid-match.
    int playerArmy = rm::sim::kNoArmy;

    /// The definitions themselves, owned here so the catalog's pointers stay valid.
    std::deque<rm::unitdef::UnitDef> definitions;

    /// Shots in flight.
    std::vector<rm::sim::Projectile> projectiles;

    /// What the sim reported this tick (§7 P6.1). Owned here and handed to `Match`, like the
    /// projectile list — cleared by the tick, so nothing accumulates.
    rm::sim::EventQueue events;

    /// What each alliance can see (ADR-037). Empty — and therefore "everything is seen" —
    /// until `configureIntel` sizes it, which a skirmish does and a `--units` crowd does not.
    rm::sim::Intel intel;

    /// Reused across frames so plotting the minimap's blips is not an allocation a frame.
    /// Mutable because building the display out of a const scene is what every other draw
    /// path here does.
    mutable std::vector<rm::sim::Contact> contactScratch;

    /// One economy per army, indexed by army. Empty outside a skirmish.
    std::vector<rm::sim::Economy> economies;

    /// Everything under construction, all armies together. Partitioned per army each tick
    /// rather than held per army, because a build is a thing in the world and belongs with
    /// the others rather than filed under its owner.
    std::vector<rm::sim::Construction> building;

    /// `buildable` USED TO BE HERE, and its absence is the fix (`#3090`).
    ///
    /// It was a second registry of definitions, indexed 0..N in registration order and typed
    /// `rm::UnitTypeIndex` — the same type name the catalog uses for a completely different
    /// number. `Construction::blueprintIndex` meant "an index into `scene.buildable`" while
    /// `sim::applyCommand` read `catalog.def(command.buildType)`, so routing a build order
    /// through the one order path turned a 36-mass extractor into an 18,000-mass experimental
    /// and the economy never paid it off. The doc comment on `resolveBuildable` described the
    /// unified design for weeks; the code never caught up.
    ///
    /// Now there is one registry. `resolveBuildable` registers into `definitions` + `catalog`
    /// like everything else and returns a REAL type index, so the sim, the catalog and the draw
    /// gather all mean the same number by it.

    /// The VFS path behind each registered type, indexed BY TYPE INDEX. What a FINISHED
    /// construction spawns from: a definition alone cannot resolve a model.
    ///
    /// Sparse in the sense that a type registered from a model rather than a blueprint has an
    /// empty entry — that is a legitimate state (`--units` spawns bare models), not a hole.
    std::vector<std::string> pathForType;

    /// The draw batch each TYPE is rendered by, indexed by type index, or `kNoBatch`.
    ///
    /// WHY THIS EXISTS. The gather used to read `batch = unit.type` — the two index spaces were
    /// the same number by construction, and `resolveUnits` asserted it. That assertion is what
    /// made a buildable type impossible to register before something of it spawned, because
    /// doing so would have shifted every later type past its batch.
    ///
    /// With a real mapping, a type may exist with nothing to draw it — which is exactly what a
    /// blueprint that is buildable but not yet built IS. The gather skips those, as it already
    /// did for a type past the end of `drawScratch`; the difference is that now that state is
    /// representable on purpose rather than by running off the end of an array.
    static constexpr std::size_t kNoBatch = static_cast<std::size_t>(-1);
    std::vector<std::size_t> batchForType;

    /// Records that `type` draws with `batch`, growing the map as types are registered.
    void setBatchForType(rm::UnitTypeIndex type, std::size_t batch) {
        if (batchForType.size() <= static_cast<std::size_t>(type)) {
            batchForType.resize(static_cast<std::size_t>(type) + 1, kNoBatch);
        }
        batchForType[static_cast<std::size_t>(type)] = batch;
    }

    /// The batch for a type, or `kNoBatch` when it has none.
    [[nodiscard]] std::size_t batchOf(rm::UnitTypeIndex type) const noexcept {
        const auto index = static_cast<std::size_t>(type);
        return index < batchForType.size() ? batchForType[index] : kNoBatch;
    }

    /// The VFS path a type was loaded from, or empty.
    [[nodiscard]] std::string_view pathOf(rm::UnitTypeIndex type) const noexcept {
        const auto index = static_cast<std::size_t>(type);
        return index < pathForType.size() ? std::string_view{pathForType[index]}
                                          : std::string_view{};
    }

    /// Records the path a type was loaded from.
    void setPathForType(rm::UnitTypeIndex type, std::string_view path) {
        if (pathForType.size() <= static_cast<std::size_t>(type)) {
            pathForType.resize(static_cast<std::size_t>(type) + 1);
        }
        pathForType[static_cast<std::size_t>(type)] = std::string{path};
    }

    /// Records the per-type facts the passability grids and the draw scale read.
    ///
    /// INDEXED RATHER THAN APPENDED, and that is the whole point. These three were
    /// `push_back`ed at the batch-creation sites, which worked only while every type WAS a
    /// batch. The moment `resolveBuildable` began registering a type with no batch (`#3090`),
    /// appending fell one behind and every later type read the wrong slope limit and the wrong
    /// model scale — which moved a screenshot, because `instanceFor` reads `typeScale[type]`.
    ///
    /// That is the same parallel-array hazard the third batch site's own comment records having
    /// been bitten by before. Setting by index cannot fall behind, whatever order types are
    /// registered in.
    void setTypeTraits(rm::UnitTypeIndex type, float slopeDegrees, float waterDepthElmos,
                       float scale) {
        const auto index = static_cast<std::size_t>(type);
        if (maxSlopeDegrees.size() <= index) {
            maxSlopeDegrees.resize(index + 1);
            maxWaterDepthElmos.resize(index + 1);
            typeScale.resize(index + 1, 1.0f);
        }
        maxSlopeDegrees[index] = slopeDegrees;
        maxWaterDepthElmos[index] = waterDepthElmos;
        typeScale[index] = scale;
    }

    /// The TYPE each spawned blueprint reuses, so twenty tanks are one type, one batch and
    /// one draw call rather than twenty. Keyed by VFS path, same as the cache in
    /// spawnCommanders is keyed by faction.
    ///
    /// **THIS HELD A BATCH INDEX AND WAS READ AS A TYPE INDEX** (`#3090`) —
    /// `static_cast<rm::UnitTypeIndex>(found->second)` in `spawnUnit`, which was correct only
    /// while the two numbers were the same by construction. It is the type now; the batch comes
    /// from `batchOf`. This was the regression the screenshot caught when the index spaces were
    /// separated, and it is exactly the class of bug that separating them exists to prevent.
    std::map<std::string, rm::UnitTypeIndex, std::less<>> typeForBlueprint;

    /// Set when a spawn added instances mid-simulation, so the tick loop knows its
    /// collision spans point at moved storage and rebuilds them.
    bool grewThisTick = false;

    /// What the dead left behind, as OBJECTS (§7 P6.2).
    ///
    /// This used to be `std::vector<rm::DecalVertex>` — GPU vertices accumulated per death —
    /// which made a triangle list the only record that anything had died there. The sim owns
    /// the fact now (`sim::FeatureStore`) and the decals are a projection of it, rebuilt from
    /// `wreckDecals()` when the count changes. Same shape as the units: state in the sim,
    /// geometry derived at the boundary.
    rm::sim::FeatureStore features;

    /// The scorch marks, projected from `features`. Rebuilt when a wreck is added rather than
    /// every frame — a wreck does not move, so there is nothing to recompute otherwise.
    std::vector<rm::DecalVertex> wreckDecals;

    /// How many features `wreckDecals` was built from, so the rebuild happens exactly when
    /// there is something new to draw.
    std::size_t wreckDecalsFrom = 0;

    /// Death explosions set off, and the damage they dealt. BOTH, because they answer
    /// different questions: a blast that goes off and hurts nothing is the ordinary case when
    /// two commanders kill each other in the same tick, and reporting only the damage would
    /// make that look like the explosions never happened.
    std::size_t deathBlasts = 0;
    rm::sim::Mag deathBlastDamage{};

    /// How many commanders each army STARTED with, indexed by army. The win condition
    /// needs it to tell "lost its commander" from "never had one" — a `--units` crowd must
    /// not be declared a draw on the first tick.
    std::vector<int> commandersEver;

    /// Living commanders per army, recounted each tick.
    ///
    /// Delegates now: the sim owns the definition of "a living commander of army N", and this
    /// used to be a second copy of it walking a different layout. `UnitCensus` (P1.3) makes
    /// the same answer O(1), and wiring it here is P3's job — the scan is still cheap next to
    /// a tick, and swapping it now would be a second change riding on this one.
    [[nodiscard]] std::vector<int> countCommanders() const {
        return rm::sim::countCommanders(store, catalog, armies.size());
    }

    /// Rotates the snapshots and takes a fresh one (§7 P7.1).
    ///
    /// Called once per SIM TICK, not once per frame — the whole point is that the two snapshots
    /// are a tick apart. Called outside the tick too, after a spawn, and there `previous` and
    /// `current` end up equal, which is exactly right: a unit that has just appeared has not
    /// travelled anywhere, so there is nothing for any alpha to blend.
    void publish(rm::TickIndex tick) {
        std::swap(snapshotPrevious, snapshotCurrent);
        rm::sim::snapshotInto(store, tick, snapshotCurrent);
    }

    /// Refills `drawScratch` and `drawIndexOf` from the store.
    ///
    /// One contiguous run per batch, because that is what the GPU is uploaded: the store is
    /// flat and a draw call wants one model's instances together. Dead units are left out —
    /// a corpse's scale is already zero so drawing it costs a collapsed mesh, but leaving it
    /// out costs nothing at all.
    ///
    /// The scratch vectors are cleared rather than freed, so a steady-state frame does no
    /// allocation after the first few.
    ///
    /// It also re-points each batch's `instances` span at its scratch vector, which is not
    /// tidiness — it is a correctness requirement with a sharp edge. `Renderer::setUnits`
    /// takes each batch's instance capacity from `batch.instances.size()`, and a batch built
    /// mid-match starts with an empty span. Before the flat store, `spawnUnit` re-pointed the
    /// span on every spawn, so a new batch was never empty by the time `setUnits` saw it;
    /// there is no per-batch vector to re-point any more. Doing it here means the invariant
    /// holds wherever the gather is called, rather than at four call sites that must
    /// remember. (The span also goes stale on its own: a scratch vector that grows past its
    /// capacity reallocates.)
    /// The alliance whose view the screen shows, or `kNoAlliance` for one that sees all.
    ///
    /// `--observer` seats nobody, and nobody is exactly who should see the whole board: that
    /// is what watching two scripted opponents play each other means. A scene with no intel
    /// configured answers the same way, which is every scene that predates ADR-037.
    static constexpr int kNoAlliance = -1;

    [[nodiscard]] int viewingAlliance() const noexcept {
        if (!intel.active() || playerArmy == rm::sim::kNoArmy) {
            return kNoAlliance;
        }
        const auto army = static_cast<std::size_t>(playerArmy);
        return army < armies.size() ? armies[army].alliance : kNoAlliance;
    }

    /// Whether the viewer's side may see a unit at this drawn position.
    ///
    /// TAKES THE DRAWN POSITION, not the sim's, because that is where the player is looking:
    /// a unit interpolated a few elmos ahead of its last tick should appear and disappear
    /// against the ground it appears to be on. The difference is under one square at any
    /// speed this engine moves things at.
    [[nodiscard]] bool visibleToViewer(int viewer, int owner, float worldX,
                                       float worldZ) const noexcept {
        if (viewer == kNoAlliance) {
            return true;
        }
        if (owner >= 0 && static_cast<std::size_t>(owner) < armies.size()
            && armies[static_cast<std::size_t>(owner)].alliance == viewer) {
            return true;  // your own side, wherever it has got to
        }
        return intel.sees(viewer, rm::sim::IntelKind::Vision, rm::sim::fxFromFloat(worldX),
                          rm::sim::fxFromFloat(worldZ));
    }

    /// Hands the viewer's vision grid to whatever draws this scene.
    ///
    /// TEMPLATED ON THE TARGET because the two paths take different types — a `Window` in
    /// the interactive loop and a bare `Renderer` in the headless capture — and both have
    /// the same two calls. The alternative was an interface with two implementations, for
    /// two call sites, to abstract over a pair of methods that already agree.
    template <typename Target>
    void applyFog(Target& target) const {
        const int viewer = viewingAlliance();
        if (viewer == kNoAlliance) {
            target.clearFog();  // an observer sees the whole board; that is what watching is
            return;
        }
        const rm::sim::IntelGrid& sight = intel.grid(viewer, rm::sim::IntelKind::Vision);
        target.setFog(sight.counts(), sight.squaresX(), sight.squaresZ(),
                      static_cast<float>(sight.squaresX())
                          * rm::sim::fxToFloat(sight.squareElmos()),
                      static_cast<float>(sight.squaresZ())
                          * rm::sim::fxToFloat(sight.squareElmos()));
    }

    void gatherForDrawing(float alpha = 1.0f) {
        // FROM THE SNAPSHOTS, not from the store (§7 P7.1/P7.2). This used to walk
        // `store.transforms()` and `store.motion()` directly, which is why motion stepped at
        // the tick rate: a frame drew wherever the sim happened to be, and there was no second
        // state to blend with.
        if (interpolate) {
            rm::interpolate(snapshotPrevious, snapshotCurrent, alpha, drawUnits);
        } else {
            rm::project(snapshotCurrent, drawUnits);
        }

        drawScratch.resize(batches.size());
        drawSlotOf.resize(batches.size());
        for (std::vector<rm::UnitInstance>& batch : drawScratch) {
            batch.clear();
        }
        for (std::vector<rm::UnitIndex>& batch : drawSlotOf) {
            batch.clear();
        }
        drawIndexOf.assign(store.slotCount(), rm::SelectionEntry{});

        const int viewer = viewingAlliance();

        for (const rm::DrawUnit& unit : drawUnits) {
            // FOG OF WAR (ADR-037). A unit the viewer's side cannot see is not drawn at all —
            // not drawn dimmed, not drawn as a ghost. It is also left out of `drawIndexOf`,
            // which is what picking reads, so an invisible unit cannot be clicked either;
            // that is the same rule the gather already applied to the dead, and getting it
            // wrong would leak positions through the cursor rather than through the screen.
            if (!visibleToViewer(viewer, unit.armyIndex, unit.position[0], unit.position[2])) {
                continue;
            }

            // THROUGH THE MAP, not `unit.type` directly (`#3090`). A type and a batch are no
            // longer the same number: a blueprint can be registered as buildable long before
            // anything of it is built, and such a type has no batch until it spawns.
            const std::size_t batch = batchOf(unit.type);
            if (batch == kNoBatch || batch >= drawScratch.size()) {
                continue;  // a type with no batch: nothing to draw it with
            }
            // Still keyed by SLOT, because that is what selection and picking name a unit by,
            // and a snapshot entry carries the id it came from.
            const auto slot = static_cast<std::size_t>(unit.id.index);
            if (slot < drawIndexOf.size()) {
                drawIndexOf[slot] =
                    rm::SelectionEntry{.batch = batch, .instance = drawScratch[batch].size()};
            }
            drawScratch[batch].push_back(instanceFor(unit));
            drawSlotOf[batch].push_back(unit.id.index);
        }

        for (std::size_t batch = 0; batch < batches.size(); ++batch) {
            batches[batch].instances = drawScratch[batch];
        }
    }

    /// Builds the GPU's view of one unit from the sim's.
    ///
    /// THE PROJECTION, and the reason `UnitInstance` is no longer sim state (P2.2). Everything
    /// here is derived: the position and angles convert from fixed point, the scale is a
    /// property of the TYPE, the colour is a property of the ARMY, and the walk-cycle phase is
    /// the ground the unit has covered divided by the stride its animation implies.
    ///
    /// One way only. Nothing reads a `UnitInstance` back into the store — that would be a
    /// float round-trip through the middle of a match, which is exactly what fixed point is
    /// for avoiding.
    /// TAKES A `DrawUnit` now, not a slot and two sim structs (§7 P7.2). The position and
    /// angles arrive already interpolated and already float; what is left here is the part that
    /// comes from the TYPE and the ARMY rather than from the tick — the scale, the colour, and
    /// the animation clip the stride is measured against.
    [[nodiscard]] rm::UnitInstance instanceFor(const rm::DrawUnit& unit) const {
        rm::UnitInstance instance{};
        instance.position = unit.position;
        instance.rotationY = unit.rotationY;
        instance.rotationX = unit.rotationX;
        instance.rotationZ = unit.rotationZ;

        const auto type = static_cast<std::size_t>(unit.type);
        instance.scale = type < typeScale.size() ? typeScale[type] : 1.0f;

        // The army's colour. `kNoArmy` and an out-of-range owner both get the first palette
        // entry, which is what a decorative crowd should look like — the alternative, treating
        // an unowned unit as army zero's, is the bug `kNoArmy = -1` exists to prevent, and it
        // is prevented in the SIM rather than here.
        // Indexed from the PALETTE rather than read off the army (§7 P6.3): a colour is how an
        // army is drawn and not a fact about one, so it lives with the renderer's other
        // presentation and the sim no longer carries it. Same answer, one include fewer in the
        // sim — which is the whole of P6.3's assertion.
        const int owner = unit.armyIndex;
        instance.teamColour =
            owner >= 0 && static_cast<std::size_t>(owner) < armies.size()
                ? rm::teamColour(static_cast<std::size_t>(owner))
                : rm::kTeamColours[0];

        // The walk cycle, paced by ground covered rather than by wall time — a unit pivoting
        // on the spot or standing still must not keep striding. Zero for a type with no
        // animation, which is every structure. The distance is INTERPOLATED, so a leg no longer
        // steps at the tick rate either.
        const float duration = type < batches.size() && batches[type].animation != nullptr
                                   ? batches[type].animation->duration
                                   : 0.0f;
        const float speed =
            unit.speedPerTick * static_cast<float>(gAppTickRate.ticksPerSecond());
        const float strideElmos = speed * duration;
        if (strideElmos > 0.0f) {
            instance.animationPhase = unit.distanceTravelledElmos / strideElmos;
        }

        return instance;
    }

    /// The unit behind a drawn instance, or nothing when the pair names nothing drawn.
    [[nodiscard]] std::optional<rm::sim::UnitId> unitDrawnAt(std::size_t batch,
                                                             std::size_t index) const {
        if (batch >= drawSlotOf.size() || index >= drawSlotOf[batch].size()) {
            return std::nullopt;
        }
        return store.idAt(drawSlotOf[batch][index]);
    }

    /// Who owns the unit in a slot, or kNoArmy.
    [[nodiscard]] int armyOf(rm::UnitIndex slot) const noexcept {
        const std::span<const rm::sim::MoveState> motion = store.motion();
        if (slot >= motion.size()) {
            return rm::sim::kNoArmy;
        }
        return motion[slot].armyIndex;
    }

    /// Where a live unit is being drawn, or nothing when it is dead or undrawable.
    ///
    /// The bridge between a handle, which is how the sim and the UI name a unit, and a
    /// (batch, index) pair, which is the only thing the renderer can outline.
    [[nodiscard]] std::optional<rm::SelectionEntry> drawnAt(rm::sim::UnitId id) const {
        if (!store.alive(id) || id.index >= drawIndexOf.size()) {
            return std::nullopt;
        }
        const rm::SelectionEntry& where = drawIndexOf[id.index];
        if (where.batch >= drawScratch.size()
            || where.instance >= drawScratch[where.batch].size()) {
            return std::nullopt;
        }
        return where;
    }

};

// The passability grids a scene needs, one per distinct pair of limits.
//
// Keyed on the LIMITS rather than on the unit type, because the grid depends on
// nothing else — every unit that climbs 17 degrees and wades 12 elmos sees the
// same map, whatever model it wears. On a scene of a dozen unit types that is
// usually two or three grids rather than a dozen.
class PassabilitySet {
public:
    PassabilitySet(const rm::HeightField& field, float waterLevel)
        : field_{&field}, waterLevel_{waterLevel} {}

    [[nodiscard]] const rm::sim::PassabilityGrid& gridFor(float slopeDegrees, float depthElmos) {
        const auto key = std::make_pair(slopeDegrees, depthElmos);
        const auto existing = grids_.find(key);
        if (existing != grids_.end()) {
            return existing->second;
        }

        rm::sim::PassabilityGrid grid =
            rm::sim::buildPassability(*field_, waterLevel_, slopeDegrees, depthElmos);
        std::printf("passability: %d x %d cells of %.0f elmos, %zu%% walkable"
                    " (maxslope %.0f deg, maxwaterdepth %.0f)\n",
                    grid.cellsX, grid.cellsZ,
                    static_cast<double>(rm::sim::fxToFloat(grid.elmosPerCell)),
                    grid.passable.empty()
                        ? 0u
                        : 100u * static_cast<std::size_t>(std::count(grid.passable.begin(),
                                                                     grid.passable.end(),
                                                                     std::uint8_t{1}))
                              / grid.passable.size(),
                    static_cast<double>(slopeDegrees), static_cast<double>(depthElmos));

        return grids_.emplace(key, std::move(grid)).first->second;
    }

private:
    const rm::HeightField* field_;
    float waterLevel_;
    std::map<std::pair<float, float>, rm::sim::PassabilityGrid> grids_;
};

/// The colour of the ring drawn on the ground under a selected unit.
///
/// The ONLY selection feedback. Milestone 13 also tinted the selected unit
/// white through its team-colour slot, which was free but misleading: in an RTS
/// a unit's own colours mean allegiance, and overwriting them to mean "selected"
/// makes a unit change sides for as long as it is in the set. A marker on the
/// ground says the same thing without lying about the unit (ADR-021).
///
/// One colour for every unit rather than the unit's own team colour: a
/// selection is "mine", and the question a ring answers is which units an order
/// will reach — not which army they belong to, which the model already says.
///
/// Bright green because it is the one hue no stratum, sea or sky in either
/// game's palette occupies at this saturation, and translucent so the ground it
/// marks still reads through it.
inline constexpr std::array<float, 4> kSelectionRingColour{{0.35f, 1.0f, 0.45f, 0.55f}};

/// How much wider than the unit's collision radius the ring is drawn.
///
/// A ring exactly on the radius touches the model's feet and reads as part of
/// it. A little outside reads as a marker on the ground, which is what it is.
inline constexpr float kSelectionRingMargin = 1.35f;

/// The range ring: the longest firing weapon's reach, drawn while a unit is selected.
///
/// QUIET BY DESIGN — a thin band at low alpha in the loss family's hue, because a range is a
/// threat radius and red is what threat already means here. Selection is green and range is
/// dim red, so the two rings around one unit cannot be confused; the alpha is low enough
/// that a crowd's overlapping ranges shade the ground rather than painting it.
inline constexpr std::array<float, 4> kRangeRingColour{{0.95f, 0.45f, 0.35f, 0.28f}};
inline constexpr float kRangeRingThicknessElmos = 1.2f;

/// The colour of the marker where a move order was given.
///
/// Amber against the rings' green, and a cross rather than a plain ring, because
/// the two appear on the same ground within a second of each other — a right-click
/// follows a left-click — and one distinction would not be enough in the one place
/// the player is looking. Two, so it survives being small or being colour-blind.
inline constexpr std::array<float, 4> kOrderMarkerColour{{1.0f, 0.72f, 0.20f, 0.85f}};

/// The build ghost's ring, where a structure would be founded.
///
/// CYAN, which is neither the selection's green nor the order marker's amber — three cues can
/// share the same ground within a second (select a commander, arm a cell, hover a site) and a
/// ghost that borrowed either colour would read as one of them. It is also the interface's own
/// hue, which is what a thing that is not yet part of the world should look like.
inline constexpr std::array<float, 4> kBuildGhostColour{{0.35f, 0.85f, 1.0f, 0.70f}};

/// The same ring where the footprint will not fit.
///
/// RED AND MORE OPAQUE, because it is a refusal and a refusal should not be the subtler of two
/// states. The pair carries the answer without a label, and the difference survives being small
/// — red against cyan differs in brightness as well as hue, so it does not rely on colour
/// vision alone, which is the same reasoning the order marker's cross records.
inline constexpr std::array<float, 4> kBuildGhostBlockedColour{{1.0f, 0.30f, 0.25f, 0.85f}};

// --- What the scene layer does ------------------------------------------------------------

void setAppTickRate(std::uint32_t ticksPerSecond);

/// Builds the roster from the whole blueprint corpus.
[[nodiscard]] rm::data::Roster buildRoster(const rm::vfs::Vfs& content);

/// A float world position as the fixed-point triple the geometry functions take.
[[nodiscard]] std::array<rm::sim::Fx, 3> fxPoint(const std::array<float, 3>& position);

[[nodiscard]] rm::sim::Transform transformAt(const std::array<float, 3>& position, float yaw);

/// Rebuilds the wreck decals from the features, if any have been added since the last time.
void refreshWreckDecals(UnitScene& scene, const rm::HeightField& field);

[[nodiscard]] rm::data::OpeningStep stepForRole(const rm::data::Opening& opening,
                                                rm::unitdef::Role role);
[[nodiscard]] rm::data::OpeningStep energyStep(const rm::data::Opening& o);
[[nodiscard]] rm::data::OpeningStep factoryStep(const rm::data::Opening& o);
[[nodiscard]] rm::data::OpeningStep extractorStep(const rm::data::Opening& o);

/// The blueprint path an opening step resolves to for one army, or empty.
[[nodiscard]] std::string blueprintFor(const UnitScene& scene, const rm::sim::Army& army,
                                       const rm::data::OpeningStep& step);

} // namespace rm::app
