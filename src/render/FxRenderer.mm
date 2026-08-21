// Effects: ground decals, particles, and the shadow pass.
//
// Grouped because they are what Recoil calls a projectile drawer plus a decal handler, and
// because the shadow map is written here and sampled by everything else.
//
// ONE CLASS, SEVERAL TRANSLATION UNITS (PLAN2.md §7 P7.3). This file defines `Renderer`'s
// effect and shadow members; it is not a separate object. That distinction is the honest state of the
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

void Renderer::setGroundDecals(std::span<const DecalVertex> vertices) noexcept {
    decalVertexCount_ = 0;
    if (decalBuffer_ == nullptr || vertices.empty()) {
        return;
    }

    // Dropped rather than grown — the buffer cannot be reallocated while up to
    // two other frames may still be reading it. Truncated to whole triangles so
    // the tail is never a partial one, which would render as a stray sliver
    // rather than as a missing ring.
    const std::size_t fits = std::min(vertices.size(), kMaxDecalVertices);
    decalVertexCount_ = fits - (fits % 3u);
    if (decalVertexCount_ == 0) {
        return;
    }

    auto* base = static_cast<DecalVertex*>(decalBuffer_->contents());
    std::memcpy(base + instanceSlot_ * kMaxDecalVertices, vertices.data(),
                decalVertexCount_ * sizeof(DecalVertex));
}

void Renderer::setParticles(std::span<const Particle> particles) noexcept {
    particleCount_ = 0;
    if (particleBuffer_ == nullptr || particles.empty()) {
        return;
    }

    // Dropped rather than grown, like the decals: the buffer cannot be
    // reallocated while up to two other frames may still be reading it. No
    // rounding needed here — a particle is a whole thing, not three vertices.
    particleCount_ = std::min(particles.size(), kMaxParticles);

    auto* base = static_cast<Particle*>(particleBuffer_->contents());
    std::memcpy(base + instanceSlot_ * kMaxParticles, particles.data(),
                particleCount_ * sizeof(Particle));
}

void Renderer::updateLightMatrix() noexcept {
    hasShadows_ = false;
    if (shadowMap_ == nullptr || indexCount_ == 0) {
        return;
    }

    // The light's box follows the CAMERA, not the map.
    //
    // Covering the whole map sounds tidier and is much worse: on an 8192-elmo
    // map a 2048-texel shadow map is 4 elmos per texel, and the depth range
    // spans the map's diagonal — so the bias needed to stop the ground
    // shadowing itself is several elmos, which is most of a tank's height. The
    // shadow then lifts clean off its caster and nothing appears at all.
    //
    // Sized to what the camera can actually see, the same map is fractions of
    // an elmo per texel and the bias costs nothing visible.
    const float extent = std::clamp(camera_.distance, 100.0f, 6000.0f);
    const simd_float3 centre = camera_.target;

    // Stand the light off far enough to catch anything that could cast into
    // view, and give the box enough depth to hold it.
    const float depthRange = extent * 8.0f;
    const simd_float3 eye = centre + kSunDirection * extent * 4.0f;

    const simd_float3 forward = simd_normalize(centre - eye);
    const simd_float3 reference =
        std::abs(forward.y) > 0.99f ? simd_make_float3(0, 0, 1) : simd_make_float3(0, 1, 0);
    const simd_float3 right = simd_normalize(simd_cross(reference, forward));
    const simd_float3 up = simd_cross(forward, right);

    const simd_float4x4 view = simd_matrix(
        simd_make_float4(right.x, up.x, forward.x, 0.0f),
        simd_make_float4(right.y, up.y, forward.y, 0.0f),
        simd_make_float4(right.z, up.z, forward.z, 0.0f),
        simd_make_float4(-simd_dot(right, eye), -simd_dot(up, eye), -simd_dot(forward, eye),
                         1.0f));

    // Orthographic, with Metal's [0, 1] depth range — the same convention the
    // camera's perspective matrix uses, and for the same reason.
    const simd_float4x4 projection = simd_matrix(
        simd_make_float4(1.0f / extent, 0.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, 1.0f / extent, 0.0f, 0.0f),
        simd_make_float4(0.0f, 0.0f, 1.0f / depthRange, 0.0f),
        simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));

    lightViewProjection_ = simd_mul(projection, view);
    lightCentre_ = centre;
    lightExtent_ = extent;
    hasShadows_ = true;
}

void Renderer::encodeShadowPass(MTL::CommandBuffer* commandBuffer) noexcept {
    // The box follows the camera, so it is rebuilt every frame — a handful of
    // matrix operations against a pass that draws the whole scene.
    updateLightMatrix();
    if (!hasShadows_ || shadowMap_ == nullptr || commandBuffer == nullptr) {
        return;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(shadowMap_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionStore);  // the fragment shaders read it
    depth->setClearDepth(1.0);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

    // Only the light matrix is read by the shadow vertex shaders, but the whole
    // struct goes up: one layout, one place to get it wrong.
    TerrainUniforms uniforms{};
    uniforms.lightViewProjection = lightViewProjection_;
    uniforms.hasShadows = 1.0f;

    // No encoder depth bias: setDepthBias works in units of the depth format's
    // smallest resolvable step, which for a 32-bit float over a map-sized
    // orthographic range is not a quantity worth guessing at. The bias is
    // applied in the shader instead, in clip-space units this code chooses.
    encoder->setDepthStencilState(depthState_);
    encoder->setCullMode(MTL::CullMode::CullModeNone);
    encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

    if (indexCount_ > 0) {
        encoder->setRenderPipelineState(terrainShadowPipeline_);
        encoder->setVertexBuffer(vertexBuffer_, 0, kVertexBufferIndex);

        // Only the chunks the light's box touches. The box follows the camera
        // and covers a fraction of the map, so on a large map this is a handful
        // of chunks against sixteen by sixteen — and it was the whole reason
        // the shadow pass cost as much as it did.
        //
        // Tested in the light's own axes as a sphere against the box: the box
        // is oriented along the sun, so an axis-aligned test in world space
        // would be wrong, and a chunk's bounding sphere is cheap and never too
        // small. Too generous is harmless here; too tight drops shadows.
        // Only the chunks the light's box touches. The box follows the camera
        // and covers a fraction of the map, so this is usually a handful of
        // chunks against sixteen by sixteen — and it is why the shadow pass no
        // longer costs what it did.
        //
        // Tested as a sphere against the box in the LIGHT's axes: the box is
        // oriented along the sun, so an axis-aligned world-space test would be
        // wrong. A chunk's bounding sphere is cheap and never too small, and
        // here too generous is harmless while too tight drops shadows.
        // Full detail everywhere, deliberately, and NOT the cheapest level —
        // which is what "varyDetail = false" buys, since level 0 is the finest.
        //
        // A shadow map is a record of the surface the main pass will shade. Draw
        // a coarser surface into it and the two disagree by the whole chord
        // error of that level (13 elmos at the median on aw04, 92 at the worst),
        // which is far more than any depth bias can absorb: the ground shadows
        // itself in bands wherever the coarse surface sits above the fine one,
        // and contact shadows lift off wherever it sits below. Cheap here would
        // cost more than it saves — the light's box already culls this pass to a
        // fraction of the map, which is where the shadow saving came from.
        drawTerrainChunks(encoder, [this](const TerrainChunk& chunk) {
            const simd_float3 centre = simd_make_float3((chunk.minX + chunk.maxX) * 0.5f,
                                                        (chunk.minY + chunk.maxY) * 0.5f,
                                                        (chunk.minZ + chunk.maxZ) * 0.5f);
            const float radius = 0.5f * simd_length(simd_make_float3(chunk.maxX - chunk.minX,
                                                                     chunk.maxY - chunk.minY,
                                                                     chunk.maxZ - chunk.minZ));
            const simd_float3 offset = centre - lightCentre_;
            const simd_float3 along = kSunDirection * simd_dot(offset, kSunDirection);
            return simd_length(offset - along) <= lightExtent_ + radius;
        }, lightCentre_, /*varyDetail=*/false);
    }

    encoder->setRenderPipelineState(unitShadowPipeline_);
    for (const GpuUnitBatch& batch : unitBatches_) {
        if (batch.instanceCount == 0) {
            continue;
        }

        encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
        encoder->setVertexBuffer(
            batch.instanceBuffer,
            static_cast<NS::UInteger>(instanceSlot_ * batch.instanceCapacity
                                      * sizeof(UnitInstance)),
            kInstanceBufferIndex);
        encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

        PoseUniforms pose;
        pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
        pose.boneCount =
            static_cast<std::uint32_t>(batch.boneStrideBytes / sizeof(BoneTransform));
        pose.duration = batch.duration;
        pose.time = batch.animationDrivenByInstance ? 0.0f : animationTime_;
        encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(batch.indexCount),
                                       MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                       /*indexBufferOffset=*/0,
                                       static_cast<NS::UInteger>(batch.instanceCount));
    }

    // --- Props -------------------------------------------------------------
    // On its own pipeline, because a prop's shape is cut out of its quads by the
    // albedo's alpha and a depth-only pass would record the untrimmed quad: a tree
    // casting the shadow of the card its leaves are painted on.
    //
    // The runs and levels are whatever cullProps decided for this frame, so a prop
    // too far to be drawn is also too far to shade — and the level a tree casts from
    // is the level it is drawn at, which keeps the shadow the shape of the tree.
    if (!propGroups_.empty() && propsVisible_ && propShadowPipeline_ != nullptr) {
        encoder->setRenderPipelineState(propShadowPipeline_);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);

        for (const GpuPropGroup& group : propGroups_) {
            for (const GpuPropLevel& level : group.levels) {
                if (level.instanceCount == 0) {
                    continue;
                }

                MTL::Texture* albedo = nullptr;
                if (level.albedo >= 0
                    && static_cast<std::size_t>(level.albedo) < propTextures_.size()) {
                    albedo = propTextures_[static_cast<std::size_t>(level.albedo)];
                }
                if (albedo == nullptr) {
                    continue;  // no alpha to cut with, so nothing to cast honestly
                }
                encoder->setFragmentTexture(albedo, kGroundTextureIndex);

                encoder->setVertexBuffer(level.vertexBuffer, 0, kVertexBufferIndex);
                encoder->setVertexBuffer(
                    group.instanceBuffer,
                    static_cast<NS::UInteger>((instanceSlot_ * group.instanceCapacity
                                               + level.firstInstance)
                                              * sizeof(UnitInstance)),
                    kInstanceBufferIndex);
                encoder->setVertexBuffer(level.boneBuffer, 0, kBoneBufferIndex);

                PoseUniforms pose;
                pose.poseCount = 1;
                pose.boneCount =
                    static_cast<std::uint32_t>(level.boneStrideBytes / sizeof(BoneTransform));
                pose.duration = 0.0f;
                pose.time = 0.0f;
                encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    static_cast<NS::UInteger>(level.indexCount), MTL::IndexType::IndexTypeUInt32,
                    level.indexBuffer, /*indexBufferOffset=*/0,
                    static_cast<NS::UInteger>(level.instanceCount));
            }
        }
    }

    encoder->endEncoding();
    pass->release();
}

} // namespace rm
