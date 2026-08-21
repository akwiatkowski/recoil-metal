#pragma once

#include "core/text/TextLayout.hpp"
#include "core/ui/Hud.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace rm::ui {

// The map, small, in the corner.
//
// WHY IT IS CHEAP NOW AND WAS NOT BEFORE (PLAN2.md §7 P7.4). §7 calls it "nearly free once a
// `Map` object and a snapshot exist; today there is nothing to read from", and that was exactly
// the problem: the only list of where units were was the renderer's per-batch instance arrays,
// which are float, GPU-shaped and rebuilt per frame. A minimap needed a second walk over game
// state that nothing owned. `sim::Snapshot` (P7.1) is that list.
//
// AND IT NEEDS NO NEW PIPELINE, which is the other half of "nearly free". Everything here is a
// screen-space rectangle, and the HUD already draws those: `text::appendRect` uses the font
// atlas's solid block, so a panel, a pip and a view outline are the same primitive as the
// resource bars. A minimap that had wanted a texture would have wanted the `.scmap`'s embedded
// 256-square preview, a sampler and a pass; that is the version this is deliberately not, and
// the preview is a later item rather than a prerequisite.
//
// WHAT IT DOES NOT DO, so the gap is stated rather than discovered: no terrain image (it is a
// flat panel, not a picture of the map), no fog of war, and no drag. Click to jump, which is
// the one interaction that pays for itself immediately, and which §7's manual check names.

/// Where the minimap sits, in points.
///
/// SQUARE, whatever the map's aspect ratio is, and the projection letterboxes inside it. A panel
/// that changed shape per map would move every other HUD element around it, and both games this
/// engine reads content from keep the minimap a fixed frame.
struct MinimapLayout {
    float x = 0.0f;       ///< left edge, points from the viewport's left
    float y = 0.0f;       ///< top edge, points from the viewport's top
    float size = 0.0f;    ///< width and height
    float inset = 0.0f;   ///< the border between the panel and the projected map
};

/// The default placement: bottom-left, one margin in.
///
/// BOTTOM-LEFT because the resource panel is top-left and the clock top-right (`Hud.cpp`), and
/// because that is where Supreme Commander puts it — the interface this one is modelled on
/// (ADR-028). Sized as a fraction of the SHORTER side, so it stays the same physical size on a
/// wide monitor rather than growing with the width.
[[nodiscard]] MinimapLayout minimapLayout(float viewportWidth, float viewportHeight) noexcept;

/// A world position in elmos, projected into the minimap panel.
///
/// LETTERBOXED: a non-square map keeps its aspect ratio and is centred in the square, so a unit
/// halfway across a wide map is drawn halfway across the projected area rather than halfway
/// across the panel. Getting that wrong is the classic minimap bug — everything lands in
/// roughly the right place and nothing lands in exactly the right one.
[[nodiscard]] std::array<float, 2> worldToMinimap(const MinimapLayout& layout,
                                                 float mapWidthElmos, float mapDepthElmos,
                                                 float worldX, float worldZ) noexcept;

/// The inverse, for a click.
///
/// ROUND-TRIPS with `worldToMinimap`, which is §7 P7.4's stated test — and the reason it is the
/// stated test is that these two are the whole of a minimap's correctness. A pip drawn in the
/// wrong place and a click that goes to the wrong place are the same bug, and it is invisible in
/// a screenshot: everything looks plausible.
///
/// A point outside the projected area still returns a world position; it is clamped to the map,
/// because a click one pixel off the edge of a letterboxed map obviously means the edge.
[[nodiscard]] std::array<float, 2> minimapToWorld(const MinimapLayout& layout,
                                                 float mapWidthElmos, float mapDepthElmos,
                                                 float pointX, float pointY) noexcept;

/// Whether a screen point is on the panel at all.
[[nodiscard]] bool insideMinimap(const MinimapLayout& layout, float pointX,
                                float pointY) noexcept;

/// One thing to mark on the map.
struct MinimapPip {
    float worldX = 0.0f;
    float worldZ = 0.0f;
    Colour colour{};

    /// Points across. Bigger for a commander than for a tank — a minimap's job is to say
    /// where the important things are, and every pip the same size says only "units".
    float size = 2.0f;
};

/// Draws the panel, the pips, and an outline of what the camera can see.
///
/// `viewCorners` are the four ground points the viewport's corners project to, in world elmos,
/// in order — the caller has them from the same ray-to-ground pick a right-click uses, so the
/// outline is exact rather than an estimate from the camera's distance. Fewer than four corners
/// draws no outline, which is what a camera looking at the sky should produce.
void appendMinimap(Geometry& out, const text::Font& font, const Theme& theme,
                   const MinimapLayout& layout, float mapWidthElmos, float mapDepthElmos,
                   std::span<const MinimapPip> pips,
                   std::span<const std::array<float, 2>> viewCorners);

} // namespace rm::ui
