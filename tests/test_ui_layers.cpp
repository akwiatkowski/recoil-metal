#include <catch2/catch_test_macros.hpp>

#include "core/ui/Hud.hpp"
#include "core/ui/UiLayers.hpp"

TEST_CASE("UI layer partitions hold complete quads", "[ui][layers]") {
    STATIC_CHECK(rm::ui::kUiLayerCount == 6);
    STATIC_CHECK(rm::ui::uiLayerIndex(rm::ui::UiLayer::WorldOverlay)
                 < rm::ui::uiLayerIndex(rm::ui::UiLayer::PanelSurface));
    STATIC_CHECK(rm::ui::uiLayerIndex(rm::ui::UiLayer::PanelSurface)
                 < rm::ui::uiLayerIndex(rm::ui::UiLayer::Chrome));
    STATIC_CHECK(rm::ui::uiLayerIndex(rm::ui::UiLayer::Chrome)
                 < rm::ui::uiLayerIndex(rm::ui::UiLayer::Icon));
    STATIC_CHECK(rm::ui::uiLayerIndex(rm::ui::UiLayer::Icon)
                 < rm::ui::uiLayerIndex(rm::ui::UiLayer::Label));
    STATIC_CHECK(rm::ui::uiLayerIndex(rm::ui::UiLayer::Label)
                 < rm::ui::uiLayerIndex(rm::ui::UiLayer::ForegroundReadout));
    STATIC_CHECK(rm::ui::kUiLayerVertexCapacity % rm::text::kVerticesPerGlyph == 0);
    STATIC_CHECK(rm::ui::kUiVerticesPerFrame
                 == rm::ui::kUiLayerVertexCapacity * rm::ui::kUiLayerCount);

    std::array<std::size_t, rm::ui::kUiLayerCount> submitted{};
    submitted[rm::ui::uiLayerIndex(rm::ui::UiLayer::Label)] =
        rm::ui::kUiLayerVertexCapacity;
    const rm::ui::UiCapacityReport report = rm::ui::uiCapacityReport(submitted);

    CHECK(report[rm::ui::UiLayer::Label].uploaded == rm::ui::kUiLayerVertexCapacity);
    CHECK(report[rm::ui::UiLayer::Label].dropped() == 0);
}

TEST_CASE("overflow in one UI layer cannot evict another", "[ui][layers]") {
    std::array<std::size_t, rm::ui::kUiLayerCount> submitted{};
    submitted[rm::ui::uiLayerIndex(rm::ui::UiLayer::Label)] =
        rm::ui::kUiLayerVertexCapacity + rm::text::kVerticesPerGlyph;
    submitted[rm::ui::uiLayerIndex(rm::ui::UiLayer::Icon)] =
        rm::text::kVerticesPerGlyph;
    const rm::ui::UiCapacityReport report = rm::ui::uiCapacityReport(submitted);

    CHECK(report.droppedAny());
    CHECK(report[rm::ui::UiLayer::Label].dropped() == rm::text::kVerticesPerGlyph);
    CHECK(report[rm::ui::UiLayer::Icon].uploaded == rm::text::kVerticesPerGlyph);
    CHECK(report[rm::ui::UiLayer::Icon].dropped() == 0);
}

TEST_CASE("mixed UI streams share one partition without splitting quads", "[ui][layers]") {
    constexpr std::size_t kQuad = rm::text::kVerticesPerGlyph;
    constexpr std::size_t kTwoQuads = kQuad * 2;

    const auto balanced = rm::ui::uiLayerUpload(kQuad, kQuad, kTwoQuads);
    CHECK(balanced.first == kQuad);
    CHECK(balanced.second == kQuad);

    const auto saturated = rm::ui::uiLayerUpload(kTwoQuads, kQuad, kTwoQuads);
    CHECK(saturated.first == kTwoQuads);
    CHECK(saturated.second == 0);

    // A corrupt producer cannot make Metal consume half a quad.
    const auto malformed = rm::ui::uiLayerUpload(kQuad + 1, kQuad - 1, kTwoQuads);
    CHECK(malformed.first == kQuad);
    CHECK(malformed.second == 0);

    const auto malformedCapacity = rm::ui::uiLayerUpload(kQuad, 0, kQuad - 1);
    CHECK(malformedCapacity.total() == 0);
}

TEST_CASE("UI geometry reports and clears every semantic layer", "[ui][layers]") {
    rm::ui::Geometry geometry;
    const rm::text::TextVertex vertex{};
    geometry.worldOverlay.solid.push_back(vertex);
    geometry.worldOverlay.image.push_back(vertex);
    geometry.panelSurface.solid.push_back(vertex);
    geometry.panelSurface.image.push_back(vertex);
    geometry.chrome.push_back(vertex);
    geometry.icon.push_back(vertex);
    geometry.label.push_back(vertex);
    geometry.foregroundReadout.push_back(vertex);

    const std::array<std::size_t, rm::ui::kUiLayerCount> expected{2, 2, 1, 1, 1, 1};
    CHECK(geometry.submittedVertices() == expected);
    CHECK_FALSE(geometry.empty());
    geometry.clear();
    CHECK(geometry.empty());
}
