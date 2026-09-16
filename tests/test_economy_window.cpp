// The economy-management window: the layout arithmetic, the hit regions a click can
// mean, the row grouping/sorting, and the weir's appear-only-under-scarcity rule.
// All of it is arithmetic over a view struct, so all of it is asserted rather than
// eyeballed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/EconomyWindow.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using rm::BuildPriority;
using rm::ui::EconRowView;
using rm::ui::EconomyWindowView;
using rm::ui::Rect;

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

[[nodiscard]] rm::ui::FrameLayout testFrame() {
    return rm::ui::frameLayout(rm::ui::UiViewport::authored(1600, 900));
}

[[nodiscard]] EconRowView row(const char* id, const char* job, BuildPriority tier,
                              float drain, std::uint32_t member = 1) {
    EconRowView view;
    view.id = id;
    view.name = id;
    view.job = job;
    view.priority = tier;
    view.massPerSecond = drain;
    view.members.push_back(rm::sim::UnitId{.index = member, .generation = 1});
    return view;
}

[[nodiscard]] EconomyWindowView viewWith(std::vector<EconRowView> rows) {
    EconomyWindowView view;
    view.weirIndex = rm::ui::econSortRows(rows);
    view.rows = std::move(rows);
    return view;
}

} // namespace

TEST_CASE("the window anchors top-centre of the battlefield and grows with its rows",
          "[ui][economy-window]") {
    const rm::ui::FrameLayout frame = testFrame();
    const Rect one = rm::ui::economyWindowRect(frame, 1);
    const Rect ten = rm::ui::economyWindowRect(frame, 10);
    CHECK(one.x >= frame.battlefield.x);
    CHECK(one.right() <= frame.battlefield.right());
    CHECK(one.y >= frame.battlefield.y);
    CHECK(one.y >= frame.economy.bottom());
    CHECK(one.y >= frame.match.bottom());
    // One row's worth of height exactly: the bands are fixed, the table is the variable.
    CHECK(ten.height - one.height == Catch::Approx(9.0f * 17.0f));
    // Past capacity the rect stops growing — the rows page instead of covering the map.
    const Rect huge = rm::ui::economyWindowRect(frame, 10000);
    CHECK(huge.bottom() <= frame.battlefield.bottom());
}

TEST_CASE("row capacity tracks the battlefield, and the page math divides it",
          "[ui][economy-window]") {
    const rm::ui::FrameLayout frame = testFrame();
    const std::size_t capacity = rm::ui::econRowCapacity(frame);
    REQUIRE(capacity > 3);
    const Rect rect = rm::ui::economyWindowRect(frame, 10000);
    CHECK(rm::ui::econRowsFor(rect) == capacity);

    const auto page0 = rm::ui::econPage(rect, capacity + 2, 0);
    CHECK(page0.pages == 2);
    CHECK(page0.first == 0);
    CHECK(page0.shown == capacity);
    const auto page1 = rm::ui::econPage(rect, capacity + 2, 1);
    CHECK(page1.first == capacity);
    CHECK(page1.shown == 2);
    // A requested page past the end clamps, not wraps.
    CHECK(rm::ui::econPage(rect, capacity + 2, 9).page == 1);
    CHECK(rm::ui::econPage(rect, 0, 0).pages == 1);
}

TEST_CASE("every row region a click can mean is reachable and distinct",
          "[ui][economy-window]") {
    const rm::ui::FrameLayout frame = testFrame();
    auto view = viewWith({row("UEL0309", "T2 MEX", BuildPriority::High, 10.0f, 1),
                          row("URB0101", "IDLE", BuildPriority::Normal, 0.0f, 2),
                          row("UEL0309", "ASSIST", BuildPriority::Normal, 6.0f, 3)});
    const Rect rect = rm::ui::economyWindowRect(frame, view.rows.size());
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const Rect rowRect = rm::ui::econRowRect(rect, i);
        CHECK(rect.contains(rowRect.x, rowRect.y));
        CHECK(rect.contains(rowRect.right() - 1, rowRect.bottom() - 1));
        // The body answers the row index; the cells do not answer the body's question.
        CHECK(rm::ui::econRowAt(rect, view, rowRect.x + 10, rowRect.y + 5) == i);
        const Rect priority = rm::ui::econPriorityCellRect(rect, i);
        const Rect pause = rm::ui::econPauseCellRect(rect, i);
        CHECK(rm::ui::econPriorityAt(rect, view, priority.x + 1, priority.y + 1) == i);
        CHECK_FALSE(rm::ui::econRowAt(rect, view, priority.x + 1, priority.y + 1));
        // Pause answers only for pausable rows — an unpausable cell draws no bars and
        // takes no click.
        CHECK_FALSE(rm::ui::econPauseAt(rect, view, pause.x + 1, pause.y + 1));
        CHECK_FALSE(rm::ui::econRowAt(rect, view, pause.x + 1, pause.y + 1));
    }
    view.rows[0].pausable = true;
    const Rect pause = rm::ui::econPauseCellRect(rect, 0);
    CHECK(rm::ui::econPauseAt(rect, view, pause.x + 1, pause.y + 1) == 0);
    // Off the window entirely, nothing answers.
    CHECK_FALSE(rm::ui::econRowAt(rect, view, rect.x - 4, rect.y + 4));
    CHECK_FALSE(rm::ui::econPriorityAt(rect, view, rect.x - 4, rect.y + 4));
}

TEST_CASE("hit regions address the drawn page, not the whole list",
          "[ui][economy-window]") {
    const rm::ui::FrameLayout frame = testFrame();
    std::vector<EconRowView> rows;
    for (std::uint32_t i = 0; i < 500; ++i) {
        rows.push_back(row("UEL0309", "WORK", BuildPriority::Normal, 1.0f, i + 1));
    }
    auto view = viewWith(std::move(rows));
    const Rect rect = rm::ui::economyWindowRect(frame, view.rows.size());
    const std::size_t capacity = rm::ui::econRowsFor(rect);
    REQUIRE(capacity < 500);

    const Rect lastRow = rm::ui::econRowRect(rect, capacity - 1);
    CHECK(rm::ui::econRowAt(rect, view, lastRow.x + 10, lastRow.y + 5, 1)
          == capacity * 2 - 1);
    // Page controls: forward from the first page, back from the second, nowhere else.
    const Rect prev = rm::ui::econPageButtonRect(rect, false);
    const Rect next = rm::ui::econPageButtonRect(rect, true);
    CHECK(rm::ui::econPageStepAt(rect, view, next.x + 1, next.y + 1, 0) == 1);
    CHECK_FALSE(rm::ui::econPageStepAt(rect, view, prev.x + 1, prev.y + 1, 0));
    CHECK(rm::ui::econPageStepAt(rect, view, prev.x + 1, prev.y + 1, 1) == -1);
    // One page fits all — no controls answer.
    auto small = viewWith({row("A", "J", BuildPriority::Normal, 1.0f, 1)});
    const Rect smallRect = rm::ui::economyWindowRect(frame, small.rows.size());
    CHECK_FALSE(rm::ui::econPageStepAt(smallRect, small, next.x + 1, next.y + 1, 0));
}

TEST_CASE("the fabricator strip: cells while readable, the slider always",
          "[ui][economy-window]") {
    const rm::ui::FrameLayout frame = testFrame();
    EconomyWindowView view;
    for (std::uint32_t i = 0; i < 4; ++i) {
        view.fabricators.push_back({.unit = rm::sim::UnitId{i, 1}, .name = "fab",
                                    .energyDraw = 50.0f, .running = true});
    }
    view.fabDrawTotal = 200.0f;
    view.fabBudget = 150.0f;
    const Rect rect = rm::ui::economyWindowRect(frame, 0);

    for (std::size_t fab = 0; fab < view.fabricators.size(); ++fab) {
        const Rect cell = rm::ui::econFabCellRect(rect, fab);
        CHECK(rm::ui::econFabAt(rect, view, cell.x + 1, cell.y + 1) == fab);
    }
    // Past the cell cap the per-fab targets go away entirely — the slider stays.
    for (std::uint32_t i = 4; i < 20; ++i) {
        view.fabricators.push_back({.unit = rm::sim::UnitId{i, 1}, .name = "fab",
                                    .energyDraw = 10.0f, .running = false});
    }
    view.fabDrawTotal = 360.0f;
    const Rect first = rm::ui::econFabCellRect(rect, 0);
    CHECK_FALSE(rm::ui::econFabAt(rect, view, first.x + 1, first.y + 1));

    const Rect track = rm::ui::econSliderTrack(rect);
    CHECK(rm::ui::econSliderAt(rect, view, track.x + 1, track.y + 5));
    CHECK(rm::ui::econSliderValueAt(rect, view, track.x) == Catch::Approx(0.0f));
    CHECK(rm::ui::econSliderValueAt(rect, view, track.right())
          == Catch::Approx(view.fabDrawTotal));
    CHECK(rm::ui::econSliderValueAt(rect, view, track.x + track.width * 0.5f)
          == Catch::Approx(180.0f));
    // Past the ends clamps rather than running the budget negative or past the total.
    CHECK(rm::ui::econSliderValueAt(rect, view, track.x - 40.0f) == Catch::Approx(0.0f));
    CHECK(rm::ui::econSliderValueAt(rect, view, track.right() + 40.0f)
          == Catch::Approx(view.fabDrawTotal));
    // No fabricators means no track to grab.
    EconomyWindowView empty;
    CHECK_FALSE(rm::ui::econSliderAt(rect, empty, track.x + 1, track.y + 5));
}

TEST_CASE("the close control is a real target", "[ui][economy-window]") {
    const Rect rect = rm::ui::economyWindowRect(testFrame(), 3);
    const Rect close = rm::ui::econCloseRect(rect);
    CHECK(rect.contains(close.x, close.y));
    CHECK(rm::ui::econCloseAt(rect, close.x + 1, close.y + 1));
    CHECK_FALSE(rm::ui::econCloseAt(rect, close.x - 4, close.y + 1));
    CHECK_FALSE(rm::ui::econCloseAt({}, 0, 0));
}

TEST_CASE("grouping merges by type, job and tier — and only those three",
          "[ui][economy-window]") {
    std::vector<EconRowView> rows;
    auto a = row("UEL0309", "T2 MEX", BuildPriority::Normal, 10.0f, 1);
    a.energyPerSecond = 60.0f;
    a.funded = 1.0f;
    auto b = row("UEL0309", "T2 MEX", BuildPriority::Normal, 10.0f, 2);
    b.energyPerSecond = 20.0f;
    b.funded = 0.5f;
    rows.push_back(a);
    rows.push_back(b);
    rows.push_back(row("UEL0309", "T2 MEX", BuildPriority::High, 10.0f, 3));
    rows.push_back(row("UEL0309", "ASSIST", BuildPriority::Normal, 5.0f, 4));

    const auto grouped = rm::ui::econGroupedRows(std::move(rows));
    REQUIRE(grouped.size() == 3);
    const auto& merged = grouped[0];
    CHECK(merged.members.size() == 2);
    CHECK(merged.massPerSecond == Catch::Approx(20.0f));
    CHECK(merged.energyPerSecond == Catch::Approx(80.0f));
    // Drain-weighted: (1.0*70 + 0.5*30) / 100.
    CHECK(merged.funded == Catch::Approx(0.85f));
}

TEST_CASE("sort puts priority first, drain second, and returns the weir",
          "[ui][economy-window]") {
    std::vector<EconRowView> rows{
        row("URB0101", "IDLE", BuildPriority::Normal, 0.0f, 9),
        row("UEL0309", "T2 MEX", BuildPriority::Normal, 10.0f, 2),
        row("XEB2306", "NUKE", BuildPriority::High, 50.0f, 1),
        row("UEL0309", "REPAIR", BuildPriority::Normal, 20.0f, 3),
        row("UEB0101", "PLATOON", BuildPriority::High, 5.0f, 4),
    };
    const std::size_t weir = rm::ui::econSortRows(rows);
    CHECK(weir == 2);
    CHECK(rows[0].priority == BuildPriority::High);
    CHECK(rows[1].priority == BuildPriority::High);
    CHECK(rows[0].id == "XEB2306");  // the bigger drain leads the tier
    CHECK(rows[2].id == "UEL0309");
    CHECK(rows[2].job == "REPAIR");  // 20/s ahead of 10/s inside Normal
    CHECK(rows[4].job == "IDLE");    // zero drain last
}

TEST_CASE("the weir divider exists only under scarcity", "[ui][economy-window]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    const rm::ui::Theme theme = rm::ui::neutralTheme();
    const rm::ui::FrameLayout frame = testFrame();

    auto rows = {row("XEB2306", "NUKE", BuildPriority::High, 50.0f, 1),
                 row("UEL0309", "MEX", BuildPriority::Normal, 10.0f, 2)};
    auto stalled = viewWith(std::vector<EconRowView>(rows));
    stalled.fundedFraction = 0.4f;
    stalled.massBinding = false;
    stalled.tierFunded = {0.2f, 0.4f, 1.0f};
    stalled.tierAsked = {true, true, true};
    const Rect rect = rm::ui::economyWindowRect(frame, stalled.rows.size());

    rm::ui::Geometry out;
    rm::ui::appendEconomyWindow(out, font, font, theme, rect, stalled);
    REQUIRE_FALSE(out.chrome.empty());

    // The dashed rule between row 0 and row 1 — a horizontal run of short segments at
    // the boundary, which a funded economy does not draw. Interior x only: a row
    // band's bottom edge shares the rule's y, but only dash corners land inside the
    // band's width, which is what makes the count mean "dashes" and not "edges".
    const float ruleY = rm::ui::econRowRect(rect, 1).y - 1.0f;
    const Rect firstRow = rm::ui::econRowRect(rect, 0);
    const auto dashVerts = [ruleY, &firstRow](const rm::ui::Geometry& geometry) {
        std::size_t count = 0;
        for (const rm::text::TextVertex& vertex : geometry.chrome) {
            if (vertex.position[1] == Catch::Approx(ruleY).margin(0.01f)
                && vertex.position[0] > firstRow.x + 4.0f
                && vertex.position[0] < firstRow.right() - 10.0f) {
                ++count;
            }
        }
        return count;
    };
    CHECK(dashVerts(out) > 10);  // a dashed run across the row's width

    rm::ui::Geometry funded;
    auto flush = stalled;
    flush.fundedFraction = 1.0f;
    rm::ui::appendEconomyWindow(funded, font, font, theme, rect, flush);
    CHECK(dashVerts(funded) == 0);
}

TEST_CASE("a drawn row is the hit-tested row", "[ui][economy-window]") {
    // The regression shape every overlay shares: the click and the pixels have to come
    // from the same arithmetic. The row a vertex was drawn in is the row the click
    // answers for.
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    const rm::ui::Theme theme = rm::ui::neutralTheme();
    auto view = viewWith({row("UEL0309", "MEX", BuildPriority::Normal, 10.0f, 7),
                          row("URB0101", "IDLE", BuildPriority::Normal, 0.0f, 8)});
    const Rect rect = rm::ui::economyWindowRect(testFrame(), view.rows.size());
    rm::ui::Geometry out;
    rm::ui::appendEconomyWindow(out, font, font, theme, rect, view);

    const Rect rowOne = rm::ui::econRowRect(rect, 1);
    bool inside = false;
    for (const rm::text::TextVertex& vertex : out.label) {
        if (rowOne.contains(vertex.position[0], vertex.position[1])) inside = true;
    }
    CHECK(inside);
    CHECK(rm::ui::econRowAt(rect, view, rowOne.x + 10, rowOne.y + 5) == 1);
}
