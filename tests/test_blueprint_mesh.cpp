// The filename convention that finds a blueprint's geometry.
//
// Pure string work, so it is tested here rather than inferred from whether a
// tree appears on screen. The corpus checks that the convention RESOLVES —
// test_real_prop_meshes and test_real_unit_blueprints — which is a different
// question from whether the name is built correctly.
#include <catch2/catch_test_macros.hpp>

#include "core/blueprint/BlueprintMesh.hpp"

using rm::blueprint::kPropSuffix;
using rm::blueprint::kUnitSuffix;
using rm::blueprint::meshBeside;

TEST_CASE("a blueprint's mesh sits beside it, named for its level") {
    const auto mesh = meshBeside("/game/units/UEL0201/UEL0201_unit.bp", kUnitSuffix, 0);
    CHECK(mesh == "/game/units/UEL0201/UEL0201_lod0.scm");

    const auto coarse = meshBeside("/game/units/UEL0201/UEL0201_unit.bp", kUnitSuffix, 2);
    CHECK(coarse == "/game/units/UEL0201/UEL0201_lod2.scm");
}

TEST_CASE("props follow the same convention with their own suffix") {
    const auto mesh = meshBeside("/env/Evergreen/props/Tree01_prop.bp", kPropSuffix, 1);
    CHECK(mesh == "/env/Evergreen/props/Tree01_lod1.scm");
}

TEST_CASE("the suffix match ignores case, because the content does") {
    // Both spellings occur, and in the same directory: the maps say
    // `palm02_s4_prop.bp` in lower case where the archive holds `Palm02_s4_lod0.scm`.
    CHECK(meshBeside("/env/x/Palm02_PROP.bp", kPropSuffix, 0) == "/env/x/Palm02_lod0.scm");
    CHECK(meshBeside("/units/X/X_Unit.bp", kUnitSuffix, 0) == "/units/X/X_lod0.scm");
}

TEST_CASE("a stem that does not end in the expected suffix names no mesh") {
    // An empty path, not a guess. A prop suffix asked of a unit blueprint is a
    // caller bug, and a `.bp` that is neither is content this does not describe.
    CHECK(meshBeside("/units/UEL0201/UEL0201_unit.bp", kPropSuffix, 0).empty());
    CHECK(meshBeside("/env/x/Tree01_prop.bp", kUnitSuffix, 0).empty());
    CHECK(meshBeside("/env/x/something_else.bp", kPropSuffix, 0).empty());
}

TEST_CASE("a stem that is only the suffix names no mesh") {
    // `_unit.bp` would otherwise yield `_lod0.scm` — a plausible-looking path
    // with nothing in front of it, which is worse than an empty one.
    CHECK(meshBeside("/units/_unit.bp", kUnitSuffix, 0).empty());
    CHECK(meshBeside("/env/_prop.bp", kPropSuffix, 0).empty());
}
