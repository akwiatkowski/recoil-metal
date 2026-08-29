#include "core/ui/Roster.hpp"

#include "core/ui/IconAtlas.hpp"

#include <algorithm>
#include <cmath>

namespace rm::ui {

float RosterTile::fill() const noexcept {
    if (maxHealth <= 0.0f) {
        return 1.0f;  // no stated maximum reads as full — see the note on the field
    }
    return std::clamp(health / maxHealth, 0.0f, 1.0f);
}

RosterLayout rosterLayout(const FrameLayout& frame, std::size_t tileCount,
                           std::size_t page) noexcept {
    RosterLayout layout;
    if (tileCount == 0 || frame.rosterSlots == 0) {
        return layout;  // nothing selected: the roster is absent, not an empty frame
    }

    layout.x = frame.selection.x;
    layout.y = frame.selection.y;
    layout.width = frame.selection.width;
    layout.height = frame.selection.height;
    layout.pages = (tileCount + frame.rosterSlots - 1) / frame.rosterSlots;
    layout.page = std::min(page, layout.pages - 1);
    layout.first = layout.page * frame.rosterSlots;
    layout.shown = std::min(frame.rosterSlots, tileCount - layout.first);
    layout.hidden = tileCount - layout.shown;
    layout.tilesX = layout.x + 8.0f;
    layout.tilesY = layout.y + 109.0f;
    return layout;
}

std::array<float, 2> rosterTileOrigin(const RosterLayout& layout, std::size_t index) noexcept {
    return {{layout.tilesX + static_cast<float>(index) * (layout.tileSize + kRosterGap),
              layout.tilesY}};
}

std::optional<std::size_t> rosterTileAt(const RosterLayout& layout, float pointX,
                                        float pointY) noexcept {
    if (layout.empty()) {
        return std::nullopt;
    }
    const float localX = pointX - layout.tilesX;
    const float localY = pointY - layout.tilesY;
    if (localX < 0.0f || localY < 0.0f || localY > layout.tileSize) {
        return std::nullopt;
    }

    const float pitch = layout.tileSize + kRosterGap;
    const auto local = static_cast<std::size_t>(std::floor(localX / pitch));
    if (local >= layout.shown) {
        return std::nullopt;
    }
    // The gutter is dead space, as the build tray's is and for the same reason.
    if (localX - static_cast<float>(local) * pitch > layout.tileSize) {
        return std::nullopt;
    }
    return layout.first + local;
}

std::optional<int> rosterPageStepAt(const RosterLayout& layout, float pointX,
                                    float pointY) noexcept {
    if (layout.pages <= 1 || pointY < layout.y + 84.0f || pointY >= layout.tilesY) {
        return std::nullopt;
    }
    constexpr float kArrowWidth = 22.0f;
    const float right = layout.x + layout.width - 8.0f;
    if (pointX >= right - kArrowWidth && pointX < right) {
        return 1;
    }
    if (pointX >= right - kArrowWidth * 2.0f && pointX < right - kArrowWidth) {
        return -1;
    }
    return std::nullopt;
}

bool insideRoster(const RosterLayout& layout, float pointX, float pointY) noexcept {
    if (layout.empty()) {
        return false;
    }
    return pointX >= layout.x && pointX < layout.x + layout.width && pointY >= layout.y
           && pointY < layout.y + layout.height;
}

std::vector<RosterTile> groupSelection(std::span<const std::string> ids,
                                       std::span<const float> health,
                                       std::span<const float> maxHealth,
                                       std::span<const std::string> names) {
    std::vector<RosterTile> tiles;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const float hp = i < health.size() ? health[i] : 0.0f;
        const float max = i < maxHealth.size() ? maxHealth[i] : 0.0f;

        // A LINEAR SCAN, deliberately. A selection is tens of units and a handful of types, so a
        // map costs an allocation and a hash to save nothing — and the scan is what keeps the
        // order first-appearance without a second structure remembering it.
        const auto found = std::find_if(tiles.begin(), tiles.end(),
                                        [&](const RosterTile& t) { return t.id == ids[i]; });
        if (found != tiles.end()) {
            ++found->count;
            found->health += hp;
            found->maxHealth += max;
            continue;
        }
        tiles.push_back(RosterTile{.id = ids[i],
                                   .name = i < names.size() ? names[i] : std::string{},
                                   .count = 1,
                                   .health = hp,
                                   .maxHealth = max});
    }
    return tiles;
}

InfoCard rosterTileCard(const RosterTile& tile) {
    InfoCard card;
    card.title = tile.name.empty() ? tile.id : tile.name;
    // No blueprint id beside the name it already spells out — see `kShowBlueprintIds`.
    if (kShowBlueprintIds && !tile.name.empty()) {
        card.corner = tile.id;
    }

    // Stated even at one — "COUNT 1" is the difference between "this tile is one unit" and a
    // reader wondering whether the badge was dropped. The tile's own badge stays silent at
    // one for the opposite reason: on the grid the common case must cost no ink.
    card.rows.push_back(InfoRow{.label = "COUNT", .value = std::to_string(tile.count)});

    // The exact numbers the underbar compresses into colour, in that bar's own colour — the
    // card and the bar must not disagree about how bad it is.
    if (tile.maxHealth > 0.0f) {
        const float fill = tile.fill();
        card.rows.push_back(InfoRow{
            .label = "HEALTH",
            .value = formatAmount(tile.health) + " / " + formatAmount(tile.maxHealth),
            .tint = fill > 0.6f ? kGain : (fill > 0.3f ? kWarn : kLoss)});
    }
    return card;
}

void appendRoster(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
                   const Theme& theme, const RosterLayout& layout,
                   std::span<const RosterTile> tiles, std::optional<std::size_t> hovered,
                   const InfoCard* inspector) {
    if (layout.empty() || tiles.empty() || !labelFont.usable()) {
        return;
    }

    appendPanel(out, labelFont, theme, layout.x, layout.y, layout.width, layout.height);
    const Rect inspectorRect{layout.x + 8.0f, layout.y + 8.0f, layout.width - 16.0f, 68.0f};
    if (inspector != nullptr) {
        appendInspector(out, labelFont, readoutFont, theme, inspectorRect, *inspector);
    }
    text::appendRect(out.chrome, labelFont, layout.x + 8.0f, layout.y + 84.0f,
                     layout.width - 16.0f, kBevel, fade(theme.edge, 0.9f));

    // The header: the selection's TOTAL, which grouping-by-type otherwise erases. "14 UNITS"
    // is the first fact of a selection — how many things the next order is about to move — and
    // the one number no tile can carry. Set like the build tray's header, because the two
    // panels are the same fitting and a player should read them as one instrument family.
    {
        std::size_t total = 0;
        for (const RosterTile& tile : tiles) {
            total += tile.count;
        }
        const float headerBaseline = layout.y + 102.0f;
        const std::string summary =
            std::to_string(total) + (total == 1 ? " UNIT" : " UNITS");
        (void)text::appendText(out.label, labelFont.glyphs, summary, layout.x + 8.0f,
                               headerBaseline, theme.label);

        // WHAT WAS DROPPED, SAID OUT LOUD — in the summary line, where a count belongs. A
        // roster that silently omits types is a roster that lies about the selection, and the
        // lie is invisible: the row looks complete.
        if (layout.pages > 1 && readoutFont.usable()) {
            const std::string more = std::to_string(layout.page + 1) + "/"
                                   + std::to_string(layout.pages) + "  <  >";
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, more,
                                    layout.x + layout.width - 8.0f
                                        - text::measureText(readoutFont.glyphs, more),
                                    headerBaseline, kInk);
        }
    }

    for (std::size_t local = 0; local < layout.shown; ++local) {
        const std::size_t index = layout.first + local;
        if (index >= tiles.size()) {
            break;
        }
        const RosterTile& tile = tiles[index];
        const std::array<float, 2> origin = rosterTileOrigin(layout, local);
        const float tx = origin[0];
        const float ty = origin[1];

        // The tile's well in gradient glass, the build cell's treatment at the roster's size.
        text::appendRectV(out.chrome, labelFont, tx, ty, layout.tileSize, layout.tileSize,
                          Colour{{theme.well[0] * 1.5f, theme.well[1] * 1.5f,
                                  theme.well[2] * 1.5f, theme.well[3]}},
                          Colour{{theme.well[0] * 0.7f, theme.well[1] * 0.7f,
                                  theme.well[2] * 0.7f, theme.well[3]}});

        // The icon, from the same atlas the build tray packs.
        if (tile.iconSlot) {
            const IconUv uv = iconUv(*tile.iconSlot);
            constexpr Colour kFull{{1.0f, 1.0f, 1.0f, 1.0f}};
            const float x1 = tx + layout.tileSize;
            const float y1 = ty + layout.tileSize;
            out.icon.push_back({{tx, ty}, {uv.u0, uv.v0}, kFull});
            out.icon.push_back({{x1, ty}, {uv.u1, uv.v0}, kFull});
            out.icon.push_back({{x1, y1}, {uv.u1, uv.v1}, kFull});
            out.icon.push_back({{tx, ty}, {uv.u0, uv.v0}, kFull});
            out.icon.push_back({{x1, y1}, {uv.u1, uv.v1}, kFull});
            out.icon.push_back({{tx, y1}, {uv.u0, uv.v1}, kFull});
        } else if (labelFont.usable()) {
            // No picture: the NAME, wrapped small, so a tile is never anonymous. The id is the
            // fallback's fallback — content that states no display name leaves nothing else to
            // print, which is the only place `kShowBlueprintIds` does not reach.
            const std::string& label = tile.name.empty() ? tile.id : tile.name;
            const float lineHeight =
                labelFont.lineHeight > 0.0f ? labelFont.lineHeight : kUnit * 1.8f;
            const std::vector<std::string> lines =
                wrapToWidth(labelFont.glyphs, label, layout.tileSize - kUnit, 2);
            for (std::size_t line = 0; line < lines.size(); ++line) {
                const float width = text::measureText(labelFont.glyphs, lines[line]);
                (void)text::appendText(out.label, labelFont.glyphs, lines[line],
                                       tx + (layout.tileSize - width) * 0.5f,
                                       ty + layout.tileSize * 0.5f
                                           + static_cast<float>(line) * lineHeight,
                                       theme.label);
            }
        }

        // THE COUNT BADGE, bottom-right of the tile and only when there is more than one.
        // A `x1` on every single unit is noise on the common case — one thing selected — and
        // the badge's job is to say "this tile is a GROUP".
        //
        // ON A PLATE, because it is drawn over the ARTWORK and the artwork is bright metal.
        // The first version put light ink straight onto the icon and the number was legible on
        // a dark unit and gone on a pale one — which is the worst kind of readout, since it
        // fails per unit type rather than visibly. Near-black rather than the theme's well,
        // because a plate has to work against the BRIGHTEST icon in the corpus and the well is
        // itself a mid tone — and white on black is the one pairing no artwork can defeat.
        if (tile.count > 1 && readoutFont.usable()) {
            const std::string badge = "x" + std::to_string(tile.count);
            const float width = text::measureText(readoutFont.glyphs, badge);
            const float plateW = width + kUnit * 0.8f;
            const float plateH = kUnit * 1.8f;
            const float plateX = tx + layout.tileSize - plateW - kBevel;
            const float plateY = ty + layout.tileSize - plateH - kBevel;
            // INTO THE FOREGROUND READOUT LAYER, so both plate and badge remain above icon art.
            text::appendRect(out.foregroundReadout, readoutFont, plateX, plateY, plateW, plateH,
                             Colour{{0.0f, 0.0f, 0.0f, 0.72f}});
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, badge,
                                   plateX + kUnit * 0.4f, plateY + plateH - kUnit * 0.5f,
                                   Colour{{1.0f, 1.0f, 1.0f, 1.0f}});
        }

        // The health underbar: a track, and the group's summed fill over it.
        const float barY = ty + layout.tileSize + kUnit * 0.3f;
        text::appendRect(out.chrome, labelFont, tx, barY, layout.tileSize, kRosterBar,
                         fade(theme.edge, 0.5f));
        const float fill = tile.fill();
        if (fill > 0.0f) {
            // GREEN DOWN TO RED, because a health bar is read by colour before it is read by
            // length — at this size the length difference between half and two-thirds is a
            // couple of pixels and the colour difference is not.
            const Colour bar = fill > 0.6f ? kGain : (fill > 0.3f ? kWarn : kLoss);
            text::appendRect(out.chrome, labelFont, tx, barY, layout.tileSize * fill, kRosterBar,
                             bar);
        }

        const bool lit = hovered.has_value() && *hovered == index;
        const Colour border = lit ? theme.edgeLit : theme.edge;
        const float thickness = lit ? kBevel * 2.0f : kBevel;
        text::appendRect(out.chrome, labelFont, tx, ty, layout.tileSize, thickness, border);
        text::appendRect(out.chrome, labelFont, tx, ty + layout.tileSize - thickness,
                          layout.tileSize,
                          thickness, border);
        text::appendRect(out.chrome, labelFont, tx, ty, thickness, layout.tileSize, border);
        text::appendRect(out.chrome, labelFont, tx + layout.tileSize - thickness, ty, thickness,
                          layout.tileSize, border);
    }

}

} // namespace rm::ui
