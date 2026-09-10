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

/// A cell's WIDTH, in authored HUD points.
///
/// SIZED FROM THE LONGEST THING IT HOLDS, which is a seven-character blueprint id like
/// `UEB1103` at the label font's natural size. The first version guessed 48 and the ids ran
/// straight through their cell borders into the next column — visible immediately in a capture,
/// invisible in every layout assertion, because overflow is a font fact and the tests only knew
/// about rectangles. Scaling the text down instead would cost crispness, which `appendText`'s
/// own note warns against.
inline constexpr float kBuildCellHeight = 66.0f;

/// The gap between cells. One unit, not two: BAR's grid is TIGHT, and the tightness is what
/// makes it read as one control rather than as scattered buttons.
inline constexpr float kBuildGap = kUnit * 0.5f;

/// How many columns the grid runs before wrapping.
///
/// Three, as BAR's default does. A wider grid needs a wider eye sweep for the same number of
/// items and stops fitting beside the minimap; a narrower one grows tall enough to cover the
/// view. Three is also what makes tier rows land together for the shipped corpus, where a
/// tech level's structures come in threes and fours.
inline constexpr int kBuildRows = 2;

/// The strip above the grid carrying the builder's name.
inline constexpr float kBuildHeader = 24.0f;
inline constexpr float kBuildPadding = 8.0f;

/// How many lines the cell's name may run to. Two: "Mass Extractor" needs both and nothing in
/// the shipped corpus needs a third at this cell width.
inline constexpr std::size_t kBuildNameLines = 2;

/// The band at the foot of a cell that `kShowCostOnCell` fills, and that the name gives way to.
inline constexpr float kBuildCostStrip = 20.0f;

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

    /// Whether this cell UPGRADES the selected builder rather than building something beside it.
    ///
    /// The tech path — `General.UpgradesTo` — is not a member of any `BuildableCategory`, so a
    /// factory's own tier two was in the corpus, reachable by the sim, and absent from every
    /// menu. It behaves differently in two ways the cell has to say out loud: it needs no
    /// PLACE (the factory upgrades where it stands), and it CONSUMES the builder rather than
    /// adding to the base.
    bool upgrade = false;
    bool queuedUpgrade = false; ///< successor of an upgrade already in progress

    /// Whether this cell submits at the builder itself even though the selected unit is not a
    /// factory — production queued on a factory STILL UNDER CONSTRUCTION. The order parks on
    /// the founder's queue and starts when the rising factory comes online, so it needs no
    /// placement click the way the founder's own structures do.
    bool atBuilder = false;

    /// Whether the army's stored mass covers the full price RIGHT NOW.
    ///
    /// This is advisory presentation state, never an eligibility gate. Supreme Commander lets
    /// construction start against resource flow and slows it when the economy cannot keep up.
    /// The cost turns to the loss colour, but the cell stays fully visible and clickable.
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

enum class BuildOptionAction { ArmPlacement, SubmitAtBuilder };

/// What clicking a build cell does. Stored resources deliberately do not participate: a
/// shortfall is resolved by economy funding after the order starts, not by disabling the order.
[[nodiscard]] BuildOptionAction buildOptionAction(const BuildOption& option,
                                                  std::string_view builderRole) noexcept;

/// Where the grid sits and how it is divided.
struct BuildPanelLayout {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    /// The grid's origin — below the header, inside the padding.
    float gridX = 0.0f;
    float gridY = 0.0f;
    float cellWidth = 0.0f;
    float cellHeight = kBuildCellHeight;

    int columns = 0;
    int rows = kBuildRows;
    std::size_t first = 0;
    std::size_t shown = 0;
    std::size_t page = 0;
    std::size_t pages = 0;

    [[nodiscard]] bool empty() const noexcept { return shown == 0; }
};

/// Fits one two-row page inside the frame's bounded build rectangle.
[[nodiscard]] BuildPanelLayout buildPanelLayout(const FrameLayout& frame,
                                                 std::size_t optionCount,
                                                 std::size_t page = 0) noexcept;

/// Whether an authored HUD point is on the panel at all — cells, gutters, header and padding alike.
///
/// DISTINCT FROM `buildOptionAt`, which answers "which cell", and the difference is the whole
/// reason both exist. A caller deciding whether the world behind the panel should hear about a
/// click must treat the gutters as panel: they are inside the frame the player aimed at, and
/// letting a click through one reaches past the interface to the ground under it. A caller
/// deciding what to BUILD must treat them as neither, which is what `buildOptionAt` does.
[[nodiscard]] bool insideBuildPanel(const BuildPanelLayout& layout, float pointX,
                                    float pointY) noexcept;

/// The top-left of one cell, in authored HUD points.
[[nodiscard]] std::array<float, 2> buildCellOrigin(const BuildPanelLayout& layout,
                                                   std::size_t index) noexcept;

/// Which option an authored HUD point is over, if any.
///
/// ROUND-TRIPS with `buildCellOrigin`, and that pairing is the whole of this panel's
/// correctness — a cell drawn in one place and clicked in another is the same bug twice, and it
/// looks perfectly fine in a screenshot. The gap between cells is dead space rather than
/// belonging to a neighbour: a click landing in the gutter should do nothing, not the wrong
/// thing.
[[nodiscard]] std::optional<std::size_t> buildOptionAt(const BuildPanelLayout& layout,
                                                       std::size_t optionCount, float pointX,
                                                       float pointY) noexcept;

/// -1 or +1 when an authored HUD point hits a visible page arrow in the header.
[[nodiscard]] std::optional<int> buildPageStepAt(const BuildPanelLayout& layout, float pointX,
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

/// The hover card for one build option: full name, id in the corner, and the facts a player
/// weighs before building — construction material (in the loss colour when it cannot be paid),
/// energy, build time at the selected builder's rate, and what the result can survive. A zero row
/// is omitted rather than printed: zero means the content stated nothing, not a measurement.
[[nodiscard]] InfoCard buildOptionCard(const BuildOption& option, GameProfile profile);

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

/// Cap on one drag's sites: a full-map swipe at minimum spacing would otherwise queue
/// hundreds of dead orders, and repeating the gesture is cheap.
inline constexpr std::size_t kArrayMaxSites = 32;

/// Bounds on the wheel spacing scale, as multiples of the structure diameter. Below
/// half the ghosts merge into one smear; above quadruple the "array" is dots.
inline constexpr float kArraySpacingMinScale = 0.5f;
inline constexpr float kArraySpacingMaxScale = 4.0f;

/// World-space points, so both the ghost row and the release submit read the same
/// answer — a ghost that promises a site the order then refuses is the failure this
/// file's tests exist to catch. Pure arithmetic over the two ground points, like
/// everything else here: snapping, validation and issuing all happen downstream.
///
/// Sites march from `from` toward `to` one `spacingElmos` apart, so the press point
/// always builds and a short drag degrades to the single click it nearly was.
/// Non-positive spacing or a zero cap answers no sites rather than dividing by zero.
[[nodiscard]] std::vector<std::array<float, 2>> arrayBuildCells(std::array<float, 2> from,
    std::array<float, 2> to, float spacingElmos, std::size_t maxSites = kArrayMaxSites);

/// The same march written into a caller-kept buffer: the per-frame ghost row reuses
/// its scratch instead of allocating sixty small vectors a second mid-drag.
void arrayBuildCellsInto(std::array<float, 2> from, std::array<float, 2> to,
    float spacingElmos, std::size_t maxSites, std::vector<std::array<float, 2>>& out);

/// The wheel step for array spacing, as a scale on the structure diameter. Scrolling
/// up tightens toward half-diameter packing, scrolling down loosens toward quadruple —
/// the same direction as the zoom the wheel otherwise drives, so one hand learns one
/// gesture. A still wheel is the identity; the bounds clamp rather than saturate.
[[nodiscard]] float arraySpacingScaleStep(float scale, float wheelPoints) noexcept;

} // namespace rm::ui
