#include "core/map/TerrainType.hpp"

#include <algorithm>
#include <array>

namespace {

using Rgb = std::array<std::uint8_t, 3>;

// Ground colours, ordered as a terrain ramp rather than as arbitrary hues: a
// map's low ground reads as vegetation, its high ground as rock and snow. The
// ordering is the only thing carrying meaning here — see the header on why the
// terrain-type numbers themselves cannot.
//
// Twelve entries because the corpus survey found 3-12 distinct types per map
// (docs/recoil-metal/scmap-v60-format.md); a map with more wraps, which is a
// repeated colour rather than a wrong one.
constexpr std::array<Rgb, 12> kTerrainRamp{{
    {{58, 84, 44}},     // deep green
    {{84, 104, 52}},    // grass
    {{112, 122, 66}},   // dry grass
    {{134, 128, 88}},   // scrub
    {{156, 140, 104}},  // dirt
    {{176, 158, 122}},  // sand
    {{150, 146, 140}},  // gravel
    {{128, 126, 126}},  // rock
    {{104, 102, 104}},  // dark rock
    {{86, 88, 96}},     // slate
    {{178, 182, 188}},  // scree
    {{224, 228, 232}},  // snow
}};

} // namespace

namespace rm {

ColourImage colourTerrainTypes(std::span<const std::uint8_t> types, int width, int height) {
    ColourImage image;

    if (width <= 0 || height <= 0) {
        return image;
    }

    const auto expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (types.size() != expected) {
        return image;
    }

    // Rank every value that actually occurs. 256 flags rather than a set: the
    // domain is one byte wide, so this is both smaller and a single pass.
    std::array<bool, 256> present{};
    for (const std::uint8_t type : types) {
        present[type] = true;
    }

    std::array<Rgb, 256> colourFor{};
    std::size_t rank = 0;
    for (std::size_t value = 0; value < present.size(); ++value) {
        if (!present[value]) {
            continue;
        }
        colourFor[value] = kTerrainRamp[rank % kTerrainRamp.size()];
        ++rank;
    }

    image.width = width;
    image.height = height;
    image.rgba.resize(expected * 4);

    for (std::size_t i = 0; i < expected; ++i) {
        const Rgb& colour = colourFor[types[i]];
        image.rgba[i * 4 + 0] = colour[0];
        image.rgba[i * 4 + 1] = colour[1];
        image.rgba[i * 4 + 2] = colour[2];
        image.rgba[i * 4 + 3] = 255;  // opaque: this is ground, not an overlay
    }

    return image;
}

} // namespace rm
