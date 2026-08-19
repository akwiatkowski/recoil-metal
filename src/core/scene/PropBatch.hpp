#pragma once

#include "core/model/Model.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>

namespace rm {

// One prop mesh and every place it stands — the scenery equivalent of UnitBatch.
//
// Deliberately NOT a UnitBatch, though the two are nearly the same shape. A prop
// is not a unit and must not end up in the list that is: the sim ticks every unit
// batch, resolves collisions across them, and picking searches them. A tree that
// arrived in that list would be shoved about by passing infantry, would be
// selectable, and would take a move order.
//
// It reuses UnitInstance for the per-instance data even so, because the two want
// exactly the same 48 bytes and the vertex shader is already written to read
// them. The team-colour field goes unused, which is 16 bytes a prop does not need
// against a whole second instance layout, a second shader input struct and a
// second vertex function that would differ from the first in nothing.
struct PropBatch {
    const Model* model = nullptr;
    std::span<const UnitInstance> instances;

    /// Index into the texture list passed alongside, or -1 for none.
    ///
    /// One texture rather than a pair: a prop blueprint names an albedo and
    /// sometimes a normal map, and never the second channel a unit's shading
    /// texture carries. Its albedo's ALPHA is opacity — the leaf shape cut out
    /// of the quad — so the renderer must be told not to read it as the
    /// team-colour mask both unit families keep somewhere.
    int albedo = -1;

    /// Beyond this distance from the camera, instances of this prop are not drawn.
    ///
    /// The blueprint's own furthest LOD cutoff (PropBlueprint.hpp). Graded per
    /// prop, which is what makes zooming out thin the small detail first: at a
    /// whole-map framing every one of them is past its cutoff and the scenery
    /// costs nothing, while a shrub has already gone at a working zoom where a
    /// landmark tree is still there.
    float drawDistanceElmos = std::numeric_limits<float>::infinity();
};

/// Copies the instances worth drawing from `eye` into `out`, returning how many.
///
/// Pure, and in core/, because this is the whole of the decision and none of it
/// needs a GPU — and because what it gets wrong is invisible in the good case and
/// looks like missing content in the bad one. `out` must be at least as long as
/// `instances`; the survivors keep their relative order so that a capture does not
/// depend on how a partition happened to fall.
///
/// Distance is measured to the instance's own position rather than to a bounding
/// sphere: a prop is a handful of elmos across and the cutoffs are hundreds, so the
/// difference is far below the threshold's own precision.
[[nodiscard]] std::size_t cullPropsByDistance(std::span<const UnitInstance> instances,
                                              std::array<float, 3> eye, float drawDistanceElmos,
                                              std::span<UnitInstance> out) noexcept;

} // namespace rm
