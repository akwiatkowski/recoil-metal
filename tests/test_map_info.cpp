// Tests for the mapinfo.lua reader. These pin behaviour on synthetic input;
// tests/test_real_map.cpp checks it against the actual BAR mapinfo.lua.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/MapInfo.hpp"

using Catch::Approx;

TEST_CASE("the smf height override is read from a mapinfo table") {
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

    const auto info = rm::mapinfo::parse(kLua);

    REQUIRE(info.has_value());
    REQUIRE(info->verticalRange.has_value());
    REQUIRE(info->verticalRange->minHeight == Approx(-150.0f));
    REQUIRE(info->verticalRange->maxHeight == Approx(850.0f));
}

TEST_CASE("key matching is case-insensitive") {
    // Lua is case-sensitive but map authors are not: the engine documents these
    // as minHeight/maxHeight while real files write them lower-case. Missing an
    // override over capitalisation would render the map upside down.
    const auto info = rm::mapinfo::parse("local t = { SMF = { minHeight = 12.5, MaxHeight = 99 } }");

    REQUIRE(info.has_value());
    REQUIRE(info->verticalRange.has_value());
    REQUIRE(info->verticalRange->minHeight == Approx(12.5f));
    REQUIRE(info->verticalRange->maxHeight == Approx(99.0f));
}

TEST_CASE("a bare 'height' key is not mistaken for a height override") {
    // Real mapinfo.lua files carry unrelated `height` keys — the grass block has
    // `height = "80%"`. Structure, not substring matching, is what rules them out.
    const auto info = rm::mapinfo::parse(
        R"(local t = { grass = { height = 80 }, water = { height = 12 } })");

    REQUIRE(info.has_value());
    REQUIRE_FALSE(info->verticalRange.has_value());
}

TEST_CASE("both height keys are required, never one") {
    // Mixing a Lua minimum with a header maximum would produce a plausible but
    // wrong vertical scale.
    const auto onlyMin = rm::mapinfo::parse("local t = { smf = { minheight = -150 } }");
    REQUIRE(onlyMin.has_value());
    REQUIRE_FALSE(onlyMin->verticalRange.has_value());

    const auto onlyMax = rm::mapinfo::parse("local t = { smf = { maxheight = 850 } }");
    REQUIRE(onlyMax.has_value());
    REQUIRE_FALSE(onlyMax->verticalRange.has_value());
}

TEST_CASE("an inverted range is reported as written, not silently corrected") {
    // min > max is legal: it means the raw domain runs downhill. Reordering it
    // would be guessing at intent.
    const auto info = rm::mapinfo::parse("local t = { smf = { minheight = 850, maxheight = -150 } }");

    REQUIRE(info.has_value());
    REQUIRE(info->verticalRange->minHeight == Approx(850.0f));
    REQUIRE(info->verticalRange->maxHeight == Approx(-150.0f));
}

TEST_CASE("smt file name overrides are collected in index order") {
    const auto info = rm::mapinfo::parse(R"(
        local t = { smf = {
            smtFileName0 = "maps/first.smt",
            smtFileName1 = "maps/second.smt",
        } }
    )");

    REQUIRE(info.has_value());
    REQUIRE(info->smtFileNames.size() == 2);
    REQUIRE(info->smtFileNames[0] == "maps/first.smt");
    REQUIRE(info->smtFileNames[1] == "maps/second.smt");
}

TEST_CASE("a gap ends the smt name sequence rather than skipping it") {
    // The engine only applies these when the count matches the .smf's embedded
    // list, so a sequence with a hole must not silently compact into a shorter
    // one that happens to match.
    const auto info = rm::mapinfo::parse(R"(
        local t = { smf = {
            smtFileName0 = "maps/first.smt",
            smtFileName2 = "maps/third.smt",
        } }
    )");

    REQUIRE(info.has_value());
    REQUIRE(info->smtFileNames.size() == 1);
}

TEST_CASE("mapfile is read when present") {
    const auto info = rm::mapinfo::parse(R"(local t = { smf = { mapfile = "maps/aw04.smf" } })");

    REQUIRE(info.has_value());
    REQUIRE(info->mapFile.has_value());
    REQUIRE(*info->mapFile == "maps/aw04.smf");
}

TEST_CASE("a computed value is a parse error, so callers keep the header values") {
    const auto info = rm::mapinfo::parse("local t = { smf = { minheight = base - 10 } }");

    REQUIRE_FALSE(info.has_value());
}

TEST_CASE("an absent file yields an error, and an absent mapinfo yields no path") {
    REQUIRE_FALSE(rm::mapinfo::parseFile("/nonexistent/mapinfo.lua").has_value());
    REQUIRE_FALSE(rm::mapinfo::findBesideMap("/nonexistent/maps/foo.smf").has_value());
}
