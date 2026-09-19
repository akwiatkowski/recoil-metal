// The ground, sampled in fixed point.
//
// The claim under test is that `sim::Terrain` and `HeightField::heightAtWorld` agree about
// where the ground is — one in fixed point for the sim, one in float for the renderer. If they
// drifted, a unit would stand at a height the terrain mesh does not draw it at, which reads as
// a rendering bug and is not one.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/MaxHeightPyramid.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Terrain.hpp"
#include "core/vfs/Vfs.hpp"

#include "support/FxMatchers.hpp"

#include <cstdint>

using Catch::Approx;
using rm::sim::Fx;
using rm::sim::Terrain;

namespace {

/// A field with a real vertical scale and a ramp in it, so interpolation has something to do.
[[nodiscard]] rm::HeightField rampField() {
    rm::HeightField field;
    field.squaresX = 16;
    field.squaresZ = 16;
    field.baseHeight = 12.5f;
    field.heightScale = 0.01f;  // the order of magnitude a real .smf states
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    for (int z = 0; z <= field.squaresZ; ++z) {
        for (int x = 0; x <= field.squaresX; ++x) {
            const auto index = static_cast<std::size_t>(z)
                                   * static_cast<std::size_t>(field.verticesX())
                               + static_cast<std::size_t>(x);
            field.raw[index] = static_cast<std::uint16_t>(x * 800 + z * 300);
        }
    }
    return field;
}

} // namespace

TEST_CASE("a corner height matches the float accessor exactly where it can") {
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    for (int z = 0; z <= field.squaresZ; ++z) {
        for (int x = 0; x <= field.squaresX; ++x) {
            REQUIRE(rm::test::asFloat(terrain.cornerHeight(x, z))
                    == Approx(field.heightAt(x, z)).margin(rm::test::kFxStep));
        }
    }
}

TEST_CASE("an interpolated height matches the float accessor across the map") {
    // Every eighth of a square, so the fractional part takes every value that matters — the
    // corners, the midpoints, and the places in between where a rounding difference would
    // show up.
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    double worst = 0.0;
    for (int i = 0; i <= 16 * 8; ++i) {
        for (int j = 0; j <= 16 * 8; ++j) {
            const float x = static_cast<float>(i) * (8.0f / 8.0f);
            const float z = static_cast<float>(j) * (8.0f / 8.0f);
            const double got =
                static_cast<double>(rm::test::asFloat(terrain.heightAt(rm::test::fx(x),
                                                                      rm::test::fx(z))));
            worst = std::max(worst, std::abs(got - static_cast<double>(
                                                       field.heightAtWorld(x, z))));
        }
    }
    // Four steps: the sample position, the two axis interpolations and the decode each round
    // once. Well below the vertical resolution of the source data, which is 0.01 elmos.
    CHECK(worst <= 4.0 * static_cast<double>(rm::test::kFxStep));
}

TEST_CASE("a square is eight elmos, so the sample position is exact") {
    // Small piece of luck worth pinning: 8 is a power of two, so dividing a world coordinate
    // by the square size introduces no rounding at all. On a grid of any other pitch every
    // height would carry an error from this one division.
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    // A position exactly on a corner must give exactly that corner's height, with no slack.
    for (int c = 0; c <= field.squaresX; ++c) {
        const Fx at = Fx::fromInt(c * rm::kSquareSize);
        REQUIRE(terrain.heightAt(at, Fx{}) == terrain.cornerHeight(c, 0));
    }
}

TEST_CASE("off the map clamps rather than reading past the grid") {
    const rm::HeightField field = rampField();
    const Terrain terrain{field};

    const Fx farPast = Fx::fromInt(100000);
    const Fx farBefore = Fx::fromInt(-100000);

    CHECK(terrain.heightAt(farBefore, farBefore) == terrain.cornerHeight(0, 0));
    CHECK(terrain.heightAt(farPast, farPast)
          == terrain.cornerHeight(field.squaresX, field.squaresZ));

    // And the clamp happens BEFORE the floor, so a coordinate large enough to overflow the
    // cast is not reached — checked by not crashing and by giving the edge answer.
    CHECK(terrain.heightAt(Fx::fromRaw(INT32_MAX), Fx{})
          == terrain.cornerHeight(field.squaresX, 0));
}

TEST_CASE("an empty field is flat at its base height, not a crash") {
    rm::HeightField empty;
    empty.baseHeight = 7.0f;
    const Terrain terrain{empty};
    CHECK(rm::test::asFloat(terrain.heightAt(Fx::fromInt(500), Fx::fromInt(500)))
          == Approx(7.0f).margin(rm::test::kFxStep));
}

TEST_CASE("the look-ahead reports the highest surface in the containing power-of-two cell") {
    // `C-246`: retail indexes a max-height pyramid at the level whose cell is at least
    // half the reach wide, aligned to the grid — so the answer is a neighbourhood maximum,
    // not a directional scan, and below one square of reach it is the point sample.
    rm::HeightField field;
    field.squaresX = 64;
    field.squaresZ = 64;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    // One spike at corner (20, 20): 500 elmos.
    field.raw[static_cast<std::size_t>(20) * static_cast<std::size_t>(field.verticesX()) + 20] = 500;
    const Terrain dry{field};

    // Standing at square (2, 2), 16 elmos in: a reach of 200 elmos is 25 squares, half is
    // 12, whose highest bit is 3, so level 4 and a 16-square cell [0, 16] — the spike at 20
    // is outside it.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(200)) == Fx{});
    // A reach of 400 elmos: 50 squares, half 25, bit 4, level 5, cell [0, 32] — inside.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(400))
          == Fx::fromInt(500));
    // Under a square of reach the point sample is what comes back.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(160), Fx::fromInt(160), Fx::fromInt(4))
          == Fx::fromInt(500));
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(4)) == Fx{});

    // From square 62 the level-5 cell is [32, 64], up against the map edge: its far
    // corners are the border itself and the spike at 20 is not in it. A reach past the
    // whole map caps at the level whose cell is the map, and that cell has the spike.
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(500), Fx::fromInt(500), Fx::fromInt(400)) == Fx{});
    CHECK(dry.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(100000))
          == Fx::fromInt(500));

    // Water is surface: a drowned cell reports the water level, and so does the point.
    const Terrain wet{field, true, 30.0f};
    CHECK(wet.maxSurfaceHeightNear(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(200))
          == Fx::fromInt(30));
    CHECK(wet.surfaceHeightAt(Fx::fromInt(16), Fx::fromInt(16)) == Fx::fromInt(30));
    CHECK(wet.surfaceHeightAt(Fx::fromInt(160), Fx::fromInt(160)) == Fx::fromInt(500));
}

TEST_CASE("the max-height pyramid answers exactly what the corner scan answers") {
    // `C-246`'s O(1) lookup must be indistinguishable from the scan it replaces: same cell
    // semantics (grid-aligned, far corner inclusive, corners clamped at the edge), same
    // water floor. Swept over positions, reaches and both odd and even map sizes so the
    // clamped last cell and every level get exercised.
    for (const int squares : {64, 37}) {
        rm::HeightField field;
        field.squaresX = squares;
        field.squaresZ = squares + 5;
        field.baseHeight = -20.0f;
        field.heightScale = 0.5f;
        field.raw.assign(field.sampleCount(), std::uint16_t{0});
        // A deterministic scatter of heights, with one tall spike near the far corner.
        for (int z = 0; z <= field.squaresZ; ++z) {
            for (int x = 0; x <= field.squaresX; ++x) {
                const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                                   + static_cast<std::size_t>(x);
                field.raw[index] = static_cast<std::uint16_t>((x * 37 + z * 91) % 997);
            }
        }
        field.raw[static_cast<std::size_t>(field.squaresZ - 1) * static_cast<std::size_t>(field.verticesX())
                  + static_cast<std::size_t>(field.squaresX - 1)] = 60000;
        const rm::MaxHeightPyramid pyramid{field};
        const Terrain scan{field, true, 15.0f};
        const Terrain fast{field, true, 15.0f, &pyramid};

        for (int px = 0; px <= squares * 8; px += 29) {
            for (int pz = 0; pz <= (squares + 5) * 8; pz += 31) {
                for (const int reach : {4, 8, 16, 60, 200, 401, 1000, 5000, 100000}) {
                    const Fx x = Fx::fromInt(px);
                    const Fx z = Fx::fromInt(pz);
                    const Fx r = Fx::fromInt(reach);
                    REQUIRE(fast.maxSurfaceHeightNear(x, z, r) == scan.maxSurfaceHeightNear(x, z, r));
                }
            }
        }
    }
}

TEST_CASE("a downhill vertical scale bypasses the pyramid") {
    // Raw maxima are height MINIMA when the scale is negative (legal: `setVerticalRange`
    // with min > max), so the view must scan instead of trusting the pyramid.
    rm::HeightField field;
    field.squaresX = 16;
    field.squaresZ = 16;
    field.baseHeight = 100.0f;
    field.heightScale = -1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{50});
    field.raw[static_cast<std::size_t>(3) * static_cast<std::size_t>(field.verticesX()) + 3] = 0;  // the HIGH point: 100 - 0
    const rm::MaxHeightPyramid pyramid{field};
    const Terrain fast{field, false, 0.0f, &pyramid};
    CHECK(fast.maxSurfaceHeightNear(Fx::fromInt(8), Fx::fromInt(8), Fx::fromInt(100))
          == Fx::fromInt(100));
}

TEST_CASE("flattenRect writes a uniform elevation over the rect's cells") {
    // C-286 (`Sim::FlattenMapRect`, `0x007524e0`): the sim-side heightfield is mutable —
    // a uniform u16 write over the rect, clamped to the map, with no pathing or terrain-type
    // invalidation. The rect arrives in elmos and covers whole heightmap cells: a corner-
    // sampled grid needs the corner at each end of the span written, so cells 2..5 of the
    // rect below are corners 2..6 inclusive.
    rm::HeightField field = rampField();
    Terrain terrain{field};

    terrain.flattenRect(Fx::fromInt(16), Fx::fromInt(16), Fx::fromInt(48), Fx::fromInt(48),
                        Fx::fromInt(50));

    for (int z = 2; z <= 6; ++z) {
        for (int x = 2; x <= 6; ++x) {
            CHECK(field.heightAt(x, z) == Approx(50.0f));
        }
    }
    // The write is bounded: the first corner outside the rect keeps its ramp value.
    CHECK(field.heightAt(7, 3) == Approx(12.5f + (7 * 800 + 3 * 300) * 0.01f));
    CHECK(field.heightAt(1, 3) == Approx(12.5f + (1 * 800 + 3 * 300) * 0.01f));
    // And the sim view agrees with the float accessor inside the flattened area.
    CHECK(terrain.heightAt(Fx::fromInt(32), Fx::fromInt(32)) == Fx::fromInt(50));
}

TEST_CASE("flattenRect clamps to the map instead of writing outside it") {
    // Retail logs "Attempted to flatten terrain outside map boundary!" and returns only
    // when the CLAMPED rect is empty; a rect that overlaps the map still writes the part
    // that is on it (`0x75251b`-`0x75255c`).
    rm::HeightField field = rampField();
    Terrain terrain{field};

    terrain.flattenRect(Fx::fromInt(-64), Fx::fromInt(-64), Fx::fromInt(16), Fx::fromInt(16),
                        Fx::fromInt(50));

    CHECK(field.heightAt(0, 0) == Approx(50.0f));
    CHECK(field.heightAt(2, 2) == Approx(50.0f));
    CHECK(field.heightAt(3, 3) == Approx(12.5f + (3 * 800 + 3 * 300) * 0.01f));
}

namespace {

/// A skirted structure def: the shape `Physics.FlattenSkirt` marks in retail (a factory
/// authors `SkirtSizeX/Z` in ogrids). `isMobile()` keys on speed, so a zero-speed def is
/// the structure case.
[[nodiscard]] rm::unitdef::UnitDef testFactory() {
    rm::unitdef::UnitDef def;
    def.name = "TESTFAC";
    def.skirtSquaresX = 4.0f;
    def.skirtSquaresZ = 4.0f;
    def.health = rm::sim::magFromFloat(1000.0f);
    return def;
}

/// Registers a def the way the scenario harness does: the type lives in the catalog and
/// `typeForBlueprint` is primed so `spawnUnit` skips its VFS model load.
[[nodiscard]] rm::UnitTypeIndex registerDef(rm::app::UnitScene& scene,
                                            const rm::unitdef::UnitDef& def) {
    scene.definitions.push_back(def);
    const auto type = scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(def), def.meshToElmos);
    const std::string path = "/units/" + def.name + "/" + def.name + "_unit.bp";
    scene.setPathForType(type, path);
    scene.typeForBlueprint.emplace(path, type);
    return type;
}

} // namespace

TEST_CASE("a structure spawn flattens its skirt to the placement height") {
    // C-286's sole shipped caller: `StructureUnit.FlattenSkirt` (defaultunits.lua:72)
    // runs from `OnCreate` on the Land layer and flattens the skirt rect to the unit's
    // own Y. Our unit entity materialises at spawn, so `spawnUnit` is the seam.
    rm::HeightField field = rampField();
    rm::app::UnitScene scene;
    rm::vfs::Vfs content;

    (void)registerDef(scene, testFactory());
    const rm::sim::Army army{};
    const auto spawned = rm::app::spawnUnit(scene, content, field,
                                            "/units/TESTFAC/TESTFAC_unit.bp",
                                            {32.0f, 0.0f, 32.0f}, army, rm::Brad{0});
    REQUIRE(spawned);

    // The placement point's height — what the structure stands on, and the elevation the
    // whole skirt takes: 12.5 + (4*800 + 4*300) * 0.01 = 56.5.
    const float flat = field.heightAtWorld(32.0f, 32.0f);
    CHECK(flat == Approx(56.5f));
    // A 4x4-ogrid skirt is 32 elmos across, so cells 2..5 around the centre are flat —
    // corners 2..6 inclusive, the boundary corner belonging to the flattened area…
    for (int z = 2; z <= 6; ++z) {
        for (int x = 2; x <= 6; ++x) {
            CHECK(field.heightAt(x, z) == Approx(56.5f));
        }
    }
    // …and the ramp resumes outside it.
    CHECK(field.heightAt(7, 7) == Approx(12.5f + (7 * 800 + 7 * 300) * 0.01f));

    // A unit standing inside the rect is re-seated on the new ground by the ordinary
    // movement pass — retail's `CUnitMotion+0x90` re-seat flag is our `placeOnMotionLayer`,
    // which re-reads the terrain every tick.
    rm::unitdef::UnitDef tank;
    tank.name = "TESTTANK";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 30.0f;
    tank.health = rm::sim::magFromFloat(100.0f);
    const auto tankType = registerDef(scene, tank);
    const auto parked = scene.store.spawn({
        .type = tankType,
        .transform = {.x = rm::sim::fxFromFloat(24.0f), .z = rm::sim::fxFromFloat(24.0f)},
        .motion = rm::app::motionFor(tank, 0),
        .health = rm::sim::initialHealth(tank.health),
    });
    rm::sim::Transform& at = scene.store.transforms()[parked.index];
    rm::sim::placeOnMotionLayer(at, scene.store.motion()[parked.index],
                                scene.terrain(field));
    CHECK(at.y == rm::sim::fxFromFloat(56.5f));
}

TEST_CASE("a mobile or skirtless spawn leaves the terrain alone") {
    // The gate is the skirt, not the category: retail's `Physics.FlattenSkirt` flag is
    // unparsed, and a skirtless structure's flatten is a retail no-op anyway — the rect
    // is empty.
    rm::HeightField field = rampField();
    rm::app::UnitScene scene;
    rm::vfs::Vfs content;

    rm::unitdef::UnitDef tank;
    tank.name = "TESTTANK";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 30.0f;
    tank.health = rm::sim::magFromFloat(100.0f);
    (void)registerDef(scene, tank);

    const rm::sim::Army army{};
    const auto spawned = rm::app::spawnUnit(scene, content, field,
                                            "/units/TESTTANK/TESTTANK_unit.bp",
                                            {32.0f, 0.0f, 32.0f}, army, rm::Brad{0});
    REQUIRE(spawned);
    CHECK(field.heightAt(4, 4) == Approx(56.5f));
    CHECK(field.heightAt(3, 3) == Approx(12.5f + (3 * 800 + 3 * 300) * 0.01f));
}
