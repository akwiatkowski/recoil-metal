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
// One detail level of a prop: the mesh, its texture, and how far out it is the
// right level to draw.
struct PropLevel {
    const Model* model = nullptr;

    /// Index into the texture list passed alongside, or -1 for none.
    ///
    /// One texture rather than a pair: a prop blueprint names an albedo and
    /// sometimes a normal map, and never the second channel a unit's shading
    /// texture carries. Its albedo's ALPHA is opacity — the leaf shape cut out
    /// of the quad — so the renderer must be told not to read it as the
    /// team-colour mask both unit families keep somewhere.
    int albedo = -1;

    /// Drawn at this level while the camera is nearer than this. The coarsest
    /// level's cutoff is therefore also the distance past which the prop is not
    /// drawn at all.
    float cutoffElmos = std::numeric_limits<float>::infinity();
};

struct PropBatch {
    /// The levels, FINEST FIRST, as the blueprint states them.
    ///
    /// Most props have one. The trees have three, and the trees are what there are
    /// ten thousand of.
    std::span<const PropLevel> levels;

    /// Every place this prop stands. One list for all the levels: which level an
    /// instance is drawn at depends on where the camera is, so the split is decided
    /// per frame rather than baked.
    std::span<const UnitInstance> instances;
};

/// Sorts instances into one contiguous run per level, finest first, and drops the
/// ones past the coarsest cutoff entirely.
///
/// `out` receives the runs back to back and must be at least as long as
/// `instances`; `countsOut` receives one count per level, so a level's run starts at
/// the sum of the counts before it. Survivors keep their relative order within a
/// run, so a capture does not depend on how the partition happened to fall.
///
/// Pure, and in core/, because this is the whole of the decision and none of it
/// needs a GPU — and because what it gets wrong is invisible in the good case and
/// looks like missing content in the bad one.
///
/// Distance is measured to the instance's own position rather than to a bounding
/// sphere: a prop is a handful of elmos across and the cutoffs are hundreds, so the
/// difference is far below the threshold's own precision.
void cullPropsByLevel(std::span<const UnitInstance> instances, std::array<float, 3> eye,
                      std::span<const float> cutoffsElmos, std::span<UnitInstance> out,
                      std::span<std::size_t> countsOut) noexcept;

} // namespace rm
