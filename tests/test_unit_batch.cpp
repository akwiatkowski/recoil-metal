// Draw-order tests. Pure index shuffling, so all of it is checkable without a
// GPU — which matters, because a renderer that ignores the order draws exactly
// the same image and nothing on screen would ever show the bug.
#include "core/scene/UnitBatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <vector>

namespace {

/// Distinct pairs in a list — the floor on how many binds any order can need.
[[nodiscard]] std::size_t distinctPairs(const std::vector<rm::TexturePair>& batches) {
    std::set<std::pair<int, int>> seen;
    for (const rm::TexturePair& pair : batches) {
        seen.insert({pair.diffuse, pair.shading});
    }
    return seen.size();
}

} // namespace

TEST_CASE("an empty scene has an empty order") {
    REQUIRE(rm::orderByTexturePair({}).empty());
    REQUIRE(rm::textureBindCount({}, {}) == 0);
}

TEST_CASE("every batch is drawn exactly once") {
    const std::vector<rm::TexturePair> batches{
        {2, 3}, {0, 1}, {2, 3}, {5, -1}, {0, 1}, {-1, -1},
    };

    const auto order = rm::orderByTexturePair(batches);

    REQUIRE(order.size() == batches.size());

    // A sort that dropped or duplicated a batch would lose or double-draw a
    // model — both of which look plausible in a busy scene.
    std::vector<std::size_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        REQUIRE(sorted[i] == i);
    }
}

TEST_CASE("identical texture pairs end up adjacent") {
    const std::vector<rm::TexturePair> batches{
        {2, 3}, {0, 1}, {2, 3}, {5, -1}, {0, 1}, {2, 3},
    };

    const auto order = rm::orderByTexturePair(batches);

    // The point of the whole exercise: one bind per distinct pair, not one per
    // batch. Six batches, three distinct pairs.
    REQUIRE(distinctPairs(batches) == 3);
    REQUIRE(rm::textureBindCount(batches, order) == 3);
}

TEST_CASE("the diffuse and the shading texture both count") {
    // Same diffuse, different shading — still two pairs, because the shader
    // samples both and swapping one changes the image.
    const std::vector<rm::TexturePair> batches{{7, 1}, {7, 2}, {7, 1}};

    const auto order = rm::orderByTexturePair(batches);

    REQUIRE(rm::textureBindCount(batches, order) == 2);
}

TEST_CASE("batches sharing a pair keep the order they were given") {
    // Stability is what keeps a screenshot reproducible: two instances of one
    // model drawn in either order are identical on screen, but any dependence
    // on the sort's tie-breaking makes the scene an implementation detail.
    const std::vector<rm::TexturePair> batches{{1, 1}, {0, 0}, {1, 1}, {0, 0}};

    const auto order = rm::orderByTexturePair(batches);

    // The {0,0} group sorts first and must hold batches 1 then 3; the {1,1}
    // group follows with 0 then 2.
    REQUIRE(order == std::vector<std::size_t>{1, 3, 0, 2});
}

TEST_CASE("models naming no texture at all group together") {
    // -1 is a real state — a model may name no texture — and those batches
    // should share one bind of the fallback rather than one each.
    const std::vector<rm::TexturePair> batches{{-1, -1}, {4, 4}, {-1, -1}, {-1, -1}};

    const auto order = rm::orderByTexturePair(batches);

    REQUIRE(rm::textureBindCount(batches, order) == 2);
}

TEST_CASE("an already-grouped scene is left alone") {
    const std::vector<rm::TexturePair> batches{{0, 0}, {0, 0}, {1, 1}, {1, 1}};

    const auto order = rm::orderByTexturePair(batches);

    REQUIRE(order == std::vector<std::size_t>{0, 1, 2, 3});
    REQUIRE(rm::textureBindCount(batches, order) == 2);
}
