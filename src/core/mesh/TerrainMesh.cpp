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

int chooseStride(const HeightField& field) noexcept {
    int stride = 1;
    while (field.squaresX / stride + 1 > kMaxVerticesPerSide
           || field.squaresZ / stride + 1 > kMaxVerticesPerSide) {
        stride *= 2;
    }
    return stride;
}

TerrainMesh buildTerrainMesh(const HeightField& field, int stride) {
    TerrainMesh mesh;

    if (field.squaresX <= 0 || field.squaresZ <= 0 || field.raw.empty()) {
        return mesh;
    }

    const int step = std::max(1, stride);

    // Squares along each axis AT THIS STRIDE. Rounded up, so a dimension the
    // step does not divide keeps its far edge instead of losing a strip — the
    // last row of squares is then slightly wider in samples, which no map in
    // either format actually hits (both are multiples of 128 or powers of two).
    const int squaresX = (field.squaresX + step - 1) / step;
    const int squaresZ = (field.squaresZ + step - 1) / step;
    const int nx = squaresX + 1;
    const int nz = squaresZ + 1;

    // --- Vertices ----------------------------------------------------------
    mesh.vertices.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(nz));

    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    // Horizontal spacing between adjacent samples. The central difference spans
    // two squares, hence the doubling in the denominator below.
    constexpr float kSpacing = static_cast<float>(kSquareSize);
    constexpr float kCentralSpan = 2.0f * kSpacing;

    for (int zi = 0; zi < nz; ++zi) {
        for (int xi = 0; xi < nx; ++xi) {
            // Clamped so the final row samples the map's true edge rather than
            // running past it when the step does not divide the dimension.
            const int x = std::min(xi * step, field.squaresX);
            const int z = std::min(zi * step, field.squaresZ);
            const float height = field.heightAt(x, z);
            minY = std::min(minY, height);
            maxY = std::max(maxY, height);

            // Central differences give the surface gradient in elmos per elmo.
            // heightAt clamps at the borders, so edge vertices see a one-sided
            // slope mirrored — the "flat continuation" that keeps map edges from
            // developing a false lip.
            const float span = kCentralSpan * static_cast<float>(step);
            const float dhdx =
                (field.heightAt(x + step, z) - field.heightAt(x - step, z)) / span;
            const float dhdz =
                (field.heightAt(x, z + step) - field.heightAt(x, z - step)) / span;

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
        static_cast<std::size_t>(squaresX) * static_cast<std::size_t>(squaresZ);
    mesh.indices.reserve(squares * 6);

    // Emitted chunk by chunk rather than row by row, so each chunk's triangles
    // are one contiguous range of the index buffer and can be drawn on their
    // own. Row-major order would interleave every chunk in a row.
    for (int chunkZ = 0; chunkZ < squaresZ; chunkZ += kChunkSquares) {
        for (int chunkX = 0; chunkX < squaresX; chunkX += kChunkSquares) {
            const int endX = std::min(chunkX + kChunkSquares, squaresX);
            const int endZ = std::min(chunkZ + kChunkSquares, squaresZ);

            TerrainChunk chunk;
            chunk.minX = static_cast<float>(chunkX) * kSpacing * static_cast<float>(step);
            chunk.maxX = static_cast<float>(endX) * kSpacing * static_cast<float>(step);
            chunk.minZ = static_cast<float>(chunkZ) * kSpacing * static_cast<float>(step);
            chunk.maxZ = static_cast<float>(endZ) * kSpacing * static_cast<float>(step);
            chunk.minY = std::numeric_limits<float>::max();
            chunk.maxY = std::numeric_limits<float>::lowest();

            // One index set per detail level, emitted back to back so each is
            // its own contiguous range and can be drawn alone.
            for (int lod = 0; lod < kLodLevels; ++lod) {
                const int span = 1 << lod;
                chunk.lods[static_cast<std::size_t>(lod)].firstIndex = mesh.indices.size();

                for (int z = chunkZ; z + span <= endZ; z += span) {
                    for (int x = chunkX; x + span <= endX; x += span) {
                        const auto row = static_cast<std::uint32_t>(nx);
                        const auto stepX = static_cast<std::uint32_t>(span);
                        const auto stepZ = static_cast<std::uint32_t>(span) * row;

                        const auto v00 =
                            static_cast<std::uint32_t>(z) * row + static_cast<std::uint32_t>(x);
                        const std::uint32_t v10 = v00 + stepX;
                        const std::uint32_t v01 = v00 + stepZ;
                        const std::uint32_t v11 = v01 + stepX;

                        mesh.indices.push_back(v00);
                        mesh.indices.push_back(v01);
                        mesh.indices.push_back(v11);

                        mesh.indices.push_back(v00);
                        mesh.indices.push_back(v11);
                        mesh.indices.push_back(v10);

                        // The vertical extent comes from the FINEST level: a
                        // coarse level skips the very peaks and valleys that
                        // decide whether a chunk is visible.
                        if (lod == 0) {
                            for (const std::uint32_t v : {v00, v10, v01, v11}) {
                                const float y = mesh.vertices[v].position[1];
                                chunk.minY = std::min(chunk.minY, y);
                                chunk.maxY = std::max(chunk.maxY, y);
                            }
                        }
                    }
                }

                chunk.lods[static_cast<std::size_t>(lod)].indexCount =
                    mesh.indices.size() - chunk.lods[static_cast<std::size_t>(lod)].firstIndex;
            }

            chunk.firstIndex = chunk.lods[0].firstIndex;
            chunk.indexCount = chunk.lods[0].indexCount;
            if (chunk.indexCount > 0) {
                mesh.chunks.push_back(chunk);
            }
        }
    }

    mesh.verticesX = nx;
    mesh.verticesZ = nz;
    mesh.minX = 0.0f;
    mesh.maxX = field.widthElmos();
    mesh.minZ = 0.0f;
    mesh.maxZ = field.depthElmos();
    mesh.minY = minY;
    mesh.maxY = maxY;

    return mesh;
}

std::size_t TerrainMesh::triangleCount() const noexcept {
    if (chunks.empty()) {
        return indices.size() / 3;
    }

    std::size_t total = 0;
    for (const TerrainChunk& chunk : chunks) {
        total += chunk.lods[0].indexCount;
    }
    return total / 3;
}

float TerrainMesh::heightAt(int x, int z) const noexcept {
    if (verticesX <= 0 || verticesZ <= 0 || vertices.empty()) {
        return 0.0f;
    }
    const int cx = std::clamp(x, 0, verticesX - 1);
    const int cz = std::clamp(z, 0, verticesZ - 1);
    const auto index = static_cast<std::size_t>(cz) * static_cast<std::size_t>(verticesX)
                     + static_cast<std::size_t>(cx);
    return index < vertices.size() ? vertices[index].position[1] : 0.0f;
}

} // namespace rm
