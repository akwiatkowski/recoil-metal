// Selection-ring geometry. A ring is the one piece of interface this renderer
// draws, and "is the unit I clicked the one that is highlighted" is a question
// nobody can answer from a screenshot when two units stand close together — so
// the shape is pinned here rather than judged by looking.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/scene/SelectionRing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::HeightField;
using rm::RingVertex;

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

TEST_CASE("a ring is two triangles per segment and nothing else") {
    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, flatAt(16, 0), {{64.0f, 0.0f, 64.0f}}, 10.0f, kWhite);

    CHECK(out.size() == rm::ringVertexCount());
    CHECK(out.size() == static_cast<std::size_t>(rm::kRingSegments) * 6u);
}

TEST_CASE("appending accumulates rather than replacing") {
    // One buffer holds the whole selection, so a second call must add to it.
    std::vector<RingVertex> out;
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

    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, radius, kWhite);

    REQUIRE_FALSE(out.empty());

    bool sawInner = false;
    bool sawOuter = false;
    for (const RingVertex& v : out) {
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
    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, 20.0f, kWhite);

    // Angles of every vertex, sorted: consecutive ones must never be further
    // apart than one segment.
    std::vector<float> angles;
    angles.reserve(out.size());
    for (const RingVertex& v : out) {
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
    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, field, centre, 30.0f, kWhite);

    float lowest = 1e9f;
    float highest = -1e9f;
    for (const RingVertex& v : out) {
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

    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, field, {{100.0f, 0.0f, 100.0f}}, 15.0f, kWhite);

    for (const RingVertex& v : out) {
        CHECK(v.position[1] > ground);
    }
}

TEST_CASE("the colour is carried on every vertex") {
    // Per vertex, so one draw covers a mixed selection.
    const std::array<float, 4> teal{{0.0f, 0.8f, 0.7f, 0.55f}};
    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, flatAt(16, 0), {{64.0f, 0.0f, 64.0f}}, 8.0f, teal);

    for (const RingVertex& v : out) {
        CHECK(v.colour[0] == Approx(teal[0]));
        CHECK(v.colour[1] == Approx(teal[1]));
        CHECK(v.colour[2] == Approx(teal[2]));
        CHECK(v.colour[3] == Approx(teal[3]));
    }
}

TEST_CASE("a nonsensical ring produces nothing rather than garbage") {
    const HeightField field = flatAt(16, 0);
    const std::array<float, 3> centre{{64.0f, 0.0f, 64.0f}};

    std::vector<RingVertex> out;
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
    std::vector<RingVertex> out;
    rm::appendSelectionRing(out, flatAt(64, 0), centre, 4.0f, kWhite, /*thickness=*/20.0f);

    REQUIRE_FALSE(out.empty());
    for (const RingVertex& v : out) {
        const float dx = v.position[0] - centre[0];
        const float dz = v.position[2] - centre[2];
        CHECK(std::sqrt(dx * dx + dz * dz) >= Approx(0.0f).margin(1e-4));
    }
}
