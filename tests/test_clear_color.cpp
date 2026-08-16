// First test file in the project — deliberately small, but it establishes
// the TDD harness that every parser and pure-math module will grow into.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/ClearColor.hpp"

using Catch::Approx;
using rm::Rgba;

TEST_CASE("hsvToRgb reproduces the primaries exactly at sector boundaries") {
    const Rgba red = rm::hsvToRgb(0.0f, 1.0f, 1.0f);
    REQUIRE(red.r == Approx(1.0f));
    REQUIRE(red.g == Approx(0.0f));
    REQUIRE(red.b == Approx(0.0f));

    const Rgba green = rm::hsvToRgb(1.0f / 3.0f, 1.0f, 1.0f);
    REQUIRE(green.r == Approx(0.0f).margin(1e-6f));
    REQUIRE(green.g == Approx(1.0f));
    REQUIRE(green.b == Approx(0.0f).margin(1e-6f));

    const Rgba blue = rm::hsvToRgb(2.0f / 3.0f, 1.0f, 1.0f);
    REQUIRE(blue.r == Approx(0.0f).margin(1e-6f));
    REQUIRE(blue.g == Approx(0.0f).margin(1e-6f));
    REQUIRE(blue.b == Approx(1.0f));
}

TEST_CASE("hsvToRgb with zero saturation is grey of value v") {
    const Rgba grey = rm::hsvToRgb(0.7f, 0.0f, 0.4f);
    REQUIRE(grey.r == Approx(0.4f));
    REQUIRE(grey.g == Approx(0.4f));
    REQUIRE(grey.b == Approx(0.4f));
}

TEST_CASE("hsvToRgb wraps hue, including negative drift") {
    const Rgba a = rm::hsvToRgb(0.25f, 1.0f, 1.0f);
    const Rgba b = rm::hsvToRgb(1.25f, 1.0f, 1.0f);
    const Rgba c = rm::hsvToRgb(-0.75f, 1.0f, 1.0f);
    REQUIRE(a.r == Approx(b.r));
    REQUIRE(a.r == Approx(c.r));
    REQUIRE(a.g == Approx(b.g));
    REQUIRE(a.b == Approx(c.b));
}

TEST_CASE("animatedClearColor is periodic with the 12 s cycle") {
    const Rgba a = rm::animatedClearColor(1.5);
    const Rgba b = rm::animatedClearColor(13.5);
    REQUIRE(a.r == Approx(b.r).margin(1e-5));
    REQUIRE(a.g == Approx(b.g).margin(1e-5));
    REQUIRE(a.b == Approx(b.b).margin(1e-5));
}

TEST_CASE("animatedClearColor always returns full alpha") {
    REQUIRE(rm::animatedClearColor(0.0).a == Approx(1.0f));
    REQUIRE(rm::animatedClearColor(123.456).a == Approx(1.0f));
}
