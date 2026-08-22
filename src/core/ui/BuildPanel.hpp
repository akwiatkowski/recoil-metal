#pragma once

#include "core/text/TextLayout.hpp"
#include "core/ui/Hud.hpp"
#include "core/ui/Minimap.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace rm::ui {

// What the selected builder can build, as a grid of cells.
//
// WHY THIS EXISTS. `core/unit/BuildTree.hpp` already answers "what can this unit build" from the
// corpus — 105 builders resolved against 568 units at load — and nothing showed the answer. A
// player selecting a commander had no way to see the thing the engine already knew, and the
// scripted opponent was the only consumer of a build order. This is the readout for it.
//
// MODELLED ON BEYOND ALL REASON, deliberately and specifically. BAR's build menu is a tight grid
// of square buttons in the bottom-left cluster, immediately right of the minimap, and the two
// read as one control block rather than as two panels that happen to be near each other. Three
// things about it are worth copying and are copied here:
//
//   THE GRID IS SQUARE-CELLED AND TIGHT. Buttons are squares with a small gap, not rows. A grid
//   is scannable by POSITION — the third item is always in the same place — which is what lets a
//   player learn a build order as muscle memory rather than reading labels every time.
//
//   IT ANCHORS TO THE MINIMAP. BAR grows the menu upward and rightward from the minimap's corner,
//   so the whole bottom-left is one block. `buildPanelLayout` takes the `MinimapLayout` for
//   exactly this reason rather than computing its own corner.
//
//   COST IS ON THE FACE OF THE BUTTON. Not in a tooltip. The number a player decides on is the
//   mass cost, and hiding it behind a hover makes comparing two options a two-step task.
//
// THE ICON IS THE GAME'S OWN. `textures.scd` ships 538 unit icons at
// `textures/ui/common/icons/units/<ID>_icon.dds`, 64x64 DXT5, and they are packed into one atlas
// so the whole menu is a single texture bind (`core/ui/IconAtlas.hpp`). This note used to say we
// had none and that a cell carried "the blueprint id and a tint band instead"; the square it
// described as reserved for later is the square they go in now. The id stays, beneath the icon
// rather than instead of it — a five-character code is still how a player names the thing to
// somebody else.
//
// EVERYTHING HERE IS ARITHMETIC over a state struct, tested without a renderer — the same reason
// `Hud.hpp` gives. A grid that overlaps its neighbour, a hit test that disagrees with the drawn
// cell, a wrap that loses the last row: all invisible in a screenshot and obvious in an assertion.

// --- Metrics ----------------------------------------------------------------

/// A cell's WIDTH, in points.
///
/// SIZED FROM THE LONGEST THING IT HOLDS, which is a seven-character blueprint id like
/// `UEB1103` at the label font's natural size. The first version guessed 48 and the ids ran
/// straight through their cell borders into the next column — visible immediately in a capture,
/// invisible in every layout assertion, because overflow is a font fact and the tests only knew
/// about rectangles. Scaling the text down instead would cost crispness, which `appendText`'s
/// own note warns against.
inline constexpr float kBuildCell = kUnit * 12.0f;

/// A cell's HEIGHT, in points — taller than it is wide, since display names arrived.
///
/// The face carries an icon, up to TWO lines of name, and the cost; a square cell held the
/// icon, one id line and the cost exactly, and "Mass Extractor" does not fit one line at this
/// width. The height buys the second line; the width stays, because three columns of it is
/// what docks onto the minimap and what the by-position muscle memory is trained on.
inline constexpr float kBuildCellHeight = kUnit * 15.0f;

/// The gap between cells. One unit, not two: BAR's grid is TIGHT, and the tightness is what
/// makes it read as one control rather than as scattered buttons.
inline constexpr float kBuildGap = kUnit * 0.5f;

/// How many columns the grid runs before wrapping.
///
/// Three, as BAR's default does. A wider grid needs a wider eye sweep for the same number of
/// items and stops fitting beside the minimap; a narrower one grows tall enough to cover the
/// view. Three is also what makes tier rows land together for the shipped corpus, where a
/// tech level's structures come in threes and fours.
inline constexpr int kBuildColumns = 3;

/// The strip above the grid carrying the builder's name.
inline constexpr float kBuildHeader = kUnit * 3.0f;

/// The icon square inside a cell. What is left below it carries two lines: the id, then the cost.
inline constexpr float kBuildIcon = kBuildCell - kUnit * 6.0f;

// --- State ------------------------------------------------------------------

/// One thing the selected builder could start.
struct BuildOption {
    /// The blueprint id as its directory spells it — `UEB1103`. The hover card states it;
    /// the cell face carries the name below when there is one.
    std::string id;

    /// The display name — "Mass Extractor" — or empty, in which case the cell face shows the
    /// id, which is what every cell showed before names existed.
    std::string name;

    float massCost = 0.0f;
    float energyCost = 0.0f;

    /// For the hover card: how long the build takes at the builder's own rate, and what the
    /// result can survive. Zero means the content stated nothing, and the card omits the row
    /// rather than printing a zero as if it were a measurement.
    float buildSeconds = 0.0f;
    float health = 0.0f;

    /// Whether the army can pay for it RIGHT NOW, from stored mass.
    ///
    /// Shown by dimming rather than by hiding: a player deciding what to build next needs to see
    /// the thing they cannot yet afford, because "not yet" is the information. Hiding it would
    /// make the grid reflow as the economy moves, which is the one thing a
    /// learned-by-position layout must never do.
    bool affordable = true;

    /// The tint band across the top of the cell. Carries the tech tier, so a grid of a dozen
    /// options separates into tiers without a label per row.
    Colour tint{};

    /// Which slot in the icon atlas holds this unit's picture, or none.
    ///
    /// AN INDEX RATHER THAN A TEXTURE, because the whole menu is one atlas and one draw — see
    /// `core/ui/IconAtlas.hpp`. None is ordinary: a blueprint whose icon is missing from the
    /// archives keeps its reserved square and its id, which is what the panel looked like
    /// before there were icons at all.
    std::optional<std::size_t> iconSlot;
};

/// Where the grid sits and how it is divided.
struct BuildPanelLayout {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    /// The grid's origin — below the header, inside the padding.
    float gridX = 0.0f;
    float gridY = 0.0f;

    int columns = kBuildColumns;
    int rows = 0;

    [[nodiscard]] bool empty() const noexcept { return rows <= 0; }
};

/// Places the panel against the minimap, DOCKED onto its top edge and growing UPWARD.
///
/// Upward rather than downward because the minimap is already at the bottom margin, and rightward
/// would put the grid where the eye expects the map. Growing up means a long option list pushes
/// into empty screen rather than off it, and a short one sits just above the map — which is
/// exactly how BAR's behaves as a factory's queue changes.
///
/// Docked flush — `y + height == minimap.y` — so the two hairlines meet and the tray and the
/// map read as one bottom-left command block rather than as neighbours.
[[nodiscard]] BuildPanelLayout buildPanelLayout(const MinimapLayout& minimap,
                                                std::size_t optionCount) noexcept;

/// Whether a screen point is on the panel at all — cells, gutters, header and padding alike.
///
/// DISTINCT FROM `buildOptionAt`, which answers "which cell", and the difference is the whole
/// reason both exist. A caller deciding whether the world behind the panel should hear about a
/// click must treat the gutters as panel: they are inside the frame the player aimed at, and
/// letting a click through one reaches past the interface to the ground under it. A caller
/// deciding what to BUILD must treat them as neither, which is what `buildOptionAt` does.
[[nodiscard]] bool insideBuildPanel(const BuildPanelLayout& layout, float pointX,
                                    float pointY) noexcept;

/// The top-left of one cell, in points.
[[nodiscard]] std::array<float, 2> buildCellOrigin(const BuildPanelLayout& layout,
                                                   std::size_t index) noexcept;

/// Which option a screen point is over, if any.
///
/// ROUND-TRIPS with `buildCellOrigin`, and that pairing is the whole of this panel's
/// correctness — a cell drawn in one place and clicked in another is the same bug twice, and it
/// looks perfectly fine in a screenshot. The gap between cells is dead space rather than
/// belonging to a neighbour: a click landing in the gutter should do nothing, not the wrong
/// thing.
[[nodiscard]] std::optional<std::size_t> buildOptionAt(const BuildPanelLayout& layout,
                                                       std::size_t optionCount, float pointX,
                                                       float pointY) noexcept;

/// Draws the panel: header, grid, and each cell's tint band, id and cost.
///
/// `hovered` gets the lit border BAR uses to say "this one". An out-of-range index draws no
/// highlight rather than clamping to a neighbour, because highlighting the wrong cell is worse
/// than highlighting none.
///
/// The header sets `builderRole` — the word a player thinks in, "COMMANDER" — as its lead, and
/// `builderName` (the blueprint id) right-aligned in the readout face. An empty role promotes
/// the id back to the lead seat rather than leaving the line blank.
void appendBuildPanel(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
                      const Theme& theme, const BuildPanelLayout& layout,
                      std::span<const BuildOption> options, std::optional<std::size_t> hovered,
                      std::string_view builderName, std::string_view builderRole = {});

/// A display name broken to fit a cell face: up to two lines, empty slots unused.
///
/// GREEDY BY WORDS — "Mass Extractor" becomes "Mass" / "Extractor" — because a name is words
/// and a break inside one is a different name. A single word wider than the cell is truncated
/// by characters instead, which is the honest floor: "Experiment" clipped is still legible
/// where a vanished line (the id's old overflow rule) says the cell is nameless. A third line's
/// worth of words is dropped; the hover card states the full name.
[[nodiscard]] std::array<std::string_view, 2> wrapCellName(
    std::span<const text::Glyph> glyphs, std::string_view name, float maxWidth) noexcept;

/// The hover card for one build option: full name, id in the corner, and the facts a player
/// weighs before building — mass (in the loss colour when it cannot be paid), energy, build
/// time at the selected builder's rate, and what the result can survive. A zero row is
/// omitted rather than printed: zero means the content stated nothing, not a measurement.
[[nodiscard]] InfoCard buildOptionCard(const BuildOption& option);

/// The tint for a tech tier, 1..3.
///
/// A ramp from the faction's own edge colour toward white, so tiers read as "more" rather than
/// as unrelated categories — which is what a hue-per-tier scheme does.
///
/// OUT OF RANGE CLAMPS rather than falling back to tier one, and the direction matters in only
/// one place: tier 4 is what `techOf` returns for an EXPERIMENTAL, and reading an experimental
/// as tier one would paint the biggest thing in the game the same as a power generator. Below
/// one — a blueprint declaring no tier at all — clamps up to one, which is the honest floor.
/// An earlier draft of this line claimed the opposite of what the code does; the test asserts
/// the clamp, so the comment was the thing that was wrong.
[[nodiscard]] Colour tierTint(const Theme& theme, int tier) noexcept;

} // namespace rm::ui
