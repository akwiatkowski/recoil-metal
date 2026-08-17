#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace rm {

// An uncompressed RGBA8 image, rows top-down — the layout Metal wants and the
// one every DDS in both content families already uses.
struct ColourImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  ///< width * height * 4, tightly packed

    [[nodiscard]] bool empty() const noexcept { return rgba.empty(); }
};

// Colours a Supreme Commander map by its terrain-type array.
//
// Supreme Commander bakes no ground texture: where SMF ships a finished BC1
// atlas in its .smt, a .scmap names nine stratum textures that live in the
// game's archives and blends them through two weight masks at runtime. Until
// that splat shader exists, this is the stand-in — and a good one, because the
// terrain-type array is full map resolution, needs no external asset, no ZIP
// reader and no DDS decode, and is banded in a way that correlates with the
// strata a map uses.
//
// What the values MEAN is undocumented here — the engine reads them for
// movement and effects, not for looks. So the palette is assigned by rank: the
// lowest value present takes the first colour, the next the second, and so on.
// That makes the output depend only on which values a map uses, not on the
// arbitrary numbers themselves, and keeps two maps with different type numbering
// looking equally sensible.
//
// Returns an empty image when the array does not match the stated dimensions,
// rather than guessing at a stride.
[[nodiscard]] ColourImage colourTerrainTypes(std::span<const std::uint8_t> types, int width,
                                             int height);

} // namespace rm
