#pragma once

#include "core/sim/Army.hpp"
#include "core/text/TextLayout.hpp"
#include "core/ui/GameProfile.hpp"
#include "core/ui/IconAtlas.hpp"
#include "core/ui/UiLayers.hpp"
#include "core/ui/Viewport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::ui {

// The interface, and its whole visual language in one place.
//
// THE IDEA, stated once so the layout below can be judged against it: a resource panel is an
// INSTRUMENT, not a scoreboard. What a player of this game watches is not how much mass they
// have but which way it is going and whether their build is being funded — the economy is a
// flow (core/sim/Economy.hpp) and a stall slows everything by one shared fraction. So the
// panel is built to show rate and throttle first and totals second, which is the reverse of
// what a naive readout does.
//
// Everything here is arithmetic over a state struct, which is why it is in core/ with tests:
// a layout that overlaps, a bar that clamps wrongly, a flow strip that points the wrong way
// are all invisible in a screenshot at a glance and obvious in an assertion.

// --- Palette ----------------------------------------------------------------
//
// Cool instrument dark. Two kinds of colour, and keeping them apart is the whole of the
// scheme:
//
//   FIXED       what a thing IS. Mass is green, energy is amber, a gain is green and a loss is
//               red, in every faction's livery and on every map. These never move, because a
//               player reads them without looking.
//   LIVERY      whose interface this is. The chrome — glass, bevels, the lit edge, the
//               labels — takes the faction's colour, exactly as the game does.
//
// THREE OF THE FOUR FACTION COLOURS COLLIDE with a fixed one: Aeon's green with mass, Cybran's
// red with a loss, Seraphim's gold with energy. The collision has to be resolved one way
// round, and it is the LIVERY that shifts — a resource's colour is an identity a player has
// learned, while a faction's is a coat of paint. So Aeon runs turquoise rather than leaf
// green, Cybran crimson rather than orange-red, and Seraphim pale gold rather than amber. Each
// stays unmistakably its own faction and stops competing with a number.

/// rgba, straight (not premultiplied) — the text pipeline premultiplies in the shader.
using Colour = std::array<float, 4>;

// --- FIXED: what a thing is -------------------------------------------------
inline constexpr Colour kMass{{0.341f, 0.808f, 0.541f, 1.0f}};    ///< #57CE8A
inline constexpr Colour kEnergy{{0.937f, 0.639f, 0.220f, 1.0f}};  ///< #EFA338
inline constexpr Colour kGain{{0.549f, 0.878f, 0.478f, 1.0f}};    ///< #8CE07A
inline constexpr Colour kLoss{{0.894f, 0.341f, 0.239f, 1.0f}};    ///< #E4573D
inline constexpr Colour kWarn{{1.0f, 0.694f, 0.231f, 1.0f}};      ///< a throttled build
inline constexpr Colour kInk{{0.780f, 0.839f, 0.863f, 1.0f}};     ///< #C7D6DC readouts

// --- LIVERY: whose interface this is ----------------------------------------

/// The chrome, in one faction's colours.
/// The game's own chrome, when a profile packs it: the nine `generic_brd` slices in the
/// icon atlas — corners at their native size, edges stretched along their run, the middle
/// stretched both ways. Inactive draws the glass the HUD always drew; active swaps every
/// panel's fill and bevel for the game's art while text and bars ride on top unchanged.
struct PanelSkin {
    bool active = false;
    /// ul, um, ur, l, m, r, ll, lm, lr — atlas UVs and native size in authored HUD points.
    std::array<IconUv, 9> uv{};
    std::array<std::array<float, 2>, 9> size{};
};

struct Theme {
    Colour glass;    ///< a panel's fill
    Colour well;     ///< a recess inside a panel: a bar's track, a readout's field
    Colour edge;     ///< the bevel around a panel
    Colour edgeLit;  ///< the top edge, and the corner brackets. The one bright line
    Colour label;    ///< silkscreen text. Quieter than kInk, because a label is read once

    PanelSkin skin;  ///< the FAF chrome, inactive by default
};

/// The four liveries. See the note above on why three of them are not the faction's own hue.
[[nodiscard]] Theme themeFor(sim::Faction faction) noexcept;

/// The livery for an interface belonging to nobody — a scene with no armies.
[[nodiscard]] Theme neutralTheme() noexcept;

/// Deterministic BAR-family fallback until content-specific faction accents are ingested.
[[nodiscard]] Theme barTheme() noexcept;

/// A colour at a different opacity. Used constantly — a bar's track is its own fill at a
/// tenth — and worth naming so the intent reads as "the same colour, quieter".
[[nodiscard]] constexpr Colour fade(Colour colour, float alpha) noexcept {
    return Colour{{colour[0], colour[1], colour[2], colour[3] * alpha}};
}

// --- Metrics ----------------------------------------------------------------
//
// One spacing unit, and everything derived from it. A HUD whose paddings are all chosen
// individually looks approximately aligned from a distance and wrong up close.

// --- What the interface is willing to say -----------------------------------
//
// TWO SWITCHES, deliberately constants rather than settings, because they are a decision about
// what the interface is FOR rather than a preference. Both are one edit to reverse, which is
// the point of naming them at all. They live here rather than beside the build panel because
// the roster and the info cards obey them too.

/// Whether a build cell's face carries the costs, or the hover card alone does.
///
/// OFF. The face used to carry mass and nothing else, and the note that decision left behind
/// argued that a bare number was the deciding fact and energy beside it would not fit. What it
/// missed is that a cell then carried an icon and a number and NO NAME — so a player looking
/// for the engineer in a factory's tray had six pictures and six numbers and no way to tell
/// which was which. The name is what a menu is for; the numbers are what a hover is for.
///
/// ON restores all three — mass, energy and build time — as a set. Not one of them: half a
/// tray showing two numbers and half showing one reads as a bug rather than a rule, which is
/// the same trap the mass-only version fell into from the other side.
inline constexpr bool kShowCostOnCell = false;

/// Whether blueprint ids appear anywhere a human is meant to read.
///
/// OFF. `UEB0101` is a filename. It belongs in a log, in a command line and in a bug report,
/// and it was being set in the build panel's header, in every info card's corner and on any
/// tile whose blueprint stated no display name — places where the reader wanted "Land
/// Factory". The one exception is content that states no name at all, where the id is the only
/// thing left to print and an anonymous cell would be worse than a part number.
inline constexpr bool kShowBlueprintIds = false;

inline constexpr float kUnit = 6.0f;         ///< the grid everything snaps to
inline constexpr float kMargin = kUnit * 2;  ///< panel to safe-content edge
inline constexpr float kPad = kUnit * 1.5f;  ///< panel edge to its contents
inline constexpr float kBevel = 1.0f;        ///< the hairline that catches the light

enum class HudProfile : std::uint8_t { Compact, Standard, Wide };

/// The universal interface anatomy. Individual modules own their internals; this owns every
/// outer rectangle so drawing, hover, clicks and drag exclusion cannot derive different HUDs.
struct FrameLayout {
    HudProfile profile = HudProfile::Compact;
    Rect viewport;
    Rect safeContent;
    Rect economy;
    Rect match;
    Rect minimap;
    Rect selection;
    Rect build;
    Rect commands;
    Rect battlefield;
    std::size_t buildColumns = 9;
    std::size_t rosterSlots = 5;
};

/// Selects Compact (1280x720), Standard (1600x900), or Wide (2240x1000) from the viewport's
/// safe HUD space. Fixed modules anchor inside the safe rectangle; world overlays continue to
/// project through the full viewport.
[[nodiscard]] FrameLayout frameLayout(const UiViewport& viewport) noexcept;

/// The drop shadow's offset, in authored HUD points — toward the implied light's opposite
/// corner, so a panel sits ON the world rather than in it. Three: enough to separate, not
/// enough to float.
inline constexpr float kShadow = 3.0f;

/// How far the corner brackets run along each edge of a panel, in authored HUD points.
///
/// The one ornamental gesture, and it is doing structural work: brackets at the corners say
/// where a panel's bounds are without a full border, which keeps the frame from competing
/// with the readouts inside it. A full box around every panel is what makes a dense interface
/// read as a spreadsheet.
inline constexpr float kBracket = kUnit * 2.5f;

/// The resource panel's size.
inline constexpr float kResourcePanelWidth = 360.0f;

/// A resource's chip: the small square of its own colour that says which row this is.
///
/// The game uses a drawn icon; a chip is the same idea reduced to the part that carries the
/// information. Colour is what a player actually reads at a glance — mass green, energy amber —
/// and an icon would add detail nobody looks at twice.
inline constexpr float kChip = kUnit * 1.5f;

/// Where the label column ends and the readouts begin, measured from the content's left edge.
///
/// A fixed column rather than "after the label", so MATERIAL (the longest profile name) and
/// ENERGY line their numbers up despite being different lengths. Ragged number columns are the
/// single thing that makes a dense readout look unfinished.
inline constexpr float kLabelColumn = kUnit * 16.0f;

inline constexpr float kGaugeHeight = kUnit * 1.2f;  ///< the storage bar
inline constexpr float kFlowHeight = kUnit * 0.55f;  ///< the flow strip under it
inline constexpr float kFlowGap = 2.0f;              ///< gap between the two, so they read apart

// --- State ------------------------------------------------------------------

/// One resource, as the panel needs to show it.
struct Gauge {
    float stored = 0.0f;
    float capacity = 0.0f;
    float incomePerSecond = 0.0f;
    float drainPerSecond = 0.0f;

    /// 0..1 of capacity, clamped. Zero capacity reads as EMPTY rather than full: an army with
    /// no storage has nowhere to put anything, and a full bar would say the opposite.
    [[nodiscard]] float fill() const noexcept;

    /// Income less drain. The number the panel shows largest.
    [[nodiscard]] float net() const noexcept { return incomePerSecond - drainPerSecond; }

    /// Whether income is being thrown away: full, and still earning. A real mechanic — the
    /// economy discards anything past capacity — and the pressure to spend.
    [[nodiscard]] bool wasting() const noexcept;
};

/// One of the two economy readings the selected game profile presents, in display order.
///
/// This is a closed pair, not arbitrary-resource infrastructure: both supported games have one
/// construction material and energy, while disagreeing only on the first resource's name.
struct ResourceView {
    std::string_view name;
    Gauge gauge;
    Colour tint;
};

using ResourceViews = std::array<ResourceView, 2>;

[[nodiscard]] ResourceViews resourceViews(GameProfile profile, Gauge primary,
                                           Gauge energy) noexcept;

/// What the whole interface reports.
struct MatchState {
    ResourceViews resources = resourceViews(GameProfile::Fa, {}, {});

    /// The share of what construction asked for that was actually paid, 0..1. The stall.
    float fundedFraction = 1.0f;

    std::size_t unitsAlive = 0;
    std::size_t armiesLeft = 0;
    std::size_t armiesTotal = 0;
    float elapsedSeconds = 0.0f;

    enum class Outcome : std::uint8_t { Running, Win, Draw };
    Outcome outcome = Outcome::Running;
    int winningTeam = 0;
};

/// One semantic layer that needs both solid-colour and icon-atlas draws.
struct MixedGeometryLayer {
    std::vector<text::TextVertex> solid;
    std::vector<text::TextVertex> image;

    void clear() noexcept {
        solid.clear();
        image.clear();
    }

    [[nodiscard]] bool empty() const noexcept { return solid.empty() && image.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return solid.size() + image.size(); }
};

/// The geometry for one interface frame, split by semantic compositing order.
struct Geometry {
    MixedGeometryLayer worldOverlay;  ///< strategic images, bars, contacts, selection band
    MixedGeometryLayer panelSurface;  ///< glass/shadows and optional nine-slice art
    std::vector<text::TextVertex> chrome;             ///< bevels, wells, bars, minimap marks
    std::vector<text::TextVertex> icon;               ///< build and roster icon-atlas quads
    std::vector<text::TextVertex> label;              ///< condensed face
    std::vector<text::TextVertex> foregroundReadout;  ///< monospaced values and badges

    void clear() noexcept {
        worldOverlay.clear();
        panelSurface.clear();
        chrome.clear();
        icon.clear();
        label.clear();
        foregroundReadout.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return worldOverlay.empty() && panelSurface.empty() && chrome.empty() && icon.empty()
            && label.empty() && foregroundReadout.empty();
    }

    [[nodiscard]] std::array<std::size_t, kUiLayerCount> submittedVertices() const noexcept {
        return {worldOverlay.size(), panelSurface.size(), chrome.size(), icon.size(), label.size(),
                foregroundReadout.size()};
    }
};

/// Draws a panel: glass, a bevel, and corner brackets.
/// `filled` draws the glass interior. FALSE leaves it transparent so something already drawn
/// there — the map's preview under the minimap — shows through the chrome rather than under it.
void appendPanel(Geometry& out, const text::Font& font, const Theme& theme, float x, float y,
                 float width, float height, bool filled = true);

// --- The info card ----------------------------------------------------------
//
// What HOVER means in this interface: a small panel of facts about the thing under the
// cursor, docked onto the panel that owns it rather than chasing the pointer. A tooltip that
// follows the mouse covers the neighbouring cells — the very things the player is comparing
// against — and jitters with the hand; a card in a fixed slot is read with the same glance
// every time, which is the instrument idea the rest of the HUD is built on.

/// One fact: a label on the left, a value on the right, the value in the colour of what it
/// IS — mass green, energy amber — per the palette's fixed/livery rule.
struct InfoRow {
    std::string label;
    std::string value;
    Colour tint = kInk;
};

/// The card: a title (the display name), the id set quiet in the corner, and the rows.
struct InfoCard {
    std::string title;
    std::string corner;
    std::vector<InfoRow> rows;

    [[nodiscard]] bool empty() const noexcept { return title.empty() && rows.empty(); }
};

/// The card's height for `rowCount` rows at `lineHeight`, so a caller can dock it above the
/// panel it belongs to. One function used by layout and drawing both — a height computed
/// twice is a card that overlaps its owner by the difference.
[[nodiscard]] float infoCardHeight(float lineHeight, std::size_t rowCount) noexcept;

/// Breaks `text` into at most `maxLines` lines that each fit `maxWidth`, in the given face.
///
/// WHY A BUILD CELL NEEDS THIS. A cell is about fifty points across and a unit is called "Mass
/// Extractor"; the choice is between a name that overflows into its neighbour, a name clipped
/// mid-word, and two short lines. Two short lines is the only one of the three a player can
/// read. Breaks on spaces where it can and mid-word where it cannot, because a single
/// unbreakable word is still better shown in part than not at all.
///
/// The last line is truncated with two dots when what remains does not fit — two rather than an
/// ellipsis because the atlas holds ASCII and nothing else (`text::kFirstGlyph`).
[[nodiscard]] std::vector<std::string> wrapToWidth(std::span<const text::Glyph> glyphs,
                                                    std::string_view text, float maxWidth,
                                                    std::size_t maxLines, float scale = 1.0f);

/// Draws the card at (x, y), `width` across.
void appendInfoCard(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
                     const Theme& theme, float x, float y, float width, const InfoCard& card);

/// Draws card content inside a fixed inspector rectangle, clipping its information budget to
/// three fact rows. The rectangle never changes with hover content.
void appendInspector(Geometry& out, const text::Font& labelFont,
                     const text::Font& readoutFont, const Theme& theme, const Rect& rect,
                     const InfoCard& card);

/// Builds the whole interface for one frame.
///
/// `labelFont` is the condensed face and `readoutFont` the monospaced one; either being
/// unusable degrades that part to nothing rather than failing, so a missing font costs the
/// labels and keeps the numbers.
void build(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
           const Theme& theme, const MatchState& state, const FrameLayout& frame);

/// Formats a resource figure the way the panel shows it: thousands as `12.4k`, below that a
/// plain integer.
///
/// Because an energy store runs to five digits and a mass store to three, and a column that
/// changes width as the number grows is the jitter the monospaced face was chosen to avoid.
[[nodiscard]] std::string formatAmount(float value);

/// Formats a rate with an explicit sign: `+46`, `-40`, `0`.
[[nodiscard]] std::string formatRate(float value);

/// Formats elapsed seconds as `M:SS`, or `H:MM:SS` past an hour.
[[nodiscard]] std::string formatClock(float seconds);

} // namespace rm::ui
