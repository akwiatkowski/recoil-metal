// The ground, the water and what covers them.
//
// Terrain buffers, the water plane, the ground atlas, the stratum splat, the colour map, and
// the reflection pass the water samples.
//
// ONE CLASS, SEVERAL TRANSLATION UNITS (PLAN2.md §7 P7.3). This file defines `Renderer`'s
// map and water members; it is not a separate object. That distinction is the honest state of the
// split: §7 asks for `MapRenderer`, `UnitRenderer`, `FxRenderer` and `UiRenderer` as Recoil has
// them (`CBaseGroundDrawer`, `CUnitDrawer`, `CProjectileDrawer`, `CMiniMap`), and four objects
// would mean deciding who owns the device, the command queue, the camera, the depth textures
// and the shadow map — every one of which all four need. Splitting the code first and the
// ownership second is the order that keeps every step verifiable: this one is provably
// behaviour-preserving, because it moves definitions between files and changes nothing else.
//
// The metal-cpp `*_PRIVATE_IMPLEMENTATION` defines stay in `Renderer.mm`, which must remain the
// one translation unit that instantiates them (AGENT.md gotchas).

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "render/RendererInternal.hpp"

#include "core/Error.hpp"
#include "core/camera/Frustum.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace rm {

// The uniform layouts, the buffer indices and the small helpers live in
// `RendererInternal.hpp` — see the note there on why they are not in `Renderer.hpp`.
using namespace render_detail;  // NOLINT(google-build-using-namespace)

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
    terrainChunks_ = mesh.chunks;
    terrainMinY_ = mesh.minY;
    terrainMaxY_ = mesh.maxY;
    mapWidth_ = mesh.maxX - mesh.minX;
    mapDepth_ = mesh.maxZ - mesh.minZ;

    camera_.frame(simd_make_float3(mesh.minX, mesh.minY, mesh.minZ),
                  simd_make_float3(mesh.maxX, mesh.maxY, mesh.maxZ));

    // The far plane must clear the map diagonal from any orbit position, or the
    // far edge of a large map is clipped away.
    camera_.farZ = camera_.distance * 4.0f + mapWidth_ * 2.0f;

    // The water surface samples the ground under it, so it is rebuilt with the
    // terrain rather than kept as a fixed quad.
    cacheWaterGround(mesh);
    buildWaterMesh();
}

void Renderer::releaseWaterBuffers() noexcept {
    if (waterIndexBuffer_ != nullptr) waterIndexBuffer_->release();
    if (waterVertexBuffer_ != nullptr) waterVertexBuffer_->release();
    waterIndexBuffer_ = nullptr;
    waterVertexBuffer_ = nullptr;
    waterIndexCount_ = 0;
}

void Renderer::cacheWaterGround(const TerrainMesh& mesh) {
    waterGroundHeights_.clear();
    if (mesh.verticesX <= 1 || mesh.verticesZ <= 1) {
        return;
    }

    waterGroundHeights_.reserve(static_cast<std::size_t>(kWaterSpans + 1)
                                * static_cast<std::size_t>(kWaterSpans + 1));
    for (int z = 0; z <= kWaterSpans; ++z) {
        for (int x = 0; x <= kWaterSpans; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kWaterSpans);
            const float v = static_cast<float>(z) / static_cast<float>(kWaterSpans);

            // Nearest terrain vertex. The water grid is much coarser than the
            // terrain, so interpolating here would only blur a value that is
            // already interpolated across the water triangle.
            const int tx = static_cast<int>(std::lround(u * static_cast<float>(mesh.verticesX - 1)));
            const int tz = static_cast<int>(std::lround(v * static_cast<float>(mesh.verticesZ - 1)));
            waterGroundHeights_.push_back(mesh.heightAt(tx, tz));
        }
    }
}

void Renderer::buildWaterMesh() {
    releaseWaterBuffers();
    if (waterGroundHeights_.empty()) {
        return;
    }

    constexpr int kSpans = kWaterSpans;

    struct WaterVertex {
        std::array<float, 3> position;
        float depth;
    };
    static_assert(sizeof(WaterVertex) == 16, "must match the MSL WaterVertexIn");

    std::vector<WaterVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(kSpans + 1) * static_cast<std::size_t>(kSpans + 1));

    for (int z = 0; z <= kSpans; ++z) {
        for (int x = 0; x <= kSpans; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kSpans);
            const float v = static_cast<float>(z) / static_cast<float>(kSpans);
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(kSpans + 1)
                             + static_cast<std::size_t>(x);

            vertices.push_back(WaterVertex{
                .position = {u * mapWidth_, waterLevel_, v * mapDepth_},
                .depth = waterLevel_ - waterGroundHeights_[index],
            });
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(kSpans) * static_cast<std::size_t>(kSpans) * 6);
    for (int z = 0; z < kSpans; ++z) {
        for (int x = 0; x < kSpans; ++x) {
            const auto row = static_cast<std::uint32_t>(kSpans + 1);
            const auto v00 = static_cast<std::uint32_t>(z) * row + static_cast<std::uint32_t>(x);
            const std::uint32_t v10 = v00 + 1;
            const std::uint32_t v01 = v00 + row;
            const std::uint32_t v11 = v01 + 1;

            indices.insert(indices.end(), {v00, v01, v11, v00, v11, v10});
        }
    }

    waterVertexBuffer_ = device_->newBuffer(vertices.data(), vertices.size() * sizeof(WaterVertex),
                                            MTL::ResourceStorageModeShared);
    waterIndexBuffer_ = device_->newBuffer(indices.data(), indices.size() * sizeof(std::uint32_t),
                                           MTL::ResourceStorageModeShared);
    if (waterVertexBuffer_ == nullptr || waterIndexBuffer_ == nullptr) {
        releaseWaterBuffers();
        throw RendererError{"failed to allocate water buffers"};
    }
    waterIndexCount_ = indices.size();
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

void Renderer::releaseSplat() noexcept {
    for (MTL::Texture*& layer : splatLayers_) {
        if (layer != nullptr) {
            layer->release();
            layer = nullptr;
        }
    }
    for (MTL::Texture*& normal : splatNormals_) {
        if (normal != nullptr) {
            normal->release();
            normal = nullptr;
        }
    }
    if (splatMaskA_ != nullptr) {
        splatMaskA_->release();
        splatMaskA_ = nullptr;
    }
    if (splatMaskB_ != nullptr) {
        splatMaskB_->release();
        splatMaskB_ = nullptr;
    }
    splatEnabled_ = false;
}

void Renderer::setWaterWaveTexture(const dds::Texture& normal) {
    if (waterWaves_ != nullptr) {
        waterWaves_->release();
    }
    waterWaves_ = uploadTexture(normal, "water waves");
}

void Renderer::setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                        const dds::Texture& maskB) {
    releaseSplat();

    // Both masks are required. Without them every stratum weight is undefined
    // and the map would render as its base layer alone — which looks like a
    // finished image rather than like a missing one, so refuse instead.
    if (layers.empty() || maskA.width <= 0 || maskB.width <= 0) {
        return;
    }

    splatMaskA_ = uploadTexture(maskA, "splat mask A");
    splatMaskB_ = uploadTexture(maskB, "splat mask B");

    for (std::size_t i = 0; i < kSplatLayers; ++i) {
        const bool have = i < layers.size() && layers[i].present();

        // Every slot must be bound: sampling an unbound texture is undefined,
        // and the shader references all nine unconditionally. Unused slots get
        // the same 4x4 fallback the ground texture uses, and are weighted to
        // zero by `present` so what they hold never reaches the image.
        splatLayers_[i] = have ? uploadTexture(layers[i].texture, "splat layer", true) : nullptr;
        splatPresent_[i] = have ? 1.0f : 0.0f;

        // Never zero, even for an absent layer: the shader divides by this to
        // build the layer UV, and a division by zero would produce NaNs that
        // survive the multiply by a zero weight.
        splatTileElmos_[i] = (have && layers[i].tileElmos > 0.0f) ? layers[i].tileElmos : 1.0f;

        // The stratum's normal map, on its own texture and its own tile size —
        // a .scmap scales the two independently. Slot 9 is the macrotexture,
        // which has no normal entry in the format at all.
        const bool haveNormal =
            i < kSplatNormalLayers && i < layers.size() && layers[i].hasNormal();
        if (i < kSplatNormalLayers) {
            splatNormals_[i] =
                haveNormal ? uploadTexture(layers[i].normal, "splat normal") : nullptr;
            splatNormalPresent_[i] = haveNormal ? 1.0f : 0.0f;
            splatNormalTileElmos_[i] =
                (haveNormal && layers[i].normalTileElmos > 0.0f) ? layers[i].normalTileElmos
                                                                 : 1.0f;
        }
    }

    // The base layer is not optional — everything else is laid over it.
    splatEnabled_ = splatPresent_[0] > 0.5f;
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

void Renderer::setEnvironment(const Environment& environment) noexcept {
    environment_ = environment;
}

void Renderer::clearFog() noexcept { hasFog_ = false; }

void Renderer::setFog(std::span<const std::uint16_t> counts, int squaresX, int squaresZ,
                      float widthElmos, float depthElmos) {
    const auto expected = static_cast<std::size_t>(squaresX) * static_cast<std::size_t>(squaresZ);
    if (squaresX <= 0 || squaresZ <= 0 || counts.size() != expected) {
        clearFog();
        return;
    }

    // R8, one byte a square, and the counts are flattened to 0 or 255 on the way in. The
    // shader wants "seen or not"; uploading the count itself would put a number on the GPU
    // that means something only to the sim, and a fog whose darkness varied with how many
    // units happened to overlap would be reporting troop strength through the terrain.
    fogMask_.resize(expected);
    for (std::size_t i = 0; i < expected; ++i) {
        fogMask_[i] = counts[i] != 0 ? std::uint8_t{255} : std::uint8_t{0};
    }

    // REALLOCATED ONLY WHEN THE SHAPE CHANGES. The mask is uploaded every frame — vision
    // moves every tick — so allocating a texture per frame would be a texture per frame.
    if (fogTexture_ == nullptr || fogSquaresX_ != squaresX || fogSquaresZ_ != squaresZ) {
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormat::PixelFormatR8Unorm, static_cast<NS::UInteger>(squaresX),
            static_cast<NS::UInteger>(squaresZ), /*mipmapped=*/false);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        descriptor->setStorageMode(MTL::StorageModeShared);

        MTL::Texture* texture = device_->newTexture(descriptor);
        if (texture == nullptr) {
            throw RendererError{"failed to allocate the fog of war mask"};
        }
        if (fogTexture_ != nullptr) {
            fogTexture_->release();
        }
        fogTexture_ = texture;
        fogSquaresX_ = squaresX;
        fogSquaresZ_ = squaresZ;
    }

    fogTexture_->replaceRegion(
        MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(squaresX),
                            static_cast<NS::UInteger>(squaresZ)),
        0, fogMask_.data(), static_cast<NS::UInteger>(squaresX));

    fogWidthElmos_ = widthElmos;
    fogDepthElmos_ = depthElmos;
    hasFog_ = true;
}

void Renderer::setWater(bool enabled, float levelElmos) noexcept {
    const bool levelChanged = waterLevel_ != levelElmos;
    hasWater_ = enabled;
    waterLevel_ = levelElmos;

    // The surface bakes the level into its vertices and its depths, and this
    // call arrives AFTER setTerrain — so without the rebuild every map would
    // wear the default level of zero.
    if (levelChanged && !waterGroundHeights_.empty()) {
        buildWaterMesh();
    }
}


void Renderer::encodeReflectionPass(MTL::CommandBuffer* commandBuffer) noexcept {
    if (!reflectionsEnabled_ || reflectionColour_ == nullptr || commandBuffer == nullptr
        || indexCount_ == 0 || !hasWater_ || terrainMinY_ >= waterLevel_) {
        return;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* colour = pass->colorAttachments()->object(0);
    colour->setTexture(reflectionColour_);
    colour->setLoadAction(MTL::LoadAction::LoadActionClear);
    colour->setStoreAction(MTL::StoreAction::StoreActionStore);
    // Cleared to ZERO alpha, which is how the water tells "the mirror saw
    // something here" from "the mirror saw nothing". Clearing to the sky
    // colour with alpha 1 makes every texel look like geometry, and the water
    // then reflects the clear colour everywhere the world is not.
    colour->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(reflectionDepth_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
    depth->setClearDepth(1.0);


    // The view matrix, composed with a reflection in the water plane.
    //
    // NOT a mirrored OrbitCamera: that camera derives its eye from a pitch it
    // clamps to a positive band, and a reflected camera looks UP from below the
    // surface — a negative pitch it cannot express at all. Reflecting the
    // matrix sidesteps the parameterisation entirely.
    //
    //   y' = 2 * waterLevel - y
    //
    // as a matrix, applied to the world before the ordinary view transform.
    const simd_float4x4 reflect = simd_matrix(
        simd_make_float4(1.0f, 0.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, -1.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, 0.0f, 1.0f, 0.0f),
        simd_make_float4(0.0f, 2.0f * waterLevel_, 0.0f, 1.0f));

    const float aspect =
        static_cast<float>(kReflectionWidth) / static_cast<float>(kReflectionHeight);

    SceneOverride override;
    override.viewProjection =
        simd_mul(camera_.projectionMatrix(aspect), simd_mul(camera_.viewMatrix(), reflect));
    // Anything under the surface is not in the mirror. A little above it, so
    // the shoreline itself does not shimmer in and out along the seam.
    override.clipBelowY = waterLevel_ - 1.0f;
    override.skipWater = true;

    encodeScene(commandBuffer, pass, kReflectionWidth, kReflectionHeight, &override);
    pass->release();
}

} // namespace rm
