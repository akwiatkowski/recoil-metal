#include "core/ui/Minimap.hpp"

#include <algorithm>
#include <cmath>

namespace rm::ui {
namespace {

/// The projected map's inset from the panel edge: the border the panel's own bevel needs plus a
/// little, so a unit at the very edge of the map still draws a whole pip inside the glass.
inline constexpr float kMinimapInset = kPad;

void appendLine(std::vector<text::TextVertex>& out, const text::Font& font,
                std::array<float, 2> a, std::array<float, 2> b, float thickness,
                Colour colour) {
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.0f) || !(thickness > 0.0f)) {
        return;
    }
    const float half = thickness * 0.5f;
    const float px = -dy / length * half;
    const float py = dx / length * half;
    const std::array<float, 2> q0{{a[0] + px, a[1] + py}};
    const std::array<float, 2> q1{{b[0] + px, b[1] + py}};
    const std::array<float, 2> q2{{b[0] - px, b[1] - py}};
    const std::array<float, 2> q3{{a[0] - px, a[1] - py}};
    const std::array<float, 2> uv{{font.solidUv[0], font.solidUv[1]}};
    out.push_back({q0, uv, colour});
    out.push_back({q1, uv, colour});
    out.push_back({q2, uv, colour});
    out.push_back({q0, uv, colour});
    out.push_back({q2, uv, colour});
    out.push_back({q3, uv, colour});
}

} // namespace

MinimapLayout minimapLayout(const FrameLayout& frame) noexcept {
    const Rect rect = frame.minimap;
    return MinimapLayout{
        .x = rect.x,
        .y = rect.y,
        .size = rect.width,
        .inset = kMinimapInset,
    };
}

MinimapProjection minimapProjection(const MinimapLayout& layout, float mapWidthElmos,
                                     float mapDepthElmos) noexcept {
    const float span = std::max(0.0f, layout.size - 2.0f * layout.inset);
    if (!(span > 0.0f) || !(mapWidthElmos > 0.0f) || !(mapDepthElmos > 0.0f)) {
        return {};
    }
    const float scale = std::min(span / mapWidthElmos, span / mapDepthElmos);
    const float width = mapWidthElmos * scale;
    const float height = mapDepthElmos * scale;
    return MinimapProjection{
        .content = {layout.x + layout.inset + (span - width) * 0.5f,
                    layout.y + layout.inset + (span - height) * 0.5f, width, height},
        .pointsPerElmo = scale,
    };
}

std::array<float, 2> worldToMinimap(const MinimapLayout& layout, float mapWidthElmos,
                                    float mapDepthElmos, float worldX,
                                    float worldZ) noexcept {
    const MinimapProjection projection =
        minimapProjection(layout, mapWidthElmos, mapDepthElmos);
    return {projection.content.x + worldX * projection.pointsPerElmo,
            projection.content.y + worldZ * projection.pointsPerElmo};
}

std::optional<std::array<float, 2>> minimapToWorld(const MinimapLayout& layout,
                                                   float mapWidthElmos,
                                                   float mapDepthElmos, float pointX,
                                                   float pointY) noexcept {
    const MinimapProjection projection =
        minimapProjection(layout, mapWidthElmos, mapDepthElmos);
    if (!(projection.pointsPerElmo > 0.0f) || pointX < projection.content.x
        || pointX > projection.content.right() || pointY < projection.content.y
        || pointY > projection.content.bottom()) {
        return std::nullopt;
    }
    return std::array<float, 2>{
        std::clamp((pointX - projection.content.x) / projection.pointsPerElmo, 0.0f,
                   mapWidthElmos),
        std::clamp((pointY - projection.content.y) / projection.pointsPerElmo, 0.0f,
                   mapDepthElmos)};
}

bool insideMinimap(const MinimapLayout& layout, float pointX, float pointY) noexcept {
    return pointX >= layout.x && pointX < layout.x + layout.size && pointY >= layout.y
            && pointY < layout.y + layout.size;
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

            appendLine(out.chrome, font, a, b, 1.5f, edge);
        }
    }

    for (const MinimapPip& pip : pips) {
        const std::array<float, 2> at =
            worldToMinimap(layout, mapWidthElmos, mapDepthElmos, pip.worldX, pip.worldZ);
        // Centred on the position rather than starting at it, so a pip marks where a unit is
        // rather than sitting down and to the right of it.
        const float half = pip.size * 0.5f;
        text::appendRect(out.chrome, font, at[0] - half, at[1] - half, pip.size, pip.size,
                         pip.colour);
    }
}

} // namespace rm::ui
