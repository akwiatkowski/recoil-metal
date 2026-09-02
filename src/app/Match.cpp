#include "app/Match.hpp"

#include "app/FafAi.hpp"
#include "app/FafOpponent.hpp"

#include "core/sim/BuildOrder.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/SlowUpdate.hpp"
#include "core/sim/StateHash.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace rm::app {

// Defined here, declared `extern` in the header.
bool gPrintEvents = false;
bool gFafOpponents = false;
bool gFafLog = false;

/// Sends one unit to a world position, routed around whatever is in the way.
///
/// Falls back to nothing rather than to a straight line when no route exists:
/// walking into a cliff because the search failed is worse than standing still,
/// and standing still is at least legible as "it cannot get there".
/// Issues a move order AS A COMMAND, records it, and says whether it took.
///
/// THE ONE PATH (P2.5). Every order in this file — a player's right-click, a scripted
/// opponent's attack, a tank rolling off a factory floor — goes through `applyCommand`, so
/// there is no way for the human path and the script path to drift apart. The command is
/// recorded on the way through, which is what makes `scene.commands` the match.
///
/// `player` is who is issuing it, and it is checked rather than assumed: `applyCommand` refuses
/// an order from a player who does not command the unit's army.
/// `queued` is the shift key: the order goes behind whatever the unit is already doing rather
/// than replacing it (§7 P4.1). The scripted opponents never pass it — an opponent that queued
/// its orders would still be walking a route it decided on thirty seconds ago.
[[nodiscard]] bool issueMove(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                               rm::PlayerIndex player, rm::TickIndex tick, rm::sim::Fx toX,
                               rm::sim::Fx toZ, bool queued, rm::sim::CommandKind kind,
                               rm::sim::CommandPhase phase) {
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = phase,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = kind,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = toX,
        .targetZ = toZ,
        .buildType = 0,
    }).has_value();
}

[[nodiscard]] bool issueMove(UnitScene& scene, rm::sim::UnitId unit, rm::PlayerIndex player,
                              rm::TickIndex tick, rm::sim::Fx toX, rm::sim::Fx toZ, bool queued,
                              rm::sim::CommandKind kind, rm::sim::CommandPhase phase) {
    return issueMove(scene, std::span<const rm::sim::UnitId>{&unit, 1}, player, tick, toX, toZ,
                     queued, kind, phase);
}

[[nodiscard]] bool issueAttack(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::sim::UnitId target, rm::sim::Fx toX, rm::sim::Fx toZ,
                                bool queued) {
    // issueMove's sibling, and deliberately its shape: one command, one path through
    // applyCommand, recorded only when it took. The target handle is what turns the order
    // into a pursuit (`advanceOrders`' chase); toX/toZ are where the target IS right now,
    // which routes the first leg and seeds the chase's memory.
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Attack,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = toX,
        .targetZ = toZ,
        .target = target,
        .buildType = 0,
    }).has_value();
}

[[nodiscard]] bool issueOvercharge(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                    rm::PlayerIndex player, rm::TickIndex tick,
                                    rm::sim::UnitId target, rm::sim::Fx toX, rm::sim::Fx toZ,
                                    bool queued) {
    // issueAttack with a different kind: same pursuit, but the shot is the MANUAL
    // weapon's, gated on energy, and one per order (`fireOvercharge`).
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Overcharge,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = toX,
        .targetZ = toZ,
        .target = target,
        .buildType = 0,
    }).has_value();
}

[[nodiscard]] bool issueAssist(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::sim::UnitId target, bool queued) {
    // issueAttack's shape with the guard order's kind: the target's position seeds the
    // route, the pursuit holds at build reach, and `applyAssistance` does the lending.
    if (!scene.store.alive(target)) {
        return false;
    }
    const rm::sim::Transform& at = scene.store.transforms()[target.index];
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Assist,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = at.x,
        .targetZ = at.z,
        .target = target,
        .buildType = 0,
    }).has_value();
}

[[nodiscard]] bool issueRepair(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::sim::UnitId target, bool queued) {
    if (!scene.store.alive(target)) {
        return false;
    }
    const rm::sim::Transform& at = scene.store.transforms()[target.index];
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Repair,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = at.x,
        .targetZ = at.z,
        .target = target,
        .buildType = 0,
    }).has_value();
}

[[nodiscard]] bool issueReclaim(UnitScene& scene, std::span<const rm::sim::UnitId> units,
                                 rm::PlayerIndex player, rm::TickIndex tick,
                                 rm::sim::FeatureId wreck, bool queued) {
    // issueAttack's sibling for the ground's own treasure: the feature handle rides in
    // `target`, the wreck's position seeds the route, and the one path applies it.
    const rm::sim::Feature* found = scene.features.find(wreck);
    if (found == nullptr) {
        return false;  // clicked a wreck that was reclaimed this very tick
    }
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Reclaim,
        .queued = queued,
        .units = {units.begin(), units.end()},
        .targetX = found->at[0],
        .targetZ = found->at[2],
        .target = wreck,
        .buildType = 0,
    }).has_value();
}

// One loose end survives the routing, deliberate and small: a blueprint registered by
// `resolveBuildable` and later built gets a SECOND type from `spawnUnit`, because that path
// creates a type and a batch together. Both are real type indices resolving through the catalog,
// so nothing is ambiguous — it costs one catalog entry per buildable blueprint. Collapsing them
// means teaching `spawnUnit` to add a batch to an existing type.

/// The definition a `Construction::blueprintIndex` names.
///
/// ONE INDEX SPACE (`#3090`). That field used to index a private `scene.buildable` list while
/// `sim::applyCommand` read `catalog.def()` — the same type name meaning two different numbers,
/// which is why builds could not go through the one order path. It is a real `UnitTypeIndex`
/// now, so this is a catalog lookup like every other.
///
/// A missing definition is not reachable through the build path — a construction only exists
/// because `resolveBuildable` registered its type — but the catalog returns a pointer, so the
/// empty definition is what a caller gets rather than a dereferenced null.
[[nodiscard]] const rm::unitdef::UnitDef& buildableDef(const UnitScene& scene,
                                                       std::size_t blueprintIndex) {
    static const rm::unitdef::UnitDef kNone{};
    const rm::unitdef::UnitDef* def =
        scene.catalog.def(static_cast<rm::UnitTypeIndex>(blueprintIndex));
    return def != nullptr ? *def : kNone;
}

// `playerDriving` moved to `SceneBuild.cpp`, beside `issueBuild`, which needs it too.

// `orderRouted` used to live here, and every order in this file went through it.
//
// It is gone: `issueMove` above does the same job and records the order on the way through, so
// there is one path into the sim rather than two similar ones (P2.5). That is what makes
// "a human log and a script log replay through the same path" a structural fact rather than
// an aspiration — there is no other path to take.


/// Gathers what `army` has standing, by walking the store once.
///
/// A scan, still: `UnitCensus` (P1.3) answers this in O(1) and wiring it in is P3's, where
/// the roles it keys on stop being hardcoded blueprint ids. One pass a second over a few
/// hundred units is not what this costs.
[[nodiscard]] Standing standingFor(const UnitScene& scene, int army) {
    Standing standing;
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }
        if (motion[slot].armyIndex != army || !health[slot].alive()) {
            continue;
        }
        if (rm::sim::isCommanderId(def->name)) {
            standing.commanderAlive = true;
            standing.commanderPosition = rm::sim::positionOf(transforms[slot]);
            standing.commanderBuildRate = def->buildRate;
            standing.commander = scene.store.idAt(slot);
        } else {
            // BY ROLE, not by blueprint id (P3.3). These were four `def->name == "UEB1103"`
            // comparisons, which is why the census only recognised UEF structures: a Cybran
            // mass extractor is a mass extractor and was counted as nothing.
            switch (rm::unitdef::roleOf(*def)) {
            case rm::unitdef::Role::Extractor:
                ++standing.extractors;
                break;
            case rm::unitdef::Role::Energy:
                ++standing.powerGenerators;
                break;
            case rm::unitdef::Role::Factory:
                ++standing.factories;
                standing.factoryPosition = rm::sim::positionOf(transforms[slot]);
                standing.factoryBuildRate = def->buildRate;
                standing.factory = scene.store.idAt(slot);
                break;
            case rm::unitdef::Role::Raider:
            case rm::unitdef::Role::Assault:
                standing.tanks.push_back(scene.store.idAt(slot));
                break;
            default:
                break;
            }
        }
    }
    return standing;
}

/// The nearest living enemy commander to `from`, or nothing when the war is over.
/// Where the attack wave walks: kill it and its army is defeated, which is the
/// whole win condition.
[[nodiscard]] std::optional<std::array<rm::sim::Fx, 3>> nearestEnemyCommander(
    const UnitScene& scene, int army, const std::array<rm::sim::Fx, 3>& from) {
    if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
        return std::nullopt;
    }
    std::optional<std::array<rm::sim::Fx, 3>> best;
    rm::sim::Fx bestDistance{};
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
            continue;
        }
        const int theirs = motion[slot].armyIndex;
        if (theirs < 0 || static_cast<std::size_t>(theirs) >= scene.armies.size()
            || !health[slot].alive()
            || !rm::sim::hostile(scene.armies[static_cast<std::size_t>(army)],
                                 scene.armies[static_cast<std::size_t>(theirs)])) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, rm::sim::positionOf(transforms[slot]));
        if (!best || distance < bestDistance) {
            best = rm::sim::positionOf(transforms[slot]);
            bestDistance = distance;
        }
    }
    return best;
}

/// The nearest Mass deposit to `from` that nothing has claimed — no construction
/// (finished ones stay in the list, so a standing extractor counts) within a
/// footprint of it.
[[nodiscard]] const rm::scenario::Marker* nearestFreeDeposit(
    const UnitScene& scene, std::span<const rm::scenario::Marker> markers,
    const std::array<rm::sim::Fx, 3>& from) {
    /// A deposit within this of an existing build site is the SAME deposit —
    /// half an extractor footprint. The old comment said "generous against float drift";
    /// there is no float drift here any more, and the generosity is now purely about the
    /// deposit being a point and the extractor a footprint.
    constexpr rm::sim::Fx kClaimedRadius = rm::sim::Fx::fromInt(8);

    const rm::scenario::Marker* nearest = nullptr;
    rm::sim::Fx nearestDistance{};
    for (const rm::scenario::Marker& marker : markers) {
        if (!marker.isType("Mass")) {
            continue;
        }
        bool claimed = false;
        for (const rm::sim::Construction& work : scene.building) {
            // `work.position` is already fixed point (§7 P10.0); only the MARKER still
            // needs converting, because it comes out of a map file as floats.
            if (rm::sim::groundDistanceElmos(work.position, fxPoint(marker.position))
                < kClaimedRadius) {
                claimed = true;
                break;
            }
        }
        if (claimed) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, fxPoint(marker.position));
        if (nearest == nullptr || distance < nearestDistance) {
            nearest = &marker;
            nearestDistance = distance;
        }
    }
    return nearest;
}

} // namespace rm::app

namespace rm::ai {

void ScriptedOpponent::observe(const World& world, std::span<const rm::sim::Event> events) {
    // The events are ignored, and that is the script rather than the port: it decides from what
    // is STANDING, re-read every pass, so a structure that dies is simply rebuilt without anyone
    // having to notice it died. An adapter hosting a real AI is the consumer these exist for.
    (void)events;
    world_ = &world;
}

void ScriptedOpponent::advance(rm::TickIndex tick) {
    (void)tick;  // this script's cadence is the caller's; it has no clock of its own
    decisions_.clear();
    if (world_ == nullptr || army_ < 0) {
        return;
    }

    const rm::app::UnitScene& scene = world_->scene;
    const rm::sim::Army& army = scene.armies[static_cast<std::size_t>(army_)];
    const rm::app::Standing standing = rm::app::standingFor(scene, army_);

    // What is being paid for right now, split by who builds it: the commander
    // owns structures, the factory owns tanks.
    bool structureUnderway = false;
    bool tankUnderway = false;
    for (const rm::sim::Construction& work : scene.building) {
        if (work.armyIndex != army_ || work.finished()) {
            continue;
        }
        (rm::app::buildableDef(scene, work.blueprintIndex).isMobile() ? tankUnderway
                                                                     : structureUnderway) = true;
    }

    const rm::sim::ArmyView view{
        .commanderAlive = standing.commanderAlive,
        .commanderBusy = structureUnderway,
        .factoryBusy = tankUnderway,
        .extractorsStanding = standing.extractors,
        .powerGeneratorsStanding = standing.powerGenerators,
        .factoriesStanding = standing.factories,
        .tanksAlive = standing.tanks.size(),
    };

    // The commander's next structure, placed around ITS OWN start position —
    // the base grows where the map put the army, not where the commander wandered.
    const rm::sim::StructureOrder structure = rm::sim::nextStructure(view);
    if (structure != rm::sim::StructureOrder::None) {
        std::string blueprint;
        std::optional<std::array<rm::sim::Fx, 3>> site;
        const std::size_t slot = standing.powerGenerators + standing.factories;
        const rm::mapinfo::StartPosition& start =
            world_->starts[static_cast<std::size_t>(army_)];
        const std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x), rm::sim::Fx{},
                                              rm::sim::fxFromFloat(start.z)};
        switch (structure) {
        case rm::sim::StructureOrder::PowerGenerator:
            blueprint = rm::app::blueprintFor(scene, army, rm::app::energyStep(scene.opening));
            site = rm::sim::structureSite(home, world_->centreX, world_->centreZ,
                                          static_cast<int>(slot));
            break;
        case rm::sim::StructureOrder::Factory:
            blueprint = rm::app::blueprintFor(scene, army, rm::app::factoryStep(scene.opening));
            site = rm::sim::structureSite(home, world_->centreX, world_->centreZ,
                                          static_cast<int>(slot));
            break;
        case rm::sim::StructureOrder::Extractor: {
            blueprint =
                rm::app::blueprintFor(scene, army, rm::app::extractorStep(scene.opening));
            const rm::scenario::Marker* deposit =
                rm::app::nearestFreeDeposit(scene, world_->markers, standing.commanderPosition);
            if (deposit != nullptr) {
                site = rm::app::fxPoint(deposit->position);
            }
            break;
        }
        case rm::sim::StructureOrder::None:
            break;
        }
        if (site) {
            decisions_.push_back(Decision{
                .kind = Decision::Kind::StartConstruction,
                .blueprint = blueprint,
                .site = *site,
                .builder = standing.commander,
                .buildRate = standing.commanderBuildRate,
            });
        }
    }

    // The factory's next tank, built where the factory stands and rolled off it
    // once finished (the spawn handles the rolloff).
    if (rm::sim::wantsTank(view)) {
        decisions_.push_back(Decision{
            .kind = Decision::Kind::StartConstruction,
            .blueprint = rm::app::blueprintFor(scene, army, scene.opening.waveUnit),
            .site = standing.factoryPosition,
            .builder = standing.factory,
            .buildRate = standing.factoryBuildRate,
        });
    }

    // The one attack wave: at strength, every tank walks at the nearest enemy
    // commander. After this, reinforcements are sent as they roll off.
    //
    // The strength is the WAVE UNIT'S OWN (waveSizeFor): a 300 hp tank earns the plan's
    // twenty, a 29 hp bot earns its thirty-nine — one number per plan was the recorded
    // simplification, and the plan's figure now only covers the ticks before the first
    // wave unit has resolved a type (when no wave could launch anyway).
    std::size_t waveSize = scene.opening.waveSize;
    if (const auto waveType = scene.typeForBlueprint.find(
            rm::app::blueprintFor(scene, army, scene.opening.waveUnit));
        waveType != scene.typeForBlueprint.end()) {
        if (const rm::unitdef::UnitDef* waveDef = scene.catalog.def(waveType->second)) {
            waveSize = rm::unitdef::waveSizeFor(*waveDef);
        }
    }
    if (rm::sim::launchesAttack(script_, view, waveSize)) {
        const std::optional<std::array<rm::sim::Fx, 3>> target =
            rm::app::nearestEnemyCommander(scene, army_, standing.commanderPosition);
        if (target) {
            script_.attackLaunched = true;
            for (const rm::sim::UnitId tank : standing.tanks) {
                decisions_.push_back(Decision{
                    .kind = Decision::Kind::Move,
                    .unit = tank,
                    .toX = (*target)[0],
                    .toZ = (*target)[2],
                });
            }
        }
    }
}

} // namespace rm::ai

namespace rm::app {

/// One `ScriptedOpponent` per army, each told which army it plays.
///
/// A free function rather than a loop at the call site because the human player's slot is
/// filled too: `runOpponents` skips it by army index, and a hole in the vector would make
/// every later index arithmetic instead of a lookup.
[[nodiscard]] std::vector<std::unique_ptr<rm::ai::Opponent>> makeScriptedOpponents(
    std::size_t armies) {
    std::vector<std::unique_ptr<rm::ai::Opponent>> scripts;
    scripts.reserve(armies);
    for (std::size_t army = 0; army < armies; ++army) {
        auto scripted = std::make_unique<rm::ai::ScriptedOpponent>();
        scripted->playFor(static_cast<int>(army));
        scripts.push_back(std::move(scripted));
    }
    return scripts;
}

/// Applies what one opponent decided, in the order it decided it.
///
/// THE OTHER HALF OF THE PORT. An opponent hands back data; everything that needs the VFS, the
/// buildable list or a passability grid happens here, which is what lets `Opponent` be an
/// interface a Lua adapter can implement without ever seeing a `UnitScene`.
///
/// Order is preserved exactly, and it has to be: a structure decided before a tank was pushed
/// before it, and the golden match is a per-tick hash of the result.
void applyDecisions(UnitScene& scene, const rm::vfs::Vfs& content, const rm::sim::Army& army,
                    std::span<const rm::ai::Decision> decisions, float elapsedSeconds,
                    rm::TickIndex tickIndex) {
    std::size_t ordered = 0;
    std::size_t marching = 0;

    for (const rm::ai::Decision& decision : decisions) {
        switch (decision.kind) {
        case rm::ai::Decision::Kind::StartConstruction: {
            const std::optional<std::size_t> blueprintIndex =
                resolveBuildable(scene, content, decision.blueprint);
            if (!blueprintIndex) {
                break;
            }
            const rm::unitdef::UnitDef& def = buildableDef(scene, *blueprintIndex);

            // THROUGH `applyCommand`, like every other order. The sim reads the cost, the build
            // time and the builder's own rate off the definitions, raises `ConstructionStarted`
            // itself, and refuses the order if the player does not command the builder's army.
            //
            if (!issueBuild(scene, decision.builder,
                            playerDriving(scene, army.index), tickIndex,
                            static_cast<rm::UnitTypeIndex>(*blueprintIndex), decision.site[0],
                            decision.site[2])) {
                break;  // refused deterministically — a dead builder, or one this army lost
            }
            // Structures announce themselves and tanks do not, which is what the original
            // printed: the commander's build order is the story of the opening, while a
            // factory turning out its ninth tank is noise.
            if (!def.isMobile()) {
                std::printf("  [%6.1fs] army %d starts %.*s\n",
                            static_cast<double>(elapsedSeconds), army.index,
                            static_cast<int>(decision.blueprint.size()),
                            decision.blueprint.data());
            }
            break;
        }
        case rm::ai::Decision::Kind::Move: {
            ++ordered;
            if (!scene.store.alive(decision.unit)) {
                break;  // died between the census and the order
            }
            if (issueMove(scene, decision.unit, playerDriving(scene, army.index),
                          tickIndex, decision.toX, decision.toZ)) {
                ++marching;
            }
            break;
        }
        }
    }

    // Moves only ever come from the one attack wave, so a batch of them IS the attack.
    if (ordered > 0) {
        std::printf("  [%6.1fs] army %d ATTACKS with %zu of %zu tanks\n",
                    static_cast<double>(elapsedSeconds), army.index, marching, ordered);
    }
}

/// One decision pass, for every army but the player's.
///
/// This is the port's driver (ADR-038) and it holds no opinion about what an opponent is: it
/// paces them, shows each one the world, and applies what comes back. The scripted opponent's
/// own logic lives in `rm::ai::ScriptedOpponent` above, and the pure decisions it is made of
/// stay in `core/sim/BuildOrder.hpp` where they are tested.
///
/// Run once a second rather than every tick, because nothing here changes faster than a build
/// finishes and the decisions read the whole scene.
void runOpponents(UnitScene& scene, const rm::vfs::Vfs& content, const rm::HeightField& field,
                    std::span<const rm::mapinfo::StartPosition> starts,
                    std::span<const rm::scenario::Marker> markers,
                    std::vector<std::unique_ptr<rm::ai::Opponent>>& scripts, float elapsedSeconds,
                    rm::TickIndex tickIndex,
                    const std::optional<rm::sim::PlayableRect>& playableRect) {
    // The middle of the map, in fixed point: `structureSite` and `rolloffPoint` place things
    // relative to it, and both are sim geometry now. Computed here rather than inside an
    // opponent, so no implementation of the port does map arithmetic of its own.
    const rm::sim::Fx centreX = rm::sim::Fx::fromInt(field.squaresX * rm::kSquareSize / 2);
    const rm::sim::Fx centreZ = rm::sim::Fx::fromInt(field.squaresZ * rm::kSquareSize / 2);

    // The cadence, and the STAGGER (PLAN2.md §6.7, §7 P3.6). Each opponent thinks once a
    // second, and they take their turns on different ticks of that second rather than all on
    // the same one. Each pass walks the whole scene, so eight armies used to mean eight scene
    // walks landing on one tick in ten and none on the other nine — a stutter, which is what
    // `--bench` is asked to show gone.
    //
    // This replaces `tickIndex % decisionTicks() == 0` at the call site: the same period, now
    // asked per army rather than for all of them at once.
    //
    // ONCE A SECOND, and authored in seconds: nothing an opponent reacts to changes faster
    // than a build finishes. The period kept here because it belongs to the work being paced.
    // It was `inline constexpr int kDecisionTicks = rm::sim::kTicksPerSecond` once, which is a
    // literal wearing a different hat — it names a RATE where a duration belongs, so at 20 Hz
    // the opponents thought twice a second. `check_no_tick_literals.sh` catches that shape now.
    //
    // Staggering does not create an ordering advantage, it spreads one that was already there:
    // the armies were served in array order within a single tick, so army 0 already moved
    // first. A tenth of a second between them changes when, not who.
    const rm::sim::SlowUpdate pacing{gAppTickRate, rm::sim::seconds(1.0f)};

    const rm::ai::World world{
        .scene = scene,
        .content = content,
        .field = field,
        .starts = starts,
        .markers = markers,
        .playableRect = playableRect,
        .centreX = centreX,
        .centreZ = centreZ,
    };

    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy || army.defeated
            || static_cast<std::size_t>(army.index) >= scripts.size()) {
            continue;
        }
        if (!pacing.due(static_cast<std::size_t>(army.index), tickIndex)) {
            continue;
        }
        rm::ai::Opponent& opponent = *scripts[static_cast<std::size_t>(army.index)];

        opponent.observe(world, scene.events.all());
        opponent.advance(tickIndex);
        applyDecisions(scene, content, army, opponent.drain(),
                       elapsedSeconds, tickIndex);
    }
}

[[nodiscard]] MatchRunner makeMatchRunner(UnitScene& scene, const rm::HeightField& field,
                                           PassabilitySet& passability,
                                           const rm::vfs::Vfs& content,
                                           std::span<const rm::mapinfo::StartPosition> starts,
                                            std::span<const rm::scenario::Marker> markers,
                                            std::optional<rm::sim::PlayableRect> playableRect) {
    MatchRunner runner{
        .scene = scene,
        .field = field,
        .passability = passability,
        .content = content,
        .starts = starts,
        .markers = markers,
        .scripts = makeScriptedOpponents(scene.armies.size()),
        .match =
            rm::sim::Match{
                .armies = scene.armies,
                .economies = scene.economies,
                .projectiles = &scene.projectiles,
                .building = &scene.building,
                .events = &scene.events,
                .features = &scene.features,
                .commandersEver = scene.commandersEver,
                .baseStorage = kStartingStorage,
                .intel = &scene.intel,
                .playableRect = playableRect,
                // Seeded from the scene rather than defaulted to false, using the same
                // predicate the sim decides on (Skirmish.cpp): a match with one side left
                // is already over. Two cases need it, and both are announcements that
                // would otherwise be wrong:
                //
                //   `--play` pre-runs the match headless and THEN opens the window. A
                //   runner built fresh over those armies would re-detect the end on its
                //   first tick and announce a winner the pre-run already announced.
                //
                //   A `--units` crowd has no armies at all, so `survivorCount` is zero,
                //   which is also `<= 1` — without this the interactive window would
                //   declare a DRAW on tick one of every decorative scene.
                .over = scene.armies.empty()
                        || rm::sim::survivorCount(scene.armies) <= 1,
            },
    };
    scene.grewThisTick = false;

    // `--ai-faf`: FAF opponents for every army, sharing one sandbox the runner owns. The
    // entry points and data sweeps load HERE, once — the builder registries are what the
    // opponents' brains walk. Anything short of a working sandbox falls back to the scripted
    // opponents already seated above, saying why: a match that silently plays a different
    // AI than asked is worse than one that says it could not.
    if (gFafOpponents) {
        auto sandbox = std::make_unique<rm::ai::FafAi>(rm::ai::defaultCorpus());
        if (sandbox->ready()) {
            sandbox->setLogPassthrough(gFafLog);
            // Driver BEFORE the corpus: the condition files capture engine functions out of
            // `moho.aibrain_methods` at import, so what sits there when they load is what
            // they call for the rest of the match.
            const bool driverUp = rm::ai::installFafDriver(*sandbox);
            rm::ai::importAiEntryPoints(*sandbox);
            if (driverUp) {
                for (std::size_t army = 0; army < scene.armies.size(); ++army) {
                    runner.scripts[army] = std::make_unique<rm::ai::FafOpponent>(
                        *sandbox, static_cast<int>(army));
                }
                runner.fafSandbox = std::move(sandbox);
                std::printf("faf: %zu armies seated with FAF opponents\n",
                            scene.armies.size());
            } else {
                std::printf("faf: driver failed (%s) — scripted opponents play instead\n",
                            sandbox->lastError().c_str());
            }
        } else {
            std::printf("faf: sandbox failed (%s) — scripted opponents play instead\n",
                        sandbox->lastError().c_str());
        }
    }
    return runner;
}



void printEvents(const rm::sim::EventQueue& events, float now) {
    for (const rm::sim::Event& event : events.all()) {
        std::printf("  [%6.1fs] %-21s unit %u.%u by %u.%u army %d amount %.0f at %.0f,%.0f\n",
                    static_cast<double>(now),
                    std::string{rm::sim::eventKindName(event.kind)}.c_str(),
                    event.unit.index, event.unit.generation, event.instigator.index,
                    event.instigator.generation, event.army,
                    static_cast<double>(rm::sim::magToFloat(event.amount)),
                    static_cast<double>(rm::sim::fxToFloat(event.at[0])),
                    static_cast<double>(rm::sim::fxToFloat(event.at[2])));
    }
}

rm::sim::TickReport advanceMatch(MatchRunner& runner, int tickIndex, float now) {
    UnitScene& scene = runner.scene;

    // THE SANDBOX'S HEARTBEAT, when FAF opponents play: `pump` resumes due corpus threads
    // AND advances the clock GetGameTimeSeconds reads. It was pumped only by the sanity
    // harness, so a plain --ai-faf match ran with the AI's watch stopped at zero — and
    // every time-gated builder in the corpus (the mex upgrade's GameTime > 480 among them)
    // stayed off for the whole match. Here rather than in a run mode, because both the
    // headless pre-run and the windowed loop go through this tick.
    if (runner.fafSandbox != nullptr) {
        (void)runner.fafSandbox->pump(tickIndex);
    }

    // THE TICK'S EVENTS START HERE, not inside `tickSkirmish`. The caller raises some of them
    // itself — the opponents' build orders below, and `UnitCreated` when a finished construction
    // becomes a unit — so the boundary has to be the caller's tick, which is this function.
    //
    // `beginFrame` rather than the `clear()` this used to be: the frame now carries the tick it
    // belongs to, and advancing it is idempotent, so a second caller beginning the frame it is
    // already in destroys nothing. See `Events.hpp` — that property exists because the opposite
    // once cost two event kinds that were emitted and never observable.
    scene.events.beginFrame(static_cast<rm::TickIndex>(tickIndex));

    // The routing table the sim uses to advance queued orders, refreshed for whatever types the
    // catalog now holds. `gridFor` is memoised on the LIMITS, so this is a map lookup per type
    // per tick and builds a grid only the first time a new pair of limits appears.
    runner.gridForType.clear();
    runner.gridForType.reserve(scene.catalog.size());
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        runner.gridForType.push_back(&runner.passability.gridFor(scene, type));
    }
    runner.match.passability = runner.gridForType;

    // Replay submits the exact semantic phase the original run recorded. Setup commands are
    // regenerated before the runner exists, so a matching prefix is already applied and owns
    // the same IDs; all remaining records consume their explicit source-local IDs through the
    // same intake as live producers.
    const auto submitReplayPhase = [&](rm::sim::CommandPhase phase) {
        std::vector<std::vector<rm::sim::UnitId>> expected;
        if (runner.replay == nullptr) {
            return expected;
        }
        const rm::TickIndex tick = static_cast<rm::TickIndex>(tickIndex);
        const std::span<const rm::sim::CommandIssue> due = runner.replay->at(tick, phase);
        const std::span<const rm::sim::CommandIssue> already = scene.commands.at(tick, phase);
        std::size_t matched = 0;
        for (const rm::sim::CommandIssue& recorded : due) {
            if (matched < already.size() && recorded == already[matched]) {
                ++matched;
                continue;
            }
            rm::sim::CommandIssue issue = recorded;
            if (issue.kind == rm::sim::CommandKind::Build && runner.replayPaths != nullptr) {
                const auto offset = static_cast<std::size_t>(
                    &recorded - runner.replay->all().data());
                if (offset < runner.replayPaths->size()
                    && !(*runner.replayPaths)[offset].empty()) {
                    const std::optional<std::size_t> resolved = resolveBuildable(
                        scene, runner.content, (*runner.replayPaths)[offset]);
                    if (!resolved) {
                        throw std::runtime_error{"replay build blueprint is unavailable"};
                    }
                    issue.buildType = static_cast<rm::UnitTypeIndex>(*resolved);
                }
            }
            expected.push_back(recorded.units);
            if (!submitCommand(scene, std::move(issue))) {
                throw std::runtime_error{"replay command ID diverged from live allocation"};
            }
        }
        return expected;
    };

    const auto dispatchPhase = [&](rm::sim::CommandPhase phase,
                                   const std::vector<std::vector<rm::sim::UnitId>>& expected) {
        const std::vector<DispatchedCommand> dispatched = dispatchCommands(
            scene, runner.field, runner.passability, static_cast<rm::TickIndex>(tickIndex), phase,
            &runner.pathService);
        if (runner.replay != nullptr) {
            if (dispatched.size() != expected.size()) {
                throw std::runtime_error{"replay command batch size diverged"};
            }
            for (std::size_t i = 0; i < dispatched.size(); ++i) {
                if (dispatched[i].result.accepted != expected[i]) {
                    throw std::runtime_error{"replay command accepted set diverged"};
                }
            }
        }
    };

    const auto replayPre = submitReplayPhase(rm::sim::CommandPhase::PreTick);

    // Called EVERY tick now, and the pacing lives inside — `runOpponents` asks a
    // `sim::SlowUpdate` whether each army's turn is this tick (§7 P3.6). The modulo that used
    // to be on this line was the hand-rolled version of that mechanism, and it could only ever
    // ask the question for all armies at once.
    if (!scene.armies.empty() && !runner.matchOver) {
        runOpponents(scene, runner.content, runner.field, runner.starts, runner.markers,
                       runner.scripts, now,
                       static_cast<rm::TickIndex>(tickIndex), runner.match.playableRect);
    }

    dispatchPhase(rm::sim::CommandPhase::PreTick, replayPre);

    // MatchRunner owns this state, so every tick resumes the same per-army FIFO rather than
    // constructing a service whose admissions vanish at the end of the beat.
    runner.match.pathService = &runner.pathService;

    // ONE call, and the same one both callers make. What used to be here — the order of
    // movement, collision, aiming, firing, death, defeat and economy — is a fact about
    // core/sim/Skirmish.cpp rather than about whichever loop you are reading.
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(scene.store, scene.catalog, runner.match,
                               scene.terrain(runner.field), gAppTickRate);

    runner.shotsFired += report.shotsFired;
    scene.deathBlasts += report.deathBlasts;
    scene.deathBlastDamage += report.deathBlastDamage;
    if (report.matchEnded) {
        runner.matchOver = true;
    }

    // The scorch each death leaves, sized from what died: a commander marks more ground
    // than a tank. Permanent, because a wreck IS the record of what happened here and a
    // battlefield that tidied itself up would lose it.
    //
    // The caller's work rather than the sim's: a decal buffer is a thing the renderer
    // uploads, and the sim has no business owning one.
    // FROM THE EVENTS, not from the report (§7 P6.3). The tally and the wreck marks are the
    // app's reaction to a death, and reacting to `UnitDestroyed` rather than to
    // `TickReport::died` is what the event queue is for — the caller stops needing a
    // second, differently-shaped channel for the same fact. `report.died` is still what the
    // sim hands back for the callers that need a death's radius, and both agree by
    // construction: `retireDead` fills one from the other.
    for (const rm::sim::Event& event : scene.events.all()) {
        if (event.kind == rm::sim::EventKind::UnitDestroyed) {
            ++runner.unitsDestroyed;
        }
    }
    refreshWreckDecals(scene, runner.field);

    // What finished this tick BECOMES A UNIT: an extractor that is done stands on its
    // deposit, a factory stands by the base, and a tank rolls off the factory floor.
    //
    // The caller's work, and it has to be: a finished construction turns into a model out
    // of the VFS, which is the one thing the sim cannot do for itself.
    for (const rm::sim::Construction& work : report.finished) {
        const auto army = static_cast<std::size_t>(work.armyIndex);
        if (army >= scene.armies.size()) {
            continue;
        }
        ++runner.completedBuilds;
        // AN UPGRADE REPLACES: the old unit leaves before the new one stands, and it
        // leaves without a wreck — a factory becoming its T2 self did not die. Straight
        // through the store: no death report means no debris and no defeat accounting,
        // both of which are for units the war removed.
        if (work.isUpgrade() && scene.store.alive(work.upgradeOf)) {
            scene.store.kill(work.upgradeOf);
        }
        // OUT of fixed point, here at the edge (§7 P10.0). Putting a unit on the map needs
        // a model and a float transform, so this is the legitimate direction — the sim
        // holds the authority and the renderer gets a copy, never the other way round.
        const std::array<float, 3> site{rm::sim::fxToFloat(work.position[0]),
                                        rm::sim::fxToFloat(work.position[1]),
                                        rm::sim::fxToFloat(work.position[2])};
        // Face the map centre — a base laid out toward the fight reads as one. The bearing
        // is computed in FIXED POINT (`fxBearing`, the same CORDIC the sim's own aiming
        // uses), not `std::atan2`: this yaw becomes `Transform.heading`, which the state
        // hash covers, and libm's atan2 is exactly where two architectures disagree in the
        // last ulp. The old float path was a cross-machine desync waiting on every
        // factory roll-off.
        const rm::Brad yaw = rm::sim::fxBearing(
            rm::sim::fxFromFloat(runner.field.widthElmos() * 0.5f) - work.position[0],
            rm::sim::fxFromFloat(runner.field.depthElmos() * 0.5f) - work.position[2]);
        const auto spawned = spawnUnit(scene, runner.content, runner.field,
                                       std::string{scene.pathOf(static_cast<rm::UnitTypeIndex>(work.blueprintIndex))},
                                       site, scene.armies[army], yaw);
        // `UnitFinished` after `UnitCreated`, which `spawnUnit` raised: the pair is Recoil's
        // (`04 §4.2` — `UnitCreated` then `UnitFinished` when the build completes) and the
        // distinction matters to anything that treats a built unit differently from one placed
        // at match start. Both come from the caller, because both need the model to exist.
        if (spawned) {
            scene.events.emit(rm::sim::Event{
                .kind = rm::sim::EventKind::UnitFinished,
                .unit = *spawned,
                .army = work.armyIndex,
                .amount = work.cost.mass,
                .at = work.position,
            });
        }
        // The roll-off STANDS DOWN in a replay: its move was recorded by the original
        // run and replays from the log — issuing it here as well would order it twice.
        if (spawned && runner.replay == nullptr
            && buildableDef(scene, work.blueprintIndex).isMobile()) {
            // Off the factory floor: straight to the fight once the wave has gone, to the
            // rally point outside the base while it forms.
            const auto target = army < runner.scripts.size()
                                        && runner.scripts[army]->attackLaunched()
                                    ? nearestEnemyCommander(scene, work.armyIndex,
                                                            work.position)
                                    : std::nullopt;
            std::array<rm::sim::Fx, 2> to =
                target ? std::array<rm::sim::Fx, 2>{(*target)[0], (*target)[2]}
                       : rm::sim::rolloffPoint(
                             work.position,
                             rm::sim::Fx::fromInt(runner.field.squaresX * rm::kSquareSize / 2),
                             rm::sim::Fx::fromInt(runner.field.squaresZ * rm::kSquareSize
                                                  / 2));
            if (scene.store.motion()[spawned->index].surfaceWater) {
                const auto type = static_cast<std::size_t>(scene.store.typeAt(spawned->index));
                const rm::sim::PassabilityGrid& grid = runner.passability.gridFor(scene, type);
                if (const auto waterTarget = rm::sim::reachablePointToward(
                        grid, work.position[0], work.position[2], to[0], to[1])) {
                    to = *waterTarget;
                }
            }
            (void)issueMove(scene, *spawned, playerDriving(scene, work.armyIndex),
                            static_cast<rm::TickIndex>(tickIndex), to[0], to[1], false,
                            rm::sim::CommandKind::Move, rm::sim::CommandPhase::PostSpawn);
        }
    }

    const auto replayPost = submitReplayPhase(rm::sim::CommandPhase::PostSpawn);
    dispatchPhase(rm::sim::CommandPhase::PostSpawn, replayPost);

    // Nothing to rebuild any more. This used to re-point a vector of per-batch spans,
    // because a spawn into an existing batch reallocated the vector it landed in and left
    // every span into it dangling — a segfault for the first reader that looked straight
    // after the call. The store hands out spans on demand instead, so a grown array is
    // simply a longer span next time somebody asks.
    scene.grewThisTick = false;

    // THE SNAPSHOT (§7 P7.1), last thing in the tick — after the spawns, so a unit that
    // appeared this tick is in it. `publish` rotates: what was current becomes previous, and
    // the frame loop draws between the two.
    scene.publish(static_cast<rm::TickIndex>(tickIndex) + 1);

    // LAST, so the dump is the whole tick: the sim's own events and then the caller's
    // `UnitCreated`/`UnitFinished`, in the order they happened. Printed before the caller-side
    // work at first, which meant the two kinds only the caller can raise were emitted after the
    // print and cleared by the next tick — declared, emitted, and never once observable.
    if (gPrintEvents) {
        printEvents(scene.events, now);
    }

    return report;
}

void march(UnitScene& scene, const rm::HeightField& field, PassabilitySet& passability,
           const MarchOptions& options, std::span<const rm::AmbientEmitter> ambient,
           std::vector<rm::Particle>& dust, const rm::vfs::Vfs& content,
           std::span<const rm::mapinfo::StartPosition> starts,
           std::span<const rm::scenario::Marker> markers) {
    std::size_t routed = 0;
    std::size_t total = 0;
    std::vector<bool> announced(scene.armies.size(), false);
    // `--march` sends everything at one point; `--play` lets the match decide.
    if (options.orderAll) {
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            {
                ++total;
                if (issueMove(scene, scene.store.idAt(slot),
                              playerDriving(scene, scene.store.motion()[slot].armyIndex), 0,
                              rm::sim::fxFromFloat(options.x),
                              rm::sim::fxFromFloat(options.z))) {
                    ++routed;
                }
            }
        }
        (void)dispatchCommands(scene, field, passability, 0, rm::sim::CommandPhase::PreTick);
    }

    // Whole ticks from a duration, rather than feeding a wall clock: this has
    // to land on exactly the same state every run.
    // Through the RUN'S rate, not the default one. This read `kTicksPerSecond` and so ran a
    // 520-second `--play` for 5200 ticks whatever `--tick-rate` said — at 20 Hz that is 260
    // seconds of match, which is why nothing was built and nobody fired. Exactly the bug
    // §5.1 exists to prevent, found by running the four rates rather than by reading.
    const auto ticks =
        static_cast<int>(gAppTickRate.ticks(rm::sim::seconds(options.seconds)));

    // The caller-side tick, shared with the windowed frame loop. See MatchRunner.
    MatchRunner runner =
        makeMatchRunner(scene, field, passability, content, starts, markers,
                        rm::sim::PlayableRect{
                            .minX = {},
                            .maxX = rm::sim::fxFromFloat(field.widthElmos()),
                            .minZ = {},
                            .maxZ = rm::sim::fxFromFloat(field.depthElmos()),
                        });

    // Per-tick state hashes, kept when this run has been asked to record or check them.
    // Reserved up front so the recording cannot itself perturb what it measures by
    // reallocating mid-match.
    const bool hashing = !options.hashLogPath.empty() || !options.checkHashLogPath.empty();
    std::vector<rm::StateHash> hashes;
    if (hashing) {
        hashes.reserve(static_cast<std::size_t>(ticks));
    }

    const float kTickSeconds = gAppTickRate.secondsPerTick();
    float dustDebt = 0.0f;
    float ambientDebt = 0.0f;
    std::uint32_t dustSeed = 0x51ED27u;
    std::vector<rm::DustEmitter> emitters;

    // `--ai-sanity`: profile the sandbox the FAF opponents actually use. When no live sandbox
    // exists — either none was requested or its boot failed — a standalone one is pumped on the
    // match clock instead; either way, what got built is tallied for the report.
    // `--replay-commands`: the log drives, nothing thinks. Loaded before the loop so a
    // bad path aborts the run instead of replaying half a match; scripts emptied so the
    // opponents' slot in the tick belongs to the log's own commands.
    std::optional<rm::sim::CommandLog> replayLog;
    std::vector<std::string> replayPaths;
    if (!options.replayCommandsPath.empty()) {
        replayLog = rm::sim::readCommandLog(options.replayCommandsPath, &replayPaths);
        if (!replayLog) {
            std::printf("replay: cannot read %s — refusing to run half a match\n",
                        options.replayCommandsPath.c_str());
            return;
        }
        runner.scripts.clear();
        runner.replay = &*replayLog;
        runner.replayPaths = &replayPaths;
        std::printf("replay: %zu command(s) from %s, driving to tick %llu\n",
                    replayLog->size(), options.replayCommandsPath.c_str(),
                    replayLog->lastTick());
    }

    rm::ai::FafAi* sanity = nullptr;
    std::unique_ptr<rm::ai::FafAi> ownSanity;
    std::map<rm::UnitTypeIndex, std::size_t> builtByType;
    if (options.aiSanity) {
        if (runner.fafSandbox != nullptr) {
            // `--ai-faf` already booted the sandbox the opponents play in — measure THAT
            // one. Its corpus loaded before this line, so the profile starts at the first
            // decision pass: with a live opponent the runtime code IS the report.
            sanity = runner.fafSandbox.get();
            sanity->setProfiling(true);
        } else {
            ownSanity = std::make_unique<rm::ai::FafAi>(rm::ai::defaultCorpus());
            if (ownSanity->ready()) {
                // Profiler BEFORE the entry points: with nothing driving an army, all the
                // corpus runs is load-time code, and a profile that starts after the load
                // reports ninety-five modules of running code as silence.
                ownSanity->setProfiling(true);
                rm::ai::importAiEntryPoints(*ownSanity);
                sanity = ownSanity.get();
            } else {
                std::printf("ai-sanity: %s\n", ownSanity->lastError().c_str());
                ownSanity.reset();
            }
        }
    }

    for (int i = 0; i < ticks; ++i) {
        const float now = static_cast<float>(i) * kTickSeconds;

        // ONE call, and the same one the windowed frame loop makes: the opponents'
        // decisions, the sim's tick, the wrecks, and the finished builds that become
        // units. See MatchRunner for what is deliberately left to each caller.
        const rm::sim::TickReport report = advanceMatch(runner, i, now);

        // Fingerprinted after the WHOLE caller-side tick, which deliberately includes the
        // units a finished construction spawned: what got built is part of what the match
        // did, and a hash that stopped at the sim's own passes would agree about two runs
        // that built different things. The renderer's side stays out on its own — the decal
        // buffer is not reachable from a group or a match, so `hashMatch` cannot see it.
        if (hashing) {
            hashes.push_back(rm::sim::hashMatch(scene.store, runner.match));
        }

        if (!scene.armies.empty()) {

            // What the tick decided, announced. The decisions themselves are the sim's;
            // saying them out loud is this run's, which is why only the printing is left.
            for (const rm::sim::Army& army : scene.armies) {
                if (army.defeated && !announced[static_cast<std::size_t>(army.index)]) {
                    announced[static_cast<std::size_t>(army.index)] = true;
                    std::printf("  [%6.1fs] army %d (%s) has lost its commander\n",
                                static_cast<double>(now), army.index,
                                std::string{rm::sim::factionName(army.faction)}.c_str());
                }
            }
            if (report.matchEnded) {
                if (report.winner) {
                    std::printf("  [%6.1fs] team %d WINS\n", static_cast<double>(now),
                                *report.winner);
                } else {
                    std::printf("  [%6.1fs] a DRAW: every army lost its commander\n",
                                static_cast<double>(now));
                }
            }
        }

        // What finished this tick, narrated. The spawning itself is `advanceMatch`'s, so
        // that the windowed loop gets the buildings too; only saying so is this run's.
        for (const rm::sim::Construction& work : report.finished) {
            const auto army = static_cast<std::size_t>(work.armyIndex);
            if (army >= scene.armies.size()) {
                continue;
            }
            std::printf("  [%6.1fs] army %zu completes %s\n", static_cast<double>(now), army,
                        buildableDef(scene, work.blueprintIndex).name.c_str());
            if (sanity != nullptr) {
                ++builtByType[static_cast<rm::UnitTypeIndex>(work.blueprintIndex)];
            }
        }

        // Only the harness's OWN sandbox pumps here — the opponents' shared one beats
        // inside advanceMatch, and a second pump per tick would be a second heart.
        if (ownSanity != nullptr) {
            (void)ownSanity->pump(i);
        }

        // Dust as the walk happens, aged as the walk continues, so what a capture
        // shows is a trail rather than a puff at everyone's feet.
        emitters.clear();
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::sim::MoveState& motion = scene.store.motion()[slot];
            emitters.push_back(rm::DustEmitter{
                .position = {rm::sim::fxToFloat(scene.store.transforms()[slot].x),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].y),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].z)},
                .moving = motion.moving,
                .topSpeedElmosPerSecond = rm::sim::fxToFloat(motion.speedPerTick)
                                          * static_cast<float>(
                                              gAppTickRate.ticksPerSecond()),
                .radiusElmos = rm::sim::fxToFloat(motion.radiusElmos),
            });
        }
        rm::advanceParticles(dust, kTickSeconds);
        rm::emitDust(dust, emitters, field, kTickSeconds, dustDebt, dustSeed);
        rm::emitAmbient(dust, ambient, kTickSeconds, ambientDebt, dustSeed);
    }

    // A marched scene has been walked, so its walk cycles are paced by the
    // ground covered — the same rule the windowed path follows. This is also
    // what makes such a screenshot independent of `--time`: the sim decided
    // where the legs are, not the clock.
    // The walk cycles, paced from the snapshot, then gathered for drawing. Per TYPE because the
    // animation belongs to the model: every unit of one type shares its clock length, and the
    // pass writes each unit's own phase from the ground it has covered.
    //
    // Alpha 1 explicitly: a marched scene is a capture of one tick, and a blend of two would
    // make the screenshot depend on where the pre-run happened to stop.
    scene.publish(0);
    scene.gatherForDrawing(1.0f);

    // --- The sanity report ---------------------------------------------------
    //
    // Three answers, one block: what the match BUILT (the tech question — a T1-only tally
    // means the opponent has no tech-up, whoever is driving), what the AI corpus asked the
    // ENGINE for (the bindings work queue), and which of the corpus's OWN functions ran
    // (proof the AI's code executes, not merely loads).
    if (sanity != nullptr) {
        std::printf("\n=== AI SANITY ========================================================\n");

        std::size_t totalBuilt = 0;
        for (const auto& [type, count] : builtByType) {
            totalBuilt += count;
        }
        std::printf("  BUILT  %zu unit(s) across %zu type(s):\n", totalBuilt,
                    builtByType.size());
        std::vector<std::pair<rm::UnitTypeIndex, std::size_t>> ranked{builtByType.begin(),
                                                                      builtByType.end()};
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [type, count] : ranked) {
            std::printf("    %6zu  %s\n", count, buildableDef(scene, type).name.c_str());
        }

        // DISTINCT FILES, not load events. `modules()` records every attempt: a missing file
        // is recorded again on every import (nothing to cache), and a cyclic import records —
        // and re-runs — a file that is still mid-load. Counting events reported "95 executed"
        // for what turned out to be far fewer files, and a sanity report that disagrees with
        // its own coverage line is worse than no report. Executed wins over Failed over
        // Missing when one path saw several outcomes.
        const auto foldedPath = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        const auto outcomeRank = [](rm::ai::LoadOutcome outcome) {
            switch (outcome) {
            case rm::ai::LoadOutcome::Executed: return 2;
            case rm::ai::LoadOutcome::Failed: return 1;
            case rm::ai::LoadOutcome::Missing: return 0;
            }
            return 0;
        };
        std::map<std::string, rm::ai::LoadOutcome> outcomes;
        for (const rm::ai::ModuleLoad& module : sanity->modules()) {
            auto [it, fresh] = outcomes.try_emplace(foldedPath(module.path), module.outcome);
            if (!fresh && outcomeRank(module.outcome) > outcomeRank(it->second)) {
                it->second = module.outcome;
            }
        }
        std::size_t executed = 0;
        std::size_t missing = 0;
        std::size_t failed = 0;
        for (const auto& [path, outcome] : outcomes) {
            switch (outcome) {
            case rm::ai::LoadOutcome::Executed: ++executed; break;
            case rm::ai::LoadOutcome::Missing: ++missing; break;
            case rm::ai::LoadOutcome::Failed: ++failed; break;
            }
        }
        std::printf("  SANDBOX  %zu distinct modules executed, %zu missing (not vendored),"
                    " %zu failed; %zu thread(s) alive, %zu thread error(s)\n",
                    executed, missing, failed, sanity->threadsAlive(),
                    sanity->threadErrors().size());
        std::size_t shown = 0;
        for (const std::string& error : sanity->threadErrors()) {
            std::printf("    thread died: %s\n", error.c_str());
            if (++shown == 5) {
                break;
            }
        }
        // Name what failed, not just how many — a count nobody can act on is the silent
        // failure ADR-039 exists to avoid. Distinct paths only; a file that failed once is
        // re-imported by half the corpus.
        std::set<std::string> failedNamed;
        for (const rm::ai::ModuleLoad& module : sanity->modules()) {
            if (module.outcome == rm::ai::LoadOutcome::Failed
                && failedNamed.insert(foldedPath(module.path)).second
                && failedNamed.size() <= 5) {
                std::printf("    failed: %-40s %s\n", module.path.c_str(),
                            module.error.c_str());
            }
        }

        std::printf("  ENGINE BINDINGS CALLED (top 10 of the AI's asks):\n");
        shown = 0;
        for (const rm::ai::Binding& binding : sanity->report()) {
            if (binding.calls == 0) {
                break;  // the report is calls-first; zeroes are the tail
            }
            std::printf("    %8zu  %-28s %s\n", binding.calls, binding.name.c_str(),
                        std::string{rm::ai::fidelityName(binding.fidelity)}.c_str());
            if (++shown == 10) {
                break;
            }
        }

        std::printf("  AI CODE EXECUTED (top 15 corpus functions):\n");
        shown = 0;
        for (const auto& [where, count] : sanity->callProfile()) {
            std::printf("    %8zu  %s\n", count, where.c_str());
            if (++shown == 15) {
                break;
            }
        }
        if (sanity->callProfile().empty()) {
            std::printf("    (none ran — nothing forks threads at load, and no opponent"
                        " bridge drives a brain yet)\n");
        }

        // The opponent bridge's own ledgers: brain methods conditions wanted and did not
        // find (fail-closed, so each line is a builder family silently switched off), and
        // errors raised INSIDE condition code. Empty when no FAF opponent played.
        const std::vector<std::string> missingMethods =
            rm::ai::fafMissingBrainMethods(*sanity);
        if (!missingMethods.empty()) {
            std::printf("  BRAIN METHODS MISSING (conditions fail closed without them):\n");
            shown = 0;
            for (const std::string& line : missingMethods) {
                std::printf("    %s\n", line.c_str());
                if (++shown == 10) {
                    break;
                }
            }
        }
        const std::vector<std::string> conditionErrors = rm::ai::fafConditionErrors(*sanity);
        const std::string conditionErrorReport =
            rm::ai::formatFafConditionErrorReport(conditionErrors);
        std::printf("%s", conditionErrorReport.c_str());

        // Coverage from the other side: not "what did the imports miss" but "what sits in
        // the vendored tree that NOTHING imported". Missing modules above are files we chose
        // not to fetch; these are files we DID fetch that no import path reaches — either
        // dead vendoring or an entry point the sanity run does not load yet. FAF paths are
        // case-insensitive, so the comparison is too.
        const auto lowered = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::set<std::string> loaded;
        for (const rm::ai::ModuleLoad& module : sanity->modules()) {
            if (module.outcome == rm::ai::LoadOutcome::Executed) {
                loaded.insert(lowered(module.path));
            }
        }
        std::vector<std::string> neverLoaded;
        std::size_t vendored = 0;
        const std::filesystem::path corpus = rm::ai::defaultCorpus();
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(corpus, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".lua") {
                continue;
            }
            const std::string rel =
                "/" + entry.path().lexically_relative(corpus).generic_string();
            // `/engine/` is the annotation stubs the binding generator reads — documentation
            // of Moho's surface, not code the corpus ever imports. Counting them as "unused
            // AI code" would bury the real answer under seventy-eight files that were never
            // going to load.
            if (rel.starts_with("/engine/")) {
                continue;
            }
            ++vendored;
            if (!loaded.contains(lowered(rel))) {
                neverLoaded.push_back(rel);
            }
        }
        std::sort(neverLoaded.begin(), neverLoaded.end());
        std::printf("  COVERAGE  %zu of %zu vendored files executed", vendored - neverLoaded.size(),
                    vendored);
        if (neverLoaded.empty()) {
            std::printf(" — the whole vendored corpus is in use\n");
        } else {
            std::printf("; never loaded:\n");
            shown = 0;
            for (const std::string& path : neverLoaded) {
                std::printf("    %s\n", path.c_str());
                if (++shown == 12 && neverLoaded.size() > 12) {
                    std::printf("    ... and %zu more\n", neverLoaded.size() - shown);
                    break;
                }
            }
        }
        std::printf("======================================================================\n\n");
    }

    // --- Determinism ---------------------------------------------------------
    //
    // Written before the summaries below, so that a run whose whole purpose was the
    // artifact says whether it got one before saying anything else.
    if (!options.commandLogPath.empty()) {
        const auto blueprintOf = [&scene](std::uint32_t type) {
            return std::string{scene.pathOf(static_cast<rm::UnitTypeIndex>(type))};
        };
        if (rm::sim::writeCommandLog(scene.commands, options.commandLogPath, blueprintOf)) {
            std::printf("commands: %zu order(s) over %llu tick(s) written to %s\n",
                        scene.commands.size(),
                        static_cast<unsigned long long>(scene.commands.lastTick()),
                        options.commandLogPath.c_str());
        } else {
            std::printf("commands: cannot write %s\n", options.commandLogPath.c_str());
        }
    }

    if (hashing) {
        rm::sim::ReplayHeader header;
        header.ticksPerSecond = static_cast<int>(gAppTickRate.ticksPerSecond());
        header.widthsFingerprint = rm::sim::widthsFingerprint();
        header.tickCount = hashes.size();

        if (!options.checkHashLogPath.empty()) {
            const auto recorded = rm::sim::readHashLog(options.checkHashLogPath);
            if (!recorded) {
                std::printf("determinism: cannot read %s — %s\n",
                            options.checkHashLogPath.c_str(),
                            recorded.error().message.c_str());
            } else {
                const rm::sim::Divergence heads =
                    rm::sim::compareHeaders(recorded->header, header);
                if (heads.incomparable) {
                    std::printf("determinism: NOT COMPARABLE — %s\n", heads.why.c_str());
                } else {
                    const rm::sim::Divergence d =
                        rm::sim::compareHashes(recorded->hashes, hashes);
                    if (!d.diverged) {
                        std::printf("determinism: MATCH — %zu ticks identical to %s\n",
                                    hashes.size(), options.checkHashLogPath.c_str());
                    } else {
                        // The first divergent tick, which is the only one with diagnostic
                        // value: every later tick is downstream of it.
                        std::printf("determinism: DIVERGED at tick %llu —"
                                    " recorded %016llx, this run %016llx\n",
                                    static_cast<unsigned long long>(d.tick),
                                    static_cast<unsigned long long>(d.recorded),
                                    static_cast<unsigned long long>(d.replayed));
                        if (!d.why.empty()) {
                            std::printf("            %s\n", d.why.c_str());
                        }
                        std::printf("            re-run with --hash-log to capture this"
                                    " run, then diff the two files at that line\n");
                    }
                }
            }
        }

        if (!options.hashLogPath.empty()) {
            const auto written =
                rm::sim::writeHashLog(options.hashLogPath, header, hashes);
            if (written) {
                std::printf("determinism: %zu tick hashes written to %s\n", hashes.size(),
                            options.hashLogPath.c_str());
            } else {
                std::printf("determinism: cannot write %s — %s\n",
                            options.hashLogPath.c_str(), written.error().message.c_str());
            }
        }
    }

    // Saying how many found a route matters on a map like aw04, where most
    // units are on islands: a unit that cannot walk there stays put, and
    // without this line that reads as the sim being broken.
    if (options.orderAll) {
        std::printf("march: %zu of %zu units routed to (%.0f, %.0f), %d ticks simulated,"
                    " %zu dust particles still in the air\n",
                    routed, total, static_cast<double>(options.x),
                    static_cast<double>(options.z), ticks, dust.size());
    } else {
        std::printf("play: %d ticks simulated, %zu dust particles still in the air\n", ticks,
                    dust.size());
    }

    if (!scene.armies.empty()) {
        // What the fight came to. Reported rather than inferred from a screenshot,
        // because a unit that died is a unit that is no longer in the frame and its
        // absence looks the same as its never having been there.
        rm::sim::Mag remaining{};
        rm::sim::Mag maximum{};
        for (const rm::sim::Health& one : scene.store.health()) {
            remaining += one.current;
            maximum += one.maximum;
        }

        std::printf("combat: %zu shots fired, %zu in flight, %zu unit(s) destroyed,"
                    " %.0f of %.0f hp left\n",
                    runner.shotsFired, scene.projectiles.size(), runner.unitsDestroyed,
                    static_cast<double>(rm::sim::magToFloat(remaining)),
                    static_cast<double>(rm::sim::magToFloat(maximum)));

        // Each commander's state, because the match hangs on exactly these numbers
        // and "the fight is still on" and "the fight never reached anyone" read the
        // same from the aggregate.
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
            if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
                continue;
            }
            std::printf("  army %d commander: %.0f of %.0f hp\n",
                        scene.store.motion()[slot].armyIndex,
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].current)),
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].maximum)));
        }
        if (scene.deathBlasts > 0 || scene.features.size() > 0) {
            // Counted from the FEATURES now, not by dividing a vertex buffer by the vertices
            // per mark (§7 P6.2). The old form was the tell that the decal buffer was the only
            // record: to say how many things had died you had to do arithmetic on triangles.
            std::printf("wreckage: %zu wreck(s), %zu death explosion(s) dealing %.0f damage\n",
                        scene.features.size(), scene.deathBlasts,
                        static_cast<double>(rm::sim::magToFloat(scene.deathBlastDamage)));
        }
    }

    if (!scene.economies.empty()) {
        const rm::sim::Economy& first = scene.economies.front();
        std::printf("economy: %zu of %zu build(s) complete; army 0 holds %.0f mass /"
                    " %.0f energy, earning %.1f / %.1f a second against %.1f energy of"
                    " upkeep, %.0f%% funded\n",
                    runner.completedBuilds, scene.building.size(),
                    static_cast<double>(rm::sim::magToFloat(first.stored.mass)),
                    static_cast<double>(rm::sim::magToFloat(first.stored.energy)),
                    // Reported PER SECOND, which is what a reader wants, converted back from
                    // the per-tick figure the sim keeps.
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.mass))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.energy))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.upkeepPerTick.energy))
                        * gAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::fxToFloat(first.fundedFraction)) * 100.0);
    }
}

} // namespace rm::app
