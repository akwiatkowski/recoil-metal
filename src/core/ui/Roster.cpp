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

RosterLayout rosterLayout(float viewportWidth, float viewportHeight,
                          std::size_t tileCount) noexcept {
    RosterLayout layout;
    if (tileCount == 0 || viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
        return layout;  // nothing selected: the roster is absent, not an empty frame
    }

    layout.shown = std::min(tileCount, kRosterMaxTiles);
    layout.hidden = tileCount - layout.shown;

    const auto shown = static_cast<float>(layout.shown);
    const float row = shown * kRosterTile + (shown - 1.0f) * kRosterGap;

    layout.width = row + kPad * 2.0f;
    layout.height = kRosterHeader + kRosterTile + kRosterBar + kUnit * 0.5f + kPad * 2.0f;

    // Centred horizontally, one margin off the bottom.
    layout.x = (viewportWidth - layout.width) * 0.5f;
    layout.y = viewportHeight - layout.height - kMargin;
    return layout;
}

std::array<float, 2> rosterTileOrigin(const RosterLayout& layout, std::size_t index) noexcept {
    return {{layout.x + kPad + static_cast<float>(index) * (kRosterTile + kRosterGap),
             layout.y + kPad + kRosterHeader}};
}

std::optional<std::size_t> rosterTileAt(const RosterLayout& layout, float pointX,
                                        float pointY) noexcept {
    if (layout.empty()) {
        return std::nullopt;
    }
    const float localX = pointX - (layout.x + kPad);
    const float localY = pointY - (layout.y + kPad + kRosterHeader);
    if (localX < 0.0f || localY < 0.0f || localY > kRosterTile) {
        return std::nullopt;
    }

    const float pitch = kRosterTile + kRosterGap;
    const auto index = static_cast<std::size_t>(std::floor(localX / pitch));
    if (index >= layout.shown) {
        return std::nullopt;
    }
    // The gutter is dead space, as the build tray's is and for the same reason.
    if (localX - static_cast<float>(index) * pitch > kRosterTile) {
        return std::nullopt;
    }
    return index;
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
    if (!tile.name.empty()) {
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
                  std::span<const RosterTile> tiles, std::optional<std::size_t> hovered) {
    if (layout.empty() || tiles.empty() || !labelFont.usable()) {
        return;
    }

    appendPanel(out, labelFont, theme, layout.x, layout.y, layout.width, layout.height);

    // The header: the selection's TOTAL, which grouping-by-type otherwise erases. "14 UNITS"
    // is the first fact of a selection — how many things the next order is about to move — and
    // the one number no tile can carry. Set like the build tray's header, because the two
    // panels are the same fitting and a player should read them as one instrument family.
    {
        std::size_t total = 0;
        for (const RosterTile& tile : tiles) {
            total += tile.count;
        }
        const float headerBaseline = layout.y + kPad + kRosterHeader * 0.7f;
        const std::string summary =
            std::to_string(total) + (total == 1 ? " UNIT" : " UNITS");
        (void)text::appendText(out.label, labelFont.glyphs, summary, layout.x + kPad,
                               headerBaseline, theme.label);

        // WHAT WAS DROPPED, SAID OUT LOUD — in the summary line, where a count belongs. A
        // roster that silently omits types is a roster that lies about the selection, and the
        // lie is invisible: the row looks complete.
        if (layout.hidden > 0 && readoutFont.usable()) {
            const std::string more = "+" + std::to_string(layout.hidden)
                                     + (layout.hidden == 1 ? " TYPE" : " TYPES");
            (void)text::appendText(out.readout, readoutFont.glyphs, more,
                                   layout.x + layout.width - kPad
                                       - text::measureText(readoutFont.glyphs, more),
                                   headerBaseline, kInk);
        }

        // The rule under the header — the build tray's device, shared deliberately.
        text::appendRect(out.label, labelFont, layout.x + kPad,
                         layout.y + kPad + kRosterHeader - kBevel, layout.width - kPad * 2.0f,
                         kBevel, fade(theme.edge, 0.9f));
    }

    for (std::size_t index = 0; index < layout.shown && index < tiles.size(); ++index) {
        const RosterTile& tile = tiles[index];
        const std::array<float, 2> origin = rosterTileOrigin(layout, index);
        const float tx = origin[0];
        const float ty = origin[1];

        // The tile's well in gradient glass, the build cell's treatment at the roster's size.
        text::appendRectV(out.label, labelFont, tx, ty, kRosterTile, kRosterTile,
                          Colour{{theme.well[0] * 1.5f, theme.well[1] * 1.5f,
                                  theme.well[2] * 1.5f, theme.well[3]}},
                          Colour{{theme.well[0] * 0.7f, theme.well[1] * 0.7f,
                                  theme.well[2] * 0.7f, theme.well[3]}});

        // The icon, from the same atlas the build tray packs.
        if (tile.iconSlot) {
            const IconUv uv = iconUv(*tile.iconSlot);
            constexpr Colour kFull{{1.0f, 1.0f, 1.0f, 1.0f}};
            const float x1 = tx + kRosterTile;
            const float y1 = ty + kRosterTile;
            out.image.push_back({{tx, ty}, {uv.u0, uv.v0}, kFull});
            out.image.push_back({{x1, ty}, {uv.u1, uv.v0}, kFull});
            out.image.push_back({{x1, y1}, {uv.u1, uv.v1}, kFull});
            out.image.push_back({{tx, ty}, {uv.u0, uv.v0}, kFull});
            out.image.push_back({{x1, y1}, {uv.u1, uv.v1}, kFull});
            out.image.push_back({{tx, y1}, {uv.u0, uv.v1}, kFull});
        } else if (readoutFont.usable()) {
            // No picture: the id, small, so a tile is never anonymous.
            const float width = text::measureText(readoutFont.glyphs, tile.id);
            if (width <= kRosterTile - kUnit) {
                (void)text::appendText(out.readout, readoutFont.glyphs, tile.id,
                                       tx + (kRosterTile - width) * 0.5f,
                                       ty + kRosterTile * 0.55f, theme.label);
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
            const float plateX = tx + kRosterTile - plateW - kBevel;
            const float plateY = ty + kRosterTile - plateH - kBevel;
            // INTO THE READOUT LIST, not the label list, and that is the whole reason the
            // badge is readable. The icons are drawn between the two faces, so anything in
            // `label` ends up UNDER the artwork — a plate there is a plate nobody sees, with
            // its own text floating over the icon it was meant to sit on.
            text::appendRect(out.readout, readoutFont, plateX, plateY, plateW, plateH,
                             Colour{{0.0f, 0.0f, 0.0f, 0.72f}});
            (void)text::appendText(out.readout, readoutFont.glyphs, badge,
                                   plateX + kUnit * 0.4f, plateY + plateH - kUnit * 0.5f,
                                   Colour{{1.0f, 1.0f, 1.0f, 1.0f}});
        }

        // The health underbar: a track, and the group's summed fill over it.
        const float barY = ty + kRosterTile + kUnit * 0.3f;
        text::appendRect(out.label, labelFont, tx, barY, kRosterTile, kRosterBar,
                         fade(theme.edge, 0.5f));
        const float fill = tile.fill();
        if (fill > 0.0f) {
            // GREEN DOWN TO RED, because a health bar is read by colour before it is read by
            // length — at this size the length difference between half and two-thirds is a
            // couple of pixels and the colour difference is not.
            const Colour bar = fill > 0.6f ? kGain : (fill > 0.3f ? kWarn : kLoss);
            text::appendRect(out.label, labelFont, tx, barY, kRosterTile * fill, kRosterBar,
                             bar);
        }

        const bool lit = hovered.has_value() && *hovered == index;
        const Colour border = lit ? theme.edgeLit : theme.edge;
        const float thickness = lit ? kBevel * 2.0f : kBevel;
        text::appendRect(out.label, labelFont, tx, ty, kRosterTile, thickness, border);
        text::appendRect(out.label, labelFont, tx, ty + kRosterTile - thickness, kRosterTile,
                         thickness, border);
        text::appendRect(out.label, labelFont, tx, ty, thickness, kRosterTile, border);
        text::appendRect(out.label, labelFont, tx + kRosterTile - thickness, ty, thickness,
                         kRosterTile, border);
    }

}

} // namespace rm::ui
