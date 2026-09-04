// The factory production panel: what the selected factory will build, in order, with the
// progress of the current build. Layout is checked against the row budget the rectangle
// affords, and the overflow line is checked to be a statement rather than a silent drop.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/ProductionPanel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

/// A font table good enough to lay out against: every glyph a 10-wide box.
[[nodiscard]] std::vector<rm::text::Glyph> boxGlyphs() {
    std::vector<rm::text::Glyph> glyphs(rm::text::kGlyphCount);
    for (rm::text::Glyph& glyph : glyphs) {
        glyph = rm::text::Glyph{.uv = {0.0f, 0.0f, 0.1f, 0.1f},
                                .width = 8.0f,
                                .height = 12.0f,
                                .bearingX = 1.0f,
                                .bearingY = -12.0f,
                                .advance = 10.0f};
    }
    return glyphs;
}

[[nodiscard]] rm::text::Font fontOver(const std::vector<rm::text::Glyph>& glyphs) {
    return rm::text::Font{
        .glyphs = glyphs, .lineHeight = 18.0f, .solidUv = {0.5f, 0.5f, 0.6f, 0.6f}};
}

/// Six quads of six vertices per glyph, so the number of glyphs a layer drew.
[[nodiscard]] std::size_t glyphsIn(const std::vector<rm::text::TextVertex>& layer) {
    return layer.size() / 6;
}

[[nodiscard]] float rightmost(const std::vector<rm::text::TextVertex>& layer) {
    float right = 0.0f;
    for (const rm::text::TextVertex& vertex : layer) {
        right = std::max(right, vertex.position[0]);
    }
    return right;
}

[[nodiscard]] rm::ui::ProductionView factoryWith(std::size_t orders) {
    rm::ui::ProductionView view;
    view.factoryId = "URB0101";
    view.factoryName = "Land Factory";
    for (std::size_t i = 0; i < orders; ++i) {
        view.queue.push_back({.id = "URL0107", .name = "Mantis", .count = 1});
    }
    return view;
}

} // namespace

TEST_CASE("the row budget follows the rectangle's height", "[ui][production]") {
    // Header and bar take the top 56 points; then one row per 16, the last needing 6 of room.
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 40}) == 0);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 62}) == 1);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 78}) == 2);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 126}) == 5);
}

TEST_CASE("every order gets a row when they fit, and a count on the right", "[ui][production]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::Geometry out;
    rm::ui::ProductionView view = factoryWith(3);
    view.queue[1].count = 5;
    view.building = true;
    view.progress = 0.5f;
    rm::ui::appendProductionPanel(out, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 200, 126}, view);

    // Title (12) plus three "Mantis" rows (6 each): 30 label glyphs, no overflow line.
    CHECK(glyphsIn(out.label) == 12 + 3 * 6);
    // "ONCE" (4) plus "x1", "x5", "x1" (2 each) on the readout layer.
    CHECK(glyphsIn(out.foregroundReadout) == 4 + 3 * 2);
    // The well and its half fill, on top of the panel's own chrome.
    CHECK_FALSE(out.chrome.empty());
}

TEST_CASE("orders past the room are summarised, never dropped silently", "[ui][production]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::Geometry out;
    // Two rows of room, seven orders: one row shown and "+6 MORE" in the second.
    rm::ui::appendProductionPanel(out, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 200, 78}, factoryWith(7));
    CHECK(glyphsIn(out.label) == 12 + 6 + 7);  // title, "Mantis", "+6 MORE"
    CHECK(glyphsIn(out.foregroundReadout) == 4 + 2);  // "ONCE", one "x1"
}

TEST_CASE("long factory and order names stay inside the production panel",
          "[ui][production][stress]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::ProductionView view = factoryWith(1);
    view.factoryName = "Experimental Mobile Rapid-Fire Artillery Installation";
    view.queue[0].name = "Experimental Strategic Missile Defence Construction Vehicle";

    constexpr rm::ui::Rect kPanel{20.0f, 10.0f, 200.0f, 126.0f};
    rm::ui::Geometry out;
    rm::ui::appendProductionPanel(out, font, font, rm::ui::neutralTheme(), kPanel, view);

    CHECK(rightmost(out.label) <= kPanel.right());
    CHECK(rightmost(out.foregroundReadout) <= kPanel.right());
}

TEST_CASE("an idle factory says so and a non-factory draws nothing", "[ui][production]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::Geometry out;
    rm::ui::appendProductionPanel(out, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 200, 126}, factoryWith(0));
    CHECK(glyphsIn(out.label) == 12 + 4);  // title and "IDLE"

    rm::ui::Geometry none;
    rm::ui::appendProductionPanel(none, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 200, 126}, rm::ui::ProductionView{});
    CHECK(none.empty());
}
