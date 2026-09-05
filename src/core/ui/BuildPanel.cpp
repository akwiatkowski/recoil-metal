#include "core/ui/BuildPanel.hpp"

#include "core/ui/IconAtlas.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace rm::ui {
BuildOptionAction buildOptionAction(const BuildOption& option,
                                    std::string_view builderRole) noexcept {
    return option.upgrade || builderRole == "factory"
        ? BuildOptionAction::SubmitAtBuilder
        : BuildOptionAction::ArmPlacement;
}

InfoCard buildOptionCard(const BuildOption& option, GameProfile profile) {
    InfoCard card;
    card.title = option.name.empty() ? option.id : option.name;
    // The card is where an upgrade gets to say what it does in words: the cell's frame says
    // "this one is different" and only this can say how.
    if (option.upgrade) {
        card.rows.push_back(InfoRow{.label = "UPGRADE", .value = "replaces this building"});
    }
    // The corner repeats nothing: when the title IS the id there is no second fact to state.
    // And with ids hidden it states nothing at all — a blueprint id is a filename, useful in a
    // log and in a bug report and never to the player deciding what to build.
    if (kShowBlueprintIds && !option.name.empty()) {
        card.corner = option.id;
    }

    // The construction material is always stated first, named as this profile names it, and in
    // the loss colour when the store cannot cover it — advisory, not a disabled state.
    card.rows.push_back(InfoRow{.label = std::string{resourceViews(profile, {}, {})[0].name},
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

BuildPanelLayout buildPanelLayout(const FrameLayout& frame, std::size_t optionCount,
                                  std::size_t page) noexcept {
    BuildPanelLayout layout;
    if (optionCount == 0 || frame.buildColumns == 0) {
        return layout;  // nothing selected that builds: the panel is simply absent
    }

    // Never make a cell narrower than it is tall. Fewer columns paginate predictably; keeping
    // the authored count in a collapsed palette would produce negative pitch and reversed quads.
    const float usableWidth = frame.build.width - kBuildPadding * 2.0f;
    const int fittingColumns = static_cast<int>(
        std::floor((usableWidth + kBuildGap) / (kBuildCellHeight + kBuildGap)));
    layout.columns = std::min(static_cast<int>(frame.buildColumns), fittingColumns);
    if (layout.columns <= 0) {
        return layout;
    }

    layout.x = frame.build.x;
    layout.y = frame.build.y;
    layout.width = frame.build.width;
    layout.height = frame.build.height;
    const std::size_t capacity =
        static_cast<std::size_t>(layout.columns) * static_cast<std::size_t>(kBuildRows);
    layout.pages = (optionCount + capacity - 1) / capacity;
    layout.page = std::min(page, layout.pages - 1);
    layout.first = layout.page * capacity;
    layout.shown = std::min(capacity, optionCount - layout.first);
    layout.gridX = layout.x + kBuildPadding;
    layout.gridY = layout.y + kBuildPadding + kBuildHeader;
    const float gaps = static_cast<float>(layout.columns - 1) * kBuildGap;
    layout.cellWidth = (layout.width - kBuildPadding * 2.0f - gaps)
                     / static_cast<float>(layout.columns);
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
    const auto columns = static_cast<std::size_t>(layout.columns);
    const auto column = static_cast<float>(index % columns);
    const auto row = static_cast<float>(index / columns);
    return {{layout.gridX + column * (layout.cellWidth + kBuildGap),
             layout.gridY + row * (layout.cellHeight + kBuildGap)}};
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

    const float pitchX = layout.cellWidth + kBuildGap;
    const float pitchY = layout.cellHeight + kBuildGap;
    const auto column = static_cast<int>(std::floor(localX / pitchX));
    const auto row = static_cast<int>(std::floor(localY / pitchY));
    if (column < 0 || column >= layout.columns || row < 0 || row >= layout.rows) {
        return std::nullopt;
    }

    // THE GUTTER IS DEAD SPACE, not the nearest cell's. A click that lands between two buttons
    // is a miss, and treating it as a hit on whichever cell is closer is how a player ends up
    // queueing something they did not choose.
    if (localX - static_cast<float>(column) * pitchX > layout.cellWidth
        || localY - static_cast<float>(row) * pitchY > layout.cellHeight) {
        return std::nullopt;
    }

    const auto local = static_cast<std::size_t>(row)
                     * static_cast<std::size_t>(layout.columns)
                     + static_cast<std::size_t>(column);
    // The last row is usually short. A point in one of its empty trailing cells is inside the
    // grid and over nothing.
    const std::size_t index = layout.first + local;
    return local < layout.shown && index < optionCount
               ? std::optional<std::size_t>{index}
               : std::nullopt;
}

std::optional<int> buildPageStepAt(const BuildPanelLayout& layout, float pointX,
                                   float pointY) noexcept {
    if (layout.pages <= 1 || pointY < layout.y || pointY >= layout.gridY) {
        return std::nullopt;
    }
    constexpr float kArrowWidth = 22.0f;
    const float right = layout.x + layout.width - kBuildPadding;
    if (pointX >= right - kArrowWidth && pointX < right) {
        return 1;
    }
    if (pointX >= right - kArrowWidth * 2.0f && pointX < right - kArrowWidth) {
        return -1;
    }
    return std::nullopt;
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
    const float headerBaseline = layout.y + kBuildPadding + kBuildHeader * 0.7f;
    if (!builderRole.empty()) {
        // The silkscreen face is caps-only in spirit; the role arrives lowercase from
        // `roleName` and is lifted here, where it becomes a label.
        std::string role{builderRole};
        std::transform(role.begin(), role.end(), role.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        (void)text::appendText(out.label, labelFont.glyphs, role, layout.x + kBuildPadding,
                               headerBaseline, theme.label);
        // The id used to sit here, right-aligned in the readout face, on the argument that it
        // is how a player names the unit to somebody else. That is true of a forum post and
        // false of the screen: `UEB0101` is on the panel of the thing already selected and
        // named, so it was a part number repeated beside the word it stands for.
        if (kShowBlueprintIds && readoutFont.usable()) {
            const float idWidth = text::measureText(readoutFont.glyphs, builderName);
            const float idRight = layout.x + layout.width - kBuildPadding
                                - (layout.pages > 1 ? 76.0f : 0.0f);
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, builderName,
                                   idRight - idWidth, headerBaseline, fade(kInk, 0.7f));
        }
    } else {
        // No role stated: the id keeps the lead seat rather than the line going blank.
        (void)text::appendText(out.label, labelFont.glyphs, builderName,
                               layout.x + kBuildPadding,
                               headerBaseline, theme.label);
    }

    if (layout.pages > 1 && readoutFont.usable()) {
        const std::string page = std::to_string(layout.page + 1) + "/"
                               + std::to_string(layout.pages) + "  <  >";
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, page,
                               layout.x + layout.width - kBuildPadding
                                   - text::measureText(readoutFont.glyphs, page),
                               headerBaseline, kInk);
    }

    // A rule under the header, the full content width. The same device the resource panel's
    // bevel is: it says "the prose ends here, the instrument begins" without a second panel.
    text::appendRect(out.chrome, labelFont, layout.x + kBuildPadding,
                     layout.y + kBuildPadding + kBuildHeader - kBevel,
                     layout.width - kBuildPadding * 2.0f, kBevel, fade(theme.edge, 0.9f));

    for (std::size_t local = 0; local < layout.shown; ++local) {
        const std::size_t index = layout.first + local;
        if (index >= options.size()) {
            break;
        }
        const BuildOption& option = options[index];
        const std::array<float, 2> origin = buildCellOrigin(layout, local);
        const float cx = origin[0];
        const float cy = origin[1];

        // A resource shortfall changes the cost colour below, never the cell's availability.
        // Construction is flow-funded and may start before the full price is stored.
        constexpr float alpha = 1.0f;

        // The well in gradient glass, like its panel — one light for chrome and cells alike.
        const Colour well = fade(theme.well, alpha);
        text::appendRectV(out.chrome, labelFont, cx, cy, layout.cellWidth, layout.cellHeight,
                          Colour{{well[0] * 1.5f, well[1] * 1.5f, well[2] * 1.5f, well[3]}},
                          Colour{{well[0] * 0.7f, well[1] * 0.7f, well[2] * 0.7f, well[3]}});

        // The hovered cell's fill lifts as well as its border brightening below: a button
        // under the cursor should look pressed toward the light, not merely outlined.
        if (hovered.has_value() && *hovered == index) {
            text::appendRect(out.chrome, labelFont, cx, cy, layout.cellWidth, layout.cellHeight,
                             fade(theme.edgeLit, 0.10f));
        }

        // The tier band across the top. Three authored points, up from two: at two it vanished
        // into the cell border on a Retina capture and the tier grouping it exists for was
        // invisible at a glance. Three is still a trim, not a feature.
        //
        // AN UPGRADE WEARS THE BAND ON ALL FOUR SIDES, at the lit edge colour. It is not
        // another thing to build beside the others — it CONSUMES the selected building and
        // replaces it — so a cell that looked like its neighbours would be a click a player
        // could not take back. A frame is the loudest thing this cell vocabulary has that is
        // still part of the vocabulary.
        const float bandWeight = kBevel * 3.0f;
        text::appendRect(out.chrome, labelFont, cx, cy, layout.cellWidth, bandWeight,
                          fade(option.tint, alpha));
        if (option.upgrade) {
            const Colour band = fade(theme.edgeLit, alpha);
            text::appendRect(out.chrome, labelFont, cx, cy + layout.cellHeight - bandWeight,
                             layout.cellWidth, bandWeight, band);
            text::appendRect(out.chrome, labelFont, cx, cy, bandWeight, layout.cellHeight, band);
            text::appendRect(out.chrome, labelFont, cx + layout.cellWidth - bandWeight, cy,
                             bandWeight, layout.cellHeight, band);
        }

        // The icon square: a recess, and the game's own icon in it when the archives have one.
        //
        // THE RECESS IS DRAWN EITHER WAY, under the icon. An icon is mostly transparent — a
        // silhouette on nothing — so without a well behind it the shape floats on the cell's
        // own fill and loses its edges against a light tint band.
        //
        // SMALLER THAN IT WAS (34, from 38) to make room for the NAME beneath it. A picture is
        // how a player who already knows the tray finds a thing; a name is how a player
        // learning it does, and the cell used to carry only the first.
        const float iconSize = std::min(30.0f, layout.cellWidth - 12.0f);
        const float iconX = cx + (layout.cellWidth - iconSize) * 0.5f;
        const float iconY = cy + 5.0f;
        text::appendRect(out.chrome, labelFont, iconX, iconY, iconSize, iconSize,
                          fade(theme.glass, alpha * 0.9f));

        // The icon itself goes in the semantic icon layer, below type but above panel chrome.
        // Six vertices, uv'd into the shared atlas, tinted white so the artwork arrives as
        // authored. A short bank changes only the mass-cost colour; the action stays enabled.
        if (option.iconSlot) {
            const IconUv uv = iconUv(*option.iconSlot);
            const Colour tint{{1.0f, 1.0f, 1.0f, alpha}};
            const float x1 = iconX + iconSize;
            const float y1 = iconY + iconSize;
            out.icon.push_back({{iconX, iconY}, {uv.u0, uv.v0}, tint});
            out.icon.push_back({{x1, iconY}, {uv.u1, uv.v0}, tint});
            out.icon.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.icon.push_back({{iconX, iconY}, {uv.u0, uv.v0}, tint});
            out.icon.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.icon.push_back({{iconX, y1}, {uv.u0, uv.v1}, tint});
        }

        // THE NAME, under the icon, wrapped to two short lines.
        //
        // THIS IS WHAT THE CELL EXISTS TO SAY. The face carried a picture and a mass figure
        // and no name at all, which meant a factory's tray was six silhouettes and six numbers
        // — findable by muscle memory and by nothing else. A player looking for the engineer
        // had to hover each cell in turn to be told which was which.
        //
        // The blueprint id is the fallback and only the fallback: content that states no
        // display name leaves nothing else to print, and an unlabelled cell is worse than a
        // part number (see `kShowBlueprintIds`).
        // IN THE LABEL FACE, not the readout one. The readout is monospaced — it is set that
        // way so a changing number does not jitter its column — and a monospaced face is the
        // worst possible choice for a proper noun in a narrow cell: "Land Scout" costs ten full
        // advances where the condensed label face sets it in about six.
        const std::string& label = option.name.empty() ? option.id : option.name;
        if (labelFont.usable() && !label.empty()) {
            const float textWidth = layout.cellWidth - 6.0f;
            // TIGHTER THAN THE FACE'S OWN LEADING. A font's line height is measured for
            // paragraphs; two lines of a proper noun set at it leave a gap wide enough for the
            // pair to stop reading as one name, and the second line then collides with the
            // icon above. Four fifths is the label's cap height plus a hair.
            const float lineHeight =
                (labelFont.lineHeight > 0.0f ? labelFont.lineHeight : kUnit * 1.8f) * 0.80f;
            const std::vector<std::string> lines =
                wrapToWidth(labelFont.glyphs, label, textWidth, kBuildNameLines);
            // Bottom-anchored, so a one-line name and a two-line name share their last
            // baseline and the grid keeps a common horizon rather than each cell floating its
            // own text.
            const float lastBaseline =
                cy + layout.cellHeight - (kShowCostOnCell ? kBuildCostStrip : 0.0f) - 5.0f;
            for (std::size_t line = 0; line < lines.size(); ++line) {
                const float baseline =
                    lastBaseline - static_cast<float>(lines.size() - 1 - line) * lineHeight;
                const float width = text::measureText(labelFont.glyphs, lines[line]);
                (void)text::appendText(out.label, labelFont.glyphs, lines[line],
                                       cx + (layout.cellWidth - width) * 0.5f, baseline,
                                       fade(theme.label, alpha));
            }
        }

        // THE COSTS, when the face is asked to carry them — mass, energy and time as a SET.
        //
        // The chip of the resource's colour is the device the resource panel introduced, so
        // "small green square" already means mass by the time a player reads a cell. Off by
        // default: see `kShowCostOnCell` for why the name won the space instead, and why it is
        // all three or none rather than mass alone.
        if (kShowCostOnCell && readoutFont.usable()) {
            constexpr float kCostChip = kUnit * 0.7f;  ///< a footnote, not a row heading
            const float costBaseline = cy + layout.cellHeight - 5.0f;
            const std::string mass = formatAmount(option.massCost);
            const std::string energy = formatAmount(option.energyCost);
            const std::string seconds = formatClock(option.buildSeconds);
            const float gap = kUnit * 0.35f;
            const float massWidth = text::measureText(readoutFont.glyphs, mass);
            const float energyWidth = text::measureText(readoutFont.glyphs, energy);
            const float timeWidth = text::measureText(readoutFont.glyphs, seconds);

            // Two rows rather than one: mass and energy read as a pair, and the time is the
            // odd one out because it is not a resource. Three figures on one line at this cell
            // width would be four points apart and unreadable.
            float pen = cx
                      + (layout.cellWidth
                         - (kCostChip * 2.0f + gap * 3.0f + massWidth + energyWidth))
                            * 0.5f;
            const float pairBaseline = costBaseline - kBuildCostStrip * 0.5f;
            const Colour massTint = fade(option.affordable ? kMass : kLoss, alpha);
            text::appendRect(out.foregroundReadout, readoutFont, pen,
                             pairBaseline - kCostChip, kCostChip,
                             kCostChip, massTint);
            pen = text::appendText(out.foregroundReadout, readoutFont.glyphs, mass,
                                   pen + kCostChip + gap,
                                   pairBaseline, massTint);
            pen += gap;
            text::appendRect(out.foregroundReadout, readoutFont, pen,
                             pairBaseline - kCostChip, kCostChip,
                             kCostChip, fade(kEnergy, alpha));
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, energy,
                                   pen + kCostChip + gap, pairBaseline, fade(kEnergy, alpha));

            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, seconds,
                                   cx + (layout.cellWidth - timeWidth) * 0.5f, costBaseline,
                                   fade(kInk, alpha * 0.8f));
        }

        // The hovered cell gets a full lit border, which is BAR's own "this one" and reads
        // instantly against the hairline every other cell carries.
        const bool lit = hovered.has_value() && *hovered == index;
        const Colour border = lit ? theme.edgeLit : fade(theme.edge, alpha);
        const float thickness = lit ? kBevel * 2.0f : kBevel;
        text::appendRect(out.chrome, labelFont, cx, cy, layout.cellWidth, thickness, border);
        text::appendRect(out.chrome, labelFont, cx, cy + layout.cellHeight - thickness,
                          layout.cellWidth, thickness, border);
        text::appendRect(out.chrome, labelFont, cx, cy, thickness, layout.cellHeight, border);
        text::appendRect(out.chrome, labelFont, cx + layout.cellWidth - thickness, cy, thickness,
                          layout.cellHeight, border);
    }
}

} // namespace rm::ui
