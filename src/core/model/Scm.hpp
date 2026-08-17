#pragma once

#include "core/Error.hpp"
#include "core/model/Model.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

namespace rm::scm {

// "MODL", not NUL-terminated.
inline constexpr char kMagic[4] = {'M', 'O', 'D', 'L'};

/// The only version retail Forged Alliance ships, and the one this decodes.
inline constexpr std::uint32_t kVersion = 5;

// Header on disk: the magic then eleven uint32s — version, then five section
// offsets and their counts. Unlike .scmap, this format IS offset-based, so every
// offset is bounds-checked before use rather than walked to.
inline constexpr std::size_t kHeaderSize = 4 + 11 * 4;  // 48

// Bone record: a 4x4 rest-pose inverse, a position, a quaternion, the offset of
// its name, its parent's index, and two reserved words.
//   64 + 12 + 16 + 4 + 4 + 8 = 108
inline constexpr std::size_t kBoneSize = 108;

// Vertex record: position, tangent, normal, binormal, two UV pairs, and four
// bone indices.
//   12 * 4 + 8 * 2 + 4 = 68
inline constexpr std::size_t kVertexSize = 68;

// Section markers immediately precede the data their header offset points at.
// Checking them turns a plausible-but-wrong offset into a loud failure, which is
// the only defence an offset-based format has.
inline constexpr char kSkeletonMarker[4] = {'S', 'K', 'E', 'L'};
inline constexpr char kVertexMarker[4] = {'V', 'T', 'X', 'L'};
inline constexpr char kIndexMarker[4] = {'T', 'R', 'I', 'S'};

// The layout below was derived byte-by-byte from the retail corpus and then
// found to agree, field for field, with the reference reader ADR-004 cites —
// FAR's `tools/scstudio/sc_io.py:36-42`, whose struct strings are `4s11I` for
// the header, `16f3f4f4i` per bone and `3f3f3f3f2f2f4B` per vertex. Two
// independent derivations agreeing is worth more than either alone.
//
// Parses a Supreme Commander .scm into the shared Model representation.
//
// The whole point of ADR-004: this fills the same struct the .s3o loader does,
// so everything downstream — placement, batching, the renderer — is untouched by
// a second content family arriving. What it must record is where the two
// genuinely differ; see Family in Model.hpp.
//
// Indices are uint16 on disk and widen to the shared uint32. Of the four
// per-vertex bone indices only the first is kept: the format reserves four for
// weighted skinning, but every retail model uses one, and inventing weights we
// have no data for would be worse than dropping fields we cannot use.
[[nodiscard]] std::expected<Model, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<Model, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::scm
