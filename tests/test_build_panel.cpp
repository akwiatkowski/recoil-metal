// The build panel's arithmetic: where the cells are, and which one a click is over.
//
// These two are the whole of this panel's correctness, for the same reason the minimap's
// round-trip is the whole of its own: a cell drawn in one place and clicked in another looks
// perfectly fine in a screenshot and is wrong every time a player uses it.
#include <catch2/catch_test_macros.hpp>

#include "core/ui/BuildPanel.hpp"

#include <vector>

using rm::ui::BuildOption;
using rm::ui::buildCellOrigin;
using rm::ui::buildOptionAt;
using rm::ui::buildPanelLayout;
using rm::ui::MinimapLayout;

namespace {

[[nodiscard]] MinimapLayout aMinimap() {
    MinimapLayout minimap;
    minimap.x = 12.0f;
    minimap.y = 600.0f;
    minimap.size = 220.0f;
    minimap.inset = 4.0f;
    return minimap;
}

} // namespace

TEST_CASE("no options means no panel", "[ui][build]") {
    const auto layout = buildPanelLayout(aMinimap(), 0);

    // Absent rather than an empty frame: a builder that can build nothing and a non-builder look
    // the same to a player, and drawing a box around nothing invites the question "why is it
    // empty".
    CHECK(layout.empty());
    CHECK_FALSE(buildOptionAt(layout, 0, 20.0f, 500.0f).has_value());
}

TEST_CASE("the grid wraps at three columns and keeps the last short row", "[ui][build]") {
    // Seven options is two full rows and a row of one — the case that gets lost when rows are
    // computed by division rather than by rounding up.
    const auto layout = buildPanelLayout(aMinimap(), 7);
    CHECK(layout.rows == 3);
    CHECK(layout.columns == 3);

    const auto first = buildCellOrigin(layout, 0);
    const auto fourth = buildCellOrigin(layout, 3);

    // The fourth option starts a new row: same column, one row down.
    CHECK(first[0] == fourth[0]);
    CHECK(fourth[1] > first[1]);
}

TEST_CASE("the panel sits above the minimap and shares its left edge", "[ui][build]") {
    const MinimapLayout minimap = aMinimap();
    const auto layout = buildPanelLayout(minimap, 6);

    // One bottom-left block, which is the arrangement being copied from Beyond All Reason.
    CHECK(layout.x == minimap.x);
    CHECK(layout.y + layout.height < minimap.y);
}

TEST_CASE("a longer list grows upward rather than off the bottom", "[ui][build]") {
    const MinimapLayout minimap = aMinimap();
    const auto few = buildPanelLayout(minimap, 3);
    const auto many = buildPanelLayout(minimap, 12);

    CHECK(many.height > few.height);
    // Both keep their bottom against the minimap; only the top moves.
    CHECK(many.y < few.y);
    CHECK(few.y + few.height == many.y + many.height);
}

TEST_CASE("every cell hit-tests back to its own index", "[ui][build]") {
    constexpr std::size_t kCount = 8;
    const auto layout = buildPanelLayout(aMinimap(), kCount);

    for (std::size_t index = 0; index < kCount; ++index) {
        const auto origin = buildCellOrigin(layout, index);
        // The centre of the cell, which is where a click actually lands.
        const float x = origin[0] + rm::ui::kBuildCell * 0.5f;
        const float y = origin[1] + rm::ui::kBuildCell * 0.5f;

        const auto hit = buildOptionAt(layout, kCount, x, y);
        REQUIRE(hit.has_value());
        CHECK(*hit == index);
    }
}

TEST_CASE("the gutter between cells is a miss, not the nearest neighbour", "[ui][build]") {
    const auto layout = buildPanelLayout(aMinimap(), 6);
    const auto first = buildCellOrigin(layout, 0);

    // Dead centre of the gap between column one and column two. Snapping this to whichever cell
    // is closer is how a player queues something they did not choose.
    const float x = first[0] + rm::ui::kBuildCell + rm::ui::kBuildGap * 0.5f;
    const float y = first[1] + rm::ui::kBuildCell * 0.5f;

    CHECK_FALSE(buildOptionAt(layout, 6, x, y).has_value());
}

TEST_CASE("an empty trailing cell in a short row is over nothing", "[ui][build]") {
    // Four options: a full row, then one. Cells five and six exist as geometry and hold nothing.
    constexpr std::size_t kCount = 4;
    const auto layout = buildPanelLayout(aMinimap(), kCount);
    const auto fifth = buildCellOrigin(layout, 4);

    const auto hit = buildOptionAt(layout, kCount, fifth[0] + rm::ui::kBuildCell * 0.5f,
                                   fifth[1] + rm::ui::kBuildCell * 0.5f);
    CHECK_FALSE(hit.has_value());
}

TEST_CASE("points outside the grid miss", "[ui][build]") {
    const auto layout = buildPanelLayout(aMinimap(), 6);

    CHECK_FALSE(buildOptionAt(layout, 6, layout.x - 10.0f, layout.gridY).has_value());
    CHECK_FALSE(buildOptionAt(layout, 6, layout.gridX, layout.y - 10.0f).has_value());
    CHECK_FALSE(
        buildOptionAt(layout, 6, layout.x + layout.width + 10.0f, layout.gridY).has_value());
}

TEST_CASE("the grid moves with the drawable, so a hit test must share its space", "[ui][build]") {
    // THIS IS THE TRAP THAT COST A BUG, written as an assertion because the bug itself is not
    // reachable from here. `MouseModifiers::pointX` arrived in POINTS while the layout is built
    // from the DRAWABLE's size, which is 2x the points on a Retina display — so a click had to
    // land in the lower-left quarter of a cell's own rectangle to register. The conversion that
    // was wrong lives in `Window.mm` behind AppKit and this target does not link it.
    //
    // What IS assertable is the relationship that makes the mismatch fatal rather than merely
    // untidy: the panel is positioned entirely by the minimap it hangs off, so at twice the
    // drawable size every cell is somewhere else. A point good in one space is not merely
    // imprecise in the other, it is a miss — which is what makes "which space is this in"
    // a question with a wrong answer rather than a rounding concern.
    MinimapLayout onex = aMinimap();
    MinimapLayout twox = aMinimap();
    twox.x *= 2.0f;
    twox.y *= 2.0f;
    twox.size *= 2.0f;

    const auto small = buildPanelLayout(onex, 6);
    const auto large = buildPanelLayout(twox, 6);

    const auto cell = buildCellOrigin(small, 0);
    const float x = cell[0] + rm::ui::kBuildCell * 0.5f;
    const float y = cell[1] + rm::ui::kBuildCell * 0.5f;

    REQUIRE(buildOptionAt(small, 6, x, y).has_value());
    CHECK_FALSE(buildOptionAt(large, 6, x, y).has_value());
}

TEST_CASE("tier tints brighten with tier and stay short of white", "[ui][build]") {
    const rm::ui::Theme theme = rm::ui::neutralTheme();

    const auto tier1 = rm::ui::tierTint(theme, 1);
    const auto tier2 = rm::ui::tierTint(theme, 2);
    const auto tier3 = rm::ui::tierTint(theme, 3);

    CHECK(tier2[0] >= tier1[0]);
    CHECK(tier3[0] >= tier2[0]);

    // Never fully white: a band that outshines the lit edge pulls the eye to the tier rather
    // than to what is selected.
    CHECK(tier3[0] < 1.0f);

    // An out-of-range tier CLAMPS rather than wrapping round to tier one. Tier 4 is what
    // `techOf` returns for an EXPERIMENTAL, and painting the biggest thing in the game the same
    // as a power generator is the one way this could be wrong and still look deliberate.
    CHECK(rm::ui::tierTint(theme, 9)[0] == tier3[0]);
    CHECK(rm::ui::tierTint(theme, 4)[0] == tier3[0]);
    CHECK(rm::ui::tierTint(theme, 0)[0] == tier1[0]);
    CHECK(rm::ui::tierTint(theme, -1)[0] == tier1[0]);
}
