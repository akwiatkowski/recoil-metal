#pragma once

#include "core/model/Model.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cstddef>
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
};

} // namespace rm
