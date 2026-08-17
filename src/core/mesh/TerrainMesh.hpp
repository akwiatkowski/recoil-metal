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

    // World-space bounds in elmos. The camera uses these to frame the map, so
    // they are computed here rather than rediscovered by the renderer.
    float minX = 0.0f, maxX = 0.0f;
    float minY = 0.0f, maxY = 0.0f;
    float minZ = 0.0f, maxZ = 0.0f;

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
};

// Builds a full-resolution triangle mesh from a decoded heightfield.
//
// One vertex per height sample and two triangles per square — no level of
// detail. Recoil uses ROAM for this (rts/Map/SMF/ROAM/) but a 1024x1024 map is
// only ~2.1M triangles, which an Apple Silicon GPU renders without complaint;
// LOD is a milestone-4 optimisation to be justified by the benchmark, not
// assumed now.
//
// Normals come from central differences over the height grid, which is why
// HeightField::heightAt clamps rather than bounds-checks.
[[nodiscard]] TerrainMesh buildTerrainMesh(const HeightField& field);

} // namespace rm
