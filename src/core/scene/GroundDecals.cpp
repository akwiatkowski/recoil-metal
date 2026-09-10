#include "core/scene/GroundDecals.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm {

/// The colour a wreck's scorch is drawn in, at its darkest.
///
/// Nearly black and half transparent, so it darkens whatever ground it lies on rather than
/// painting a grey circle on it — a scorch on sand and a scorch on grass are the same
/// scorch, and an opaque colour would make them the same colour too.
constexpr std::array<float, 4> kWreckCentreColour{{0.05f, 0.04f, 0.03f, 0.55f}};

void appendShieldSphere(std::vector<DecalVertex>& out, std::array<float, 3> centre,
                        float radiusElmos, std::array<float, 4> colour) {
    if (!(radiusElmos > 0.0f)) {
        return;
    }

    // An icosahedron gives the shell even triangles without a UV sphere's pinched poles. One
    // midpoint subdivision is 80 triangles: round enough at RTS distance, only 240 vertices.
    constexpr float phi = 1.61803398875f;
    constexpr std::array<std::array<float, 3>, 12> points{{
        {{-1.0f, phi, 0.0f}}, {{1.0f, phi, 0.0f}}, {{-1.0f, -phi, 0.0f}},
        {{1.0f, -phi, 0.0f}}, {{0.0f, -1.0f, phi}}, {{0.0f, 1.0f, phi}},
        {{0.0f, -1.0f, -phi}}, {{0.0f, 1.0f, -phi}}, {{phi, 0.0f, -1.0f}},
        {{phi, 0.0f, 1.0f}}, {{-phi, 0.0f, -1.0f}}, {{-phi, 0.0f, 1.0f}},
    }};
    constexpr std::array<std::array<std::size_t, 3>, 20> faces{{
        {{0, 11, 5}}, {{0, 5, 1}}, {{0, 1, 7}}, {{0, 7, 10}}, {{0, 10, 11}},
        {{1, 5, 9}}, {{5, 11, 4}}, {{11, 10, 2}}, {{10, 7, 6}}, {{7, 1, 8}},
        {{3, 9, 4}}, {{3, 4, 2}}, {{3, 2, 6}}, {{3, 6, 8}}, {{3, 8, 9}},
        {{4, 9, 5}}, {{2, 4, 11}}, {{6, 2, 10}}, {{8, 6, 7}}, {{9, 8, 1}},
    }};

    const auto normal = [](std::array<float, 3> point) {
        const float length = std::sqrt(point[0] * point[0] + point[1] * point[1]
                                       + point[2] * point[2]);
        return std::array<float, 3>{point[0] / length, point[1] / length,
                                    point[2] / length};
    };
    const auto midpoint = [&](std::array<float, 3> a, std::array<float, 3> b) {
        return normal({a[0] + b[0], a[1] + b[1], a[2] + b[2]});
    };
    const auto vertex = [&](std::array<float, 3> point) {
        point = normal(point);
        return DecalVertex{
            .position = {centre[0] + point[0] * radiusElmos,
                         centre[1] + point[1] * radiusElmos,
                         centre[2] + point[2] * radiusElmos},
            .colour = colour,
        };
    };
    const auto triangle = [&](std::array<float, 3> a, std::array<float, 3> b,
                              std::array<float, 3> c) {
        out.push_back(vertex(a));
        out.push_back(vertex(b));
        out.push_back(vertex(c));
    };

    out.reserve(out.size() + shieldSphereVertexCount());
    for (const auto& face : faces) {
        const std::array<float, 3> a = normal(points[face[0]]);
        const std::array<float, 3> b = normal(points[face[1]]);
        const std::array<float, 3> c = normal(points[face[2]]);
        const std::array<float, 3> ab = midpoint(a, b);
        const std::array<float, 3> bc = midpoint(b, c);
        const std::array<float, 3> ca = midpoint(c, a);
        triangle(a, ab, ca);
        triangle(b, bc, ab);
        triangle(c, ca, bc);
        triangle(ab, bc, ca);
    }
}

void appendWreckMark(std::vector<DecalVertex>& out, const HeightField& field,
                     std::array<float, 3> centre, float radiusElmos, int segments) {
    if (!(radiusElmos > 0.0f) || segments < 3) {
        return;
    }

    // Height per vertex, from the ground under THAT point — the same rule the rings follow,
    // and for the same reason: a disc at the unit's own height buries its uphill half.
    const auto vertexAt = [&](float angle, float radius, std::array<float, 4> colour) {
        const float x = centre[0] + std::cos(angle) * radius;
        const float z = centre[2] + std::sin(angle) * radius;
        return DecalVertex{
            .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
            .colour = colour,
        };
    };

    // Transparent at the rim, so the scorch fades out instead of ending at a drawn edge.
    std::array<float, 4> rim = kWreckCentreColour;
    rim[3] = 0.0f;

    const auto middle = vertexAt(0.0f, 0.0f, kWreckCentreColour);

    out.reserve(out.size() + wreckVertexCount(segments));
    const float step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1);

        // A FAN, one triangle per segment, all sharing the centre. The centre vertex is
        // recomputed rather than hoisted only in the sense that it is copied — its height is
        // sampled once, above, because the middle of a disc has one ground under it.
        out.push_back(middle);
        out.push_back(vertexAt(a0, radiusElmos, rim));
        out.push_back(vertexAt(a1, radiusElmos, rim));
    }
}

static void appendSelectionOutline(std::vector<DecalVertex>& out, const HeightField& field,
                         std::array<float, 3> centre, float radiusElmos,
                         std::array<float, 4> colour, float thicknessElmos, int segments,
                         bool square, float chamfer = 0.0f) {
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
        float dx = std::cos(angle), dz = std::sin(angle);
        if (square) {
            // Project the circle onto an axis-aligned square, retaining terrain samples.
            const float edge = std::max(std::abs(dx), std::abs(dz));
            dx /= edge;
            dz /= edge;
            if (chamfer > 0.0f) {
                // Then cut each corner along the 45-degree line |dx| + |dz| == 2 - chamfer.
                // Scaling the ray keeps the direction, so the angular samples spread along
                // the cut instead of bunching at its midpoint.
                const float limit = 2.0f - chamfer;
                const float sum = std::abs(dx) + std::abs(dz);
                if (sum > limit) {
                    const float scale = limit / sum;
                    dx *= scale;
                    dz *= scale;
                }
            }
        }
        const float x = centre[0] + dx * radius;
        const float z = centre[2] + dz * radius;
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

namespace {

/// A diagonal ground-following cross: two bars at 45 degrees, each split along its length
/// so it follows the heightmap squares instead of chording across them. Factored out of the
/// order marker so the no-route mark can be the cross ALONE — the shape is the shared part,
/// what it means is the caller's.
void appendGroundCross(std::vector<DecalVertex>& out, const HeightField& field,
                       std::array<float, 3> centre, std::array<float, 4> colour, float reach,
                       float halfWidth) {
    const auto bar = [&](float alongX, float alongZ) {
        const float acrossX = -alongZ;
        const float acrossZ = alongX;

        const auto corner = [&](float along, float across) {
            const float x = centre[0] + alongX * along + acrossX * across;
            const float z = centre[2] + alongZ * along + acrossZ * across;
            return DecalVertex{
                .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
                .colour = colour,
            };
        };

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

    // Diagonal rather than axis-aligned: a cross lying along X and Z reads as a grid
    // artefact on terrain built from an axis-aligned heightfield.
    constexpr float kDiagonal = 0.70710678f;  // 1/sqrt(2)
    bar(kDiagonal, kDiagonal);
    bar(kDiagonal, -kDiagonal);
}

} // namespace

void appendNoRouteMarker(std::vector<DecalVertex>& out, const HeightField& field,
                         std::array<float, 3> centre, float age) {
    if (age < 0.0f || age >= kOrderMarkerSecondsToLive) {
        return;  // expired, silently — same contract as the order marker
    }

    // The refusal mark: the cross ALONE, in the loss colour, at the UNIT that cannot go.
    // No ring, deliberately — a ring is what an acknowledged thing looks like here, and
    // this is the opposite of acknowledged. It fades without shrinking: "the order landed
    // HERE" is the contraction's message, and nothing landed.
    const float remaining = 1.0f - age / kOrderMarkerSecondsToLive;
    const std::array<float, 4> faded{{0.894f, 0.341f, 0.239f,  // kLoss, the fixed palette
                                      0.9f * remaining * remaining}};
    appendGroundCross(out, field, centre, faded, kOrderMarkerRadiusElmos * 0.6f,
                      kRingThicknessElmos * 0.45f);
}

void appendGroundSegment(std::vector<DecalVertex>& out, const HeightField& field,
                         std::array<float, 2> fromXZ, std::array<float, 2> toXZ,
                         std::array<float, 4> colour, float widthElmos) {
    const float dx = toXZ[0] - fromXZ[0];
    const float dz = toXZ[1] - fromXZ[1];
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!(length > 0.001f) || !(widthElmos > 0.0f)) {
        return;  // a zero-length segment has no direction to give the bar its width
    }

    const float alongX = dx / length;
    const float alongZ = dz / length;
    const float acrossX = -alongZ * widthElmos * 0.5f;
    const float acrossZ = alongX * widthElmos * 0.5f;

    // One sample about every heightmap square (8 elmos), at least two — the same pitch the
    // cross uses and for the same reason: a chord across a gully buries the line.
    const int segments = std::max(2, static_cast<int>(std::ceil(length / 8.0f)));
    const auto corner = [&](float t, float side) {
        const float x = fromXZ[0] + dx * t + acrossX * side;
        const float z = fromXZ[1] + dz * t + acrossZ * side;
        return DecalVertex{
            .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
            .colour = colour,
        };
    };
    for (int i = 0; i < segments; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(segments);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        const DecalVertex left0 = corner(t0, -1.0f);
        const DecalVertex right0 = corner(t0, 1.0f);
        const DecalVertex left1 = corner(t1, -1.0f);
        const DecalVertex right1 = corner(t1, 1.0f);
        out.push_back(left0);
        out.push_back(right0);
        out.push_back(right1);
        out.push_back(left0);
        out.push_back(right1);
        out.push_back(left1);
    }
}

void appendGroundNode(std::vector<DecalVertex>& out, const HeightField& field,
                      std::array<float, 2> atXZ, std::array<float, 4> colour,
                      float halfElmos) {
    if (!(halfElmos > 0.0f)) {
        return;
    }
    const auto point = [&](float ox, float oz) {
        const float x = atXZ[0] + ox;
        const float z = atXZ[1] + oz;
        return DecalVertex{
            .position = {x, field.heightAtWorld(x, z) + kRingLiftElmos, z},
            .colour = colour,
        };
    };
    // Two triangles over the four compass points: the diamond.
    const DecalVertex north = point(0.0f, -halfElmos);
    const DecalVertex east = point(halfElmos, 0.0f);
    const DecalVertex south = point(0.0f, halfElmos);
    const DecalVertex west = point(-halfElmos, 0.0f);
    out.push_back(north);
    out.push_back(east);
    out.push_back(south);
    out.push_back(north);
    out.push_back(south);
    out.push_back(west);
}

void appendSelectionRing(std::vector<DecalVertex>& out, const HeightField& field,
                         std::array<float, 3> centre, float radiusElmos,
                         std::array<float, 4> colour, float thicknessElmos, int segments) {
    appendSelectionOutline(out, field, centre, radiusElmos, colour, thicknessElmos, segments, false);
}

void appendSelectionSquare(std::vector<DecalVertex>& out, const HeightField& field,
                           std::array<float, 3> centre, float halfExtentElmos,
                           std::array<float, 4> colour) {
    appendSelectionOutline(out, field, centre, halfExtentElmos, colour,
                           kRingThicknessElmos, kRingSegments, true);
}

void appendSelectionChamferedSquare(std::vector<DecalVertex>& out, const HeightField& field,
                                    std::array<float, 3> centre, float halfExtentElmos,
                                    std::array<float, 4> colour, float thicknessElmos,
                                    float chamferFraction) {
    appendSelectionOutline(out, field, centre, halfExtentElmos, colour, thicknessElmos,
                           kRingSegments, true, chamferFraction);
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
    // bridging it. The reach stops inside the ring rather than at it.
    appendGroundCross(out, field, centre, faded, radius * 0.72f,
                      kRingThicknessElmos * 0.35f);
}

} // namespace rm
