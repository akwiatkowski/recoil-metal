// What ground a MOTION CLASS crosses — the correction §7 P3.4 names.
//
// Passability used to be keyed on `UnitDef::maxSlopeDegrees` and `maxWaterDepthElmos`, read
// straight off the blueprint. Two cited reports say those are the wrong fields:
// `01-unitdefs-movedefs.md §3.2` calls them "buildings only … Mobile ground units take slope from
// the MoveDef", and `recoil-engine-map.md §5` states it as a gotcha. So the old keying asked a
// building-placement question and used the answer for routing.
#include <catch2/catch_test_macros.hpp>

#include "core/data/MoveDef.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>

using rm::data::MoveDef;
using rm::data::moveDefFor;
using rm::unitdef::MotionType;
using rm::unitdef::UnitDef;

namespace {

/// A bay: flat land at y = 20, with a channel of water down the middle.
///
/// `waterLevel` is 10 elmos, so the channel floor at y = 0 is 10 elmos deep — passable to
/// something that wades or hovers and not to something that does not.
[[nodiscard]] rm::HeightField bayField() {
    rm::HeightField field;
    field.squaresX = 64;
    field.squaresZ = 64;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{20});
    for (int z = 0; z <= field.squaresZ; ++z) {
        for (int x = 28; x <= 36; ++x) {
            field.raw[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.verticesX())
                      + static_cast<std::size_t>(x)] = 0;
        }
    }
    return field;
}

[[nodiscard]] UnitDef unitOf(MotionType motion, float statedSlope, float statedDepth) {
    UnitDef def;
    def.name = "synthetic";
    def.motion = motion;
    // The fields that must be IGNORED for routing. Set to absurd values on purpose: if the
    // routing ever reads them again, these are what it will read.
    def.maxSlopeDegrees = statedSlope;
    def.maxWaterDepthElmos = statedDepth;
    return def;
}

} // namespace

TEST_CASE("a unit's own slope and depth are ignored for routing") {
    // THE CORRECTION, stated directly. Both units declare limits that would make them cross
    // anything; both are `RULEUMT_Land`, which crosses no water.
    const UnitDef liar = unitOf(MotionType::Land, 89.0f, 5000.0f);
    const MoveDef move = moveDefFor(liar);

    CHECK(move.maxWaterDepthElmos == 0.0f);   // not 5000
    CHECK(move.maxSlopeDegrees == 60.0f);     // Recoil's Tank/KBot default, not 89
    CHECK_FALSE(rm::data::canCrossWater(liar));
}

TEST_CASE("each motion class crosses what its class crosses") {
    // Land is dry land. This is OURS where Recoil differs — Recoil defaults a tank to 1e6 elmos
    // of depth, which is right for a game whose tanks ford rivers and wrong for one where
    // `RULEUMT_Land` and `RULEUMT_Amphibious` are separate classes the corpus uses heavily.
    CHECK(moveDefFor(MotionType::Land).maxWaterDepthElmos == 0.0f);
    CHECK(moveDefFor(MotionType::Land).maxSlopeDegrees == 60.0f);

    // A hovercraft cannot climb what a tank can — 15 degrees is Recoil's Hover default — and
    // depth is irrelevant to it because it is over the surface, not in it.
    CHECK(moveDefFor(MotionType::Hover).maxSlopeDegrees == 15.0f);
    CHECK(moveDefFor(MotionType::Hover).maxWaterDepthElmos > 0.0f);

    // Amphibious walks the seabed: the same legs, so the same slope, and no depth limit.
    CHECK(moveDefFor(MotionType::Amphibious).maxSlopeDegrees == 60.0f);
    CHECK(moveDefFor(MotionType::Amphibious).maxWaterDepthElmos > 0.0f);
    CHECK(moveDefFor(MotionType::AmphibiousFloating).maxWaterDepthElmos > 0.0f);

    // And the classes the ground grid cannot describe say so rather than being approximated.
    CHECK_FALSE(moveDefFor(MotionType::None).usesGroundGrid);
    CHECK_FALSE(moveDefFor(MotionType::Air).usesGroundGrid);
    CHECK_FALSE(moveDefFor(MotionType::Water).usesGroundGrid);
    CHECK_FALSE(moveDefFor(MotionType::SurfacingSub).usesGroundGrid);

    // The bounded SurfacingSub slice shares the water domain.
    CHECK(moveDefFor(MotionType::Water).usesSurfaceWaterGrid);
    CHECK(moveDefFor(MotionType::SurfacingSub).usesSurfaceWaterGrid);

    CHECK(moveDefFor(MotionType::Land).usesGroundGrid);
    CHECK(moveDefFor(MotionType::Amphibious).usesGroundGrid);
}

TEST_CASE("an immobile naval factory uses the surface-water domain") {
    UnitDef yard = unitOf(MotionType::None, 0.0f, 0.0f);
    yard.categories = {"FACTORY", "NAVAL", "STRUCTURE"};

    const MoveDef move = moveDefFor(yard);
    CHECK(move.usesSurfaceWaterGrid);
    CHECK_FALSE(move.usesGroundGrid);
}

TEST_CASE("water blocks a land unit and passes an amphibious one, on the same map") {
    // §7 P3.4's STATED TEST, and the reason the correction matters: before it, both of these
    // units routed on a grid built from whatever their blueprint happened to state about placing
    // a building, so the answer had nothing to do with what they can cross.
    const rm::HeightField field = bayField();
    constexpr float kWaterLevel = 10.0f;

    const UnitDef tank = unitOf(MotionType::Land, 0.0f, 0.0f);
    const UnitDef commander = unitOf(MotionType::Amphibious, 0.0f, 0.0f);

    const MoveDef tankMove = moveDefFor(tank);
    const MoveDef acuMove = moveDefFor(commander);

    const rm::sim::PassabilityGrid dry = rm::sim::buildPassability(
        field, kWaterLevel, tankMove.maxSlopeDegrees, tankMove.maxWaterDepthElmos);
    const rm::sim::PassabilityGrid wet = rm::sim::buildPassability(
        field, kWaterLevel, acuMove.maxSlopeDegrees, acuMove.maxWaterDepthElmos);

    // Mid-channel: 10 elmos of water over the floor.
    const int channelX = dry.cellAtWorld(rm::sim::Fx::fromInt(32 * rm::kSquareSize));
    const int channelZ = dry.cellAtWorld(rm::sim::Fx::fromInt(32 * rm::kSquareSize));

    CHECK_FALSE(dry.passableAt(channelX, channelZ));  // a tank will not wade
    CHECK(wet.passableAt(channelX, channelZ));        // the commander walks under

    // And both cross the dry land either side, so the grids differ only where they should.
    const int shoreX = dry.cellAtWorld(rm::sim::Fx::fromInt(8 * rm::kSquareSize));
    CHECK(dry.passableAt(shoreX, channelZ));
    CHECK(wet.passableAt(shoreX, channelZ));
}

TEST_CASE("all four commanders are amphibious, which makes it a fixed cost") {
    // `14-blueprint-census.md §9.1`. The very first unit of every match needs amphibious
    // movement, so it is not a later feature — which is why this is a test rather than a note.
    const std::filesystem::path root = [] {
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path{home} / "projects/llm/input/faf/units";
        }
        return std::filesystem::path{};
    }();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    for (const std::string_view id : {"UEL0001", "UAL0001", "URL0001", "XSL0001"}) {
        const std::filesystem::path path = root / id / (std::string{id} + "_unit.bp");
        if (!std::filesystem::exists(path)) {
            SKIP("no " + path.string());
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());
        INFO(id << " motion class");
        CHECK(def->motion == MotionType::Amphibious);
        CHECK(rm::data::canCrossWater(*def));
    }
}
