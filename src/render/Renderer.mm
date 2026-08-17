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

#include <algorithm>
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
    simd_float3 cameraPosition;  ///< eye in world space, for the specular half-vector
    float hasTexture2;           ///< the model's shading texture, if it has one
    float waterLevel;            ///< elmos; Recoil's is always 0, SupCom's is per map
    float supremeCommanderShading;  ///< 1 when the model follows SupCom's channel layout
};

static_assert(sizeof(TerrainUniforms) == 144, "TerrainUniforms must match the MSL layout");
static_assert(offsetof(TerrainUniforms, sunDirection) == 64, "float3 is 16-byte aligned in MSL");
static_assert(offsetof(TerrainUniforms, minHeight) == 80, "unexpected padding before minHeight");
static_assert(offsetof(TerrainUniforms, hasTexture) == 96, "unexpected padding before hasTexture");
static_assert(offsetof(TerrainUniforms, cameraPosition) == 112,
              "float3 realigns to 16 bytes, leaving 12 bytes of pad after hasTexture");
static_assert(offsetof(TerrainUniforms, hasTexture2) == 128, "unexpected padding before hasTexture2");
static_assert(offsetof(TerrainUniforms, waterLevel) == 132, "unexpected padding before waterLevel");
static_assert(offsetof(TerrainUniforms, supremeCommanderShading) == 136,
              "unexpected padding before supremeCommanderShading");

// Buffer/texture binding indices, shared between the C++ and MSL sides.
constexpr NS::UInteger kVertexBufferIndex = 0;
constexpr NS::UInteger kUniformBufferIndex = 1;
constexpr NS::UInteger kInstanceBufferIndex = 2;
constexpr NS::UInteger kBoneBufferIndex = 3;
constexpr NS::UInteger kGroundTextureIndex = 0;
/// The model shading texture — S3O's tex2, and the same slot Supreme Commander's
/// `_specTeam` texture will take.
constexpr NS::UInteger kShadingTextureIndex = 1;
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
    float3 cameraPosition;
    float hasTexture2;
    float waterLevel;
    float supremeCommanderShading;
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
// Recoil's water is a plane hard-coded at y = 0 (rts/Map/Ground.h:32); Supreme
// Commander stores an elevation per map. One uniform covers both. Four vertices
// generated from the map extents; no buffer needed.
struct WaterOut {
    float4 position [[position]];
    float3 world;
};

vertex WaterOut waterVertex(uint vid [[vertex_id]], constant Uniforms& u [[buffer(1)]]) {
    // Triangle strip: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    const float3 world = float3(corner.x * u.mapWidth, u.waterLevel, corner.y * u.mapDepth);

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

// --- Units ------------------------------------------------------------------
// packed_float2 for the UV, NOT float2: float2 is 8-byte aligned and would pad
// this struct to 40 bytes against the C++ ModelVertex's 36, shearing the stream.
struct UnitVertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
    uint boneIndex;
};

struct UnitInstanceIn {
    packed_float3 position;
    float rotationY;
    float scale;
    packed_float4 teamColour;
};

// One bone's contribution: a rotation and a translation, no scale. Matches the
// C++ BoneTransform exactly (32 bytes) — a quaternion rather than a matrix, so
// there is no row/column convention to get backwards between the two languages.
struct BoneTransformIn {
    packed_float4 rotation;     // w, x, y, z
    packed_float3 translation;
    float padding;
};

/// Rotates a vector by a quaternion, the same sandwich the C++ side uses.
static float3 rotateBy(float4 q, float3 v) {
    const float3 u = q.yzw;
    const float3 t = cross(u, v) + q.x * v;
    return v + 2.0 * cross(u, t);
}

struct UnitOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float3 world;             // for the view vector the specular term needs
    float4 teamColour [[flat]];  // per instance, so never interpolated
};

// Recoil's own defaults for model lighting, from mapinfo.lua's `lighting` table
// (rts/Map/MapInfo.cpp:215-220). A map may override all of these; we do not read
// that table yet, so these stand in — and being the engine's defaults, they are
// what a map that says nothing gets there too.
constant float3 kUnitAmbient = float3(0.4);   // unitAmbientColor
constant float3 kUnitDiffuse = float3(0.7);   // unitDiffuseColor
constant float3 kUnitSpecular = float3(0.7);  // unitSpecularColor, defaults to diffuse
constant float kSpecularExponent = 100.0;     // specularExponent (MapInfo.cpp:221)

// Stands in for Recoil's reflection cubemap, which the shading texture's green
// channel mixes into the light (ModelFragProgGL4.glsl:130). We have no env cube
// yet, so a shiny surface reflects a flat sky — the same colour the frame is
// cleared to. Swapping a real cubemap in later changes this one line.
constant float3 kEnvironment = float3(0.09, 0.12, 0.18);

vertex UnitOut unitVertex(uint vid [[vertex_id]],
                          uint iid [[instance_id]],
                          const device UnitVertexIn* vertices [[buffer(0)]],
                          constant Uniforms& u [[buffer(1)]],
                          const device UnitInstanceIn* instances [[buffer(2)]],
                          const device BoneTransformIn* bones [[buffer(3)]]) {
    const UnitVertexIn v = vertices[vid];
    const UnitInstanceIn inst = instances[iid];

    // The bone transform is applied here rather than baked into the vertices, so
    // one vertex buffer serves every instance AND every frame of an animation.
    // At rest this is a translation for Recoil content and the identity for
    // Supreme Commander content, whose vertices are already posed
    // (core/model/Pose.hpp); under animation it is the full rigid transform.
    const BoneTransformIn bone = bones[v.boneIndex];
    const float4 boneRotation = float4(bone.rotation);
    const float3 local = rotateBy(boneRotation, float3(v.position)) + float3(bone.translation);

    const float s = sin(inst.rotationY);
    const float c = cos(inst.rotationY);
    const float3 spun = float3(c * local.x + s * local.z, local.y,
                               -s * local.x + c * local.z);
    const float3 world = spun * inst.scale + float3(inst.position);

    // The normal takes the bone's rotation but not its translation.
    const float3 n = rotateBy(boneRotation, float3(v.normal));
    const float3 spunNormal = float3(c * n.x + s * n.z, n.y, -s * n.x + c * n.z);

    UnitOut out;
    out.position = u.viewProjection * float4(world, 1.0);
    out.normal = spunNormal;
    out.world = world;
    out.teamColour = float4(inst.teamColour);
    // S3O texture coordinates are authored against OpenGL, whose texture origin
    // is bottom-left; Metal's is top-left and the DDS is uploaded unflipped, so
    // V is inverted here. Flipping the compressed blocks instead would mean
    // reordering bits inside every one of them.
    out.uv = float2(v.uv.x, 1.0 - v.uv.y);
    return out;
}

// Ported from Recoil's own model shader (ModelFragProgGL4.glsl:92-131), which is
// the authority on what the two textures mean:
//
//   tex1  rgb = albedo, a = TEAM COLOUR MASK  (1 = fully team-coloured)
//   tex2  r = self-illumination, g = reflectivity/specular strength,
//         a = a one-bit alpha mask, b unused by the forward path
//
// Without tex2 a model is flat-lit and never shines — which is why every BAR
// unit looked like painted cardboard until this landed.
fragment float4 unitFragment(UnitOut in [[stage_in]],
                             constant Uniforms& u [[buffer(1)]],
                             texture2d<float> diffuse [[texture(0)]],
                             texture2d<float> shading [[texture(1)]],
                             sampler texSampler [[sampler(0)]]) {
    // Without a diffuse the model shades flat grey with no team colour, since
    // the mask lives in that texture's alpha and there is nothing to read.
    float4 tex1 = float4(0.62, 0.62, 0.60, 0.0);
    if (u.hasTexture > 0.5) {
        tex1 = diffuse.sample(texSampler, in.uv);
    }

    // Neutral shading texture: no self-illumination, no reflectivity, mask open.
    float4 tex2 = float4(0.0, 0.0, 0.0, 1.0);
    if (u.hasTexture2 > 0.5) {
        tex2 = shading.sample(texSampler, in.uv);
    }

    // Where the team-colour mask lives, and what the shading texture's channels
    // mean, differ between the two content families — see Family in Model.hpp.
    //
    //   Recoil  tex1.a is the mask; tex2.r is self-illumination and tex2.g is
    //           reflectivity (ModelFragProgGL4.glsl:101,129-131).
    //   SupCom  the mask is the alpha of `_SpecTeam`, which is what the name
    //           says and what the data shows — its albedo is frequently DXT1
    //           and carries no usable alpha at all. Its red channel is the
    //           specular strength. Green and blue are left alone: no reference
    //           this project has verified says what they hold, and inventing a
    //           meaning would look like shading rather than read as a bug.
    const bool supCom = u.supremeCommanderShading > 0.5;
    const float teamMask = supCom ? tex2.a : tex1.a;
    const float selfIllum = supCom ? 0.0 : tex2.r;
    const float shininess = supCom ? tex2.r : tex2.g;

    const float3 albedo = mix(tex1.rgb, in.teamColour.rgb, teamMask);

    const float3 N = normalize(in.normal);
    const float3 L = u.sunDirection;
    const float3 V = normalize(u.cameraPosition - in.world);
    const float3 H = normalize(L + V);

    const float NdotL = saturate(dot(N, L));
    const float HdotN = saturate(dot(N, H));

    float3 light = kUnitAmbient + NdotL * kUnitDiffuse;

    // Blinn-Phong at 2.5x the Phong exponent, plus a wide low lobe — the exact
    // expression the engine uses, comment and all.
    float3 specular = kUnitSpecular * min(pow(HdotN, 2.5 * kSpecularExponent)
                                              + 0.3 * pow(HdotN, 2.0 * 3.0),
                                          1.0);
    specular *= shininess * 4.0;

    light = mix(light, kEnvironment, shininess);  // reflection
    light += float3(selfIllum);                   // self-illum

    // tex2.a is a one-bit mask the engine only *discards* on in its alpha pass,
    // where alphaCtrl is set (ModelFragProgGL4.glsl:62-70,97). The default
    // control always passes, so an opaque pass — which is all we have — must not
    // discard, or every unit with a masked tex2 loses geometry it should keep.
    return float4(albedo * light + specular, 1.0);
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
    unitPipeline_ = makePipeline(device_, library, "unitVertex", "unitFragment",
                                 /*blend=*/false);
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
    releaseUnitBuffers();  // frees the unit textures too
    unitPipeline_->release();
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

void Renderer::releaseUnitBuffers() noexcept {
    for (GpuUnitBatch& batch : unitBatches_) {
        if (batch.instanceBuffer != nullptr) batch.instanceBuffer->release();
        if (batch.boneBuffer != nullptr)     batch.boneBuffer->release();
        if (batch.indexBuffer != nullptr)    batch.indexBuffer->release();
        if (batch.vertexBuffer != nullptr)   batch.vertexBuffer->release();
    }
    unitBatches_.clear();

    for (MTL::Texture* texture : unitTextures_) {
        if (texture != nullptr) {
            texture->release();
        }
    }
    unitTextures_.clear();
}

void Renderer::setUnits(std::span<const dds::Texture> textures,
                        std::span<const UnitBatch> batches) {
    releaseUnitBuffers();

    if (batches.empty()) {
        return;
    }

    // Textures first: a failed upload here should leave nothing half-built, and
    // the batches below reference these by index.
    unitTextures_.reserve(textures.size());
    for (const dds::Texture& texture : textures) {
        // A texture that failed to load is kept as a null slot rather than
        // renumbering everything after it — the indices in the batches were
        // decided by the caller and must keep meaning what they meant.
        unitTextures_.push_back(texture.data.empty() ? nullptr
                                                     : uploadTexture(texture, "unit"));
    }

    // Upload in the order the draws will run, so encodeScene is a plain walk.
    std::vector<TexturePair> pairs;
    pairs.reserve(batches.size());
    for (const UnitBatch& batch : batches) {
        pairs.push_back(batch.textures);
    }
    const std::vector<std::size_t> order = orderByTexturePair(pairs);

    unitBatches_.reserve(batches.size());
    for (const std::size_t index : order) {
        const UnitBatch& batch = batches[index];
        if (batch.model == nullptr || batch.model->empty() || batch.model->bones.empty()
            || batch.instances.empty()) {
            continue;
        }
        const Model& model = *batch.model;

        // Every pose the batch will ever need, baked now and concatenated into
        // one buffer: the rest pose alone, or one pose per keyframe when an
        // animation is attached. Playing it back is then a buffer offset — no
        // per-frame CPU work, and nothing writes to a buffer the GPU may still
        // be reading from a frame in flight.
        //
        // restPose is where the two families' vertex conventions are reconciled
        // (core/model/Pose.hpp): a translation for Recoil's bone-local vertices,
        // the identity for Supreme Commander's model-space ones.
        std::vector<BoneTransform> poses = restPose(model);
        std::size_t poseCount = 1;
        float duration = 0.0f;

        if (batch.animation != nullptr && !batch.animation->empty()) {
            const std::vector<int> boneMap = mapBonesToAnimation(model, *batch.animation);
            const bool drivesAnything =
                std::any_of(boneMap.begin(), boneMap.end(), [](int i) { return i >= 0; });

            if (drivesAnything) {
                poses.clear();
                poses.reserve(model.bones.size() * batch.animation->frames.size());
                for (const sca::Frame& frame : batch.animation->frames) {
                    const std::vector<BoneTransform> pose =
                        poseAt(model, *batch.animation, boneMap, frame.time);
                    poses.insert(poses.end(), pose.begin(), pose.end());
                }
                poseCount = batch.animation->frames.size();
                duration = batch.animation->duration;
            }
        }

        GpuUnitBatch uploaded;
        uploaded.textures = batch.textures;
        uploaded.supremeCommanderShading = model.family == Family::SupremeCommander;
        uploaded.poseCount = poseCount;
        uploaded.boneStrideBytes = model.bones.size() * sizeof(BoneTransform);
        uploaded.duration = duration;
        uploaded.vertexBuffer =
            device_->newBuffer(model.vertices.data(), model.vertices.size() * sizeof(ModelVertex),
                               MTL::ResourceStorageModeShared);
        uploaded.indexBuffer =
            device_->newBuffer(model.indices.data(), model.indices.size() * sizeof(std::uint32_t),
                               MTL::ResourceStorageModeShared);
        uploaded.boneBuffer = device_->newBuffer(poses.data(), poses.size() * sizeof(BoneTransform),
                                                 MTL::ResourceStorageModeShared);
        uploaded.instanceBuffer =
            device_->newBuffer(batch.instances.data(), batch.instances.size() * sizeof(UnitInstance),
                               MTL::ResourceStorageModeShared);
        uploaded.indexCount = model.indices.size();
        uploaded.instanceCount = batch.instances.size();

        if (uploaded.vertexBuffer == nullptr || uploaded.indexBuffer == nullptr
            || uploaded.boneBuffer == nullptr || uploaded.instanceBuffer == nullptr) {
            // Push what was allocated so releaseUnitBuffers frees it — throwing
            // with buffers stranded in locals would leak them.
            unitBatches_.push_back(uploaded);
            releaseUnitBuffers();
            throw RendererError{"failed to allocate unit buffers for " + model.name};
        }

        unitBatches_.push_back(uploaded);
    }
}

MTL::Texture* Renderer::uploadTexture(const dds::Texture& texture, const char* what) {
    // DXT1/3/5 map one-to-one onto BC1/BC2/BC3, all natively sampleable here, so
    // the payload goes up untouched exactly like the terrain atlas.
    MTL::PixelFormat format = MTL::PixelFormat::PixelFormatBC1_RGBA;
    switch (texture.format) {
        case dds::BlockFormat::Bc1: format = MTL::PixelFormat::PixelFormatBC1_RGBA; break;
        case dds::BlockFormat::Bc2: format = MTL::PixelFormat::PixelFormatBC2_RGBA; break;
        case dds::BlockFormat::Bc3: format = MTL::PixelFormat::PixelFormatBC3_RGBA; break;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        format, static_cast<NS::UInteger>(texture.width),
        static_cast<NS::UInteger>(texture.height), /*mipmapped=*/false);
    descriptor->setMipmapLevelCount(static_cast<NS::UInteger>(texture.mipLevels));
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* created = device_->newTexture(descriptor);
    if (created == nullptr) {
        throw RendererError{std::string{"failed to allocate the "} + what + " texture"};
    }

    for (int level = 0; level < texture.mipLevels; ++level) {
        const std::span<const std::byte> mip = texture.mip(level);
        if (mip.empty()) {
            continue;
        }
        created->replaceRegion(
            MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(texture.mipWidth(level)),
                                static_cast<NS::UInteger>(texture.mipHeight(level))),
            static_cast<NS::UInteger>(level), mip.data(),
            static_cast<NS::UInteger>(texture.mipBytesPerRow(level)));
    }

    return created;
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

void Renderer::setGroundColourMap(const ColourImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.empty()) {
        return;
    }

    // RGBA8 rather than the terrain path's BC1: this image is generated here,
    // not decoded from an asset, and compressing it would cost a block encoder
    // to save memory a terrain-type map does not use much of — a 2048-square map
    // is 16 MiB.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormat::PixelFormatRGBA8Unorm, static_cast<NS::UInteger>(image.width),
        static_cast<NS::UInteger>(image.height), /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* texture = device_->newTexture(descriptor);
    if (texture == nullptr) {
        throw RendererError{"failed to allocate the ground colour map"};
    }

    texture->replaceRegion(MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(image.width),
                                               static_cast<NS::UInteger>(image.height)),
                           0, image.rgba.data(),
                           static_cast<NS::UInteger>(image.width) * 4);

    if (groundTexture_ != nullptr) {
        groundTexture_->release();
    }
    groundTexture_ = texture;
    hasGroundTexture_ = true;
}

std::size_t Renderer::poseOffsetFor(const GpuUnitBatch& batch) const noexcept {
    if (batch.poseCount <= 1 || batch.duration <= 0.0f) {
        return 0;
    }

    // Poses were baked one per keyframe, so playback picks the keyframe whose
    // time has passed rather than interpolating on the GPU. Retail animations
    // key at 30 Hz, which is where the source data's own resolution ends —
    // interpolating between them per frame would cost CPU work every frame to
    // invent detail the file does not contain.
    float phase = std::fmod(animationTime_, batch.duration);
    if (phase < 0.0f) {
        phase += batch.duration;
    }

    const auto frame = static_cast<std::size_t>(
        (phase / batch.duration) * static_cast<float>(batch.poseCount));
    return std::min(frame, batch.poseCount - 1) * batch.boneStrideBytes;
}

void Renderer::setWater(bool enabled, float levelElmos) noexcept {
    hasWater_ = enabled;
    waterLevel_ = levelElmos;
}

Renderer::CapturedImage Renderer::renderToImage(unsigned int width, unsigned int height) {
    CapturedImage image;
    if (width == 0 || height == 0) {
        return image;
    }

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    // Shared storage, unlike the benchmark's private target: this one is read
    // back on the CPU.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kColorFormat, width, height, /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* target = device_->newTexture(descriptor);
    ensureDepthTexture(width, height);

    if (target == nullptr || depthTexture_ == nullptr) {
        if (target != nullptr) target->release();
        pool->release();
        throw RendererError{"failed to allocate an offscreen capture target"};
    }

    MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
    color0->setTexture(target);
    color0->setLoadAction(MTL::LoadAction::LoadActionClear);
    color0->setStoreAction(MTL::StoreAction::StoreActionStore);  // kept, unlike the benchmark
    color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(depthTexture_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
    depth->setClearDepth(1.0);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
    encodeScene(encoder, width, height);
    encoder->endEncoding();

    commandBuffer->commit();
    commandBuffer->waitUntilCompleted();  // a capture wants the result, not throughput

    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    target->getBytes(image.bgra.data(), static_cast<NS::UInteger>(width) * 4,
                     MTL::Region::Make2D(0, 0, width, height), 0);

    pass->release();
    target->release();
    pool->release();

    return image;
}

void Renderer::focusOn(std::array<float, 3> target, float distance) noexcept {
    camera_.target = simd_make_float3(target[0], target[1], target[2]);
    camera_.distance = std::clamp(distance, OrbitCamera::kMinDistance,
                                  OrbitCamera::kMaxDistance);
    // Keep the far plane clear of the map behind the new target.
    camera_.farZ = camera_.distance * 4.0f + mapWidth_ * 2.0f;
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
    if (height == 0) {
        return;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    TerrainUniforms uniforms{
        .viewProjection = camera_.viewProjection(aspect),
        .sunDirection = kSunDirection,
        .minHeight = terrainMinY_,
        .maxHeight = terrainMaxY_,
        .mapWidth = mapWidth_,
        .mapDepth = mapDepth_,
        .hasTexture = hasGroundTexture_ ? 1.0f : 0.0f,
        .cameraPosition = camera_.eye(),
        .hasTexture2 = 0.0f,
        .waterLevel = waterLevel_,
        .supremeCommanderShading = 0.0f,
    };

    // Culling stays off: the camera is free to dip below the terrain, and a
    // heightfield seen from underneath should still be visible rather than
    // vanishing. The mesh winding is nonetheless consistent (pinned by tests)
    // so enabling culling later is a one-line change.
    encoder->setCullMode(MTL::CullMode::CullModeNone);
    encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);

    // --- Terrain -----------------------------------------------------------
    if (indexCount_ > 0) {
        encoder->setRenderPipelineState(terrainPipeline_);
        encoder->setDepthStencilState(depthState_);

        encoder->setVertexBuffer(vertexBuffer_, 0, kVertexBufferIndex);
        // setVertexBytes/setFragmentBytes for a small, per-frame struct: under
        // 4 KB Metal copies it into the command buffer directly, so there is no
        // uniform buffer to allocate or synchronise.
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentTexture(groundTexture_, kGroundTextureIndex);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);

        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(indexCount_),
                                       MTL::IndexType::IndexTypeUInt32,
                                       indexBuffer_,
                                       /*indexBufferOffset=*/0);
    }

    // --- Units -------------------------------------------------------------
    // Deliberately NOT nested inside the terrain branch: a model should be
    // viewable with no map loaded, and the first version of this got that wrong
    // in a way that silently drew nothing at all.
    if (!unitBatches_.empty()) {
        encoder->setRenderPipelineState(unitPipeline_);
        encoder->setDepthStencilState(depthState_);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);

        // The vertex stage reads only the view-projection out of the uniforms,
        // which is the same for every batch, so it is set once here. The
        // fragment stage's copy carries the per-pair hasTexture flags and is
        // re-set whenever the pair changes.
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

        // Impossible pair, so the first batch always binds. The batches arrive
        // grouped by pair (setUnits ordered them), so this rebinds once per
        // distinct pair rather than once per model.
        TexturePair bound{-2, -2};

        const auto textureAt = [this](int index) -> MTL::Texture* {
            if (index < 0 || static_cast<std::size_t>(index) >= unitTextures_.size()) {
                return nullptr;
            }
            return unitTextures_[static_cast<std::size_t>(index)];
        };

        for (const GpuUnitBatch& batch : unitBatches_) {
            MTL::Texture* diffuse = textureAt(batch.textures.diffuse);
            MTL::Texture* shading = textureAt(batch.textures.shading);

            if (!(batch.textures == bound)) {
                bound = batch.textures;

                // Both slots are always bound — referencing an unbound texture
                // is undefined even on a branch that does not sample it — so the
                // fallback block stands in for whichever the model lacks.
                encoder->setFragmentTexture(diffuse != nullptr ? diffuse : groundTexture_,
                                            kGroundTextureIndex);
                encoder->setFragmentTexture(shading != nullptr ? shading : groundTexture_,
                                            kShadingTextureIndex);
            }

            // Per batch, not per pair: which family's channel layout to read is
            // a property of the MODEL, and two models could share a texture pair
            // without sharing a family. Under 4 KB, so Metal copies this into
            // the command buffer inline — there is no buffer to rebind.
            TerrainUniforms unitUniforms = uniforms;
            unitUniforms.hasTexture = diffuse != nullptr ? 1.0f : 0.0f;
            unitUniforms.hasTexture2 = shading != nullptr ? 1.0f : 0.0f;
            unitUniforms.supremeCommanderShading = batch.supremeCommanderShading ? 1.0f : 0.0f;
            encoder->setFragmentBytes(&unitUniforms, sizeof(unitUniforms), kUniformBufferIndex);

            encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
            encoder->setVertexBuffer(batch.instanceBuffer, 0, kInstanceBufferIndex);
            // Playback is this offset and nothing else. The stride is a multiple
            // of 32 bytes, which satisfies the buffer-offset alignment Apple
            // silicon requires (4).
            encoder->setVertexBuffer(batch.boneBuffer,
                                     static_cast<NS::UInteger>(poseOffsetFor(batch)),
                                     kBoneBufferIndex);

            // One call for every instance — the whole point of the instance
            // buffer.
            encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                           static_cast<NS::UInteger>(batch.indexCount),
                                           MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                           /*indexBufferOffset=*/0,
                                           static_cast<NS::UInteger>(batch.instanceCount));
        }
    }

    // --- Water -------------------------------------------------------------
    // Last, so it blends over whatever terrain and units sit below y = 0. Only
    // worth drawing when something actually is below it.
    if (indexCount_ > 0 && hasWater_ && terrainMinY_ < waterLevel_) {
        encoder->setRenderPipelineState(waterPipeline_);
        encoder->setDepthStencilState(waterDepthState_);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangleStrip,
                                NS::UInteger{0}, NS::UInteger{4});
    }
}

void Renderer::drawFrame(CA::MetalDrawable* drawable) noexcept {
    // The drawable is autoreleased by the display link, so every frame needs
    // its own pool or frame objects accumulate until the app exits.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    // Animation advances only here, in the windowed path. Offscreen captures
    // and benchmarks keep whatever time was set explicitly, because a scene
    // that moves between runs cannot be compared to itself.
    {
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (lastAnimationTick_ > 0.0) {
            animationTime_ += static_cast<float>(now - lastAnimationTick_);
        }
        lastAnimationTick_ = now;
    }

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
