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
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/Terrain.hpp"
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

TEST_CASE("C-292: only a land-layer death scorches the ground", "[fa-terrain]") {
    // Retail's `Unit::OnKilled` gates `CreateScorchableDecal` on
    // `layer == 'Land'` — a crashed gunship or a sunk hull leaves no mark.
    // The layer dies with the unit, so the gate's answer is recorded on the
    // feature at death (`Feature::marksGround`) and the decal rebuild reads it.
    const rm::HeightField field = flatField(128, 0.0f);

    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_unit";
    def.wreckMass = rm::test::mag(100.0f);
    const rm::UnitTypeIndex type = roster.addType(def);
    const rm::sim::UnitId tank = roster.add(type, 512.0f, 200.0f, 0, 500.0f);
    const rm::sim::UnitId gunship = roster.add(type, 600.0f, 200.0f, 0, 500.0f);
    roster.motion(gunship).canFly = true;  // the air layer

    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    const std::vector<int> commandersEver(2, 0);
    rm::sim::FeatureStore features;
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .features = &features,
                         .commandersEver = commandersEver};
    const rm::sim::Terrain terrain{field};

    roster.health(tank).current = rm::sim::Mag{};
    roster.health(gunship).current = rm::sim::Mag{};
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(features.size() == 2);

    // The gate's answer, per feature: the tank's wreck marks, the gunship's
    // does not — even though both left reclaimable mass on the same ground.
    const rm::sim::Feature& tankWreck = features.all()[0];
    const rm::sim::Feature& airWreck = features.all()[1];
    CHECK(tankWreck.marksGround);
    CHECK_FALSE(airWreck.marksGround);

    // And the projection honours it: only the land wreck becomes a decal.
    rm::app::UnitScene scene;
    (void)scene.features.add(tankWreck);
    (void)scene.features.add(airWreck);
    rm::app::refreshWreckDecals(scene, field);
    CHECK(scene.wreckDecals.size() == rm::wreckVertexCount());
}

TEST_CASE("C-289: a tarmac structure stamps its pad and the stamp dies with it",
          "[fa-terrain]") {
    // Retail's `CreateTarmac` (defaultunits.lua:75) is decal-only because its
    // `SetTerrainTypeRect(self.tarmacRect, {TypeCode = factionIndex + 189})`
    // is commented out — the write never dirtied path tables anyway. Ours is
    // live: a structure carrying `Display.Tarmacs` stamps its footprint with
    // the owner's faction tarmac code (UEF 190, Aeon 191, Cybran 192,
    // Seraphim 193 — TerrainTypes.lua:1714-1746) the moment it stands, and
    // `DestroyTarmac`'s counterpart is the tick's owner sweep.
    rm::HeightField field = flatField(128, 0.0f);
    rm::app::UnitScene scene = makeScene(false, 0.0f);

    // The pad sits on Dirt09 — a `Blocking = true` code (TerrainTypes.lua:703)
    // — so the stamp's effect is observable through the C-288 LUT: the cells
    // are unwalkable until the tarmac covers them, and unwalkable again once
    // the structure is gone.
    std::vector<std::uint8_t> base(128 * 128, std::uint8_t{0});
    for (int z = 6; z <= 10; ++z) {
        for (int x = 6; x <= 10; ++x) {
            base[static_cast<std::size_t>(z) * 128 + static_cast<std::size_t>(x)] = 9;
        }
    }
    scene.terrainTypeGrid = rm::sim::TerrainTypeGrid{base, 128, 128};
    rm::app::PassabilitySet passability{field, false, 0.0f, &scene.terrainTypeGrid};

    rm::unitdef::UnitDef factory;
    factory.name = "TESTFAC";
    factory.footprintSquaresX = 2;
    factory.footprintSquaresZ = 2;
    factory.health = rm::sim::magFromFloat(1000.0f);
    // A radius, because `retireDead` reads `radiusElmos <= 0` as "already
    // retired" — without one the corpse is never reaped and the stamp's
    // owner stays alive.
    factory.collisionRadiusElmos = 1.0f;
    const rm::UnitTypeIndex type = [&] {
        scene.definitions.push_back(factory);
        const auto added =
            scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(added, rm::data::moveDefFor(factory), 1.0f);
        const std::string path = "/units/TESTFAC/TESTFAC_unit.bp";
        scene.setPathForType(added, path);
        scene.typeForBlueprint.emplace(path, added);
        // `Display.Tarmacs` — the flag `CreateTarmac` gates on. A synthetic
        // def has no blueprint to read it from, so the type is marked the way
        // `ensureDrawableType` marks a real one.
        scene.tarmacForType[static_cast<std::size_t>(added)] = 1;
        return added;
    }();
    (void)type;

    rm::vfs::Vfs content;
    const auto spawned = rm::app::spawnUnit(scene, content, field,
                                            "/units/TESTFAC/TESTFAC_unit.bp",
                                            {64.0f, 0.0f, 64.0f}, scene.armies[0],
                                            rm::Brad{0});
    REQUIRE(spawned);

    // The footprint is 2x2 squares centred on (64,64): cells 7..8 each way,
    // stamped with the UEF tarmac code — army 0's faction is Uef, and retail's
    // formula is `factionIndex + 189` on the 1-based index.
    CHECK(scene.terrainTypeGrid.types()[7 * 128 + 7] == 190);
    CHECK(scene.terrainTypeGrid.types()[8 * 128 + 8] == 190);
    CHECK(scene.terrainTypeGrid.types()[6 * 128 + 6] == 9);

    // Pathing sees it through the same PassabilitySet the match routes on:
    // the stamped squares are walkable while the factory stands.
    const rm::sim::PassabilityGrid& stamped = passability.gridFor(17.0f, 12.0f);
    CHECK(stamped.passableAt(0, 0));

    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();

    // The stamp is sim state: two matches differing only in it must hash
    // apart, and a save must carry it.
    const rm::StateHash withPad = rm::sim::hashMatch(scene.store, runner.match);
    scene.terrainTypeGrid.setRect(rm::test::fx(0.0f), rm::test::fx(0.0f),
                                  rm::test::fx(8.0f), rm::test::fx(8.0f),
                                  std::uint8_t{9});
    CHECK(rm::sim::hashMatch(scene.store, runner.match) != withPad);

    // The journal round-trips through SaveState: the restored grid answers
    // the same type question the live one does.
    const auto saved = rm::sim::SaveState::decode(rm::sim::SaveState::encode({
        .tick = 1,
        .random = runner.match.random.snapshot(),
        .terrainStamps = std::vector<rm::sim::TerrainStamp>{
            scene.terrainTypeGrid.journal().begin(),
            scene.terrainTypeGrid.journal().end()}}));
    REQUIRE(saved);
    REQUIRE(saved->terrainStamps.has_value());
    rm::sim::TerrainTypeGrid restored{base, 128, 128};
    restored.restoreJournal(*saved->terrainStamps);
    CHECK(std::ranges::equal(restored.types(), scene.terrainTypeGrid.types()));
    scene.store.health()[spawned->index].current = rm::sim::Mag{};
    (void)rm::app::advanceMatch(runner, 0, 0.0f);
    CHECK(scene.terrainTypeGrid.types()[7 * 128 + 7] == 9);
    CHECK(scene.terrainTypeGrid.types()[8 * 128 + 8] == 9);
    // `passableAt` is "any walkable square" — a partially blocked cell still
    // answers true. The pad's unwalkable-again is `divisorAt != 1`: not
    // pristine, which is the same bar `sitePlaceable` holds a footprint to.
    CHECK(passability.gridFor(17.0f, 12.0f).divisorAt(0, 0) != 1);
}
