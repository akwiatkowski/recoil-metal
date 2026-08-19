// Which scenery is worth drawing from where the camera is, and at which detail
// level. Nothing on screen can check this: culling too much looks like content that
// failed to load, culling too little looks like nothing at all and merely costs
// milliseconds, and drawing the wrong level looks like a slightly worse mesh.
#include "core/scene/PropBatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <numeric>
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

/// A tree's real cutoffs, in elmos: the corpus states 30 / 175 / 300 ogrids for the
/// pine placed more often than anything else on any map.
const std::vector<float> kTreeCutoffs{30.0f * 8.0f, 175.0f * 8.0f, 300.0f * 8.0f};

[[nodiscard]] std::size_t total(const std::vector<std::size_t>& counts) {
    return std::accumulate(counts.begin(), counts.end(), std::size_t{0});
}

} // namespace

TEST_CASE("each prop lands in the level whose cutoff it is inside") {
    const std::vector<rm::UnitInstance> props{
        at(100.0f, 0.0f),   // 100 elmos: inside 240, the finest
        at(500.0f, 0.0f),   // inside 1400, the middle
        at(2000.0f, 0.0f),  // inside 2400, the coarsest
        at(5000.0f, 0.0f),  // past every cutoff: not drawn
    };
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(kTreeCutoffs.size());

    rm::cullPropsByLevel(props, kOrigin, kTreeCutoffs, out, counts);

    CHECK(counts[0] == 1);
    CHECK(counts[1] == 1);
    CHECK(counts[2] == 1);
    CHECK(total(counts) == 3);  // the fourth is gone

    // Runs are contiguous and finest first, so a level's run starts at the sum of
    // the counts before it.
    CHECK(out[0].position[0] == 100.0f);
    CHECK(out[1].position[0] == 500.0f);
    CHECK(out[2].position[0] == 2000.0f);
}

TEST_CASE("the runs are contiguous, in level order, whatever order the props arrive in") {
    // The property the renderer depends on: one instance buffer holds every level's
    // survivors back to back, and each level's draw is an offset and a count into
    // it. If the runs interleaved, a draw would render another level's props with
    // this level's mesh.
    std::vector<rm::UnitInstance> props;
    for (int i = 0; i < 5; ++i) {
        props.push_back(at(2000.0f, 0.0f));  // coarsest
        props.push_back(at(100.0f, 0.0f));   // finest
        props.push_back(at(500.0f, 0.0f));   // middle
    }
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(kTreeCutoffs.size());

    rm::cullPropsByLevel(props, kOrigin, kTreeCutoffs, out, counts);

    REQUIRE(counts[0] == 5);
    REQUIRE(counts[1] == 5);
    REQUIRE(counts[2] == 5);

    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(out[i].position[0] == 100.0f);
        CHECK(out[5 + i].position[0] == 500.0f);
        CHECK(out[10 + i].position[0] == 2000.0f);
    }
}

TEST_CASE("survivors keep their order within a level, so a capture is reproducible") {
    std::vector<rm::UnitInstance> props;
    for (int i = 0; i < 6; ++i) {
        props.push_back(at(static_cast<float>(i) * 10.0f, 0.0f));
    }

    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(1);
    const std::vector<float> single{1000.0f};

    rm::cullPropsByLevel(props, kOrigin, single, out, counts);

    REQUIRE(counts[0] == props.size());
    for (std::size_t i = 0; i < props.size(); ++i) {
        CHECK(out[i].position[0] == static_cast<float>(i) * 10.0f);
    }
}

TEST_CASE("a whole-map framing draws nothing at any level, which is the point") {
    // A 1024-square map is 8192 elmos across and the camera has to stand off about
    // its own width to frame it, so every prop is thousands of elmos away — and the
    // furthest cutoff any shipped blueprint states is 1000 ogrids, or 8000 elmos.
    std::vector<rm::UnitInstance> props;
    for (int x = 0; x < 8192; x += 512) {
        for (int z = 0; z < 8192; z += 512) {
            props.push_back(at(static_cast<float>(x), static_cast<float>(z)));
        }
    }
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(kTreeCutoffs.size());

    const std::array<float, 3> eye{{4096.0f, 7000.0f, 12000.0f}};
    rm::cullPropsByLevel(props, eye, kTreeCutoffs, out, counts);
    CHECK(total(counts) == 0);
}

TEST_CASE("a working camera keeps what is near it, at the finest level") {
    // The other half of the point: at a zoom someone plays at, the scenery around
    // them is all there, and the nearest of it is drawn from every vertex the mesh
    // has.
    std::vector<rm::UnitInstance> props;
    for (int x = 0; x < 200; x += 20) {
        props.push_back(at(static_cast<float>(x), 0.0f));
    }
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(kTreeCutoffs.size());

    const std::array<float, 3> eye{{0.0f, 80.0f, 80.0f}};
    rm::cullPropsByLevel(props, eye, kTreeCutoffs, out, counts);

    CHECK(total(counts) == props.size());
    CHECK(counts[0] == props.size());  // all inside 240 elmos
}

TEST_CASE("a mid zoom draws almost everything at a coarse level, which is the saving") {
    // Where the win actually comes from. A tree is at its finest only within 240
    // elmos, so at any framing that shows a battle rather than a bumper, the great
    // majority of the visible scenery is coarse geometry.
    std::vector<rm::UnitInstance> props;
    for (int x = 0; x < 2000; x += 25) {
        props.push_back(at(static_cast<float>(x), 0.0f));
    }
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(kTreeCutoffs.size());

    const std::array<float, 3> eye{{1000.0f, 600.0f, 600.0f}};
    rm::cullPropsByLevel(props, eye, kTreeCutoffs, out, counts);

    CHECK(total(counts) == props.size());  // all within 2400 elmos
    CHECK(counts[0] < total(counts) / 4);  // few are close enough to be finest
}

TEST_CASE("distance is measured in three dimensions, not on the ground") {
    // A camera 5000 elmos up looking straight down is 5000 elmos from the prop
    // under it, however close it is in x and z. Ignoring height would keep every
    // prop on the map at a top-down framing, which is exactly the framing this is
    // supposed to empty.
    const std::vector<rm::UnitInstance> props{at(0.0f, 0.0f)};
    std::vector<rm::UnitInstance> out(1);
    std::vector<std::size_t> counts(1);

    const std::array<float, 3> high{{0.0f, 5000.0f, 0.0f}};
    const std::vector<float> near{1000.0f};
    const std::vector<float> far{6000.0f};

    rm::cullPropsByLevel(props, high, near, out, counts);
    CHECK(counts[0] == 0);

    rm::cullPropsByLevel(props, high, far, out, counts);
    CHECK(counts[0] == 1);
}

TEST_CASE("an unlimited cutoff keeps everything, wherever the camera is") {
    // What a blueprint stating no cutoff means. Only the four DevTest blueprints
    // do, and reading their -1 as a distance would make them permanently invisible
    // rather than permanently visible.
    const std::vector<rm::UnitInstance> props = lineOfProps(10);
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(1);
    const std::vector<float> unlimited{std::numeric_limits<float>::infinity()};

    const std::array<float, 3> faraway{{0.0f, 100000.0f, 0.0f}};
    rm::cullPropsByLevel(props, faraway, unlimited, out, counts);
    CHECK(counts[0] == props.size());
}

TEST_CASE("a malformed request draws nothing rather than overrunning") {
    const std::vector<rm::UnitInstance> props = lineOfProps(4);
    std::vector<rm::UnitInstance> out(props.size());
    std::vector<std::size_t> counts(1);

    const std::vector<float> oneCutoff{1.0e9f};

    // An output shorter than the input: the caller sizes the buffer, and getting it
    // wrong here would be a write past the end of somebody's vector.
    std::vector<rm::UnitInstance> tooSmall(props.size() - 1);
    rm::cullPropsByLevel(props, kOrigin, oneCutoff, tooSmall, counts);
    CHECK(counts[0] == 0);

    // A count array that does not match the cutoffs.
    std::vector<std::size_t> tooFewCounts(0);
    rm::cullPropsByLevel(props, kOrigin, oneCutoff, out, tooFewCounts);
    CHECK(total(tooFewCounts) == 0);

    // No levels at all.
    const std::vector<float> noCutoffs;
    std::vector<std::size_t> noCounts;
    rm::cullPropsByLevel(props, kOrigin, noCutoffs, out, noCounts);
    SUCCEED("no levels is not a crash");
}
