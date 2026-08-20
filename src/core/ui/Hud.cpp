#include "core/ui/Hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rm::ui {
namespace {

/// Builds a livery from one accent colour.
///
/// Derived rather than hand-picked per faction, so all four are the same design in four hues
/// and none of them drifts. The glass is the accent taken almost to black and the well darker
/// still, which is what makes a bar's track read as cut INTO the panel rather than drawn on
/// it — the only depth cue a flat interface gets.
[[nodiscard]] Theme themeFrom(Colour accent) noexcept {
    return Theme{
        .glass = Colour{{accent[0] * 0.10f, accent[1] * 0.13f, accent[2] * 0.16f, 0.82f}},
        .well = Colour{{accent[0] * 0.05f, accent[1] * 0.07f, accent[2] * 0.09f, 0.90f}},
        .edge = Colour{{accent[0] * 0.45f, accent[1] * 0.48f, accent[2] * 0.52f, 0.85f}},
        .edgeLit = Colour{{accent[0], accent[1], accent[2], 0.95f}},
        .label = Colour{{accent[0] * 0.55f + 0.30f, accent[1] * 0.55f + 0.34f,
                         accent[2] * 0.55f + 0.36f, 1.0f}},
    };
}

} // namespace

Theme themeFor(sim::Faction faction) noexcept {
    switch (faction) {
        // Steel blue, and the only faction whose own colour needed no adjustment — nothing
        // fixed is blue.
        case sim::Faction::Uef:
            return themeFrom(Colour{{0.290f, 0.608f, 0.847f, 1.0f}});  // #4A9BD8

        // PALE MINT, not the leaf green the faction wears in the field: mass is green, and a
        // green frame around a green number makes the number disappear into it. Turquoise was
        // the first attempt and it was not far enough — 0.23 from mass, which the test rejects.
        // Taken up in value and down in saturation instead, so it reads as Aeon's green-white
        // rather than as a second green.
        case sim::Faction::Aeon:
            return themeFrom(Colour{{0.659f, 0.941f, 0.863f, 1.0f}});  // #A8F0DC

        // ROSE, not the faction's own orange-red: a LOSS is orange-red, and a Cybran player
        // would otherwise read their own chrome as a deficit. Crimson was the first attempt and
        // sat 0.15 away, which is not a difference an eye makes in peripheral vision. Rotated
        // toward magenta, which is still unmistakably the red faction and nothing else in the
        // interface is.
        case sim::Faction::Cybran:
            return themeFrom(Colour{{0.910f, 0.251f, 0.561f, 1.0f}});  // #E8408F

        // PALE CREAM, not amber: energy is amber. Taken most of the way to white so it still
        // reads as Seraphim's gold without being the energy bar's colour.
        case sim::Faction::Seraphim:
            return themeFrom(Colour{{0.937f, 0.890f, 0.690f, 1.0f}});  // #EFE3B0
    }
    return neutralTheme();
}

Theme neutralTheme() noexcept {
    // The cyan the interface would have had with no factions in it at all.
    return themeFrom(Colour{{0.412f, 0.812f, 0.902f, 1.0f}});  // #69CFE6
}

float Gauge::fill() const noexcept {
    if (capacity <= 0.0f) {
        return 0.0f;  // nowhere to put anything reads as EMPTY, not as full
    }
    return std::clamp(stored / capacity, 0.0f, 1.0f);
}

bool Gauge::wasting() const noexcept {
    return capacity > 0.0f && stored >= capacity * 0.999f && net() > 0.0f;
}

std::string formatAmount(float value) {
    char buffer[32];
    // Thousands as `12.4k`, because an energy store runs to five digits where a mass store
    // runs to three, and a column that changes width as the number grows is exactly the
    // jitter the monospaced face was chosen to avoid.
    if (std::abs(value) >= 10000.0f) {
        std::snprintf(buffer, sizeof(buffer), "%.1fk", static_cast<double>(value / 1000.0f));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f", static_cast<double>(value));
    }
    return buffer;
}

std::string formatRate(float value) {
    char buffer[32];
    // An explicit sign on everything but zero. A rate with no sign is ambiguous at a glance,
    // and "+0" reads as a claim about a thing that is not happening.
    if (std::abs(value) < 0.05f) {
        return "0";
    }
    const double magnitude = static_cast<double>(std::abs(value));
    const char* sign = value > 0.0f ? "+" : "-";

    // WHOLE NUMBERS FROM TEN UP, which is what the game itself shows — every rate in its own
    // interface is an integer. Below ten a decimal is kept, because the opening minutes run on a
    // commander's trickle of half a mass a second and "+0" would report nothing happening while
    // the first extractor is being paid for.
    if (magnitude >= 10.0) {
        std::snprintf(buffer, sizeof(buffer), "%s%.0f", sign, magnitude);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%s%.1f", sign, magnitude);
    }
    return buffer;
}

std::string formatClock(float seconds) {
    const int total = static_cast<int>(std::max(0.0f, seconds));
    char buffer[32];
    if (total >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60,
                      total % 60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
    }
    return buffer;
}

void appendPanel(Geometry& out, const text::Font& font, const Theme& theme, float x, float y,
                 float width, float height) {
    if (!font.usable() || width <= 0.0f || height <= 0.0f) {
        return;
    }

    text::appendRect(out.label, font, x, y, width, height, theme.glass);

    // A hairline all the way round, and a BRIGHTER one along the top. One light source,
    // implied from above, which is all a flat interface needs to stop looking like paper.
    text::appendRect(out.label, font, x, y, width, kBevel, theme.edgeLit);
    text::appendRect(out.label, font, x, y + height - kBevel, width, kBevel, theme.edge);
    text::appendRect(out.label, font, x, y, kBevel, height, theme.edge);
    text::appendRect(out.label, font, x + width - kBevel, y, kBevel, height, theme.edge);

    // The brackets: two strokes at each corner, in the lit colour. They mark the panel's
    // bounds without a full frame, so the chrome never competes with the numbers inside it.
    const float run = std::min(kBracket, std::min(width, height) * 0.4f);
    constexpr float kThick = kBevel * 2.0f;
    const std::array<std::array<float, 2>, 4> corners{
        {{x, y}, {x + width - run, y}, {x, y + height - kThick}, {x + width - run, y + height - kThick}}};
    for (const std::array<float, 2>& corner : corners) {
        text::appendRect(out.label, font, corner[0], corner[1], run, kThick, theme.edgeLit);
    }
    text::appendRect(out.label, font, x, y, kThick, run, theme.edgeLit);
    text::appendRect(out.label, font, x + width - kThick, y, kThick, run, theme.edgeLit);
    text::appendRect(out.label, font, x, y + height - run, kThick, run, theme.edgeLit);
    text::appendRect(out.label, font, x + width - kThick, y + height - run, kThick, run,
                     theme.edgeLit);
}

namespace {

/// One resource's row: a chip, a label, the numbers, a storage bar and a flow strip.
///
/// Returns the y below the row so the panel stacks rows without either knowing the other's
/// height.
[[nodiscard]] float appendResourceRow(Geometry& out, const text::Font& labelFont,
                                      const text::Font& readoutFont, const Theme& theme,
                                      const Gauge& gauge, std::string_view name, Colour tint,
                                      float x, float y, float width) {
    const text::Font& chrome = labelFont.usable() ? labelFont : readoutFont;

    // --- the chip -----------------------------------------------------------
    // Squared off and in the resource's own colour, sitting on the label's baseline. It is the
    // only place the resource's identity is stated as colour alone, which is how it will be
    // read once the panel is familiar.
    text::appendRect(out.label, chrome, x, y - kChip, kChip, kChip, tint);

    if (labelFont.usable()) {
        (void)text::appendText(out.label, labelFont.glyphs, name, x + kChip + kUnit, y,
                               theme.label);
    }

    // --- the numbers, on the label's line -----------------------------------
    //
    // The NET RATE is the largest thing in the row and sits hard right, because it is the one
    // figure a player tracks continuously and the eye should find it without reading across.
    // The store is smaller, in a fixed column so the two rows agree.
    if (readoutFont.usable()) {
        const std::string stored =
            formatAmount(gauge.stored) + " / " + formatAmount(gauge.capacity);
        (void)text::appendText(out.readout, readoutFont.glyphs, stored, x + kLabelColumn, y,
                               kInk);

        const std::string net = formatRate(gauge.net());
        constexpr float kNetScale = 1.3f;
        const float netWidth = text::measureText(readoutFont.glyphs, net, kNetScale);
        (void)text::appendText(out.readout, readoutFont.glyphs, net, x + width - netWidth, y,
                               gauge.net() >= 0.0f ? kGain : kLoss, kNetScale);
    }

    // --- the storage bar ----------------------------------------------------
    const float barTop = y + kUnit * 0.8f;
    text::appendRect(out.label, chrome, x, barTop, width, kGaugeHeight, theme.well);
    text::appendRect(out.label, chrome, x, barTop, width * gauge.fill(), kGaugeHeight, tint);

    // FULL AND STILL EARNING: a lit cap at the far end. The economy discards anything past
    // capacity, and this is the only place that fact is visible.
    if (gauge.wasting()) {
        text::appendRect(out.label, chrome, x + width - kUnit * 0.6f, barTop, kUnit * 0.6f,
                         kGaugeHeight, Colour{{1.0f, 1.0f, 1.0f, 0.92f}});
    }

    // --- the flow strip: the signature --------------------------------------
    //
    // Income grows from the left and drain from the right, meeting where the balance is. WHERE
    // THEY MEET is the reading: past the notch and you are earning, short of it and you are
    // spending down. No number has to be read for that, which is the point — the numbers are
    // for when the answer is "by how much".
    //
    // Separated from the bar above by a gap and drawn shorter, because the two are different
    // instruments and a strip flush against the bar reads as part of it. That mistake made the
    // first version of this panel unreadable: at an empty store the only coloured thing was the
    // flow strip, and every eye took it for the level.
    const float flowTop = barTop + kGaugeHeight + kFlowGap;
    text::appendRect(out.label, chrome, x, flowTop, width, kFlowHeight, theme.well);

    const float total = gauge.incomePerSecond + gauge.drainPerSecond;
    if (total > 0.0f) {
        const float inWidth = width * (gauge.incomePerSecond / total);
        text::appendRect(out.label, chrome, x, flowTop, inWidth, kFlowHeight, fade(kGain, 0.9f));
        text::appendRect(out.label, chrome, x + inWidth, flowTop, width - inWidth, kFlowHeight,
                         fade(kLoss, 0.9f));
    }

    // The balance notch, drawn whether or not anything is flowing: it is the reference the
    // strip is read against, and a reference that appears only when there is something to
    // compare it to is no reference at all.
    text::appendRect(out.label, chrome, x + width * 0.5f - kBevel, flowTop - kFlowGap,
                     kBevel * 2.0f, kFlowHeight + kFlowGap * 2.0f,
                     Colour{{1.0f, 1.0f, 1.0f, 0.6f}});

    return flowTop + kFlowHeight;
}

} // namespace

void build(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
           const Theme& theme, const MatchState& state, float viewportWidth,
           float viewportHeight) {
    (void)viewportHeight;

    const text::Font& chrome = labelFont.usable() ? labelFont : readoutFont;
    if (!chrome.usable()) {
        return;  // no font at all: the interface degrades to nothing rather than to a crash
    }

    const float lineHeight = std::max(chrome.lineHeight, 12.0f);

    // --- the resource panel, top left -------------------------------------
    const float panelX = kMargin;
    const float panelY = kMargin;
    // Measured rather than guessed, and it has to include the stall line: the first version
    // computed a height for two rows and then drew a third thing below them, which put the
    // warning across the panel's own bottom edge.
    const float rowHeight = kUnit * 0.8f + kGaugeHeight + kFlowGap + kFlowHeight;
    const float rowGap = kUnit * 2.2f;
    const bool stalling = state.fundedFraction < 0.995f;
    const float panelHeight = kPad * 2.0f + lineHeight * 0.8f + rowHeight * 2.0f + rowGap
                            + (stalling ? lineHeight : 0.0f);

    appendPanel(out, chrome, theme, panelX, panelY, kResourcePanelWidth, panelHeight);

    const float contentX = panelX + kPad;
    const float contentWidth = kResourcePanelWidth - kPad * 2.0f;

    // The pen sits on the BASELINE, so the first row starts a line down from the padding.
    float y = panelY + kPad + lineHeight * 0.8f;
    y = appendResourceRow(out, labelFont, readoutFont, theme, state.mass, "MASS", kMass,
                          contentX, y, contentWidth);
    y += rowGap;
    y = appendResourceRow(out, labelFont, readoutFont, theme, state.energy, "ENERGY", kEnergy,
                          contentX, y, contentWidth);

    // --- the throttle -----------------------------------------------------
    //
    // ONE reading, not two. The funded fraction is a fact about the ARMY — a stall slows every
    // construction by the same share — so it belongs on its own line rather than over each
    // resource bar. The first version drew it on both, and at 8% funded a band covering 92% of
    // a bar's height simply reads as a full bar of the wrong colour.
    //
    // Shown only while there IS a stall: a line that always says 100% is one a player stops
    // seeing, and then misses at 40%.
    if (stalling) {
        const float rowY = y + lineHeight * 0.85f;

        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "BUILDING AT %.0f%%",
                      static_cast<double>(state.fundedFraction * 100.0f));

        float penX = contentX;
        if (labelFont.usable()) {
            penX = text::appendText(out.label, labelFont.glyphs, buffer, contentX, rowY, kWarn);
        }

        // A short bar after the words, filled to the same fraction. The number says how much
        // and the bar says how bad, and the second is the one read at a glance.
        const float barX = penX + kUnit * 1.5f;
        const float barWidth = std::max(0.0f, contentX + contentWidth - barX);
        if (barWidth > kUnit) {
            const float barY = rowY - kGaugeHeight * 0.85f;
            text::appendRect(out.label, chrome, barX, barY, barWidth, kGaugeHeight * 0.7f,
                             theme.well);
            text::appendRect(out.label, chrome, barX, barY,
                             barWidth * std::clamp(state.fundedFraction, 0.0f, 1.0f),
                             kGaugeHeight * 0.7f, kWarn);
        }
    }

    // --- the match panel, top right ---------------------------------------
    //
    // Right-aligned, and measured rather than guessed: the clock and the army count change
    // width as they run, and text pinned by its left edge would walk sideways.
    if (readoutFont.usable()) {
        char armies[64];
        std::snprintf(armies, sizeof(armies), "%zu / %zu ARMIES", state.armiesLeft,
                      state.armiesTotal);
        char units[64];
        std::snprintf(units, sizeof(units), "%zu UNITS", state.unitsAlive);
        const std::string clock = formatClock(state.elapsedSeconds);

        const float widest = std::max({text::measureText(readoutFont.glyphs, clock, 1.25f),
                                       text::measureText(readoutFont.glyphs, armies),
                                       text::measureText(readoutFont.glyphs, units)});
        const float panelWidth = widest + kPad * 2.0f;
        const float rightX = viewportWidth - kMargin - panelWidth;
        const float height = kPad * 2.0f + lineHeight * 2.6f;

        appendPanel(out, chrome, theme, rightX, kMargin, panelWidth, height);

        const float right = rightX + panelWidth - kPad;
        float ry = kMargin + kPad + lineHeight * 0.8f;

        const float clockWidth = text::measureText(readoutFont.glyphs, clock, 1.25f);
        (void)text::appendText(out.readout, readoutFont.glyphs, clock, right - clockWidth, ry,
                               kInk, 1.25f);
        ry += lineHeight;
        (void)text::appendText(out.readout, readoutFont.glyphs, armies,
                               right - text::measureText(readoutFont.glyphs, armies), ry,
                               theme.label);
        ry += lineHeight * 0.9f;
        (void)text::appendText(out.readout, readoutFont.glyphs, units,
                               right - text::measureText(readoutFont.glyphs, units), ry,
                               theme.label);
    }

    // --- the outcome ------------------------------------------------------
    //
    // Centred and large, the one thing here that is an EVENT rather than a reading — so it is
    // the only element allowed to sit in the middle of the screen, where nothing else may go.
    if (state.outcome != MatchState::Outcome::Running && labelFont.usable()) {
        const std::string banner = state.outcome == MatchState::Outcome::Win
                                     ? ("TEAM " + std::to_string(state.winningTeam) + " WINS")
                                     : std::string{"A DRAW"};
        constexpr float kBannerScale = 3.0f;
        const float bannerWidth = text::measureText(labelFont.glyphs, banner, kBannerScale);
        const float bx = (viewportWidth - bannerWidth) * 0.5f;
        const float by = viewportHeight * 0.34f;

        // A ruled band behind it, full width of the text plus a margin, so the banner reads
        // over any terrain rather than only over dark ground.
        const float band = lineHeight * kBannerScale;
        text::appendRect(out.label, chrome, bx - kUnit * 4.0f, by - band * 0.85f,
                         bannerWidth + kUnit * 8.0f, band * 1.15f, fade(theme.glass, 1.1f));
        text::appendRect(out.label, chrome, bx - kUnit * 4.0f, by - band * 0.85f,
                         bannerWidth + kUnit * 8.0f, kBevel, theme.edgeLit);
        text::appendRect(out.label, chrome, bx - kUnit * 4.0f, by + band * 0.30f,
                         bannerWidth + kUnit * 8.0f, kBevel, theme.edgeLit);

        (void)text::appendText(out.label, labelFont.glyphs, banner, bx, by,
                               state.outcome == MatchState::Outcome::Win ? kWarn : kInk,
                               kBannerScale);
    }
}

} // namespace rm::ui
