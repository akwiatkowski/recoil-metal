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

constexpr float kWide = 1400.0f;
constexpr float kTall = 900.0f;

} // namespace

TEST_CASE("an empty selection has no roster", "[ui][roster]") {
    const auto layout = rosterLayout(kWide, kTall, 0);
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

TEST_CASE("the roster is centred along the bottom", "[ui][roster]") {
    const auto layout = rosterLayout(kWide, kTall, 4);
    REQUIRE_FALSE(layout.empty());

    // Equal margins left and right, within a rounding step.
    const float left = layout.x;
    const float right = kWide - (layout.x + layout.width);
    CHECK(std::abs(left - right) < 1.0f);
    CHECK(layout.y + layout.height < kTall);
}

TEST_CASE("every tile hit-tests back to its own index", "[ui][roster]") {
    constexpr std::size_t kCount = 5;
    const auto layout = rosterLayout(kWide, kTall, kCount);

    for (std::size_t index = 0; index < kCount; ++index) {
        const auto origin = rosterTileOrigin(layout, index);
        const auto hit = rosterTileAt(layout, origin[0] + rm::ui::kRosterTile * 0.5f,
                                      origin[1] + rm::ui::kRosterTile * 0.5f);
        REQUIRE(hit.has_value());
        CHECK(*hit == index);
    }
}

TEST_CASE("the gutter between tiles is a miss", "[ui][roster]") {
    const auto layout = rosterLayout(kWide, kTall, 4);
    const auto first = rosterTileOrigin(layout, 0);

    const float gutterX = first[0] + rm::ui::kRosterTile + rm::ui::kRosterGap * 0.5f;
    const float y = first[1] + rm::ui::kRosterTile * 0.5f;

    CHECK_FALSE(rosterTileAt(layout, gutterX, y).has_value());
    // ...and is still the PANEL, the same split the build tray makes: a click there is
    // interface, not world.
    CHECK(insideRoster(layout, gutterX, y));
}

TEST_CASE("a selection wider than the cap reports what it dropped", "[ui][roster]") {
    // A roster that silently omits types lies about the selection, and the lie is invisible —
    // the row looks complete. `hidden` is what the panel prints as `+N`.
    const auto layout = rosterLayout(kWide, kTall, rm::ui::kRosterMaxTiles + 5);

    CHECK(layout.shown == rm::ui::kRosterMaxTiles);
    CHECK(layout.hidden == 5);

    // And nothing past the cap hit-tests, so a click cannot select a tile that is not drawn.
    const auto past = rosterTileOrigin(layout, rm::ui::kRosterMaxTiles);
    CHECK_FALSE(rosterTileAt(layout, past[0] + rm::ui::kRosterTile * 0.5f,
                             past[1] + rm::ui::kRosterTile * 0.5f)
                    .has_value());
}

TEST_CASE("a selection inside the cap hides nothing", "[ui][roster]") {
    const auto layout = rosterLayout(kWide, kTall, 3);
    CHECK(layout.shown == 3);
    CHECK(layout.hidden == 0);
}
