#pragma once

#include "core/map/HeightField.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace rm {

// One terrain vertex, laid out for direct upload to Metal with no repacking.
//
// std::array<float,3> rather than simd_float3 deliberately: simd_float3 is
// 16-byte aligned, which would pad this to 32 bytes and waste a third of the
// vertex buffer. 24 bytes matches MSL's packed_float3 pair exactly.
struct TerrainVertex {
    std::array<float, 3> position;  ///< elmos: (x, height, z)
    std::array<float, 3> normal;    ///< unit length, +Y up
};

static_assert(sizeof(TerrainVertex) == 24,
              "TerrainVertex must stay tightly packed — the shader reads it as "
              "two packed_float3s and any padding would shear the stream");

// One square block of the terrain, as a range of the index buffer plus the box
// that contains it.
//
// This is what makes the terrain cullable. Without it the ground is one draw of
// two million triangles, and a shadow pass covering a fraction of the map still
// pays for all of them.
/// Detail levels per chunk. Each halves the sample rate again, so level 2 is a
/// sixteenth of the triangles — past that a 64-square chunk is four quads and
/// there is nothing left to remove.
inline constexpr int kLodLevels = 3;

struct TerrainChunk {
    /// One index range per detail level, finest first. Level 0 is every
    /// vertex; each level after it takes every other one.
    struct Lod {
        std::size_t firstIndex = 0;

        /// Everything to draw for this level: the surface, then its skirt.
        ///
        /// One range rather than two so that a chunk stays a single draw and
        /// the renderer's cull-and-merge keeps working — a separate skirt range
        /// would double the draw count, which is the cost the merge exists to
        /// avoid.
        std::size_t indexCount = 0;

        /// How much of the above is the terrain surface proper. The skirt is
        /// the remainder. Split out because the surface ranges are what tile
        /// the mesh exactly once, and the skirt is deliberately extra geometry
        /// over the top of that.
        std::size_t surfaceIndexCount = 0;
    };
    std::array<Lod, kLodLevels> lods{};

    /// How far this chunk's skirt hangs below its rim, in elmos.
    ///
    /// Derived from the terrain rather than fixed: it must exceed the worst gap
    /// a coarse chord can leave over the chunk, and no more. A constant deep
    /// enough for a cliff would hang into open air wherever the ground beside a
    /// chunk falls away, which is the classic skirt artefact.
    float skirtDepth = 0.0f;

    /// The finest level, kept as the plain range so callers that do not choose
    /// a level — and the tests that assert the chunks tile the mesh — need not
    /// know levels exist.
    std::size_t firstIndex = 0;
    std::size_t indexCount = 0;

    float minX = 0.0f, maxX = 0.0f;
    float minY = 0.0f, maxY = 0.0f;
    float minZ = 0.0f, maxZ = 0.0f;
};

/// Squares along each side of a terrain chunk (at the mesh's own stride).
///
/// 64 gives 16x16 chunks on a 1024-square map: fine enough that a camera-sized
/// shadow box touches a handful of them, coarse enough that the draw-call count
/// stays in the hundreds rather than the thousands.
inline constexpr int kChunkSquares = 64;

// A triangulated heightfield, ready for a vertex/index buffer pair.
struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;  ///< triangle list, 3 per triangle

    /// Every chunk, each carrying one index range per detail level. The
    /// level-0 ranges together cover the mesh exactly once; the coarser ranges
    /// are alternatives to them, interleaved in the same buffer.
    std::vector<TerrainChunk> chunks;

    // The grid's dimensions, so a consumer can index `vertices` by (x, z)
    // rather than reverse-engineering the stride from the bounds. The water
    // plane needs exactly that: it samples the ground under itself to know how
    // deep it is.
    int verticesX = 0;
    int verticesZ = 0;

    /// Height at a grid corner, in elmos. Out-of-range indices clamp.
    [[nodiscard]] float heightAt(int x, int z) const noexcept;

    // World-space bounds in elmos. The camera uses these to frame the map, so
    // they are computed here rather than rediscovered by the renderer.
    float minX = 0.0f, maxX = 0.0f;
    float minY = 0.0f, maxY = 0.0f;
    float minZ = 0.0f, maxZ = 0.0f;

    /// Triangles of terrain SURFACE at full detail.
    ///
    /// Not `indices.size() / 3`, for two reasons. The buffer holds every detail
    /// level back to back, and the coarse ones are alternatives to the fine
    /// ones rather than additions to them. And each level carries a skirt,
    /// which is deliberately extra geometry hiding the seams between levels
    /// rather than terrain anyone asked for. Counting either would report a
    /// mesh far larger than the ground it describes.
    [[nodiscard]] std::size_t triangleCount() const noexcept;
};

/// The most vertices this renderer will put along one side of a terrain mesh.
///
/// 1025 is a 1024-square map at full detail — the size everything here has been
/// developed and benchmarked against, at 1.05M vertices and 25 MB. Larger maps
/// are decimated to fit rather than refused, which is what lifts the old
/// hard cap: a 4096-square map at stride 4 costs exactly what a 1024-square map
/// costs today.
inline constexpr int kMaxVerticesPerSide = 1025;

/// The sample step that keeps a field inside kMaxVerticesPerSide.
///
/// Always a power of two, so it divides the 128-multiple dimensions SMF
/// requires and the power-of-two ones .scmap uses — an uneven step would leave
/// a ragged strip along the far edge.
[[nodiscard]] int chooseStride(const HeightField& field) noexcept;

// Builds a triangle mesh from a decoded heightfield, sampling every `stride`th
// height.
//
// `stride` 1 is one vertex per height sample and two triangles per square: the
// full-detail mesh, and what every map up to 1024 squares gets. Above that the
// step doubles, which is level of detail in its crudest useful form — chosen
// once at load, uniform across the map.
//
// Detail additionally varies per chunk with distance (see TerrainChunk::Lod),
// which is where the crack-stitching problem arrives: neighbouring chunks at
// different levels disagree along their shared edge. That is answered with
// skirts rather than with transition strips — see skirtDepth.
//
// Normals come from central differences over the height grid AT THE SAME
// STRIDE, so a decimated mesh is shaded by the slope it actually has rather
// than by detail it no longer carries. That is why HeightField::heightAt clamps
// rather than bounds-checks.
[[nodiscard]] TerrainMesh buildTerrainMesh(const HeightField& field, int stride = 1);

} // namespace rm
