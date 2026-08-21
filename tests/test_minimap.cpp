// The map, small, in the corner.
//
// §7 P7.4's stated test is that the world-to-minimap projection ROUND-TRIPS, and it is the right
// test because those two functions are the whole of a minimap's correctness: a pip drawn in the
// wrong place and a click that goes to the wrong place are the same bug, and it is invisible in
// a screenshot — everything looks plausible.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/Minimap.hpp"

#include <array>
#include <vector>

using Catch::Approx;
using rm::ui::MinimapLayout;
using rm::ui::MinimapPip;
using rm::ui::minimapToWorld;
using rm::ui::worldToMinimap;

namespace {

/// A usable font: one glyph and a solid block.
///
/// `Font::usable()` is `!glyphs.empty()`, and `appendPanel` refuses an unusable font — the HUD's
/// documented degradation, so that a missing system face costs the chrome rather than failing.
/// A geometry test needs to be on the other side of that guard, and one glyph is enough: the
/// minimap draws only rectangles, which use the solid texel rather than any glyph.
[[nodiscard]] rm::text::Font solidFont() {
    static const std::vector<rm::text::Glyph> one{rm::text::Glyph{}};
    return rm::text::Font{
        .glyphs = one, .lineHeight = 18.0f, .solidUv = {0.5f, 0.5f, 0.6f, 0.6f}};
}

/// A layout of round numbers, so a failure reads as an offset rather than as arithmetic.
[[nodiscard]] MinimapLayout square() {
    return MinimapLayout{.x = 20.0f, .y = 500.0f, .size = 200.0f, .inset = 10.0f};
}

} // namespace

TEST_CASE("world to minimap and back is the identity") {
    // §7 P7.4's stated test, over a square map and two rectangular ones — because the
    // letterboxing is where a projection stops being its own inverse.
    const MinimapLayout layout = square();

    struct Map {
        float width;
        float depth;
    };
    for (const Map map : {Map{4096.0f, 4096.0f}, Map{8192.0f, 4096.0f}, Map{2048.0f, 8192.0f}}) {
        for (const float fx : {0.0f, 0.13f, 0.5f, 0.87f, 1.0f}) {
            for (const float fz : {0.0f, 0.27f, 0.5f, 0.61f, 1.0f}) {
                const float worldX = map.width * fx;
                const float worldZ = map.depth * fz;

                const std::array<float, 2> point =
                    worldToMinimap(layout, map.width, map.depth, worldX, worldZ);
                const std::array<float, 2> back =
                    minimapToWorld(layout, map.width, map.depth, point[0], point[1]);

                // A tenth of an elmo on a map thousands of elmos across — the round trip goes
                // through a points-per-elmo scale of about 0.02, so a point of screen precision
                // is ~40 elmos and this is far inside it.
                CHECK(back[0] == Approx(worldX).margin(0.1));
                CHECK(back[1] == Approx(worldZ).margin(0.1));
            }
        }
    }
}

TEST_CASE("a non-square map is letterboxed, not stretched") {
    // ONE scale for both axes. Two would fill the square and stretch the map, which makes a
    // diagonal move look like it changes speed — plausible-looking and wrong.
    const MinimapLayout layout = square();
    const float width = 8192.0f;
    const float depth = 4096.0f;

    const std::array<float, 2> origin = worldToMinimap(layout, width, depth, 0.0f, 0.0f);
    const std::array<float, 2> farX = worldToMinimap(layout, width, depth, width, 0.0f);
    const std::array<float, 2> farZ = worldToMinimap(layout, width, depth, 0.0f, depth);

    const float acrossX = farX[0] - origin[0];
    const float acrossZ = farZ[1] - origin[1];

    // The wide axis fills the inner square; the short one is half of it.
    CHECK(acrossX == Approx(layout.size - 2.0f * layout.inset));
    CHECK(acrossZ == Approx(acrossX * 0.5f));

    // And the short axis is CENTRED, so the map sits in the middle of the panel rather than
    // against its top edge.
    const float innerTop = layout.y + layout.inset;
    const float slack = acrossX - acrossZ;
    CHECK(origin[1] == Approx(innerTop + slack * 0.5f));
}

TEST_CASE("the map's corners land on the projected area's corners") {
    const MinimapLayout layout = square();
    const std::array<float, 2> topLeft = worldToMinimap(layout, 4096.0f, 4096.0f, 0.0f, 0.0f);
    const std::array<float, 2> bottomRight =
        worldToMinimap(layout, 4096.0f, 4096.0f, 4096.0f, 4096.0f);

    CHECK(topLeft[0] == Approx(layout.x + layout.inset));
    CHECK(topLeft[1] == Approx(layout.y + layout.inset));
    CHECK(bottomRight[0] == Approx(layout.x + layout.size - layout.inset));
    CHECK(bottomRight[1] == Approx(layout.y + layout.size - layout.inset));
}

TEST_CASE("a click outside the map clamps to its edge") {
    // A click a pixel off the edge of a letterboxed map obviously means the edge. Unclamped it
    // would send the camera off the world, where the ground pick finds nothing and the view
    // appears to freeze.
    const MinimapLayout layout = square();
    const std::array<float, 2> before =
        minimapToWorld(layout, 4096.0f, 4096.0f, layout.x - 50.0f, layout.y - 50.0f);
    CHECK(before[0] == 0.0f);
    CHECK(before[1] == 0.0f);

    const std::array<float, 2> after = minimapToWorld(
        layout, 4096.0f, 4096.0f, layout.x + layout.size + 50.0f, layout.y + layout.size + 50.0f);
    CHECK(after[0] == 4096.0f);
    CHECK(after[1] == 4096.0f);
}

TEST_CASE("the panel knows what is on it") {
    const MinimapLayout layout = square();
    CHECK(rm::ui::insideMinimap(layout, layout.x + 1.0f, layout.y + 1.0f));
    CHECK(rm::ui::insideMinimap(layout, layout.x + layout.size, layout.y + layout.size));
    CHECK_FALSE(rm::ui::insideMinimap(layout, layout.x - 1.0f, layout.y + 1.0f));
    CHECK_FALSE(rm::ui::insideMinimap(layout, layout.x + 1.0f, layout.y + layout.size + 1.0f));
}

TEST_CASE("the default layout is bottom-left and does not grow with width") {
    // Sized off the SHORTER side, so an ultrawide monitor gets the same minimap as a square
    // one rather than a bigger one that is further from the units.
    const MinimapLayout wide = rm::ui::minimapLayout(3440.0f, 1440.0f);
    const MinimapLayout tall = rm::ui::minimapLayout(1440.0f, 1440.0f);
    CHECK(wide.size == tall.size);

    // Bottom-left: the resource panel is top-left and the clock top-right.
    CHECK(wide.x < 100.0f);
    CHECK(wide.y > 1440.0f * 0.5f);
    CHECK(wide.y + wide.size <= 1440.0f);

    // And capped, so a 5K display does not get a minimap the size of a playing card.
    CHECK(rm::ui::minimapLayout(5120.0f, 2880.0f).size <= 260.0f);
}

TEST_CASE("a degenerate layout draws nothing rather than dividing by zero") {
    const MinimapLayout none{};
    rm::ui::Geometry out;
    rm::ui::appendMinimap(out, solidFont(), rm::ui::neutralTheme(), none, 4096.0f, 4096.0f,
                          {}, {});
    CHECK(out.empty());

    // And a map of no size is answered rather than divided by.
    CHECK_NOTHROW((void)worldToMinimap(square(), 0.0f, 0.0f, 0.0f, 0.0f));
    const std::array<float, 2> world = minimapToWorld(square(), 0.0f, 0.0f, 100.0f, 600.0f);
    CHECK(world[0] == 0.0f);
    CHECK(world[1] == 0.0f);
}

TEST_CASE("pips and a view outline produce geometry") {
    // Not an image comparison — that is what the manual check is for. This asserts the shape of
    // the output: more pips means more vertices, and an outline is drawn only when there are
    // four corners to draw it between.
    const MinimapLayout layout = square();
    const rm::text::Font font = solidFont();
    const rm::ui::Theme theme = rm::ui::neutralTheme();

    const std::vector<MinimapPip> pips{
        MinimapPip{.worldX = 100.0f, .worldZ = 100.0f, .colour = {{1, 0, 0, 1}}, .size = 3.0f},
        MinimapPip{.worldX = 900.0f, .worldZ = 900.0f, .colour = {{0, 1, 0, 1}}, .size = 2.0f},
    };
    const std::vector<std::array<float, 2>> corners{
        {{200.0f, 200.0f}}, {{800.0f, 200.0f}}, {{800.0f, 800.0f}}, {{200.0f, 800.0f}}};

    rm::ui::Geometry bare;
    rm::ui::appendMinimap(bare, font, theme, layout, 4096.0f, 4096.0f, {}, {});
    const std::size_t panelOnly = bare.label.size();
    CHECK(panelOnly > 0);

    rm::ui::Geometry full;
    rm::ui::appendMinimap(full, font, theme, layout, 4096.0f, 4096.0f, pips, corners);
    CHECK(full.label.size() > panelOnly);

    // Three corners is not a quad, so no outline — a camera looking at the sky produces that.
    rm::ui::Geometry partial;
    rm::ui::appendMinimap(partial, font, theme, layout, 4096.0f, 4096.0f, pips,
                          std::span<const std::array<float, 2>>{corners.data(), 3});
    CHECK(partial.label.size() < full.label.size());
}
