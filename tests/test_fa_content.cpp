// Player-perspective coverage for content claims (see docs/fa-exe-analysis-plan.md).
// Filled by the coverage-test wave; each TEST_CASE cites its claim IDs.
//
// The content claims a player can observe: which archive's file the game
// actually reads when two mounts hold the same path (C-266), and whether the
// markers a map ships become the units on the field (C-273/C-274). The retail
// mechanisms behind them — SupComDataPath.lua, the schook hook, Lua blueprint
// merge — are documented divergences and are not what these tests pin.
#include "app/Cli.hpp"
#include "core/blueprint/BlueprintMesh.hpp"
#include "core/map/HeightField.hpp"
#include "core/map/TerrainType.hpp"
#include "core/sim/Terrain.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/scene/Picking.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/vfs/Vfs.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "miniz.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {

using Catch::Approx;

// One scratch directory per process, the same rule test_vfs.cpp keeps:
// `ctest -j` runs each case as its own rm_tests process, so the pid is all the
// uniqueness needed.
[[nodiscard]] std::filesystem::path scratch() {
    static const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("rm_fa_content-" + std::to_string(getpid()));
    return dir;
}

struct Scratch {
    Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(scratch(), ec);
        std::filesystem::create_directories(scratch());
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(scratch(), ec);
    }
};

/// Writes a ZIP with the given (name, contents) pairs.
void writeArchive(const std::filesystem::path& path,
                  const std::vector<std::pair<std::string, std::string>>& files) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0) != MZ_FALSE);
    for (const auto& [name, contents] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION))
                != MZ_FALSE);
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip) != MZ_FALSE);
    mz_zip_writer_end(&zip);
}

[[nodiscard]] std::string asText(const std::vector<std::byte>& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// A heightfield with a slope, so a placed instance's Y proves it was sampled
/// rather than zeroed or trusted from the file.
[[nodiscard]] rm::HeightField slopedField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

// Shaped like a retail <map>_save.lua: army markers out of file order among
// the other marker kinds every stock map carries.
constexpr const char* kSave = R"(
Scenario = {
    MasterChain = {
        ['_MASTERCHAIN_'] = {
            Markers = {
                ['Mass 4'] = {
                    ['type'] = STRING( 'Mass' ),
                    ['position'] = VECTOR3( 40.0, 1.0, 40.0 ),
                },
                ['ARMY_2'] = {
                    ['type'] = STRING( 'Blank Marker' ),
                    ['position'] = VECTOR3( 900.0, 20.0, 700.0 ),
                },
                ['ARMY_1'] = {
                    ['type'] = STRING( 'Blank Marker' ),
                    ['position'] = VECTOR3( 200.0, 20.0, 300.0 ),
                },
            },
        },
    },
})";

// The Armies half of a _save.lua: GROUP-wrapped unit trees, the way
// SCMP_039's WRECKAGE group nests them.
constexpr const char* kSaveWithUnits = R"(
Scenario = {
    Armies = {
        ['ARMY_1'] = {
            personality = '',
            ['Units'] = GROUP {
                orders = '',
                Units = {
                    ['INITIAL'] = GROUP {
                        orders = '',
                        Units = {
                            ['UNIT_1'] = {
                                type = 'ueb5101',
                                orders = '',
                                Position = { 100.0, 10.0, 200.0 },
                                Orientation = { 0.0, 1.5707963, 0.0 },
                            },
                        },
                    },
                    ['WRECKAGE'] = GROUP {
                        orders = '',
                        Units = {
                            ['UNIT_2'] = {
                                type = 'uec1101',
                                Position = { 50.0, 10.0, 60.0 },
                                Orientation = { 0.0, 0.0, 0.0 },
                            },
                        },
                    },
                },
            },
        },
        ['ARMY_2'] = {
            ['Units'] = GROUP {
                Units = {
                    ['INITIAL'] = GROUP {
                        Units = {},
                    },
                },
            },
        },
    },
})";

} // namespace

TEST_CASE("C-266: the mount order makes lua.scd shadow mohodata.scd",
          "[fa-content]") {
    // The player-visible outcome of retail's mount list: the game Lua wins
    // over the engine stubs. Retail gets there with first-mount-wins over a
    // sorted list; we get there with last-mount-wins over a list that puts the
    // override archives last (the mechanism divergence is documented in the
    // coverage audit — the observable shadow is what this pins).
    const Scratch guard;

    writeArchive(scratch() / "gamedata/mohodata.scd",
                 {{"lua/sim/unit.lua", "engine stub"}});
    writeArchive(scratch() / "gamedata/lua.scd",
                 {{"lua/sim/unit.lua", "game lua"}});
    writeArchive(scratch() / "gamedata/units.scd",
                 {{"units/x/x_unit.bp", "unit data"}});

    rm::vfs::Vfs vfs;
    const auto ordered = rm::app::orderArchivesForMount({
        scratch() / "gamedata/lua.scd",
        scratch() / "gamedata/units.scd",
        scratch() / "gamedata/mohodata.scd",
    });
    REQUIRE(ordered.size() == 3);
    for (const auto& archive : ordered) {
        REQUIRE(vfs.mountArchive(archive));
    }

    CHECK(asText(*vfs.read("/lua/sim/unit.lua")) == "game lua");
    CHECK(asText(*vfs.read("/units/x/x_unit.bp")) == "unit data");
}

TEST_CASE("C-273: the _save.lua unit tree yields pre-placed units per army",
          "[fa-content]") {
    // Retail's `ArmyInitializePrebuiltUnits` walks Scenario.Armies.<army>.Units
    // and spawns every leaf. The observable half this pins: the tree parse —
    // nested GROUPs flatten to (army, type, position) rows in elmos.
    const auto units = rm::scenario::loadArmyUnits(kSaveWithUnits);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 2);

    CHECK((*units)[0].army == "ARMY_1");
    CHECK((*units)[0].type == "ueb5101");
    CHECK((*units)[0].position[0] == Approx(100.0f * rm::scmap::kElmosPerOgrid));
    CHECK((*units)[0].position[2] == Approx(200.0f * rm::scmap::kElmosPerOgrid));
    CHECK((*units)[0].orientation[1] == Approx(1.5707963f));

    CHECK((*units)[1].type == "uec1101");
    CHECK((*units)[1].position[0] == Approx(50.0f * rm::scmap::kElmosPerOgrid));

    // An empty INITIAL group yields nothing — stock skirmish maps are all
    // empty trees, so absence is ordinary rather than an error.
    const auto empty = rm::scenario::loadArmyUnits(R"(
Scenario = { Armies = { ['ARMY_1'] = { ['Units'] = GROUP { Units = {} } } } })");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("C-275: TerrainTypes.lua parses to the blocking LUT the walk loop reads",
          "[fa-content]") {
    // Retail's `STIMap::LoadTerrainTypes` runs /lua/TerrainTypes.lua and writes
    // each entry's Blocking flag into a 256-entry LUT. Ours is the same LUT
    // built at compile time — this pins that the shipped file parses to
    // exactly the codes the LUT marks.
    const auto defs = rm::loadTerrainTypes(R"(
TerrainTypes = {
    {
        Name = 'Default',
        TypeCode = 1,
        Blocking = false,
    },
    {
        Name = 'Dirt09',
        TypeCode = 9,
        Blocking = true,
    },
    {
        Name = 'Lava01',
        TypeCode = 230,
        Blocking = true,
    },
})");
    REQUIRE(defs.size() == 3);
    CHECK(defs[0].typeCode == 1);
    CHECK_FALSE(defs[0].blocking);
    CHECK(defs[1].typeCode == 9);
    CHECK(defs[1].blocking);
    CHECK(defs[2].typeCode == 230);
    CHECK(defs[2].blocking);

    // The shipped file, when the corpus is mounted: every Blocking=true entry
    // must land on a code the LUT marks, and vice versa.
    const std::filesystem::path shipped =
        "re-fa/corpus/lua/lua/TerrainTypes.lua";
    std::error_code ec;
    if (std::filesystem::is_regular_file(shipped, ec)) {
        std::ifstream in{shipped, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        const auto real = rm::loadTerrainTypes(text);
        // 59 entries — the 60th "TypeCode" in the file is a string literal in
        // a key list, not an assignment.
        REQUIRE(real.size() == 59);
        for (const rm::TerrainTypeDef& def : real) {
            INFO("TypeCode " << def.typeCode);
            CHECK(rm::kTerrainTypeBlocking[static_cast<std::size_t>(def.typeCode)]
                  == def.blocking);
        }
    }
}

TEST_CASE("C-273/C-274: a map's army markers become one spawn each, on the terrain",
          "[fa-content]") {
    // The bootstrap chain a player sees: _save.lua's MasterChain markers are
    // the start positions (C-274), and each one puts a unit on the field at
    // the marker's X/Z with Y sampled from the heightmap (C-273's spawn —
    // the marker's stored height is deliberately not trusted).
    const auto starts = rm::scenario::loadStartPositions(kSave);
    REQUIRE(starts.has_value());
    REQUIRE(starts->size() == 2);

    const rm::HeightField field = slopedField();
    const std::vector<rm::UnitInstance> placed =
        rm::atStartPositions(field, *starts, /*scale=*/1.0f);
    REQUIRE(placed.size() == starts->size());

    for (std::size_t i = 0; i < starts->size(); ++i) {
        INFO("army " << i);
        CHECK(placed[i].position[0] == Approx((*starts)[i].x));
        CHECK(placed[i].position[2] == Approx((*starts)[i].z));
        // Y is the terrain's answer at that point, not the file's.
        CHECK(placed[i].position[1]
              == Approx(field.heightAtWorld((*starts)[i].x, (*starts)[i].z)));
    }
}

// --- C-019: the STIMap query surface -----------------------------------------
//
// Retail's `STIMap` answers height, water, deep, abyss and playable-rect
// queries. Height and water are already pinned (test_height_field.cpp,
// test_real_scmap.cpp); these pin the remaining three reads.

TEST_CASE("C-019: deep and abyss queries clamp the terrain to their levels",
          "[fa-content]") {
    // A field sloping from 0 to 100 elmos across 128 squares: the left edge
    // sits under both levels, the right edge above them.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    for (std::int32_t z = 0; z <= field.squaresZ; ++z) {
        for (std::int32_t x = 0; x <= field.squaresX; ++x) {
            field.raw[static_cast<std::size_t>(z * (field.squaresX + 1) + x)] =
                static_cast<std::uint16_t>(x * 100 / 128);
        }
    }

    rm::sim::Terrain terrain{field, /*hasWater=*/true, /*waterLevelElmos=*/50.0f};
    terrain.setWaterLevels(/*deepElmos=*/40.0f, /*abyssElmos=*/20.0f);

    // Above the level, the query answers the level; below it, the ground.
    const rm::sim::Fx high = rm::sim::fxFromFloat(1000.0f);  // ~78 elmos up
    const rm::sim::Fx low = rm::sim::fxFromFloat(10.0f);     // ~0.8 elmos up
    CHECK(terrain.deepHeightAt(high, high) == rm::sim::fxFromFloat(40.0f));
    CHECK(terrain.abyssHeightAt(high, high) == rm::sim::fxFromFloat(20.0f));
    CHECK(terrain.deepHeightAt(low, low) == terrain.heightAt(low, low));
    CHECK(terrain.abyssHeightAt(low, low) == terrain.heightAt(low, low));

    // Undeclared levels fall back to the water level — retail's own default.
    rm::sim::Terrain plain{field, /*hasWater=*/true, /*waterLevelElmos=*/50.0f};
    CHECK(plain.deepHeightAt(high, high) == rm::sim::fxFromFloat(50.0f));
    CHECK(plain.abyssHeightAt(high, high) == rm::sim::fxFromFloat(50.0f));
}

TEST_CASE("C-019: IsPlayable answers inside the declared rect, true without one",
          "[fa-content]") {
    const rm::HeightField field = slopedField();
    rm::sim::Terrain terrain{field};

    // No declared rect: the whole map is playable.
    CHECK(terrain.isPlayable(rm::sim::fxFromFloat(0.0f), rm::sim::fxFromFloat(0.0f)));
    CHECK(terrain.isPlayable(rm::sim::fxFromFloat(-500.0f), rm::sim::fxFromFloat(9999.0f)));

    terrain.setPlayableRect(rm::sim::fxFromFloat(100.0f), rm::sim::fxFromFloat(100.0f),
                            rm::sim::fxFromFloat(500.0f), rm::sim::fxFromFloat(500.0f));
    CHECK(terrain.isPlayable(rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(300.0f)));
    CHECK(terrain.isPlayable(rm::sim::fxFromFloat(100.0f), rm::sim::fxFromFloat(500.0f)));
    CHECK_FALSE(terrain.isPlayable(rm::sim::fxFromFloat(50.0f), rm::sim::fxFromFloat(300.0f)));
    CHECK_FALSE(terrain.isPlayable(rm::sim::fxFromFloat(300.0f), rm::sim::fxFromFloat(600.0f)));
}

TEST_CASE("C-019: surface intersection answers water over drowned ground",
          "[fa-content]") {
    // `STIMap::SurfaceIntersection` vs `TerrainIntersection`: the same ray,
    // answered against the higher of terrain and water. `pickGround` is the
    // terrain half (test_picking.cpp); `pickSurface` adds the water plane.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    // A ramp: the west half drowned at water level 50, the east half dry.
    for (std::int32_t z = 0; z <= field.squaresZ; ++z) {
        for (std::int32_t x = 0; x <= field.squaresX; ++x) {
            field.raw[static_cast<std::size_t>(z * (field.squaresX + 1) + x)] =
                static_cast<std::uint16_t>(x * 100 / 128);
        }
    }

    // A ray straight down over drowned ground hits the water plane, not the
    // seabed 40 elmos below it.
    const rm::Ray down{.origin = simd_make_float3(200.0f, 200.0f, 200.0f),
                       .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
    const std::optional<simd_float3> water = rm::pickSurface(down, field, 50.0f);
    REQUIRE(water.has_value());
    CHECK(water->y == Approx(50.0f));
    CHECK(water->x == Approx(200.0f));

    // The same ray over dry ground falls through to the terrain answer.
    const rm::Ray dry{.origin = simd_make_float3(1000.0f, 200.0f, 200.0f),
                      .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
    const std::optional<simd_float3> ground = rm::pickSurface(dry, field, 50.0f);
    REQUIRE(ground.has_value());
    CHECK(ground->y == Approx(field.heightAtWorld(1000.0f, 200.0f)));
    CHECK(ground->y > 50.0f);
}

// --- C-271: the bp→script binding chain ---------------------------------------
//
// `ScriptModule`/`ScriptClass` when authored; else the `_script.lua`/`TypeClass`
// convention; else the `/lua/sim/*.lua` default.

TEST_CASE("C-271: the script binding falls back module then class",
          "[fa-content]") {

    using rm::blueprint::scriptBindingFor;
    using rm::blueprint::ScriptBinding;
    const auto unit = scriptBindingFor("/units/UEL0201/UEL0201_unit.bp", "", "",
                                       "/lua/sim/unit.lua", "Unit");
    CHECK(unit.module == "/units/UEL0201/UEL0201_script.lua");
    CHECK(unit.className == "TypeClass");

    // Same convention for props and projectiles.
    CHECK(scriptBindingFor("/env/Evergreen/props/Tree01_prop.bp", "", "",
                           "/lua/sim/prop.lua", "Prop")
              .module
          == "/env/Evergreen/props/Tree01_script.lua");
    CHECK(scriptBindingFor("/projectiles/TDFGauss01/TDFGauss01_proj.bp", "", "",
                           "/lua/sim/projectile.lua", "Projectile")
              .module
          == "/projectiles/TDFGauss01/TDFGauss01_script.lua");

    // Authored keys win outright.
    const auto authored = scriptBindingFor("/units/X/X_unit.bp",
                                           "/lua/custom/module.lua", "MyClass",
                                           "/lua/sim/unit.lua", "Unit");
    CHECK(authored.module == "/lua/custom/module.lua");
    CHECK(authored.className == "MyClass");

    // A blueprint whose stem carries no suffix has no conventional script —
    // the import-failure default answers instead.
    const auto bare = scriptBindingFor("/units/X/X.bp", "", "",
                                       "/lua/sim/unit.lua", "Unit");
    CHECK(bare.module == "/lua/sim/unit.lua");
    CHECK(bare.className == "TypeClass");
}
