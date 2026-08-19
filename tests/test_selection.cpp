// Selection rules. These decide what a click does, and until now they lived
// inside an AppKit callback where nothing could reach them — so the only way to
// check "does shift-click add" was to click.
#include <catch2/catch_test_macros.hpp>

#include "core/scene/Selection.hpp"

#include <optional>
#include <vector>

using rm::SelectionEntry;
using rm::applyClick;

namespace {

constexpr SelectionEntry kA{0, 1};
constexpr SelectionEntry kB{0, 2};
constexpr SelectionEntry kC{1, 0};

} // namespace

TEST_CASE("a plain click on a unit selects just it") {
    const std::vector<SelectionEntry> after = applyClick({}, kA, /*addToSet=*/false);
    REQUIRE(after.size() == 1);
    CHECK(after[0] == kA);
}

TEST_CASE("a plain click on another unit replaces the selection") {
    const std::vector<SelectionEntry> current{kA, kB};
    const std::vector<SelectionEntry> after = applyClick(current, kC, false);

    REQUIRE(after.size() == 1);
    CHECK(after[0] == kC);
}

TEST_CASE("a modified click adds to the selection") {
    const std::vector<SelectionEntry> current{kA};
    const std::vector<SelectionEntry> after = applyClick(current, kB, /*addToSet=*/true);

    REQUIRE(after.size() == 2);
    CHECK(after[0] == kA);
    CHECK(after[1] == kB);
}

TEST_CASE("clicking a selected unit removes it, and leaves the rest") {
    const std::vector<SelectionEntry> current{kA, kB, kC};

    const std::vector<SelectionEntry> after = applyClick(current, kB, true);
    REQUIRE(after.size() == 2);
    CHECK(after[0] == kA);
    CHECK(after[1] == kC);

    // And unmodified too — the rule is about the unit already being in the set,
    // not about the modifier.
    const std::vector<SelectionEntry> plain = applyClick(current, kB, false);
    REQUIRE(plain.size() == 2);
    CHECK(plain[0] == kA);
}

TEST_CASE("a plain click on empty ground clears the selection") {
    const std::vector<SelectionEntry> current{kA, kB};
    CHECK(applyClick(current, std::nullopt, false).empty());
}

TEST_CASE("a modified click on empty ground keeps the selection") {
    // Shift-clicking past the edge of a unit is a miss, not an instruction to
    // throw away what is selected — which is what makes box-select-by-shift
    // survivable when the user's aim is off.
    const std::vector<SelectionEntry> current{kA, kB};
    const std::vector<SelectionEntry> after = applyClick(current, std::nullopt, true);

    REQUIRE(after.size() == 2);
    CHECK(after[0] == kA);
    CHECK(after[1] == kB);
}

TEST_CASE("selection order is stable") {
    // Orders are issued by walking the selection, so a set that reshuffled
    // itself would make two identical clicks give different results.
    std::vector<SelectionEntry> current;
    current = applyClick(current, kC, true);
    current = applyClick(current, kA, true);
    current = applyClick(current, kB, true);

    REQUIRE(current.size() == 3);
    CHECK(current[0] == kC);
    CHECK(current[1] == kA);
    CHECK(current[2] == kB);
}

TEST_CASE("entries compare on identity alone") {
    CHECK(kA == SelectionEntry{0, 1});
    CHECK_FALSE(kA == kB);
    CHECK_FALSE(SelectionEntry{0, 1} == SelectionEntry{1, 1});
}
