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

#include <string>

namespace {

// Uniforms shared by both shader stages. The layout must match the MSL struct
// below exactly; the static_asserts below are the enforcement, because a
// mismatch here produces geometry that is subtly wrong rather than absent,
// which is much harder to notice.
struct TerrainUniforms {
    simd_float4x4 viewProjection;
    simd_float3 sunDirection;
    float minHeight;
    float maxHeight;
};

static_assert(sizeof(TerrainUniforms) == 96, "TerrainUniforms must match the MSL layout");
static_assert(offsetof(TerrainUniforms, sunDirection) == 64, "float3 is 16-byte aligned in MSL");
static_assert(offsetof(TerrainUniforms, minHeight) == 80, "unexpected padding before minHeight");

// Buffer binding indices, shared between the C++ and MSL sides.
constexpr NS::UInteger kVertexBufferIndex = 0;
constexpr NS::UInteger kUniformBufferIndex = 1;

// Compiled from source at runtime — there is no Xcode on this machine, hence no
// offline `metal` compiler. Metal's runtime compiler ships with macOS itself,
// which also buys shader hot-reload later with no build step.
constexpr const char* kTerrainShaderSource = R"MSL(
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
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float height;
};

vertex VertexOut terrainVertex(uint vid [[vertex_id]],
                               const device TerrainVertexIn* vertices [[buffer(0)]],
                               constant Uniforms& u [[buffer(1)]]) {
    const float3 worldPosition = float3(vertices[vid].position);

    VertexOut out;
    out.position = u.viewProjection * float4(worldPosition, 1.0);
    out.normal = float3(vertices[vid].normal);
    out.height = worldPosition.y;
    return out;
}

fragment float4 terrainFragment(VertexOut in [[stage_in]],
                                constant Uniforms& u [[buffer(1)]]) {
    // Interpolating unit normals across a triangle does not preserve length.
    const float3 normal = normalize(in.normal);

    // Lambert against a fixed sun. Milestone 2 is solid shading only; the SMT
    // tile textures arrive in milestone 3.
    const float lambert = saturate(dot(normal, u.sunDirection));

    // Colour by elevation so relief reads clearly without textures: low ground
    // green, high ground bare rock.
    const float span = max(u.maxHeight - u.minHeight, 1.0);
    const float t = saturate((in.height - u.minHeight) / span);
    const float3 lowland = float3(0.18, 0.34, 0.16);
    const float3 highland = float3(0.66, 0.62, 0.55);
    const float3 albedo = mix(lowland, highland, t);

    // A little ambient so slopes facing away from the sun stay readable.
    const float ambient = 0.28;
    return float4(albedo * (ambient + lambert * 0.85), 1.0);
}
)MSL";

// Sun direction, pointing from the ground *towards* the sun so the fragment
// shader can dot it against the surface normal directly. Chosen high and to one
// side so slopes on both axes are differently lit.
const simd_float3 kSunDirection = simd_normalize(simd_make_float3(0.45f, 0.78f, 0.44f));

// Sky colour behind the terrain. Fixed rather than animated: a screenshot is
// this milestone's acceptance criterion and it should be reproducible.
constexpr double kSkyR = 0.09;
constexpr double kSkyG = 0.12;
constexpr double kSkyB = 0.18;

constexpr MTL::PixelFormat kColorFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
constexpr MTL::PixelFormat kDepthFormat = MTL::PixelFormat::PixelFormatDepth32Float;

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

    layer_->setDevice(device_);
    layer_->setPixelFormat(kColorFormat);

    commandQueue_ = device_->newCommandQueue();
    if (commandQueue_ == nullptr) {
        throw RendererError{"failed to create Metal command queue"};
    }

    // --- Terrain pipeline, compiled from source at runtime ------------------
    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(
        NS::String::string(kTerrainShaderSource, NS::UTF8StringEncoding),
        nullptr, // default compile options
        &error);
    if (library == nullptr) {
        throwMetalError("runtime shader compilation failed", error);
    }

    MTL::Function* vertexFn = library->newFunction(
        NS::String::string("terrainVertex", NS::UTF8StringEncoding));
    MTL::Function* fragmentFn = library->newFunction(
        NS::String::string("terrainFragment", NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    descriptor->setFragmentFunction(fragmentFn);
    descriptor->colorAttachments()->object(0)->setPixelFormat(kColorFormat);
    // The pipeline must know the depth format or the render pass silently
    // refuses to write depth.
    descriptor->setDepthAttachmentPixelFormat(kDepthFormat);

    terrainPipeline_ = device_->newRenderPipelineState(descriptor, &error);

    // The pipeline retains the functions, so every temporary is released now.
    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (fragmentFn != nullptr) fragmentFn->release();
    library->release();

    if (terrainPipeline_ == nullptr) {
        throwMetalError("failed to create terrain render pipeline", error);
    }

    // --- Depth state -------------------------------------------------------
    auto* depthDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
    depthDescriptor->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLess);
    depthDescriptor->setDepthWriteEnabled(true);
    depthState_ = device_->newDepthStencilState(depthDescriptor);
    depthDescriptor->release();

    if (depthState_ == nullptr) {
        throw RendererError{"failed to create depth-stencil state"};
    }
}

Renderer::~Renderer() {
    // Reverse acquisition order; all are +1 objects from newXxx()/CreateXxx.
    releaseTerrainBuffers();
    if (depthTexture_ != nullptr) depthTexture_->release();
    depthState_->release();
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

    camera_.frame(simd_make_float3(mesh.minX, mesh.minY, mesh.minZ),
                  simd_make_float3(mesh.maxX, mesh.maxY, mesh.maxZ));

    // The far plane must clear the map diagonal from any orbit position, or the
    // far edge of a large map is clipped away.
    camera_.farZ = camera_.distance * 4.0f + (mesh.maxX - mesh.minX) * 2.0f;
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

void Renderer::drawFrame(CA::MetalDrawable* drawable) noexcept {
    // The drawable is autoreleased by the display link, so every frame needs
    // its own pool or frame objects accumulate until the app exits.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

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

        if (indexCount_ > 0 && height > 0) {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);

            const TerrainUniforms uniforms{
                .viewProjection = camera_.viewProjection(aspect),
                .sunDirection = kSunDirection,
                .minHeight = terrainMinY_,
                .maxHeight = terrainMaxY_,
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

            encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                           static_cast<NS::UInteger>(indexCount_),
                                           MTL::IndexType::IndexTypeUInt32,
                                           indexBuffer_,
                                           /*indexBufferOffset=*/0);
        }

        encoder->endEncoding();

        commandBuffer->presentDrawable(drawable);
        commandBuffer->commit();

        pass->release();
    }

    pool->release();
}

} // namespace rm
