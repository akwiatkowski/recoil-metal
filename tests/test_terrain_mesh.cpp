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

    // The GRID is one vertex per corner, and those come first, so a consumer
    // can still index it by (x, z). The skirts add vertices after it — they
    // are the same corners dropped, which is why they cannot be shared with
    // the grid.
    const auto corners = static_cast<std::size_t>(kSquares + 1) * (kSquares + 1);
    REQUIRE(mesh.vertices.size() >= corners);
    REQUIRE(static_cast<std::size_t>(mesh.verticesX) * static_cast<std::size_t>(mesh.verticesZ)
            == corners);

    const auto squares = static_cast<std::size_t>(kSquares) * kSquares;
    // The buffer holds every detail level AND every skirt, so the full-detail
    // SURFACE count is what matches the square count — indices.size() carries
    // the coarse alternatives and the curtains too.
    REQUIRE(mesh.triangleCount() == squares * 2);
    REQUIRE(mesh.indices.size() > squares * 6);
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

TEST_CASE("a stride decimates the mesh without moving the map") {
    // 64 squares so the strides below divide it exactly, which is the case
    // every real map hits (SMF is a multiple of 128, .scmap a power of two).
    const HeightField field = makeField(64, 64, [] {
        std::vector<std::uint16_t> raw(65 * 65);
        for (int z = 0; z < 65; ++z) {
            for (int x = 0; x < 65; ++x) {
                raw[static_cast<std::size_t>(z) * 65 + static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>((x + z) * 16);
            }
        }
        return raw;
    }());

    const rm::TerrainMesh full = rm::buildTerrainMesh(field);
    const rm::TerrainMesh half = rm::buildTerrainMesh(field, 2);
    const rm::TerrainMesh quarter = rm::buildTerrainMesh(field, 4);

    // One GRID vertex per sample at each step: 65, 33, 17 along a side. The
    // skirts append more, but the grid is what the stride decimates.
    CHECK(full.verticesX == 65);
    CHECK(full.verticesZ == 65);
    CHECK(half.verticesX == 33);
    CHECK(quarter.verticesX == 17);

    // Four times fewer triangles each step, which is the point.
    CHECK(half.triangleCount() == full.triangleCount() / 4);
    CHECK(quarter.triangleCount() == full.triangleCount() / 16);

    // The map still occupies exactly the same ground. A decimated mesh that
    // shrank would put the terrain and the units on different maps.
    CHECK(half.maxX == Approx(full.maxX));
    CHECK(half.maxZ == Approx(full.maxZ));
    CHECK(quarter.maxX == Approx(full.maxX));

    // Every vertex still sits on the real surface — decimation drops samples,
    // it does not approximate them.
    for (const rm::TerrainVertex& v : quarter.vertices) {
        REQUIRE(v.position[1] == Approx(field.heightAtWorld(v.position[0], v.position[2])));
    }
}

TEST_CASE("decimated normals describe the slope the mesh actually has") {
    // A ramp along +X, so the normals genuinely lean.
    const HeightField field = makeField(64, 64, [] {
        std::vector<std::uint16_t> raw(65 * 65);
        for (int z = 0; z < 65; ++z) {
            for (int x = 0; x < 65; ++x) {
                raw[static_cast<std::size_t>(z) * 65 + static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>(x * 200);
            }
        }
        return raw;
    }());

    const rm::TerrainMesh quarter = rm::buildTerrainMesh(field, 4);
    for (const rm::TerrainVertex& v : quarter.vertices) {
        const float length = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1]
                                       + v.normal[2] * v.normal[2]);
        REQUIRE(length == Approx(1.0f).margin(1e-4));
        REQUIRE(v.normal[1] > 0.0f);  // never inverted
    }
}

TEST_CASE("the stride is chosen to keep a map inside the vertex budget") {
    const auto sized = [](int squares) {
        HeightField field;
        field.squaresX = squares;
        field.squaresZ = squares;
        field.heightScale = 1.0f;
        field.raw.assign(field.sampleCount(), std::uint16_t{0});
        return field;
    };

    // Everything up to the budget stays at full detail.
    CHECK(rm::chooseStride(sized(256)) == 1);
    CHECK(rm::chooseStride(sized(1024)) == 1);

    // Beyond it the step doubles — a 4096-square map costs what a 1024 does.
    CHECK(rm::chooseStride(sized(2048)) == 2);
    CHECK(rm::chooseStride(sized(4096)) == 4);
    CHECK(rm::chooseStride(sized(8192)) == 8);

    for (const int squares : {2048, 4096, 8192}) {
        const HeightField field = sized(squares);
        const int stride = rm::chooseStride(field);
        CHECK(squares / stride + 1 <= rm::kMaxVerticesPerSide);
    }
}

TEST_CASE("each chunk carries one index range per detail level") {
    const HeightField field = makeField(64, 64, [] {
        std::vector<std::uint16_t> raw(65 * 65);
        for (int z = 0; z < 65; ++z) {
            for (int x = 0; x < 65; ++x) {
                raw[static_cast<std::size_t>(z) * 65 + static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>((x * z) % 400);
            }
        }
        return raw;
    }());

    const rm::TerrainMesh mesh = rm::buildTerrainMesh(field);

    REQUIRE_FALSE(mesh.chunks.empty());

    // The level-0 ranges tile the mesh: together they are the full-detail
    // triangle list, no gaps and no overlap. A gap drops terrain; an overlap
    // draws it twice. They are NOT contiguous any more — each chunk's coarser
    // levels sit between them in the same buffer.
    std::size_t fullDetail = 0;
    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        REQUIRE(chunk.indexCount > 0);
        REQUIRE(chunk.firstIndex == chunk.lods[0].firstIndex);
        REQUIRE(chunk.indexCount == chunk.lods[0].indexCount);
        // The SURFACE ranges are what tile the mesh. Each level's skirt sits
        // inside the same range and is extra geometry over the top of it.
        fullDetail += chunk.lods[0].surfaceIndexCount;

        // Every level is inside the buffer, non-empty, and coarser than the
        // last — a level that grew would mean the stride ran backwards.
        std::size_t previous = chunk.lods[0].indexCount + 1;
        for (const auto& lod : chunk.lods) {
            REQUIRE(lod.indexCount > 0);
            REQUIRE(lod.firstIndex + lod.indexCount <= mesh.indices.size());
            REQUIRE(lod.indexCount < previous);
            previous = lod.indexCount;
        }
    }

    CHECK(fullDetail == mesh.triangleCount() * 3);
    // Each level is a quarter of the one before, so the buffer is about a
    // third larger than the full-detail list alone.
    CHECK(mesh.indices.size() > fullDetail);
    CHECK(mesh.indices.size() < fullDetail * 2);
}

TEST_CASE("a chunk's bounds contain the vertices it indexes") {
    // What culling relies on. Bounds too small and terrain vanishes when it is
    // still on screen — the kind of bug that only shows at one camera angle.
    const HeightField field = makeField(64, 64, [] {
        std::vector<std::uint16_t> raw(65 * 65);
        for (int z = 0; z < 65; ++z) {
            for (int x = 0; x < 65; ++x) {
                raw[static_cast<std::size_t>(z) * 65 + static_cast<std::size_t>(x)] =
                    static_cast<std::uint16_t>((x + 2 * z) * 20);
            }
        }
        return raw;
    }());

    const rm::TerrainMesh mesh = rm::buildTerrainMesh(field);

    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        REQUIRE(chunk.minX <= chunk.maxX);
        REQUIRE(chunk.minY <= chunk.maxY);
        REQUIRE(chunk.minZ <= chunk.maxZ);

        for (std::size_t i = chunk.lods[0].firstIndex;
             i < chunk.lods[0].firstIndex + chunk.lods[0].indexCount; ++i) {
            const rm::TerrainVertex& v = mesh.vertices[mesh.indices[i]];
            REQUIRE(v.position[0] >= Approx(chunk.minX));
            REQUIRE(v.position[0] <= Approx(chunk.maxX));
            REQUIRE(v.position[1] >= Approx(chunk.minY));
            REQUIRE(v.position[1] <= Approx(chunk.maxY));
            REQUIRE(v.position[2] >= Approx(chunk.minZ));
            REQUIRE(v.position[2] <= Approx(chunk.maxZ));
        }
    }
}

TEST_CASE("chunk bounds together cover the whole map") {
    const HeightField field = flatField(64);
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(field);

    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        minX = std::min(minX, chunk.minX);
        maxX = std::max(maxX, chunk.maxX);
        minZ = std::min(minZ, chunk.minZ);
        maxZ = std::max(maxZ, chunk.maxZ);
    }

    CHECK(minX == Approx(mesh.minX));
    CHECK(maxX == Approx(mesh.maxX));
    CHECK(minZ == Approx(mesh.minZ));
    CHECK(maxZ == Approx(mesh.maxZ));
}

TEST_CASE("consecutive chunks are adjacent in the buffer at the same level") {
    // What the renderer's cull-and-merge relies on. It merges chunk N's draw
    // into chunk N-1's only while the index ranges stay adjacent; where they do
    // not, culling replaces one draw of the terrain with one per chunk, which
    // is SLOWER than not culling at all whenever the camera keeps them all.
    //
    // Nothing on screen can show this. A renderer issuing 256 draws where one
    // would do renders exactly the same image.
    const HeightField field = flatField(256);
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(field);

    REQUIRE(mesh.chunks.size() > 1);

    for (std::size_t lod = 0; lod < rm::kLodLevels; ++lod) {
        for (std::size_t i = 1; i < mesh.chunks.size(); ++i) {
            const rm::TerrainChunk::Lod& previous = mesh.chunks[i - 1].lods[lod];
            const rm::TerrainChunk::Lod& current = mesh.chunks[i].lods[lod];
            INFO("level " << lod << ", chunk " << i);
            REQUIRE(previous.firstIndex + previous.indexCount == current.firstIndex);
        }
    }
}

// --- Skirts ------------------------------------------------------------------
// Neighbouring chunks drawn at different detail levels do not agree along their
// shared edge: the coarse one draws a chord where the fine one follows every
// vertex. The difference is a vertical crack you can see the sky through. A
// skirt is a curtain hanging straight down from each chunk's rim that fills it.

namespace {

/// A field with a sharp ridge, so coarse levels genuinely miss something. A
/// smooth field would let a broken skirt pass every test below.
[[nodiscard]] HeightField ridgedField(int squares) {
    const auto n = static_cast<std::size_t>(squares + 1);
    std::vector<std::uint16_t> raw(n * n);
    for (int z = 0; z <= squares; ++z) {
        for (int x = 0; x <= squares; ++x) {
            // Alternating high and low along both axes: every skipped vertex
            // is a peak or a trough, which is the worst case for a chord.
            const bool peak = (x % 2 == 0) != (z % 2 == 0);
            raw[static_cast<std::size_t>(z) * n + static_cast<std::size_t>(x)] =
                static_cast<std::uint16_t>(peak ? 4000 : 100);
        }
    }
    return makeField(squares, squares, std::move(raw));
}

} // namespace

TEST_CASE("a skirt hangs below the rim it is attached to") {
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(ridgedField(128));
    REQUIRE(mesh.chunks.size() > 1);

    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        REQUIRE(chunk.skirtDepth > 0.0f);

        for (std::size_t lod = 0; lod < rm::kLodLevels; ++lod) {
            const rm::TerrainChunk::Lod& range = chunk.lods[lod];
            REQUIRE(range.surfaceIndexCount > 0);
            // The skirt is part of the range the renderer draws, so that a
            // chunk stays ONE draw and the merge keeps working.
            REQUIRE(range.indexCount > range.surfaceIndexCount);
            REQUIRE(range.firstIndex + range.indexCount <= mesh.indices.size());
        }
    }
}

TEST_CASE("the chunk's bounds contain its skirt") {
    // Culling uses these bounds. If they describe only the surface, a chunk
    // whose skirt is on screen but whose surface is not gets culled, and the
    // crack the skirt exists to hide reappears at exactly the camera angles
    // where it shows most.
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(ridgedField(128));

    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        for (std::size_t lod = 0; lod < rm::kLodLevels; ++lod) {
            const rm::TerrainChunk::Lod& range = chunk.lods[lod];
            for (std::size_t i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i) {
                const rm::TerrainVertex& v = mesh.vertices[mesh.indices[i]];
                REQUIRE(v.position[1] >= Approx(chunk.minY));
                REQUIRE(v.position[1] <= Approx(chunk.maxY));
            }
        }
    }
}

TEST_CASE("the skirt is deep enough to cover the worst gap between levels") {
    // The property that makes a skirt WORK rather than merely exist. The gap
    // between a fine edge and a coarse chord over the same span is bounded by
    // how much the surface moves across that span; a skirt shallower than the
    // gap leaves a crack, and one measured in the field's own units is the only
    // way to know which side of that line it falls.
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(ridgedField(128));

    const auto heightAtGrid = [&mesh](int x, int z) {
        return mesh.heightAt(x, z);
    };

    for (const rm::TerrainChunk& chunk : mesh.chunks) {
        // The chunk's grid extent, recovered from its world bounds.
        const int firstX = static_cast<int>(chunk.minX / static_cast<float>(rm::kSquareSize));
        const int firstZ = static_cast<int>(chunk.minZ / static_cast<float>(rm::kSquareSize));
        const int endX = static_cast<int>(chunk.maxX / static_cast<float>(rm::kSquareSize));
        const int endZ = static_cast<int>(chunk.maxZ / static_cast<float>(rm::kSquareSize));

        const int coarsest = 1 << (rm::kLodLevels - 1);

        // Worst deviation of a coarsest-level chord from the true surface along
        // the chunk's RIM. Only the rim can crack: a coarse level's error
        // across the interior is level-of-detail error, which no skirt
        // addresses and which no neighbour disagrees with.
        float worst = 0.0f;
        const auto walk = [&](bool alongX, int fixed, int from, int to) {
            for (int a = from; a + coarsest <= to; ++a) {
                const float startY = alongX ? heightAtGrid(a, fixed) : heightAtGrid(fixed, a);
                const float endY = alongX ? heightAtGrid(a + coarsest, fixed)
                                          : heightAtGrid(fixed, a + coarsest);
                for (int k = 1; k < coarsest; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(coarsest);
                    const float chord = startY + (endY - startY) * t;
                    const float actual =
                        alongX ? heightAtGrid(a + k, fixed) : heightAtGrid(fixed, a + k);
                    worst = std::max(worst, std::abs(actual - chord));
                }
            }
        };
        walk(true, firstZ, firstX, endX);
        walk(true, endZ, firstX, endX);
        walk(false, firstX, firstZ, endZ);
        walk(false, endX, firstZ, endZ);

        INFO("chunk at " << chunk.minX << ", " << chunk.minZ);
        REQUIRE(chunk.skirtDepth >= Approx(worst).margin(1e-3));
    }
}

TEST_CASE("a flat map needs no skirt to speak of") {
    // The depth is derived from the terrain, not a constant. Flat ground has no
    // gap to cover, and a skirt sized for a cliff would hang into open air
    // wherever the ground beside it falls away.
    const rm::TerrainMesh flat = rm::buildTerrainMesh(flatField(128));
    const rm::TerrainMesh ridged = rm::buildTerrainMesh(ridgedField(128));

    REQUIRE_FALSE(flat.chunks.empty());
    for (const rm::TerrainChunk& chunk : flat.chunks) {
        CHECK(chunk.skirtDepth == Approx(0.0f).margin(1e-4));
    }
    CHECK(ridged.chunks[0].skirtDepth > 1.0f);
}

TEST_CASE("skirt vertices sit directly under the rim, not beside it") {
    // A skirt that drifted sideways would poke through the neighbouring chunk
    // instead of hiding behind it.
    const rm::TerrainMesh mesh = rm::buildTerrainMesh(ridgedField(64));
    REQUIRE_FALSE(mesh.chunks.empty());

    const rm::TerrainChunk& chunk = mesh.chunks[0];
    const rm::TerrainChunk::Lod& range = chunk.lods[0];

    // Every skirt vertex must lie on the chunk's rim in plan view.
    for (std::size_t i = range.firstIndex + range.surfaceIndexCount;
         i < range.firstIndex + range.indexCount; ++i) {
        const rm::TerrainVertex& v = mesh.vertices[mesh.indices[i]];
        const bool onEdgeX = v.position[0] == Approx(chunk.minX) || v.position[0] == Approx(chunk.maxX);
        const bool onEdgeZ = v.position[2] == Approx(chunk.minZ) || v.position[2] == Approx(chunk.maxZ);
        INFO("skirt vertex at " << v.position[0] << ", " << v.position[1] << ", " << v.position[2]);
        REQUIRE((onEdgeX || onEdgeZ));
    }
}
