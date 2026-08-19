// Which scenery is worth drawing from where the camera is. Nothing on screen can
// check this: culling too much looks like content that failed to load, culling too
// little looks like nothing at all and merely costs 6 ms.
#include "core/scene/PropBatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

namespace {

[[nodiscard]] rm::UnitInstance at(float x, float z) {
    return rm::UnitInstance{.position = {x, 0.0f, z}, .rotationY = 0.0f, .scale = 1.0f};
}

/// Props in a line along +X, one every 100 elmos.
[[nodiscard]] std::vector<rm::UnitInstance> lineOfProps(std::size_t count) {
    std::vector<rm::UnitInstance> props;
    for (std::size_t i = 0; i < count; ++i) {
        props.push_back(at(static_cast<float>(i) * 100.0f, 0.0f));
    }
    return props;
}

constexpr std::array<float, 3> kOrigin{{0.0f, 0.0f, 0.0f}};

} // namespace

TEST_CASE("props past their cutoff are left out") {
    const std::vector<rm::UnitInstance> props = lineOfProps(10);
    std::vector<rm::UnitInstance> out(props.size());

    // A cutoff of 450 keeps the props at 0, 100, 200, 300 and 400.
    const std::size_t kept = rm::cullPropsByDistance(props, kOrigin, 450.0f, out);
    REQUIRE(kept == 5);
    CHECK(out[4].position[0] == 400.0f);
}

TEST_CASE("survivors keep their order, so a capture does not depend on the partition") {
    std::vector<rm::UnitInstance> props;
    // Interleaved near and far, so any reordering shows.
    for (int i = 0; i < 6; ++i) {
        props.push_back(at(static_cast<float>(i) * 10.0f, 0.0f));      // near
        props.push_back(at(static_cast<float>(i) * 10.0f, 5000.0f));   // far
    }

    std::vector<rm::UnitInstance> out(props.size());
    const std::size_t kept = rm::cullPropsByDistance(props, kOrigin, 500.0f, out);

    REQUIRE(kept == 6);
    for (std::size_t i = 0; i < kept; ++i) {
        CHECK(out[i].position[0] == static_cast<float>(i) * 10.0f);
        CHECK(out[i].position[2] == 0.0f);
    }
}

TEST_CASE("a whole-map framing keeps nothing, which is the point") {
    // The case this exists for. A 1024-square map is 8192 elmos across and the
    // camera has to stand off about its own width to frame it, so every prop is
    // thousands of elmos away — and the furthest cutoff any shipped blueprint
    // states is 1000 ogrids, or 8000 elmos.
    std::vector<rm::UnitInstance> props;
    for (int x = 0; x < 8192; x += 512) {
        for (int z = 0; z < 8192; z += 512) {
            props.push_back(at(static_cast<float>(x), static_cast<float>(z)));
        }
    }
    std::vector<rm::UnitInstance> out(props.size());

    // A pine's cutoff: 750 ogrids.
    const std::array<float, 3> eye{{4096.0f, 7000.0f, 12000.0f}};
    const std::size_t kept = rm::cullPropsByDistance(props, eye, 750.0f * 8.0f, out);
    CHECK(kept == 0);
}

TEST_CASE("a working camera keeps the props around it") {
    // ...and the other half of the point: at a zoom someone plays at, the scenery
    // near them is all still there.
    std::vector<rm::UnitInstance> props;
    for (int x = 0; x < 2000; x += 100) {
        props.push_back(at(static_cast<float>(x), 0.0f));
    }
    std::vector<rm::UnitInstance> out(props.size());

    const std::array<float, 3> eye{{0.0f, 300.0f, 300.0f}};
    const std::size_t kept = rm::cullPropsByDistance(props, eye, 750.0f * 8.0f, out);
    CHECK(kept == props.size());
}

TEST_CASE("distance is measured in three dimensions, not on the ground") {
    // A camera 5000 elmos up looking straight down is 5000 elmos from the prop
    // under it, however close it is in x and z. Ignoring height would keep every
    // prop on the map at a top-down framing, which is exactly the framing this is
    // supposed to empty.
    const std::vector<rm::UnitInstance> props{at(0.0f, 0.0f)};
    std::vector<rm::UnitInstance> out(1);

    const std::array<float, 3> high{{0.0f, 5000.0f, 0.0f}};
    CHECK(rm::cullPropsByDistance(props, high, 1000.0f, out) == 0);
    CHECK(rm::cullPropsByDistance(props, high, 6000.0f, out) == 1);
}

TEST_CASE("an unlimited cutoff keeps everything, wherever the camera is") {
    // What a blueprint stating no cutoff means. Only the four DevTest blueprints
    // do, and reading their -1 as a distance would make them permanently invisible
    // rather than permanently visible.
    const std::vector<rm::UnitInstance> props = lineOfProps(10);
    std::vector<rm::UnitInstance> out(props.size());

    const std::array<float, 3> faraway{{0.0f, 100000.0f, 0.0f}};
    CHECK(rm::cullPropsByDistance(props, faraway, std::numeric_limits<float>::infinity(), out)
          == props.size());
}

TEST_CASE("a cutoff of zero or less keeps nothing, and an undersized output keeps nothing") {
    const std::vector<rm::UnitInstance> props = lineOfProps(4);
    std::vector<rm::UnitInstance> out(props.size());

    CHECK(rm::cullPropsByDistance(props, kOrigin, 0.0f, out) == 0);
    CHECK(rm::cullPropsByDistance(props, kOrigin, -100.0f, out) == 0);

    // Refused rather than overrunning: the caller sizes the buffer, and getting it
    // wrong here would be a write past the end of somebody's vector.
    std::vector<rm::UnitInstance> tooSmall(props.size() - 1);
    CHECK(rm::cullPropsByDistance(props, kOrigin, 1.0e9f, tooSmall) == 0);
}
