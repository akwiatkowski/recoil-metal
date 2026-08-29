#pragma once

#include <algorithm>
#include <array>

namespace rm::ui {

struct Extent {
    float width = 0.0f;
    float height = 0.0f;
};

/// One module rectangle in a top-left-origin point space: AppKit or authored HUD points.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] float right() const noexcept { return x + width; }
    [[nodiscard]] float bottom() const noexcept { return y + height; }
    [[nodiscard]] bool contains(float pointX, float pointY) const noexcept {
        return pointX >= x && pointX < right() && pointY >= y && pointY < bottom();
    }
};

/// The interface's design resolution, in authored HUD points. Geometry uses this size
/// and magnified instead of subdivided when more screen is available.
inline constexpr float kHudDesignWidth = 1280.0f;
inline constexpr float kHudDesignHeight = 720.0f;
inline constexpr float kMinAutomaticHudScale = 1.0f;
inline constexpr float kMaxAutomaticHudScale = 2.5f;
inline constexpr float kMinUserHudScale = 0.5f;
inline constexpr float kMaxUserHudScale = 3.0f;

/// Scale from authored HUD points to logical output points. The limiting axis keeps a short
/// ultrawide from magnifying the bottom deck beyond its own height and scales undersized captures
/// down without clipping Compact.
[[nodiscard]] inline float hudScale(float viewportWidth, float viewportHeight,
                                    float userScale = 1.0f) noexcept {
    if (viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
        return std::clamp(userScale, kMinUserHudScale, kMaxUserHudScale);
    }
    const float fit =
        std::min(viewportWidth / kHudDesignWidth, viewportHeight / kHudDesignHeight);
    const float requested = std::clamp(fit, kMinAutomaticHudScale, kMaxAutomaticHudScale)
                          * std::clamp(userScale, kMinUserHudScale, kMaxUserHudScale);
    // Magnifying beyond the available fit makes even Compact overlap itself. A window is never
    // smaller than design size, but headless captures may be; those scale Compact down rather
    // than accepting invalid geometry. A preference can enlarge only into room left by the
    // automatic 2.5x cap.
    return std::min(requested, fit);
}

/// The complete conversion contract between AppKit, HUD layout, and Metal output.
///
/// `logicalExtent` and `safeContent` are AppKit logical points. HUD geometry uses authored points
/// derived by dividing by `hudScale()`, while world rendering uses `logicalExtent` for camera aspect
/// and `drawableExtent()` for raster output. Keeping these related values together prevents a
/// resize or display move from updating layout, input, projection, and font rasterization by
/// different scale factors.
struct UiViewport {
    Extent logicalExtent{};
    float backingScale = 1.0f;
    Rect safeContent{};
    float magnification = 1.0f;

    [[nodiscard]] static UiViewport full(float width, float height, float backing = 1.0f,
                                         float user = 1.0f) noexcept {
        return withSafeContent(width, height, backing, {0.0f, 0.0f, width, height}, user);
    }

    [[nodiscard]] static UiViewport withSafeContent(float width, float height, float backing,
                                                    Rect safe, float user = 1.0f) noexcept {
        return UiViewport{
            .logicalExtent = {width, height},
            .backingScale = backing,
            .safeContent = safe,
            .magnification = ui::hudScale(width, height, user),
        };
    }

    /// A viewport already expressed in authored HUD points, for pure layout calculations.
    [[nodiscard]] static UiViewport authored(float width, float height) noexcept {
        return UiViewport{
            .logicalExtent = {width, height},
            .backingScale = 1.0f,
            .safeContent = {0.0f, 0.0f, width, height},
            .magnification = 1.0f,
        };
    }

    [[nodiscard]] float hudScale() const noexcept {
        return magnification > 0.0f ? magnification : 0.01f;
    }

    [[nodiscard]] Extent hudExtent() const noexcept {
        const float scale = hudScale();
        return {logicalExtent.width / scale, logicalExtent.height / scale};
    }

    [[nodiscard]] Rect hudSafeContent() const noexcept {
        const float scale = hudScale();
        return {safeContent.x / scale, safeContent.y / scale, safeContent.width / scale,
                safeContent.height / scale};
    }

    [[nodiscard]] Extent drawableExtent() const noexcept {
        const float scale = std::max(backingScale, 1.0f);
        return {logicalExtent.width * scale, logicalExtent.height * scale};
    }

    /// Pixels used to rasterize one authored HUD point: display backing scale times HUD
    /// magnification, with a 1x floor so a tiny capture downsamples readable glyphs rather than
    /// constructing a sub-pixel atlas. Font metrics are divided by this value back into authored
    /// points.
    [[nodiscard]] float fontRasterScale() const noexcept {
        return std::max(std::max(backingScale, 1.0f) * hudScale(), 1.0f);
    }

    [[nodiscard]] std::array<float, 2> toHud(std::array<float, 2> logical) const noexcept {
        const float scale = hudScale();
        return {logical[0] / scale, logical[1] / scale};
    }

    [[nodiscard]] std::array<float, 2> toLogical(std::array<float, 2> hud) const noexcept {
        const float scale = hudScale();
        return {hud[0] * scale, hud[1] * scale};
    }

    /// Converts an authored HUD point directly to a Metal drawable pixel.
    [[nodiscard]] std::array<float, 2> toDrawable(std::array<float, 2> hud) const noexcept {
        const float scale = std::max(backingScale, 1.0f) * hudScale();
        return {hud[0] * scale, hud[1] * scale};
    }
};

} // namespace rm::ui
