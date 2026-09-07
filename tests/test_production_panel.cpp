// The factory production panel: what the selected factory will build, in order, with the
// progress of the current build. Layout is checked against the row budget the rectangle
// affords, and the overflow line is checked to be a statement rather than a silent drop.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/ProductionPanel.hpp"

#include <algorithm>
#include <limits>
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
        view.queue.push_back({.id = "URL0107", .name = "Mantis", .count = 1,
            .commandId = rm::commandId(static_cast<rm::CommandSource>(1),
                                      static_cast<std::uint32_t>(i + 10))});
    }
    return view;
}

} // namespace

TEST_CASE("every production entry has a reachable cancellation target across pages",
          "[ui][production][production-cancel]") {
    constexpr rm::ui::Rect rect{100, 50, 320, 154};
    auto view = factoryWith(12);
    for (std::size_t page = 0; page < 3; ++page) {
        const auto layout = rm::ui::productionPage(rect, view.queue.size(), page);
        REQUIRE(layout.pages == 3);
        CHECK(layout.first == page * 5);
        for (std::size_t row = 0; row < layout.shown; ++row) {
            const auto cancel = rm::ui::productionCancelRect(rect, row);
            CHECK(rect.contains(cancel.x, cancel.y));
            CHECK(rect.contains(cancel.right() - 1, cancel.bottom() - 1));
            CHECK(rm::ui::productionCancelAt(rect, view, cancel.x + 1, cancel.y + 1, page)
                == view.queue[layout.first + row].commandId);
            CHECK_FALSE(rm::ui::productionCancelAt(rect, view, cancel.x - 1, cancel.y + 1, page));
        }
    }
    const auto lastRow = rm::ui::productionCancelRect(rect, 2);
    CHECK_FALSE(rm::ui::productionCancelAt(rect, view, lastRow.x + 1, lastRow.y + 1, 2));
    const auto first = rm::ui::productionCancelRect(rect, 0);
    const auto id = view.queue[10].commandId;
    view.queue.erase(view.queue.begin());
    CHECK(view.queue[9].commandId == id);  // identity survives queue index changes
    CHECK(rm::ui::productionPage(rect, 1, 2).page == 0);
    view.queue[0].commandId = rm::kInvalidCommandId;
    CHECK_FALSE(rm::ui::productionCancelAt(rect, view, first.x + 1, first.y + 1));
}

TEST_CASE("production paging shares bounds and stops at each end", "[ui][production]") {
    constexpr rm::ui::Rect rect{100, 50, 320, 154};
    const auto view = factoryWith(12);
    const auto previous = rm::ui::productionPageButtonRect(rect, false);
    const auto next = rm::ui::productionPageButtonRect(rect, true);
    CHECK_FALSE(rm::ui::productionPageStepAt(rect, view, previous.x + 1, previous.y + 1, 0));
    CHECK(rm::ui::productionPageStepAt(rect, view, next.x + 1, next.y + 1, 0) == 1);
    CHECK(rm::ui::productionPageStepAt(rect, view, previous.x + 1, previous.y + 1, 2) == -1);
    CHECK_FALSE(rm::ui::productionPageStepAt(rect, view, next.x + 1, next.y + 1, 2));
    CHECK_FALSE(rm::ui::productionCommandAt(rect, view, next.x + 1, next.y + 1));
    CHECK_FALSE(rm::ui::productionCancelAt(rect, view, next.x + 1, next.y + 1));
}

TEST_CASE("factory queue controls share the rendered panel bounds", "[ui][production]") {
    const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::authored(1600, 900));
    const auto rect = rm::ui::productionPanelRect(frame);
    CHECK(rect.x == frame.commands.x);
    CHECK(rect.bottom() < frame.commands.y);
    CHECK(rect.y >= frame.battlefield.y);
    const auto repeat = rm::ui::productionRepeatRect(rect);
    const auto clear = rm::ui::productionClearRect(rect);
    auto view = factoryWith(1);
    CHECK(rm::ui::productionCommandAt(rect, view, repeat.x + 1, repeat.y + 1)
          == rm::sim::CommandKind::ToggleFactoryRepeat);
    CHECK(rm::ui::productionCommandAt(rect, view, clear.x + 1, clear.y + 1)
          == rm::sim::CommandKind::Stop);
    CHECK_FALSE(rm::ui::productionCommandAt(rect, view, rect.x - 1, rect.y));
    CHECK_FALSE(rm::ui::productionCommandAt(rect, view, rect.x + 10, rect.y + 50));
    view.queue.clear();
    CHECK_FALSE(rm::ui::productionCommandAt(rect, view, clear.x + 1, clear.y + 1));
    CHECK_FALSE(rm::ui::productionCommandAt({}, view, 0, 0));
}

TEST_CASE("the row budget follows the rectangle's height", "[ui][production]") {
    // Header and bar take 56 points; rows need the 34-point footer for Clear Queue.
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 40}) == 0);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 90}) == 1);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 106}) == 2);
    CHECK(rm::ui::productionRowsFor(rm::ui::Rect{0, 0, 200, 154}) == 5);
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
                                  rm::ui::Rect{0, 0, 320, 154}, view);

    // Title, three product names, Clear Queue, and each row's CANCEL label.
    CHECK(glyphsIn(out.label) == 12 + 3 * 6 + 11 + 3 * 6);
    // "REPEAT OFF" (10) plus three counts on the readout layer.
    CHECK(glyphsIn(out.foregroundReadout) == 10 + 3 * 2);
    // The well and its half fill, on top of the panel's own chrome.
    CHECK_FALSE(out.chrome.empty());
}

TEST_CASE("a helped build says who is lending rate, in the title line", "[ui][production]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::ProductionView view = factoryWith(1);
    view.building = true;
    view.progress = 0.25f;
    view.assistRate = 20.0f;

    rm::ui::Geometry helped;
    rm::ui::appendProductionPanel(helped, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 320, 154}, view);
    // "REPEAT OFF" (10), one count (2) and "ASSIST +20/S" (12) on the readout layer.
    CHECK(glyphsIn(helped.foregroundReadout) == 10 + 2 + 12);

    // Help nobody is giving is not announced, and neither is help to an idle factory.
    view.assistRate = 0.0f;
    rm::ui::Geometry alone;
    rm::ui::appendProductionPanel(alone, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 320, 154}, view);
    CHECK(glyphsIn(alone.foregroundReadout) == 10 + 2);
}

TEST_CASE("orders past the room expose page controls instead of losing cancellation", "[ui][production]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::Geometry out;
    // Two rows of room, seven orders: two rows, both cancel controls, and four pages.
    rm::ui::appendProductionPanel(out, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 320, 106}, factoryWith(7));
    CHECK(glyphsIn(out.label) == 12 + 2 * 6 + 11 + 2 * 6 + 2);
    CHECK(glyphsIn(out.foregroundReadout) == 10 + 2 * 2 + 3);
}

TEST_CASE("long factory and order names stay inside the production panel",
          "[ui][production][stress]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::ProductionView view = factoryWith(1);
    view.factoryName = "Experimental Mobile Rapid-Fire Artillery Installation";
    view.queue[0].name = "Experimental Strategic Missile Defence Construction Vehicle";
    view.queue[0].count = std::numeric_limits<std::uint32_t>::max();

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
                                  rm::ui::Rect{0, 0, 320, 154}, factoryWith(0));
    CHECK(glyphsIn(out.label) == 12 + 4 + 11);  // title, idle and clear

    rm::ui::Geometry none;
    rm::ui::appendProductionPanel(none, font, font, rm::ui::neutralTheme(),
                                  rm::ui::Rect{0, 0, 200, 126}, rm::ui::ProductionView{});
    CHECK(none.empty());
}
