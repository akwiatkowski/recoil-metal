#include "core/mesh/ChunkDraws.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

/// Chunks whose level-`l` ranges are laid out exactly as buildTerrainMesh lays
/// them out: level-major, so all chunks' level 0 come first, then all level 1.
///
/// Synthetic rather than a real mesh because the merge cares about nothing but
/// the numbers — and a hand-built layout makes the ONE property it depends on
/// visible in the test rather than buried in a mesh builder.
[[nodiscard]] std::vector<rm::TerrainChunk> stripOfChunks(std::size_t count,
                                                          std::size_t indicesPerChunk = 100) {
    std::vector<rm::TerrainChunk> chunks(count);
    std::size_t cursor = 0;

    for (std::size_t level = 0; level < rm::kLodLevels; ++level) {
        // Each level is a quarter of the triangles of the one before it.
        const std::size_t size = indicesPerChunk >> (2 * level);
        for (std::size_t c = 0; c < count; ++c) {
            chunks[c].lods[level].firstIndex = cursor;
            chunks[c].lods[level].indexCount = size;
            chunks[c].lods[level].surfaceIndexCount = size;
            cursor += size;
        }
    }

    // Laid out along +X, one chunk width each, so a distance is easy to reason
    // about in the level tests below.
    for (std::size_t c = 0; c < count; ++c) {
        chunks[c].minX = static_cast<float>(c) * 512.0f;
        chunks[c].maxX = chunks[c].minX + 512.0f;
        chunks[c].minZ = 0.0f;
        chunks[c].maxZ = 512.0f;
    }
    return chunks;
}

constexpr auto keepAll = [](const rm::TerrainChunk&) { return true; };
constexpr auto finest = [](const rm::TerrainChunk&) { return 0; };

} // namespace

TEST_CASE("no chunks means no draws") {
    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(draws, {}, keepAll, finest);
    CHECK(draws.empty());
}

TEST_CASE("every chunk visible at one level is a single draw") {
    // The whole reason the plan exists. 64 chunks, one draw.
    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(64);
    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(draws, chunks, keepAll, finest);

    REQUIRE(draws.size() == 1);
    CHECK(draws[0].firstIndex == 0);
    CHECK(draws[0].indexCount == 64 * 100);
}

TEST_CASE("a culled chunk splits the run in two") {
    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(5);
    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(
        draws, chunks,
        [&chunks](const rm::TerrainChunk& chunk) { return &chunk != &chunks[2]; }, finest);

    REQUIRE(draws.size() == 2);
    CHECK(draws[0] == rm::ChunkDraw{0, 200});
    CHECK(draws[1] == rm::ChunkDraw{300, 200});
}

TEST_CASE("a culled chunk at either end does not open an empty draw") {
    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(3);

    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(
        draws, chunks,
        [&chunks](const rm::TerrainChunk& chunk) { return &chunk != &chunks[0]; }, finest);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0] == rm::ChunkDraw{100, 200});

    draws.clear();
    rm::appendChunkDraws(
        draws, chunks,
        [&chunks](const rm::TerrainChunk& chunk) { return &chunk != &chunks[2]; }, finest);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0] == rm::ChunkDraw{0, 200});
}

TEST_CASE("a change of level ends a run, because the levels are far apart in the buffer") {
    // Not a flaw to fix — it is the cost of varying detail, and the reason the
    // thresholds below are chosen to put as few boundaries as possible inside
    // one view.
    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(4);
    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(draws, chunks, keepAll, [&chunks](const rm::TerrainChunk& chunk) {
        return &chunk < &chunks[2] ? 0 : 1;
    });

    REQUIRE(draws.size() == 2);
    CHECK(draws[0] == rm::ChunkDraw{0, 200});
    // Level 1 starts after all four level-0 ranges, and chunks 2 and 3 merge.
    CHECK(draws[1] == rm::ChunkDraw{400 + 2 * 25, 50});
}

TEST_CASE("the chunks a run merges are the chunks it draws") {
    // Merging must not quietly draw a range nobody asked for. The total index
    // count across the draws equals the sum over the kept chunks, at the level
    // each was assigned — the property that makes a wrong merge a test failure
    // rather than a visual one.
    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(16);
    const auto levelOf = [&chunks](const rm::TerrainChunk& chunk) {
        return static_cast<int>((&chunk - chunks.data()) % 3);
    };
    const auto visible = [&chunks](const rm::TerrainChunk& chunk) {
        return (&chunk - chunks.data()) % 5 != 0;
    };

    std::vector<rm::ChunkDraw> draws;
    rm::appendChunkDraws(draws, chunks, visible, levelOf);

    std::size_t drawn = 0;
    for (const rm::ChunkDraw& draw : draws) {
        drawn += draw.indexCount;
    }

    std::size_t expected = 0;
    for (const rm::TerrainChunk& chunk : chunks) {
        if (visible(chunk)) {
            expected += chunk.lods[static_cast<std::size_t>(levelOf(chunk))].indexCount;
        }
    }
    CHECK(drawn == expected);
}

// --- Detail levels -----------------------------------------------------------

TEST_CASE("detail halves at each threshold, measured in chunk widths") {
    constexpr float width = 512.0f;

    CHECK(rm::lodForDistance(0.0f, width) == 0);
    CHECK(rm::lodForDistance((rm::kLodNearChunks - 0.1f) * width, width) == 0);
    CHECK(rm::lodForDistance((rm::kLodNearChunks + 0.1f) * width, width) == 1);
    CHECK(rm::lodForDistance((rm::kLodFarChunks - 0.1f) * width, width) == 1);
    CHECK(rm::lodForDistance((rm::kLodFarChunks + 0.1f) * width, width) == 2);

    // The same distance in a chunk of half the width is twice as far away in the
    // unit that matters, which is what makes one pair of numbers hold across
    // maps of different sizes.
    CHECK(rm::lodForDistance(rm::kLodNearChunks * width, width * 2.0f) == 0);
}

TEST_CASE("a chunk with no width takes the finest level rather than dividing by zero") {
    CHECK(rm::lodForDistance(10000.0f, 0.0f) == 0);
}

// --- What the thresholds are FOR ---------------------------------------------
// The two assertions below are the reason kLodNearChunks is 6 and not 4, and the
// reason it is 6 and not 20. Neither is visible on screen: the first fails as
// terrain that is subtly wrong in a way that reads as a texture problem, and the
// second fails as a frame that is merely slower than it needed to be.

TEST_CASE("a camera close enough to work at sees nothing but full detail") {
    // The constraint that sets the near threshold. Anything the player is
    // actually looking at — a unit, a build site, a firefight — must be drawn
    // from every height sample the map carries. Measured at --focus 60 on aw04,
    // where level 6 renders an image byte-identical to no LOD at all and level 4
    // does not.
    //
    // A working camera is a few hundred elmos back. Every chunk within a
    // 2000-elmo sphere of it — far more than fills such a view — is level 0.
    constexpr float chunkWidth = 512.0f;
    for (float distance = 0.0f; distance <= 2000.0f; distance += 50.0f) {
        INFO("distance " << distance);
        REQUIRE(rm::lodForDistance(distance, chunkWidth) == 0);
    }
}

TEST_CASE("a whole-map framing is coarse everywhere, so it is also one draw") {
    // The constraint that caps the thresholds from above, and the case that
    // costs the most: a 1024-square map framed whole is vertex-bound, and 47% of
    // its GPU time is detail nobody can see at that distance.
    //
    // Every chunk of an 8192-elmo map seen from outside it is past the far
    // threshold, so all 256 take level 2 — one level, therefore one merged draw
    // rather than a band boundary crossing every row of chunks.
    constexpr float chunkWidth = 512.0f;
    constexpr float mapWidth = 8192.0f;
    CHECK(rm::kLodFarChunks * chunkWidth < mapWidth);

    const std::vector<rm::TerrainChunk> chunks = stripOfChunks(256);
    std::vector<rm::ChunkDraw> draws;
    // Framed whole, the nearest chunk centre is around 8500 elmos out — the
    // camera has to stand off the map's own width to fit it in a 60-degree view.
    rm::appendChunkDraws(draws, chunks, keepAll, [](const rm::TerrainChunk&) {
        return rm::lodForDistance(8500.0f, chunkWidth);
    });

    REQUIRE(draws.size() == 1);
    CHECK(rm::lodForDistance(8500.0f, chunkWidth) == 2);
}
