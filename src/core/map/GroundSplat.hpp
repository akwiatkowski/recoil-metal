#pragma once

#include "core/texture/Dds.hpp"

#include <cstddef>

namespace rm {

// One layer of a Supreme Commander ground splat.
//
// This lives in core rather than beside the renderer because it is data, not a
// rendering concept — and because Window.hpp forwards the call but deliberately
// includes no Metal, so a type nested inside Renderer would be unnameable there.
//
// `tileElmos` is how much ground one repeat of the texture covers, which is the
// map-size-independent quantity. The stock maps set the macrotexture to the same
// value on 256- and 2048-square maps alike, which only makes sense as a physical
// size rather than as a count of repeats across the map — that is the evidence
// the unit rests on, since no reference states it.
struct SplatLayer {
    dds::Texture texture;     ///< a zero-width texture means the slot is unused
    float tileElmos = 1.0f;

    // The stratum's normal map, and how much ground one repeat of it covers.
    //
    // A separate texture and a separate tile size, because a `.scmap` gives
    // each its own entry and its own scale — the two are not obliged to repeat
    // together, and several stock maps set them differently.
    //
    // Optional independently of the albedo: a stratum may name one and not the
    // other, and the macrotexture (slot 9) has no normal entry at all.
    dds::Texture normal;
    float normalTileElmos = 1.0f;

    [[nodiscard]] bool present() const noexcept { return texture.width > 0; }
    [[nodiscard]] bool hasNormal() const noexcept { return normal.width > 0; }
};

/// Layers a splat blends: the base, eight strata in mask order, then the
/// macrotexture that goes over everything.
inline constexpr std::size_t kSplatLayers = 10;

/// Layers carrying a normal map: the base and the eight strata. The
/// macrotexture has none — `.scmap` stores nine normal entries against ten
/// albedo ones, and terrain.fx's TerrainNormalsXP blends exactly those nine.
inline constexpr std::size_t kSplatNormalLayers = 9;

} // namespace rm
