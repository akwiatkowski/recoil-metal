#pragma once

#include "core/bench/FrameStats.hpp"
#include "core/camera/OrbitCamera.hpp"
#include "core/map/TerrainType.hpp"
#include "core/map/GroundSplat.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/model/Model.hpp"
#include "core/model/Pose.hpp"
#include "core/scene/GroundDecals.hpp"
#include "core/scene/Particles.hpp"
#include "core/scene/PropBatch.hpp"
#include "core/scene/UnitBatch.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
#include "core/mesh/ChunkDraws.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <limits>
#include <mutex>
#include <array>
#include <semaphore>
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
class CommandBuffer;
class RenderPassDescriptor;
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

    // Uploads the Supreme Commander ground splat: a base layer covering the whole
    // map with up to eight strata blended over it, weighted per texel by two
    // masks. This is what a .scmap has instead of a baked ground texture — the
    // layer textures are not in the map file at all, they are named paths into
    // the game's archives, and only the masks are embedded.
    //
    // Layer 0 is the base; layers 1-8 take their weight from maskA's r,g,b,a then
    // maskB's r,g,b,a, in that order. Absent layers are skipped rather than
    // blended as black, because a map that names four strata still ships masks
    // whose unused channels are not reliably zero.
    //
    // Replaces the ground texture set by either setGroundTexture or
    // setGroundColourMap. An empty layer list clears the splat and falls back.
    void setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                  const dds::Texture& maskB);

    // The water plane. Recoil hard-codes it at y = 0 (rts/Map/Ground.h:32),
    // which is the default here; Supreme Commander stores a level per map and 17
    // of its 60 stock maps have no water at all, so both are expressible.
    void setWater(bool enabled, float levelElmos) noexcept;

    // The map's own lighting and water settings, from the blocks a `.scmap`
    // carries for exactly this. Without them every map's sea and sky are
    // identical — a map that declares a green lagoon and a hazy orange horizon
    // renders as stock blue on both counts.
    //
    // Optional: an SMF map has no such blocks, and the defaults are the
    // engine's own shader defaults rather than invented ones.
    struct Environment {
        std::array<float, 3> fogColour{{0.52f, 0.60f, 0.70f}};
        std::array<float, 3> waterSurfaceColour{{0.0f, 0.7f, 1.5f}};
        std::array<float, 3> waterSunColour{{0.80f, 0.47f, 0.33f}};
        float waterColourLerp = 0.3f;
        float waterFresnelBias = 0.1f;
        float waterFresnelPower = 1.5f;
        float waterSkyReflection = 1.5f;
        float waterSunShininess = 50.0f;

        /// How far the surface bends what is under it. water2.fx's own default;
        /// most stock maps state exactly this and a few state more.
        float waterRefractionScale = 0.015f;
    };

    void setEnvironment(const Environment& environment) noexcept;

    // Whether to render the planar reflection.
    //
    // A quality setting rather than a correctness one. The pass renders the
    // whole scene a second time, mirrored, and costs about 0.75 ms of a 1.5 ms
    // frame — half the frame for something the Fresnel term largely hides at
    // the angle an RTS camera actually looks from. Off, the water falls back to
    // the analytic sky, which is what it reflected before the pass existed and
    // is convincing on open sea; what is lost is a cliff standing in its own
    // reflection, which only reads near a shoreline.
    //
    // Switching it off does not free the reflection texture: it is allocated
    // once at startup, and keeping it means the setting can be flipped per
    // frame without reallocating mid-flight.
    void setReflections(bool enabled) noexcept { reflectionsEnabled_ = enabled; }
    [[nodiscard]] bool reflectionsEnabled() const noexcept { return reflectionsEnabled_; }

    // Whether the strata's normal maps perturb the ground's shading.
    //
    // Also a quality setting: it costs nine more texture fetches per terrain
    // fragment, which is the same order as the albedo splat itself. Off, the
    // ground is lit by the heightfield's own slope alone — correct at the scale
    // a height sample can express (8 elmos) and flat below it.
    void setStratumNormals(bool enabled) noexcept { stratumNormalsEnabled_ = enabled; }
    [[nodiscard]] bool stratumNormalsEnabled() const noexcept { return stratumNormalsEnabled_; }

    // Whether the map's scenery is drawn.
    //
    // No longer the expensive setting it was. Props are culled per frame against the
    // draw distance each blueprint states, and drawn at the LEVEL it states, so
    // there is no framing at which they cost much: the busiest stock map's 46 971
    // props measure inside the noise of not drawing them at all, whether zoomed out
    // (everything is past its cutoff) or at a working zoom (almost everything is on
    // coarse geometry). Before either, those framings cost +6.2 and +2.8 ms.
    //
    // Kept as a switch anyway, because comparing two frames is what a switch is
    // for.
    //
    // A toggle rather than a rebuild: the geometry stays uploaded, so this can be
    // flipped per frame.
    void setPropsVisible(bool visible) noexcept { propsVisible_ = visible; }
    [[nodiscard]] bool propsVisible() const noexcept { return propsVisible_; }

    // Whether the water bends what is under it, rather than only absorbing it.
    //
    // THE ONE SETTING THAT DEFAULTS OFF, and the reason is not its cost. It needs
    // a copy of the colour target, because a framebuffer fetch reads one pixel and
    // no other, so it splits the render pass in two around a blit — on a
    // tile-based GPU that means writing the colour and depth attachments out to
    // memory and reading them back, measured at +0.17 ms on aw04 and +0.28 ms on a
    // Supreme Commander sea map. That part works.
    //
    // What does not is the wave field being bent BY. This water's normal comes
    // from two analytic wave trains standing in for the engine's four scrolling
    // normal maps (ADR-018), and a screen-space offset driven by a field that
    // regular draws the field's own lattice over the water: rings tens of pixels
    // across at swell frequency, and a diagonal hatch when driven by a finer
    // ripple instead. Measured at four strengths down to a quarter of the
    // engine's; the pattern survives all of them, because it is not a matter of
    // degree — a sum of two sinusoids has a lattice and moving a sample by it
    // shows the lattice.
    //
    // So it is off until the water's normal comes from the engine's own scrolling
    // textures, which is the piece this was waiting on all along. On, it is
    // faithful to what water2.fx does and useful for judging exactly that.
    //
    // Off, the water still absorbs what is behind it by Beer-Lambert, which is
    // what it did before this existed and is most of the effect.
    void setRefraction(bool enabled) noexcept { refractionEnabled_ = enabled; }
    [[nodiscard]] bool refractionEnabled() const noexcept { return refractionEnabled_; }

    // The rings marking which units are selected, for this frame.
    //
    // Rebuilt and pushed every frame, like instances and for the same reason: a
    // ring follows the unit, and a unit moves. It rides the same frames-in-
    // flight ring, so this must be called between beginFrame and drawFrame.
    //
    // Passing an empty span draws none, and so does not calling this at all:
    // beginFrame forgets the previous frame's rings. That is the opposite of
    // how instances behave, and deliberately — a selection that survived being
    // cleared would be worse than no rings, and a count that outlived its
    // frame would point the draw at another slot's contents.
    //
    // Vertices beyond kMaxDecalVertices are dropped rather than growing the
    // buffer, which cannot be resized while the GPU may be reading it. That is
    // roughly 300 units selected at once.
    void setGroundDecals(std::span<const DecalVertex> vertices) noexcept;

    // This frame's particles — dust behind whatever is moving.
    //
    // Pushed every frame like the decals, and forgotten at beginFrame for the same
    // reason: a count that outlived its frame would point the draw at another
    // slot's contents.
    //
    // Each particle carries where it was born, the velocity it was born with and
    // its age; where it IS now is worked out in the vertex shader. So this uploads
    // a description rather than a state, and the CPU never integrates a position.
    //
    // Particles beyond kMaxParticles are dropped rather than growing the buffer,
    // which cannot be resized while the GPU may be reading it.
    void setParticles(std::span<const Particle> particles) noexcept;

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

    // Uploads the map's scenery: trees, rocks and wrecks, one instanced draw per
    // distinct mesh. Static — a prop never moves, so this is called once and
    // there is no per-frame counterpart to setInstances.
    //
    // Held apart from the units rather than folded into them, for a reason that
    // is about the SIM rather than the GPU: everything in the unit list is
    // ticked, collided and pickable, so a tree in it would be shoved aside by
    // passing infantry and would accept a move order. See PropBatch.hpp.
    //
    // One texture per batch, and its ALPHA IS OPACITY — the shape of a leaf cut
    // out of the quad it is drawn on. Both unit families keep a team-colour mask
    // in an alpha channel somewhere, so this distinction is the difference
    // between a palm tree and a solid green card in the player's colour.
    //
    // Replaces any previously set props. An empty batch list clears them.
    void setProps(std::span<const dds::Texture> textures, std::span<const PropBatch> batches);

    // Opens a frame that is going to push new instance data, blocking until the
    // GPU has finished with the ring slot about to be overwritten.
    //
    // Must be called before any setInstances for a frame, and each call must be
    // followed by exactly one drawFrame — that pairing is what releases the
    // slot again. Window owns the pairing; nothing else should call this.
    //
    // Without the wait this would be a plain data race: instance storage is
    // StorageModeShared, so writing it while the GPU may still be reading last
    // frame's copy tears the transform of whatever unit is being written. It
    // renders as an occasional unit at the origin for a single frame, which is
    // exactly the kind of bug that gets dismissed as a fluke.
    void beginFrame() noexcept;

    // Replaces the instances of one batch for this frame.
    //
    // `batchIndex` indexes the batch list as the CALLER passed it to setUnits,
    // not the internal draw order — setUnits sorts batches by texture pair, so
    // the two disagree and using the wrong one moves the wrong model.
    //
    // Instances beyond the count the batch was uploaded with are dropped: the
    // ring is sized once, and growing it mid-frame would mean allocating on a
    // buffer the GPU is reading.
    //
    // Either push a batch every frame or never push it at all. A batch that is
    // never pushed keeps the instances it was uploaded with, because setUnits
    // seeds every ring slot with them; one pushed intermittently would show a
    // frame from three frames ago whenever it is skipped.
    void setInstances(std::size_t batchIndex, std::span<const UnitInstance> instances) noexcept;

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

    // Where in their animations the units are, in seconds. Wraps per batch at
    // that animation's own duration.
    //
    // Set explicitly for a screenshot or a benchmark, where a scene that moves
    // between runs would be useless. The windowed path advances it itself, so a
    // running app animates without the caller doing anything.
    void setAnimationTime(float seconds) noexcept { animationTime_ = seconds; }
    [[nodiscard]] float animationTime() const noexcept { return animationTime_; }

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
    void releasePropBuffers() noexcept;

    /// Uploads a decoded DDS as a Metal texture, mips and all. Returns a +1
    /// object the caller owns. Throws if the allocation fails.
    [[nodiscard]] MTL::Texture* uploadTexture(const dds::Texture& texture, const char* what);


    // What a scene pass should do differently from the ordinary one.
    //
    // The reflection pass renders the world mirrored in the water plane, from
    // a camera reflected in it, with everything below the surface clipped away
    // — otherwise the seabed appears in the reflection, which is exactly what
    // a mirror cannot show. It also skips the water itself: water reflecting
    // water is either recursive or wrong.
    struct SceneOverride {
        simd_float4x4 viewProjection{};
        float clipBelowY = 0.0f;
        bool skipWater = false;
    };

    MTL::Texture* reflectionColour_ = nullptr;  // owned
    MTL::Texture* reflectionDepth_ = nullptr;   // owned
    MTL::SamplerState* reflectionSampler_ = nullptr;  // owned

    /// Renders the world mirrored in the water plane, for the water to sample.
    void encodeReflectionPass(MTL::CommandBuffer* commandBuffer) noexcept;

    /// Encodes the whole scene — terrain, units, props, rings, then water — into
    /// the given render pass, creating and ending the encoder itself.
    ///
    /// It owns the encoder because it may need TWO: the water's refraction reads
    /// a copy of the colour target, and taking that copy means ending the first
    /// encoder, blitting, and resuming with the attachments loaded back. Callers
    /// hand over a pass descriptor and get a finished pass.
    ///
    /// Shared by the windowed, offscreen-benchmark and screenshot paths so they
    /// cannot drift apart and quietly benchmark different work.
    void encodeScene(MTL::CommandBuffer* commandBuffer, MTL::RenderPassDescriptor* pass,
                     unsigned int width, unsigned int height,
                     const SceneOverride* override = nullptr) noexcept;

    /// The copy of the colour target the water samples to refract, and whether
    /// taking it is worth the cost.
    ///
    /// Reallocated when the viewport changes; a no-op when it has not.
    void ensureSceneColour(unsigned int width, unsigned int height) noexcept;
    MTL::Texture* sceneColour_ = nullptr;  // owned

    CA::MetalLayer* layer_;                    // not owned
    MTL::Device* device_ = nullptr;            // owned
    MTL::CommandQueue* commandQueue_ = nullptr; // owned
    MTL::RenderPipelineState* terrainPipeline_ = nullptr; // owned
    MTL::DepthStencilState* depthState_ = nullptr;        // owned

    MTL::Texture* depthTexture_ = nullptr;     // owned, resized with the layer
    unsigned int depthWidth_ = 0;
    unsigned int depthHeight_ = 0;

    MTL::DepthStencilState* waterDepthState_ = nullptr;   // owned
    MTL::RenderPipelineState* skyPipeline_ = nullptr;     // owned
    MTL::DepthStencilState* skyDepthState_ = nullptr;     // owned
    MTL::RenderPipelineState* waterPipeline_ = nullptr;   // owned

    // The water surface is a grid rather than a quad, because each vertex
    // carries how deep the water is there — which is what lets a shoreline
    // fade out instead of ending in a hard line. Rebuilt whenever the terrain
    // or the water level changes, since both decide the depths.
    MTL::Buffer* waterVertexBuffer_ = nullptr;  // owned
    MTL::Buffer* waterIndexBuffer_ = nullptr;   // owned
    std::size_t waterIndexCount_ = 0;

    /// Ground height at each water-grid corner, sampled once when the terrain
    /// is uploaded. Kept so the surface can be rebuilt when the water LEVEL
    /// changes without holding on to the whole terrain mesh — setWater arrives
    /// after setTerrain, so a mesh built only there would bake a stale level.
    std::vector<float> waterGroundHeights_;

    void cacheWaterGround(const TerrainMesh& mesh);
    void buildWaterMesh();
    void releaseWaterBuffers() noexcept;
    MTL::Texture* groundTexture_ = nullptr;    // owned, null until setGroundTexture
    MTL::SamplerState* groundSampler_ = nullptr; // owned
    MTL::SamplerState* splatSampler_ = nullptr;  // owned; repeats, unlike the above

    // The Supreme Commander ground splat. Every layer slot is bound every frame
    // — an unbound texture in a shader that references one is undefined, so
    // absent layers hold the same 4x4 fallback groundTexture_ does, and their
    // weight is forced to zero by the uniforms rather than by what they sample.
    std::array<MTL::Texture*, kSplatLayers> splatLayers_{};  // owned
    std::array<float, kSplatLayers> splatTileElmos_{};
    std::array<float, kSplatLayers> splatPresent_{};

    // The strata's normal maps, blended by the same masks as the albedo and
    // giving the ground the fine relief a heightfield cannot carry: a
    // heightfield sample is 8 elmos across, so anything smaller than a tank has
    // no geometry to be expressed in.
    //
    // Nine, not ten — the macrotexture has no normal entry in the format.
    std::array<MTL::Texture*, kSplatNormalLayers> splatNormals_{};  // owned
    std::array<float, kSplatNormalLayers> splatNormalTileElmos_{};
    std::array<float, kSplatNormalLayers> splatNormalPresent_{};
    MTL::Texture* splatMaskA_ = nullptr;  // owned
    MTL::Texture* splatMaskB_ = nullptr;  // owned
    bool splatEnabled_ = false;

    void releaseSplat() noexcept;

    MTL::RenderPipelineState* unitPipeline_ = nullptr;  // owned

    // Shadows. One directional light, one map, covering the whole terrain —
    // the sun does not move and an RTS camera looks at the same ground from a
    // fairly constant height, so cascades would buy detail nobody is close
    // enough to see.
    MTL::RenderPipelineState* terrainShadowPipeline_ = nullptr;  // owned
    MTL::RenderPipelineState* unitShadowPipeline_ = nullptr;     // owned
    MTL::Texture* shadowMap_ = nullptr;                          // owned
    MTL::SamplerState* shadowSampler_ = nullptr;                 // owned
    simd_float4x4 lightViewProjection_{};
    bool hasShadows_ = false;

    /// Draws the terrain chunks a predicate accepts, at the detail level their
    /// distance from `detailFrom` earns, merging consecutive survivors into
    /// single draws.
    ///
    /// WHICH draws those work out to is decided by `appendChunkDraws` in core/,
    /// where a draw count can be asserted; this only encodes them. The merge is
    /// not a nicety — without it culling replaces one draw of the terrain with
    /// one per chunk, and whenever the camera keeps them all that is slower than
    /// not culling at all.
    template <typename Predicate>
    void drawTerrainChunks(MTL::RenderCommandEncoder* encoder, Predicate keep,
                           simd_float3 detailFrom, bool varyDetail = true) noexcept;

    /// Scratch space for the above, reused between frames and passes so that
    /// planning the terrain costs no allocation once the capacity has settled.
    std::vector<ChunkDraw> chunkDraws_;

    MTL::RenderPipelineState* particlePipeline_ = nullptr;  // owned
    MTL::DepthStencilState* particleDepthState_ = nullptr;  // owned
    MTL::Buffer* particleBuffer_ = nullptr;                 // owned
    std::size_t particleCount_ = 0;

    /// Recomputes the light's orthographic frame around the loaded terrain.
    void updateLightMatrix() noexcept;

    /// Draws terrain and units into the shadow map, from the sun.
    void encodeShadowPass(MTL::CommandBuffer* commandBuffer) noexcept;

    // One model's uploaded geometry. Held in draw order, so encodeScene walks
    // the vector straight through and rebinds only when the pair changes.
    struct GpuUnitBatch {
        MTL::Buffer* vertexBuffer = nullptr;    // owned
        MTL::Buffer* indexBuffer = nullptr;     // owned
        MTL::Buffer* boneBuffer = nullptr;      // owned
        MTL::Buffer* instanceBuffer = nullptr;  // owned
        std::size_t indexCount = 0;
        std::size_t instanceCount = 0;

        // The instance buffer holds kMaxFramesInFlight consecutive copies of
        // `instanceCapacity` instances — one ring slot per frame that may be in
        // flight. Drawing picks a slot by buffer offset, exactly as animation
        // picks a pose, so a moving scene costs one memcpy and no allocation.
        std::size_t instanceCapacity = 0;
        TexturePair textures;
        /// Which family's channel layout the fragment shader should read.
        bool supremeCommanderShading = false;

        // Animation, baked at upload. boneBuffer holds poseCount consecutive
        // poses of boneStrideBytes each, and stays immutable for the batch's
        // life — so there is no per-frame CPU work and no buffer being written
        // while the GPU may still be reading last frame's copy.
        //
        // The whole buffer is bound and the vertex shader INDEXES it per
        // instance. Binding it at one pose's offset instead would be marginally
        // cheaper and would give every unit in the batch the same clock, which
        // is what made squads walk in lockstep.
        std::size_t poseCount = 1;
        std::size_t boneStrideBytes = 0;
        float duration = 0.0f;  ///< seconds; 0 when the batch does not animate

        /// When set, the renderer's clock contributes nothing and each
        /// instance's phase is the whole answer — see UnitBatch.
        bool animationDrivenByInstance = false;

        /// When set, the diffuse texture's alpha is opacity rather than a
        /// team-colour mask, and the fragment shader cuts the fragment out
        /// instead of tinting it. Props only.
        bool alphaIsOpacity = false;

        /// Beyond this distance from the camera the batch's instances are not
        /// drawn — the blueprint's own cutoff. Props only; a unit is always drawn.
        float drawDistanceElmos = std::numeric_limits<float>::infinity();
    };

    std::vector<GpuUnitBatch> unitBatches_;
    std::vector<MTL::Texture*> unitTextures_;  // owned, indexed by TexturePair

    // Scenery, held apart from the units — see PropBatch.hpp for why props must not
    // be in the list the sim ticks.
    //
    // One GROUP per blueprint, holding its levels and ONE instance buffer they
    // share. The levels' survivors are written into it back to back, finest first,
    // so a level's draw is an offset and a count rather than a buffer of its own —
    // which matters because a prop is drawn at exactly one level, so three buffers
    // would be three times the memory to hold the same instances once.
    struct GpuPropLevel {
        MTL::Buffer* vertexBuffer = nullptr;  // owned
        MTL::Buffer* indexBuffer = nullptr;   // owned
        MTL::Buffer* boneBuffer = nullptr;    // owned
        std::size_t indexCount = 0;
        std::size_t boneStrideBytes = 0;
        int albedo = -1;
        int normals = -1;  ///< the two-channel tangent-space map, or -1
        bool supremeCommanderShading = false;

        /// Drawn at this level while the camera is nearer than this.
        float cutoffElmos = std::numeric_limits<float>::infinity();

        // Rewritten every frame by cullProps: where this level's run starts in the
        // shared instance buffer, and how long it is.
        std::size_t firstInstance = 0;
        std::size_t instanceCount = 0;
    };

    struct GpuPropGroup {
        std::vector<GpuPropLevel> levels;  // finest first

        MTL::Buffer* instanceBuffer = nullptr;  // owned
        std::size_t instanceCapacity = 0;

        /// Every place this prop stands, kept CPU-side to filter from. The list
        /// never changes — props do not move — so this is a source rather than
        /// state to keep in step. 2.2 MB on the busiest stock map.
        std::vector<UnitInstance> instances;
    };

    std::vector<GpuPropGroup> propGroups_;
    std::vector<MTL::Texture*> propTextures_;  // owned, indexed by PropLevel::albedo

    /// Scratch for the survivors and the per-level counts, reused so a frame costs
    /// no allocation.
    std::vector<UnitInstance> visibleProps_;
    std::vector<float> propCutoffs_;
    std::vector<std::size_t> propCounts_;

    /// Picks the props worth drawing and writes them into this frame's slot.
    ///
    /// Once per frame, before anything is encoded — not inside encodeScene, which
    /// runs twice (the reflection pass, then the main one) and would be writing
    /// buffers the GPU may already be reading from the first of them.
    void cullProps() noexcept;

    /// Caller batch index -> index into unitBatches_, or kNoBatch for one that
    /// was skipped at upload (no model, no geometry, no instances). setUnits
    /// reorders batches by texture pair, so this mapping is the only way back
    /// from the index the caller knows to the batch actually drawn.
    static constexpr std::size_t kNoBatch = static_cast<std::size_t>(-1);
    std::vector<std::size_t> batchForSourceIndex_;

    // Which ring slot this frame's instances live in, and how many frames the
    // GPU is allowed to be behind. Starts full: the first kMaxFramesInFlight
    // frames may be opened without waiting for anything.
    std::size_t instanceSlot_ = 0;
    bool frameOpen_ = false;
    std::counting_semaphore<static_cast<std::ptrdiff_t>(kMaxFramesInFlight)> framesInFlight_{
        static_cast<std::ptrdiff_t>(kMaxFramesInFlight)};

    MTL::Buffer* vertexBuffer_ = nullptr;      // owned
    MTL::Buffer* indexBuffer_ = nullptr;       // owned
    std::size_t indexCount_ = 0;

    /// The terrain's chunks, kept so the shadow pass can draw only the ones the
    /// light's box touches instead of the whole two million triangles.
    std::vector<TerrainChunk> terrainChunks_;

    /// World-space half-extent and centre of the light's box, in the light's
    /// own axes — enough to reject a chunk without rebuilding the matrix.
    simd_float3 lightCentre_{};
    float lightExtent_ = 0.0f;

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
    Environment environment_{};
    bool reflectionsEnabled_ = true;
    bool stratumNormalsEnabled_ = true;
    bool propsVisible_ = true;
    bool refractionEnabled_ = false;

    // Selection rings. One buffer with a slot per frame in flight, exactly like
    // the unit instances and for the same reason: the vertices are rewritten
    // every frame, and writing storage the GPU may still be reading tears a
    // ring across two positions.
    //
    // Fixed capacity, allocated once. Growing it would mean freeing a buffer
    // that up to two other frames still reference.
    MTL::RenderPipelineState* decalPipeline_ = nullptr;  // owned
    MTL::DepthStencilState* decalDepthState_ = nullptr;  // owned
    MTL::Buffer* decalBuffer_ = nullptr;                 // owned
    std::size_t decalVertexCount_ = 0;

    OrbitCamera camera_;

    float animationTime_ = 0.0f;
    double lastAnimationTick_ = 0.0;  ///< steady-clock seconds, windowed path only

    // Benchmark state. recorder_ is written from the completion handler, which
    // Metal may invoke on an internal thread — see drawFrame for how that is
    // kept safe.
    mutable std::mutex benchMutex_;
    bench::FrameRecorder recorder_;
    bool recording_ = false;
    double lastFrameStart_ = 0.0;
};

} // namespace rm
