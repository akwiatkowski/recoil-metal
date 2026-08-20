#pragma once

#include "core/sim/Army.hpp"
#include "core/text/TextLayout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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
struct Theme {
    Colour glass;    ///< a panel's fill
    Colour well;     ///< a recess inside a panel: a bar's track, a readout's field
    Colour edge;     ///< the bevel around a panel
    Colour edgeLit;  ///< the top edge, and the corner brackets. The one bright line
    Colour label;    ///< silkscreen text. Quieter than kInk, because a label is read once
};

/// The four liveries. See the note above on why three of them are not the faction's own hue.
[[nodiscard]] Theme themeFor(sim::Faction faction) noexcept;

/// The livery for an interface belonging to nobody — a scene with no armies.
[[nodiscard]] Theme neutralTheme() noexcept;

/// A colour at a different opacity. Used constantly — a bar's track is its own fill at a
/// tenth — and worth naming so the intent reads as "the same colour, quieter".
[[nodiscard]] constexpr Colour fade(Colour colour, float alpha) noexcept {
    return Colour{{colour[0], colour[1], colour[2], colour[3] * alpha}};
}

// --- Metrics ----------------------------------------------------------------
//
// One spacing unit, and everything derived from it. A HUD whose paddings are all chosen
// individually looks approximately aligned from a distance and wrong up close.

inline constexpr float kUnit = 6.0f;         ///< the grid everything snaps to
inline constexpr float kMargin = kUnit * 2;  ///< panel to screen edge
inline constexpr float kPad = kUnit * 1.5f;  ///< panel edge to its contents
inline constexpr float kBevel = 1.0f;        ///< the hairline that catches the light

/// How far the corner brackets run along each edge of a panel, in pixels.
///
/// The one ornamental gesture, and it is doing structural work: brackets at the corners say
/// where a panel's bounds are without a full border, which keeps the frame from competing
/// with the readouts inside it. A full box around every panel is what makes a dense interface
/// read as a spreadsheet.
inline constexpr float kBracket = kUnit * 2.5f;

/// The resource panel's size.
inline constexpr float kResourcePanelWidth = 344.0f;

/// A resource's chip: the small square of its own colour that says which row this is.
///
/// The game uses a drawn icon; a chip is the same idea reduced to the part that carries the
/// information. Colour is what a player actually reads at a glance — mass green, energy amber —
/// and an icon would add detail nobody looks at twice.
inline constexpr float kChip = kUnit * 1.5f;

/// Where the label column ends and the readouts begin, measured from the content's left edge.
///
/// A fixed column rather than "after the label", so MASS and ENERGY line their numbers up
/// despite being different lengths. Ragged number columns are the single thing that makes a
/// dense readout look unfinished.
inline constexpr float kLabelColumn = kUnit * 13.0f;

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

/// What the whole interface reports.
struct MatchState {
    Gauge mass;
    Gauge energy;

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

/// The geometry a frame's interface comes to, split by which atlas draws it.
///
/// Two lists because there are two typefaces, and a draw can bind one atlas at a time. The
/// SOLID CHROME rides in `label`, since it needs an opaque texel and either atlas has one.
struct Geometry {
    std::vector<text::TextVertex> label;    ///< condensed face, plus every panel and bar
    std::vector<text::TextVertex> readout;  ///< monospaced face: the numbers

    void clear() noexcept {
        label.clear();
        readout.clear();
    }

    [[nodiscard]] bool empty() const noexcept { return label.empty() && readout.empty(); }
};

/// Draws a panel: glass, a bevel, and corner brackets.
void appendPanel(Geometry& out, const text::Font& font, const Theme& theme, float x, float y,
                 float width, float height);

/// Builds the whole interface for one frame.
///
/// `labelFont` is the condensed face and `readoutFont` the monospaced one; either being
/// unusable degrades that part to nothing rather than failing, so a missing font costs the
/// labels and keeps the numbers.
void build(Geometry& out, const text::Font& labelFont, const text::Font& readoutFont,
           const Theme& theme, const MatchState& state, float viewportWidth,
           float viewportHeight);

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
