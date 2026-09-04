#include "core/scene/BuildEffects.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE(
    "UEF build beams sweep between the two cube corners nearest the builder") {
    const std::array<float, 3> site{100.0f, 10.0f, 200.0f};
    const std::array<float, 3> builder{80.0f, 10.0f, 180.0f};

    const rm::BuildBeamEnds start =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.0f);
    CHECK(start.first[0] == Approx(90.0f));
    CHECK(start.first[1] == Approx(40.0f));
    CHECK(start.first[2] == Approx(194.0f));
    CHECK(start.second[0] == Approx(90.0f));
    CHECK(start.second[2] == Approx(206.0f));

    const rm::BuildBeamEnds halfway =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.3f);
    CHECK(halfway.first[2] == Approx(200.0f));
    CHECK(halfway.second[2] == Approx(200.0f));

    const rm::BuildBeamEnds swapped =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.6f);
    CHECK(swapped.first == start.second);
    CHECK(swapped.second == start.first);

    const rm::BuildBeamEnds returned =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 1.2f);
    CHECK(returned.first == start.first);
    CHECK(returned.second == start.second);
}

TEST_CASE("UEF build beam height descends with construction progress") {
    const std::array<float, 3> site{0.0f, 5.0f, 0.0f};
    const std::array<float, 3> builder{-20.0f, 5.0f, 0.0f};

    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 0.0f, 0.0f)
              .first[1] == Approx(45.0f));
    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 0.5f, 0.0f)
              .first[1] == Approx(25.0f));
    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 1.0f, 0.0f)
              .first[1] == Approx(5.0f));
}
