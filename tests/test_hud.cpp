// The interface: its readings, its formatting, and the one colour rule it must not break.
//
// A HUD is the last place a bug is noticed, because a wrong number still looks like a number.
// These check the things a screenshot cannot: that a bar clamps, that a livery never collides
// with a fixed colour, and that a banner appears only when the match is actually over.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/ui/Hud.hpp"

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

/// How far apart two colours are, ignoring alpha. Used to assert that a livery and a fixed
/// colour are TELLABLE APART, which is a stronger claim than being unequal.
[[nodiscard]] float distance(rm::ui::Colour a, rm::ui::Colour b) {
    const float dr = a[0] - b[0];
    const float dg = a[1] - b[1];
    const float db = a[2] - b[2];
    return std::sqrt(dr * dr + dg * dg + db * db);
}

} // namespace

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
    state.mass = Gauge{.stored = 120.0f, .capacity = 650.0f, .incomePerSecond = 2.5f};
    state.energy = Gauge{
        .stored = 144.0f, .capacity = 5000.0f, .incomePerSecond = 5.0f, .drainPerSecond = 2.0f};
    state.unitsAlive = 6;
    state.armiesLeft = 6;
    state.armiesTotal = 8;
    state.elapsedSeconds = 40.0f;

    rm::ui::Geometry out;
    rm::ui::build(out, font, font, rm::ui::neutralTheme(), state, 1400.0f, 900.0f);

    // Both faces contribute: the chrome and labels in one list, the numbers in the other.
    CHECK_FALSE(out.label.empty());
    CHECK_FALSE(out.readout.empty());

    // The chrome is REALLY there, not just the labels: a panel's glass, bevels and brackets are
    // more quads than the words "MASS" and "ENERGY" could account for on their own.
    const std::size_t labelGlyphs = std::string_view{"MASSENERGY"}.size();
    CHECK(out.label.size() > labelGlyphs * rm::text::kVerticesPerGlyph * 2);

    // Whole triangles, or the renderer's truncation would cut a quad in half.
    CHECK(out.label.size() % 3 == 0);
    CHECK(out.readout.size() % 3 == 0);
}

TEST_CASE("the banner appears only when the match is over") {
    const std::vector<rm::text::Glyph> glyphs = boxGlyphs();
    const rm::text::Font font = fontOver(glyphs);

    MatchState running;
    running.armiesLeft = 4;
    running.armiesTotal = 8;

    rm::ui::Geometry mid;
    rm::ui::build(mid, font, font, rm::ui::neutralTheme(), running, 1400.0f, 900.0f);

    MatchState won = running;
    won.armiesLeft = 1;
    won.outcome = MatchState::Outcome::Win;
    won.winningTeam = 6;

    rm::ui::Geometry over;
    rm::ui::build(over, font, font, rm::ui::neutralTheme(), won, 1400.0f, 900.0f);

    // The banner is the largest thing the interface ever draws, so it is unmistakable in the
    // vertex count. A draw and a win both produce one; a running match produces none.
    CHECK(over.label.size() > mid.label.size());

    MatchState drawn = won;
    drawn.armiesLeft = 0;
    drawn.outcome = MatchState::Outcome::Draw;
    rm::ui::Geometry drawnOut;
    rm::ui::build(drawnOut, font, font, rm::ui::neutralTheme(), drawn, 1400.0f, 900.0f);
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
    rm::ui::build(a, font, font, rm::ui::neutralTheme(), funded, 1400.0f, 900.0f);
    rm::ui::build(b, font, font, rm::ui::neutralTheme(), stalling, 1400.0f, 900.0f);

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
    rm::ui::build(noLabels, missing, good, rm::ui::neutralTheme(), state, 1400.0f, 900.0f);
    CHECK_FALSE(noLabels.empty());  // the readouts, and chrome drawn with the readout atlas

    rm::ui::Geometry noReadouts;
    rm::ui::build(noReadouts, good, missing, rm::ui::neutralTheme(), state, 1400.0f, 900.0f);
    CHECK_FALSE(noReadouts.label.empty());
    CHECK(noReadouts.readout.empty());

    // Neither face: nothing at all, rather than a crash.
    rm::ui::Geometry nothing;
    rm::ui::build(nothing, missing, missing, rm::ui::neutralTheme(), state, 1400.0f, 900.0f);
    CHECK(nothing.empty());
}
