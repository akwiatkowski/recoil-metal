#include "core/mesh/TerrainMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

/// Normalise a 3-vector, falling back to +Y for a degenerate input. A zero
/// gradient cannot actually occur here (the Y component is fixed at 1), but the
/// fallback keeps the function total rather than relying on that invariant.
[[nodiscard]] std::array<float, 3> normalise(float x, float y, float z) noexcept {
    const float lengthSq = x * x + y * y + z * z;
    if (lengthSq <= 0.0f) {
        return {0.0f, 1.0f, 0.0f};
    }
    const float inv = 1.0f / std::sqrt(lengthSq);
    return {x * inv, y * inv, z * inv};
}

} // namespace

namespace rm {

TerrainMesh buildTerrainMesh(const HeightField& field) {
    TerrainMesh mesh;

    const int nx = field.verticesX();
    const int nz = field.verticesZ();
    if (field.squaresX <= 0 || field.squaresZ <= 0 || field.raw.empty()) {
        return mesh;
    }

    // --- Vertices ----------------------------------------------------------
    mesh.vertices.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz));

    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    // Horizontal spacing between adjacent samples. The central difference spans
    // two squares, hence the doubling in the denominator below.
    constexpr float kSpacing = static_cast<float>(kSquareSize);
    constexpr float kCentralSpan = 2.0f * kSpacing;

    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            const float height = field.heightAt(x, z);
            minY = std::min(minY, height);
            maxY = std::max(maxY, height);

            // Central differences give the surface gradient in elmos per elmo.
            // heightAt clamps at the borders, so edge vertices see a one-sided
            // slope mirrored — the "flat continuation" that keeps map edges from
            // developing a false lip.
            const float dhdx = (field.heightAt(x + 1, z) - field.heightAt(x - 1, z)) / kCentralSpan;
            const float dhdz = (field.heightAt(x, z + 1) - field.heightAt(x, z - 1)) / kCentralSpan;

            // For a surface y = h(x, z), the upward normal is (-dh/dx, 1, -dh/dz).
            mesh.vertices.push_back(TerrainVertex{
                .position = {static_cast<float>(x) * kSpacing,
                             height,
                             static_cast<float>(z) * kSpacing},
                .normal = normalise(-dhdx, 1.0f, -dhdz),
            });
        }
    }

    // --- Indices -----------------------------------------------------------
    // Two triangles per square, counter-clockwise when viewed from above (+Y),
    // which is the winding the render pipeline declares as front-facing.
    const std::size_t squares =
        static_cast<std::size_t>(field.squaresX) * static_cast<std::size_t>(field.squaresZ);
    mesh.indices.reserve(squares * 6);

    for (int z = 0; z < field.squaresZ; ++z) {
        for (int x = 0; x < field.squaresX; ++x) {
            const auto row = static_cast<std::uint32_t>(nx);
            const auto v00 = static_cast<std::uint32_t>(z) * row + static_cast<std::uint32_t>(x);
            const std::uint32_t v10 = v00 + 1;
            const std::uint32_t v01 = v00 + row;
            const std::uint32_t v11 = v01 + 1;

            mesh.indices.push_back(v00);
            mesh.indices.push_back(v01);
            mesh.indices.push_back(v11);

            mesh.indices.push_back(v00);
            mesh.indices.push_back(v11);
            mesh.indices.push_back(v10);
        }
    }

    mesh.minX = 0.0f;
    mesh.maxX = field.widthElmos();
    mesh.minZ = 0.0f;
    mesh.maxZ = field.depthElmos();
    mesh.minY = minY;
    mesh.maxY = maxY;

    return mesh;
}

} // namespace rm
