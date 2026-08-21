// Packing unit icons into one atlas: block arithmetic, so a misplaced icon is an assertion
// rather than something to squint at.
//
// The interesting property is that NOTHING IS DECODED. A 64x64 DXT5 icon is 16x16 blocks of 16
// bytes, and 64 is a multiple of 4, so an icon placed at a multiple of 64 lands on a block
// boundary and packing is a memcpy. These tests are what say the strides are right — an atlas
// with a wrong row stride still looks like an atlas, just sheared.
#include <catch2/catch_test_macros.hpp>

#include "core/ui/IconAtlas.hpp"

#include <cstring>
#include <vector>

using rm::dds::Format;
using rm::dds::Texture;
using rm::ui::iconUv;
using rm::ui::kBlockBytes;
using rm::ui::kIconSide;
using rm::ui::packIcons;

namespace {

/// A 64x64 DXT5 icon whose every byte is `fill`, so a packed block is identifiable by value.
[[nodiscard]] Texture anIcon(std::byte fill) {
    Texture icon;
    icon.width = kIconSide;
    icon.height = kIconSide;
    icon.mipLevels = 1;
    icon.format = Format::Bc3;
    icon.data.assign(16u * 16u * kBlockBytes, fill);
    return icon;
}

/// The first byte of the block at (blockX, blockY) in the atlas.
[[nodiscard]] std::byte blockAt(const Texture& atlas, int blockX, int blockY) {
    const auto blocksAcross = static_cast<std::size_t>(atlas.width / 4);
    const std::size_t index =
        (static_cast<std::size_t>(blockY) * blocksAcross + static_cast<std::size_t>(blockX))
        * kBlockBytes;
    return index < atlas.data.size() ? atlas.data[index] : std::byte{0xFF};
}

} // namespace

TEST_CASE("nothing to pack is an empty atlas, not a blank one", "[ui][icons]") {
    // An empty texture is what the renderer tests for to decide whether to draw icons at all. A
    // 512x512 of transparent black would upload a quarter-megabyte to draw nothing.
    CHECK(packIcons({}).data.empty());
}

TEST_CASE("an icon lands on its own slot's blocks", "[ui][icons]") {
    const std::vector<Texture> icons{anIcon(std::byte{0x11}), anIcon(std::byte{0x22}),
                                     anIcon(std::byte{0x33})};
    const Texture atlas = packIcons(icons);

    REQUIRE(atlas.format == Format::Bc3);
    REQUIRE(atlas.width == kIconSide * rm::ui::kAtlasColumns);
    REQUIRE(atlas.height == atlas.width);

    // Slot 0 at block (0,0), slot 1 at (16,0), slot 2 at (32,0) — 16 blocks per icon.
    CHECK(blockAt(atlas, 0, 0) == std::byte{0x11});
    CHECK(blockAt(atlas, 16, 0) == std::byte{0x22});
    CHECK(blockAt(atlas, 32, 0) == std::byte{0x33});

    // And the LAST block row of the first icon is still the first icon — the row stride is what
    // this catches, and a wrong one shears every icon into its neighbour.
    CHECK(blockAt(atlas, 0, 15) == std::byte{0x11});
    CHECK(blockAt(atlas, 15, 15) == std::byte{0x11});
}

TEST_CASE("the ninth icon wraps to the second row", "[ui][icons]") {
    std::vector<Texture> icons;
    for (int i = 0; i < 9; ++i) {
        icons.push_back(anIcon(static_cast<std::byte>(i + 1)));
    }
    const Texture atlas = packIcons(icons);

    // Slot 8 is row 1, column 0 — block (0, 16).
    CHECK(blockAt(atlas, 0, 16) == std::byte{9});
    // And slot 7 is the end of row 0.
    CHECK(blockAt(atlas, 7 * 16, 0) == std::byte{8});
}

TEST_CASE("an icon that is not 64x64 DXT5 leaves its slot blank", "[ui][icons]") {
    // A mod shipping a 128x128 icon is a real possibility. Scaling it would need a decode and a
    // resample; reinterpreting it would draw a quarter of it at four times the size. A blank
    // square reads as "no icon", which is exactly what is true.
    Texture wrongSize = anIcon(std::byte{0x44});
    wrongSize.width = 128;
    wrongSize.height = 128;

    Texture wrongFormat = anIcon(std::byte{0x55});
    wrongFormat.format = Format::Bgra8;

    const std::vector<Texture> icons{wrongSize, wrongFormat, anIcon(std::byte{0x66})};
    const Texture atlas = packIcons(icons);

    CHECK(blockAt(atlas, 0, 0) == std::byte{0});   // skipped, and zeroed
    CHECK(blockAt(atlas, 16, 0) == std::byte{0});  // skipped
    // THE THIRD KEEPS ITS OWN SLOT. A packer that compacted past the skipped ones would shift
    // every later icon one place left, and the panel would then label each icon with its
    // neighbour's name — which looks like a content bug and is not one.
    CHECK(blockAt(atlas, 32, 0) == std::byte{0x66});
}

TEST_CASE("nothing packable is an empty atlas", "[ui][icons]") {
    Texture wrong = anIcon(std::byte{0x77});
    wrong.format = Format::Bc1;
    const std::vector<Texture> icons{wrong};

    CHECK(packIcons(icons).data.empty());
}

TEST_CASE("uv rectangles tile the atlas and match their slots", "[ui][icons]") {
    const auto first = iconUv(0);
    CHECK(first.u0 == 0.0f);
    CHECK(first.v0 == 0.0f);
    CHECK(first.u1 == 1.0f / static_cast<float>(rm::ui::kAtlasColumns));

    // Slot 8 starts the second row: back to the left edge, one step down.
    const auto ninth = iconUv(8);
    CHECK(ninth.u0 == 0.0f);
    CHECK(ninth.v0 == first.u1);

    // Adjacent slots share an edge exactly — a gap would show as a seam and an overlap would
    // bleed a neighbour's pixels into the cell.
    CHECK(iconUv(0).u1 == iconUv(1).u0);
}

TEST_CASE("a slot past the end is nothing rather than a wrap", "[ui][icons]") {
    // Wrapping would draw somebody else's icon, which looks like a content bug and is not one.
    const auto past = iconUv(rm::ui::kAtlasColumns * rm::ui::kAtlasColumns);
    CHECK(past.u0 == 0.0f);
    CHECK(past.u1 == 0.0f);
    CHECK(past.v1 == 0.0f);
}
