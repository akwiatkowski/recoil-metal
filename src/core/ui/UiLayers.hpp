#pragma once

#include "core/text/TextLayout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace rm::ui {

/// Semantic compositing order for one HUD frame, from the battlefield toward the reader.
enum class UiLayer : std::size_t {
    WorldOverlay,
    PanelSurface,
    Chrome,
    Icon,
    Label,
    ForegroundReadout,
    Count,
};

inline constexpr std::size_t kUiLayerCount = static_cast<std::size_t>(UiLayer::Count);

[[nodiscard]] constexpr std::size_t uiLayerIndex(UiLayer layer) noexcept {
    return static_cast<std::size_t>(layer);
}

[[nodiscard]] constexpr std::string_view uiLayerName(UiLayer layer) noexcept {
    constexpr std::array<std::string_view, kUiLayerCount> kNames{
        "world-overlay", "panel-surface", "chrome", "icon", "label", "readout"};
    return kNames[uiLayerIndex(layer)];
}

/// One thousand quads per semantic layer. Separate fixed partitions mean a burst of labels can
/// no longer evict icons or battlefield warnings from the shared upload buffer.
inline constexpr std::size_t kUiLayerVertexCapacity = text::kVerticesPerGlyph * 1000;
inline constexpr std::size_t kUiVerticesPerFrame = kUiLayerVertexCapacity * kUiLayerCount;

struct UiLayerUsage {
    std::size_t submitted = 0;
    std::size_t uploaded = 0;
    std::size_t capacity = kUiLayerVertexCapacity;

    [[nodiscard]] constexpr std::size_t dropped() const noexcept {
        return submitted - uploaded;
    }
};

struct UiCapacityReport {
    std::array<UiLayerUsage, kUiLayerCount> layers{};

    [[nodiscard]] constexpr const UiLayerUsage& operator[](UiLayer layer) const noexcept {
        return layers[uiLayerIndex(layer)];
    }

    [[nodiscard]] constexpr bool droppedAny() const noexcept {
        return std::any_of(layers.begin(), layers.end(), [](const UiLayerUsage& usage) {
            return usage.dropped() > 0;
        });
    }
};

// A mixed layer still owns one fixed partition. The first material gets first use and the
// second receives the quad-aligned remainder. Keeping this arithmetic outside Metal makes
// saturation behavior deterministic and directly testable.
struct UiLayerUpload {
    std::size_t first{};
    std::size_t second{};

    [[nodiscard]] constexpr std::size_t total() const noexcept { return first + second; }
};

[[nodiscard]] constexpr UiLayerUpload
uiLayerUpload(std::size_t firstSubmitted, std::size_t secondSubmitted,
              std::size_t capacity = kUiLayerVertexCapacity) noexcept {
    const std::size_t wholeCapacity =
        capacity / text::kVerticesPerGlyph * text::kVerticesPerGlyph;
    const std::size_t firstWhole =
        firstSubmitted / text::kVerticesPerGlyph * text::kVerticesPerGlyph;
    const std::size_t first = std::min(firstWhole, wholeCapacity);
    const std::size_t remaining = wholeCapacity - first;
    const std::size_t secondWhole =
        secondSubmitted / text::kVerticesPerGlyph * text::kVerticesPerGlyph;
    return {.first = first, .second = std::min(secondWhole, remaining)};
}

/// Plans independent, whole-quad uploads for the six fixed partitions.
[[nodiscard]] constexpr UiCapacityReport
uiCapacityReport(const std::array<std::size_t, kUiLayerCount>& submitted) noexcept {
    UiCapacityReport report;
    for (std::size_t index = 0; index < submitted.size(); ++index) {
        const std::size_t uploaded =
            std::min(submitted[index], kUiLayerVertexCapacity) / text::kVerticesPerGlyph
            * text::kVerticesPerGlyph;
        report.layers[index] = UiLayerUsage{
            .submitted = submitted[index],
            .uploaded = uploaded,
            .capacity = kUiLayerVertexCapacity,
        };
    }
    return report;
}

} // namespace rm::ui
