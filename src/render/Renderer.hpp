#pragma once

#include "core/bench/FrameStats.hpp"
#include "core/camera/OrbitCamera.hpp"
#include "core/map/TerrainType.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/model/Model.hpp"
#include "core/scene/UnitBatch.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <mutex>
#include <span>
#include <vector>

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
class RenderCommandEncoder;
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
    // outlive the Renderer. Pass nullptr for a headless renderer — valid for
    // offscreen benchmarking, where there is no window and no display link.
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

    // Uploads an uncompressed ground colour map, the Supreme Commander path's
    // equivalent of the above. A .scmap bakes no ground texture at all — it
    // names strata that live in the game's archives and blends them at runtime —
    // so until that splat shader exists the terrain-type array supplies the
    // colour (core/map/TerrainType.hpp). Same texture slot, same shader; only
    // the pixel format differs, and that lives in the texture object.
    void setGroundColourMap(const ColourImage& image);

    // The water plane. Recoil hard-codes it at y = 0 (rts/Map/Ground.h:32),
    // which is the default here; Supreme Commander stores a level per map and 17
    // of its 60 stock maps have no water at all, so both are expressible.
    void setWater(bool enabled, float levelElmos) noexcept;

    // Uploads several models, the instances to draw each at, and the textures
    // they share. One instanced draw call covers every instance of a model; the
    // bone hierarchy is applied on the GPU by indexing a per-bone offset buffer
    // from the vertex, which is what keeps vertices shareable across instances.
    //
    // Textures are named by index into `textures`, so two models using the same
    // file cost one upload, and the draws are ordered so each texture pair is
    // bound once (core/scene/UnitBatch.hpp).
    //
    // Slot 0 is the diffuse, whose alpha is the team-colour mask; slot 1 is the
    // shading texture — S3O's tex2, and the slot Supreme Commander's `_specTeam`
    // will take. Either may be absent: the shader has a defined fallback for
    // each, flat grey and flat-lit respectively.
    //
    // Replaces any previously set units. An empty batch list clears them.
    void setUnits(std::span<const dds::Texture> textures, std::span<const UnitBatch> batches);

    // The camera is mutated by input handlers on the main thread. That is safe
    // because CAMetalDisplayLink was added to the *main* run loop, so
    // drawFrame runs on the main thread too — there is no render thread to
    // race with. Moving the link to its own thread would require revisiting
    // this.
    [[nodiscard]] OrbitCamera& camera() noexcept { return camera_; }

    // One offscreen frame, read back as BGRA8 pixels.
    //
    // Exists because capturing the app's window is unreliable: with multiple
    // displays and Spaces, a window on an inactive Space cannot be captured by
    // id at all. Rendering to a texture and encoding it sidesteps the window
    // server completely, and doubles as a way to record what a change looks
    // like without interrupting whoever is at the machine.
    struct CapturedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> bgra;  ///< tightly packed, width * height * 4
    };

    [[nodiscard]] CapturedImage renderToImage(unsigned int width, unsigned int height);

    /// Points the camera at a world position from a given distance. Used to look
    /// at units, which are ~2 pixels across when the whole map is framed.
    void focusOn(std::array<float, 3> target, float distance) noexcept;

    // Starts recording per-frame CPU and GPU timings. GPU time comes from the
    // command buffer's own GPUStartTime/GPUEndTime, which is the driver's
    // measurement of the work itself rather than a wall-clock guess around it.
    void beginBenchmark(std::size_t warmupFrames);

    // Unthrottled offscreen benchmark. No window, no display link, hence no
    // vsync — this is the only mode whose CPU numbers describe the renderer
    // rather than the display refresh, and therefore the only one comparable
    // against another engine's frame times.
    //
    // Up to kMaxFramesInFlight frames are kept in flight so the measurement
    // reflects a pipelined frame loop; waiting for each frame to complete would
    // measure latency instead of throughput.
    [[nodiscard]] bench::FrameRecorder runOffscreenBenchmark(unsigned int width,
                                                            unsigned int height,
                                                            std::size_t frames,
                                                            std::size_t warmupFrames);

    /// Frames the GPU may be working on at once during an offscreen run. Three
    /// is the conventional depth: enough to keep the GPU fed, few enough that a
    /// stall shows up in the numbers instead of being absorbed.
    static constexpr std::size_t kMaxFramesInFlight = 3;

    /// Thread-safe copy of the run so far. Metal invokes completion handlers on
    /// its own thread, so callers must not hold a reference into the recorder.
    [[nodiscard]] bench::FrameRecorder benchmarkSnapshot() const;
    [[nodiscard]] std::size_t recordedFrames() const;

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
    void releaseUnitBuffers() noexcept;

    /// Uploads a decoded DDS as a Metal texture, mips and all. Returns a +1
    /// object the caller owns. Throws if the allocation fails.
    [[nodiscard]] MTL::Texture* uploadTexture(const dds::Texture& texture, const char* what);

    /// Encodes the whole scene — terrain then water — into an active encoder.
    /// Shared by the windowed and offscreen paths so they cannot drift apart
    /// and quietly benchmark different work.
    void encodeScene(MTL::RenderCommandEncoder* encoder, unsigned int width,
                     unsigned int height) noexcept;

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

    MTL::RenderPipelineState* unitPipeline_ = nullptr;  // owned

    // One model's uploaded geometry. Held in draw order, so encodeScene walks
    // the vector straight through and rebinds only when the pair changes.
    struct GpuUnitBatch {
        MTL::Buffer* vertexBuffer = nullptr;    // owned
        MTL::Buffer* indexBuffer = nullptr;     // owned
        MTL::Buffer* boneBuffer = nullptr;      // owned
        MTL::Buffer* instanceBuffer = nullptr;  // owned
        std::size_t indexCount = 0;
        std::size_t instanceCount = 0;
        TexturePair textures;
        /// Which family's channel layout the fragment shader should read.
        bool supremeCommanderShading = false;
    };

    std::vector<GpuUnitBatch> unitBatches_;
    std::vector<MTL::Texture*> unitTextures_;  // owned, indexed by TexturePair

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

    // Water plane state. Defaults match Recoil, whose water is a fixed plane at
    // y = 0 that every SMF map shares.
    bool hasWater_ = true;
    float waterLevel_ = 0.0f;

    OrbitCamera camera_;

    // Benchmark state. recorder_ is written from the completion handler, which
    // Metal may invoke on an internal thread — see drawFrame for how that is
    // kept safe.
    mutable std::mutex benchMutex_;
    bench::FrameRecorder recorder_;
    bool recording_ = false;
    double lastFrameStart_ = 0.0;
};

} // namespace rm
