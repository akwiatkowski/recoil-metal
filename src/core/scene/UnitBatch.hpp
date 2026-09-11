#pragma once

#include "core/model/Model.hpp"
#include "core/model/BuilderAim.hpp"
#include "core/model/Sca.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm {

// The two textures a model draws with, as indices into a texture list.
//
// A *pair* rather than two independent textures because that is the unit both
// engines batch on: Recoil groups its draws by texture pair
// (rts/Rendering/Models/3DModel.h — every model carries exactly two), and
// Supreme Commander's models likewise name an albedo and a specTeam map. -1
// means the model named none, which is a legitimate state, not an error.
struct TexturePair {
    int diffuse = -1;
    int shading = -1;

    [[nodiscard]] friend bool operator==(const TexturePair&, const TexturePair&) = default;
};

// One model and the instances to draw it at — the unit of work a renderer
// takes. Neither the model nor the instances are owned: they belong to whoever
// loaded them and must outlive the upload call, which is the only thing that
// reads them.
//
// Geometry is per model rather than merged into one buffer per texture pair.
// Recoil merges because it keeps every model in one big VBO and can only
// distinguish draws by their texture binding; here each model already owns its
// buffers, so the pair only governs the *order* — see orderByTexturePair.
struct UnitBatch {
    const Model* model = nullptr;
    std::span<const UnitInstance> instances;
    TexturePair textures;
    /// Optional Supreme Commander `_NormalsTS` texture. Kept outside TexturePair because
    /// Recoil's native batching contract remains exactly its authored two-texture pair.
    int normals = -1;

    // Optional animation to play. Its bones are matched to the model's by name,
    // so a mismatched pair moves only what it can rather than knotting the
    // model. Where in the animation each instance is comes from its own
    // UnitInstance::animationPhase, so a batch is a squad rather than one unit
    // drawn many times.
    const sca::Animation* animation = nullptr;

    /// Optional authored construction-arm rig for this exact model. Its subtree flags are baked
    /// into the batch's bone buffer; yaw and pitch remain per UnitInstance.
    BuilderAimRig builderAim;

    /// Optional primary-turret rig for this exact model, resolved from the first turreted
    /// weapon's bones. Same shader path as the builder arm (one yaw/pitch pair per
    /// instance), so a unit aims either its arm or its turret, never both at once.
    BuilderAimRig turretAim;
    /// Index into the type's weapons of the resolved turret, for reading its live target.
    std::size_t turretWeapon = 0;

    /// Recoil slide for the primary weapon: subtree flags OR'd into the bone
    /// buffer, travel in elmos, and the fraction of travel recovered per tick.
    /// Empty flags mean no authored rack — the gun does not kick.
    std::vector<std::uint32_t> recoilFlags;
    float recoilDistanceElmos = 0.0f;
    float recoilReturnPerTick = 0.0f;

    /// A deploy animation (a fold/unfold/open .sca) played once when the unit is
    /// built, then held. Used only when the batch has no looping request
    /// animation — statics do not walk, so there is nothing to displace.
    const sca::Animation* unpackAnimation = nullptr;
    bool unpackOneshot = false;

    // Whether the phase in each instance is the WHOLE answer, or an offset
    // added to a clock the renderer advances.
    //
    // False (the default) suits a scene nobody is stepping: the renderer's
    // clock runs, every instance keeps its constant offset, and the animation
    // plays. True hands control to whoever writes the instances — which is what
    // a sim wants, because a walk cycle should be paced by ground covered
    // rather than by wall time. Feet then stop when the unit stops, and slow
    // when it turns, neither of which a clock can express.
    //
    // A batch set to true whose instances are never written simply holds its
    // pose, which is the honest result of nothing driving it.
    bool animationDrivenByInstance = false;
};

// Draw order that visits every batch exactly once with identical texture pairs
// adjacent, so a renderer walking it rebinds textures once per distinct pair
// instead of once per batch.
//
// This is worth a function of its own — rather than a sort buried in the
// renderer — because it is the only part of batching that is pure, and because
// the property that matters (no pair is visited twice) is invisible on screen.
// A renderer that rebinds redundantly renders exactly the same image.
//
// Stable: batches sharing a pair keep the caller's relative order, so the scene
// a screenshot captures does not depend on the sort's tie-breaking.
[[nodiscard]] std::vector<std::size_t> orderByTexturePair(std::span<const TexturePair> batches);

/// How many times a renderer following `order` must bind textures. Exposed for
/// tests: it is the number the ordering exists to minimise.
[[nodiscard]] std::size_t textureBindCount(std::span<const TexturePair> batches,
                                           std::span<const std::size_t> order);

} // namespace rm
