#include "core/model/S3o.hpp"

#include "core/map/ByteReader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace {

// Field offsets within S3OHeader (s3o.h:69-81).
constexpr std::size_t kOffVersion   = 12;
constexpr std::size_t kOffRadius    = 16;
constexpr std::size_t kOffHeight    = 20;
constexpr std::size_t kOffMidX      = 24;
constexpr std::size_t kOffMidY      = 28;
constexpr std::size_t kOffMidZ      = 32;
constexpr std::size_t kOffRootPiece = 36;
constexpr std::size_t kOffTexture1  = 44;
constexpr std::size_t kOffTexture2  = 48;

// Field offsets within Piece (s3o.h:9-23).
constexpr std::size_t kPieceName            = 0;
constexpr std::size_t kPieceNumChildren     = 4;
constexpr std::size_t kPieceChildren        = 8;
constexpr std::size_t kPieceNumVertices     = 12;
constexpr std::size_t kPieceVertices        = 16;
constexpr std::size_t kPiecePrimitiveType   = 24;
constexpr std::size_t kPieceVertexTableSize = 28;
constexpr std::size_t kPieceVertexTable     = 32;
constexpr std::size_t kPieceXOffset         = 40;
constexpr std::size_t kPieceYOffset         = 44;
constexpr std::size_t kPieceZOffset         = 48;

/// The engine only accepts version 0 (s3o.h:71).
constexpr std::int32_t kRequiredVersion = 0;

/// A piece name longer than this is treated as corruption rather than walked to
/// the end of the file looking for a NUL.
constexpr std::size_t kMaxNameLength = 256;

/// Below this the engine treats the header's radius/height as unsupplied and
/// derives them from the geometry instead (S3OParser.cpp:79-80).
constexpr float kMetadataEpsilon = 0.01f;

using rm::MapError;

[[nodiscard]] bool fits(std::span<const std::byte> bytes, std::size_t offset,
                        std::size_t length) noexcept {
    return offset <= bytes.size() && length <= bytes.size() - offset;
}

/// Reads a NUL-terminated string at an absolute offset. An offset of 0 means
/// "absent", matching how the engine treats the texture fields
/// (S3OParser.cpp:65-66).
[[nodiscard]] std::expected<std::string, MapError> readString(std::span<const std::byte> bytes,
                                                              std::size_t offset,
                                                              const char* what) {
    if (offset == 0) {
        return std::string{};
    }
    if (offset >= bytes.size()) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            std::string{what} + " name offset " + std::to_string(offset)
                + " is past the end of a " + std::to_string(bytes.size()) + "-byte file"});
    }

    std::string out;
    for (std::size_t i = offset; i < bytes.size(); ++i) {
        const auto c = static_cast<char>(bytes[i]);
        if (c == '\0') {
            return out;
        }
        if (out.size() >= kMaxNameLength) {
            return std::unexpected(MapError{
                MapError::Code::BadHeader,
                std::string{what} + " name exceeds " + std::to_string(kMaxNameLength)
                    + " characters; the offset is probably not a string"});
        }
        out.push_back(c);
    }

    return std::unexpected(MapError{MapError::Code::Truncated,
                                    std::string{what} + " name is not NUL-terminated"});
}

/// Walk state threaded through the recursive piece load.
struct Walk {
    std::span<const std::byte> bytes;
    rm::Model& model;
    /// Offsets already visited. A corrupt or hostile file can point a child back
    /// at an ancestor; without this the walk would not terminate.
    std::unordered_set<std::size_t> visited;
};

[[nodiscard]] std::expected<void, MapError> loadPiece(Walk& walk, std::size_t offset, int parent,
                                                      std::array<float, 3> parentGlobal);

/// Appends one piece's geometry, converting its index table to a triangle list.
[[nodiscard]] std::expected<void, MapError> appendGeometry(Walk& walk, std::size_t pieceOffset,
                                                          std::uint32_t boneIndex) {
    const std::span<const std::byte> bytes = walk.bytes;

    const std::int32_t vertexCount = rm::readI32(bytes, pieceOffset + kPieceNumVertices);
    const std::int32_t indexCount = rm::readI32(bytes, pieceOffset + kPieceVertexTableSize);
    const std::int32_t primitive = rm::readI32(bytes, pieceOffset + kPiecePrimitiveType);

    if (vertexCount < 0 || indexCount < 0) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "piece declares " + std::to_string(vertexCount) + " vertices and "
                + std::to_string(indexCount) + " indices"});
    }

    const auto vertexBase = static_cast<std::uint32_t>(walk.model.vertices.size());

    // --- Vertices ----------------------------------------------------------
    if (vertexCount > 0) {
        const auto verticesAt = static_cast<std::size_t>(
            rm::readI32(bytes, pieceOffset + kPieceVertices));
        const std::size_t vertexBytes = static_cast<std::size_t>(vertexCount) * rm::s3o::kVertexSize;

        if (!fits(bytes, verticesAt, vertexBytes)) {
            return std::unexpected(MapError{
                MapError::Code::Truncated,
                "piece vertex array needs " + std::to_string(vertexBytes) + " bytes at offset "
                    + std::to_string(verticesAt) + " but the file is only "
                    + std::to_string(bytes.size()) + " bytes"});
        }

        walk.model.vertices.reserve(walk.model.vertices.size()
                                    + static_cast<std::size_t>(vertexCount));

        for (std::int32_t i = 0; i < vertexCount; ++i) {
            const std::size_t at = verticesAt + static_cast<std::size_t>(i) * rm::s3o::kVertexSize;

            std::array<float, 3> normal{{rm::readF32(bytes, at + 12), rm::readF32(bytes, at + 16),
                                         rm::readF32(bytes, at + 20)}};

            // The engine zeroes non-finite normals rather than propagating NaN
            // into lighting (S3OParser.cpp:143-147); a zero normal shades flat,
            // which is visible but not poisonous.
            const bool finite = std::isfinite(normal[0]) && std::isfinite(normal[1])
                              && std::isfinite(normal[2]);
            if (finite) {
                const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]
                                             + normal[2] * normal[2]);
                if (length > 0.0f) {
                    normal = {{normal[0] / length, normal[1] / length, normal[2] / length}};
                }
            } else {
                normal = {{0.0f, 0.0f, 0.0f}};
            }

            walk.model.vertices.push_back(rm::ModelVertex{
                .position = {{rm::readF32(bytes, at + 0), rm::readF32(bytes, at + 4),
                              rm::readF32(bytes, at + 8)}},
                .normal = normal,
                .uv = {{rm::readF32(bytes, at + 24), rm::readF32(bytes, at + 28)}},
                .boneIndex = boneIndex,
            });
        }
    }

    // --- Index table -------------------------------------------------------
    if (indexCount == 0) {
        return {};
    }

    const auto tableAt = static_cast<std::size_t>(
        rm::readI32(bytes, pieceOffset + kPieceVertexTable));
    const std::size_t tableBytes = static_cast<std::size_t>(indexCount) * sizeof(std::uint32_t);

    if (!fits(bytes, tableAt, tableBytes)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "piece index table needs " + std::to_string(tableBytes) + " bytes at offset "
                + std::to_string(tableAt) + " but the file is only "
                + std::to_string(bytes.size()) + " bytes"});
    }

    std::vector<std::uint32_t> local(static_cast<std::size_t>(indexCount));
    for (std::size_t i = 0; i < local.size(); ++i) {
        local[i] = rm::readU32(bytes, tableAt + i * sizeof(std::uint32_t));
    }

    // Emits one triangle, rebased onto the merged vertex array. Out-of-range
    // indices are dropped rather than failing the load: a single bad triangle in
    // one piece should not lose the whole model, which is also the engine's
    // posture towards malformed s3o geometry.
    const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const auto count = static_cast<std::uint32_t>(vertexCount);
        if (a >= count || b >= count || c >= count) {
            return;
        }
        walk.model.indices.push_back(vertexBase + a);
        walk.model.indices.push_back(vertexBase + b);
        walk.model.indices.push_back(vertexBase + c);
    };

    switch (static_cast<rm::s3o::PrimitiveType>(primitive)) {
        case rm::s3o::PrimitiveType::Triangles: {
            for (std::size_t i = 0; i + 2 < local.size(); i += 3) {
                emit(local[i], local[i + 1], local[i + 2]);
            }
        } break;

        case rm::s3o::PrimitiveType::TriangleStrip: {
            // Every consecutive triple, skipping any that spans an end marker.
            // Winding is deliberately NOT alternated — see the header comment.
            if (local.size() < 3) {
                break;
            }
            for (std::size_t i = 0; i + 2 < local.size(); ++i) {
                if (local[i] == rm::s3o::kStripEnd || local[i + 1] == rm::s3o::kStripEnd
                    || local[i + 2] == rm::s3o::kStripEnd) {
                    continue;
                }
                emit(local[i], local[i + 1], local[i + 2]);
            }
        } break;

        case rm::s3o::PrimitiveType::Quads: {
            // The engine discards the whole piece when the count is not a
            // multiple of four (S3OParser.cpp:227-231).
            if (local.size() % 4 != 0) {
                break;
            }
            for (std::size_t i = 0; i < local.size(); i += 4) {
                emit(local[i], local[i + 1], local[i + 2]);
                emit(local[i], local[i + 2], local[i + 3]);
            }
        } break;

        default: {
            return std::unexpected(MapError{
                MapError::Code::BadHeader,
                "piece declares unknown primitive type " + std::to_string(primitive)});
        }
    }

    return {};
}

std::expected<void, MapError> loadPiece(Walk& walk, std::size_t offset, int parent,
                                        std::array<float, 3> parentGlobal) {
    if (!fits(walk.bytes, offset, rm::s3o::kPieceSize)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "piece at offset " + std::to_string(offset) + " runs past the end of a "
                + std::to_string(walk.bytes.size()) + "-byte file"});
    }
    if (!walk.visited.insert(offset).second) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "piece tree revisits offset " + std::to_string(offset) + "; the hierarchy is cyclic"});
    }
    if (walk.model.bones.size() >= static_cast<std::size_t>(rm::s3o::kMaxPieces)) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "model exceeds the engine's limit of " + std::to_string(rm::s3o::kMaxPieces)
                + " pieces"});
    }

    auto name = readString(walk.bytes,
                           static_cast<std::size_t>(rm::readI32(walk.bytes, offset + kPieceName)),
                           "piece");
    if (!name) {
        return std::unexpected(name.error());
    }

    const std::array<float, 3> localOffset{{rm::readF32(walk.bytes, offset + kPieceXOffset),
                                            rm::readF32(walk.bytes, offset + kPieceYOffset),
                                            rm::readF32(walk.bytes, offset + kPieceZOffset)}};

    const std::array<float, 3> globalOffset{{parentGlobal[0] + localOffset[0],
                                             parentGlobal[1] + localOffset[1],
                                             parentGlobal[2] + localOffset[2]}};

    const auto boneIndex = static_cast<std::uint32_t>(walk.model.bones.size());
    walk.model.bones.push_back(rm::ModelBone{
        .name = std::move(*name),
        .parent = parent,
        .offset = localOffset,
        .globalOffset = globalOffset,
    });

    if (auto geometry = appendGeometry(walk, offset, boneIndex); !geometry) {
        return std::unexpected(geometry.error());
    }

    // --- Children ----------------------------------------------------------
    const std::int32_t childCount = rm::readI32(walk.bytes, offset + kPieceNumChildren);
    if (childCount < 0) {
        return std::unexpected(MapError{MapError::Code::BadGeometry,
                                        "piece declares " + std::to_string(childCount)
                                            + " children"});
    }
    if (childCount == 0) {
        return {};
    }

    const auto childTable = static_cast<std::size_t>(
        rm::readI32(walk.bytes, offset + kPieceChildren));
    const std::size_t childBytes = static_cast<std::size_t>(childCount) * sizeof(std::int32_t);

    if (!fits(walk.bytes, childTable, childBytes)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "child table for " + std::to_string(childCount) + " children at offset "
                + std::to_string(childTable) + " runs past the end of the file"});
    }

    for (std::int32_t i = 0; i < childCount; ++i) {
        const auto childOffset = static_cast<std::size_t>(rm::readI32(
            walk.bytes, childTable + static_cast<std::size_t>(i) * sizeof(std::int32_t)));

        // The child's index in `bones` is not known before the recursive call,
        // so the parent link is passed down rather than patched up afterwards.
        if (auto child = loadPiece(walk, childOffset, static_cast<int>(boneIndex), globalOffset);
            !child) {
            return std::unexpected(child.error());
        }
    }

    return {};
}

} // namespace

namespace rm {

namespace s3o {

std::expected<Model, MapError> load(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "file is " + std::to_string(bytes.size()) + " bytes, shorter than the "
                + std::to_string(kHeaderSize) + "-byte S3O header"});
    }

    // Compared as a C string, like the engine does for SMF; the field is the 11
    // characters plus their terminating NUL.
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return std::unexpected(MapError{
            MapError::Code::NotSmf,
            "not a Recoil S3O model: expected magic \"Spring unit\""});
    }

    const std::int32_t version = readI32(bytes, kOffVersion);
    if (version != kRequiredVersion) {
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "S3O version is " + std::to_string(version) + ", expected "
                + std::to_string(kRequiredVersion)});
    }

    Model model;
    model.radius = readF32(bytes, kOffRadius);
    model.height = readF32(bytes, kOffHeight);
    model.midPos = {{readF32(bytes, kOffMidX), readF32(bytes, kOffMidY),
                     readF32(bytes, kOffMidZ)}};

    auto texture1 = readString(bytes, static_cast<std::size_t>(readI32(bytes, kOffTexture1)),
                               "texture1");
    if (!texture1) {
        return std::unexpected(texture1.error());
    }
    auto texture2 = readString(bytes, static_cast<std::size_t>(readI32(bytes, kOffTexture2)),
                               "texture2");
    if (!texture2) {
        return std::unexpected(texture2.error());
    }
    model.textures = {std::move(*texture1), std::move(*texture2)};

    Walk walk{bytes, model, {}};
    const auto rootPiece = static_cast<std::size_t>(readI32(bytes, kOffRootPiece));
    if (auto root = loadPiece(walk, rootPiece, -1, {{0.0f, 0.0f, 0.0f}}); !root) {
        return std::unexpected(root.error());
    }

    computeBounds(model);

    // The engine derives these from the geometry when the header leaves them at
    // (near) zero (S3OParser.cpp:79-80). Same rule here so callers need not know
    // which of the two supplied the value.
    if (model.radius <= kMetadataEpsilon) {
        model.radius = model.computedRadius();
    }
    if (model.height <= kMetadataEpsilon) {
        model.height = model.boundsMax[1] - model.boundsMin[1];
    }

    return model;
}

std::expected<Model, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(MapError{MapError::Code::Truncated,
                                        "could not open \"" + path.string() + "\""});
    }

    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};

    auto model = load(std::as_bytes(std::span{data}));
    if (model) {
        model->name = path.stem().string();
    }
    return model;
}

} // namespace s3o
} // namespace rm
