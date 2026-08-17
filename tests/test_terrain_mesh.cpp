// Mesh-builder tests. Everything here is GPU-free: the point of keeping the
// builder in core/ is that triangulation and normals can be pinned by
// arithmetic rather than by looking at a screen.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Approx;
using rm::HeightField;

namespace {

constexpr int kSquares = 4;  // tiny: the builder does not care about the 128 rule

// One raw unit = 1/16 elmo, the same exact scale the SMF tests use.
constexpr float kElmosPerRawUnit = 0.0625f;

/// A heightfield with the given raw samples and an exact power-of-two scale.
[[nodiscard]] HeightField makeField(int squaresX, int squaresZ,
                                    std::vector<std::uint16_t> raw) {
    HeightField field;
    field.squaresX = squaresX;
    field.squaresZ = squaresZ;
    field.baseHeight = 0.0f;
    field.heightScale = kElmosPerRawUnit;
    field.raw = std::move(raw);
    return field;
}

[[nodiscard]] HeightField flatField(int squares) {
    const auto n = static_cast<std::size_t>(squares + 1);
    return makeField(squares, squares, std::vector<std::uint16_t>(n * n, std::uint16_t{0}));
}

} // namespace

TEST_CASE("mesh has one vertex per corner and two triangles per square") {
    const auto mesh = rm::buildTerrainMesh(flatField(kSquares));

    const auto corners = static_cast<std::size_t>(kSquares + 1) * (kSquares + 1);
    REQUIRE(mesh.vertices.size() == corners);

    const auto squares = static_cast<std::size_t>(kSquares) * kSquares;
    REQUIRE(mesh.triangleCount() == squares * 2);
    REQUIRE(mesh.indices.size() == squares * 6);
}

TEST_CASE("every index addresses a real vertex") {
    const auto mesh = rm::buildTerrainMesh(flatField(kSquares));

    const auto vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const std::uint32_t index : mesh.indices) {
        REQUIRE(index < vertexCount);
    }
}

TEST_CASE("vertices are spaced one square apart in world elmos") {
    const auto mesh = rm::buildTerrainMesh(flatField(kSquares));

    // Vertex 0 is the origin corner; vertex 1 is one square along +X.
    REQUIRE(mesh.vertices[0].position[0] == Approx(0.0f));
    REQUIRE(mesh.vertices[0].position[2] == Approx(0.0f));
    REQUIRE(mesh.vertices[1].position[0] == Approx(static_cast<float>(rm::kSquareSize)));

    // The row stride steps one square along +Z.
    const auto rowStride = static_cast<std::size_t>(kSquares + 1);
    REQUIRE(mesh.vertices[rowStride].position[2] == Approx(static_cast<float>(rm::kSquareSize)));

    // Bounds span squares, not vertices: 4 squares of 8 elmos = 32 elmos.
    REQUIRE(mesh.maxX == Approx(static_cast<float>(kSquares * rm::kSquareSize)));
    REQUIRE(mesh.maxZ == Approx(static_cast<float>(kSquares * rm::kSquareSize)));
}

TEST_CASE("a flat field yields straight-up normals everywhere") {
    const auto mesh = rm::buildTerrainMesh(flatField(kSquares));

    for (const auto& vertex : mesh.vertices) {
        REQUIRE(vertex.normal[0] == Approx(0.0f).margin(1e-6f));
        REQUIRE(vertex.normal[1] == Approx(1.0f));
        REQUIRE(vertex.normal[2] == Approx(0.0f).margin(1e-6f));
    }

    REQUIRE(mesh.minY == Approx(0.0f));
    REQUIRE(mesh.maxY == Approx(0.0f));
}

TEST_CASE("a 45-degree ramp yields a 45-degree normal") {
    // Rise one square's worth of elmos per square along +X. With 1/16 elmo per
    // raw unit, 8 elmos of rise is 128 raw units.
    constexpr std::uint16_t kRawStepFor45Deg = 128;

    const auto n = static_cast<std::size_t>(kSquares + 1);
    std::vector<std::uint16_t> raw(n * n);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            raw[z * n + x] = static_cast<std::uint16_t>(x * kRawStepFor45Deg);
        }
    }

    const auto mesh = rm::buildTerrainMesh(makeField(kSquares, kSquares, std::move(raw)));

    // Sample an interior vertex, where the central difference is two-sided.
    const std::size_t interior = 2 * n + 2;
    const auto& normal = mesh.vertices[interior].normal;

    // Slope of exactly 1 gives normal (-1, 1, 0) normalised: components ±1/sqrt(2).
    constexpr float kInvSqrt2 = 0.70710678f;
    REQUIRE(normal[0] == Approx(-kInvSqrt2).margin(1e-5f));
    REQUIRE(normal[1] == Approx(kInvSqrt2).margin(1e-5f));
    REQUIRE(normal[2] == Approx(0.0f).margin(1e-6f));

    // And it is a unit vector.
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]
                                 + normal[2] * normal[2]);
    REQUIRE(length == Approx(1.0f));

    // The ramp climbs 4 squares x 8 elmos.
    REQUIRE(mesh.minY == Approx(0.0f));
    REQUIRE(mesh.maxY == Approx(static_cast<float>(kSquares * rm::kSquareSize)));
}

TEST_CASE("triangles wind counter-clockwise seen from above") {
    const auto mesh = rm::buildTerrainMesh(flatField(kSquares));

    // Cross product of the first triangle's edges must point along +Y for the
    // winding the pipeline declares as front-facing.
    const auto& a = mesh.vertices[mesh.indices[0]].position;
    const auto& b = mesh.vertices[mesh.indices[1]].position;
    const auto& c = mesh.vertices[mesh.indices[2]].position;

    // Only the Y component of u x v is needed, so the Y terms of u and v drop out.
    const float ux = b[0] - a[0], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vz = c[2] - a[2];

    const float crossY = uz * vx - ux * vz;  // +Y component of u x v
    REQUIRE(crossY > 0.0f);
}

TEST_CASE("an empty or degenerate field yields an empty mesh") {
    HeightField empty;
    const auto mesh = rm::buildTerrainMesh(empty);

    REQUIRE(mesh.vertices.empty());
    REQUIRE(mesh.indices.empty());
}
