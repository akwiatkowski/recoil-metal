#include "core/model/Scm.hpp"

#include "core/map/ByteReader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

using rm::MapError;

// Field offsets within the header, after the 4-byte magic.
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffBoneOffset = 8;
constexpr std::size_t kOffWeightedBoneCount = 12;
constexpr std::size_t kOffVertexOffset = 16;
constexpr std::size_t kOffVertexCount = 24;
constexpr std::size_t kOffIndexOffset = 28;
constexpr std::size_t kOffIndexCount = 32;
constexpr std::size_t kOffBoneCount = 44;

// Field offsets within a bone record.
constexpr std::size_t kBonePosition = 64;  // after the 4x4 rest-pose inverse
constexpr std::size_t kBoneRotation = 76;
constexpr std::size_t kBoneNameOffset = 92;
constexpr std::size_t kBoneParent = 96;

// Field offsets within a vertex record. Tangent and binormal are read past —
// they are for normal mapping, and there is no normal map path yet.
constexpr std::size_t kVertexPosition = 0;
constexpr std::size_t kVertexNormal = 24;
constexpr std::size_t kVertexUv0 = 48;
constexpr std::size_t kVertexBoneIndices = 64;

/// A bone name longer than this is corruption, not a name — the same rule the
/// .s3o loader applies, and for the same reason: an unterminated string would
/// otherwise be walked to the end of the file.
constexpr std::size_t kMaxNameLength = 256;

[[nodiscard]] bool fits(std::span<const std::byte> bytes, std::size_t offset,
                        std::size_t length) noexcept {
    return offset <= bytes.size() && length <= bytes.size() - offset;
}

[[nodiscard]] std::unexpected<MapError> fail(MapError::Code code, std::string message) {
    return std::unexpected{MapError{code, std::move(message)}};
}

/// Checks the 4-byte marker that immediately precedes a section's data.
///
/// The only real defence an offset-based format has: a plausible-but-wrong
/// offset lands somewhere the marker is not, and fails loudly instead of
/// producing geometry made of whatever happened to be there.
[[nodiscard]] bool markerPrecedes(std::span<const std::byte> bytes, std::size_t offset,
                                  const char (&marker)[4]) noexcept {
    if (offset < 4 || offset > bytes.size()) {
        return false;
    }
    return std::memcmp(bytes.data() + offset - 4, marker, 4) == 0;
}

[[nodiscard]] std::expected<std::string, MapError> readName(std::span<const std::byte> bytes,
                                                            std::size_t offset) {
    if (offset == 0 || offset >= bytes.size()) {
        return fail(MapError::Code::Truncated,
                    "bone name offset " + std::to_string(offset) + " is outside a "
                        + std::to_string(bytes.size()) + "-byte file");
    }

    std::string name;
    for (std::size_t i = offset; i < bytes.size(); ++i) {
        const auto c = static_cast<char>(bytes[i]);
        if (c == '\0') {
            return name;
        }
        if (name.size() >= kMaxNameLength) {
            break;
        }
        name.push_back(c);
    }

    return fail(MapError::Code::Truncated, "unterminated bone name at offset "
                                               + std::to_string(offset));
}

[[nodiscard]] std::array<float, 3> readVec3(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
    return {{rm::readF32(bytes, offset), rm::readF32(bytes, offset + 4),
             rm::readF32(bytes, offset + 8)}};
}

} // namespace

namespace rm::scm {

std::expected<Model, MapError> load(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize
        || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail(MapError::Code::NotScmap,
                    "not a Supreme Commander model: expected the 4-byte magic \"MODL\"");
    }

    const std::uint32_t version = readU32(bytes, kOffVersion);
    if (version != kVersion) {
        return fail(MapError::Code::BadHeader,
                    "unsupported .scm version " + std::to_string(version)
                        + " (this reader decodes retail v" + std::to_string(kVersion) + ")");
    }

    const auto boneOffset = static_cast<std::size_t>(readU32(bytes, kOffBoneOffset));
    const auto boneCount = static_cast<std::size_t>(readU32(bytes, kOffBoneCount));
    const auto weightedBoneCount =
        static_cast<std::size_t>(readU32(bytes, kOffWeightedBoneCount));
    const auto vertexOffset = static_cast<std::size_t>(readU32(bytes, kOffVertexOffset));
    const auto vertexCount = static_cast<std::size_t>(readU32(bytes, kOffVertexCount));
    const auto indexOffset = static_cast<std::size_t>(readU32(bytes, kOffIndexOffset));
    const auto indexCount = static_cast<std::size_t>(readU32(bytes, kOffIndexCount));

    if (!markerPrecedes(bytes, boneOffset, kSkeletonMarker)
        || !markerPrecedes(bytes, vertexOffset, kVertexMarker)
        || !markerPrecedes(bytes, indexOffset, kIndexMarker)) {
        return fail(MapError::Code::BadHeader,
                    "a section offset does not point just past its SKEL/VTXL/TRIS marker, "
                    "so the header is not describing this file");
    }

    if (!fits(bytes, boneOffset, boneCount * kBoneSize)
        || !fits(bytes, vertexOffset, vertexCount * kVertexSize)
        || !fits(bytes, indexOffset, indexCount * 2)) {
        return fail(MapError::Code::Truncated,
                    "a section runs past the end of a " + std::to_string(bytes.size())
                        + "-byte file");
    }

    if (boneCount == 0 || vertexCount == 0 || indexCount == 0) {
        return fail(MapError::Code::BadGeometry, "model has no bones, vertices or indices");
    }
    if (indexCount % 3 != 0) {
        return fail(MapError::Code::BadGeometry,
                    std::to_string(indexCount) + " indices is not a whole number of triangles");
    }
    if (weightedBoneCount > boneCount) {
        return fail(MapError::Code::BadHeader,
                    "more weighted bones (" + std::to_string(weightedBoneCount)
                        + ") than bones (" + std::to_string(boneCount) + ")");
    }

    Model model;
    model.family = Family::SupremeCommander;

    // --- Bones -------------------------------------------------------------
    model.bones.reserve(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const std::size_t record = boneOffset + i * kBoneSize;

        auto name = readName(bytes, static_cast<std::size_t>(readU32(bytes, record + kBoneNameOffset)));
        if (!name) {
            return std::unexpected(name.error());
        }

        ModelBone bone;
        bone.name = std::move(*name);
        bone.parent = readI32(bytes, record + kBoneParent);
        bone.offset = readVec3(bytes, record + kBonePosition);
        // Stored w first, which is not universal — the corpus check is that bone
        // rotations come out unit-length and the roots come out identity.
        bone.rotation = {{readF32(bytes, record + kBoneRotation),
                          readF32(bytes, record + kBoneRotation + 4),
                          readF32(bytes, record + kBoneRotation + 8),
                          readF32(bytes, record + kBoneRotation + 12)}};

        if (bone.parent >= static_cast<int>(i)) {
            // Every retail model lists parents before children. Relying on that
            // is what lets the hierarchy accumulate in one pass — so it is
            // checked rather than assumed.
            return fail(MapError::Code::BadGeometry,
                        "bone " + std::to_string(i) + " names parent "
                            + std::to_string(bone.parent) + ", which does not precede it");
        }
        if (bone.parent < -1) {
            return fail(MapError::Code::BadGeometry,
                        "bone " + std::to_string(i) + " has a negative parent index "
                            + std::to_string(bone.parent));
        }

        if (bone.parent < 0) {
            bone.globalOffset = bone.offset;
            bone.globalRotation = bone.rotation;
        } else {
            const ModelBone& parent = model.bones[static_cast<std::size_t>(bone.parent)];
            // Real composition, not addition: a child's offset is expressed in
            // its parent's rotated frame. .s3o could add vectors because it has
            // no rotations at all.
            const std::array<float, 3> rotated =
                rotateByQuaternion(parent.globalRotation, bone.offset);
            bone.globalOffset = {{parent.globalOffset[0] + rotated[0],
                                  parent.globalOffset[1] + rotated[1],
                                  parent.globalOffset[2] + rotated[2]}};
            bone.globalRotation = multiplyQuaternions(parent.globalRotation, bone.rotation);
        }

        model.bones.push_back(std::move(bone));
    }

    // --- Vertices ----------------------------------------------------------
    model.vertices.reserve(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const std::size_t record = vertexOffset + i * kVertexSize;

        // Of the four bone indices only the first is used; see Scm.hpp.
        const auto boneIndex = std::to_integer<std::uint32_t>(bytes[record + kVertexBoneIndices]);
        if (boneIndex >= boneCount) {
            return fail(MapError::Code::BadGeometry,
                        "vertex " + std::to_string(i) + " names bone "
                            + std::to_string(boneIndex) + " of "
                            + std::to_string(boneCount));
        }

        model.vertices.push_back(ModelVertex{
            .position = readVec3(bytes, record + kVertexPosition),
            .normal = readVec3(bytes, record + kVertexNormal),
            .uv = {{readF32(bytes, record + kVertexUv0),
                    readF32(bytes, record + kVertexUv0 + 4)}},
            .boneIndex = boneIndex,
        });
    }

    // --- Indices -----------------------------------------------------------
    model.indices.reserve(indexCount);
    for (std::size_t i = 0; i < indexCount; ++i) {
        const std::uint16_t index = readU16(bytes, indexOffset + i * 2);
        if (index >= vertexCount) {
            return fail(MapError::Code::BadGeometry,
                        "index " + std::to_string(i) + " refers to vertex "
                            + std::to_string(index) + " of " + std::to_string(vertexCount));
        }
        model.indices.push_back(index);
    }

    computeBounds(model);

    // Unlike .s3o, the format carries no radius, height or midpoint — those live
    // in the unit's blueprint, which is Lua and a separate concern. Deriving
    // them from the geometry is what the .s3o path already does when its header
    // leaves them empty, so callers see no difference.
    model.radius = model.computedRadius();
    model.height = model.boundsMax[1] - model.boundsMin[1];
    model.midPos = {{(model.boundsMin[0] + model.boundsMax[0]) * 0.5f,
                     (model.boundsMin[1] + model.boundsMax[1]) * 0.5f,
                     (model.boundsMin[2] + model.boundsMax[2]) * 0.5f}};

    return model;
}

std::expected<Model, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return fail(MapError::Code::Truncated, "could not open \"" + path.string() + "\"");
    }

    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};

    auto model = load(std::as_bytes(std::span{data}));
    if (model) {
        model->name = path.stem().string();
    }
    return model;
}

} // namespace rm::scm
