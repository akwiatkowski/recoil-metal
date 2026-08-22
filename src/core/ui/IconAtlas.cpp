#include "core/ui/IconAtlas.hpp"

#include <algorithm>
#include <cstring>

namespace rm::ui {
namespace {

/// Blocks across one icon, and across the whole atlas.
constexpr int kIconBlocks = kIconSide / kBlockSide;
constexpr int kAtlasBlocks = kIconBlocks * kAtlasColumns;
constexpr int kAtlasSide = kIconSide * kAtlasColumns;

/// Whether a texture is one this packer can copy blocks out of: BC3, block-aligned sides,
/// and no larger than a cell. Smaller is fine — it lands in the cell's top-left corner and
/// `iconUvSized` addresses it.
[[nodiscard]] bool packable(const dds::Texture& icon) noexcept {
    return icon.width > 0 && icon.height > 0 && icon.width <= kIconSide
           && icon.height <= kIconSide && icon.width % kBlockSide == 0
           && icon.height % kBlockSide == 0 && icon.format == dds::Format::Bc3
           && icon.data.size() >= static_cast<std::size_t>(icon.width / kBlockSide)
                                      * static_cast<std::size_t>(icon.height / kBlockSide)
                                      * kBlockBytes;
}

} // namespace

IconUv iconUvSized(std::size_t slot, int widthTexels, int heightTexels) noexcept {
    const auto columns = static_cast<std::size_t>(kAtlasColumns);
    if (slot >= columns * columns || widthTexels <= 0 || heightTexels <= 0) {
        return IconUv{};
    }
    const auto column = static_cast<float>(slot % columns);
    const auto row = static_cast<float>(slot / columns);
    const float step = 1.0f / static_cast<float>(kAtlasColumns);
    // From the cell's origin, only as far as the icon reaches — clamped to the cell so an
    // oversize claim reads the whole cell rather than a neighbour's corner.
    const float acrossU = std::min(1.0f, static_cast<float>(widthTexels)
                                             / static_cast<float>(kIconSide));
    const float acrossV = std::min(1.0f, static_cast<float>(heightTexels)
                                             / static_cast<float>(kIconSide));
    return IconUv{.u0 = column * step,
                  .v0 = row * step,
                  .u1 = (column + acrossU) * step,
                  .v1 = (row + acrossV) * step};
}

IconUv iconUv(std::size_t slot) noexcept {
    const auto columns = static_cast<std::size_t>(kAtlasColumns);
    if (slot >= columns * columns) {
        return IconUv{};  // nothing, rather than somebody else's icon
    }
    const auto column = static_cast<float>(slot % columns);
    const auto row = static_cast<float>(slot / columns);
    const float step = 1.0f / static_cast<float>(kAtlasColumns);
    return IconUv{.u0 = column * step,
                  .v0 = row * step,
                  .u1 = (column + 1.0f) * step,
                  .v1 = (row + 1.0f) * step};
}

dds::Texture packIcons(std::span<const dds::Texture> icons) {
    const auto capacity = static_cast<std::size_t>(kAtlasColumns) * kAtlasColumns;
    if (icons.empty()) {
        return dds::Texture{};
    }

    dds::Texture atlas;
    atlas.width = kAtlasSide;
    atlas.height = kAtlasSide;
    atlas.mipLevels = 1;
    atlas.format = dds::Format::Bc3;
    // Zeroed, so an unpacked slot is transparent black rather than whatever the allocator had.
    // A DXT5 block of all zeroes decodes to fully transparent, which is what "no icon" should
    // look like — the cell's reserved square shows through instead.
    atlas.data.assign(static_cast<std::size_t>(kAtlasBlocks) * kAtlasBlocks * kBlockBytes,
                      std::byte{});

    bool packedAny = false;
    const std::size_t count = std::min(icons.size(), capacity);
    for (std::size_t slot = 0; slot < count; ++slot) {
        const dds::Texture& icon = icons[slot];
        if (!packable(icon)) {
            continue;  // its slot stays blank — see the header on why not scaled
        }

        const auto column = static_cast<int>(slot % static_cast<std::size_t>(kAtlasColumns));
        const auto row = static_cast<int>(slot / static_cast<std::size_t>(kAtlasColumns));

        // ROW BY ROW OF BLOCKS, the icon's OWN block count per row — a full-size unit icon
        // fills its cell, a strategic glyph fills the cell's top-left corner and leaves the
        // rest transparent. An icon is contiguous in its own buffer and a horizontal strip
        // in the atlas's, so this is the one place the two layouts differ.
        const int iconBlocksX = icon.width / kBlockSide;
        const int iconBlocksY = icon.height / kBlockSide;
        for (int blockRow = 0; blockRow < iconBlocksY; ++blockRow) {
            const std::size_t from =
                static_cast<std::size_t>(blockRow) * static_cast<std::size_t>(iconBlocksX)
                * kBlockBytes;
            const std::size_t to =
                (static_cast<std::size_t>(row * kIconBlocks + blockRow) * kAtlasBlocks
                 + static_cast<std::size_t>(column * kIconBlocks))
                * kBlockBytes;
            std::memcpy(atlas.data.data() + to, icon.data.data() + from,
                        static_cast<std::size_t>(iconBlocksX) * kBlockBytes);
        }
        packedAny = true;
    }

    return packedAny ? atlas : dds::Texture{};
}

} // namespace rm::ui
