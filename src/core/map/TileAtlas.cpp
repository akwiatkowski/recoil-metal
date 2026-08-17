#include "core/map/TileAtlas.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

/// Blocks along one axis of a tile at a given mip level: 8, 4, 2, 1.
[[nodiscard]] int tileBlocksAt(int level) noexcept {
    return (rm::smt::kTileSize / rm::kBlockTexels) >> level;
}

} // namespace

namespace rm {

int TileAtlas::mipWidth(int level) const noexcept {
    return std::max(1, widthTexels >> level);
}

int TileAtlas::mipHeight(int level) const noexcept {
    return std::max(1, heightTexels >> level);
}

std::size_t TileAtlas::mipBytesPerRow(int level) const noexcept {
    const auto blocksPerRow = static_cast<std::size_t>((mipWidth(level) + kBlockTexels - 1)
                                                       / kBlockTexels);
    return blocksPerRow * kBlockBytes;
}

std::size_t TileAtlas::mipBytes(int level) const noexcept {
    const auto blockRows = static_cast<std::size_t>((mipHeight(level) + kBlockTexels - 1)
                                                     / kBlockTexels);
    return mipBytesPerRow(level) * blockRows;
}

std::size_t TileAtlas::mipOffset(int level) const noexcept {
    std::size_t offset = 0;
    for (int i = 0; i < level; ++i) {
        offset += mipBytes(i);
    }
    return offset;
}

std::span<const std::byte> TileAtlas::mip(int level) const noexcept {
    if (level < 0 || level >= mipLevels) {
        return {};
    }
    const std::size_t offset = mipOffset(level);
    const std::size_t length = mipBytes(level);
    if (offset + length > data.size()) {
        return {};
    }
    return std::span{data}.subspan(offset, length);
}

std::expected<TileAtlas, MapError> buildTileAtlas(const smf::TileIndex& index,
                                                  const smt::TileSet& tiles) {
    if (index.tilesX <= 0 || index.tilesZ <= 0) {
        return std::unexpected(MapError{MapError::Code::BadGeometry,
                                        "tile index has no tiles"});
    }
    if (index.indices.size() != index.indexCount()) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "tile index holds " + std::to_string(index.indices.size()) + " entries but its "
                + std::to_string(index.tilesX) + "x" + std::to_string(index.tilesZ)
                + " geometry needs " + std::to_string(index.indexCount())});
    }

    TileAtlas atlas;
    atlas.widthTexels = index.tilesX * smt::kTileSize;
    atlas.heightTexels = index.tilesZ * smt::kTileSize;
    atlas.mipLevels = smt::kMipLevels;

    std::size_t total = 0;
    for (int level = 0; level < atlas.mipLevels; ++level) {
        total += atlas.mipBytes(level);
    }

    if (total > kMaxAtlasBytes) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "atlas for a " + std::to_string(index.tilesX) + "x" + std::to_string(index.tilesZ)
                + " tile map would need " + std::to_string(total / (1024 * 1024))
                + " MiB, over the " + std::to_string(kMaxAtlasBytes / (1024 * 1024))
                + " MiB cap; this map needs streaming, not one atlas"});
    }

    // 0xAA is what the engine memsets missing tiles to, which renders as a
    // conspicuous red. Pre-filling means an out-of-range index needs no special
    // case below — it simply keeps the fill.
    atlas.data.assign(total, std::byte{0xAA});

    for (int level = 0; level < atlas.mipLevels; ++level) {
        const int blocks = tileBlocksAt(level);          // blocks per tile side
        const std::size_t rowBytes = atlas.mipBytesPerRow(level);
        const std::size_t levelBase = atlas.mipOffset(level);
        const std::size_t copyBytes = static_cast<std::size_t>(blocks) * kBlockBytes;

        for (int ty = 0; ty < index.tilesZ; ++ty) {
            for (int tx = 0; tx < index.tilesX; ++tx) {
                const std::size_t slot = static_cast<std::size_t>(ty)
                                           * static_cast<std::size_t>(index.tilesX)
                                       + static_cast<std::size_t>(tx);
                const std::span<const std::byte> source = tiles.mip(index.indices[slot], level);
                if (source.empty()) {
                    continue;  // out-of-range index keeps the 0xAA fill
                }

                // Blocks are stored row-major, so a tile is copied one block-row
                // at a time — its rows are contiguous in the tile but strided in
                // the atlas.
                for (int row = 0; row < blocks; ++row) {
                    const std::size_t dstBlockRow =
                        static_cast<std::size_t>(ty) * static_cast<std::size_t>(blocks)
                        + static_cast<std::size_t>(row);
                    const std::size_t dst = levelBase + dstBlockRow * rowBytes
                                          + static_cast<std::size_t>(tx) * copyBytes;
                    const std::size_t src = static_cast<std::size_t>(row) * copyBytes;

                    if (dst + copyBytes > atlas.data.size() || src + copyBytes > source.size()) {
                        continue;
                    }
                    std::memcpy(atlas.data.data() + dst, source.data() + src, copyBytes);
                }
            }
        }
    }

    return atlas;
}

} // namespace rm
