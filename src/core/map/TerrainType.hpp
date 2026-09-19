#pragma once

#include <array>
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

/// Which terrain-type codes block ground movement — retail's `STIMap+0x1434`
/// 256-byte LUT (C-288).
///
/// The mechanism, not a convenience: retail's `STIMap::LoadTerrainTypes`
/// (`0x0057ea30`) runs `/lua/TerrainTypes.lua` and writes each entry's
/// `Blocking` flag into `+0x1434[TypeCode]` (`0x57ed5d`), and
/// `STIMap::IsBlockingTerrain` (`0x0057e9f0`) is the ONLY thing that reads the
/// type grid for pathing — `COGrid::CheckFootprintAt` (`0x727460`) calls it for
/// every `CAiNavigator*` and every placement query. `Slippery`, `Bumpiness`
/// and `HealthEffectPerSecond` never reach the image (the health applier
/// `0x006b04d0` is unreferenced dead code), so there is nothing else to model.
///
/// The shipped table (`build/re-fa/corpus/lua/lua/TerrainTypes.lua`) marks
/// exactly two of its 60 codes `Blocking = true`: Dirt09 (TypeCode 9, line
/// 703) and Lava01 (TypeCode 230, line 2141). Kept as the same 256-entry LUT
/// retail builds, so the walk loop reads `kTerrainTypeBlocking[type]` exactly
/// the way `IsBlockingTerrain` reads `+0x1434[type]`.
inline constexpr std::array<bool, 256> kTerrainTypeBlocking = [] {
    std::array<bool, 256> blocking{};
    blocking[9] = true;    // Dirt09  — TerrainTypes.lua:703
    blocking[230] = true;  // Lava01  — TerrainTypes.lua:2141
    return blocking;
}();

} // namespace rm
