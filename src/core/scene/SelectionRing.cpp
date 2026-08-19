#include "core/scene/SelectionRing.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm {

void appendSelectionRing(std::vector<RingVertex>& out, const HeightField& field,
                         std::array<float, 3> centre, float radiusElmos,
                         std::array<float, 4> colour, float thicknessElmos, int segments) {
    // A ring with no radius, no width or no segments is not a degenerate ring,
    // it is a caller mistake — and emitting a fan of zero-area triangles would
    // hide it behind something that renders as nothing anyway.
    if (!(radiusElmos > 0.0f) || !(thicknessElmos > 0.0f) || segments < 3) {
        return;
    }

    // Clamped so the inner rim never crosses the centre. Unclamped, a ring
    // thicker than twice its radius folds the band through the middle and
    // renders as a bow tie — which happens for real, because a footprint of two
    // squares is a radius of eight elmos.
    const float half = std::min(thicknessElmos * 0.5f, radiusElmos);
    const float inner = radiusElmos - half;
    const float outer = radiusElmos + half;

    // Height is sampled per vertex, from the ground under THAT point rather
    // than under the unit. On a slope the two differ by metres, and a ring at
    // the unit's own height buries its uphill half.
    const auto vertexAt = [&](float angle, float radius) {
        const float x = centre[0] + std::cos(angle) * radius;
        const float z = centre[2] + std::sin(angle) * radius;
        return RingVertex{
            .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
            .colour = colour,
        };
    };

    out.reserve(out.size() + ringVertexCount(segments));

    const float step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * step;
        // The last segment's far edge is segment 0's near edge by construction
        // — i + 1 rather than a separately accumulated angle, so the ring
        // closes exactly instead of to within a rounding error.
        const float a1 = static_cast<float>(i + 1) * step;

        const RingVertex innerA = vertexAt(a0, inner);
        const RingVertex outerA = vertexAt(a0, outer);
        const RingVertex innerB = vertexAt(a1, inner);
        const RingVertex outerB = vertexAt(a1, outer);

        // Two triangles closing the quad. Winding is not load-bearing: rings
        // draw with culling off, since a camera below the ground should still
        // see which units are selected.
        out.push_back(innerA);
        out.push_back(outerA);
        out.push_back(outerB);

        out.push_back(innerA);
        out.push_back(outerB);
        out.push_back(innerB);
    }
}

} // namespace rm
