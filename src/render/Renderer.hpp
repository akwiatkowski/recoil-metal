#pragma once

#include "core/camera/OrbitCamera.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/mesh/TerrainMesh.hpp"

// Forward declarations keep Metal types out of the header: anything including
// this file stays pure C++ and test-compilable without frameworks.
namespace CA { class MetalLayer; class MetalDrawable; }
namespace MTL {
class Device;
class CommandQueue;
class RenderPipelineState;
class DepthStencilState;
class Texture;
class Buffer;
class SamplerState;
}

namespace rm {

// Owns every long-lived Metal object — device, command queue, pipelines,
// depth buffer, terrain geometry — and renders frames into the window's
// CAMetalLayer.
//
// Ownership follows Cocoa rules, which metal-cpp exposes raw (no ARC for C++
// objects): newXxx() results are +1 and released in the destructor, in
// reverse acquisition order.
class Renderer {
public:
    // layer is NOT owned; it belongs to the window's content view and must
    // outlive the Renderer.
    explicit Renderer(CA::MetalLayer* layer);
    ~Renderer();

    // GPU resources are unique and the display link holds a raw pointer to
    // this object, so neither copying nor moving is safe.
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Uploads a terrain mesh to the GPU and frames the camera on it. Replaces
    // any previously set terrain. Throws RendererError if a buffer cannot be
    // allocated — that is startup-fatal, unlike a bad map file.
    void setTerrain(const TerrainMesh& mesh);

    // Uploads the map's ground texture. Optional: without it the terrain is
    // shaded by elevation alone, which is also what a map with a missing .smt
    // gets. BC1 blocks are uploaded verbatim — Apple Silicon samples them
    // natively, so there is no decode or transcode step.
    void setGroundTexture(const TileAtlas& atlas);

    // The camera is mutated by input handlers on the main thread. That is safe
    // because CAMetalDisplayLink was added to the *main* run loop, so
    // drawFrame runs on the main thread too — there is no render thread to
    // race with. Moving the link to its own thread would require revisiting
    // this.
    [[nodiscard]] OrbitCamera& camera() noexcept { return camera_; }

    // Renders one frame into drawable. The drawable comes FROM the
    // CAMetalDisplayLinkUpdate — with CAMetalDisplayLink, calling
    // nextDrawable() yourself throws CAMetalLayerInvalidOperation; the link
    // owns the drawable pool (and the frame pacing with it).
    // noexcept: a throw mid-frame would escape into the ObjC display-link
    // callback, which is undefined territory.
    void drawFrame(CA::MetalDrawable* drawable) noexcept;

private:
    // Allocates (or reallocates) the depth attachment to match the drawable.
    // Cheap no-op when the size is unchanged, which is the common case.
    void ensureDepthTexture(unsigned int width, unsigned int height) noexcept;

    void releaseTerrainBuffers() noexcept;

    CA::MetalLayer* layer_;                    // not owned
    MTL::Device* device_ = nullptr;            // owned
    MTL::CommandQueue* commandQueue_ = nullptr; // owned
    MTL::RenderPipelineState* terrainPipeline_ = nullptr; // owned
    MTL::DepthStencilState* depthState_ = nullptr;        // owned

    MTL::Texture* depthTexture_ = nullptr;     // owned, resized with the layer
    unsigned int depthWidth_ = 0;
    unsigned int depthHeight_ = 0;

    MTL::DepthStencilState* waterDepthState_ = nullptr;   // owned
    MTL::RenderPipelineState* waterPipeline_ = nullptr;   // owned
    MTL::Texture* groundTexture_ = nullptr;    // owned, null until setGroundTexture
    MTL::SamplerState* groundSampler_ = nullptr; // owned

    MTL::Buffer* vertexBuffer_ = nullptr;      // owned
    MTL::Buffer* indexBuffer_ = nullptr;       // owned
    std::size_t indexCount_ = 0;

    // Vertical range of the loaded terrain, used to colour it by elevation.
    float terrainMinY_ = 0.0f;
    float terrainMaxY_ = 0.0f;

    // World extent in elmos, used for texture UVs and the water quad.
    float mapWidth_ = 0.0f;
    float mapDepth_ = 0.0f;

    bool hasGroundTexture_ = false;

    OrbitCamera camera_;
};

} // namespace rm
