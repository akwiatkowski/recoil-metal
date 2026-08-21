#pragma once

#include "core/texture/Dds.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm::ui {

// The build tray's unit icons, packed into one texture.
//
// WHY AN ATLAS AND NOT ONE TEXTURE PER CELL. A build menu shows a dozen or more options and the
// encoder binds one fragment texture per draw, so a texture apiece is a dozen draws and a dozen
// binds for a panel that is otherwise a handful of rectangles. Packed, it is one bind and one
// draw — the same shape the two font atlases already have, for the same reason.
//
// **THE BLOCKS ARE COPIED, NOT DECODED.** Supreme Commander's unit icons are 64x64 DXT5 (538 of
// them in `textures.scd` under `textures/ui/common/icons/units/<ID>_icon.dds`, measured), and 64
// is a multiple of the 4x4 block a DXT5 image is made of. So an icon placed at a multiple of 64
// lands on a block boundary, and packing is a memcpy of 16 rows of 16 blocks rather than a
// decompress, a blit and a recompress. The atlas comes out DXT5 too, which is what the GPU
// wanted in the first place.
//
// That is the whole trick, and it is worth stating because it is what makes this cheap enough to
// rebuild whenever the selection changes rather than something that needs a cache with an
// invalidation rule.

/// One icon's side, in texels. What the corpus ships; asserted rather than assumed.
inline constexpr int kIconSide = 64;

/// A DXT5 block covers 4x4 texels and costs 16 bytes.
inline constexpr int kBlockSide = 4;
inline constexpr std::size_t kBlockBytes = 16;

/// How many icons fit across the atlas. 8 across and 8 down holds 64, which is more than any
/// builder's menu — the commander's is fifteen — and keeps the texture at 512x512.
inline constexpr int kAtlasColumns = 8;

/// Where one icon sits in the atlas, as uv corners.
struct IconUv {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
};

/// The uv rectangle for a slot. Slots run left to right, then down.
///
/// Out-of-range slots return a ZERO rectangle rather than wrapping, so a caller that lost track
/// of how many it packed draws nothing instead of somebody else's icon — which is the failure
/// that looks like a content bug and is not one.
[[nodiscard]] IconUv iconUv(std::size_t slot) noexcept;

/// Packs 64x64 DXT5 icons into one atlas, in the order given.
///
/// Every input must be 64x64 and `Format::Bc3`; anything else is SKIPPED and leaves its slot
/// blank rather than being scaled or reinterpreted. A mod shipping a 128x128 icon is a real
/// possibility and silently stretching it into a quarter of the space it wants would be worse
/// than an empty square — the square at least reads as "no icon", which is exactly true.
///
/// Returns an empty texture when nothing could be packed.
[[nodiscard]] dds::Texture packIcons(std::span<const dds::Texture> icons);

} // namespace rm::ui
