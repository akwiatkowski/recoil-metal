#include "core/scene/GroundDecals.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm {

void appendSelectionRing(std::vector<DecalVertex>& out, const HeightField& field,
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
        return DecalVertex{
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

        const DecalVertex innerA = vertexAt(a0, inner);
        const DecalVertex outerA = vertexAt(a0, outer);
        const DecalVertex innerB = vertexAt(a1, inner);
        const DecalVertex outerB = vertexAt(a1, outer);

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

void appendOrderMarker(std::vector<DecalVertex>& out, const HeightField& field,
                       std::array<float, 3> centre, std::array<float, 4> colour, float age,
                       float radiusElmos) {
    // Expired, and silently: a caller holding one marker per order should be able
    // to keep offering it every frame rather than tracking which have run out.
    if (age < 0.0f || age >= kOrderMarkerSecondsToLive || !(radiusElmos > 0.0f)) {
        return;
    }

    const float remaining = 1.0f - age / kOrderMarkerSecondsToLive;

    // Shrinks toward the point it marks, and fades. Shrinking rather than only
    // fading because the two say different things: a fading marker says "this is
    // going away", a contracting one says "the order landed HERE" — which is the
    // question the marker exists to answer, and the reason a click feels
    // acknowledged rather than merely recorded.
    //
    // Not to nothing: it stops at a third of its size, so the last frame before it
    // disappears is still a marker rather than a dot. Fading is squared, so most
    // of the life is spent visible and the exit is quick.
    const float scale = 0.34f + 0.66f * remaining;
    std::array<float, 4> faded = colour;
    faded[3] = colour[3] * remaining * remaining;

    const float radius = radiusElmos * scale;

    // The ring, thinner than a selection ring at the same size — this is a
    // momentary mark rather than a state, and a heavy one draws the eye away from
    // the units that are about to move.
    appendSelectionRing(out, field, centre, radius, faded, kRingThicknessElmos * 0.7f);

    // ...and a cross through it, which is what distinguishes it from a selection
    // at a glance. Two bars, each a quad following the ground along its length —
    // sampled per vertex like the ring, so the cross lies in a gully rather than
    // bridging it.
    const float halfWidth = kRingThicknessElmos * 0.35f;
    const float reach = radius * 0.72f;  // stops inside the ring rather than at it

    const auto bar = [&](float alongX, float alongZ) {
        // Perpendicular, in the ground plane, to give the bar its width.
        const float acrossX = -alongZ;
        const float acrossZ = alongX;

        const auto corner = [&](float along, float across) {
            const float x = centre[0] + alongX * along + acrossX * across;
            const float z = centre[2] + alongZ * along + acrossZ * across;
            return DecalVertex{
                .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
                .colour = faded,
            };
        };

        // Split along its length, so a bar spanning several heightmap squares
        // follows them instead of chording across. Four segments over 20 elmos is
        // a sample every 5, which is finer than the 8-elmo grid underneath.
        constexpr int kSegments = 4;
        const float step = 2.0f * reach / static_cast<float>(kSegments);
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = -reach + step * static_cast<float>(i);
            const float a1 = a0 + step;

            const DecalVertex left0 = corner(a0, -halfWidth);
            const DecalVertex right0 = corner(a0, halfWidth);
            const DecalVertex left1 = corner(a1, -halfWidth);
            const DecalVertex right1 = corner(a1, halfWidth);

            out.push_back(left0);
            out.push_back(right0);
            out.push_back(right1);

            out.push_back(left0);
            out.push_back(right1);
            out.push_back(left1);
        }
    };

    // Diagonal rather than axis-aligned: a cross lying along X and Z reads as a
    // grid artefact on terrain built from an axis-aligned heightfield, and every
    // other straight line on screen is already one of those two directions.
    constexpr float kDiagonal = 0.70710678f;  // 1/sqrt(2)
    bar(kDiagonal, kDiagonal);
    bar(kDiagonal, -kDiagonal);
}

} // namespace rm
