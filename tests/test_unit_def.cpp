// Unit-definition tests. The conversions are the point: three of the four
// numbers a definition contributes are authored in units that are not the ones
// the sim uses, and each is wrong in a way that looks like something else.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Movement.hpp"
#include "core/unit/UnitDef.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>

using Catch::Approx;

namespace {

/// Writes a definition to a temporary file and reads it back.
[[nodiscard]] std::expected<rm::unitdef::UnitDef, rm::lua::ParseError> parse(
    const std::string& source) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm_test_unitdef.lua";
    {
        std::ofstream out{path};
        out << source;
    }
    auto def = rm::unitdef::loadFile(path);
    std::filesystem::remove(path);
    return def;
}

} // namespace

TEST_CASE("a unit definition yields its name, model and stats") {
    // BAR's Pawn, trimmed to the fields this engine acts on.
    const auto def = parse(R"(
return {
	armpw = {
		health = 370,
		footprintx = 2,
		footprintz = 2,
		maxslope = 17,
		maxwaterdepth = 12,
		objectname = "Units/ARMPW.s3o",
		speed = 87,
		turnrate = 1214.40002,
	},
}
)");

    REQUIRE(def.has_value());

    // The NAME comes from the table's key, not the filename — that is what the
    // engine keys on.
    CHECK(def->name == "armpw");
    CHECK(def->modelPath == "Units/ARMPW.s3o");
    CHECK(def->health == Approx(370.0f));

    // Speed is already elmos/second in the modern field.
    CHECK(def->speedElmosPerSecond == Approx(87.0f));

    // Turn rate is circle divisions per FRAME, over 65536 to the circle, at 30
    // frames a second. Read as radians per second it would be ~700 revolutions
    // a second; read as degrees per second, still 300x too fast.
    const float expectedTurn = 1214.40002f / 65536.0f * 2.0f * std::numbers::pi_v<float>
                             * static_cast<float>(rm::sim::kTicksPerSecond);
    CHECK(def->turnRateRadiansPerSecond == Approx(expectedTurn));
    CHECK(def->turnRateRadiansPerSecond == Approx(3.4934f).margin(0.01));

    // Slope and depth pass through in the units the sim already expects.
    CHECK(def->maxSlopeDegrees == Approx(17.0f));
    CHECK(def->maxWaterDepthElmos == Approx(12.0f));

    // Footprints are scaled by the engine's factor of 2, so a file saying 2 is
    // 4 squares — 32 elmos — across.
    CHECK(def->footprintSquaresX == 4);
    CHECK(def->footprintSquaresZ == 4);
    CHECK(def->footprintRadiusElmos() == Approx(16.0f));

    CHECK(def->isMobile());
}

TEST_CASE("a building is read as an immobile unit rather than rejected") {
    const auto def = parse(R"(
return {
	armsolar = {
		health = 260,
		footprintx = 5,
		footprintz = 5,
		objectname = "Units/ARMSOLAR.s3o",
	},
}
)");

    REQUIRE(def.has_value());
    CHECK_FALSE(def->isMobile());
    CHECK(def->speedElmosPerSecond == Approx(0.0f));
    CHECK(def->turnRateRadiansPerSecond == Approx(0.0f));
    // Its footprint is still meaningful — it is a thing to walk around.
    CHECK(def->footprintSquaresX == 10);
    CHECK(def->footprintRadiusElmos() == Approx(40.0f));
}

TEST_CASE("an absent field leaves its default rather than reading as zero") {
    // A definition that omits a footprint gets the engine's minimum of 1, not 0
    // — a zero-sized unit would occupy no space at all in a collision test.
    const auto def = parse("return { thing = { speed = 10 } }");

    REQUIRE(def.has_value());
    CHECK(def->footprintSquaresX == 2);
    CHECK(def->footprintSquaresZ == 2);
    CHECK(def->maxSlopeDegrees == Approx(0.0f));
}

TEST_CASE("a definition that is not a named table is an error") {
    CHECK_FALSE(parse("return { }").has_value());
    CHECK_FALSE(parse("return { armpw = 5 }").has_value());
}

TEST_CASE("a missing file is an error, not an empty definition") {
    const auto def = rm::unitdef::loadFile("/nonexistent/nowhere.lua");
    CHECK_FALSE(def.has_value());
}

TEST_CASE("model resolution is by basename and case-insensitive") {
    // Definitions say `Units/ARMPW.s3o`; the file on disk is `armpw.s3o`, and
    // the directory in the name does not always match the one it lives in.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "rm_test_objects3d";
    std::filesystem::create_directories(root / "Units");
    { std::ofstream out{root / "Units" / "armpw.s3o"}; out << "x"; }

    CHECK(rm::unitdef::resolveModel(root, "Units/ARMPW.s3o").filename() == "armpw.s3o");
    CHECK(rm::unitdef::resolveModel(root, "somewhere/else/ArmPw.S3O").filename() == "armpw.s3o");
    CHECK(rm::unitdef::resolveModel(root, "Units/NOSUCH.s3o").empty());
    CHECK(rm::unitdef::resolveModel(root, "").empty());
    CHECK(rm::unitdef::resolveModel("/nonexistent", "Units/ARMPW.s3o").empty());

    std::filesystem::remove_all(root);
}
