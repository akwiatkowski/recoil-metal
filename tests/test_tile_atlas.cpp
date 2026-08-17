// Atlas assembly tests. The whole job is copying BC1 blocks to the right
// offsets, and an off-by-one there produces a texture that looks *almost* right
// — sheared or offset by a tile — which is exactly the sort of thing that
// survives a glance at the screen. So the offsets get pinned arithmetically.
#include <catch2/catch_test_macros.hpp>

#include "core/map/TileAtlas.hpp"

#include <cstdint>
#include <vector>

namespace {

/// A tile set where every tile is filled with a distinct byte value, so the
/// atlas can be checked by asking "which tile ended up here?".
[[nodiscard]] rm::smt::TileSet markedTiles(std::int32_t count) {
    rm::smt::TileSet set;
    set.tileCount = count;
    set.tiles.resize(static_cast<std::size_t>(count) * rm::smt::kTileBytes);
    for (std::int32_t tile = 0; tile < count; ++tile) {
        const std::size_t base = static_cast<std::size_t>(tile) * rm::smt::kTileBytes;
        for (std::size_t b = 0; b < rm::smt::kTileBytes; ++b) {
            set.tiles[base + b] = static_cast<std::byte>(tile + 1);
        }
    }
    return set;
}

[[nodiscard]] rm::smf::TileIndex indexOf(int tilesX, int tilesZ,
                                         std::vector<std::int32_t> indices) {
    rm::smf::TileIndex index;
    index.tilesX = tilesX;
    index.tilesZ = tilesZ;
    index.totalTiles = static_cast<std::int32_t>(indices.size());
    index.indices = std::move(indices);
    return index;
}

} // namespace

TEST_CASE("atlas dimensions follow from the tile grid") {
    // 1 texel per elmo, 32 texels per tile: a 2x3 tile grid is 64x96 texels.
    const auto atlas = rm::buildTileAtlas(indexOf(2, 3, {0, 0, 0, 0, 0, 0}), markedTiles(1));

    REQUIRE(atlas.has_value());
    REQUIRE(atlas->widthTexels == 64);
    REQUIRE(atlas->heightTexels == 96);
    REQUIRE(atlas->mipLevels == rm::smt::kMipLevels);
}

TEST_CASE("mip byte layout matches BC1 block arithmetic") {
    const auto atlas = rm::buildTileAtlas(indexOf(2, 2, {0, 0, 0, 0}), markedTiles(1));
    REQUIRE(atlas.has_value());

    // 64x64 texels = 16x16 blocks of 8 bytes = 2048 bytes at level 0,
    // then 512, 128, 32 as each axis halves.
    REQUIRE(atlas->mipBytes(0) == 2048);
    REQUIRE(atlas->mipBytes(1) == 512);
    REQUIRE(atlas->mipBytes(2) == 128);
    REQUIRE(atlas->mipBytes(3) == 32);

    REQUIRE(atlas->mipBytesPerRow(0) == 16 * rm::kBlockBytes);
    REQUIRE(atlas->mipBytesPerRow(1) == 8 * rm::kBlockBytes);

    REQUIRE(atlas->mipOffset(0) == 0);
    REQUIRE(atlas->mipOffset(1) == 2048);
    REQUIRE(atlas->mipOffset(2) == 2048 + 512);

    REQUIRE(atlas->data.size() == 2048 + 512 + 128 + 32);
    REQUIRE(atlas->mip(0).size() == 2048);
    REQUIRE(atlas->mip(rm::smt::kMipLevels).empty());
}

TEST_CASE("each tile lands at its own indexed position") {
    // A 2x2 grid with four different tiles. Every block of the atlas must carry
    // the marker of the tile whose cell it falls in — that catches both a wrong
    // row stride and a transposed x/z.
    const auto index = indexOf(2, 2, {0, 1, 2, 3});
    const auto atlas = rm::buildTileAtlas(index, markedTiles(4));
    REQUIRE(atlas.has_value());

    constexpr std::size_t kBlocksPerTile = rm::smt::kTileSize / rm::kBlockTexels;  // 8
    const std::size_t rowBytes = atlas->mipBytesPerRow(0);

    for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
            const std::size_t slot =
                static_cast<std::size_t>(ty) * 2 + static_cast<std::size_t>(tx);
            const auto expected = static_cast<std::byte>(index.indices[slot] + 1);

            // Check the tile's first and last block.
            for (const auto [br, bc] : {std::pair<std::size_t, std::size_t>{0, 0},
                                        std::pair<std::size_t, std::size_t>{kBlocksPerTile - 1,
                                                                            kBlocksPerTile - 1}}) {
                const std::size_t blockRow =
                    static_cast<std::size_t>(ty) * kBlocksPerTile + br;
                const std::size_t offset =
                    blockRow * rowBytes
                    + (static_cast<std::size_t>(tx) * kBlocksPerTile + bc) * rm::kBlockBytes;
                REQUIRE(atlas->data[offset] == expected);
            }
        }
    }
}

TEST_CASE("a repeated index reuses the same tile, which is SMF's whole compression story") {
    // Every cell points at tile 0; nothing else may appear in the atlas.
    const auto atlas = rm::buildTileAtlas(indexOf(2, 2, {0, 0, 0, 0}), markedTiles(3));
    REQUIRE(atlas.has_value());

    const auto mip0 = atlas->mip(0);
    for (const std::byte b : mip0) {
        REQUIRE(b == std::byte{1});
    }
}

TEST_CASE("an out-of-range index keeps the engine's 0xAA fill instead of failing") {
    // A missing .smt is not fatal in Recoil either — those tiles get memset to
    // 0xAA and render conspicuously (SMFGroundTextures.cpp:147-157).
    const auto atlas = rm::buildTileAtlas(indexOf(2, 1, {0, 99}), markedTiles(1));
    REQUIRE(atlas.has_value());

    const std::size_t rowBytes = atlas->mipBytesPerRow(0);
    REQUIRE(atlas->data[0] == std::byte{1});  // tile 0 placed

    // The second tile's first block, still filled.
    constexpr std::size_t kBlocksPerTile = rm::smt::kTileSize / rm::kBlockTexels;
    REQUIRE(atlas->data[kBlocksPerTile * rm::kBlockBytes]
            == std::byte{0xAA});
    REQUIRE(rowBytes == 2 * kBlocksPerTile * rm::kBlockBytes);
}

TEST_CASE("a mismatched index array is rejected rather than read out of bounds") {
    rm::smf::TileIndex index;
    index.tilesX = 4;
    index.tilesZ = 4;
    index.indices = {0, 0};  // needs 16

    const auto atlas = rm::buildTileAtlas(index, markedTiles(1));

    REQUIRE_FALSE(atlas.has_value());
    REQUIRE(atlas.error().code == rm::MapError::Code::BadGeometry);
}

TEST_CASE("an empty tile grid is rejected") {
    const auto atlas = rm::buildTileAtlas(indexOf(0, 0, {}), markedTiles(1));

    REQUIRE_FALSE(atlas.has_value());
    REQUIRE(atlas.error().code == rm::MapError::Code::BadGeometry);
}

TEST_CASE("an atlas beyond the size cap is refused with a useful message") {
    // A 4096-square map wants ~700 MB. Streaming is the right answer there;
    // silently allocating it is not.
    rm::smf::TileIndex index;
    index.tilesX = 1024;
    index.tilesZ = 1024;
    index.indices.assign(1024u * 1024u, 0);

    const auto atlas = rm::buildTileAtlas(index, markedTiles(1));

    REQUIRE_FALSE(atlas.has_value());
    REQUIRE(atlas.error().message.find("streaming") != std::string::npos);
}
