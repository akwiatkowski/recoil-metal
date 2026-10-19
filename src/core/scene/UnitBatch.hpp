#pragma once

#include "core/model/Model.hpp"
#include "core/model/BuilderAim.hpp"
#include "core/model/Sca.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cstddef>
#include <span>
#include <string_view>
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

/// Retail's faction-fragment pick as the `unitFragment` variant the batch draws:
/// 0 base `NormalMappedPS`, 1 Aeon `AeonPS`, 2 Seraphim `UnitFalloffPS`,
/// 3 `WreckagePS`, 4 Metal `NormalMappedMetalPS`, 5 Insect `NormalMappedInsectPS`
/// (effects/mesh.fx techniques). The blueprint's `ShaderName` wins when stated;
/// wrecks keep variant 3 even when their blueprint says otherwise (retail retargets
/// only `ShaderName` at wreck extraction, and the wreck mesh still shades wrecked).
/// Otherwise the FA id's second letter is the fallback the renderer used before
/// `ShaderName` was plumbed (UEL/UAL/URL/XSL) — kept so bare meshes without a
/// blueprint still shade by faction. Case-insensitive; unknown draws base.
[[nodiscard]] inline unsigned shaderVariantFor(std::string_view shaderName,
                                              std::string_view modelName) noexcept {
    if (modelName.find("_wreck") != std::string::npos) {
        return 3;
    }
    if (!shaderName.empty()) {
        const char c = static_cast<char>(shaderName[0] | 0x20);
        switch (c) {
        case 'a':
            return 1;
        case 's':
            return 2;
        case 'm':
            return 4;
        case 'i':
            return 5;
        default:
            return 0;
        }
    }
    if (modelName.size() < 2) {
        return 0;
    }
    switch (modelName[1]) {
    case 'A':
    case 'a':
        return 1;
    case 'S':
    case 's':
        return 2;
    default:
        return 0;
    }
}
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
    /// Retail's faction-fragment pick from `Display.Mesh.LODs[1].ShaderName`
    /// ('Seraphim', 'Insect', 'Metal', 'Aeon', 'Unit'; empty draws base). Stamped
    /// into the instances' team-colour alpha at upload (UnitRenderer.mm), so one
    /// fragment reads per-instance channels — variants 4/5 ride here too.
    /// 0 base NormalMappedPS, 1 Aeon, 2 Seraphim, 3 wreckage, 4 Metal, 5 Insect.
    unsigned shaderVariant = 0;
    /// The per-blueprint view-ramp LUT (`Display.Mesh.LODs[1].LookupName`,
    /// mesh.fx:2663 `falloffSampler`) as a texture index, or -1 when unbound.
    /// Only Seraphim builds name one; Metal/Insect LUTs are engine-global and
    /// ride their own batch fields below.
    int lookup = -1;
    /// Engine-global aniso LUT (`textures/engine/anisotropiclookup.dds`,
    /// mesh.fx:2546-2547 `anisotropicSampler`), bound for Metal batches (variant 4).
    int anisoLookup = -1;
    /// Engine-global insect LUT (`textures/engine/insectlookup.dds`,
    /// mesh.fx:3053-3054 `insectSampler`), bound for Insect batches (variant 5).
    int insectLookup = -1;

    // Optional animation to play. Its bones are matched to the model's by name,
    // so a mismatched pair moves only what it can rather than knotting the
    // model. Where in the animation each instance is comes from its own
    // UnitInstance::animationPhase, so a batch is a squad rather than one unit
    // drawn many times.
    const sca::Animation* animation = nullptr;

    /// The blueprint's `AnimationWalkRate` — playback rate at top speed. The
    /// walk phase is paced by distance, so it lands here as the divisor of the
    /// stride: one cycle covers `speed x duration / walkRate`. 1 when unstated.
    float animationWalkRate = 1.0f;

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
    /// buffer (rack AND telescope bits), SIGNED travel in elmos (negative =
    /// backwards along the barrel, as authored), and the fraction of travel
    /// recovered per tick. Empty flags mean no authored rack — the gun does
    /// not kick.
    std::vector<std::uint32_t> recoilFlags;
    float recoilDistanceElmos = 0.0f;
    float recoilReturnPerTick = 0.0f;
    /// The telescope channel of the same slide — its own manipulator and goal
    /// in retail (`TelescopeRecoilDistance`, falling back to the rack
    /// distance). Zero distance, no channel.
    float telescopeDistanceElmos = 0.0f;
    float telescopeReturnPerTick = 0.0f;

    /// A deploy animation (a fold/unfold/open .sca) played once when the unit is
    /// built, then held. Used only when the batch has no looping request
    /// animation — statics do not walk, so there is nothing to displace.
    const sca::Animation* unpackAnimation = nullptr;
    bool unpackOneshot = false;

    /// A rack-salvo reload clip (`AnimationReload`, `defaultweapons.lua:202-210`
    /// `PlayFxRackSalvoReloadSequence`): only 2 shipped weapons name one
    /// (`XSL0111`, `XEL0306`). Null on every other batch — a rack without
    /// authored reload simply fires without one. Baked as the batch's pose
    /// source when neither a loop nor a deploy clip exists (UnitRenderer.mm),
    /// stamped one-shot from `UnitScene::reloadTick` like deploy (Scene.hpp).
    const sca::Animation* reloadAnimation = nullptr;

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
