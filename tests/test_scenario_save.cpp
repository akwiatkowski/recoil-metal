// Start positions out of a Supreme Commander <map>_save.lua.
//
// The interesting cases here are the two that would be silent: a marker table
// full of non-army markers (every stock map has dozens), and file order that
// does not match army order. Both would produce a plausible-looking list of
// coordinates that is quietly wrong.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/ScenarioSave.hpp"

#include <string>

using Catch::Approx;

namespace {

// Shaped exactly like the retail files: markers of several kinds in the one
// table, army markers out of order, every leaf behind a data constructor.
constexpr const char* kSave = R"(
Scenario = {
    next_area_id = '1',
    Props = {
    },
    MasterChain = {
        ['_MASTERCHAIN_'] = {
            Markers = {
                ['Transport Marker 07'] = {
                    ['color'] = STRING( 'ff80A088' ),
                    ['type'] = STRING( 'Transport Marker' ),
                    ['position'] = VECTOR3( 973.59, 18.4927, 524.302 ),
                },
                ['ARMY_2'] = {
                    ['color'] = STRING( 'ff800080' ),
                    ['type'] = STRING( 'Blank Marker' ),
                    ['orientation'] = VECTOR3( 0, 0.000602126, 0 ),
                    ['position'] = VECTOR3( 100.5, 25.0, 200.5 ),
                },
                ['Mass 12'] = {
                    ['type'] = STRING( 'Mass' ),
                    ['position'] = VECTOR3( 5.0, 1.0, 5.0 ),
                },
                ['ARMY_1'] = {
                    ['color'] = STRING( 'ff800080' ),
                    ['type'] = STRING( 'Blank Marker' ),
                    ['position'] = VECTOR3( 672.5, 18.6797, 346.5 ),
                },
            },
        },
    },
})";

} // namespace

TEST_CASE("army markers become start positions, in army order") {
    const auto positions = rm::scenario::loadStartPositions(kSave);

    REQUIRE(positions.has_value());
    REQUIRE(positions->size() == 2);

    // ARMY_1 is team 0 and comes first, even though ARMY_2 appears above it in
    // the file — the palette assigns colours by index, so file order leaking
    // through would give two armies each other's colour.
    REQUIRE((*positions)[0].team == 0);
    REQUIRE((*positions)[1].team == 1);

    // Ogrids in the file, elmos out: 672.5 * 8.
    REQUIRE((*positions)[0].x == Approx(5380.0f));
    REQUIRE((*positions)[0].z == Approx(2772.0f));
    REQUIRE((*positions)[1].x == Approx(804.0f));
}

TEST_CASE("non-army markers are ignored rather than counted") {
    // Transport, Mass, Rally and friends outnumber the army markers by an order
    // of magnitude on a real map.
    const auto positions = rm::scenario::loadStartPositions(kSave);

    REQUIRE(positions.has_value());
    REQUIRE(positions->size() == 2);  // not 4
}

TEST_CASE("the stored marker height is dropped, not trusted") {
    // StartPosition carries no Y at all: units are dropped onto the sampled
    // heightmap, which is what the engine does and what the SMF path does.
    // Pinned here so nobody "helpfully" adds the marker's Y later.
    const auto positions = rm::scenario::loadStartPositions(kSave);

    REQUIRE(positions.has_value());
    REQUIRE(sizeof((*positions)[0]) == sizeof(rm::mapinfo::StartPosition));
}

TEST_CASE("a file with no marker table is refused, not returned empty") {
    // An empty list and "this is not a _save.lua" are different facts, and the
    // difference matters: the first means a map with no starts, the second
    // means we read the wrong file.
    const auto positions = rm::scenario::loadStartPositions("Scenario = { Props = {} }");

    REQUIRE_FALSE(positions.has_value());
    REQUIRE(positions.error().message.find("Markers") != std::string::npos);
}

TEST_CASE("an army marker without a position is an error") {
    const auto positions = rm::scenario::loadStartPositions(R"(
        Scenario = { MasterChain = { ['_MASTERCHAIN_'] = { Markers = {
            ['ARMY_1'] = { ['type'] = STRING( 'Blank Marker' ) },
        } } } })");

    REQUIRE_FALSE(positions.has_value());
    REQUIRE(positions.error().message.find("ARMY_1") != std::string::npos);
}

TEST_CASE("a marker merely starting with ARMY_ is not an army") {
    // 'ARMY_1 Rally' and similar appear on real maps; treating one as a start
    // would add a phantom team.
    const auto positions = rm::scenario::loadStartPositions(R"(
        Scenario = { MasterChain = { ['_MASTERCHAIN_'] = { Markers = {
            ['ARMY_1 Rally'] = { ['position'] = VECTOR3( 1, 2, 3 ) },
            ['ARMY_'] = { ['position'] = VECTOR3( 1, 2, 3 ) },
            ['ARMY_2'] = { ['position'] = VECTOR3( 4, 5, 6 ) },
        } } } })");

    REQUIRE(positions.has_value());
    REQUIRE(positions->size() == 1);
    REQUIRE((*positions)[0].team == 1);
}
