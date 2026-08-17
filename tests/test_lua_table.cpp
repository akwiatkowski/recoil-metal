// Tests for the Lua table-literal reader.
//
// The contract worth defending here is the *refusal*: this parses data, never
// expressions. A reader that quietly evaluated — or worse, guessed — would put
// us straight back to the silent-wrongness class of bug that made reading
// mapinfo.lua necessary at all.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/lua/LuaTable.hpp"

using Catch::Approx;
using rm::lua::Value;

TEST_CASE("a flat table of scalars parses") {
    const auto root = rm::lua::parseTable(
        R"(local t = { name = "Angel Crossing", version = 1.4, playable = true, extra = nil })");

    REQUIRE(root.has_value());
    REQUIRE(root->isTable());
    REQUIRE(root->stringAt("name") == "Angel Crossing");
    REQUIRE(root->numberAt("version") == Approx(1.4));
    REQUIRE(root->find("playable")->asBoolean() == true);
    REQUIRE(root->find("extra")->isNil());
    REQUIRE(root->find("absent") == nullptr);
}

TEST_CASE("nested tables are reachable by path") {
    const auto root = rm::lua::parseTable(R"(
        local mapinfo = {
            smf = {
                minheight = -150.0,
                maxheight = 850.0,
                smtFileName0 = "maps/aw04.smt",
            },
        }
        return mapinfo
    )");

    REQUIRE(root.has_value());
    REQUIRE(root->path("smf", "minheight")->asNumber() == Approx(-150.0));
    REQUIRE(root->path("smf", "maxheight")->asNumber() == Approx(850.0));
    REQUIRE(root->path("smf", "smtFileName0")->asString() == "maps/aw04.smt");
    REQUIRE(root->path("smf", "nope") == nullptr);
    REQUIRE(root->path("nope", "minheight") == nullptr);
}

TEST_CASE("comments in both forms are ignored") {
    const auto root = rm::lua::parseTable(R"(
        -- a line comment
        local t = {
            a = 1, -- trailing
            --[[ a long
                 comment spanning lines ]]
            b = 2,
        }
    )");

    REQUIRE(root.has_value());
    REQUIRE(root->numberAt("a") == Approx(1.0));
    REQUIRE(root->numberAt("b") == Approx(2.0));
}

TEST_CASE("number literals cover the forms Lua accepts") {
    const auto root = rm::lua::parseTable(
        R"(local t = { neg = -150, frac = 0.0078125, exp = 1.5e3, hex = 0x20, dot = .5 })");

    REQUIRE(root.has_value());
    REQUIRE(root->numberAt("neg") == Approx(-150.0));
    REQUIRE(root->numberAt("frac") == Approx(0.0078125));
    REQUIRE(root->numberAt("exp") == Approx(1500.0));
    REQUIRE(root->numberAt("hex") == Approx(32.0));
    REQUIRE(root->numberAt("dot") == Approx(0.5));
}

TEST_CASE("strings handle quotes, escapes and long brackets") {
    const auto root = rm::lua::parseTable(R"LUA(
        local t = {
            dq = "double",
            sq = 'single',
            esc = "a\"b\\c",
            long = [[raw \n not an escape]],
        }
    )LUA");

    REQUIRE(root.has_value());
    REQUIRE(root->stringAt("dq") == "double");
    REQUIRE(root->stringAt("sq") == "single");
    REQUIRE(root->stringAt("esc") == "a\"b\\c");
    REQUIRE(root->stringAt("long") == "raw \\n not an escape");
}

TEST_CASE("positional entries land in the array part") {
    const auto root = rm::lua::parseTable("local t = { texScales = { 0.02, 0.03, 0.04, 0.05 } }");

    REQUIRE(root.has_value());
    const Value* scales = root->find("texScales");
    REQUIRE(scales != nullptr);
    REQUIRE(scales->items.size() == 4);
    REQUIRE(scales->items[0].asNumber() == Approx(0.02));
    REQUIRE(scales->items[3].asNumber() == Approx(0.05));
}

TEST_CASE("both comma and semicolon separate entries, trailing ones are fine") {
    const auto root = rm::lua::parseTable("local t = { a = 1; b = 2, c = 3, }");

    REQUIRE(root.has_value());
    REQUIRE(root->numberAt("a") == Approx(1.0));
    REQUIRE(root->numberAt("c") == Approx(3.0));
}

TEST_CASE("bracketed keys are supported") {
    const auto root = rm::lua::parseTable(
        R"(local t = { terrainTypes = { [0] = { name = "Default" } }, ["quoted"] = 7 })");

    REQUIRE(root.has_value());
    REQUIRE(root->numberAt("quoted") == Approx(7.0));
    REQUIRE(root->path("terrainTypes", "0", "name")->asString() == "Default");
}

TEST_CASE("a computed value is refused, not guessed") {
    // The whole point. `baseLevel - 10` needs evaluation; returning 0, or the
    // value of `baseLevel`, or skipping the key silently would each be a lie.
    const auto root = rm::lua::parseTable("local t = { minheight = baseLevel - 10 }");

    REQUIRE_FALSE(root.has_value());
    REQUIRE(root.error().message.find("requires evaluation") != std::string::npos);
    REQUIRE(root.error().line > 0);
}

TEST_CASE("a function call is refused") {
    const auto root = rm::lua::parseTable(R"(local t = { list = VFS.DirList("mapconfig/") })");

    REQUIRE_FALSE(root.has_value());
}

TEST_CASE("an unterminated table is an error, not a partial result") {
    const auto root = rm::lua::parseTable("local t = { a = 1, b = 2");

    REQUIRE_FALSE(root.has_value());
    REQUIRE(root.error().message.find("unterminated") != std::string::npos);
}

TEST_CASE("source with no table at all is an error") {
    REQUIRE_FALSE(rm::lua::parseTable("return 42").has_value());
    REQUIRE_FALSE(rm::lua::parseTable("").has_value());
}

TEST_CASE("errors carry a line number that points at the problem") {
    const auto root = rm::lua::parseTable("local t = {\n  a = 1,\n  b = someVariable,\n}");

    REQUIRE_FALSE(root.has_value());
    REQUIRE(root.error().line == 3);
}
