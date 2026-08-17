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

    // Rotation relative to the parent, as a quaternion (w, x, y, z), and the
    // same accumulated to the root.
    //
    // Identity for every .s3o: the format carries no rotation whatsoever, which
    // is why milestone 5 could add a vector and be done. .scm carries one per
    // bone, so accumulating the hierarchy became real composition rather than
    // addition — and this is the rest pose a .sca animates away from.
    std::array<float, 4> rotation{{1.0f, 0.0f, 0.0f, 0.0f}};
    std::array<float, 4> globalRotation{{1.0f, 0.0f, 0.0f, 0.0f}};
};

/// Rotates a vector by a quaternion (w, x, y, z). Free-standing because both
/// the loader and anything walking a hierarchy needs it.
[[nodiscard]] std::array<float, 3> rotateByQuaternion(const std::array<float, 4>& q,
                                                      const std::array<float, 3>& v) noexcept;

/// Quaternion product, parent-first: the rotation of `b` applied within `a`.
[[nodiscard]] std::array<float, 4> multiplyQuaternions(const std::array<float, 4>& a,
                                                       const std::array<float, 4>& b) noexcept;

// Which engine's conventions a model was authored under.
//
// Both families reduce to the same bones/vertices/indices, which is what makes
// one struct honest — but two things genuinely differ, and both are invisible
// rather than wrong-looking if guessed:
//
//   vertex space  .s3o partitions geometry per rigid piece and stores each
//                 piece's vertices relative to that piece, so a bone offset is
//                 added at draw time. .scm stores every vertex in MODEL space
//                 already; adding the offset would scatter the model. Verified
//                 against the retail corpus, not assumed.
//   team mask     .s3o puts it in tex1's alpha (ModelFragProgGL4.glsl:101).
//                 Supreme Commander puts it in the alpha of the `_SpecTeam`
//                 texture — which is what the name says, and what the data
//                 shows: its albedo is frequently DXT1 and has no usable alpha.
//
// A per-format loader could not express either difference without this, and a
// renderer cannot infer them from the geometry.
enum class Family {
    Recoil,            ///< .s3o — Recoil / Beyond All Reason
    SupremeCommander,  ///< .scm — Supreme Commander / Forged Alliance
};

struct Model {
    std::string name;
    Family family = Family::Recoil;

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

    /// A vertex's position in model space, which is where the two families
    /// differ: Recoil's are bone-local and need the bone's accumulated offset,
    /// Supreme Commander's are already there. See Family.
    [[nodiscard]] std::array<float, 3> modelSpacePosition(const ModelVertex& vertex) const noexcept;
};

/// Fills boundsMin/boundsMax from the vertices, honouring the model's family.
void computeBounds(Model& model) noexcept;

} // namespace rm
