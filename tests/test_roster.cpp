// The selection roster's arithmetic: grouping, layout, and the hit test.
//
// The grouping is the part worth testing hardest. A roster that miscounts is a roster that
// tells a player they have eleven tanks when they have nine, and a screenshot cannot catch it —
// the tile looks the same either way.
#include <catch2/catch_test_macros.hpp>

#include "core/ui/Roster.hpp"

#include <string>
#include <vector>

using rm::ui::groupSelection;
using rm::ui::insideRoster;
using rm::ui::RosterLayout;
using rm::ui::rosterLayout;
using rm::ui::RosterTile;
using rm::ui::rosterTileAt;
using rm::ui::rosterTileOrigin;

namespace {

[[nodiscard]] rm::ui::FrameLayout aFrame() {
    return rm::ui::frameLayout(rm::ui::UiViewport::full(1280.0f, 720.0f));
}

} // namespace

TEST_CASE("an empty selection has no roster", "[ui][roster]") {
    const auto layout = rosterLayout(aFrame(), 0);
    CHECK(layout.empty());
    CHECK_FALSE(rosterTileAt(layout, 700.0f, 850.0f).has_value());
    CHECK_FALSE(insideRoster(layout, 700.0f, 850.0f));
}

TEST_CASE("units of one type become one tile with a count", "[ui][roster]") {
    const std::vector<std::string> ids{"UEL0201", "UEL0201", "UEL0201"};
    const std::vector<float> health{300.0f, 150.0f, 75.0f};
    const std::vector<float> maxHealth{300.0f, 300.0f, 300.0f};

    const auto tiles = groupSelection(ids, health, maxHealth);
    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].id == "UEL0201");
    CHECK(tiles[0].count == 3);
    // SUMMED, not averaged: the bar answers "is this group worth committing", which is a
    // question about the group's total, not about its typical member.
    CHECK(tiles[0].health == 525.0f);
    CHECK(tiles[0].maxHealth == 900.0f);
}

TEST_CASE("types keep first-appearance order, not count order", "[ui][roster]") {
    // Sorting by count would reorder the row as units die, which destroys the by-position
    // reading a tiled roster exists for — the thing that was third is third next frame too.
    const std::vector<std::string> ids{"UEL0001", "UEL0201", "UEL0201", "UEL0105"};
    const std::vector<float> hp{12000.0f, 300.0f, 300.0f, 150.0f};

    const auto tiles = groupSelection(ids, hp, hp);
    REQUIRE(tiles.size() == 3);
    CHECK(tiles[0].id == "UEL0001");  // first seen, and the least numerous
    CHECK(tiles[1].id == "UEL0201");
    CHECK(tiles[2].id == "UEL0105");
    CHECK(tiles[1].count == 2);
}

TEST_CASE("a short health span is tolerated rather than read past its end", "[ui][roster]") {
    // The caller assembles three parallel lists and can get them out of step. Reading past the
    // end for a HUD is not worth a crash, and zero is the honest stand-in.
    const std::vector<std::string> ids{"A", "B"};
    const std::vector<float> health{100.0f};

    const auto tiles = groupSelection(ids, health, {});
    REQUIRE(tiles.size() == 2);
    CHECK(tiles[0].health == 100.0f);
    CHECK(tiles[1].health == 0.0f);
}

TEST_CASE("a group with no stated maximum reads as full", "[ui][roster]") {
    // An empty bar under a healthy selection is the more alarming of the two wrong answers, and
    // a type whose maximum this engine has not read is not a group in trouble.
    RosterTile tile;
    tile.health = 0.0f;
    tile.maxHealth = 0.0f;
    CHECK(tile.fill() == 1.0f);

    tile.maxHealth = 200.0f;
    tile.health = 50.0f;
    CHECK(tile.fill() == 0.25f);

    // Over-full clamps rather than overflowing its track.
    tile.health = 400.0f;
    CHECK(tile.fill() == 1.0f);
}

TEST_CASE("a tile's hover card names the type and shows the exact numbers", "[ui][roster]") {
    const rm::ui::RosterTile hurt{.id = "UEL0201",
                                  .name = "Medium Tank",
                                  .count = 3,
                                  .health = 150.0f,
                                  .maxHealth = 900.0f};
    const rm::ui::InfoCard card = rm::ui::rosterTileCard(hurt);
    CHECK(card.title == "Medium Tank");
    // No id beside the name — see `kShowBlueprintIds`. The nameless case below is the one
    // place an id still reaches the screen, because there is nothing else to print.
    CHECK(card.corner == (rm::ui::kShowBlueprintIds ? "UEL0201" : ""));
    REQUIRE(card.rows.size() == 2);
    CHECK(card.rows[0].value == "3");
    // A sixth of maximum is deep in the loss band; the card's colour must agree with the
    // underbar's, which turns red below thirty percent.
    CHECK(card.rows[1].tint == rm::ui::kLoss);

    // Nameless: the id leads and is not repeated; no stated maximum, no health row.
    const rm::ui::RosterTile bare{.id = "XXL0001", .count = 1};
    const rm::ui::InfoCard sparse = rm::ui::rosterTileCard(bare);
    CHECK(sparse.title == "XXL0001");
    CHECK(sparse.corner.empty());
    CHECK(sparse.rows.size() == 1);
}

TEST_CASE("grouping keeps the first unit's name for the type", "[ui][roster]") {
    const std::vector<std::string> ids{"UEL0201", "UEL0201"};
    const std::vector<float> hp{100.0f, 200.0f};
    const std::vector<float> max{300.0f, 300.0f};
    const std::vector<std::string> names{"Medium Tank", "Medium Tank"};
    const auto tiles = rm::ui::groupSelection(ids, hp, max, names);
    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].name == "Medium Tank");

    // Without names the tile is nameless rather than wrong.
    const auto plain = rm::ui::groupSelection(ids, hp, max);
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].name.empty());
}

TEST_CASE("the roster occupies the frame's selection bay", "[ui][roster]") {
    const rm::ui::FrameLayout frame = aFrame();
    const auto layout = rosterLayout(frame, 4);
    REQUIRE_FALSE(layout.empty());
    CHECK(layout.x == frame.selection.x);
    CHECK(layout.y == frame.selection.y);
    CHECK(layout.width == frame.selection.width);
    CHECK(layout.height == frame.selection.height);
}

TEST_CASE("every tile hit-tests back to its own index", "[ui][roster]") {
    constexpr std::size_t kCount = 5;
    const auto layout = rosterLayout(aFrame(), kCount);

    for (std::size_t index = 0; index < kCount; ++index) {
        const auto origin = rosterTileOrigin(layout, index);
        const auto hit = rosterTileAt(layout, origin[0] + layout.tileSize * 0.5f,
                                      origin[1] + layout.tileSize * 0.5f);
        REQUIRE(hit.has_value());
        CHECK(*hit == index);
    }
}

TEST_CASE("the gutter between tiles is a miss", "[ui][roster]") {
    const auto layout = rosterLayout(aFrame(), 4);
    const auto first = rosterTileOrigin(layout, 0);

    const float gutterX = first[0] + layout.tileSize + rm::ui::kRosterGap * 0.5f;
    const float y = first[1] + layout.tileSize * 0.5f;

    CHECK_FALSE(rosterTileAt(layout, gutterX, y).has_value());
    // ...and is still the PANEL, the same split the build tray makes: a click there is
    // interface, not world.
    CHECK(insideRoster(layout, gutterX, y));
}

TEST_CASE("a selection wider than the profile capacity reports what is not on the page",
          "[ui][roster]") {
    // A roster that silently omits types lies about the selection, and the lie is invisible —
    // the row looks complete. `hidden` is what the panel prints as `+N`.
    const auto layout = rosterLayout(aFrame(), 10);

    CHECK(layout.shown == 5);
    CHECK(layout.hidden == 5);
    CHECK(layout.pages == 2);

    // And nothing past the cap hit-tests, so a click cannot select a tile that is not drawn.
    const auto past = rosterTileOrigin(layout, 5);
    CHECK_FALSE(rosterTileAt(layout, past[0] + layout.tileSize * 0.5f,
                             past[1] + layout.tileSize * 0.5f)
                    .has_value());
}

TEST_CASE("a selection inside the cap hides nothing", "[ui][roster]") {
    const auto layout = rosterLayout(aFrame(), 3);
    CHECK(layout.shown == 3);
    CHECK(layout.hidden == 0);
}

TEST_CASE("a later roster page hit-tests to the global tile index", "[ui][roster]") {
    const auto layout = rosterLayout(aFrame(), 6, 1);
    REQUIRE(layout.shown == 1);
    CHECK(layout.first == 5);
    const auto origin = rosterTileOrigin(layout, 0);
    const auto hit = rosterTileAt(layout, origin[0] + layout.tileSize * 0.5f,
                                  origin[1] + layout.tileSize * 0.5f);
    REQUIRE(hit.has_value());
    CHECK(*hit == 5);
}
