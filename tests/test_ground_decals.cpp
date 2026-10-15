// Ground-decal geometry: the ring under a selected unit, and the marker where an
// order was given. These are the interface this renderer draws, and "is the unit I
// clicked the one that is highlighted" is a question nobody can answer from a
// screenshot when two units stand close together — so the shapes are pinned here
// rather than judged by looking.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/TickRate.hpp"

#include "app/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::HeightField;
using rm::DecalVertex;

namespace {

constexpr float kElmosPerRawUnit = 0.0625f;
constexpr std::array<float, 4> kWhite{{1.0f, 1.0f, 1.0f, 1.0f}};

[[nodiscard]] HeightField makeField(int squares, std::vector<std::uint16_t> raw) {
    HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = 0.0f;
    field.heightScale = kElmosPerRawUnit;
    field.raw = std::move(raw);
    return field;
}

/// Flat ground at a chosen raw height.
[[nodiscard]] HeightField flatAt(int squares, std::uint16_t raw) {
    const auto n = static_cast<std::size_t>(squares + 1);
    return makeField(squares, std::vector<std::uint16_t>(n * n, raw));
}

/// Ground that rises steadily along +X, so a ring spans real relief.
[[nodiscard]] HeightField rampAlongX(int squares) {
    const auto n = static_cast<std::size_t>(squares + 1);
    std::vector<std::uint16_t> raw(n * n);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            raw[z * n + x] = static_cast<std::uint16_t>(x * 200);
        }
    }
    return makeField(squares, std::move(raw));
}

} // namespace

TEST_CASE("a shield sphere is closed triangle geometry at the requested radius") {
    const std::array<float, 3> centre{{100.0f, 40.0f, 200.0f}};
    const std::array<float, 4> cyan{{0.2f, 0.75f, 1.0f, 0.12f}};
    constexpr float radius = 80.0f;

    std::vector<DecalVertex> out;
    rm::appendShieldSphere(out, centre, radius, cyan);

    REQUIRE(out.size() == rm::shieldSphereVertexCount());
    REQUIRE(out.size() % 3u == 0u);
    for (const DecalVertex& vertex : out) {
        const float dx = vertex.position[0] - centre[0];
        const float dy = vertex.position[1] - centre[1];
        const float dz = vertex.position[2] - centre[2];
        CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) == Approx(radius).margin(1e-3));
        CHECK(vertex.colour == cyan);
    }
}

TEST_CASE("a shield sphere appends and rejects a non-positive radius") {
    std::vector<DecalVertex> out(3);
    rm::appendShieldSphere(out, {{0.0f, 0.0f, 0.0f}}, 0.0f, kWhite);
    rm::appendShieldSphere(out, {{0.0f, 0.0f, 0.0f}}, -1.0f, kWhite);
    CHECK(out.size() == 3);

    rm::appendShieldSphere(out, {{0.0f, 0.0f, 0.0f}}, 1.0f, kWhite);
    CHECK(out.size() == 3u + rm::shieldSphereVertexCount());
}

TEST_CASE("a ring is two triangles per segment and nothing else") {
    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, flatAt(16, 0), {{64.0f, 0.0f, 64.0f}}, 10.0f, kWhite);

    CHECK(out.size() == rm::ringVertexCount());
    CHECK(out.size() == static_cast<std::size_t>(rm::kRingSegments) * 6u);
}

TEST_CASE("appending accumulates rather than replacing") {
    // One buffer holds the whole selection, so a second call must add to it.
    std::vector<DecalVertex> out;
    const HeightField field = flatAt(16, 0);

    rm::appendSelectionRing(out, field, {{32.0f, 0.0f, 32.0f}}, 8.0f, kWhite);
    const std::size_t afterFirst = out.size();
    rm::appendSelectionRing(out, field, {{64.0f, 0.0f, 64.0f}}, 8.0f, kWhite);

    CHECK(afterFirst == rm::ringVertexCount());
    CHECK(out.size() == rm::ringVertexCount() * 2u);
}

TEST_CASE("every vertex lands in the band, at the right distance from the centre") {
    const float radius = 20.0f;
    const float thickness = rm::kRingThicknessElmos;
    const std::array<float, 3> centre{{100.0f, 0.0f, 100.0f}};

    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, radius, kWhite);

    REQUIRE_FALSE(out.empty());

    bool sawInner = false;
    bool sawOuter = false;
    for (const DecalVertex& v : out) {
        const float dx = v.position[0] - centre[0];
        const float dz = v.position[2] - centre[2];
        const float distance = std::sqrt(dx * dx + dz * dz);

        // Every vertex is on one rim or the other — a ring, not a disc. A disc
        // would hide the unit's own feet under a coloured plate.
        const bool inner = distance == Approx(radius - thickness * 0.5f).margin(1e-3);
        const bool outer = distance == Approx(radius + thickness * 0.5f).margin(1e-3);
        INFO("distance " << distance << " for radius " << radius);
        REQUIRE((inner || outer));
        sawInner = sawInner || inner;
        sawOuter = sawOuter || outer;
    }

    CHECK(sawInner);
    CHECK(sawOuter);
}

TEST_CASE("the ring closes") {
    // The last segment must join the first. An off-by-one in the angle step
    // leaves a gap that reads as the ring being for a different unit.
    const std::array<float, 3> centre{{100.0f, 0.0f, 100.0f}};
    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, 20.0f, kWhite);

    // Angles of every vertex, sorted: consecutive ones must never be further
    // apart than one segment.
    std::vector<float> angles;
    angles.reserve(out.size());
    for (const DecalVertex& v : out) {
        angles.push_back(std::atan2(v.position[2] - centre[2], v.position[0] - centre[0]));
    }
    std::sort(angles.begin(), angles.end());

    const float step = 6.2831853f / static_cast<float>(rm::kRingSegments);
    for (std::size_t i = 1; i < angles.size(); ++i) {
        REQUIRE(angles[i] - angles[i - 1] < step * 1.5f);
    }
    // And the wrap from the last back to the first.
    const float wrap = (angles.front() + 6.2831853f) - angles.back();
    REQUIRE(wrap < step * 1.5f);
}

TEST_CASE("the ring follows the ground rather than the unit standing on it") {
    // The property a flat tilted disc cannot have. On a ramp the far side of a
    // ring is metres above the near side, and a ring drawn at the unit's own
    // height buries half of itself.
    const HeightField field = rampAlongX(64);

    const std::array<float, 3> centre{{200.0f, 0.0f, 200.0f}};
    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, field, centre, 30.0f, kWhite);

    float lowest = 1e9f;
    float highest = -1e9f;
    for (const DecalVertex& v : out) {
        lowest = std::min(lowest, v.position[1]);
        highest = std::max(highest, v.position[1]);

        // Each vertex sits its own lift above the ground beneath IT.
        const float ground = field.heightAtWorld(v.position[0], v.position[2]);
        CHECK(v.position[1] == Approx(ground + rm::kRingLiftElmos).margin(1e-2));
    }

    // And the ramp really did make a difference, so the check above is not
    // vacuously passing on flat ground.
    CHECK(highest - lowest > 1.0f);
}

TEST_CASE("the ring clears the ground it sits on") {
    // Coplanar with the terrain means z-fighting, which speckles the band.
    const HeightField field = flatAt(64, 800);
    const float ground = field.heightAtWorld(100.0f, 100.0f);

    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, field, {{100.0f, 0.0f, 100.0f}}, 15.0f, kWhite);

    for (const DecalVertex& v : out) {
        CHECK(v.position[1] > ground);
    }
}

TEST_CASE("the colour is carried on every vertex") {
    // Per vertex, so one draw covers a mixed selection.
    const std::array<float, 4> teal{{0.0f, 0.8f, 0.7f, 0.55f}};
    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, flatAt(16, 0), {{64.0f, 0.0f, 64.0f}}, 8.0f, teal);

    for (const DecalVertex& v : out) {
        CHECK(v.colour[0] == Approx(teal[0]));
        CHECK(v.colour[1] == Approx(teal[1]));
        CHECK(v.colour[2] == Approx(teal[2]));
        CHECK(v.colour[3] == Approx(teal[3]));
    }
}

TEST_CASE("a nonsensical ring produces nothing rather than garbage") {
    const HeightField field = flatAt(16, 0);
    const std::array<float, 3> centre{{64.0f, 0.0f, 64.0f}};

    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, field, centre, 0.0f, kWhite);
    CHECK(out.empty());

    rm::appendSelectionRing(out, field, centre, -5.0f, kWhite);
    CHECK(out.empty());

    rm::appendSelectionRing(out, field, centre, 10.0f, kWhite, 0.0f);
    CHECK(out.empty());

    rm::appendSelectionRing(out, field, centre, 10.0f, kWhite, 2.0f, 2);
    CHECK(out.empty());
}

TEST_CASE("a ring wider than its radius does not turn inside out") {
    // Clamped rather than allowed to produce a negative inner radius, which
    // would fold the band through the centre and render as a bow tie.
    const std::array<float, 3> centre{{100.0f, 0.0f, 100.0f}};
    std::vector<DecalVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, 4.0f, kWhite, /*thickness=*/20.0f);

    REQUIRE_FALSE(out.empty());
    for (const DecalVertex& v : out) {
        const float dx = v.position[0] - centre[0];
        const float dz = v.position[2] - centre[2];
        CHECK(std::sqrt(dx * dx + dz * dz) >= Approx(0.0f).margin(1e-4));
    }
}

// --- Order markers -----------------------------------------------------------
// Where a right-click landed. A ring like a selection's, plus a cross through it,
// shrinking and fading over a second and a half.

TEST_CASE("an order marker is a ring and a cross, all on the ground") {
    std::vector<DecalVertex> out;
    rm::appendOrderMarker(out, flatAt(64, 800), {{200.0f, 0.0f, 200.0f}}, kWhite, /*age=*/0.0f);

    // A ring's worth, plus two bars of four quads each.
    REQUIRE(out.size() == rm::ringVertexCount() + 2u * 4u * 6u);

    const float ground = 800.0f * kElmosPerRawUnit + rm::kRingLiftElmos;
    for (const DecalVertex& vertex : out) {
        CHECK(vertex.position[1] == Approx(ground));
    }
}

TEST_CASE("an order marker takes its height from the ground under each point") {
    // The same rule the ring follows, and it matters more for the cross: a bar is
    // 20 elmos long across a heightfield whose squares are 8, so a bar drawn at
    // one height chords over two and a half squares of relief and sinks into any
    // slope. Segmented along its length for exactly this.
    std::vector<DecalVertex> out;
    rm::appendOrderMarker(out, rampAlongX(64), {{200.0f, 0.0f, 200.0f}}, kWhite, 0.0f);
    REQUIRE_FALSE(out.empty());

    float lowest = out.front().position[1];
    float highest = lowest;
    for (const DecalVertex& vertex : out) {
        lowest = std::min(lowest, vertex.position[1]);
        highest = std::max(highest, vertex.position[1]);
    }
    // The ramp climbs 200 raw units per square, which is 12.5 elmos, and the
    // marker spans about three squares — so a flat marker would be metres out.
    CHECK(highest - lowest > 10.0f);
}

TEST_CASE("an order marker shrinks toward the point it marks, and fades") {
    const HeightField field = flatAt(64, 0);
    const std::array<float, 3> centre{{200.0f, 0.0f, 200.0f}};

    const auto spanOf = [](const std::vector<DecalVertex>& out) {
        float lowX = out.front().position[0];
        float highX = lowX;
        for (const DecalVertex& vertex : out) {
            lowX = std::min(lowX, vertex.position[0]);
            highX = std::max(highX, vertex.position[0]);
        }
        return highX - lowX;
    };

    std::vector<DecalVertex> fresh;
    rm::appendOrderMarker(fresh, field, centre, kWhite, 0.0f);
    std::vector<DecalVertex> old;
    rm::appendOrderMarker(old, field, centre, kWhite, rm::kOrderMarkerSecondsToLive * 0.9f);

    REQUIRE_FALSE(fresh.empty());
    REQUIRE_FALSE(old.empty());
    CHECK(spanOf(old) < spanOf(fresh));
    // ...but never to a dot: the last visible frame is still a marker.
    CHECK(spanOf(old) > 0.3f * spanOf(fresh));

    CHECK(old.front().colour[3] < fresh.front().colour[3]);
    CHECK(fresh.front().colour[3] == Approx(kWhite[3]));
}

TEST_CASE("an expired order marker contributes nothing, and says so by staying silent") {
    // So the app can hold one marker per order and offer them every frame without
    // tracking which have run out.
    const HeightField field = flatAt(64, 0);
    std::vector<DecalVertex> out;

    rm::appendOrderMarker(out, field, {{200.0f, 0.0f, 200.0f}}, kWhite,
                          rm::kOrderMarkerSecondsToLive);
    CHECK(out.empty());

    rm::appendOrderMarker(out, field, {{200.0f, 0.0f, 200.0f}}, kWhite, 999.0f);
    CHECK(out.empty());

    // A negative age is a clock that went backwards, not a marker from the
    // future: refused rather than drawn at full size.
    rm::appendOrderMarker(out, field, {{200.0f, 0.0f, 200.0f}}, kWhite, -0.5f);
    CHECK(out.empty());
}

TEST_CASE("an order marker's cross is diagonal, not axis-aligned") {
    // A cross along X and Z reads as a grid artefact on terrain built from an
    // axis-aligned heightfield. Checked by looking for vertices off both axes:
    // the bars run at 45 degrees, so the four arm tips are the extremes in x AND z
    // at once, which an axis-aligned cross would never produce.
    std::vector<DecalVertex> out;
    rm::appendOrderMarker(out, flatAt(64, 0), {{0.0f, 0.0f, 0.0f}}, kWhite, 0.0f);

    bool foundDiagonalTip = false;
    for (const DecalVertex& vertex : out) {
        const float x = std::abs(vertex.position[0]);
        const float z = std::abs(vertex.position[2]);
        if (x > 0.5f * rm::kOrderMarkerRadiusElmos && z > 0.5f * rm::kOrderMarkerRadiusElmos) {
            foundDiagonalTip = true;
            break;
        }
    }
    CHECK(foundDiagonalTip);
}

TEST_CASE("a wreck mark is a filled disc, dark in the middle and gone at the rim") {
    const HeightField field = flatAt(64, 0);
    std::vector<rm::DecalVertex> vertices;

    rm::appendWreckMark(vertices, field, {{100.0f, 0.0f, 100.0f}}, 20.0f);

    // A fan: one triangle per segment, all sharing the centre — where a ring is a band and
    // needs two.
    REQUIRE(vertices.size() == rm::wreckVertexCount());

    // Every triangle's first vertex is the centre, and it is the opaque one. A hard-edged
    // disc reads as a painted circle, and scorching does not have an edge.
    for (std::size_t i = 0; i < vertices.size(); i += 3) {
        CHECK(vertices[i].position[0] == Catch::Approx(100.0f));
        CHECK(vertices[i].position[2] == Catch::Approx(100.0f));
        CHECK(vertices[i].colour[3] > 0.0f);
        CHECK(vertices[i + 1].colour[3] == Catch::Approx(0.0f));
        CHECK(vertices[i + 2].colour[3] == Catch::Approx(0.0f));
    }

    // Dark rather than team-coloured: a battlefield with team-coloured wrecks reads as one
    // still occupied by both teams.
    CHECK(vertices.front().colour[0] < 0.2f);
    CHECK(vertices.front().colour[1] < 0.2f);
    CHECK(vertices.front().colour[2] < 0.2f);
}

TEST_CASE("a wreck mark with no radius draws nothing") {
    const HeightField field = flatAt(64, 0);
    std::vector<rm::DecalVertex> vertices;
    rm::appendWreckMark(vertices, field, {{0.0f, 0.0f, 0.0f}}, 0.0f);
    CHECK(vertices.empty());
    rm::appendWreckMark(vertices, field, {{0.0f, 0.0f, 0.0f}}, 20.0f, 2);
    CHECK(vertices.empty());  // two segments is not a disc
}

TEST_CASE("the no-route mark is a bare cross that fades without shrinking") {
    const HeightField field = flatAt(64, 0);
    std::vector<rm::DecalVertex> young;
    rm::appendNoRouteMarker(young, field, {100.0f, 0.0f, 100.0f}, 0.0f);

    // Two diagonal bars of four segments, six vertices each — and no ring: a ring is what
    // an acknowledged thing looks like, and a refusal is the opposite.
    CHECK(young.size() == 2 * 4 * 6);

    // Same footprint later in life (no shrink), lower alpha (the fade).
    std::vector<rm::DecalVertex> old;
    rm::appendNoRouteMarker(old, field, {100.0f, 0.0f, 100.0f}, 1.0f);
    REQUIRE(old.size() == young.size());
    CHECK(old[0].position[0] == young[0].position[0]);
    CHECK(old[0].colour[3] < young[0].colour[3]);

    // Expired: nothing, silently — the caller may keep asking.
    std::vector<rm::DecalVertex> dead;
    rm::appendNoRouteMarker(dead, field, {100.0f, 0.0f, 100.0f},
                            rm::kOrderMarkerSecondsToLive);
    CHECK(dead.empty());
}

TEST_CASE("a ground segment samples along its length and a zero-length one draws nothing") {
    const HeightField field = flatAt(64, 0);
    std::vector<rm::DecalVertex> out;

    // 40 elmos at one sample per 8-elmo square: five spans, six vertices each.
    rm::appendGroundSegment(out, field, {100.0f, 100.0f}, {140.0f, 100.0f},
                            {{1.0f, 1.0f, 1.0f, 1.0f}}, 2.0f);
    CHECK(out.size() == 5 * 6);

    std::vector<rm::DecalVertex> nothing;
    rm::appendGroundSegment(nothing, field, {100.0f, 100.0f}, {100.0f, 100.0f},
                            {{1.0f, 1.0f, 1.0f, 1.0f}}, 2.0f);
    CHECK(nothing.empty());
}

TEST_CASE("a node is one diamond, centred where it was asked for") {
    const HeightField field = flatAt(64, 0);
    std::vector<rm::DecalVertex> out;
    rm::appendGroundNode(out, field, {200.0f, 200.0f}, {{1.0f, 1.0f, 1.0f, 1.0f}}, 4.0f);
    REQUIRE(out.size() == 6);

    // The four compass points straddle the centre symmetrically.
    float minX = out[0].position[0];
    float maxX = minX;
    for (const rm::DecalVertex& vertex : out) {
        minX = std::min(minX, vertex.position[0]);
        maxX = std::max(maxX, vertex.position[0]);
    }
    CHECK(minX == 196.0f);
    CHECK(maxX == 204.0f);
}

TEST_CASE("square selection outlines follow the grid and terrain", "[selection-square]") {
    const auto field = rampAlongX(64);
    std::vector<DecalVertex> vertices;
    rm::appendSelectionSquare(vertices, field, {100, 0, 100}, 20, {0, 1, 0, 1});
    REQUIRE(vertices.size() == rm::ringVertexCount(rm::kRingSegments));
    for (const auto& vertex : vertices) {
        const auto& p = vertex.position;
        const float distance = std::max(std::abs(p[0] - 100), std::abs(p[2] - 100));
        CHECK((distance == Approx(20 - rm::kRingThicknessElmos / 2)
            || distance == Approx(20 + rm::kRingThicknessElmos / 2)));
        CHECK(p[1] == Approx(field.heightAtWorld(p[0], p[2]) + rm::kRingLiftElmos));
    }
}

TEST_CASE("impact marks draw scorch beside the wrecks", "[decals]") {
    const HeightField field = flatAt(64, 0);
    rm::app::UnitScene scene;
    rm::app::noteImpactMark(scene, 10.0f, 20.0f);
    rm::app::noteImpactMark(scene, 30.0f, 40.0f);
    REQUIRE(scene.impactMarks.size() == 2);
    rm::app::refreshWreckDecals(scene, field);
    // Two small fans beside zero wreck marks: every triangle's first vertex is
    // its own dark centre, at the impact point.
    REQUIRE(scene.wreckDecals.size() == 2 * rm::wreckVertexCount(12));
    CHECK(scene.wreckDecals[0].position[0] == Approx(10.0f));
    CHECK(scene.wreckDecals[0].position[2] == Approx(20.0f));
    CHECK(scene.wreckDecals[0].colour[3] > 0.0f);
    const std::size_t second = rm::wreckVertexCount(12);
    CHECK(scene.wreckDecals[second].position[0] == Approx(30.0f));
    // A second refresh with nothing new draws nothing new: same vertices.
    rm::app::refreshWreckDecals(scene, field);
    CHECK(scene.wreckDecals.size() == 2 * rm::wreckVertexCount(12));
}
TEST_CASE("impact marks cover the blast that made them", "[decals]") {
    // Retail scorches by explosion scale (`defaultexplosions.lua` C-290):
    // scale above 1.2 lays a decal at x3, otherwise a splat at x4. Marks
    // carry the shot's blast radius in elmos, so the scale is radius/8
    // (8 elmos per ogrid, Scmap.hpp `kElmosPerOgrid`): 8 elmos is scale 1.0
    // and draws 32 elmos; 24 elmos is scale 3.0 and draws 72 elmos. A direct
    // hit with no blast keeps the legacy 2.5-elmo boot-print.
    const HeightField field = flatAt(64, 0);
    rm::app::UnitScene direct;
    rm::app::UnitScene splat;
    rm::app::UnitScene decal;
    rm::app::noteImpactMark(direct, 10.0f, 20.0f, 0.0f);
    rm::app::noteImpactMark(splat, 10.0f, 20.0f, 8.0f);
    rm::app::noteImpactMark(decal, 10.0f, 20.0f, 24.0f);
    rm::app::refreshWreckDecals(direct, field);
    rm::app::refreshWreckDecals(splat, field);
    rm::app::refreshWreckDecals(decal, field);
    const auto extent = [](const std::vector<DecalVertex>& out) {
        float far = 0.0f;
        for (const DecalVertex& vertex : out) {
            const float dx = vertex.position[0] - 10.0f;
            const float dz = vertex.position[2] - 20.0f;
            far = std::max(far, std::sqrt(dx * dx + dz * dz));
        }
        return far;
    };
    CHECK(extent(direct.wreckDecals) == Approx(2.5f));
    CHECK(extent(splat.wreckDecals) == Approx(32.0f));
    CHECK(extent(decal.wreckDecals) == Approx(72.0f));
}
TEST_CASE("impact marks record retail's texture pick", "[decals]") {
    // Retail lays one of 10 splat textures (`scorch_001`-`010_albedo`) or one
    // of 2 decal textures (`scorch_*_albedo`) per scorch, picked at random
    // (`defaultexplosions.lua` C-290). Ours records the pick from the mark's
    // own bits — replay-identical, off the sim RNG (C-297) — so the future
    // FA-PRESENT texture projection samples what retail would have drawn.
    // Splat-scale marks (scale <= 1.2, blast <= 9.6 elmos through 8 elmos per
    // ogrid) pick 0..9; decal-scale marks pick 0..1. A blast-less direct hit
    // rides the splat path.
    rm::app::UnitScene splat;
    rm::app::noteImpactMark(splat, 10.0f, 20.0f, 8.0f);
    REQUIRE(splat.impactMarks.size() == 1);
    CHECK(splat.impactMarks[0].textureIndex >= 0);
    CHECK(splat.impactMarks[0].textureIndex < 10);

    rm::app::UnitScene decal;
    rm::app::noteImpactMark(decal, 10.0f, 20.0f, 24.0f);
    REQUIRE(decal.impactMarks.size() == 1);
    CHECK(decal.impactMarks[0].textureIndex >= 0);
    CHECK(decal.impactMarks[0].textureIndex < 2);

    rm::app::UnitScene direct;
    rm::app::noteImpactMark(direct, 10.0f, 20.0f, 0.0f);
    REQUIRE(direct.impactMarks.size() == 1);
    CHECK(direct.impactMarks[0].textureIndex >= 0);
    CHECK(direct.impactMarks[0].textureIndex < 10);

    // Variety is real across marks, and replay-identical per mark.
    std::vector<int> picks;
    for (int i = 0; i < 64; ++i) {
        rm::app::UnitScene scene;
        rm::app::noteImpactMark(scene, static_cast<float>(i) * 1.3f,
                                static_cast<float>(i) * 0.7f, 8.0f);
        picks.push_back(scene.impactMarks[0].textureIndex);
    }
    std::sort(picks.begin(), picks.end());
    CHECK(std::unique(picks.begin(), picks.end()) - picks.begin() >= 2);

    rm::app::UnitScene replay;
    rm::app::noteImpactMark(replay, 10.0f, 20.0f, 8.0f);
    CHECK(replay.impactMarks[0].textureIndex == splat.impactMarks[0].textureIndex);
}
TEST_CASE("impact marks carry deterministic rotation", "[decals]") {
    // Retail rotates every scorch by a random 0..2pi angle (`defaultexplosions.lua`
    // C-290: `GetRandomFloat(0,2*math.pi)` in both `CreateScorchMarkSplat` and
    // `CreateScorchMarkDecal`). Ours derives the angle from the mark's own bits —
    // presentation-only, no sim RNG spent (C-297) — so a replay draws identically.
    const HeightField field = flatAt(64, 0);
    rm::app::UnitScene scene;
    rm::app::noteImpactMark(scene, 10.0f, 20.0f);
    rm::app::noteImpactMark(scene, 30.0f, 40.0f);
    rm::app::refreshWreckDecals(scene, field);
    const auto rimAngle = [](const std::vector<DecalVertex>& out, std::size_t base,
                             float cx, float cz) {
        const float dx = out[base + 1].position[0] - cx;
        const float dz = out[base + 1].position[2] - cz;
        return std::atan2(dz, dx);
    };
    const std::size_t stride = rm::wreckVertexCount(12);
    REQUIRE(scene.wreckDecals.size() == 2 * stride);
    // Different marks land at different angles: no shared axis-aligned fan.
    CHECK(rimAngle(scene.wreckDecals, 0, 10.0f, 20.0f)
          != Approx(rimAngle(scene.wreckDecals, stride, 30.0f, 40.0f)));
    // The same marks rebuilt draw bit-identically: rotation is a pure function
    // of the mark, not a draw off any RNG stream.
    rm::app::UnitScene replay;
    rm::app::noteImpactMark(replay, 10.0f, 20.0f);
    rm::app::noteImpactMark(replay, 30.0f, 40.0f);
    rm::app::refreshWreckDecals(replay, field);
    REQUIRE(replay.wreckDecals.size() == scene.wreckDecals.size());
    for (std::size_t i = 0; i < scene.wreckDecals.size(); ++i) {
        CHECK(scene.wreckDecals[i].position[0] == Approx(replay.wreckDecals[i].position[0]));
        CHECK(scene.wreckDecals[i].position[2] == Approx(replay.wreckDecals[i].position[2]));
    }
}
TEST_CASE("impact marks expire inside retail's scorch lifetime spread", "[decals]") {
    // Retail scorch lives 300-600 s (`defaultexplosions.lua` C-290); each
    // mark retires at its own bit-derived point inside the spread (see the
    // spread test below). The invariant here: nothing retires before the
    // 300 s floor, nothing survives the 600 s ceiling — derived through the
    // tick rate rather than a literal so multi-rate runs agree.
    const HeightField field = flatAt(64, 0);
    const rm::sim::TickRate rate{};
    const rm::TickIndex floor = rate.ticks(rm::sim::seconds(300.0f));  // ticks, s (C-290)
    const rm::TickIndex ceiling = rate.ticks(rm::sim::seconds(600.0f));  // ticks, s (C-290)
    rm::app::UnitScene scene;
    rm::app::noteImpactMark(scene, 10.0f, 20.0f, 0.0f, 0);
    rm::app::refreshWreckDecals(scene, field, floor - 1);
    CHECK(scene.wreckDecals.size() == rm::wreckVertexCount(12));
    rm::app::refreshWreckDecals(scene, field, ceiling);
    CHECK(scene.wreckDecals.empty());
}
TEST_CASE("impact marks retire across retail's scorch lifetime spread", "[decals]") {
    // Retail scorch lives 300-600 s, not a flat 300 s (`defaultexplosions.lua`
    // C-290). Each mark retires at its own point in the spread, derived from
    // the mark's own bits (splitmix32, like the rotation axis) — replay-stable
    // and off the sim RNG (C-297), presentation only.
    const HeightField field = flatAt(64, 0);
    const rm::sim::TickRate rate{};
    const rm::TickIndex floor = rate.ticks(rm::sim::seconds(300.0f));  // ticks, s (C-290)
    const rm::TickIndex ceiling = rate.ticks(rm::sim::seconds(600.0f));  // ticks, s (C-290)
    bool sawEarlyRetire = false;
    bool sawLateSurvivor = false;
    for (int i = 0; i < 512; ++i) {
        rm::app::UnitScene scene;
        rm::app::noteImpactMark(scene, static_cast<float>(i), static_cast<float>(i) * 1.7f, 0.0f, 0);
        rm::app::refreshWreckDecals(scene, field, floor);
        const bool aliveAtFloor = !scene.wreckDecals.empty();
        rm::app::refreshWreckDecals(scene, field, ceiling);
        const bool aliveAtCeiling = !scene.wreckDecals.empty();
        // Nothing outlives the ceiling; nothing retires before the floor.
        CHECK_FALSE(aliveAtCeiling);
        sawEarlyRetire = sawEarlyRetire || !aliveAtFloor;
        sawLateSurvivor = sawLateSurvivor || aliveAtFloor;
    }
    // The spread is real in both directions: some marks retire at the floor,
    // some survive past it.
    CHECK(sawEarlyRetire);
    CHECK(sawLateSurvivor);
}


TEST_CASE("impact marks rotate past their cap", "[decals]") {
    rm::app::UnitScene scene;
    for (int i = 0; i < 600; ++i) {
        rm::app::noteImpactMark(scene, static_cast<float>(i), 0.0f);
    }
    // Sawtooth between 512 - 64 and 512: the erase drops sixty-four past the cap.
    CHECK(scene.impactMarks.size() <= rm::app::UnitScene::kMaxImpactMarks);
    CHECK(scene.impactMarks.size() >= rm::app::UnitScene::kMaxImpactMarks - 64);
    // No gaps, no reorder: the front is always (pushed - size).
    CHECK(scene.impactMarks.front().x
          == Approx(600.0f - static_cast<float>(scene.impactMarks.size())));
    CHECK(scene.impactMarks.back().x == Approx(599.0f));
}

TEST_CASE("a chamfered square cuts its corners at 45 degrees", "[selection-square]") {
    const auto field = flatAt(64, 0);
    std::vector<DecalVertex> vertices;
    constexpr float half = 20.0f;
    constexpr float thickness = 1.0f;
    constexpr float chamfer = 0.4f;
    rm::appendSelectionChamferedSquare(vertices, field, {100, 0, 100}, half, {0, 1, 0, 1},
                                       thickness, chamfer);
    REQUIRE(vertices.size() == rm::ringVertexCount(rm::kRingSegments));

    // The outer rim is the band's widest reach, so the chamfer line lives at
    // |dx| + |dz| == (2 - chamfer) * outer.
    const float outer = half + thickness * 0.5f;
    const float limit = (2.0f - chamfer) * outer;
    constexpr float eps = 0.01f;
    bool cutIsDrawn = false;
    for (const auto& vertex : vertices) {
        const float dx = std::abs(vertex.position[0] - 100.0f);
        const float dz = std::abs(vertex.position[2] - 100.0f);
        // Nothing survives in the corner wedge the chamfer removed...
        CHECK(dx <= outer + eps);
        CHECK(dz <= outer + eps);
        CHECK(dx + dz <= limit + eps);
        // ...and the cut itself is a real edge: vertices strictly inside the square's
        // reach, sitting on the chamfer line.
        if (dx < outer - eps && dz < outer - eps && dx + dz > limit - 0.1f) {
            cutIsDrawn = true;
        }
    }
    CHECK(cutIsDrawn);
}

// --- The queue's per-order palette ---------------------------------------------------
//
// One colour per command family, so a drawn queue reads as the orders it holds rather
// than as one indifferent chain. The mapping is the vocabulary the interface already
// speaks: green for go, red for hurt, cyan for "not yet in the world".

TEST_CASE("a move queue keeps the queue's own green", "[queue-colours]") {
    const rm::app::QueueRouteColours move =
        rm::app::queueRouteColours(rm::sim::CommandKind::Move);
    CHECK(move.line == rm::app::kQueueLineColour);
    CHECK(move.node == rm::app::kQueueNodeColour);
}

TEST_CASE("each order family draws in its own hue", "[queue-colours]") {
    using rm::sim::CommandKind;
    const rm::app::QueueRouteColours move =
        rm::app::queueRouteColours(CommandKind::Move);
    const rm::app::QueueRouteColours attackMove =
        rm::app::queueRouteColours(CommandKind::AttackMove);
    const rm::app::QueueRouteColours patrol =
        rm::app::queueRouteColours(CommandKind::Patrol);
    const rm::app::QueueRouteColours attack =
        rm::app::queueRouteColours(CommandKind::Attack);
    const rm::app::QueueRouteColours build =
        rm::app::queueRouteColours(CommandKind::Build);
    const rm::app::QueueRouteColours assist =
        rm::app::queueRouteColours(CommandKind::Assist);
    const rm::app::QueueRouteColours reclaim =
        rm::app::queueRouteColours(CommandKind::Reclaim);

    // Aggression ramps green → amber → red: an attack-move is a move that fights,
    // an attack is only the fight, and the hues must not collide with each other
    // or with move on the same ground.
    CHECK(attack.line[0] > 0.9f);
    CHECK(attack.line[1] < 0.45f);
    CHECK(attack.line != attackMove.line);
    CHECK(attackMove.line != move.line);
    CHECK(patrol.line != move.line);
    CHECK(patrol.line != attackMove.line);

    // Build borrows the ghost's cyan — both are "where a structure will stand".
    CHECK(build.node == rm::app::kBuildGhostColour);

    // Assist and guard are the same standing family: a unit watching another.
    CHECK(assist.line == rm::app::queueRouteColours(CommandKind::Guard).line);
    CHECK(assist.line == rm::app::queueRouteColours(CommandKind::Repair).line);
    CHECK(assist.line != move.line);

    // Reclaim gets a sand of its own — it ends with something taken, but taken for
    // parts rather than destroyed, so it does not wear the attack's red.
    CHECK(reclaim.line == rm::app::queueRouteColours(CommandKind::ReclaimUnit).line);
    CHECK(reclaim.line != move.line);
    CHECK(reclaim.line != attack.line);
    CHECK(rm::app::queueRouteColours(CommandKind::Overcharge).line == attack.line);
    CHECK(rm::app::queueRouteColours(CommandKind::Capture).line == attack.line);
}

TEST_CASE("kinds without a drawn destination keep the plain queue colour",
          "[queue-colours]") {
    using rm::sim::CommandKind;
    for (const CommandKind kind :
         {CommandKind::Stop, CommandKind::Script, CommandKind::ToggleFactoryRepeat,
          CommandKind::CancelFactoryBuild, CommandKind::Dive}) {
        CHECK(rm::app::queueRouteColours(kind).line == rm::app::kQueueLineColour);
    }
}

TEST_CASE("a queue node is always the louder half of its segment", "[queue-colours]") {
    for (const rm::sim::CommandKind kind :
         {rm::sim::CommandKind::Move, rm::sim::CommandKind::Attack,
          rm::sim::CommandKind::Patrol, rm::sim::CommandKind::Build}) {
        const rm::app::QueueRouteColours colours = rm::app::queueRouteColours(kind);
        // The node is the decision; the line only connects. Whatever the family,
        // the node's alpha outshouts the line's and the hues match.
        CHECK(colours.node[3] > colours.line[3]);
        CHECK(colours.node[0] == Approx(colours.line[0]));
        CHECK(colours.node[1] == Approx(colours.line[1]));
        CHECK(colours.node[2] == Approx(colours.line[2]));
    }
}

