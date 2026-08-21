#include "core/ui/BuildPanel.hpp"

#include <algorithm>
#include <cmath>

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

Colour tierTint(const Theme& theme, int tier) noexcept {
    // Toward white as the tier rises, never all the way: a fully white band would outshine the
    // lit edge and pull the eye to the tier rather than to the selection.
    const float lift = tier >= 3 ? 0.62f : (tier == 2 ? 0.34f : 0.0f);
    return Colour{{theme.edgeLit[0] + (1.0f - theme.edgeLit[0]) * lift,
                   theme.edgeLit[1] + (1.0f - theme.edgeLit[1]) * lift,
                   theme.edgeLit[2] + (1.0f - theme.edgeLit[2]) * lift, 1.0f}};
}

BuildPanelLayout buildPanelLayout(const MinimapLayout& minimap, std::size_t optionCount) noexcept {
    BuildPanelLayout layout;
    layout.rows = rowsFor(optionCount);
    if (layout.rows == 0) {
        return layout;  // nothing selected that builds: the panel is simply absent
    }

    const auto columns = static_cast<float>(kBuildColumns);
    const float gridWidth = columns * kBuildCell + (columns - 1.0f) * kBuildGap;
    const float gridHeight =
        static_cast<float>(layout.rows) * kBuildCell + static_cast<float>(layout.rows - 1) * kBuildGap;

    layout.width = gridWidth + kPad * 2.0f;
    layout.height = gridHeight + kBuildHeader + kPad * 2.0f;

    // Flush with the minimap's left edge and sitting on top of it, one unit clear. The two read
    // as one bottom-left block, which is the arrangement being copied.
    layout.x = minimap.x;
    layout.y = minimap.y - layout.height - kUnit;

    layout.gridX = layout.x + kPad;
    layout.gridY = layout.y + kPad + kBuildHeader;
    return layout;
}

std::array<float, 2> buildCellOrigin(const BuildPanelLayout& layout, std::size_t index) noexcept {
    const auto columns = static_cast<std::size_t>(kBuildColumns);
    const auto column = static_cast<float>(index % columns);
    const auto row = static_cast<float>(index / columns);
    return {{layout.gridX + column * (kBuildCell + kBuildGap),
             layout.gridY + row * (kBuildCell + kBuildGap)}};
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

    const float pitch = kBuildCell + kBuildGap;
    const auto column = static_cast<int>(std::floor(localX / pitch));
    const auto row = static_cast<int>(std::floor(localY / pitch));
    if (column < 0 || column >= kBuildColumns || row < 0 || row >= layout.rows) {
        return std::nullopt;
    }

    // THE GUTTER IS DEAD SPACE, not the nearest cell's. A click that lands between two buttons
    // is a miss, and treating it as a hit on whichever cell is closer is how a player ends up
    // queueing something they did not choose.
    if (localX - static_cast<float>(column) * pitch > kBuildCell
        || localY - static_cast<float>(row) * pitch > kBuildCell) {
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
                      std::string_view builderName) {
    if (layout.empty() || options.empty() || !labelFont.usable()) {
        return;
    }

    appendPanel(out, labelFont, theme, layout.x, layout.y, layout.width, layout.height);

    // The header: what is selected, so a grid of ids is attributable to a builder. Baseline
    // rather than top — `appendText` takes the pen's baseline, and the header strip is sized so
    // the cap height sits centred in it.
    const float headerBaseline = layout.y + kPad + kBuildHeader * 0.7f;
    (void)text::appendText(out.label, labelFont.glyphs, builderName, layout.x + kPad,
                           headerBaseline, theme.label);

    for (std::size_t index = 0; index < options.size(); ++index) {
        const BuildOption& option = options[index];
        const std::array<float, 2> origin = buildCellOrigin(layout, index);
        const float cx = origin[0];
        const float cy = origin[1];

        // UNAFFORDABLE DIMS, it does not vanish — see the note on `BuildOption::affordable`.
        // Everything in the cell fades together so the cell reads as one disabled object rather
        // than as a lit frame around grey contents.
        const float alpha = option.affordable ? 1.0f : 0.38f;

        text::appendRect(out.label, labelFont, cx, cy, kBuildCell, kBuildCell,
                         fade(theme.well, alpha));

        // The tier band across the top. Two pixels: enough to group a row at a glance, not
        // enough to become the cell's dominant feature.
        text::appendRect(out.label, labelFont, cx, cy, kBuildCell, kBevel * 2.0f,
                         fade(option.tint, alpha));

        // The icon square, reserved and drawn as a recess. This is the hole a real unit icon
        // drops into once there is an atlas for one; until then it gives the cell a centre of
        // gravity so the id is not floating in a plain box.
        const float iconX = cx + (kBuildCell - kBuildIcon) * 0.5f;
        const float iconY = cy + kUnit * 0.9f;
        text::appendRect(out.label, labelFont, iconX, iconY, kBuildIcon, kBuildIcon,
                         fade(theme.glass, alpha * 0.9f));

        // TWO LINES UNDER THE ICON, id then cost, rather than the id centred and the cost
        // tucked into the same band. They collided at the old cell size and would collide again
        // for any long id; stacking them means the cell grows in one direction only.
        //
        // The id is CLIPPED TO THE CELL rather than allowed to overhang: an id wider than its
        // button is a content surprise (a mod's long name), and bleeding into the neighbouring
        // cell makes two buttons unreadable instead of one.
        const float idWidth = text::measureText(labelFont.glyphs, option.id);
        if (idWidth <= kBuildCell - kUnit) {
            (void)text::appendText(out.label, labelFont.glyphs, option.id,
                                   cx + (kBuildCell - idWidth) * 0.5f,
                                   cy + kBuildCell - kUnit * 2.6f, fade(theme.label, alpha));
        }

        // The mass cost, on the face of the button — its own line, centred, in the mass colour,
        // so the number a player compares is never behind a hover.
        if (readoutFont.usable()) {
            const std::string cost = formatAmount(option.massCost);
            const float costWidth = text::measureText(readoutFont.glyphs, cost);
            (void)text::appendText(out.readout, readoutFont.glyphs, cost,
                                   cx + (kBuildCell - costWidth) * 0.5f,
                                   cy + kBuildCell - kUnit * 0.8f,
                                   fade(option.affordable ? kMass : kLoss, alpha));
        }

        // The hovered cell gets a full lit border, which is BAR's own "this one" and reads
        // instantly against the hairline every other cell carries.
        const bool lit = hovered.has_value() && *hovered == index;
        const Colour border = lit ? theme.edgeLit : fade(theme.edge, alpha);
        const float thickness = lit ? kBevel * 2.0f : kBevel;
        text::appendRect(out.label, labelFont, cx, cy, kBuildCell, thickness, border);
        text::appendRect(out.label, labelFont, cx, cy + kBuildCell - thickness, kBuildCell,
                         thickness, border);
        text::appendRect(out.label, labelFont, cx, cy, thickness, kBuildCell, border);
        text::appendRect(out.label, labelFont, cx + kBuildCell - thickness, cy, thickness,
                         kBuildCell, border);
    }
}

} // namespace rm::ui
