#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rm {

// A decoded unit model, deliberately format-agnostic — the seam described in
// ADR-004.
//
// Three formats are in scope for this project and all three reduce to the same
// three things: a bone hierarchy, one vertex array in which each vertex names
// its bone, and one index array.
//
//   .s3o  (Recoil / BAR native)  partitions geometry per rigid piece, so each
//                                piece becomes a bone and all of its vertices
//                                carry that bone's index.
//   .scm  (Supreme Commander)    is already one vertex array with per-vertex
//                                bone indices — it maps on directly.
//   glTF  (FAR's intermediate)   deliberately not implemented; see ADR-004.
//
// This costs nothing today: the piece hierarchy is needed for .s3o regardless.
// Vertices stay bone-local rather than being pre-flattened into model space,
// because that is exactly what animation will need later — a bone's `offset`
// becomes a full transform and nothing else changes.

// One model vertex, laid out for direct upload with no repacking.
// 12 + 12 + 8 + 4 = 36 bytes, tightly packed.
struct ModelVertex {
    std::array<float, 3> position;  ///< bone-local
    std::array<float, 3> normal;
    std::array<float, 2> uv;
    std::uint32_t boneIndex;  ///< index into Model::bones
};

static_assert(sizeof(ModelVertex) == 36,
              "ModelVertex must stay tightly packed — the shader reads it as two "
              "packed_float3s, a float2 and a uint");

// A node in the model's hierarchy. Called a bone rather than a piece because
// that is the term both formats' animation paths use, and because .scm has bones
// that are not geometry.
//
// S3O carries translation only — no per-piece rotation exists in the file
// (s3o.h:20-22), so `offset` is all there is. `.scm` bones carry a full matrix
// plus a quaternion; when that lands, this grows a rotation and the shader
// applies a matrix instead of adding a vector.
struct ModelBone {
    std::string name;
    int parent = -1;  ///< index into Model::bones, -1 for the root

    std::array<float, 3> offset{{0.0f, 0.0f, 0.0f}};        ///< relative to parent
    std::array<float, 3> globalOffset{{0.0f, 0.0f, 0.0f}};  ///< accumulated to root
};

struct Model {
    std::string name;

    std::vector<ModelBone> bones;
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;  ///< triangle list, into `vertices`

    /// Texture file names as referenced by the model. S3O carries exactly two
    /// (diffuse/team in tex1, spec/other in tex2 — report 05 §5.1); empty means
    /// the model named none.
    std::array<std::string, 2> textures;

    // Collision/draw metadata straight from the file. Zero or near-zero means
    // "not supplied", in which case the engine derives it from the extents —
    // callers can use the bounds below to do the same.
    float radius = 0.0f;
    float height = 0.0f;
    std::array<float, 3> midPos{{0.0f, 0.0f, 0.0f}};

    // Model-space bounds, i.e. vertex positions offset by their bone's
    // globalOffset. Computed by the loader because it already walks everything.
    std::array<float, 3> boundsMin{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> boundsMax{{0.0f, 0.0f, 0.0f}};

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
    [[nodiscard]] bool empty() const noexcept { return vertices.empty() || indices.empty(); }

    /// Largest extent from the origin, used when the file supplies no radius.
    [[nodiscard]] float computedRadius() const noexcept;
};

} // namespace rm
