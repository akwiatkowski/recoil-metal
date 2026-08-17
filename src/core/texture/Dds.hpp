#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

namespace rm::dds {

// Block-compressed formats a Recoil-era .dds may hold.
//
// All three are natively sampleable on Apple Silicon, so the payload is never
// decoded — the same deal as the terrain atlas. Recoil's own note is that
// "DXT1/3/5 pass straight to the GPU — this is the format to ship"
// (FAR report 05 §5.2).
enum class BlockFormat {
    Bc1,  ///< DXT1 — 8 bytes per 4x4 block, 1-bit alpha
    Bc2,  ///< DXT3 — 16 bytes per block, 4-bit explicit alpha
    Bc3,  ///< DXT5 — 16 bytes per block, interpolated alpha
};

/// Bytes per 4x4 block for a format.
[[nodiscard]] constexpr std::size_t blockBytes(BlockFormat format) noexcept {
    return format == BlockFormat::Bc1 ? 8u : 16u;
}

// A decoded DDS: dimensions, format, mip chain, and the compressed payload.
//
// Rows are stored top-down, as DDS does, and are uploaded to Metal unchanged.
// Recoil flips DDS vertically on load (`Bitmap.cpp:1236`) because OpenGL's
// texture origin is bottom-left; Metal's is top-left, so the flip is not wanted
// here. The consequence is that S3O texture coordinates — authored against
// OpenGL — need V inverted at sample time. Doing it in the shader is free;
// flipping BC blocks would mean reordering bits inside every block.
struct Texture {
    int width = 0;
    int height = 0;
    int mipLevels = 0;
    BlockFormat format = BlockFormat::Bc1;

    /// All mip levels concatenated, level 0 first.
    std::vector<std::byte> data;

    [[nodiscard]] int mipWidth(int level) const noexcept;
    [[nodiscard]] int mipHeight(int level) const noexcept;

    /// Bytes per row of blocks, which is what Metal wants as bytesPerRow.
    [[nodiscard]] std::size_t mipBytesPerRow(int level) const noexcept;
    [[nodiscard]] std::size_t mipBytes(int level) const noexcept;
    [[nodiscard]] std::size_t mipOffset(int level) const noexcept;

    [[nodiscard]] std::span<const std::byte> mip(int level) const noexcept;
};

/// `"DDS "` magic, then a 124-byte DDS_HEADER.
inline constexpr std::size_t kMagicSize = 4;
inline constexpr std::size_t kHeaderSize = 124;
inline constexpr std::size_t kDataOffset = kMagicSize + kHeaderSize;  // 128

// Parses a block-compressed .dds.
//
// Only DXT1/DXT3/DXT5 are accepted. Uncompressed and DX10-extended-header files
// are rejected with a specific message rather than guessed at — BAR's unit
// textures are 110 DXT5, 29 DXT3 and ~10 DXT1 by survey, so the uncovered cases
// are genuinely rare and silently mishandling them would be worse than failing.
[[nodiscard]] std::expected<Texture, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<Texture, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::dds
