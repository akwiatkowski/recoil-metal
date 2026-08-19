// Supreme Commander unit blueprints, read into the shared UnitDef.
//
// The conversions are what this file is really about — ogrids to elmos, degrees
// to radians, a motion class to a slope limit — and every one of them is a place
// a plausible-looking wrong number can hide. The corpus test beside this one
// (test_real_unit_blueprints.cpp) checks all 568 shipped files; these pin the
// arithmetic against values worked out by hand.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Pathfinding.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>

using Catch::Approx;
using rm::unitdef::MotionType;

namespace {

// Blueprints are read from disk rather than from a string, because that is the
// entry point the app uses and the file NAME carries the unit's id — there being
// no BlueprintId in any of the 568 shipped files. A temporary file is the honest
// way to test that.
class Blueprint {
public:
    explicit Blueprint(std::string fileName, std::string_view source)
        : path_{std::filesystem::temp_directory_path() / "rm_unitbp_test" / fileName} {
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream out{path_};
        out << source;
    }
    Blueprint(const Blueprint&) = delete;
    Blueprint& operator=(const Blueprint&) = delete;
    Blueprint(Blueprint&&) = delete;
    Blueprint& operator=(Blueprint&&) = delete;
    ~Blueprint() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// A medium tank's real numbers, from units/UEL0201/UEL0201_unit.bp.
constexpr std::string_view kMediumTank = R"(
UnitBlueprint {
    Defense = { MaxHealth = 300 },
    Display = { UniformScale = 0.07 },
    Physics = {
        MaxSpeed = 3.4,
        MotionType = 'RULEUMT_Land',
        TurnRate = 90,
    },
    SizeX = 0.7,
    SizeY = 0.55,
    SizeZ = 0.9,
}
)";

} // namespace

TEST_CASE("a unit blueprint's numbers arrive in the engine's own units") {
    const Blueprint bp{"UEL0201_unit.bp", kMediumTank};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    // The id is the FILE NAME with its suffix removed. Nothing inside says it.
    CHECK(def->name == "UEL0201");

    // 3.4 ogrids/s x 8 elmos/ogrid.
    CHECK(def->speedElmosPerSecond == Approx(27.2f));
    CHECK(def->isMobile());

    // 90 degrees/s in radians — no 65536 and no tick rate, unlike BAR's.
    CHECK(def->turnRateRadiansPerSecond == Approx(std::numbers::pi_v<float> / 2.0f));

    CHECK(def->health == Approx(300.0f));
    CHECK(def->motion == MotionType::Land);
    CHECK_FALSE(def->canFly);

    // Half the larger side, 0.9 ogrids, in elmos: 0.5 * 0.9 * 8.
    CHECK(def->collisionRadiusElmos == Approx(3.6f));

    // 0.07 to ogrids and 8 to elmos, as ONE factor.
    CHECK(def->meshToElmos == Approx(0.56f));
}

TEST_CASE("a fractional size keeps its precision instead of rounding to squares") {
    // 418 of the 568 shipped sizes are fractional and 154 are under a single
    // ogrid, the smallest 0.01 — which as whole squares would be a radius of 4
    // elmos instead of 0.04, a hundredfold, and would space a crowd of them out
    // by a distance none of them needs.
    const Blueprint bp{"TINY_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 0.01, SizeZ = 0.01,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->collisionRadiusElmos == Approx(0.04f));

    // The BUILD footprint is whole squares by nature, and rounds UP: a unit must
    // never claim less ground than it stands on, and zero squares would let it be
    // placed inside a wall.
    CHECK(def->footprintSquaresX == 1);
    CHECK(def->footprintSquaresZ == 1);
}

TEST_CASE("a structure's own build footprint wins over its collision size") {
    // 363 of 568 state one, essentially the structures — which are the things
    // that occupy a grid. Where it exists it is the authority.
    const Blueprint bp{"UEB0101_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 5, SizeZ = 5 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 4.15, SizeZ = 4.35,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->footprintSquaresX == 5);
    CHECK(def->footprintSquaresZ == 5);
    // ...and the collision radius still comes from the collision box.
    CHECK(def->collisionRadiusElmos == Approx(0.5f * 4.35f * 8.0f));
    CHECK_FALSE(def->isMobile());
    CHECK(def->motion == MotionType::None);
}

TEST_CASE("the motion class decides where a unit may go, since the file does not") {
    // A `.bp` states no slope limit and no wading depth for ANY of the 568. The
    // only slope figure the format carries is Footprint.MaxSlope, a gradient on
    // aircraft landing sites. So these come from the class.
    struct Case {
        std::string_view motion;
        MotionType expected;
        bool onGround;
        bool entersWater;
    };
    const Case cases[] = {
        {"RULEUMT_Land", MotionType::Land, true, false},
        {"RULEUMT_Hover", MotionType::Hover, true, true},
        {"RULEUMT_Amphibious", MotionType::Amphibious, true, true},
        {"RULEUMT_AmphibiousFloating", MotionType::AmphibiousFloating, true, true},
        {"RULEUMT_Air", MotionType::Air, false, false},
        {"RULEUMT_Water", MotionType::Water, false, false},
        {"RULEUMT_SurfacingSub", MotionType::SurfacingSub, false, false},
        {"RULEUMT_None", MotionType::None, false, false},
    };

    for (const Case& c : cases) {
        const std::string source = std::string{"UnitBlueprint { SizeX = 1, SizeZ = 1, Physics = { "}
                                 + "MotionType = '" + std::string{c.motion} + "' } }";
        const Blueprint bp{"X_unit.bp", source};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        CHECK(def->motion == c.expected);
        CHECK(rm::unitdef::travelsOnGround(def->motion) == c.onGround);

        if (c.onGround) {
            // The engine's own limit, which is also what BAR's ground units
            // authorise — the two families agree without either being bent.
            CHECK(def->maxSlopeDegrees == Approx(rm::sim::kDefaultMaxSlopeDegrees));
        }
        // Land is the sharp one: Supreme Commander's land units do not ford at
        // all, where a BAR unit wades 12 elmos and a shoreline is passable.
        CHECK((def->maxWaterDepthElmos > 0.0f) == c.entersWater);
    }
}

TEST_CASE("an aircraft is known by its motion class") {
    const Blueprint bp{"UEA0101_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 10, TurnRate = 180 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->canFly);
    CHECK_FALSE(rm::unitdef::travelsOnGround(def->motion));
}

TEST_CASE("a blueprint with no Physics table is refused, not read as a building") {
    // A building has a Physics table stating RULEUMT_None. A blueprint with none
    // at all is a file this reader has misunderstood, and defaulting it to
    // immobile would present as a unit that mysteriously will not move.
    const Blueprint bp{"BROKEN_unit.bp", "UnitBlueprint { SizeX = 1, SizeZ = 1 }"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE_FALSE(def.has_value());
    CHECK(def.error().message.find("Physics") != std::string::npos);
}

TEST_CASE("an unknown motion class is refused rather than treated as immobile") {
    // Nothing in the retail corpus is unknown; a mod inventing a class should
    // say so rather than ship a unit that silently never moves.
    const Blueprint bp{"MODDED_unit.bp", R"(
        UnitBlueprint { SizeX = 1, SizeZ = 1, Physics = { MotionType = 'RULEUMT_Teleport' } }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE_FALSE(def.has_value());
    CHECK(def.error().message.find("RULEUMT_Teleport") != std::string::npos);
}

TEST_CASE("a missing blueprint is a parse error, not a crash") {
    const auto def = rm::unitbp::loadFile("/nonexistent/NOPE_unit.bp");
    REQUIRE_FALSE(def.has_value());
}

TEST_CASE("a blueprint names no mesh, so an absent one resolves to nothing") {
    // The convention is `X_unit.bp` beside `X_lod0.scm`. No file, no path — and
    // emphatically not a guess, since a path that does not exist would be handed
    // to the model loader as if it might.
    const Blueprint bp{"ALONE_unit.bp", kMediumTank};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->modelPath.empty());  // by convention, so nothing to state
    CHECK(rm::unitbp::resolveMesh(*def, bp.path()).empty());
}

TEST_CASE("MeshName is kept as the file states it, unresolved") {
    // Two forms, told apart by the extension, and `modelPath` holds either
    // verbatim — exactly as it holds BAR's `objectname`. Resolution is a separate
    // step because it touches the filesystem.
    const Blueprint asPath{"OPC1001_unit.bp", R"(
        UnitBlueprint {
            Display = { Mesh = { LODs = { { MeshName = '/Env/Props/Warehouse_lod0.scm' } } } },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto byPath = rm::unitbp::loadFile(asPath.path());
    REQUIRE(byPath.has_value());
    CHECK(byPath->modelPath == "/Env/Props/Warehouse_lod0.scm");
    // A VFS path with no root to join it to resolves to nothing rather than to
    // the filesystem root, which is what dropping the leading slash would do.
    CHECK(rm::unitbp::resolveMesh(*byPath, asPath.path()).empty());

    const Blueprint asId{"XAC8101_unit.bp", R"(
        UnitBlueprint {
            Display = { Mesh = { LODs = { { MeshName = 'UEC1101' } } } },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto byId = rm::unitbp::loadFile(asId.path());
    REQUIRE(byId.has_value());
    CHECK(byId->modelPath == "UEC1101");
}

TEST_CASE("a scale of zero is read as stated, not corrected to one") {
    // Two shipped blueprints do this — unused Seraphim civilian entries. A mesh
    // scaled to nothing is what the file says; inventing a 1 would be inventing
    // content, and the corpus test records the count so it cannot drift unnoticed.
    const Blueprint bp{"XSC9010_unit.bp", R"(
        UnitBlueprint {
            Display = { UniformScale = 0 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->meshToElmos == Approx(0.0f));
}
