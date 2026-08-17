#pragma once

#include "core/Error.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace rm {

// The map's ground texture, assembled into one BC1 image with mip levels.
//
// WHY AN ATLAS AND NOT A TEXTURE ARRAY: a real map has far more tiles than
// Metal allows array layers — Angel Crossing has 65 536 tiles against a limit
// of 2 048. But the tiles tile the map exactly (1 texel per elmo, 32 texels per
// tile), so pasting each one at its indexed position reconstitutes the full
// ground texture: mapx*8 by mapy*8 texels. For a 1024-square map that is
// 8192x8192 BC1 = 32 MB, which is nothing on unified memory.
//
// The DXT1 blocks are copied verbatim, never decoded. Apple Silicon supports
// BC1 natively, so this data reaches the GPU untouched — the ETC1 transcode
// Recoil performs for drivers without S3TC (SMFGroundTextures.cpp:290-317) has
// no counterpart here.
//
// Mip levels come from the .smt itself, which stores 32/16/8/4 per tile. Beyond
// those four the atlas would need real filtering across tile boundaries, which
// means decompressing BC1 — deliberately not done. Four levels is enough for
// the framed view of a map (at ~8 texels per pixel the smallest level is the
// one being sampled) and everything closer.
struct TileAtlas {
    int widthTexels = 0;
    int heightTexels = 0;
    int mipLevels = 0;

    /// All mip levels concatenated, level 0 first.
    std::vector<std::byte> data;

    [[nodiscard]] int mipWidth(int level) const noexcept;
    [[nodiscard]] int mipHeight(int level) const noexcept;

    /// Byte offset of a mip level within data.
    [[nodiscard]] std::size_t mipOffset(int level) const noexcept;
    [[nodiscard]] std::size_t mipBytes(int level) const noexcept;

    /// Bytes per row of BC1 blocks, which is what Metal wants as bytesPerRow.
    [[nodiscard]] std::size_t mipBytesPerRow(int level) const noexcept;

    [[nodiscard]] std::span<const std::byte> mip(int level) const noexcept;
};

// BC1: 4x4 texels per block, 8 bytes per block.
inline constexpr int kBlockTexels = 4;
inline constexpr std::size_t kBlockBytes = 8;

// Refuse to assemble an atlas larger than this. A 4096-square map would want
// ~700 MB, at which point streaming or sparse textures are the right answer and
// silently allocating it would be the wrong one.
inline constexpr std::size_t kMaxAtlasBytes = 512u * 1024u * 1024u;

// Pastes every indexed tile into its place in the atlas.
//
// A tile index outside the tile set is filled with 0xAA bytes rather than
// failing, matching what the engine does for a missing .smt
// (SMFGroundTextures.cpp:147-157): the map still renders, and the affected
// area is conspicuous instead of silently black.
[[nodiscard]] std::expected<TileAtlas, MapError> buildTileAtlas(const smf::TileIndex& index,
                                                                const smt::TileSet& tiles);

} // namespace rm
