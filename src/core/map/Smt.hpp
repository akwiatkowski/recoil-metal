#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

namespace rm::smt {

// "spring tilefile\0" — 16 bytes including the terminator (SMFFormat.h:177).
inline constexpr char kMagic[] = "spring tilefile";

/// TileFileHeader on disk: char[16] + 4 int32 (SMFFormat.h:175-183).
inline constexpr std::size_t kHeaderSize = 16 + 4 * 4;

// Every tile is a 32x32 DXT1 image plus three mips (16, 8, 4), stored back to
// back: 512 + 128 + 32 + 8 = 680 bytes (SMALL_TILE_SIZE, SMFFormat.h:28).
inline constexpr int kTileSize = 32;
inline constexpr int kMipLevels = 4;
inline constexpr std::size_t kTileBytes = 680;

/// Byte offset of each mip within a tile (TILE_MIP_OFFSET, SMFGroundTextures.cpp:515).
inline constexpr std::size_t kMipOffsets[kMipLevels] = {0, 512, 640, 672};

/// Byte size of each mip within a tile.
inline constexpr std::size_t kMipBytes[kMipLevels] = {512, 128, 32, 8};

// A decoded .smt: the header's tile count plus the raw DXT1 payload.
//
// The tile data is kept compressed. Apple Silicon supports BC1 natively
// (MTLDevice::supportsBCTextureCompression is true on an M4 Pro), so these
// bytes go to the GPU untouched — no decode, no transcode. Recoil has to
// recompress the whole set to ETC1 for drivers without S3TC
// (SMFGroundTextures.cpp:290-317); that pass simply does not apply here.
struct TileSet {
    std::int32_t tileCount = 0;
    std::vector<std::byte> tiles;  ///< tileCount * kTileBytes

    /// Raw bytes of one mip level of one tile. Empty span if out of range.
    [[nodiscard]] std::span<const std::byte> mip(std::int32_t tile, int level) const noexcept;
};

/// Parses a .smt image. Validates exactly what the engine validates
/// (SMFGroundTextures.cpp:161-168): magic, version 1, tileSize 32,
/// compressionType 1 (= DXT1).
[[nodiscard]] std::expected<TileSet, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<TileSet, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::smt
