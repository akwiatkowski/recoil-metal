// Tests for the mapinfo.lua height-override extractor. These pin the scanner's
// behaviour on synthetic input; tests/test_real_map.cpp checks it against the
// actual BAR mapinfo.lua it was written for.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/MapInfo.hpp"

using Catch::Approx;

TEST_CASE("the smf height override is extracted from a mapinfo table") {
    // Shaped like the real thing: nested table, trailing commas, comments.
    constexpr const char* kLua = R"LUA(
local mapinfo = {
    name = "Angel Crossing",
    mapfile = "maps/aw04.smf",
    smf = {
        minheight = -150.0,
        maxheight = 850.0,
    },
}
return mapinfo
)LUA";

    const auto range = rm::mapinfo::findVerticalRange(kLua);

    REQUIRE(range.has_value());
    REQUIRE(range->minHeight == Approx(-150.0f));
    REQUIRE(range->maxHeight == Approx(850.0f));
}

TEST_CASE("key matching is case-insensitive") {
    const auto range = rm::mapinfo::findVerticalRange(
        "smf = { minHeight = 12.5, maxHeight = 99 }");

    REQUIRE(range.has_value());
    REQUIRE(range->minHeight == Approx(12.5f));
    REQUIRE(range->maxHeight == Approx(99.0f));
}

TEST_CASE("a bare 'height' key is not mistaken for a height override") {
    // Real mapinfo.lua files contain unrelated `height` keys — the grass block
    // has `height = "80%"`. Latching onto one of those would be silent nonsense.
    const auto range = rm::mapinfo::findVerticalRange(
        "grass = { height = 80 }, water = { height = 12 }");

    REQUIRE_FALSE(range.has_value());
}

TEST_CASE("both keys are required, never one") {
    // Mixing a Lua minimum with a header maximum would produce a plausible but
    // wrong vertical scale, which is the exact failure mode this module exists
    // to prevent.
    REQUIRE_FALSE(rm::mapinfo::findVerticalRange("smf = { minheight = -150 }").has_value());
    REQUIRE_FALSE(rm::mapinfo::findVerticalRange("smf = { maxheight = 850 }").has_value());
}

TEST_CASE("a computed or non-numeric value is declined rather than guessed") {
    // The scanner is not a Lua interpreter and must not pretend to be one.
    const auto range = rm::mapinfo::findVerticalRange(
        "smf = { minheight = baseLevel - 10, maxheight = 850 }");

    REQUIRE_FALSE(range.has_value());
}

TEST_CASE("an inverted range is reported as written, not silently corrected") {
    // Min > max is legal — it means the raw domain runs downhill. Reordering it
    // here would be guessing at intent.
    const auto range = rm::mapinfo::findVerticalRange(
        "smf = { minheight = 850, maxheight = -150 }");

    REQUIRE(range.has_value());
    REQUIRE(range->minHeight == Approx(850.0f));
    REQUIRE(range->maxHeight == Approx(-150.0f));
}

TEST_CASE("an absent file yields no override rather than an error") {
    REQUIRE_FALSE(rm::mapinfo::findVerticalRangeInFile("/nonexistent/mapinfo.lua").has_value());
    REQUIRE_FALSE(rm::mapinfo::findBesideMap("/nonexistent/maps/foo.smf").has_value());
}
