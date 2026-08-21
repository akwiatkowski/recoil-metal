#include "core/ui/IconAtlas.hpp"

#include <algorithm>
#include <cstring>

namespace rm::ui {
namespace {

/// Blocks across one icon, and across the whole atlas.
constexpr int kIconBlocks = kIconSide / kBlockSide;
constexpr int kAtlasBlocks = kIconBlocks * kAtlasColumns;
constexpr int kAtlasSide = kIconSide * kAtlasColumns;

/// Whether a texture is one this packer can copy blocks out of.
[[nodiscard]] bool packable(const dds::Texture& icon) noexcept {
    return icon.width == kIconSide && icon.height == kIconSide
           && icon.format == dds::Format::Bc3
           && icon.data.size() >= static_cast<std::size_t>(kIconBlocks) * kIconBlocks
                                      * kBlockBytes;
}

} // namespace

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

        // ROW BY ROW OF BLOCKS. An icon is contiguous in its own buffer and a horizontal strip
        // in the atlas's, so this is the one place the two layouts differ and the only reason
        // this is a loop rather than a single copy.
        for (int blockRow = 0; blockRow < kIconBlocks; ++blockRow) {
            const std::size_t from =
                static_cast<std::size_t>(blockRow) * kIconBlocks * kBlockBytes;
            const std::size_t to =
                (static_cast<std::size_t>(row * kIconBlocks + blockRow) * kAtlasBlocks
                 + static_cast<std::size_t>(column * kIconBlocks))
                * kBlockBytes;
            std::memcpy(atlas.data.data() + to, icon.data.data() + from,
                        static_cast<std::size_t>(kIconBlocks) * kBlockBytes);
        }
        packedAny = true;
    }

    return packedAny ? atlas : dds::Texture{};
}

} // namespace rm::ui
