// A real tactical silo, spawned through the real route, building real ammunition.
//
// WP-29's reachability proof: the SiloAmmo records are created where units enter a match
// (`SceneBuild`), not by a test poking the vector, or the whole ammunition path would be
// plumbing with no source. Runs against the retail install's own archives — UEB4302, the
// SMD whose interceptor occupies the TACTICAL silo slot (`C-082`) — and SKIPped where the
// drive is absent, like test_real_vfs.cpp.
//
// The numbers are the corpus's: `ART-S001 /units/UEB4302/UEB4302_unit.bp` (BuildRate 1080,
// CountedProjectile, MaxProjectileStorage 7) and
// `ART-S013 /projectiles/TIMMissileIntercerptor01/TIMMissileIntercerptor01_proj.bp:Economy`
// (3600 mass, 360000 energy, BuildTime 259200). `C-083`'s formula turns those into 2400
// production ticks per interceptor at full funding.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/vfs/Vfs.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path gamedata() {
    return "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata";
}

/// A flat field: a test about silo production is not also a test about terrain.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::sim::Resources bank(float mass, float energy) {
    return {.mass = rm::sim::magFromFloat(mass), .energy = rm::sim::magFromFloat(energy)};
}

} // namespace

TEST_CASE("two counted weapons on one retail slot produce one silo record", "[corpus]") {
    // C-081 gives a CAiSiloBuildImpl one tactical and one nuke SLOT, not one record per
    // weapon. A unit carrying two tactical counted weapons keeps the FIRST — the
    // order-dependent selection C-085 records for `Unit::GetCountedProjectileWeapon`
    // (0x006b1fb0). No shipped unit is shaped like this, so the fixture is a synthetic
    // blueprint borrowing UEB4302's real mesh and textures from the mounted archive.
    const std::filesystem::path units = gamedata() / "units.scd";
    const std::filesystem::path projectiles = gamedata() / "projectiles.scd";
    if (!std::filesystem::exists(units) || !std::filesystem::exists(projectiles)) {
        SKIP("no retail install at " + gamedata().string());
    }

    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "rm_silo_slot_test";
    std::filesystem::remove_all(scratch);
    const std::filesystem::path dir = scratch / "units" / "XXS4302";
    std::filesystem::create_directories(dir);
    {
        std::ofstream bp{dir / "XXS4302_unit.bp"};
        bp << R"(UnitBlueprint {
    BlueprintId = 'XXS4302',
    Defense = { Health = 1000, MaxHealth = 1000 },
    Economy = { BuildRate = 1080 },
    Physics = { MotionType = 'RULEUMT_None' },
    Display = {
        UniformScale = 0.1,
        Mesh = {
            LODs = {
                {
                    MeshName = '/units/UEB4302/UEB4302_lod0.scm',
                    AlbedoName = '/units/UEB4302/UEB4302_Albedo.dds',
                    SpecularName = '/units/UEB4302/UEB4302_SpecTeam.dds',
                    ShaderName = 'Unit',
                },
            },
        },
    },
    Weapon = {
        {
            Label = 'FirstSilo',
            CountedProjectile = true,
            MaxProjectileStorage = 7,
            ProjectileId = '/projectiles/TIMMissileIntercerptor01/TIMMissileIntercerptor01_proj.bp',
        },
        {
            Label = 'SecondSilo',
            CountedProjectile = true,
            MaxProjectileStorage = 3,
            ProjectileId = '/projectiles/TIMMissileIntercerptor01/TIMMissileIntercerptor01_proj.bp',
        },
    },
})";
    }

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(units));
    REQUIRE(vfs.mountArchive(projectiles));
    vfs.mountDirectory(scratch);

    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.economies.assign(2, rm::sim::Economy{});
    scene.commandersEver = {0, 0};

    const rm::HeightField field = flatField();
    const auto spawned = rm::app::spawnUnit(scene, vfs, field, "/units/XXS4302/XXS4302_unit.bp",
                                            {200.0f, 0.0f, 200.0f}, scene.armies[0], rm::Brad{0});
    std::filesystem::remove_all(scratch);
    REQUIRE(spawned.has_value());

    // One record, on the tactical slot, from the FIRST counted weapon — capacity 7 is the
    // first weapon's authored value; 3 belongs to the duplicate that must not exist.
    REQUIRE(scene.siloAmmo.size() == 1);
    CHECK(scene.siloAmmo.front().slot == 0);
    CHECK(scene.siloAmmo.front().weapon == 0);
    CHECK(scene.siloAmmo.front().capacity == 7);
}

TEST_CASE("a spawned UEB4302 automatically builds one interceptor in its blueprint-derived time",
          "[corpus]") {
    const std::filesystem::path units = gamedata() / "units.scd";
    const std::filesystem::path projectiles = gamedata() / "projectiles.scd";
    if (!std::filesystem::exists(units) || !std::filesystem::exists(projectiles)) {
        SKIP("no retail install at " + gamedata().string());
    }

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(units));
    REQUIRE(vfs.mountArchive(projectiles));

    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.economies.assign(2, rm::sim::Economy{});
    scene.economies[0].stored = bank(10000.0, 1000000.0);
    scene.commandersEver = {0, 0};

    const rm::HeightField field = flatField();
    const auto spawned = rm::app::spawnUnit(scene, vfs, field, "/units/UEB4302/UEB4302_unit.bp",
                                            {200.0f, 0.0f, 200.0f}, scene.armies[0], rm::Brad{0});
    REQUIRE(spawned.has_value());

    // C-241: an idle silo enqueues its tactical build itself — no command is issued anywhere
    // in this test, so the record's existence is the auto-start working.
    REQUIRE(scene.siloAmmo.size() == 1);
    CHECK(scene.siloAmmo.front().slot == 0);
    CHECK(scene.siloAmmo.front().capacity == 7);
    CHECK(scene.siloAmmo.front().totalTicks == 2400);

    rm::sim::Match match{.armies = scene.armies,
                         .economies = scene.economies,
                         .projectiles = &scene.projectiles,
                         .building = &scene.building,
                         .siloAmmo = &scene.siloAmmo,
                         .features = &scene.features,
                         .events = &scene.events,
                         .commandersEver = scene.commandersEver,
                         .baseStorage = bank(10000.0, 1000000.0)};

    for (int tick = 0; tick < 2400; ++tick) {
        (void)rm::sim::tickSkirmish(scene.store, scene.catalog, match, rm::sim::Terrain{field});
    }

    REQUIRE(scene.siloAmmo.size() == 1);
    CHECK(scene.siloAmmo.front().stored == 1);
    CHECK(scene.siloAmmo.front().elapsedTicks == 0);

    // Charged what the projectile blueprint states, spread over the whole production.
    CHECK(rm::sim::magToFloat(scene.economies[0].stored.mass) == Approx(10000.0 - 3600.0).margin(1.0));
    CHECK(rm::sim::magToFloat(scene.economies[0].stored.energy)
          == Approx(1000000.0 - 360000.0).margin(100.0));
}

TEST_CASE("an unenhanced ACU never manufactures enhancement-only missile ammunition",
          "[corpus][economy-acu]") {
    rm::vfs::Vfs vfs;
    if (!std::filesystem::exists(gamedata() / "projectiles.scd")) SKIP("retail install unavailable");
    REQUIRE(vfs.mountArchive(gamedata() / "units.scd"));
    REQUIRE(vfs.mountArchive(gamedata() / "projectiles.scd"));
    rm::app::UnitScene scene;
    const auto field = flatField();
    SECTION("skirmish commander spawn") {
        const std::array<rm::mapinfo::StartPosition, 1> starts{{{0, 200, 200}}};
        rm::app::spawnCommanders(scene, field, starts, vfs);
    }
    SECTION("ordinary unit spawn") {
        scene.armies = rm::sim::freeForAll(1);
        scene.economies.resize(1);
        scene.commandersEver = {0};
        REQUIRE(rm::app::spawnUnit(scene, vfs, field, "/units/UEL0001/UEL0001_unit.bp",
            {200, 0, 200}, scene.armies[0], rm::Brad{0}));
    }
    REQUIRE(scene.store.liveCount() == 1);
    CHECK(scene.siloAmmo.empty());
    scene.economies[0].stored = {};
    rm::sim::Match match{.armies = scene.armies, .economies = scene.economies,
        .projectiles = &scene.projectiles, .building = &scene.building,
        .siloAmmo = &scene.siloAmmo, .baseStorage = rm::app::kStartingStorage};
    for (rm::TickIndex tick = 0; tick < 100; ++tick) {
        (void)rm::sim::tickSkirmish(scene.store, scene.catalog, match,
            scene.terrain(field), rm::app::gAppTickRate, tick);
        CHECK(scene.economies[0].usageLastTick.energy == rm::sim::Mag{});
    }
    CHECK(scene.economies[0].stored.energy > rm::sim::Mag::fromInt(49));
}
