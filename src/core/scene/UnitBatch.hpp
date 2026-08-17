#pragma once

#include "core/model/Model.hpp"
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
