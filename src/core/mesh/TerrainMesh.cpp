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

/// The chunk grid, in squares at the mesh's own stride.
struct ChunkSquares {
    int firstX = 0, firstZ = 0, endX = 0, endZ = 0;
};

/// The last grid line a level actually reaches, which is not always the chunk's
/// far edge: a coarse level steps by `span` and stops before overshooting.
[[nodiscard]] int lastLine(int first, int end, int span) noexcept {
    return first + ((end - first) / span) * span;
}

/// The worst gap a coarse chord can leave along this chunk's rim, in elmos.
///
/// Only the rim matters. A coarse level's error across a chunk's INTERIOR is
/// level-of-detail error — the ground is simply smoother than it should be, and
/// no skirt addresses that. A crack happens only where two chunks meet and
/// disagree about where the edge is, so the four boundary lines are the whole
/// question.
///
/// Measured against the coarsest level, since that is the largest disagreement
/// any neighbour can present.
[[nodiscard]] float rimGap(const rm::TerrainMesh& mesh, const ChunkSquares& chunk,
                           int nx) noexcept {
    const int coarsest = 1 << (rm::kLodLevels - 1);

    const auto heightAt = [&mesh, nx](int x, int z) {
        return mesh.vertices[static_cast<std::size_t>(z) * static_cast<std::size_t>(nx)
                             + static_cast<std::size_t>(x)]
            .position[1];
    };

    float worst = 0.0f;

    // How far the true surface strays from the chord between two samples
    // `coarsest` apart, walking one line.
    const auto walk = [&](bool alongX, int fixed, int from, int to) {
        for (int a = from; a + coarsest <= to; ++a) {
            const float startY = alongX ? heightAt(a, fixed) : heightAt(fixed, a);
            const float endY =
                alongX ? heightAt(a + coarsest, fixed) : heightAt(fixed, a + coarsest);

            for (int k = 1; k < coarsest; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(coarsest);
                const float chord = startY + (endY - startY) * t;
                const float actual = alongX ? heightAt(a + k, fixed) : heightAt(fixed, a + k);
                worst = std::max(worst, std::abs(actual - chord));
            }
        }
    };

    walk(/*alongX=*/true, chunk.firstZ, chunk.firstX, chunk.endX);
    walk(/*alongX=*/true, chunk.endZ, chunk.firstX, chunk.endX);
    walk(/*alongX=*/false, chunk.firstX, chunk.firstZ, chunk.endZ);
    walk(/*alongX=*/false, chunk.endX, chunk.firstZ, chunk.endZ);

    return worst;
}

/// Appends one level's skirt: a curtain hanging `depth` elmos straight down
/// from the chunk's rim, at that level's own resolution.
///
/// The dropped vertices are new — a skirt vertex is the rim vertex moved down,
/// and nothing else in the mesh wants a copy of it there. They inherit the rim
/// vertex's normal, so the curtain shades as a continuation of the ground
/// rather than as a black wall, and its uv (derived from world x/z downstream)
/// is the rim's, so it reads as a vertical smear of the terrain texture.
void appendSkirt(rm::TerrainMesh& mesh, const ChunkSquares& chunk, int span, int nx,
                 float depth) {
    const int lastX = lastLine(chunk.firstX, chunk.endX, span);
    const int lastZ = lastLine(chunk.firstZ, chunk.endZ, span);

    // One edge of the rim, as the grid line it runs along.
    const auto emitEdge = [&](bool alongX, int fixed, int from, int to) {
        const auto gridIndex = [nx, alongX, fixed](int moving) {
            const int x = alongX ? moving : fixed;
            const int z = alongX ? fixed : moving;
            return static_cast<std::uint32_t>(z) * static_cast<std::uint32_t>(nx)
                 + static_cast<std::uint32_t>(x);
        };

        if (from + span > to) {
            return;  // nothing to hang a curtain from
        }

        // The dropped copies, one per rim position and SHARED by the two
        // segments either side of it. Emitting a pair per segment instead is
        // the obvious version and costs twice the vertices — 22% on top of a
        // real map's mesh rather than 11%, which on a 1024-square map is five
        // megabytes to say the same thing.
        const auto firstLow = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int a = from; a <= to; a += span) {
            rm::TerrainVertex dropped = mesh.vertices[gridIndex(a)];
            dropped.position[1] -= depth;
            mesh.vertices.push_back(dropped);
        }

        std::uint32_t step = 0;
        for (int a = from; a + span <= to; a += span, ++step) {
            const std::uint32_t rimA = gridIndex(a);
            const std::uint32_t rimB = gridIndex(a + span);
            const std::uint32_t lowA = firstLow + step;
            const std::uint32_t lowB = firstLow + step + 1;

            // Two triangles closing the quad rim(a) - rim(b) - low(b) - low(a).
            // Winding is not load-bearing: the terrain pass draws with culling
            // off, because the camera is allowed under the ground.
            mesh.indices.push_back(rimA);
            mesh.indices.push_back(rimB);
            mesh.indices.push_back(lowB);

            mesh.indices.push_back(rimA);
            mesh.indices.push_back(lowB);
            mesh.indices.push_back(lowA);
        }
    };

    emitEdge(/*alongX=*/true, chunk.firstZ, chunk.firstX, lastX);
    emitEdge(/*alongX=*/true, lastZ, chunk.firstX, lastX);
    emitEdge(/*alongX=*/false, chunk.firstX, chunk.firstZ, lastZ);
    emitEdge(/*alongX=*/false, lastX, chunk.firstZ, lastZ);
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

    // The chunk grid, in the order the renderer walks it.
    std::vector<ChunkSquares> grid;
    for (int chunkZ = 0; chunkZ < squaresZ; chunkZ += kChunkSquares) {
        for (int chunkX = 0; chunkX < squaresX; chunkX += kChunkSquares) {
            grid.push_back(ChunkSquares{
                .firstX = chunkX,
                .firstZ = chunkZ,
                .endX = std::min(chunkX + kChunkSquares, squaresX),
                .endZ = std::min(chunkZ + kChunkSquares, squaresZ),
            });
        }
    }

    mesh.chunks.resize(grid.size());
    for (std::size_t c = 0; c < grid.size(); ++c) {
        const ChunkSquares& squaresIn = grid[c];
        TerrainChunk& chunk = mesh.chunks[c];
        const float worldStep = kSpacing * static_cast<float>(step);
        chunk.minX = static_cast<float>(squaresIn.firstX) * worldStep;
        chunk.maxX = static_cast<float>(squaresIn.endX) * worldStep;
        chunk.minZ = static_cast<float>(squaresIn.firstZ) * worldStep;
        chunk.maxZ = static_cast<float>(squaresIn.endZ) * worldStep;
        chunk.minY = std::numeric_limits<float>::max();
        chunk.maxY = std::numeric_limits<float>::lowest();
        // Sized to the chunk's own terrain, before any index is emitted —
        // every level's skirt hangs the same distance, so a chunk switching
        // level does not visibly grow or shrink a curtain.
        chunk.skirtDepth = rimGap(mesh, squaresIn, nx);
    }

    // LEVEL outer, chunk inner — and this order is the whole point.
    //
    // The renderer merges a chunk's draw into the previous one only while their
    // index ranges stay adjacent. Emitting all of one chunk's levels together
    // (the obvious order, and what this did until the adjacency was tested) puts
    // chunk N's coarse levels between chunk N's fine range and chunk N+1's, so
    // no two chunks are ever adjacent at the level they are actually drawn at
    // and the merge never fires. Terrain then costs one draw per chunk — which
    // renders an identical image, and is slower than not culling at all.
    for (int lod = 0; lod < kLodLevels; ++lod) {
        const int span = 1 << lod;

        for (std::size_t c = 0; c < grid.size(); ++c) {
            const ChunkSquares& squaresIn = grid[c];
            TerrainChunk& chunk = mesh.chunks[c];
            TerrainChunk::Lod& range = chunk.lods[static_cast<std::size_t>(lod)];
            range.firstIndex = mesh.indices.size();

            for (int z = squaresIn.firstZ; z + span <= squaresIn.endZ; z += span) {
                for (int x = squaresIn.firstX; x + span <= squaresIn.endX; x += span) {
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

                    // The vertical extent comes from the FINEST level: a coarse
                    // level skips the very peaks and valleys that decide
                    // whether a chunk is visible.
                    if (lod == 0) {
                        for (const std::uint32_t v : {v00, v10, v01, v11}) {
                            const float y = mesh.vertices[v].position[1];
                            chunk.minY = std::min(chunk.minY, y);
                            chunk.maxY = std::max(chunk.maxY, y);
                        }
                    }
                }
            }

            range.surfaceIndexCount = mesh.indices.size() - range.firstIndex;

            // --- Skirt -----------------------------------------------------
            // A curtain hanging straight down from the chunk's rim, filling the
            // crack where a neighbour drawn at a different level puts its edge
            // somewhere else.
            //
            // BOTH sides need one. Where this chunk's rim is above the
            // neighbour's chord, this skirt covers the gap; where it is below,
            // the neighbour's does. Skirting only the coarse levels leaves half
            // the cracks open, which is the version that looks fixed until the
            // camera moves.
            //
            // Emitted inside this level's range, right after the surface, so
            // the chunk remains one draw.
            appendSkirt(mesh, squaresIn, span, nx, chunk.skirtDepth);

            range.indexCount = mesh.indices.size() - range.firstIndex;
        }
    }

    // A chunk with no triangles at all would leave its bounds at the sentinel
    // values and cull wrongly, so it is dropped rather than kept empty.
    std::erase_if(mesh.chunks, [](const TerrainChunk& chunk) {
        return chunk.lods[0].indexCount == 0;
    });
    for (TerrainChunk& chunk : mesh.chunks) {
        chunk.firstIndex = chunk.lods[0].firstIndex;
        chunk.indexCount = chunk.lods[0].indexCount;
        // The bounds must contain the skirt, not just the surface. Otherwise a
        // chunk whose curtain is on screen while its surface is not gets
        // culled, and the crack reappears at exactly the grazing angles where
        // it shows most.
        chunk.minY -= chunk.skirtDepth;
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
        total += chunk.lods[0].surfaceIndexCount;
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
