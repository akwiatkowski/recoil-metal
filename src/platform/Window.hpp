#pragma once

#include "core/bench/FrameStats.hpp"
#include "core/map/TerrainType.hpp"
#include "core/map/GroundSplat.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/model/Model.hpp"
#include "core/scene/UnitBatch.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <memory>
#include <array>
#include <span>

namespace rm {

// Owns the NSWindow, its CAMetalLayer, the vsync display link, and the
// Renderer. The pImpl idiom keeps every Objective-C type out of this header:
// the rest of the codebase (and the test target) never includes AppKit.
class Window {
public:
    // width/height are in points, not pixels — on a Retina display the Metal
    // drawable is 2x; the Impl matches contentsScale so we render at full
    // backing resolution (a classic silent half-resolution bug otherwise).
    Window(int width, int height, const char* title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;

    // Uploads terrain for the renderer to draw and frames the camera on it.
    void setTerrain(const TerrainMesh& mesh);

    // Uploads the map ground texture. Optional — see Renderer::setGroundTexture.
    void setGroundTexture(const TileAtlas& atlas);

    // The Supreme Commander ground path and the per-map water plane.
    // See Renderer::setGroundColourMap and Renderer::setWater.
    void setGroundColourMap(const ColourImage& image);
    void setWater(bool enabled, float levelElmos);

    // The Supreme Commander ground splat. See Renderer::setSplat.
    void setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                  const dds::Texture& maskB);

    // Units to draw on the terrain. See Renderer::setUnits.
    void setUnits(std::span<const dds::Texture> textures, std::span<const UnitBatch> batches);

    /// Points the camera at a world position. See Renderer::focusOn.
    void focusOn(std::array<float, 3> target, float distance);

    // Benchmark control. See Renderer::beginBenchmark.
    void beginBenchmark(std::size_t warmupFrames);
    [[nodiscard]] std::size_t recordedFrames() const;
    [[nodiscard]] bench::FrameRecorder benchmarkSnapshot() const;

    void show();

private:
    struct Impl;
    // unique_ptr to incomplete type: the destructor MUST be defined (even if
    // defaulted) in the .mm where Impl is complete, otherwise the inline
    // destructor here can't delete the pointee.
    std::unique_ptr<Impl> impl_;
};

} // namespace rm
