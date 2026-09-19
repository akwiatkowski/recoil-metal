// `C-220`/`C-314`: the blueprint merge a mod's `.bp` files apply over the base
// ones — retail's `table.merged` plus `StoreBlueprint`, as the catalog sees it.
//
// These pin the four rules the claim states: recursive DEEP merge, arrays
// merged BY INDEX (`{'A','B','C'}` + `{'X'}` → `{'X','B','C'}`), `nil` doing
// nothing (no deletion), and `false` taking effect — plus the store-level
// behaviour: `Merge = true` merges only onto an existing id, a duplicate id
// without it is silent last-writer-wins, and registration walks the fixed
// group order alphabetically.
#include <catch2/catch_test_macros.hpp>

#include "core/lua/LuaTable.hpp"
#include "core/unit/UnitCatalog.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

[[nodiscard]] rm::lua::Value parse(std::string_view source) {
    auto value = rm::lua::parseTable(source);
    REQUIRE(value.has_value());
    return std::move(*value);
}

[[nodiscard]] std::string_view textAt(const rm::lua::Value& table,
                                      std::string_view key) {
    const rm::lua::Value* value = table.find(key);
    REQUIRE(value != nullptr);
    const std::optional<std::string_view> text = value->asString();
    REQUIRE(text.has_value());
    return *text;
}

} // namespace

TEST_CASE("C-220: a merge is recursive and deep, not a field swap",
          "[blueprint][merge]") {
    const rm::lua::Value base = parse(R"(
        { Economy = { BuildCostMass = 100, BuildTime = 50 },
          Defense = { MaxHealth = 300 },
          Name = 'base' }
    )");
    const rm::lua::Value overlay = parse(R"(
        { Economy = { BuildCostMass = 40 },
          Name = 'modded' }
    )");

    const rm::lua::Value result = rm::unit::merged(base, overlay);

    // The overlaid scalar wins...
    CHECK(result.path("Economy", "BuildCostMass")->number == 40.0);
    // ...the untouched sibling inside the SAME table survives — the merge
    // recursed rather than replacing Economy whole.
    CHECK(result.path("Economy", "BuildTime")->number == 50.0);
    // Untouched tables survive whole.
    CHECK(result.path("Defense", "MaxHealth")->number == 300.0);
    CHECK(textAt(result, "Name") == "modded");
}

TEST_CASE("C-220: arrays merge by index, not by replace or append",
          "[blueprint][merge]") {
    const rm::lua::Value base = parse("{ Categories = { 'A', 'B', 'C' } }");
    const rm::lua::Value overlay = parse("{ Categories = { 'X' } }");

    const rm::lua::Value result = rm::unit::merged(base, overlay);
    const rm::lua::Value* categories = result.find("Categories");
    REQUIRE(categories != nullptr);
    REQUIRE(categories->items.size() == 3);
    // The claim's own example: {'A','B','C'} merged with {'X'} is {'X','B','C'}.
    CHECK(categories->items[0].text == "X");
    CHECK(categories->items[1].text == "B");
    CHECK(categories->items[2].text == "C");
}

TEST_CASE("C-220: overlay items past the base's length append",
          "[blueprint][merge]") {
    const rm::lua::Value base = parse("{ Categories = { 'A' } }");
    const rm::lua::Value overlay = parse("{ Categories = { 'X', 'Y', 'Z' } }");

    const rm::lua::Value result = rm::unit::merged(base, overlay);
    const rm::lua::Value* categories = result.find("Categories");
    REQUIRE(categories != nullptr);
    REQUIRE(categories->items.size() == 3);
    CHECK(categories->items[0].text == "X");
    CHECK(categories->items[1].text == "Y");
    CHECK(categories->items[2].text == "Z");
}

TEST_CASE("C-220: nil does nothing and false takes effect",
          "[blueprint][merge]") {
    // `for k,v in t2` never visits a nil, so a field cannot be deleted by a
    // merge — while `false` is a value and lands like any other.
    const rm::lua::Value base = parse(R"(
        { Physics = { CanFly = true, MaxSpeed = 3.4 }, Name = 'base' }
    )");
    const rm::lua::Value overlay = parse(R"(
        { Physics = { CanFly = false, MaxSpeed = nil }, Name = nil }
    )");

    const rm::lua::Value result = rm::unit::merged(base, overlay);

    const rm::lua::Value* canFly = result.path("Physics", "CanFly");
    REQUIRE(canFly != nullptr);
    CHECK(canFly->asBoolean() == std::optional<bool>{false});
    // MaxSpeed = nil in the overlay leaves the base's value standing.
    CHECK(result.path("Physics", "MaxSpeed")->number == 3.4);
    // And a nil at the root cannot delete either.
    CHECK(textAt(result, "Name") == "base");
}

TEST_CASE("C-220: a scalar overlay replaces a table wholesale",
          "[blueprint][merge]") {
    const rm::lua::Value base = parse("{ Physics = { MaxSpeed = 3.4 } }");
    const rm::lua::Value overlay = parse("{ Physics = 'gone' }");

    const rm::lua::Value result = rm::unit::merged(base, overlay);
    CHECK(textAt(result, "Physics") == "gone");
}

TEST_CASE("C-314: Merge = true overlays the same-id base blueprint",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    const rm::unit::UnitCatalog::Outcome first = catalog.store(
        rm::unit::BlueprintGroup::Unit,
        parse("{ Economy = { BuildCostMass = 100, BuildTime = 50 } }"),
        "/units/UEL0201/UEL0201_unit.bp");
    CHECK(first == rm::unit::UnitCatalog::Outcome::Stored);

    // A mod's overlay: same unit id (from its own file name), Merge = true.
    const rm::unit::UnitCatalog::Outcome second = catalog.store(
        rm::unit::BlueprintGroup::Unit,
        parse("{ Merge = true, Economy = { BuildCostMass = 40 } }"),
        "/mods/cheap/units/UEL0201/UEL0201_unit.bp");
    CHECK(second == rm::unit::UnitCatalog::Outcome::Merged);

    const rm::lua::Value* bp = catalog.find(rm::unit::BlueprintGroup::Unit, "uel0201");
    REQUIRE(bp != nullptr);
    CHECK(bp->path("Economy", "BuildCostMass")->number == 40.0);
    CHECK(bp->path("Economy", "BuildTime")->number == 50.0);
    // The merge flag and the overlay's Source are not part of the result —
    // retail nils both before merging, and the base's Source survives.
    CHECK(bp->find("Merge") == nullptr);
    CHECK(textAt(*bp, "Source") == "/units/UEL0201/UEL0201_unit.bp");
}

TEST_CASE("C-220: Merge = true with no base stores whole, flag and all",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    const rm::unit::UnitCatalog::Outcome outcome = catalog.store(
        rm::unit::BlueprintGroup::Unit,
        parse("{ Merge = true, Economy = { BuildCostMass = 40 } }"),
        "/mods/new/units/XXX0001/XXX0001_unit.bp");
    CHECK(outcome == rm::unit::UnitCatalog::Outcome::Stored);

    const rm::lua::Value* bp = catalog.find(rm::unit::BlueprintGroup::Unit, "xxx0001");
    REQUIRE(bp != nullptr);
    CHECK(bp->path("Economy", "BuildCostMass")->number == 40.0);
}

TEST_CASE("C-220: a duplicate id without Merge is silent last-writer-wins",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    catalog.store(rm::unit::BlueprintGroup::Unit,
                  parse("{ Economy = { BuildCostMass = 100, BuildTime = 50 } }"),
                  "/units/UEL0201/UEL0201_unit.bp");
    const rm::unit::UnitCatalog::Outcome outcome = catalog.store(
        rm::unit::BlueprintGroup::Unit,
        parse("{ Economy = { BuildCostMass = 40 } }"),
        "/mods/repl/units/UEL0201/UEL0201_unit.bp");
    CHECK(outcome == rm::unit::UnitCatalog::Outcome::Replaced);

    const rm::lua::Value* bp = catalog.find(rm::unit::BlueprintGroup::Unit, "uel0201");
    REQUIRE(bp != nullptr);
    CHECK(bp->path("Economy", "BuildCostMass")->number == 40.0);
    // Replaced, not merged: the base's BuildTime is gone with the rest.
    CHECK(bp->path("Economy", "BuildTime") == nullptr);
}

TEST_CASE("C-314: an authored BlueprintId names the blueprint it overlays",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    catalog.store(rm::unit::BlueprintGroup::Unit,
                  parse("{ Economy = { BuildCostMass = 100 } }"),
                  "/units/DAA0206/DAA0206_unit.bp");

    // The shipped form — mods.scd's all_units.bp carries BlueprintId fields
    // inside one generated file, so the file name cannot be the id.
    const std::size_t stored = catalog.storeSource(R"(
        UnitBlueprint {
            BlueprintId = 'daa0206',
            Merge = true,
            Economy = { BuildCostMass = 40 },
        }
    )", "/mods/campaignbranch/all_units.bp");
    CHECK(stored == 1);

    const rm::lua::Value* bp = catalog.find(rm::unit::BlueprintGroup::Unit, "daa0206");
    REQUIRE(bp != nullptr);
    CHECK(bp->path("Economy", "BuildCostMass")->number == 40.0);
}

TEST_CASE("C-220: a .bp file's constructor names its group, and a broken one is skipped",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    const std::size_t stored = catalog.storeSource(R"(
        # a comment naming UnitBlueprint { that must not parse as a call
        PropBlueprint { BlueprintId = '/env/tree_prop.bp', Display = { } }
        UnitBlueprint { Economy = { BuildCostMass = 10 } }
        UnitBlueprint { Economy = { BuildCostMass = 'broken
    )", "/props/mixed.bp");
    // Two good calls stored; the unterminated third is pcall'd and skipped.
    CHECK(stored == 2);
    CHECK(catalog.size(rm::unit::BlueprintGroup::Prop) == 1);
    CHECK(catalog.size(rm::unit::BlueprintGroup::Unit) == 1);
    // No `_unit` tail on the file name, so SetShortId's gsub keeps the whole
    // lowered path — the same fallback retail produces.
    CHECK(catalog.find(rm::unit::BlueprintGroup::Unit, "/props/mixed.bp") != nullptr);
}

TEST_CASE("C-270: registration walks the fixed group order, alphabetical within",
          "[blueprint][merge]") {
    rm::unit::UnitCatalog catalog;
    // Inserted deliberately out of order — the store's order is the contract,
    // not the caller's.
    catalog.storeSource("UnitBlueprint { }", "/units/UEL0201/UEL0201_unit.bp");
    catalog.storeSource("PropBlueprint { }", "/env/b/b_prop.bp");
    catalog.storeSource("UnitBlueprint { }", "/units/UEL0105/UEL0105_unit.bp");
    catalog.storeSource("MeshBlueprint { }", "/meshes/m_mesh.bp");
    catalog.storeSource("PropBlueprint { }", "/env/a/a_prop.bp");

    const std::vector<rm::unit::UnitCatalog::Entry> order = catalog.registrationOrder();
    REQUIRE(order.size() == 5);
    // Mesh → Unit → Prop, alphabetical inside each group.
    CHECK(order[0].group == rm::unit::BlueprintGroup::Mesh);
    CHECK(order[1].group == rm::unit::BlueprintGroup::Unit);
    CHECK(order[1].id == "uel0105");
    CHECK(order[2].group == rm::unit::BlueprintGroup::Unit);
    CHECK(order[2].id == "uel0201");
    CHECK(order[3].group == rm::unit::BlueprintGroup::Prop);
    CHECK(order[3].id == "/env/a/a_prop.bp");
    CHECK(order[4].group == rm::unit::BlueprintGroup::Prop);
    CHECK(order[4].id == "/env/b/b_prop.bp");
}

TEST_CASE("C-314: the mod pass scans /mods/<name> after the fixed dirs",
          "[blueprint][merge]") {
    // `LoadBlueprints`'s order: fixed dirs first, then each active mod's `.bp`
    // files under its mount in `__active_mods` order — so a mod's `Merge=true`
    // overlay lands on the base blueprint the fixed scan already stored.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "rm_bp_mods_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto write = [&](const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out{path};
        out << text;
    };
    write(root / "stock/units/UEL0201/UEL0201_unit.bp",
          "UnitBlueprint { Economy = { BuildCostMass = 100, BuildTime = 50 } }");
    write(root / "mods/Balance/units/uel0201_balance.bp",
          "UnitBlueprint { BlueprintId = 'uel0201', Merge = true,\n"
          "  Economy = { BuildCostMass = 40 } }");
    write(root / "mods/Balance/units/uel9999/uel9999_unit.bp",
          "UnitBlueprint { Economy = { BuildCostMass = 5 } }");

    rm::vfs::Vfs vfs;
    vfs.mountDirectory(root / "stock");
    REQUIRE(vfs.mountMod(rm::vfs::ActiveMod{.uid = "uid-1", .name = "Balance",
                                          .location = (root / "mods/Balance").string()}));

    rm::unit::UnitCatalog catalog;
    const std::size_t stored = catalog.loadBlueprints(
        vfs, {rm::vfs::ActiveMod{.uid = "uid-1", .name = "Balance",
                                 .location = (root / "mods/Balance").string()}});
    CHECK(stored == 3);

    // The merge landed on the base unit: overlaid field wins, sibling survives.
    const rm::lua::Value* bp = catalog.find(rm::unit::BlueprintGroup::Unit, "uel0201");
    REQUIRE(bp != nullptr);
    CHECK(bp->path("Economy", "BuildCostMass")->number == 40.0);
    CHECK(bp->path("Economy", "BuildTime")->number == 50.0);

    // The mod's new unit registered too — under its own id.
    CHECK(catalog.find(rm::unit::BlueprintGroup::Unit, "uel9999") != nullptr);

    std::filesystem::remove_all(root, ec);
}
