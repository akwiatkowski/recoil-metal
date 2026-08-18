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

// A triangulated heightfield, ready for a vertex/index buffer pair.
struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;  ///< triangle list, 3 per triangle

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

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
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
// That uniformity is the deliberate limitation. Recoil uses ROAM
// (rts/Map/SMF/ROAM/) to vary detail with distance, and per-patch LOD would do
// better here too, but it brings crack-stitching between neighbouring patches
// at different levels, and none of that is needed to make a large map render at
// all — which is the problem this solves.
//
// Normals come from central differences over the height grid AT THE SAME
// STRIDE, so a decimated mesh is shaded by the slope it actually has rather
// than by detail it no longer carries. That is why HeightField::heightAt clamps
// rather than bounds-checks.
[[nodiscard]] TerrainMesh buildTerrainMesh(const HeightField& field, int stride = 1);

} // namespace rm
