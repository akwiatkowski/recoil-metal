#pragma once

#include "core/text/TextLayout.hpp"
#include "core/ui/Hud.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rm::ui {

// What is selected, as a row of typed tiles. Track 0's UI-2.
//
// WHY A ROSTER AND NOT A COUNT. "23 units selected" is a number a player cannot act on. What
// they need before giving an order is the COMPOSITION — eleven tanks, one commander, three
// engineers — because the order they are about to give applies to all of it, and "attack" means
// something different when an engineer is in the box. Both reference games show the composition
// and neither shows a bare total.
//
// GROUPED BY TYPE, WITH A COUNT, which is the whole design. A tile per unit means a hundred
// tiles for a hundred tanks and no information; a tile per TYPE with `xN` on it is the same
// screen in one row. Recoil and Supreme Commander both do this, and both cap the tile count
// rather than shrinking tiles — a roster that reflows as units die is a roster a player cannot
// learn the shape of.
//
// THE HEALTH UNDERBAR IS THE GROUP'S, NOT A UNIT'S. Eleven tanks are one tile, so the bar shows
// their summed health against their summed maximum. That is the number a player is deciding on:
// "is this group still worth committing", not "which of these eleven is hurt".
//
// EVERYTHING HERE IS ARITHMETIC over a state struct, tested without a renderer, for the reason
// `Hud.hpp` and `BuildPanel.hpp` both give — a tile that overlaps its neighbour or a bar that
// clamps wrongly is invisible in a screenshot and obvious in an assertion.

// --- Metrics ----------------------------------------------------------------

/// A tile's side, in points. Smaller than a build cell: a roster tile carries an icon, a count
/// and a bar, and no cost line.
inline constexpr float kRosterTile = kUnit * 9.0f;

inline constexpr float kRosterGap = kUnit * 0.5f;

/// The health bar under each tile.
inline constexpr float kRosterBar = kUnit * 0.7f;

/// How many tiles the row shows before it stops.
///
/// TWELVE, and it is a cap rather than a scroll or a shrink. A selection of forty types does not
/// exist in practice — the corpus's widest realistic mixed selection is a handful — and the
/// failure mode being avoided is a row that changes tile size as units die, which destroys the
/// by-position reading the grid exists for. What is dropped is reported (`hidden`) rather than
/// silently missing, because a roster that quietly omits is a roster that lies.
inline constexpr std::size_t kRosterMaxTiles = 12;

// --- State ------------------------------------------------------------------

/// One type in the selection.
struct RosterTile {
    /// The blueprint id, as the corpus spells it.
    std::string id;

    /// How many of this type are selected. Always at least one.
    std::size_t count = 1;

    /// The group's health, summed, and its summed maximum.
    float health = 0.0f;
    float maxHealth = 0.0f;

    /// Which slot in the icon atlas holds this type's picture, or none. Same atlas the build
    /// tray uses (`core/ui/IconAtlas.hpp`) — a unit's icon is a fact about the unit, not about
    /// which panel is asking.
    std::optional<std::size_t> iconSlot;

    /// 0..1 of maximum, clamped. A group with no stated maximum reads as FULL rather than
    /// empty: an indestructible or unread type is not a group in trouble, and an empty bar
    /// under a healthy selection is the more alarming of the two wrong answers.
    [[nodiscard]] float fill() const noexcept;
};

/// Where the roster sits and how wide it runs.
struct RosterLayout {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    /// How many tiles are drawn, and how many the selection had that will not fit.
    std::size_t shown = 0;
    std::size_t hidden = 0;

    [[nodiscard]] bool empty() const noexcept { return shown == 0; }
};

/// Places the roster along the bottom of the viewport, centred.
///
/// BOTTOM CENTRE, which is where both reference games put it and is not arbitrary: the minimap
/// and build tray own the bottom-left, the clock the top-right, and the resource panel the
/// top-left. Centre-bottom is the one region left, and it is where the eye already is during a
/// fight — a player watching their units is looking at the middle of the screen.
[[nodiscard]] RosterLayout rosterLayout(float viewportWidth, float viewportHeight,
                                        std::size_t tileCount) noexcept;

/// The top-left of one tile, in points.
[[nodiscard]] std::array<float, 2> rosterTileOrigin(const RosterLayout& layout,
                                                    std::size_t index) noexcept;

/// Which tile a screen point is over, if any. Gutters are dead space, as the build tray's are.
[[nodiscard]] std::optional<std::size_t> rosterTileAt(const RosterLayout& layout, float pointX,
                                                      float pointY) noexcept;

/// Whether a screen point is on the roster at all — tiles, gutters and padding alike.
[[nodiscard]] bool insideRoster(const RosterLayout& layout, float pointX,
                                float pointY) noexcept;

/// Draws the roster: a tile per type, its icon, its `xN` badge and its health underbar.
void appendRoster(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
                  const Theme& theme, const RosterLayout& layout,
                  std::span<const RosterTile> tiles, std::optional<std::size_t> hovered);

/// Groups a flat list of (id, health, maxHealth) into tiles, one per type.
///
/// ORDER IS FIRST-APPEARANCE, not alphabetical and not by count. A selection is made by
/// dragging a box, and the thing a player expects at the left of the roster is whatever the
/// selection is mostly about — which in practice is the first thing the box caught. Sorting by
/// count would reorder the row as units die, which is the reflow this design exists to avoid.
[[nodiscard]] std::vector<RosterTile> groupSelection(
    std::span<const std::string> ids, std::span<const float> health,
    std::span<const float> maxHealth);

} // namespace rm::ui
