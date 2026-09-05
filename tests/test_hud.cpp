// The interface: its readings, its formatting, and the one colour rule it must not break.
//
// A HUD is the last place a bug is noticed, because a wrong number still looks like a number.
// These check the things a screenshot cannot: that a bar clamps, that a livery never collides
// with a fixed colour, and that a banner appears only when the match is actually over.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/BuildPanel.hpp"  // the cell metrics the column counts have to keep room for
#include "core/ui/Hud.hpp"
#include "core/ui/Roster.hpp"

#include <cmath>
#include <string_view>
#include <vector>

using Catch::Approx;
using rm::ui::Gauge;
using rm::ui::MatchState;

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

[[nodiscard]] rm::ui::FrameLayout frameAt(float width, float height) {
    return rm::ui::frameLayout(rm::ui::UiViewport::authored(width, height));
}

/// How far apart two colours are, ignoring alpha. Used to assert that a livery and a fixed
/// colour are TELLABLE APART, which is a stronger claim than being unequal.
[[nodiscard]] float distance(rm::ui::Colour a, rm::ui::Colour b) {
    const float dr = a[0] - b[0];
    const float dg = a[1] - b[1];
    const float db = a[2] - b[2];
    return std::sqrt(dr * dr + dg * dg + db * db);
}

} // namespace

TEST_CASE("construction inspector renders a bounded progress bar in the real roster slot", "[ui][build]") {
    const auto glyphs = boxGlyphs();
    const auto font = fontOver(glyphs);
    const rm::ui::Rect rect{20, 30, 200, 68};
    rm::ui::InfoCard card;
    card.title = "BUILDING";
    for (const float progress : {0.0f, 0.5f, 1.0f, 2.0f}) {
        card.progress = progress;
        rm::ui::Geometry geometry;
        rm::ui::appendInspector(geometry, font, font, rm::ui::neutralTheme(), rect, card);
        REQUIRE(geometry.chrome.size() >= 6);
        float right = rect.x + 4;
        for (const auto& vertex : geometry.chrome) {
            CHECK(vertex.position[0] >= rect.x);
            CHECK(vertex.position[0] <= rect.right());
            CHECK(vertex.position[1] >= rect.y);
            CHECK(vertex.position[1] <= rect.bottom());
        }
        if (progress > 0) {
            REQUIRE(geometry.chrome.size() == 12);
            for (std::size_t i = 6; i < 12; ++i) {
                right = std::max(right, geometry.chrome[i].position[0]);
            }
            CHECK(right == Approx(rect.x + 4 + 192 * std::min(progress, 1.0f)));
        }
    }
}

TEST_CASE("a gauge's fill clamps, and no capacity reads as empty") {
    CHECK(Gauge{.stored = 50.0f, .capacity = 100.0f}.fill() == Approx(0.5f));
    CHECK(Gauge{.stored = 0.0f, .capacity = 100.0f}.fill() == Approx(0.0f));

    // Over capacity is full rather than a bar past its own end.
    CHECK(Gauge{.stored = 500.0f, .capacity = 100.0f}.fill() == Approx(1.0f));

    // NO CAPACITY IS EMPTY, not full. An army with no storage has nowhere to put anything, and
    // a full bar would say precisely the opposite of the truth.
    CHECK(Gauge{.stored = 0.0f, .capacity = 0.0f}.fill() == Approx(0.0f));
    CHECK(Gauge{.stored = 10.0f, .capacity = 0.0f}.fill() == Approx(0.0f));
}

TEST_CASE("each game profile presents exactly two ordered resource views") {
    STATIC_REQUIRE(std::tuple_size_v<rm::ui::ResourceViews> == 2);

    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    CHECK(rm::ui::kChip + rm::ui::kUnit
              + rm::text::measureText(glyphs, "MATERIAL")
          <= rm::ui::kLabelColumn);

    const Gauge primary{.stored = 11.0f};
    const Gauge energy{.stored = 22.0f};
    for (const auto& [profile, firstName] :
         std::array<std::pair<rm::ui::GameProfile, std::string_view>, 4>{
             {{rm::ui::GameProfile::Fa, "MASS"},
              {rm::ui::GameProfile::Bar, "METAL"},
              {rm::ui::GameProfile::Neutral, "MATERIAL"},
              {rm::ui::GameProfile::ClassicFaf, "MASS"}}}) {
        const rm::ui::ResourceViews views = rm::ui::resourceViews(profile, primary, energy);
        CHECK(views[0].name == firstName);
        CHECK(views[0].gauge.stored == 11.0f);
        CHECK(views[0].tint == rm::ui::kMass);
        CHECK(views[1].name == "ENERGY");
        CHECK(views[1].gauge.stored == 22.0f);
        CHECK(views[1].tint == rm::ui::kEnergy);
    }

    CHECK(rm::ui::gameProfile(rm::ui::GameProfile::Fa).factionOwned);
    CHECK(rm::ui::gameProfile(rm::ui::GameProfile::ClassicFaf).classicChrome);
    CHECK_FALSE(rm::ui::gameProfile(rm::ui::GameProfile::Bar).factionOwned);
    CHECK_FALSE(rm::ui::gameProfile(rm::ui::GameProfile::Neutral).factionOwned);
}

TEST_CASE("the responsive frame selects the largest profile that fits") {
    const rm::ui::FrameLayout compact = frameAt(1280.0f, 720.0f);
    CHECK(compact.profile == rm::ui::HudProfile::Compact);
    CHECK(compact.minimap.width == 176.0f);
    CHECK(compact.minimap.height == 176.0f);
    CHECK(compact.selection.width == 320.0f);
    CHECK(compact.build.width == 528.0f);
    CHECK(compact.buildColumns == 6);
    CHECK(compact.rosterSlots == 5);

    const rm::ui::FrameLayout standard = frameAt(1600.0f, 900.0f);
    CHECK(standard.profile == rm::ui::HudProfile::Standard);
    CHECK(standard.minimap.width == 216.0f);
    CHECK(standard.buildColumns == 7);
    CHECK(standard.rosterSlots == 8);

    const rm::ui::FrameLayout wide = frameAt(2240.0f, 1000.0f);
    CHECK(wide.profile == rm::ui::HudProfile::Wide);
    CHECK(wide.minimap.width == 256.0f);
    CHECK(wide.buildColumns == 9);
    CHECK(wide.rosterSlots == 12);

    // Width alone is not enough: a low-height ultrawide uses the lower vertical metrics.
    CHECK(frameAt(2400.0f, 900.0f).profile == rm::ui::HudProfile::Standard);

    // A WIDER PROFILE NEVER MEANS A NARROWER CELL, which is the trap the column counts fell
    // into: nine cells in 528 points and sixteen in 893 made the button SHRINK as the panel
    // grew, from 54 points across to 52. A cell has to hold a name, so the count is chosen to
    // keep the width, not to fill the panel.
    const auto cellWidth = [](const rm::ui::FrameLayout& frame) {
        const auto columns = static_cast<float>(frame.buildColumns);
        return (frame.build.width - rm::ui::kBuildPadding * 2.0f
                - (columns - 1.0f) * rm::ui::kBuildGap)
             / columns;
    };
    CHECK(cellWidth(standard) >= cellWidth(compact));
    CHECK(cellWidth(wide) >= cellWidth(standard));
}

TEST_CASE("the interface grows with the viewport instead of subdividing it") {
    // THE BUG THIS LOCKS DOWN, and it was backwards in the way that is hardest to see: every
    // metric in `Hud.hpp` is a constant in points, so a bigger window used to spend the extra
    // room on MORE COLUMNS rather than bigger ones — nine cells across 528 points at 1280 wide,
    // sixteen across 893 at 2240, and the cell itself shrinking from 54 points to 52. The
    // interface got physically smaller relative to the screen the larger the screen was.
    CHECK(rm::ui::hudScale(1280.0f, 720.0f) == Approx(1.0f));
    CHECK(rm::ui::hudScale(2560.0f, 1440.0f) == Approx(2.0f));

    // MONOTONIC, which is the property the old behaviour violated: a wider window is never a
    // smaller interface.
    float previous = 0.0f;
    for (const float width : {1280.0f, 1440.0f, 1600.0f, 1920.0f, 2560.0f, 3840.0f}) {
        const float scale = rm::ui::hudScale(width, width * 9.0f / 16.0f);
        CHECK(scale >= previous);
        previous = scale;
    }

    // A headless output may be smaller than the window's enforced minimum. It scales the whole
    // authored frame down rather than clipping or overlapping Compact.
    CHECK(rm::ui::hudScale(800.0f, 600.0f) == Approx(0.625f));

    // THE LIMITING AXIS DECIDES. A 3440x1440 ultrawide has the width for 2.68x and the height
    // for 2.0, and magnifying by the width would push the bottom deck off its own screen.
    CHECK(rm::ui::hudScale(3440.0f, 1440.0f) == Approx(2.0f));

    // Capped, or a 5K panel gets a build tray the size of a paperback.
    CHECK(rm::ui::hudScale(5120.0f, 2880.0f)
          == Approx(rm::ui::kMaxAutomaticHudScale));

    // The player's preference multiplies the automatic figure, but not past the room available
    // for Compact. At the minimum supported viewport there is no spare room; a 5K viewport has
    // room above the automatic cap. The preference itself remains bounded.
    CHECK(rm::ui::hudScale(1280.0f, 720.0f, 1.5f) == Approx(1.0f));
    CHECK(rm::ui::hudScale(5120.0f, 2880.0f, 1.5f) == Approx(3.75f));
    CHECK(rm::ui::hudScale(10000.0f, 5625.0f, 99.0f)
          == Approx(rm::ui::kMaxAutomaticHudScale * rm::ui::kMaxUserHudScale));
    CHECK(rm::ui::hudScale(1280.0f, 720.0f, 0.01f) == Approx(rm::ui::kMinUserHudScale));

    // Captures may be smaller than the window's enforced 1280x720 minimum. There the HUD scales
    // down to preserve its authored extent, and a large preference cannot collapse it further.
    CHECK(rm::ui::hudScale(640.0f, 360.0f, 3.0f) == Approx(0.5f));

    // A viewport of nothing still answers, because a window can be zero-sized mid-resize and a
    // division by it would take the layout with it.
    CHECK(rm::ui::hudScale(0.0f, 0.0f) > 0.0f);
}

TEST_CASE("a magnified interface lays out in design space, not the window's") {
    // The frame is fed `width / scale`, so the SAME layout serves every viewport and the
    // renderer does the enlarging. What that buys is the cell count staying put: six columns at
    // 1280 and six columns at 2560, each twice the size.
    const rm::ui::FrameLayout big =
        rm::ui::frameLayout(rm::ui::UiViewport::full(2560.0f, 1440.0f));
    const rm::ui::FrameLayout small = frameAt(1280.0f, 720.0f);
    CHECK(big.buildColumns == small.buildColumns);
    CHECK(big.build.width == small.build.width);
    CHECK(big.minimap.width == small.minimap.width);
}

TEST_CASE("responsive frame modules stay anchored and do not overlap") {
    for (const std::array<float, 2> viewport :
         {std::array{1280.0f, 720.0f}, std::array{1600.0f, 900.0f},
          std::array{2240.0f, 1000.0f}, std::array{2560.0f, 1080.0f}}) {
        const rm::ui::FrameLayout frame = frameAt(viewport[0], viewport[1]);
        CHECK(frame.economy.x == rm::ui::kMargin);
        CHECK(frame.economy.y == rm::ui::kMargin);
        CHECK(frame.match.right() == viewport[0] - rm::ui::kMargin);
        CHECK(frame.minimap.bottom() == viewport[1] - rm::ui::kMargin);
        CHECK(frame.commands.right() == viewport[0] - rm::ui::kMargin);
        CHECK(frame.commands.bottom() == viewport[1] - rm::ui::kMargin);
        CHECK(frame.minimap.right() <= frame.selection.x);
        CHECK(frame.selection.right() <= frame.build.x);
        CHECK(frame.build.right() <= frame.commands.x);
        CHECK(frame.battlefield.bottom() <= frame.minimap.y);
    }
}

TEST_CASE("responsive frame anchors inside safe content") {
    const rm::ui::UiViewport viewport = rm::ui::UiViewport::withSafeContent(
        1280.0f, 720.0f, 2.0f, {40.0f, 24.0f, 1200.0f, 672.0f});
    const rm::ui::FrameLayout frame = rm::ui::frameLayout(viewport);

    CHECK(frame.viewport.x == 0.0f);
    CHECK(frame.viewport.width == 1280.0f);
    CHECK(frame.safeContent.x == 40.0f);
    CHECK(frame.safeContent.y == 24.0f);
    CHECK(frame.economy.x == frame.safeContent.x + rm::ui::kMargin);
    CHECK(frame.economy.y == frame.safeContent.y + rm::ui::kMargin);
    CHECK(frame.match.right() == frame.safeContent.right() - rm::ui::kMargin);
    CHECK(frame.commands.bottom() == frame.safeContent.bottom() - rm::ui::kMargin);
    CHECK(frame.minimap.right() <= frame.selection.x);
    CHECK(frame.selection.right() <= frame.build.x);
    CHECK(frame.build.right() <= frame.commands.x);
}

TEST_CASE("UI preference cannot magnify Compact into overlapping itself") {
    const rm::ui::UiViewport viewport =
        rm::ui::UiViewport::full(1280.0f, 720.0f, 2.0f, 3.0f);
    const rm::ui::FrameLayout frame = rm::ui::frameLayout(viewport);

    CHECK(viewport.hudScale() == 1.0f);
    CHECK(frame.selection.right() <= frame.build.x);
    CHECK(frame.build.right() <= frame.commands.x);
}

TEST_CASE("undersized output scales Compact down instead of overlapping it") {
    const rm::ui::UiViewport viewport = rm::ui::UiViewport::full(640.0f, 360.0f, 1.0f, 3.0f);
    const rm::ui::FrameLayout frame = rm::ui::frameLayout(viewport);

    CHECK(viewport.hudExtent().width == 1280.0f);
    CHECK(viewport.hudExtent().height == 720.0f);
    CHECK(viewport.fontRasterScale() == 1.0f);
    CHECK(viewport.toDrawable({1280.0f, 720.0f}) == std::array{640.0f, 360.0f});
    CHECK(frame.minimap.right() <= frame.selection.x);
    CHECK(frame.selection.right() <= frame.build.x);
    CHECK(frame.build.right() <= frame.commands.x);
}

TEST_CASE("a constrained authored frame contracts modules without overlap") {
    const rm::ui::FrameLayout frame = frameAt(640.0f, 480.0f);

    CHECK(frame.minimap.right() <= frame.selection.x);
    CHECK(frame.selection.right() <= frame.build.x);
    CHECK(frame.build.right() <= frame.commands.x);
}

TEST_CASE("a full store that is still earning is wasting, and says so") {
    // The economy discards anything past capacity, and the lit cap on the bar is the only place
    // that fact is visible.
    CHECK(Gauge{.stored = 100.0f, .capacity = 100.0f, .incomePerSecond = 5.0f}.wasting());

    // Full but breaking even is not waste.
    CHECK_FALSE(Gauge{.stored = 100.0f,
                      .capacity = 100.0f,
                      .incomePerSecond = 5.0f,
                      .drainPerSecond = 5.0f}
                    .wasting());
    // Nor is earning with room to spare.
    CHECK_FALSE(Gauge{.stored = 50.0f, .capacity = 100.0f, .incomePerSecond = 5.0f}.wasting());
    // Nor is a gauge with no capacity at all, which would otherwise be "full" by division.
    CHECK_FALSE(Gauge{.stored = 0.0f, .capacity = 0.0f, .incomePerSecond = 5.0f}.wasting());
}

TEST_CASE("a rate carries its sign, and zero does not claim one") {
    CHECK(rm::ui::formatRate(46.0f) == "+46");
    CHECK(rm::ui::formatRate(-40.0f) == "-40");
    CHECK(rm::ui::formatRate(2.5f) == "+2.5");

    // "+0" reads as a claim about something that is not happening.
    CHECK(rm::ui::formatRate(0.0f) == "0");
    CHECK(rm::ui::formatRate(0.01f) == "0");
}

TEST_CASE("an amount keeps its column width as it grows") {
    // The reason the readout face is monospaced in the first place: a number that changes width
    // as it climbs makes the whole column jitter.
    CHECK(rm::ui::formatAmount(0.0f) == "0");
    CHECK(rm::ui::formatAmount(650.0f) == "650");
    CHECK(rm::ui::formatAmount(5000.0f) == "5000");
    CHECK(rm::ui::formatAmount(12400.0f) == "12.4k");
    CHECK(rm::ui::formatAmount(1000000.0f) == "1000.0k");
}

TEST_CASE("a clock grows an hours field only when it needs one") {
    CHECK(rm::ui::formatClock(0.0f) == "0:00");
    CHECK(rm::ui::formatClock(64.5f) == "1:04");
    CHECK(rm::ui::formatClock(3599.0f) == "59:59");
    CHECK(rm::ui::formatClock(3600.0f) == "1:00:00");

    // A negative elapsed time is a caller's arithmetic showing, and reads as zero rather than
    // as a clock running backwards.
    CHECK(rm::ui::formatClock(-10.0f) == "0:00");
}

TEST_CASE("no faction's livery collides with a colour that means something") {
    // THE RULE THIS INTERFACE IS BUILT ON. Mass is green, energy is amber, a gain is green and a
    // loss is red, in every livery — a player reads those without looking. So when a faction's
    // own colour lands on one of them, it is the LIVERY that moves: Aeon runs turquoise rather
    // than leaf green, Cybran crimson rather than orange-red, Seraphim pale gold rather than
    // amber.
    //
    // Asserted as a DISTANCE rather than an inequality, because two colours can differ in the
    // last bit and still be the same colour to an eye.
    constexpr float kTellableApart = 0.25f;
    const std::array<rm::ui::Colour, 4> fixed{
        {rm::ui::kMass, rm::ui::kEnergy, rm::ui::kGain, rm::ui::kLoss}};

    for (const rm::sim::Faction faction :
         {rm::sim::Faction::Uef, rm::sim::Faction::Aeon, rm::sim::Faction::Cybran,
          rm::sim::Faction::Seraphim}) {
        const rm::ui::Theme theme = rm::ui::themeFor(faction);
        for (const rm::ui::Colour& reserved : fixed) {
            CHECK(distance(theme.edgeLit, reserved) > kTellableApart);
        }
    }
}

TEST_CASE("every faction's livery is tellable from every other") {
    // Otherwise the chrome stops saying whose interface this is, which is the only job it has
    // beyond looking like something.
    const std::array<rm::ui::Theme, 4> themes{
        {rm::ui::themeFor(rm::sim::Faction::Uef), rm::ui::themeFor(rm::sim::Faction::Aeon),
         rm::ui::themeFor(rm::sim::Faction::Cybran),
         rm::ui::themeFor(rm::sim::Faction::Seraphim)}};

    for (std::size_t i = 0; i < themes.size(); ++i) {
        for (std::size_t j = i + 1; j < themes.size(); ++j) {
            CHECK(distance(themes[i].edgeLit, themes[j].edgeLit) > 0.25f);
        }
    }
}

TEST_CASE("faction materials differ without changing their semantic palette") {
    const std::array<rm::ui::PanelMaterial, 6> materials{{
        rm::ui::themeFor(rm::sim::Faction::Uef).material,
        rm::ui::themeFor(rm::sim::Faction::Aeon).material,
        rm::ui::themeFor(rm::sim::Faction::Cybran).material,
        rm::ui::themeFor(rm::sim::Faction::Seraphim).material,
        rm::ui::barTheme().material,
        rm::ui::neutralTheme().material,
    }};
    for (std::size_t first = 0; first < materials.size(); ++first) {
        for (std::size_t second = first + 1; second < materials.size(); ++second) {
            CHECK(materials[first] != materials[second]);
        }
    }
}

TEST_CASE("a livery's glass is dark enough to read numbers over") {
    // A panel is behind text. However bright a faction's accent, the glass derived from it has
    // to stay near black or the readouts lose their contrast — which is why the glass is
    // derived from the accent rather than being the accent.
    for (const rm::sim::Faction faction :
         {rm::sim::Faction::Uef, rm::sim::Faction::Aeon, rm::sim::Faction::Cybran,
          rm::sim::Faction::Seraphim}) {
        const rm::ui::Theme theme = rm::ui::themeFor(faction);
        const float luminance = theme.glass[0] * 0.2126f + theme.glass[1] * 0.7152f
                              + theme.glass[2] * 0.0722f;
        CHECK(luminance < 0.2f);
        CHECK(theme.well[0] + theme.well[1] + theme.well[2]
              < theme.glass[0] + theme.glass[1] + theme.glass[2]);  // a well is recessed
    }
}

TEST_CASE("the interface draws something, and the chrome outweighs the numbers") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);

    MatchState state;
    state.resources = rm::ui::resourceViews(
        rm::ui::GameProfile::Fa,
        Gauge{.stored = 120.0f, .capacity = 650.0f, .incomePerSecond = 2.5f},
        Gauge{.stored = 144.0f,
              .capacity = 5000.0f,
              .incomePerSecond = 5.0f,
              .drainPerSecond = 2.0f});
    state.unitsAlive = 6;
    state.armiesLeft = 6;
    state.armiesTotal = 8;
    state.elapsedSeconds = 40.0f;

    rm::ui::Geometry out;
    rm::ui::build(out, font, font, rm::ui::neutralTheme(), state, frameAt(1400.0f, 900.0f));

    // Every visual role is independent: surfaces and chrome no longer borrow the label stream.
    CHECK_FALSE(out.panelSurface.solid.empty());
    CHECK_FALSE(out.chrome.empty());
    CHECK_FALSE(out.label.empty());
    CHECK_FALSE(out.foregroundReadout.empty());

    // The chrome is REALLY there, not only a renamed label list: panels, gauges and brackets
    // outweigh the two resource words.
    const std::size_t labelGlyphs = std::string_view{"MASSENERGY"}.size();
    CHECK(out.chrome.size() > labelGlyphs * rm::text::kVerticesPerGlyph * 2);

    // Whole quads, which is the unit every semantic capacity partition admits or drops.
    for (const std::size_t submitted : out.submittedVertices()) {
        CHECK(submitted % rm::text::kVerticesPerGlyph == 0);
    }
}

TEST_CASE("a skinned panel stays in the panel-surface image stream", "[ui][layers]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    rm::ui::Theme theme = rm::ui::neutralTheme();
    theme.skin.active = true;
    for (std::size_t piece = 0; piece < theme.skin.size.size(); ++piece) {
        theme.skin.uv[piece] = {.u0 = 0.0f, .v0 = 0.0f, .u1 = 0.1f, .v1 = 0.1f};
        theme.skin.size[piece] = {4.0f, 4.0f};
    }

    rm::ui::Geometry out;
    rm::ui::appendPanel(out, font, theme, 10.0f, 20.0f, 100.0f, 80.0f);

    CHECK(out.panelSurface.solid.size() == rm::text::kVerticesPerGlyph);  // shadow
    CHECK(out.panelSurface.image.size() == 9 * rm::text::kVerticesPerGlyph);
    CHECK(out.chrome.empty());  // the nine-slice replaces procedural chrome
}

TEST_CASE("build panel art and type use their semantic layers", "[ui][layers]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    const rm::ui::BuildOption option{
        .id = "UEB1103", .name = "Mass Extractor", .massCost = 36.0f, .iconSlot = 0};
    const rm::ui::BuildPanelLayout layout =
        rm::ui::buildPanelLayout(frameAt(1400.0f, 900.0f), 1);

    rm::ui::Geometry out;
    rm::ui::appendBuildPanel(out, font, font, rm::ui::neutralTheme(), layout,
                             std::span<const rm::ui::BuildOption>{&option, 1}, std::nullopt,
                             "Commander", "COMMANDER");

    CHECK_FALSE(out.panelSurface.empty());
    CHECK_FALSE(out.chrome.empty());
    CHECK(out.icon.size() == rm::text::kVerticesPerGlyph);
    CHECK_FALSE(out.label.empty());
    CHECK(out.foregroundReadout.empty());  // face values are disabled by this HUD profile
    CHECK(out.worldOverlay.empty());
}

TEST_CASE("a roster name fallback remains a label rather than a readout", "[ui][layers]") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);
    const rm::ui::RosterLayout layout =
        rm::ui::rosterLayout(frameAt(1400.0f, 900.0f), 1);
    const rm::ui::RosterTile withoutIcon{
        .id = "UEL0201", .name = "Medium Tank", .health = 300.0f, .maxHealth = 300.0f};
    rm::ui::RosterTile withIcon = withoutIcon;
    withIcon.iconSlot = 0;

    rm::ui::Geometry fallback;
    rm::ui::appendRoster(fallback, font, font, rm::ui::neutralTheme(), layout,
                         std::span<const rm::ui::RosterTile>{&withoutIcon, 1}, std::nullopt);
    rm::ui::Geometry pictured;
    rm::ui::appendRoster(pictured, font, font, rm::ui::neutralTheme(), layout,
                         std::span<const rm::ui::RosterTile>{&withIcon, 1}, std::nullopt);

    CHECK(fallback.icon.empty());
    CHECK(pictured.icon.size() == rm::text::kVerticesPerGlyph);
    CHECK(fallback.label.size() > pictured.label.size());
    CHECK(fallback.foregroundReadout.empty());
}

TEST_CASE("the banner appears only when the match is over") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);

    MatchState running;
    running.armiesLeft = 4;
    running.armiesTotal = 8;

    rm::ui::Geometry mid;
    rm::ui::build(mid, font, font, rm::ui::neutralTheme(), running,
                  frameAt(1400.0f, 900.0f));

    MatchState won = running;
    won.armiesLeft = 1;
    won.outcome = MatchState::Outcome::Win;
    won.winningTeam = 6;

    rm::ui::Geometry over;
    rm::ui::build(over, font, font, rm::ui::neutralTheme(), won,
                  frameAt(1400.0f, 900.0f));

    // The banner is the largest thing the interface ever draws, so it is unmistakable in the
    // vertex count. A draw and a win both produce one; a running match produces none.
    CHECK(over.label.size() > mid.label.size());

    MatchState drawn = won;
    drawn.armiesLeft = 0;
    drawn.outcome = MatchState::Outcome::Draw;
    rm::ui::Geometry drawnOut;
    rm::ui::build(drawnOut, font, font, rm::ui::neutralTheme(), drawn,
                  frameAt(1400.0f, 900.0f));
    CHECK(drawnOut.label.size() > mid.label.size());
}

TEST_CASE("a stall adds a line, and no stall does not") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);

    MatchState funded;
    funded.fundedFraction = 1.0f;
    MatchState stalling = funded;
    stalling.fundedFraction = 0.08f;

    rm::ui::Geometry a;
    rm::ui::Geometry b;
    rm::ui::build(a, font, font, rm::ui::neutralTheme(), funded,
                  frameAt(1400.0f, 900.0f));
    rm::ui::build(b, font, font, rm::ui::neutralTheme(), stalling,
                  frameAt(1400.0f, 900.0f));

    // A line that always reads 100% is one a player stops seeing, and then misses at 40%.
    CHECK(b.label.size() > a.label.size());
}

TEST_CASE("a missing face costs its own text and nothing else") {
    // The label face and the readout face are rasterised separately, and either can fail. Losing
    // one must not take the interface with it.
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font good = fontOver(glyphs);
    const rm::text::Font missing;

    MatchState state;
    state.armiesTotal = 2;
    state.armiesLeft = 2;

    rm::ui::Geometry noLabels;
    rm::ui::build(noLabels, missing, good, rm::ui::neutralTheme(), state,
                  frameAt(1400.0f, 900.0f));
    CHECK_FALSE(noLabels.empty());  // surfaces, chrome, and readouts remain
    CHECK(noLabels.label.empty());
    CHECK_FALSE(noLabels.foregroundReadout.empty());

    rm::ui::Geometry noReadouts;
    rm::ui::build(noReadouts, good, missing, rm::ui::neutralTheme(), state,
                  frameAt(1400.0f, 900.0f));
    CHECK_FALSE(noReadouts.label.empty());
    CHECK(noReadouts.foregroundReadout.empty());

    // Neither face: nothing at all, rather than a crash.
    rm::ui::Geometry nothing;
    rm::ui::build(nothing, missing, missing, rm::ui::neutralTheme(), state,
                  frameAt(1400.0f, 900.0f));
    CHECK(nothing.empty());
}
