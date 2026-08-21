#include "core/ui/Minimap.hpp"

#include <algorithm>
#include <cmath>

namespace rm::ui {
namespace {

/// A fraction of the SHORTER viewport side, so the panel is the same physical size on a wide
/// monitor as on a square one. A fifth is what Supreme Commander's is at 1080p, measured off a
/// screenshot rather than guessed.
inline constexpr float kMinimapFraction = 0.20f;

/// The largest the panel gets, in points. Without a cap a 5K display gets a minimap the size of
/// a playing card, which is not more useful — it is just further from the units.
inline constexpr float kMinimapMax = 260.0f;

/// The projected map's inset from the panel edge: the border the panel's own bevel needs plus a
/// little, so a unit at the very edge of the map still draws a whole pip inside the glass.
inline constexpr float kMinimapInset = kPad;

/// How the map fits inside the square: the scale, and the letterbox offsets.
struct Fit {
    float scale = 1.0f;   ///< points per elmo
    float offsetX = 0.0f; ///< points, from the panel's inner left edge
    float offsetY = 0.0f;
    float innerX = 0.0f;  ///< the inner area's own origin
    float innerY = 0.0f;
    float span = 0.0f;    ///< the inner area's side
};

[[nodiscard]] Fit fitOf(const MinimapLayout& layout, float mapWidthElmos,
                        float mapDepthElmos) noexcept {
    Fit fit;
    fit.span = std::max(0.0f, layout.size - 2.0f * layout.inset);
    fit.innerX = layout.x + layout.inset;
    fit.innerY = layout.y + layout.inset;

    const float width = std::max(1.0f, mapWidthElmos);
    const float depth = std::max(1.0f, mapDepthElmos);

    // ONE scale for both axes, from whichever is the binding constraint. Two scales would fill
    // the square and stretch the map, which makes a diagonal move look like it changes speed.
    fit.scale = std::min(fit.span / width, fit.span / depth);
    fit.offsetX = (fit.span - width * fit.scale) * 0.5f;
    fit.offsetY = (fit.span - depth * fit.scale) * 0.5f;
    return fit;
}

} // namespace

MinimapLayout minimapLayout(float viewportWidth, float viewportHeight) noexcept {
    const float side =
        std::min(kMinimapMax, std::min(viewportWidth, viewportHeight) * kMinimapFraction);
    return MinimapLayout{
        .x = kMargin,
        .y = viewportHeight - kMargin - side,
        .size = side,
        .inset = kMinimapInset,
    };
}

std::array<float, 2> worldToMinimap(const MinimapLayout& layout, float mapWidthElmos,
                                    float mapDepthElmos, float worldX,
                                    float worldZ) noexcept {
    const Fit fit = fitOf(layout, mapWidthElmos, mapDepthElmos);
    return {fit.innerX + fit.offsetX + worldX * fit.scale,
            fit.innerY + fit.offsetY + worldZ * fit.scale};
}

std::array<float, 2> minimapToWorld(const MinimapLayout& layout, float mapWidthElmos,
                                    float mapDepthElmos, float pointX,
                                    float pointY) noexcept {
    const Fit fit = fitOf(layout, mapWidthElmos, mapDepthElmos);
    if (fit.scale <= 0.0f) {
        return {0.0f, 0.0f};
    }
    const float x = (pointX - fit.innerX - fit.offsetX) / fit.scale;
    const float z = (pointY - fit.innerY - fit.offsetY) / fit.scale;

    // CLAMPED to the map. A click a pixel off the edge of a letterboxed map obviously means the
    // edge, and an unclamped answer would send the camera off the world — where `pickGround`
    // finds nothing and the view appears to freeze.
    return {std::clamp(x, 0.0f, std::max(0.0f, mapWidthElmos)),
            std::clamp(z, 0.0f, std::max(0.0f, mapDepthElmos))};
}

bool insideMinimap(const MinimapLayout& layout, float pointX, float pointY) noexcept {
    return pointX >= layout.x && pointX <= layout.x + layout.size && pointY >= layout.y
           && pointY <= layout.y + layout.size;
}

void appendMinimap(Geometry& out, const text::Font& font, const Theme& theme,
                   const MinimapLayout& layout, float mapWidthElmos, float mapDepthElmos,
                   std::span<const MinimapPip> pips,
                   std::span<const std::array<float, 2>> viewCorners, bool filled) {
    if (layout.size <= 0.0f) {
        return;
    }

    appendPanel(out, font, theme, layout.x, layout.y, layout.size, layout.size, filled);

    // THE VIEW OUTLINE FIRST, so a pip inside it is drawn on top. Four segments rather than a
    // filled shape: a filled trapezoid over the panel would dim every pip under it, and the
    // thing being communicated is the boundary.
    if (viewCorners.size() >= 4) {
        const Colour edge = fade(theme.edgeLit, 0.85f);
        for (std::size_t i = 0; i < 4; ++i) {
            const std::array<float, 2> a =
                worldToMinimap(layout, mapWidthElmos, mapDepthElmos, viewCorners[i][0],
                               viewCorners[i][1]);
            const std::array<float, 2> b = worldToMinimap(
                layout, mapWidthElmos, mapDepthElmos, viewCorners[(i + 1) % 4][0],
                viewCorners[(i + 1) % 4][1]);

            // An axis-aligned rectangle per segment, because `appendRect` is what the HUD has
            // and a rotated quad would need a second primitive. A diagonal comes out as its
            // bounding sliver, which for a near-overhead camera — the resting view is 65
            // degrees (`OrbitCamera`) — is within a point or two of the real edge.
            const float left = std::min(a[0], b[0]);
            const float top = std::min(a[1], b[1]);
            const float width = std::max(1.0f, std::abs(b[0] - a[0]));
            const float height = std::max(1.0f, std::abs(b[1] - a[1]));
            text::appendRect(out.label, font, left, top, width, height, edge);
        }
    }

    for (const MinimapPip& pip : pips) {
        const std::array<float, 2> at =
            worldToMinimap(layout, mapWidthElmos, mapDepthElmos, pip.worldX, pip.worldZ);
        // Centred on the position rather than starting at it, so a pip marks where a unit is
        // rather than sitting down and to the right of it.
        const float half = pip.size * 0.5f;
        text::appendRect(out.label, font, at[0] - half, at[1] - half, pip.size, pip.size,
                         pip.colour);
    }
}

} // namespace rm::ui
