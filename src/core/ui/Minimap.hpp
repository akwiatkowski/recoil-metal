#pragma once

#include "core/text/TextLayout.hpp"
#include "core/ui/Hud.hpp"

#include <array>
#include <cstddef>
#include <optional>
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
// IT NEEDED NO NEW PIPELINE, and then it needed one. Everything drawn HERE is still a
// screen-space rectangle over the font atlas's solid block — panel, pip and view outline are the
// same primitive as the resource bars — and that is what made the first version nearly free.
// This note used to end by calling the `.scmap`'s embedded 256-square preview "a later item
// rather than a prerequisite", and it was exactly that: the later item arrived, and it did want
// a sampler and a second fragment function, because the text shader reads a texture's red
// channel as COVERAGE and would have rendered a photograph as a one-colour silhouette.
//
// The split that came out of it is worth keeping in mind when reading `filled` below: the
// PICTURE is the renderer's (`Renderer::setMinimapImage`, one quad, drawn first), the PANEL is
// this file's, and the panel stops filling its own interior so the picture shows through it.
//
// The renderer puts its existing R8 vision mask over the preview, so terrain and minimap share
// one fog answer. Click and drag both pan the camera through the same inverse projection.

/// Where the minimap sits, in authored HUD points.
///
/// SQUARE, whatever the map's aspect ratio is, and the projection letterboxes inside it. A panel
/// that changed shape per map would move every other HUD element around it, and both games this
/// engine reads content from keep the minimap a fixed frame.
struct MinimapLayout {
    float x = 0.0f;       ///< left edge, authored points from the viewport's left
    float y = 0.0f;       ///< top edge, authored points from the viewport's top
    float size = 0.0f;    ///< width and height
    float inset = 0.0f;   ///< the border between the panel and the projected map
};

/// The default placement: bottom-left, one margin in.
///
/// BOTTOM-LEFT because the resource panel is top-left and the clock top-right (`Hud.cpp`), and
/// because that is where Supreme Commander puts it — the interface this one is modelled on
/// (ADR-028). Its profile chooses a stable authored size, constrained only when safe content is
/// smaller, so a wide monitor does not stretch it.
[[nodiscard]] MinimapLayout minimapLayout(const FrameLayout& frame) noexcept;

/// The one aspect-preserving map projection shared by art, fog, pips, input, and footprint.
struct MinimapProjection {
    Rect content;
    float pointsPerElmo = 0.0f;
};

[[nodiscard]] MinimapProjection minimapProjection(const MinimapLayout& layout,
                                                   float mapWidthElmos,
                                                   float mapDepthElmos) noexcept;

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
/// A point in the fixed panel's letterbox is not a point on the map and therefore misses.
[[nodiscard]] std::optional<std::array<float, 2>>
minimapToWorld(const MinimapLayout& layout, float mapWidthElmos, float mapDepthElmos,
               float pointX, float pointY) noexcept;

/// Whether an authored HUD point is on the panel at all.
[[nodiscard]] bool insideMinimap(const MinimapLayout& layout, float pointX,
                                float pointY) noexcept;

/// One thing to mark on the map.
struct MinimapPip {
    float worldX = 0.0f;
    float worldZ = 0.0f;
    Colour colour{};

    /// Authored HUD points across. Bigger for a commander than for a tank — the map's job is to say
    /// where the important things are, and every pip the same size says only "units".
    float size = 2.0f;
};

/// Draws the panel, the pips, and an outline of what the camera can see.
///
/// `viewCorners` are the four ground points the viewport's corners project to, in world elmos,
/// in order — the caller has them from the same ray-to-ground pick a right-click uses, so the
/// outline is exact rather than an estimate from the camera's distance. Fewer than four corners
/// draws no outline, which is what a camera looking at the sky should produce.
/// `filled` draws the panel's own interior. FALSE when the map's preview thumbnail is being
/// drawn underneath: two things filling one rectangle means the lower one is invisible, and a
/// flat well over a photograph is the wrong one to keep. The bevels, brackets, pips and view
/// outline are drawn either way — they are what makes it a panel rather than a picture.
void appendMinimap(Geometry& out, const text::Font& font, const Theme& theme,
                   const MinimapLayout& layout, float mapWidthElmos, float mapDepthElmos,
                   std::span<const MinimapPip> pips,
                   std::span<const std::array<float, 2>> viewCorners, bool filled = true);

} // namespace rm::ui
