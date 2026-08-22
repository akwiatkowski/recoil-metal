#include "core/ui/BuildPanel.hpp"

#include "core/ui/IconAtlas.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace rm::ui {
namespace {

/// Rows needed for `count` options at `kBuildColumns` across, rounding up.
[[nodiscard]] int rowsFor(std::size_t count) noexcept {
    if (count == 0) {
        return 0;
    }
    const auto columns = static_cast<std::size_t>(kBuildColumns);
    return static_cast<int>((count + columns - 1) / columns);
}

} // namespace

InfoCard buildOptionCard(const BuildOption& option) {
    InfoCard card;
    card.title = option.name.empty() ? option.id : option.name;
    // The corner repeats nothing: when the title IS the id there is no second fact to state.
    if (!option.name.empty()) {
        card.corner = option.id;
    }

    // Mass is always stated — a build decision is a mass decision first — and in the loss
    // colour when the store cannot pay it, which is the card agreeing with the dimmed cell.
    card.rows.push_back(InfoRow{.label = "MASS",
                                .value = formatAmount(option.massCost),
                                .tint = option.affordable ? kMass : kLoss});
    if (option.energyCost > 0.0f) {
        card.rows.push_back(InfoRow{
            .label = "ENERGY", .value = formatAmount(option.energyCost), .tint = kEnergy});
    }
    if (option.buildSeconds > 0.0f) {
        card.rows.push_back(
            InfoRow{.label = "BUILD TIME", .value = formatClock(option.buildSeconds)});
    }
    if (option.health > 0.0f) {
        card.rows.push_back(InfoRow{.label = "HEALTH", .value = formatAmount(option.health)});
    }
    return card;
}

Colour tierTint(const Theme& theme, int tier) noexcept {
    // Toward white as the tier rises, never all the way: a fully white band would outshine the
    // lit edge and pull the eye to the tier rather than to the selection.
    const float lift = tier >= 3 ? 0.62f : (tier == 2 ? 0.34f : 0.0f);
    return Colour{{theme.edgeLit[0] + (1.0f - theme.edgeLit[0]) * lift,
                   theme.edgeLit[1] + (1.0f - theme.edgeLit[1]) * lift,
                   theme.edgeLit[2] + (1.0f - theme.edgeLit[2]) * lift, 1.0f}};
}

std::array<std::string_view, 2> wrapCellName(std::span<const text::Glyph> glyphs,
                                             std::string_view name, float maxWidth) noexcept {
    std::array<std::string_view, 2> lines{};

    // Truncates one overlong word by characters until it fits. Measured per prefix rather
    // than estimated per glyph, because the widths differ per character and "roughly fits"
    // here means "runs into the neighbouring cell".
    const auto fitted = [&](std::string_view word) {
        while (word.size() > 1 && text::measureText(glyphs, word) > maxWidth) {
            word.remove_suffix(1);
        }
        return word;
    };

    std::size_t lineStart = 0;   // where the current line begins in `name`
    std::size_t lineEnd = 0;     // one past the last word taken into it
    std::size_t line = 0;
    std::size_t cursor = 0;
    while (cursor < name.size() && line < lines.size()) {
        const std::size_t wordEnd = std::min(name.find(' ', cursor), name.size());
        const std::string_view candidate = name.substr(lineStart, wordEnd - lineStart);
        if (lineEnd == lineStart || text::measureText(glyphs, candidate) <= maxWidth) {
            // The first word always joins — an empty line helps nobody — and any further
            // word joins while the line still fits.
            lineEnd = wordEnd;
        } else {
            lines[line++] = fitted(name.substr(lineStart, lineEnd - lineStart));
            lineStart = cursor;
            lineEnd = wordEnd;
        }
        cursor = wordEnd + 1;
    }
    if (line < lines.size() && lineEnd > lineStart) {
        lines[line] = fitted(name.substr(lineStart, lineEnd - lineStart));
    }
    return lines;
}

BuildPanelLayout buildPanelLayout(const MinimapLayout& minimap, std::size_t optionCount) noexcept {
    BuildPanelLayout layout;
    layout.rows = rowsFor(optionCount);
    if (layout.rows == 0) {
        return layout;  // nothing selected that builds: the panel is simply absent
    }

    const auto columns = static_cast<float>(kBuildColumns);
    const float gridWidth = columns * kBuildCell + (columns - 1.0f) * kBuildGap;
    const float gridHeight = static_cast<float>(layout.rows) * kBuildCellHeight
                             + static_cast<float>(layout.rows - 1) * kBuildGap;

    layout.width = gridWidth + kPad * 2.0f;
    layout.height = gridHeight + kBuildHeader + kPad * 2.0f;

    // DOCKED: flush with the minimap's left edge and sitting directly on its top edge, so the
    // panel's bottom hairline and the minimap's lit top edge form one shared rail. The first
    // version floated one unit clear, and the gap read as two unrelated panels that happened to
    // be stacked — the arrangement being copied (BAR's bottom-left cluster) is ONE control
    // block, and the dock is what makes it one.
    layout.x = minimap.x;
    layout.y = minimap.y - layout.height;

    layout.gridX = layout.x + kPad;
    layout.gridY = layout.y + kPad + kBuildHeader;
    return layout;
}

bool insideBuildPanel(const BuildPanelLayout& layout, float pointX, float pointY) noexcept {
    if (layout.empty()) {
        return false;  // no panel is drawn, so nothing can be on it
    }
    return pointX >= layout.x && pointX < layout.x + layout.width && pointY >= layout.y
           && pointY < layout.y + layout.height;
}

std::array<float, 2> buildCellOrigin(const BuildPanelLayout& layout, std::size_t index) noexcept {
    const auto columns = static_cast<std::size_t>(kBuildColumns);
    const auto column = static_cast<float>(index % columns);
    const auto row = static_cast<float>(index / columns);
    return {{layout.gridX + column * (kBuildCell + kBuildGap),
             layout.gridY + row * (kBuildCellHeight + kBuildGap)}};
}

std::optional<std::size_t> buildOptionAt(const BuildPanelLayout& layout, std::size_t optionCount,
                                         float pointX, float pointY) noexcept {
    if (layout.empty() || optionCount == 0) {
        return std::nullopt;
    }
    const float localX = pointX - layout.gridX;
    const float localY = pointY - layout.gridY;
    if (localX < 0.0f || localY < 0.0f) {
        return std::nullopt;
    }

    const float pitchX = kBuildCell + kBuildGap;
    const float pitchY = kBuildCellHeight + kBuildGap;
    const auto column = static_cast<int>(std::floor(localX / pitchX));
    const auto row = static_cast<int>(std::floor(localY / pitchY));
    if (column < 0 || column >= kBuildColumns || row < 0 || row >= layout.rows) {
        return std::nullopt;
    }

    // THE GUTTER IS DEAD SPACE, not the nearest cell's. A click that lands between two buttons
    // is a miss, and treating it as a hit on whichever cell is closer is how a player ends up
    // queueing something they did not choose.
    if (localX - static_cast<float>(column) * pitchX > kBuildCell
        || localY - static_cast<float>(row) * pitchY > kBuildCellHeight) {
        return std::nullopt;
    }

    const auto index =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(kBuildColumns)
        + static_cast<std::size_t>(column);
    // The last row is usually short. A point in one of its empty trailing cells is inside the
    // grid and over nothing.
    return index < optionCount ? std::optional<std::size_t>{index} : std::nullopt;
}

void appendBuildPanel(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
                      const Theme& theme, const BuildPanelLayout& layout,
                      std::span<const BuildOption> options, std::optional<std::size_t> hovered,
                      std::string_view builderName, std::string_view builderRole) {
    if (layout.empty() || options.empty() || !labelFont.usable()) {
        return;
    }

    appendPanel(out, labelFont, theme, layout.x, layout.y, layout.width, layout.height);

    // The header: what is selected, so a grid of ids is attributable to a builder. Baseline
    // rather than top — `appendText` takes the pen's baseline, and the header strip is sized so
    // the cap height sits centred in it.
    //
    // THE ROLE LEADS AND THE ID FOLLOWS, because they answer different questions at different
    // moments. "COMMANDER" is what a player glances at to confirm whose menu this is — a word
    // they think in. `UEL0001` is how they name the unit to somebody else, and it earns a
    // quieter seat on the right, in the readout face, the way every other identifier-shaped
    // fact in this interface is set. The first version put the raw id where the word belongs,
    // which made the panel's one line of prose read as a part number.
    const float headerBaseline = layout.y + kPad + kBuildHeader * 0.7f;
    if (!builderRole.empty()) {
        // The silkscreen face is caps-only in spirit; the role arrives lowercase from
        // `roleName` and is lifted here, where it becomes a label.
        std::string role{builderRole};
        std::transform(role.begin(), role.end(), role.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        (void)text::appendText(out.label, labelFont.glyphs, role, layout.x + kPad,
                               headerBaseline, theme.label);
        if (readoutFont.usable()) {
            const float idWidth = text::measureText(readoutFont.glyphs, builderName);
            (void)text::appendText(out.readout, readoutFont.glyphs, builderName,
                                   layout.x + layout.width - kPad - idWidth, headerBaseline,
                                   fade(kInk, 0.7f));
        }
    } else {
        // No role stated: the id keeps the lead seat rather than the line going blank.
        (void)text::appendText(out.label, labelFont.glyphs, builderName, layout.x + kPad,
                               headerBaseline, theme.label);
    }

    // A rule under the header, the full content width. The same device the resource panel's
    // bevel is: it says "the prose ends here, the instrument begins" without a second panel.
    text::appendRect(out.label, labelFont, layout.x + kPad, layout.y + kPad + kBuildHeader - kBevel,
                     layout.width - kPad * 2.0f, kBevel, fade(theme.edge, 0.9f));

    for (std::size_t index = 0; index < options.size(); ++index) {
        const BuildOption& option = options[index];
        const std::array<float, 2> origin = buildCellOrigin(layout, index);
        const float cx = origin[0];
        const float cy = origin[1];

        // UNAFFORDABLE DIMS, it does not vanish — see the note on `BuildOption::affordable`.
        // Everything in the cell fades together so the cell reads as one disabled object rather
        // than as a lit frame around grey contents.
        const float alpha = option.affordable ? 1.0f : 0.38f;

        // The well in gradient glass, like its panel — one light for chrome and cells alike.
        const Colour well = fade(theme.well, alpha);
        text::appendRectV(out.label, labelFont, cx, cy, kBuildCell, kBuildCellHeight,
                          Colour{{well[0] * 1.5f, well[1] * 1.5f, well[2] * 1.5f, well[3]}},
                          Colour{{well[0] * 0.7f, well[1] * 0.7f, well[2] * 0.7f, well[3]}});

        // The hovered cell's fill lifts as well as its border brightening below: a button
        // under the cursor should look pressed toward the light, not merely outlined.
        if (hovered.has_value() && *hovered == index) {
            text::appendRect(out.label, labelFont, cx, cy, kBuildCell, kBuildCellHeight,
                             fade(theme.edgeLit, 0.10f));
        }

        // The tier band across the top. Three pixels, up from two: at two the band vanished
        // into the cell border on a Retina capture and the tier grouping it exists for was
        // invisible at a glance. Three is still a trim, not a feature.
        text::appendRect(out.label, labelFont, cx, cy, kBuildCell, kBevel * 3.0f,
                         fade(option.tint, alpha));

        // The icon square: a recess, and the game's own icon in it when the archives have one.
        //
        // THE RECESS IS DRAWN EITHER WAY, under the icon. An icon is mostly transparent — a
        // silhouette on nothing — so without a well behind it the shape floats on the cell's
        // own fill and loses its edges against a light tint band.
        const float iconX = cx + (kBuildCell - kBuildIcon) * 0.5f;
        const float iconY = cy + kUnit * 1.1f;
        text::appendRect(out.label, labelFont, iconX, iconY, kBuildIcon, kBuildIcon,
                         fade(theme.glass, alpha * 0.9f));

        // The icon itself goes in the IMAGE list, which is drawn after both font atlases — see
        // `Geometry::image`. Six vertices, uv'd into the shared atlas, tinted white so the
        // artwork arrives as authored and faded with the rest of an unaffordable cell.
        if (option.iconSlot) {
            const IconUv uv = iconUv(*option.iconSlot);
            const Colour tint{{1.0f, 1.0f, 1.0f, alpha}};
            const float x1 = iconX + kBuildIcon;
            const float y1 = iconY + kBuildIcon;
            out.image.push_back({{iconX, iconY}, {uv.u0, uv.v0}, tint});
            out.image.push_back({{x1, iconY}, {uv.u1, uv.v0}, tint});
            out.image.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.image.push_back({{iconX, iconY}, {uv.u0, uv.v0}, tint});
            out.image.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.image.push_back({{iconX, y1}, {uv.u0, uv.v1}, tint});
        }

        // THE NAME ON THE FACE, up to two lines, then the cost. The id moved to the hover
        // card: a player scanning a menu reads "Mass Extractor", and `UEB1103` is for naming
        // the thing to somebody else. A nameless blueprint (one of 568, plus everything BAR
        // until its language files are read) falls back to the id, which is what every cell
        // showed before names existed — clipped to the cell, never overhanging a neighbour.
        const std::array<std::string_view, 2> lines =
            wrapCellName(labelFont.glyphs, option.name.empty() ? option.id : option.name,
                         kBuildCell - kUnit);
        const float lineOne = cy + kBuildCellHeight - kUnit * 5.6f;
        const float lineTwo = cy + kBuildCellHeight - kUnit * 3.6f;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].empty()) {
                continue;
            }
            const float width = text::measureText(labelFont.glyphs, lines[i]);
            (void)text::appendText(out.label, labelFont.glyphs, lines[i],
                                   cx + (kBuildCell - width) * 0.5f,
                                   i == 0 ? lineOne : lineTwo, fade(theme.label, alpha));
        }

        // THE MASS COST, on the face of the button, with a chip of the resource's colour —
        // the device the resource panel introduced, so "small green square" already means
        // mass by the time a player reads a cell. A bare green number was the first version,
        // and it asked the reader to know the colour code before the menu made sense.
        //
        // MASS ALONE, deliberately. A draft put energy beside it and the pair fit only the
        // cheap cells — half the tray showed two numbers and half showed one, which reads as
        // a bug rather than a rule. Mass is the deciding number and the affordability
        // signal; the hover card always states both.
        if (readoutFont.usable()) {
            constexpr float kCostChip = kUnit;  ///< smaller than the panel's kChip: it is a
                                                ///< footnote here, not a row heading
            const float costBaseline = cy + kBuildCellHeight - kUnit * 0.8f;
            const std::string mass = formatAmount(option.massCost);
            const float massWidth = text::measureText(readoutFont.glyphs, mass);
            const float gap = kUnit * 0.5f;
            const Colour tint = fade(option.affordable ? kMass : kLoss, alpha);

            float pen = cx + (kBuildCell - (kCostChip + gap + massWidth)) * 0.5f;
            text::appendRect(out.readout, readoutFont, pen, costBaseline - kCostChip,
                             kCostChip, kCostChip, tint);
            (void)text::appendText(out.readout, readoutFont.glyphs, mass, pen + kCostChip + gap,
                                   costBaseline, tint);
        }

        // The hovered cell gets a full lit border, which is BAR's own "this one" and reads
        // instantly against the hairline every other cell carries.
        const bool lit = hovered.has_value() && *hovered == index;
        const Colour border = lit ? theme.edgeLit : fade(theme.edge, alpha);
        const float thickness = lit ? kBevel * 2.0f : kBevel;
        text::appendRect(out.label, labelFont, cx, cy, kBuildCell, thickness, border);
        text::appendRect(out.label, labelFont, cx, cy + kBuildCellHeight - thickness,
                         kBuildCell, thickness, border);
        text::appendRect(out.label, labelFont, cx, cy, thickness, kBuildCellHeight, border);
        text::appendRect(out.label, labelFont, cx + kBuildCell - thickness, cy, thickness,
                         kBuildCellHeight, border);
    }
}

} // namespace rm::ui
