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

/// How many icons fit across the atlas. 12 across and 12 down holds 144 — the tray's fifteen,
/// the roster's twelve, and the ~100 strategic icon glyphs the corpus declares, together with
/// room to grow — at a 768x768 texture, which is still one modest bind.
inline constexpr int kAtlasColumns = 12;
inline constexpr std::size_t kAtlasCapacity =
    static_cast<std::size_t>(kAtlasColumns) * static_cast<std::size_t>(kAtlasColumns);

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

/// The uv rectangle for the top-left `widthTexels` x `heightTexels` of a slot — for an icon
/// SMALLER than its cell, which is where the strategic glyphs live (16x16-ish in a 64 cell,
/// placed at the cell's origin by `packIcons`). The caller knows the size because it held the
/// texture it packed; asking for more than a cell answers the whole cell rather than bleeding
/// into a neighbour.
[[nodiscard]] IconUv iconUvSized(std::size_t slot, int widthTexels, int heightTexels) noexcept;

/// Packs DXT5 icons into one atlas, in the order given — one 64x64 cell each.
///
/// An input up to 64x64 with block-aligned sides (multiples of 4, which DXT compression
/// guarantees of anything it encodes) is copied into its cell's TOP-LEFT corner; the unit
/// icons fill their cells exactly, the strategic glyphs occupy a corner and `iconUvSized`
/// addresses them. Anything larger, or not BC3, is SKIPPED and leaves its slot blank rather
/// than being scaled or reinterpreted. A mod shipping a 128x128 icon is a real possibility
/// and silently stretching it would be worse than an empty square — the square at least reads
/// as "no icon", which is exactly true.
///
/// Returns an empty texture when nothing could be packed.
[[nodiscard]] dds::Texture packIcons(std::span<const dds::Texture> icons);

} // namespace rm::ui
