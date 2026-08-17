// Terrain-type colouring tests. Pure, so all of it runs without a GPU.
#include "core/map/TerrainType.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

namespace {

/// The RGB triple at a pixel, as a comparable value.
[[nodiscard]] std::array<std::uint8_t, 3> pixel(const rm::ColourImage& image, std::size_t index) {
    return {image.rgba[index * 4], image.rgba[index * 4 + 1], image.rgba[index * 4 + 2]};
}

} // namespace

TEST_CASE("a terrain-type array becomes an opaque RGBA image of the same size") {
    const std::vector<std::uint8_t> types{0, 1, 2, 3, 4, 5};

    const rm::ColourImage image = rm::colourTerrainTypes(types, 3, 2);

    REQUIRE(image.width == 3);
    REQUIRE(image.height == 2);
    REQUIRE(image.rgba.size() == 3 * 2 * 4);

    for (std::size_t i = 0; i < types.size(); ++i) {
        REQUIRE(image.rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("distinct terrain types get distinct colours") {
    const std::vector<std::uint8_t> types{0, 1, 2, 3};

    const rm::ColourImage image = rm::colourTerrainTypes(types, 4, 1);

    std::set<std::array<std::uint8_t, 3>> colours;
    for (std::size_t i = 0; i < types.size(); ++i) {
        colours.insert(pixel(image, i));
    }
    REQUIRE(colours.size() == types.size());
}

TEST_CASE("equal terrain types get equal colours") {
    const std::vector<std::uint8_t> types{7, 3, 7, 3, 7, 3};

    const rm::ColourImage image = rm::colourTerrainTypes(types, 6, 1);

    REQUIRE(pixel(image, 0) == pixel(image, 2));
    REQUIRE(pixel(image, 0) == pixel(image, 4));
    REQUIRE(pixel(image, 1) == pixel(image, 3));
    REQUIRE(pixel(image, 0) != pixel(image, 1));
}

TEST_CASE("colours are assigned by rank, not by the raw value") {
    // Two maps using wildly different type numbers for the same number of
    // bands must look the same. Keying the palette on the value itself would
    // make one map green and the other snow for no reason a player could see.
    const std::vector<std::uint8_t> low{0, 1, 2, 2};
    const std::vector<std::uint8_t> high{50, 130, 240, 240};

    const rm::ColourImage first = rm::colourTerrainTypes(low, 4, 1);
    const rm::ColourImage second = rm::colourTerrainTypes(high, 4, 1);

    REQUIRE(first.rgba == second.rgba);
}

TEST_CASE("a single-type map is a single flat colour") {
    const std::vector<std::uint8_t> types(64, std::uint8_t{9});

    const rm::ColourImage image = rm::colourTerrainTypes(types, 8, 8);

    for (std::size_t i = 1; i < types.size(); ++i) {
        REQUIRE(pixel(image, i) == pixel(image, 0));
    }
}

TEST_CASE("more types than the ramp holds wraps rather than running off the end") {
    // 20 distinct values against a 12-entry ramp. A repeated colour is a
    // cosmetic loss; reading past the array would not be.
    std::vector<std::uint8_t> types(20);
    for (std::size_t i = 0; i < types.size(); ++i) {
        types[i] = static_cast<std::uint8_t>(i);
    }

    const rm::ColourImage image = rm::colourTerrainTypes(types, 20, 1);

    REQUIRE(image.rgba.size() == 20 * 4);
    REQUIRE(pixel(image, 0) == pixel(image, 12));
}

TEST_CASE("a mismatched array yields nothing rather than a guessed stride") {
    const std::vector<std::uint8_t> types(10);

    REQUIRE(rm::colourTerrainTypes(types, 4, 4).empty());
    REQUIRE(rm::colourTerrainTypes(types, 0, 0).empty());
    REQUIRE(rm::colourTerrainTypes({}, 4, 4).empty());
}
