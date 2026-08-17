// Unit placement tests. Pure geometry, so all of it is checkable without a GPU.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/ProceduralField.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using Catch::Approx;

namespace {

/// A field spanning -100..300 elmos, so roughly a quarter of it is below the
/// water plane at y = 0 and the land filter has something to reject.
[[nodiscard]] rm::HeightField mixedField() {
    return rm::makeSineHills(128, 128, -100.0f, 300.0f);
}

/// A field entirely below the water plane.
[[nodiscard]] rm::HeightField drownedField() {
    return rm::makeSineHills(128, 128, -500.0f, -100.0f);
}

} // namespace

TEST_CASE("scattered instances sit on the terrain") {
    const auto field = mixedField();
    const auto instances = rm::scatterOnLand(field, 200, 1234);

    REQUIRE_FALSE(instances.empty());

    for (const rm::UnitInstance& instance : instances) {
        // Inside the map.
        REQUIRE(instance.position[0] >= 0.0f);
        REQUIRE(instance.position[0] <= field.widthElmos());
        REQUIRE(instance.position[2] >= 0.0f);
        REQUIRE(instance.position[2] <= field.depthElmos());

        // On the ground, not floating or buried: the Y must be a height the
        // field actually reports at that spot.
        const int gx = static_cast<int>(std::lround(instance.position[0] / rm::kSquareSize));
        const int gz = static_cast<int>(std::lround(instance.position[2] / rm::kSquareSize));
        REQUIRE(instance.position[1] == Approx(field.heightAt(gx, gz)));
    }
}

TEST_CASE("scattering keeps instances above the water plane") {
    // Recoil fixes water at y = 0 (rts/Map/Ground.h:32). Without this filter most
    // of a map like Angel Crossing spawns units underwater.
    const auto field = mixedField();
    const auto instances = rm::scatterOnLand(field, 300, 99);

    REQUIRE_FALSE(instances.empty());
    for (const rm::UnitInstance& instance : instances) {
        REQUIRE(instance.position[1] >= 0.0f);
    }
}

TEST_CASE("a custom minimum height is honoured") {
    const auto field = mixedField();
    const auto instances = rm::scatterOnLand(field, 200, 7, 1.0f, 150.0f);

    for (const rm::UnitInstance& instance : instances) {
        REQUIRE(instance.position[1] >= 150.0f);
    }
}

TEST_CASE("scattering is deterministic for a seed") {
    // A scene that changes between runs makes screenshots incomparable and
    // benchmarks meaningless.
    const auto field = mixedField();

    const auto first = rm::scatterOnLand(field, 50, 4242);
    const auto second = rm::scatterOnLand(field, 50, 4242);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i].position[0] == Approx(second[i].position[0]));
        REQUIRE(first[i].position[2] == Approx(second[i].position[2]));
        REQUIRE(first[i].rotationY == Approx(second[i].rotationY));
    }
}

TEST_CASE("different seeds give different layouts") {
    const auto field = mixedField();
    const auto a = rm::scatterOnLand(field, 50, 1);
    const auto b = rm::scatterOnLand(field, 50, 2);

    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    REQUIRE(a[0].position[0] != Approx(b[0].position[0]));
}

TEST_CASE("rotations cover the circle rather than clustering") {
    const auto field = mixedField();
    const auto instances = rm::scatterOnLand(field, 400, 5);

    const auto [lo, hi] = std::minmax_element(
        instances.begin(), instances.end(),
        [](const rm::UnitInstance& a, const rm::UnitInstance& b) {
            return a.rotationY < b.rotationY;
        });

    // Not a distribution test, just a check that yaw is varied at all — a
    // constant would make every instance face the same way, which reads as a bug
    // on screen but would pass every other assertion here.
    REQUIRE(hi->rotationY - lo->rotationY > 3.0f);
}

TEST_CASE("an all-underwater map yields fewer instances instead of looping") {
    // The resample cap is what stops this spinning; returning a short list is the
    // honest outcome.
    const auto field = drownedField();
    const auto instances = rm::scatterOnLand(field, 100, 3);

    REQUIRE(instances.empty());
}

TEST_CASE("degenerate inputs yield nothing rather than misbehaving") {
    const rm::HeightField empty;
    REQUIRE(rm::scatterOnLand(empty, 10, 1).empty());
    REQUIRE(rm::scatterOnLand(mixedField(), 0, 1).empty());
}

TEST_CASE("the requested scale reaches every instance") {
    const auto instances = rm::scatterOnLand(mixedField(), 20, 8, 2.5f);

    REQUIRE_FALSE(instances.empty());
    for (const rm::UnitInstance& instance : instances) {
        REQUIRE(instance.scale == Approx(2.5f));
    }
}

TEST_CASE("start positions become one instance each, dropped onto the ground") {
    const auto field = mixedField();

    const std::vector<rm::mapinfo::StartPosition> positions{
        {0, 100.0f, 200.0f},
        {1, 500.0f, 600.0f},
    };

    const auto instances = rm::atStartPositions(field, positions);

    REQUIRE(instances.size() == 2);
    REQUIRE(instances[0].position[0] == Approx(100.0f));
    REQUIRE(instances[0].position[2] == Approx(200.0f));
    REQUIRE(instances[1].position[0] == Approx(500.0f));

    // Y sampled from the terrain, since mapinfo.lua supplies only X and Z.
    const int gx = static_cast<int>(std::lround(100.0f / rm::kSquareSize));
    const int gz = static_cast<int>(std::lround(200.0f / rm::kSquareSize));
    REQUIRE(instances[0].position[1] == Approx(field.heightAt(gx, gz)));

    // Spawn markers face a consistent direction; there is no facing data to
    // honour and a random one reads as noise.
    REQUIRE(instances[0].rotationY == Approx(0.0f));
}

TEST_CASE("start positions outside the map are clamped by the height sampler") {
    // heightAt clamps, so an out-of-range spawn still gets a sane Y rather than
    // reading out of bounds.
    const auto field = mixedField();
    const std::vector<rm::mapinfo::StartPosition> positions{{0, -500.0f, 999999.0f}};

    const auto instances = rm::atStartPositions(field, positions);

    REQUIRE(instances.size() == 1);
    REQUIRE(std::isfinite(instances[0].position[1]));
}

TEST_CASE("no start positions yields no instances") {
    REQUIRE(rm::atStartPositions(mixedField(), {}).empty());
}
