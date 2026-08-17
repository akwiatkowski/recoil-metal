#pragma once

#include "core/Error.hpp"
#include "core/model/Model.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>

namespace rm::s3o {

// "Spring unit\0" — 12 bytes including the terminator (s3o.h:70).
inline constexpr char kMagic[] = "Spring unit";

/// S3OHeader on disk: char[12] + int + 5 floats + 4 ints (s3o.h:69-81).
inline constexpr std::size_t kHeaderSize = 12 + 4 + 5 * 4 + 4 * 4;  // 52

/// Piece on disk: 10 ints + 3 floats (s3o.h:9-23).
inline constexpr std::size_t kPieceSize = 10 * 4 + 3 * 4;  // 52

/// Vertex on disk: position, normal, u, v (s3o.h:44-53).
inline constexpr std::size_t kVertexSize = 8 * 4;  // 32

// Primitive types a piece's index table may use (s3o.h:16).
enum class PrimitiveType : std::int32_t {
    Triangles = 0,
    TriangleStrip = 1,
    Quads = 2,
};

/// End-of-strip marker inside a strip index table (s3o.h:18).
inline constexpr std::uint32_t kStripEnd = 0xFFFFFFFFu;

// The engine's cap; exceeding it swaps in a dummy model (3DModelDefs.hpp:8,
// enforced at IModelParser.cpp:426-429). We reject instead of substituting.
inline constexpr int kMaxPieces = 65534;

// Parses a Recoil .s3o model into the shared Model representation.
//
// Everything in the file is reached through absolute offsets — piece names,
// child tables, vertex arrays and index tables all live wherever the writer put
// them — so every one is bounds-checked before use. Real .s3o files in the wild
// are known to contain offsets pointing past the end of the buffer
// (S3OParser.cpp:115-117 documents the engine working around exactly that), so
// this is not defensive theatre.
//
// Triangulation matches the engine (S3OParser.cpp:197-254): strips emit every
// consecutive triple, skipping any containing the end marker, and quads split
// 0-1-2 / 0-2-3. Note that the strip conversion does NOT alternate winding, so
// every other triangle faces the other way — that is what the engine does and
// what the content therefore expects, and it is harmless here because the
// renderer does not cull.
[[nodiscard]] std::expected<Model, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<Model, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::s3o
