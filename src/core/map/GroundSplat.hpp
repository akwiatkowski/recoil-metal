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

    [[nodiscard]] bool present() const noexcept { return texture.width > 0; }
};

/// Layers a splat blends: the base plus eight strata, in mask order.
inline constexpr std::size_t kSplatLayers = 9;

} // namespace rm
