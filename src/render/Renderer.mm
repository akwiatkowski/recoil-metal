// The PRIVATE_IMPLEMENTATION defines instantiate metal-cpp's inline
// implementations. They must appear in EXACTLY ONE translation unit in the
// whole program — this one (AGENT.md gotchas).
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "render/Renderer.hpp"

#include "core/Error.hpp"

#include <simd/simd.h>

#include <chrono>
#include <mutex>
#include <semaphore>
#include <string>

namespace {

// Uniforms shared by every stage. The layout must match the MSL struct below
// exactly; the static_asserts are the enforcement, because a mismatch produces
// geometry that is subtly wrong rather than absent — much harder to notice.
struct TerrainUniforms {
    simd_float4x4 viewProjection;
    simd_float3 sunDirection;
    float minHeight;
    float maxHeight;
    float mapWidth;
    float mapDepth;
    float hasTexture;  ///< float, not bool: bool packing across the ABI is a trap
};

static_assert(sizeof(TerrainUniforms) == 112, "TerrainUniforms must match the MSL layout");
static_assert(offsetof(TerrainUniforms, sunDirection) == 64, "float3 is 16-byte aligned in MSL");
static_assert(offsetof(TerrainUniforms, minHeight) == 80, "unexpected padding before minHeight");
static_assert(offsetof(TerrainUniforms, hasTexture) == 96, "unexpected padding before hasTexture");

// Buffer/texture binding indices, shared between the C++ and MSL sides.
constexpr NS::UInteger kVertexBufferIndex = 0;
constexpr NS::UInteger kUniformBufferIndex = 1;
constexpr NS::UInteger kGroundTextureIndex = 0;
constexpr NS::UInteger kGroundSamplerIndex = 0;

// Compiled from source at runtime — there is no Xcode on this machine, hence no
// offline `metal` compiler. Metal's runtime compiler ships with macOS itself,
// which also buys shader hot-reload later with no build step.
constexpr const char* kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

// packed_float3 (12 bytes, no padding) matches the C++ TerrainVertex exactly.
// Plain float3 would be 16-byte aligned and would shear the vertex stream.
struct TerrainVertexIn {
    packed_float3 position;
    packed_float3 normal;
};

struct Uniforms {
    float4x4 viewProjection;
    float3 sunDirection;
    float minHeight;
    float maxHeight;
    float mapWidth;
    float mapDepth;
    float hasTexture;
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float height;
};

vertex VertexOut terrainVertex(uint vid [[vertex_id]],
                               const device TerrainVertexIn* vertices [[buffer(0)]],
                               constant Uniforms& u [[buffer(1)]]) {
    const float3 worldPosition = float3(vertices[vid].position);

    VertexOut out;
    out.position = u.viewProjection * float4(worldPosition, 1.0);
    out.normal = float3(vertices[vid].normal);
    // The ground texture covers the map exactly — 1 texel per elmo — so the UV
    // is just the world position normalised by the map extent. No per-vertex UV
    // needs storing, which saves 8 MB on a million-vertex map.
    out.uv = float2(worldPosition.x / u.mapWidth, worldPosition.z / u.mapDepth);
    out.height = worldPosition.y;
    return out;
}

fragment float4 terrainFragment(VertexOut in [[stage_in]],
                                constant Uniforms& u [[buffer(1)]],
                                texture2d<float> ground [[texture(0)]],
                                sampler groundSampler [[sampler(0)]]) {
    // Interpolating unit normals across a triangle does not preserve length.
    const float3 normal = normalize(in.normal);

    // Lambert against a fixed sun.
    const float lambert = saturate(dot(normal, u.sunDirection));

    float3 albedo;
    if (u.hasTexture > 0.5) {
        albedo = ground.sample(groundSampler, in.uv).rgb;
    } else {
        // No .smt: fall back to colouring by elevation so relief still reads.
        const float span = max(u.maxHeight - u.minHeight, 1.0);
        const float t = saturate((in.height - u.minHeight) / span);
        albedo = mix(float3(0.18, 0.34, 0.16), float3(0.66, 0.62, 0.55), t);
    }

    // A little ambient so slopes facing away from the sun stay readable.
    const float ambient = 0.35;
    return float4(albedo * (ambient + lambert * 0.8), 1.0);
}

// --- Water ------------------------------------------------------------------
// Recoil's water is a plane hard-coded at y = 0 (rts/Map/Ground.h:32) — there is
// no dynamic water level. Four vertices generated from the map extents; no
// buffer needed.
struct WaterOut {
    float4 position [[position]];
    float3 world;
};

vertex WaterOut waterVertex(uint vid [[vertex_id]], constant Uniforms& u [[buffer(1)]]) {
    // Triangle strip: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    const float3 world = float3(corner.x * u.mapWidth, 0.0, corner.y * u.mapDepth);

    WaterOut out;
    out.position = u.viewProjection * float4(world, 1.0);
    out.world = world;
    return out;
}

fragment float4 waterFragment(WaterOut in [[stage_in]]) {
    // Flat translucent blue. Depth-based tinting would need the depth buffer as
    // an input attachment; that belongs with the rest of the water treatment in
    // a later milestone, not smuggled in here.
    return float4(0.09, 0.22, 0.38, 0.62);
}
)MSL";

// Sun direction, pointing from the ground *towards* the sun so the fragment
// shader can dot it against the surface normal directly. High and to one side
// so slopes on both axes are differently lit.
const simd_float3 kSunDirection = simd_normalize(simd_make_float3(0.45f, 0.78f, 0.44f));

// Sky colour behind the terrain. Fixed rather than animated: a screenshot is
// this milestone's acceptance criterion and it should be reproducible.
constexpr double kSkyR = 0.09;
constexpr double kSkyG = 0.12;
constexpr double kSkyB = 0.18;

constexpr MTL::PixelFormat kColorFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
constexpr MTL::PixelFormat kDepthFormat = MTL::PixelFormat::PixelFormatDepth32Float;
constexpr MTL::PixelFormat kGroundFormat = MTL::PixelFormat::PixelFormatBC1_RGBA;

// A single BC1 block of mid-grey, used as the always-bound fallback so the
// fragment shader never references an unbound texture (which is undefined even
// on the branch that does not sample it). RGB565 0x8410 in both endpoints with
// all-zero selectors = one flat colour.
constexpr unsigned char kFallbackBlock[8] = {0x10, 0x84, 0x10, 0x84, 0x00, 0x00, 0x00, 0x00};

// Wraps NS::Error text into our exception type. [[noreturn]] so call sites
// read as plain control flow: `if (!x) throwMetalError(...);`.
[[noreturn]] void throwMetalError(const char* what, NS::Error* err) {
    std::string message{what};
    if (err != nullptr && err->localizedDescription() != nullptr) {
        message += ": ";
        message += err->localizedDescription()->utf8String();
    }
    throw rm::RendererError{message};
}

/// Builds a pipeline from two named functions in an already-compiled library.
[[nodiscard]] MTL::RenderPipelineState* makePipeline(MTL::Device* device, MTL::Library* library,
                                                     const char* vertexName,
                                                     const char* fragmentName, bool blend) {
    MTL::Function* vertexFn =
        library->newFunction(NS::String::string(vertexName, NS::UTF8StringEncoding));
    MTL::Function* fragmentFn =
        library->newFunction(NS::String::string(fragmentName, NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    descriptor->setFragmentFunction(fragmentFn);

    MTL::RenderPipelineColorAttachmentDescriptor* color0 =
        descriptor->colorAttachments()->object(0);
    color0->setPixelFormat(kColorFormat);
    if (blend) {
        color0->setBlendingEnabled(true);
        color0->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        color0->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        color0->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        color0->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    }
    // The pipeline must know the depth format or the render pass silently
    // refuses to write depth.
    descriptor->setDepthAttachmentPixelFormat(kDepthFormat);

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = device->newRenderPipelineState(descriptor, &error);

    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (fragmentFn != nullptr) fragmentFn->release();

    if (pipeline == nullptr) {
        throwMetalError("failed to create render pipeline", error);
    }
    return pipeline;
}

} // namespace

namespace rm {

Renderer::Renderer(CA::MetalLayer* layer)
    : layer_{layer}
{
    // MTL::CreateSystemDefaultDevice wraps the C function
    // MTLCreateSystemDefaultDevice, which by Cocoa convention returns a
    // +1 (retained) object → released in ~Renderer.
    device_ = MTL::CreateSystemDefaultDevice();
    if (device_ == nullptr) {
        throw RendererError{"no Metal-capable GPU found"};
    }

    // BC1 is what .smt tiles already are. Apple Silicon supports it natively,
    // so the tiles reach the GPU untouched; checking here means a machine
    // without it fails loudly at startup instead of rendering garbage.
    if (!device_->supportsBCTextureCompression()) {
        throw RendererError{
            "this GPU cannot sample BC1 textures, which is the format Recoil's "
            ".smt tiles are stored in"};
    }

    // Null layer = headless (offscreen benchmarking). Everything else below is
    // layer-independent.
    if (layer_ != nullptr) {
        layer_->setDevice(device_);
        layer_->setPixelFormat(kColorFormat);
    }

    commandQueue_ = device_->newCommandQueue();
    if (commandQueue_ == nullptr) {
        throw RendererError{"failed to create Metal command queue"};
    }

    // --- Pipelines, compiled from source at runtime -------------------------
    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(
        NS::String::string(kShaderSource, NS::UTF8StringEncoding),
        nullptr, // default compile options
        &error);
    if (library == nullptr) {
        throwMetalError("runtime shader compilation failed", error);
    }

    terrainPipeline_ = makePipeline(device_, library, "terrainVertex", "terrainFragment",
                                    /*blend=*/false);
    waterPipeline_ = makePipeline(device_, library, "waterVertex", "waterFragment",
                                  /*blend=*/true);
    library->release();

    // --- Depth state -------------------------------------------------------
    auto* depthDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
    depthDescriptor->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLess);
    depthDescriptor->setDepthWriteEnabled(true);
    depthState_ = device_->newDepthStencilState(depthDescriptor);

    // Water tests against terrain but does not write depth: it is translucent,
    // so writing would occlude anything drawn behind it later.
    depthDescriptor->setDepthWriteEnabled(false);
    waterDepthState_ = device_->newDepthStencilState(depthDescriptor);
    depthDescriptor->release();

    if (depthState_ == nullptr || waterDepthState_ == nullptr) {
        throw RendererError{"failed to create depth-stencil state"};
    }

    // --- Ground sampler ----------------------------------------------------
    auto* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
    samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    samplerDescriptor->setMipFilter(MTL::SamplerMipFilter::SamplerMipFilterLinear);
    // Clamp, not repeat: the atlas covers the map exactly, and repeating would
    // wrap the far edge back onto the near one.
    samplerDescriptor->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    samplerDescriptor->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    samplerDescriptor->setMaxAnisotropy(8);  // terrain is viewed at grazing angles
    groundSampler_ = device_->newSamplerState(samplerDescriptor);
    samplerDescriptor->release();

    if (groundSampler_ == nullptr) {
        throw RendererError{"failed to create ground sampler state"};
    }

    // --- Fallback ground texture -------------------------------------------
    // Always bound, so the shader never names an unbound texture even on the
    // branch that does not sample it.
    MTL::TextureDescriptor* fallback =
        MTL::TextureDescriptor::texture2DDescriptor(kGroundFormat, 4, 4, /*mipmapped=*/false);
    fallback->setUsage(MTL::TextureUsageShaderRead);
    groundTexture_ = device_->newTexture(fallback);
    if (groundTexture_ == nullptr) {
        throw RendererError{"failed to create fallback ground texture"};
    }
    groundTexture_->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, kFallbackBlock,
                                  sizeof(kFallbackBlock));
}

Renderer::~Renderer() {
    // Reverse acquisition order; all are +1 objects from newXxx()/CreateXxx.
    releaseTerrainBuffers();
    if (groundTexture_ != nullptr) groundTexture_->release();
    groundSampler_->release();
    if (depthTexture_ != nullptr) depthTexture_->release();
    waterDepthState_->release();
    depthState_->release();
    waterPipeline_->release();
    terrainPipeline_->release();
    commandQueue_->release();
    device_->release();
}

void Renderer::releaseTerrainBuffers() noexcept {
    if (indexBuffer_ != nullptr) {
        indexBuffer_->release();
        indexBuffer_ = nullptr;
    }
    if (vertexBuffer_ != nullptr) {
        vertexBuffer_->release();
        vertexBuffer_ = nullptr;
    }
    indexCount_ = 0;
}

void Renderer::setTerrain(const TerrainMesh& mesh) {
    releaseTerrainBuffers();

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }

    const std::size_t vertexBytes = mesh.vertices.size() * sizeof(TerrainVertex);
    const std::size_t indexBytes = mesh.indices.size() * sizeof(std::uint32_t);

    // StorageModeShared is the right default on Apple Silicon: CPU and GPU share
    // one physical memory pool, so there is nothing to blit and no private copy
    // to keep coherent.
    vertexBuffer_ = device_->newBuffer(mesh.vertices.data(), vertexBytes,
                                       MTL::ResourceStorageModeShared);
    indexBuffer_ = device_->newBuffer(mesh.indices.data(), indexBytes,
                                      MTL::ResourceStorageModeShared);

    if (vertexBuffer_ == nullptr || indexBuffer_ == nullptr) {
        releaseTerrainBuffers();
        throw RendererError{"failed to allocate terrain vertex/index buffers ("
                            + std::to_string((vertexBytes + indexBytes) / (1024 * 1024))
                            + " MiB)"};
    }

    indexCount_ = mesh.indices.size();
    terrainMinY_ = mesh.minY;
    terrainMaxY_ = mesh.maxY;
    mapWidth_ = mesh.maxX - mesh.minX;
    mapDepth_ = mesh.maxZ - mesh.minZ;

    camera_.frame(simd_make_float3(mesh.minX, mesh.minY, mesh.minZ),
                  simd_make_float3(mesh.maxX, mesh.maxY, mesh.maxZ));

    // The far plane must clear the map diagonal from any orbit position, or the
    // far edge of a large map is clipped away.
    camera_.farZ = camera_.distance * 4.0f + mapWidth_ * 2.0f;
}

void Renderer::setGroundTexture(const TileAtlas& atlas) {
    if (atlas.widthTexels <= 0 || atlas.heightTexels <= 0 || atlas.data.empty()) {
        return;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kGroundFormat, static_cast<NS::UInteger>(atlas.widthTexels),
        static_cast<NS::UInteger>(atlas.heightTexels), /*mipmapped=*/false);
    descriptor->setMipmapLevelCount(static_cast<NS::UInteger>(atlas.mipLevels));
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* texture = device_->newTexture(descriptor);
    if (texture == nullptr) {
        throw RendererError{"failed to allocate the "
                            + std::to_string(atlas.data.size() / (1024 * 1024))
                            + " MiB ground texture"};
    }

    // The .smt supplies four mip levels per tile, so the atlas has four too.
    // Each is uploaded whole — the blocks are already in the layout Metal wants.
    for (int level = 0; level < atlas.mipLevels; ++level) {
        const std::span<const std::byte> mip = atlas.mip(level);
        if (mip.empty()) {
            continue;
        }
        texture->replaceRegion(
            MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(atlas.mipWidth(level)),
                                static_cast<NS::UInteger>(atlas.mipHeight(level))),
            static_cast<NS::UInteger>(level), mip.data(),
            static_cast<NS::UInteger>(atlas.mipBytesPerRow(level)));
    }

    if (groundTexture_ != nullptr) {
        groundTexture_->release();
    }
    groundTexture_ = texture;
    hasGroundTexture_ = true;
}

void Renderer::beginBenchmark(std::size_t warmupFrames) {
    const std::lock_guard lock{benchMutex_};
    recorder_ = bench::FrameRecorder{warmupFrames};
    recording_ = true;
    lastFrameStart_ = 0.0;
}

bench::FrameRecorder Renderer::benchmarkSnapshot() const {
    const std::lock_guard lock{benchMutex_};
    return recorder_;
}

std::size_t Renderer::recordedFrames() const {
    const std::lock_guard lock{benchMutex_};
    return recorder_.recorded();
}

void Renderer::ensureDepthTexture(unsigned int width, unsigned int height) noexcept {
    if (depthTexture_ != nullptr && depthWidth_ == width && depthHeight_ == height) {
        return;
    }

    if (depthTexture_ != nullptr) {
        depthTexture_->release();
        depthTexture_ = nullptr;
    }

    // texture2DDescriptor returns an autoreleased object — the caller's pool
    // (drawFrame's) owns it.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kDepthFormat, width, height, /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    // Private: the depth buffer is never read back by the CPU, so it does not
    // need to live in shared memory.
    descriptor->setStorageMode(MTL::StorageModePrivate);

    depthTexture_ = device_->newTexture(descriptor);
    depthWidth_ = width;
    depthHeight_ = height;
}

bench::FrameRecorder Renderer::runOffscreenBenchmark(unsigned int width, unsigned int height,
                                                     std::size_t frames,
                                                     std::size_t warmupFrames) {
    bench::FrameRecorder recorder{warmupFrames};
    if (indexCount_ == 0 || width == 0 || height == 0 || frames == 0) {
        return recorder;
    }

    NS::AutoreleasePool* setupPool = NS::AutoreleasePool::alloc()->init();

    // Offscreen colour target. Private storage: nothing reads it back, the
    // point is to make the GPU do the work, not to keep the image.
    MTL::TextureDescriptor* colorDescriptor =
        MTL::TextureDescriptor::texture2DDescriptor(kColorFormat, width, height,
                                                    /*mipmapped=*/false);
    colorDescriptor->setUsage(MTL::TextureUsageRenderTarget);
    colorDescriptor->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* colorTarget = device_->newTexture(colorDescriptor);

    ensureDepthTexture(width, height);

    if (colorTarget == nullptr || depthTexture_ == nullptr) {
        if (colorTarget != nullptr) colorTarget->release();
        setupPool->release();
        throw RendererError{"failed to allocate offscreen benchmark targets"};
    }
    setupPool->release();

    // Bound in-flight frames. Without this the loop would queue every frame at
    // once, measuring how fast the CPU can encode rather than how fast the GPU
    // can draw.
    std::counting_semaphore<static_cast<std::ptrdiff_t>(kMaxFramesInFlight)> inFlight{
        static_cast<std::ptrdiff_t>(kMaxFramesInFlight)};

    std::mutex recordMutex;
    double previousStart = 0.0;

    const auto nowSeconds = [] {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    for (std::size_t frame = 0; frame < frames; ++frame) {
        inFlight.acquire();

        NS::AutoreleasePool* framePool = NS::AutoreleasePool::alloc()->init();

        const double start = nowSeconds();
        double cpuMs = 0.0;
        if (previousStart > 0.0) {
            cpuMs = (start - previousStart) * 1000.0;
        }
        previousStart = start;

        MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
        MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
        color0->setTexture(colorTarget);
        color0->setLoadAction(MTL::LoadAction::LoadActionClear);
        // DontCare: the image is never presented or read, so on a tile-based GPU
        // this avoids a full-framebuffer writeback that would not happen in the
        // windowed path either.
        color0->setStoreAction(MTL::StoreAction::StoreActionDontCare);
        color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

        MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
        depth->setTexture(depthTexture_);
        depth->setLoadAction(MTL::LoadAction::LoadActionClear);
        depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
        depth->setClearDepth(1.0);

        MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
        encodeScene(encoder, width, height);
        encoder->endEncoding();

        commandBuffer->addCompletedHandler(MTL::HandlerFunction{
            [&recorder, &recordMutex, &inFlight, cpuMs](MTL::CommandBuffer* completed) {
                const double gpuMs =
                    (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                {
                    const std::lock_guard lock{recordMutex};
                    recorder.add(bench::FrameSample{cpuMs, gpuMs});
                }
                inFlight.release();
            }});

        commandBuffer->commit();

        pass->release();
        framePool->release();
    }

    // Drain: reacquire every slot so all completion handlers have run before the
    // recorder is read or the targets are freed.
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        inFlight.acquire();
    }

    colorTarget->release();

    const std::lock_guard lock{recordMutex};
    return recorder;
}

void Renderer::encodeScene(MTL::RenderCommandEncoder* encoder, unsigned int width,
                           unsigned int height) noexcept {
    if (indexCount_ > 0 && height > 0) {
        const float aspect = static_cast<float>(width) / static_cast<float>(height);

        const TerrainUniforms uniforms{
            .viewProjection = camera_.viewProjection(aspect),
            .sunDirection = kSunDirection,
            .minHeight = terrainMinY_,
            .maxHeight = terrainMaxY_,
            .mapWidth = mapWidth_,
            .mapDepth = mapDepth_,
            .hasTexture = hasGroundTexture_ ? 1.0f : 0.0f,
        };

        encoder->setRenderPipelineState(terrainPipeline_);
        encoder->setDepthStencilState(depthState_);
        // Culling stays off: the camera is free to dip below the terrain,
        // and a heightfield seen from underneath should still be visible
        // rather than vanishing. The mesh winding is nonetheless
        // consistent (pinned by tests) so enabling culling later is a
        // one-line change.
        encoder->setCullMode(MTL::CullMode::CullModeNone);
        encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);

        encoder->setVertexBuffer(vertexBuffer_, 0, kVertexBufferIndex);
        // setVertexBytes/setFragmentBytes for a small, per-frame struct:
        // under 4 KB Metal copies it into the command buffer directly, so
        // there is no uniform buffer to allocate or synchronise.
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentTexture(groundTexture_, kGroundTextureIndex);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);

        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(indexCount_),
                                       MTL::IndexType::IndexTypeUInt32,
                                       indexBuffer_,
                                       /*indexBufferOffset=*/0);

        // Water last, so it blends over whatever terrain sits below y = 0.
        // Only worth drawing when some terrain actually is below it.
        if (terrainMinY_ < 0.0f) {
            encoder->setRenderPipelineState(waterPipeline_);
            encoder->setDepthStencilState(waterDepthState_);
            encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip,
                                    NS::UInteger{0}, NS::UInteger{4});
        }
    }
}

void Renderer::drawFrame(CA::MetalDrawable* drawable) noexcept {
    // The drawable is autoreleased by the display link, so every frame needs
    // its own pool or frame objects accumulate until the app exits.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    // Wall time between successive frame starts. With CAMetalDisplayLink this
    // is the display's cadence, not the renderer's throughput — see the comment
    // on the completed handler below.
    double cpuMs = 0.0;
    if (recording_) {
        // steady_clock rather than CACurrentMediaTime: same monotonic guarantee,
        // no extra QuartzCore ObjC header in this translation unit.
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (lastFrameStart_ > 0.0) {
            cpuMs = (now - lastFrameStart_) * 1000.0;
        }
        lastFrameStart_ = now;
    }

    if (drawable != nullptr) {
        MTL::Texture* colorTexture = drawable->texture();
        const auto width = static_cast<unsigned int>(colorTexture->width());
        const auto height = static_cast<unsigned int>(colorTexture->height());
        ensureDepthTexture(width, height);

        MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();

        MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
        color0->setTexture(colorTexture);
        color0->setLoadAction(MTL::LoadAction::LoadActionClear);
        color0->setStoreAction(MTL::StoreAction::StoreActionStore);
        color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

        if (depthTexture_ != nullptr) {
            MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
            depth->setTexture(depthTexture_);
            depth->setLoadAction(MTL::LoadAction::LoadActionClear);
            // DontCare: nothing reads the depth buffer after the pass, and on
            // tile-based GPUs this avoids writing it back to memory at all.
            depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
            depth->setClearDepth(1.0);
        }

        // The encoder exists (and must end) even with zero draw calls: a
        // render pass with LoadActionClear is what performs the clear.
        MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

        encodeScene(encoder, width, height);

        encoder->endEncoding();

        if (recording_ && cpuMs > 0.0) {
            // GPUEndTime - GPUStartTime is the driver's own measurement of the
            // work, so it is unaffected by vsync pacing. That makes it the
            // meaningful number here: the CPU figure is pinned to the display
            // refresh by CAMetalDisplayLink and says nothing about how fast the
            // renderer could go.
            //
            // The handler runs on a Metal-owned thread, hence the mutex. It is
            // taken once per frame, not per draw.
            // Explicit HandlerFunction: a bare lambda is ambiguous between the
            // std::function and ObjC-block overloads.
            commandBuffer->addCompletedHandler(MTL::HandlerFunction{
                [this, cpuMs](MTL::CommandBuffer* completed) {
                    const double gpuMs =
                        (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                    const std::lock_guard lock{benchMutex_};
                    recorder_.add(bench::FrameSample{cpuMs, gpuMs});
                }});
        }

        commandBuffer->presentDrawable(drawable);
        commandBuffer->commit();

        pass->release();
    }

    pool->release();
}

} // namespace rm
