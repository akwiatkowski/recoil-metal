// Units and props: their buffers, their culling, their instances, their selection.
//
// The instanced-draw side. `setUnits` and `setProps` own the per-batch buffers and textures;
// `setInstances` is what a frame writes into the ring.
//
// ONE CLASS, SEVERAL TRANSLATION UNITS (PLAN2.md §7 P7.3). This file defines `Renderer`'s
// unit and prop members; it is not a separate object. That distinction is the honest state of the
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
    batchForSourceIndex_.clear();
}

void Renderer::releasePropBuffers() noexcept {
    for (GpuPropGroup& group : propGroups_) {
        for (GpuPropLevel& level : group.levels) {
            if (level.boneBuffer != nullptr)   level.boneBuffer->release();
            if (level.indexBuffer != nullptr)  level.indexBuffer->release();
            if (level.vertexBuffer != nullptr) level.vertexBuffer->release();
        }
        if (group.instanceBuffer != nullptr) group.instanceBuffer->release();
    }
    propGroups_.clear();

    for (MTL::Texture* texture : propTextures_) {
        if (texture != nullptr) {
            texture->release();
        }
    }
    propTextures_.clear();
}

void Renderer::setProps(std::span<const dds::Texture> textures,
                        std::span<const PropBatch> batches) {
    releasePropBuffers();

    if (batches.empty()) {
        return;
    }

    propTextures_.reserve(textures.size());
    for (const dds::Texture& texture : textures) {
        // A null slot rather than renumbering, exactly as the units do: the
        // indices in the batches were decided by the caller.
        propTextures_.push_back(texture.data.empty() ? nullptr
                                                     : uploadTexture(texture, "prop"));
    }

    // No reordering by texture. Units are sorted so each texture pair binds once
    // a frame, which is worth doing when a scene holds a handful of models; a map
    // holds up to 80 distinct props and every one has its own albedo, so there is
    // nothing to group — the sort would be a no-op over a list where no two
    // entries share a key. The caller's order stands.
    propGroups_.reserve(batches.size());
    for (const PropBatch& batch : batches) {
        if (batch.instances.empty() || batch.levels.empty()) {
            continue;
        }

        GpuPropGroup group;
        group.instances.assign(batch.instances.begin(), batch.instances.end());

        // kMaxFramesInFlight copies of ONE buffer, shared by the levels. It does get
        // rewritten every frame — not because props move, but because which level
        // each is drawn at changes as the camera does, and a single copy written
        // while the GPU may still be reading last frame's would tear a transform.
        group.instanceCapacity = batch.instances.size();
        const std::size_t instanceBytes = group.instanceCapacity * sizeof(UnitInstance);
        group.instanceBuffer = device_->newBuffer(instanceBytes * kMaxFramesInFlight,
                                                  MTL::ResourceStorageModeShared);

        for (const PropLevel& source : batch.levels) {
            if (source.model == nullptr || source.model->empty() || source.model->bones.empty()) {
                continue;
            }
            const Model& model = *source.model;

            // The rest pose, and only ever the rest pose. A prop has no animation —
            // a blueprint's script class may say "Tree" and the game may topple it
            // when shot, but nothing here shoots anything. restPose is still needed
            // rather than skippable: it is where the two families' vertex conventions
            // are reconciled, and the vertex shader always indexes a bone.
            const std::vector<BoneTransform> poses = restPose(model);

            GpuPropLevel level;
            // A prop's albedo goes in the diffuse slot; the shading slot stays empty,
            // so the fragment shader takes its neutral fallback and the prop is lit
            // by sun and ambient alone.
            level.albedo = source.albedo;
            level.normals = source.normals;
            level.cutoffElmos = source.cutoffElmos;
            level.supremeCommanderShading = model.family == Family::SupremeCommander;
            level.boneStrideBytes = model.bones.size() * sizeof(BoneTransform);
            level.indexCount = model.indices.size();
            level.vertexBuffer =
                device_->newBuffer(model.vertices.data(),
                                   model.vertices.size() * sizeof(ModelVertex),
                                   MTL::ResourceStorageModeShared);
            level.indexBuffer =
                device_->newBuffer(model.indices.data(),
                                   model.indices.size() * sizeof(std::uint32_t),
                                   MTL::ResourceStorageModeShared);
            level.boneBuffer = device_->newBuffer(poses.data(),
                                                  poses.size() * sizeof(BoneTransform),
                                                  MTL::ResourceStorageModeShared);

            if (level.vertexBuffer == nullptr || level.indexBuffer == nullptr
                || level.boneBuffer == nullptr) {
                group.levels.push_back(level);  // so releasePropBuffers frees it
                propGroups_.push_back(std::move(group));
                releasePropBuffers();
                throw RendererError{"failed to allocate prop buffers for " + model.name};
            }

            group.levels.push_back(level);
        }

        if (group.levels.empty() || group.instanceBuffer == nullptr) {
            propGroups_.push_back(std::move(group));  // so what was allocated is freed
            releasePropBuffers();
            throw RendererError{"failed to allocate the prop instance buffer"};
        }

        // Seeded in every slot at the FINEST level, so a frame that never calls
        // cullProps — a headless capture — still draws the scenery rather than
        // nothing.
        auto* slots = static_cast<std::byte*>(group.instanceBuffer->contents());
        for (std::size_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
            std::memcpy(slots + slot * instanceBytes, batch.instances.data(), instanceBytes);
        }
        group.levels.front().firstInstance = 0;
        group.levels.front().instanceCount = batch.instances.size();

        propGroups_.push_back(std::move(group));
    }
}

void Renderer::cullProps() noexcept {
    if (propGroups_.empty() || !propsVisible_) {
        return;
    }

    const simd_float3 eyeVector = camera_.eye();
    const std::array<float, 3> eye{{eyeVector.x, eyeVector.y, eyeVector.z}};

    for (GpuPropGroup& group : propGroups_) {
        propCutoffs_.clear();
        for (const GpuPropLevel& level : group.levels) {
            propCutoffs_.push_back(level.cutoffElmos);
        }
        propCounts_.assign(group.levels.size(), 0);
        visibleProps_.resize(group.instances.size());

        cullPropsByLevel(group.instances, eye, propCutoffs_, visibleProps_, propCounts_);

        // The runs come back contiguous and finest first, so each level's offset is
        // the sum of the counts before it — which is the whole reason one buffer
        // serves every level.
        std::size_t at = 0;
        for (std::size_t i = 0; i < group.levels.size(); ++i) {
            group.levels[i].firstInstance = at;
            group.levels[i].instanceCount = propCounts_[i];
            at += propCounts_[i];
        }

        if (at == 0 || group.instanceBuffer == nullptr) {
            continue;  // nothing survives, and a zero-instance draw is skipped
        }

        auto* slots = static_cast<std::byte*>(group.instanceBuffer->contents());
        std::memcpy(slots + instanceSlot_ * group.instanceCapacity * sizeof(UnitInstance),
                    visibleProps_.data(), at * sizeof(UnitInstance));
    }
}

void Renderer::setUnits(std::span<const dds::Texture> textures,
                        std::span<const UnitBatch> batches) {
    releaseUnitBuffers();

    if (batches.empty()) {
        return;
    }

    // Every caller index starts unmapped, so a batch skipped below stays that
    // way and setInstances on it is a no-op rather than a stray write.
    batchForSourceIndex_.assign(batches.size(), kNoBatch);

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
        // An EMPTY batch uploads: a batch with no instances is "registered but not yet
        // built", which is exactly what the build ghost draws — a model with no unit behind
        // it. It used to be skipped here, which meant the ghost's batch never reached the
        // GPU and the silhouette silently drew nothing. The draw loop skips zero-instance
        // batches instead, which costs a comparison rather than a model.
        if (batch.model == nullptr || batch.model->empty() || batch.model->bones.empty()) {
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
        uploaded.animationDrivenByInstance = batch.animationDrivenByInstance;
        uploaded.vertexBuffer =
            device_->newBuffer(model.vertices.data(), model.vertices.size() * sizeof(ModelVertex),
                               MTL::ResourceStorageModeShared);
        uploaded.indexBuffer =
            device_->newBuffer(model.indices.data(), model.indices.size() * sizeof(std::uint32_t),
                               MTL::ResourceStorageModeShared);
        uploaded.boneBuffer = device_->newBuffer(poses.data(), poses.size() * sizeof(BoneTransform),
                                                 MTL::ResourceStorageModeShared);
        // kMaxFramesInFlight copies of the instances, not one: a scene whose
        // units move rewrites this every frame, and a single copy would be
        // written while the GPU is still reading it. Seeded with the same data
        // in every slot so a batch that is never updated draws correctly from
        // whichever slot the frame lands on.
        // At least one instance's room even when none exist yet: a zero-byte Metal buffer is
        // a null buffer, and this batch may be the ghost's — or grow its first real unit
        // next frame through setInstances, whose writes are capped by this capacity.
        uploaded.instanceCapacity = std::max<std::size_t>(1, batch.instances.size());
        const std::size_t instanceBytes = uploaded.instanceCapacity * sizeof(UnitInstance);
        uploaded.instanceBuffer = device_->newBuffer(instanceBytes * kMaxFramesInFlight,
                                                     MTL::ResourceStorageModeShared);
        if (uploaded.instanceBuffer != nullptr && !batch.instances.empty()) {
            auto* slots = static_cast<std::byte*>(uploaded.instanceBuffer->contents());
            const std::size_t filled = batch.instances.size() * sizeof(UnitInstance);
            for (std::size_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
                std::memcpy(slots + slot * instanceBytes, batch.instances.data(), filled);
            }
        }
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

        batchForSourceIndex_[index] = unitBatches_.size();
        unitBatches_.push_back(uploaded);
    }
}

void Renderer::setInstances(std::size_t batchIndex,
                            std::span<const UnitInstance> instances) noexcept {
    if (batchIndex >= batchForSourceIndex_.size()) {
        return;
    }

    const std::size_t target = batchForSourceIndex_[batchIndex];
    if (target == kNoBatch || target >= unitBatches_.size()) {
        return;  // the batch was skipped at upload; nothing to move
    }

    GpuUnitBatch& batch = unitBatches_[target];
    if (batch.instanceBuffer == nullptr || batch.instanceCapacity == 0) {
        return;
    }

    const std::size_t count = std::min(instances.size(), batch.instanceCapacity);
    const std::size_t slotBytes = batch.instanceCapacity * sizeof(UnitInstance);

    auto* slots = static_cast<std::byte*>(batch.instanceBuffer->contents());
    std::memcpy(slots + instanceSlot_ * slotBytes, instances.data(),
                count * sizeof(UnitInstance));

    // Drawing fewer than were uploaded is fine — the tail of the slot simply
    // goes unread — so a caller may shrink a batch without reallocating.
    batch.instanceCount = count;
}

void Renderer::setGhost(std::size_t batch, const UnitInstance& instance,
                        std::array<float, 4> tint) noexcept {
    // The SOURCE index, as every caller-facing batch index here is. Mapped through
    // `batchForSourceIndex_` at encode time rather than now, because an upload between
    // frames renumbers the GPU batches and a mapped-and-stored index would be stale —
    // exactly the case the ghost hits, since arming a new blueprint is what grows the
    // batch list in the first place.
    ghost_ = GhostDraw{.batch = batch, .instance = instance, .tint = tint};
}

void Renderer::clearGhost() noexcept { ghost_.reset(); }

void Renderer::setSelection(std::span<const SelectionEntry> selected) noexcept {
    outlineRuns_.clear();
    if (outlineBuffer_ == nullptr || selected.empty() || unitBatches_.empty()) {
        return;
    }

    auto* base = static_cast<UnitInstance*>(outlineBuffer_->contents())
                 + instanceSlot_ * kMaxOutlinedUnits;
    std::size_t written = 0;

    // Grouped into runs by batch, because an outline is drawn from the unit's own
    // mesh and the mesh is per batch. A selection arrives in click order, so the same
    // batch can appear more than once — each stretch becomes its own run rather than
    // being sorted, which keeps the outlines in the order the rings are built in.
    for (const SelectionEntry& entry : selected) {
        if (entry.batch >= batchForSourceIndex_.size()) {
            continue;
        }
        const std::size_t batch = batchForSourceIndex_[entry.batch];
        if (batch == kNoBatch || written >= kMaxOutlinedUnits) {
            continue;  // a batch that was skipped at upload, or past the cap
        }

        const GpuUnitBatch& uploaded = unitBatches_[batch];
        if (entry.instance >= uploaded.instanceCount) {
            continue;
        }

        // The instance as the GPU currently has it, so an outline follows a walking
        // unit rather than where it stood when it was selected.
        const auto* instances = static_cast<const UnitInstance*>(uploaded.instanceBuffer->contents())
                                + instanceSlot_ * uploaded.instanceCapacity;
        base[written] = instances[entry.instance];

        if (!outlineRuns_.empty() && outlineRuns_.back().batch == batch
            && outlineRuns_.back().firstInstance + outlineRuns_.back().instanceCount == written) {
            ++outlineRuns_.back().instanceCount;
        } else {
            outlineRuns_.push_back(OutlineRun{batch, written, 1});
        }
        ++written;
    }
}

} // namespace rm
