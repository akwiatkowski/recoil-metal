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

namespace {

[[nodiscard]] rm::ui::FrameLayout aFrame() {
    return rm::ui::frameLayout(rm::ui::UiViewport::full(1280.0f, 720.0f));
}

} // namespace

TEST_CASE("no options means no panel", "[ui][build]") {
    const auto layout = buildPanelLayout(aFrame(), 0);

    // Absent rather than an empty frame: a builder that can build nothing and a non-builder look
    // the same to a player, and drawing a box around nothing invites the question "why is it
    // empty".
    CHECK(layout.empty());
    CHECK_FALSE(buildOptionAt(layout, 0, 20.0f, 500.0f).has_value());
}

TEST_CASE("a palette too narrow for one cell is absent", "[ui][build][viewport]") {
    rm::ui::FrameLayout frame = aFrame();
    frame.build.width = 20.0f;
    const auto layout = buildPanelLayout(frame, 6);

    CHECK(layout.empty());
    CHECK(layout.cellWidth >= 0.0f);
    CHECK_FALSE(rm::ui::insideBuildPanel(layout, frame.build.x, frame.build.y));
}

TEST_CASE("a constrained palette reduces columns before narrowing cells", "[ui][build]") {
    rm::ui::FrameLayout frame = aFrame();
    frame.build.width = 156.0f;
    const auto layout = buildPanelLayout(frame, 6);

    CHECK(layout.columns == 2);
    CHECK(layout.cellWidth >= rm::ui::kBuildCellHeight);
    CHECK(layout.shown == 4);
    CHECK(layout.pages == 2);
}

TEST_CASE("the grid is two rows of the frame's own column count", "[ui][build]") {
    // FROM THE FRAME, not from a number written here. This test used to name nine, and when the
    // columns came down to six — so that a cell could hold a NAME rather than an icon and a
    // number — it failed for a change that was entirely correct. What is worth asserting is the
    // relationship: two rows, the frame's columns, and a page that holds their product.
    const rm::ui::FrameLayout frame = aFrame();
    const std::size_t columns = frame.buildColumns;
    const std::size_t capacity = columns * static_cast<std::size_t>(rm::ui::kBuildRows);
    REQUIRE(columns > 1);

    const auto layout = buildPanelLayout(frame, capacity + 1);
    CHECK(layout.rows == rm::ui::kBuildRows);
    CHECK(static_cast<std::size_t>(layout.columns) == columns);
    CHECK(layout.shown == capacity);
    CHECK(layout.pages == 2);

    // A CELL WIDE ENOUGH TO NAME ITS UNIT, which is why the count came down. "Land Scout" in
    // the condensed label face wants about fifty points; below that a tray is six pictures and
    // no words, which is what it was.
    CHECK(layout.cellWidth >= 60.0f);

    const auto first = buildCellOrigin(layout, 0);
    const auto secondRow = buildCellOrigin(layout, columns);

    CHECK(first[0] == secondRow[0]);
    CHECK(secondRow[1] > first[1]);
}

TEST_CASE("the build palette occupies the frame's bounded rectangle", "[ui][build]") {
    const rm::ui::FrameLayout frame = aFrame();
    const auto layout = buildPanelLayout(frame, 6);
    CHECK(layout.x == frame.build.x);
    CHECK(layout.y == frame.build.y);
    CHECK(layout.width == frame.build.width);
    CHECK(layout.height == frame.build.height);
}

TEST_CASE("hundreds of options page without changing panel geometry",
          "[ui][build][stress]") {
    const rm::ui::FrameLayout frame = aFrame();
    const std::size_t capacity =
        frame.buildColumns * static_cast<std::size_t>(rm::ui::kBuildRows);
    const auto few = buildPanelLayout(frame, 3);
    constexpr std::size_t kOptions = 501;
    const auto many = buildPanelLayout(frame, kOptions);
    CHECK(many.width == few.width);
    CHECK(many.height == few.height);
    CHECK(many.shown == capacity);
    CHECK(many.pages == (kOptions + capacity - 1) / capacity);
}

TEST_CASE("a later build page maps its first cell to the global option", "[ui][build]") {
    const rm::ui::FrameLayout frame = aFrame();
    const std::size_t capacity =
        frame.buildColumns * static_cast<std::size_t>(rm::ui::kBuildRows);
    const std::size_t count = capacity + 1;
    const auto layout = buildPanelLayout(frame, count, 1);
    REQUIRE(layout.shown == 1);
    CHECK(layout.first == capacity);
    const auto origin = buildCellOrigin(layout, 0);
    const auto hit = buildOptionAt(layout, count, origin[0] + layout.cellWidth * 0.5f,
                                   origin[1] + layout.cellHeight * 0.5f);
    REQUIRE(hit.has_value());
    CHECK(*hit == capacity);
    CHECK(rm::ui::buildPageStepAt(layout, layout.x + layout.width - 10.0f,
                                  layout.y + 10.0f)
              == 1);
}

TEST_CASE("every cell hit-tests back to its own index", "[ui][build]") {
    constexpr std::size_t kCount = 8;
    const auto layout = buildPanelLayout(aFrame(), kCount);

    for (std::size_t index = 0; index < kCount; ++index) {
        const auto origin = buildCellOrigin(layout, index);
        // The centre of the cell, which is where a click actually lands.
        const float x = origin[0] + layout.cellWidth * 0.5f;
        const float y = origin[1] + layout.cellHeight * 0.5f;

        const auto hit = buildOptionAt(layout, kCount, x, y);
        REQUIRE(hit.has_value());
        CHECK(*hit == index);
    }
}

TEST_CASE("a logical click round-trips through a scaled HUD cell", "[ui][viewport][build]") {
    constexpr std::size_t kCount = 8;
    const rm::ui::UiViewport viewport =
        rm::ui::UiViewport::full(2560.0f, 1440.0f, 2.0f);
    const auto layout = buildPanelLayout(rm::ui::frameLayout(viewport), kCount);
    const auto origin = buildCellOrigin(layout, 3);
    const std::array<float, 2> logical = viewport.toLogical(
        {origin[0] + layout.cellWidth * 0.5f, origin[1] + layout.cellHeight * 0.5f});
    const std::array<float, 2> hud = viewport.toHud(logical);

    CHECK(buildOptionAt(layout, kCount, hud[0], hud[1]) == std::optional<std::size_t>{3});
}

TEST_CASE("the gutter between cells is a miss, not the nearest neighbour", "[ui][build]") {
    const auto layout = buildPanelLayout(aFrame(), 6);
    const auto first = buildCellOrigin(layout, 0);

    // Dead centre of the gap between column one and column two. Snapping this to whichever cell
    // is closer is how a player queues something they did not choose.
    const float x = first[0] + layout.cellWidth + rm::ui::kBuildGap * 0.5f;
    const float y = first[1] + layout.cellHeight * 0.5f;

    CHECK_FALSE(buildOptionAt(layout, 6, x, y).has_value());
}

TEST_CASE("an empty trailing cell in a short row is over nothing", "[ui][build]") {
    constexpr std::size_t kCount = 10;
    const auto layout = buildPanelLayout(aFrame(), kCount);
    const auto empty = buildCellOrigin(layout, 11);

    const auto hit = buildOptionAt(layout, kCount, empty[0] + layout.cellWidth * 0.5f,
                                    empty[1] + layout.cellHeight * 0.5f);
    CHECK_FALSE(hit.has_value());
}

TEST_CASE("points outside the grid miss", "[ui][build]") {
    const auto layout = buildPanelLayout(aFrame(), 6);

    CHECK_FALSE(buildOptionAt(layout, 6, layout.x - 10.0f, layout.gridY).has_value());
    CHECK_FALSE(buildOptionAt(layout, 6, layout.gridX, layout.y - 10.0f).has_value());
    CHECK_FALSE(
        buildOptionAt(layout, 6, layout.x + layout.width + 10.0f, layout.gridY).has_value());
}

TEST_CASE("the vertical gutter between rows is a miss too", "[ui][build]") {
    const auto layout = buildPanelLayout(aFrame(), 12);
    const auto first = buildCellOrigin(layout, 0);

    const float x = first[0] + layout.cellWidth * 0.5f;
    const float y = first[1] + layout.cellHeight + rm::ui::kBuildGap * 0.5f;
    CHECK_FALSE(buildOptionAt(layout, 12, x, y).has_value());

    // Just inside the bottom of the first cell is still the first cell.
    const auto hit = buildOptionAt(layout, 12, x, first[1] + layout.cellHeight - 1.0f);
    REQUIRE(hit.has_value());
    CHECK(*hit == 0);
}

TEST_CASE("the hover card states the facts and omits what the content did not say",
          "[ui][build]") {
    const BuildOption full{.id = "UEB1103",
                           .name = "Mass Extractor",
                           .massCost = 36.0f,
                           .energyCost = 360.0f,
                           .buildSeconds = 10.0f,
                           .health = 600.0f,
                           .affordable = true};
    const rm::ui::InfoCard card =
        rm::ui::buildOptionCard(full, rm::ui::GameProfile::Fa);
    CHECK(card.title == "Mass Extractor");
    // The corner carried the blueprint id beside the name it stands for. `kShowBlueprintIds`
    // turned that off everywhere a human reads: an id is a filename, and the card is the one
    // place in the interface whose entire job is to say what the thing IS.
    CHECK(card.corner == (rm::ui::kShowBlueprintIds ? "UEB1103" : ""));
    REQUIRE(card.rows.size() == 4);
    CHECK(card.rows[0].label == "MASS");
    CHECK(card.rows[0].tint == rm::ui::kMass);
    CHECK(rm::ui::buildOptionCard(full, rm::ui::GameProfile::Bar).rows[0].label == "METAL");
    CHECK(rm::ui::buildOptionCard(full, rm::ui::GameProfile::Neutral).rows[0].label
          == "MATERIAL");

    // Unaffordable mass turns the row to the loss colour — the card agrees with the cell.
    BuildOption poor = full;
    poor.affordable = false;
    CHECK(rm::ui::buildOptionCard(poor, rm::ui::GameProfile::Fa).rows[0].tint
          == rm::ui::kLoss);

    // A nameless option leads with its id and repeats nothing in the corner; its zero rows
    // are absent rather than printed as measurements.
    const BuildOption bare{.id = "XXB0001", .massCost = 5.0f};
    const rm::ui::InfoCard sparse =
        rm::ui::buildOptionCard(bare, rm::ui::GameProfile::Fa);
    CHECK(sparse.title == "XXB0001");
    CHECK(sparse.corner.empty());
    CHECK(sparse.rows.size() == 1);
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

TEST_CASE("the whole panel swallows a click, gutters included", "[ui][build]") {
    // WHY THE GUTTERS COUNT. The panel exists only while a builder is selected, so a click
    // falling through it reaches the ground, clears the selection, and takes the panel away —
    // the button disappears under the cursor that pressed it. `buildOptionAt` deliberately
    // treats a gutter as a miss so nobody queues a thing they did not choose; that is the right
    // answer to "which cell" and the wrong one to "should the world hear about this".
    const auto layout = buildPanelLayout(aFrame(), 6);
    const auto first = buildCellOrigin(layout, 0);

    const float gutterX = first[0] + layout.cellWidth + rm::ui::kBuildGap * 0.5f;
    const float gutterY = first[1] + layout.cellHeight * 0.5f;

    // The two answers disagree about this point, and both are correct.
    CHECK_FALSE(buildOptionAt(layout, 6, gutterX, gutterY).has_value());
    CHECK(rm::ui::insideBuildPanel(layout, gutterX, gutterY));

    // The header strip carries no cells at all and is still the panel.
    CHECK(rm::ui::insideBuildPanel(layout, layout.x + 2.0f, layout.y + 2.0f));
}

TEST_CASE("a point off the panel is not swallowed", "[ui][build]") {
    const auto layout = buildPanelLayout(aFrame(), 6);

    CHECK_FALSE(rm::ui::insideBuildPanel(layout, layout.x - 1.0f, layout.y + 10.0f));
    CHECK_FALSE(rm::ui::insideBuildPanel(layout, layout.x + 10.0f, layout.y - 1.0f));
    // The far edges are exclusive, so the panel and whatever abuts it cannot both claim a point.
    CHECK_FALSE(rm::ui::insideBuildPanel(layout, layout.x + layout.width, layout.y + 10.0f));
    CHECK_FALSE(rm::ui::insideBuildPanel(layout, layout.x + 10.0f, layout.y + layout.height));
}

TEST_CASE("an absent panel swallows nothing", "[ui][build]") {
    // A builder that can build nothing draws no frame, so there is no rectangle to be inside —
    // and a click there is an ordinary click on the world, not a swallowed one.
    const auto layout = buildPanelLayout(aFrame(), 0);
    REQUIRE(layout.empty());
    CHECK_FALSE(rm::ui::insideBuildPanel(layout, 0.0f, 0.0f));
}
