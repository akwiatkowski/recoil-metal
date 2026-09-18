// Player-perspective coverage for terrain claims (see docs/fa-exe-analysis-plan.md).
//
// WP-37's mutable-terrain claims: wrecks as non-obstacles (C-287), scorch as a
// visual-only write (C-291), and the water gate on impact marks (C-292). The
// claims that name machinery this engine does not have — FlattenMapRect
// (C-286), the terrain-type blocking LUT (C-288), SetTerrainTypeRect (C-289) —
// are reported as unimplemented rather than tested.

#include "app/Match.hpp"
#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace {

/// A flat field of `squares` squares, every corner at `height` elmos.
[[nodiscard]] rm::HeightField flatField(int squares, float height) {
    rm::HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = height;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// The cell index under a world position, for assertions about one cell.
[[nodiscard]] int cellOf(const rm::sim::PassabilityGrid& grid, float x, float z) {
    return grid.cellAtWorld(rm::test::fx(z)) * grid.cellsX
           + grid.cellAtWorld(rm::test::fx(x));
}

/// A scene wired the way `advanceMatch` needs it: two armies, one human player,
/// and the water plane the caller states.
[[nodiscard]] rm::app::UnitScene makeScene(bool water, float level) {
    rm::app::UnitScene scene;
    scene.hasWater = water;
    scene.waterLevelElmos = level;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.playerArmy = 0;
    scene.economies.resize(2);
    scene.commandersEver.assign(2, 0);
    return scene;
}

/// The first event of a kind in the queue, or nullptr — `all().front()` would
/// assert on whichever event happened to be emitted first, not the one asked for.
[[nodiscard]] const rm::sim::Event* firstOf(const rm::sim::EventQueue& events,
                                            rm::sim::EventKind kind) {
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == kind) {
            return &event;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("C-287: a wreck does not block the ground it died on", "[fa-terrain]") {
    // Retail's wrecks are footprint-less props: `DefaultWreckage_prop.bp`
    // carries no Footprint, so a death writes nothing to the occupancy grid.
    // Ours reaches the same outcome by omission — `blockingCells` reads the
    // unit store only — and this pins it at the observable seam: the blocking
    // layer the path service is handed.
    const rm::HeightField field = flatField(128, 0.0f);
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef structure;
    structure.name = "test_structure";
    structure.wreckMass = rm::test::mag(100.0f);
    const rm::UnitTypeIndex type = roster.addType(structure);
    const rm::sim::UnitId building = roster.add(type, 512.0f, 200.0f, 0, 500.0f);
    // A building: it cannot move, and it is big enough to claim its cell.
    roster.motion(building).speedPerTick = rm::sim::Fx{};
    roster.motion(building).radiusElmos = rm::test::fx(40.0f);

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    rm::sim::FeatureStore features;
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .features = &features,
                         .commandersEver = commandersEver};
    const rm::sim::Terrain terrain{field};

    // While it stands, the structure claims the cells under it — the ordinary
    // blocking behaviour the wreck must NOT inherit.
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    const int site = cellOf(grid, 512.0f, 200.0f);
    REQUIRE(rm::sim::blockingCells(roster.store, grid)[static_cast<std::size_t>(site)]
            == 1);

    // It dies. The wreck feature exists — reclaim and the scorch decal both
    // read it — but the ground under it is free again.
    roster.health(building).current = rm::sim::Mag{};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(features.size() == 1);

    const std::vector<std::uint8_t> blocked =
        rm::sim::blockingCells(roster.store, grid);
    CHECK(std::ranges::none_of(blocked, [](std::uint8_t cell) { return cell != 0; }));

    // And a route asked for across the corpse's cell goes THROUGH it: the
    // flow field the path service would build over this blocking layer walks
    // the straight corridor rather than detouring around a grave.
    const auto shared = std::make_shared<rm::sim::PassabilityGrid>(grid);
    rm::sim::FlowField flow(shared, grid.cellAtWorld(rm::test::fx(900.0f)),
                            grid.cellAtWorld(rm::test::fx(200.0f)), blocked);
    const int start = cellOf(grid, 200.0f, 200.0f);
    const int starts[]{start};
    for (int i = 0; i < 64 && flow.reach(start) == rm::sim::FlowField::Reach::Pending;
         ++i) {
        flow.stepUntil(starts, 1000);
    }
    REQUIRE(flow.reach(start) == rm::sim::FlowField::Reach::Reached);
    const auto route = flow.route(rm::test::fx(200.0f), rm::test::fx(200.0f),
                                  rm::test::fx(900.0f), rm::test::fx(200.0f));
    REQUIRE_FALSE(route.empty());
    const bool throughWreck = std::ranges::any_of(route, [&](const auto& point) {
        return cellOf(grid, rm::test::asFloat(point[0]), rm::test::asFloat(point[1]))
               == site;
    });
    CHECK(throughWreck);
}

TEST_CASE("C-291: an impact mark is a decal write the sim cannot see", "[fa-terrain]") {
    // `CreateSplat`/`CreateDecal` write only decal-buffer state — no
    // heightfield, occupancy or pathing access. The observable form of that
    // here: noting a mark changes nothing the sim owns. The feature store is
    // the sim's record of what is on the ground, and the hash is the sim's
    // whole state; both must be identical before and after the write.
    rm::app::UnitScene scene = makeScene(false, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef tank;
    tank.name = "test_tank";
    const rm::UnitTypeIndex type = roster.addType(tank);
    (void)roster.add(type, 200.0f, 200.0f, 0, 500.0f);

    rm::sim::FeatureStore features;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .features = &features,
                         .commandersEver = commandersEver};
    const rm::StateHash before = rm::sim::hashMatch(roster.store, match);

    rm::app::noteImpactMark(scene, 300.0f, 300.0f);
    REQUIRE(scene.impactMarks.size() == 1);

    CHECK(features.size() == 0);
    CHECK(rm::sim::hashMatch(roster.store, match) == before);
}

TEST_CASE("C-292: a shot into the sea leaves no scorch", "[fa-terrain]") {
    // The gate the claim describes — scorch on `targetType=='Terrain'|'Prop'`,
    // nothing for water — lives where the app consumes `ProjectileImpact`
    // events (Match.cpp): only Terrain and Prop impacts call `noteImpactMark`.
    // Driven end to end: a projectile that expires under the water plane
    // resolves as `Underwater` and marks nothing; one that reaches dry ground
    // resolves as `Terrain` and marks it.
    rm::HeightField field = flatField(128, 0.0f);

    SECTION("underwater expiry") {
        rm::app::UnitScene scene = makeScene(true, 32.0f);
        rm::app::PassabilitySet passability{field, true, 32.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();

        // One tick of lifetime, flying level at y = 10 — under the 32-elmo
        // water plane, over the seabed at 0. It runs out submerged: an
        // `Underwater` impact, which the scorch gate ignores.
        scene.projectiles.push_back(rm::sim::Projectile{
            .position = rm::test::at(500, 10, 500),
            .velocity = rm::test::at(0, 0, 10),
            .firedByArmy = 0,
            .ticksRemaining = 1,
        });
        (void)rm::app::advanceMatch(runner, 0, 0.0f);
        // C-173: the contact is detected this tick and DELIVERED at the start of
        // the next — the event lands on the second advance.
        (void)rm::app::advanceMatch(runner, 1, 0.0f);

        REQUIRE(scene.events.count(rm::sim::EventKind::ProjectileImpact) == 1);
        const rm::sim::Event* impact =
            firstOf(scene.events, rm::sim::EventKind::ProjectileImpact);
        REQUIRE(impact != nullptr);
        CHECK(impact->impactType == rm::sim::ImpactType::Underwater);
        CHECK(scene.impactMarks.empty());
    }

    SECTION("dry ground impact") {
        rm::app::UnitScene scene = makeScene(false, 0.0f);
        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();

        // The same one-tick shot, falling onto dry ground: a `Terrain` impact,
        // which is the mark's whole reason to exist.
        scene.projectiles.push_back(rm::sim::Projectile{
            .position = rm::test::at(500, 20, 500),
            .velocity = rm::test::at(0, -40, 0),
            .firedByArmy = 0,
            .ticksRemaining = 1,
        });
        (void)rm::app::advanceMatch(runner, 0, 0.0f);
        // C-173: the contact is detected this tick and DELIVERED at the start of
        // the next — the event lands on the second advance.
        (void)rm::app::advanceMatch(runner, 1, 0.0f);

        REQUIRE(scene.events.count(rm::sim::EventKind::ProjectileImpact) == 1);
        const rm::sim::Event* impact =
            firstOf(scene.events, rm::sim::EventKind::ProjectileImpact);
        REQUIRE(impact != nullptr);
        CHECK(impact->impactType == rm::sim::ImpactType::Terrain);
        CHECK(scene.impactMarks.size() == 1);
    }

    SECTION("a unit hit marks the victim, not the ground") {
        rm::app::UnitScene scene = makeScene(false, 0.0f);
        rm::unitdef::UnitDef tank;
        tank.name = "test_tank";
        scene.definitions.push_back(tank);
        const rm::UnitTypeIndex type =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);
        const rm::sim::UnitId victim = scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = {.x = rm::test::fx(500.0f), .z = rm::test::fx(500.0f)},
            .motion = rm::app::motionFor(tank, 1),
            .health = rm::sim::initialHealth(rm::test::mag(100.0f)),
        });
        // A hittable body: the bare def derives no collision radius, and a
        // zero-radius unit is not a body the sweep can strike.
        scene.store.motion()[victim.index].radiusElmos = rm::test::fx(8.0f);

        rm::app::PassabilitySet passability{field, false, 0.0f};
        rm::vfs::Vfs content;
        rm::app::MatchRunner runner =
            rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        runner.scripts.clear();

        // A shot arriving on the victim's body: a `Unit` impact, not ground.
        scene.projectiles.push_back(rm::sim::Projectile{
            .position = rm::test::at(500, 1, 480),
            .velocity = rm::test::at(0, 0, 20),
            .damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)),
            .targetLayers = rm::unitdef::TargetLayerMask::Surface,
            .firedByArmy = 0,
            .ticksRemaining = 1,
        });
        (void)rm::app::advanceMatch(runner, 0, 0.0f);
        // C-173: the contact is detected this tick and DELIVERED at the start of
        // the next — the event lands on the second advance.
        (void)rm::app::advanceMatch(runner, 1, 0.0f);

        REQUIRE(scene.events.count(rm::sim::EventKind::ProjectileImpact) == 1);
        const rm::sim::Event* impact =
            firstOf(scene.events, rm::sim::EventKind::ProjectileImpact);
        REQUIRE(impact != nullptr);
        CHECK(impact->impactType == rm::sim::ImpactType::Unit);
        CHECK(scene.impactMarks.empty());
    }
}
