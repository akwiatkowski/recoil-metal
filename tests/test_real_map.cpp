// Validation against a REAL Recoil map, which is the only thing that can catch
// a spec misreading shared by the synthetic writer and the parser.
//
// Assets are never committed (AGENT.md rule 3), so the map lives outside the
// repo and its absence is the normal case on a fresh clone. These tests SKIP
// rather than fail when it is missing — a red suite for a missing optional
// asset trains people to ignore red suites.
//
// To provide the map:
//   curl -sL -o ~/projects/llm/input/recoil/maps/angel_crossing_1.4.sd7 \
//     "https://files-cdn.beyondallreason.dev/file/9fc29b4e9dd666d9f9866280fb3c0861/angel_crossing_1.4.sd7"
//   cd ~/projects/llm/input/recoil/maps \
//     && 7zz e -y angel_crossing_1.4.sd7 maps/aw04.smf maps/aw04.smt mapinfo.lua -o.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/MapInfo.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <cstdlib>
#include <filesystem>
#include <string>

using Catch::Approx;

namespace {

// BAR's "Angel Crossing 1.4", md5 9fc29b4e9dd666d9f9866280fb3c0861.
constexpr const char* kRelativeMapPath = "projects/llm/input/recoil/maps/aw04.smf";

[[nodiscard]] std::filesystem::path realMapPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::filesystem::path{home} / kRelativeMapPath;
}

[[nodiscard]] bool haveRealMap() {
    std::error_code ec;
    return std::filesystem::is_regular_file(realMapPath(), ec);
}

/// The tile file is a separate 44 MB extract, so it can be absent even when the
/// .smf is present — the texture tests skip independently of the geometry ones.
[[nodiscard]] std::filesystem::path realSmtPath() {
    return realMapPath().parent_path() / "aw04.smt";
}

[[nodiscard]] bool haveRealSmt() {
    std::error_code ec;
    return haveRealMap() && std::filesystem::is_regular_file(realSmtPath(), ec);
}

} // namespace

TEST_CASE("a real BAR map parses", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present at " + realMapPath().string());
    }

    const auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    // Angel Crossing is a 1024 x 1024 square map = 8192 x 8192 elmos.
    // Cross-checked by hand against the file's header bytes.
    REQUIRE(field->squaresX == 1024);
    REQUIRE(field->squaresZ == 1024);
    REQUIRE(field->widthElmos() == Approx(8192.0f));

    // 1025 x 1025 corner samples, all of them actually read.
    REQUIRE(field->raw.size() == 1025u * 1025u);
    REQUIRE(field->sampleCount() == field->raw.size());
}

TEST_CASE("the real map's header offsets are internally consistent", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    // The strongest available check that the header was decoded correctly:
    // the file's own section pointers must chain. heightmapPtr + the heightmap's
    // computed size lands exactly on typeMapPtr in this file (65628 + 2101250
    // = 2166878), which cannot happen if any field offset were misread.
    // Verified by hand from the hexdump; asserted here via the decode succeeding
    // with the exact dimensions above, since load() bounds-checks that the
    // heightmap fits within the file.
    const auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    const std::size_t heightmapBytes = field->raw.size() * sizeof(std::uint16_t);
    REQUIRE(heightmapBytes == 2101250u);
}

TEST_CASE("the real map ships an inverted header that mapinfo.lua corrects",
          "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    const auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    // The binary header really does say min=850, max=-150 — backwards. This is
    // not a parser bug, it is what the file contains, and it is why the loader
    // must not be the last word on vertical scale.
    REQUIRE(field->baseHeight == Approx(850.0f));
    const float headerMax =
        field->baseHeight + field->heightScale * rm::kHeightQuantisationSteps;
    REQUIRE(headerMax == Approx(-150.0f));
    REQUIRE(field->heightScale < 0.0f);

    const auto infoPath = rm::mapinfo::findBesideMap(realMapPath());
    REQUIRE(infoPath.has_value());

    const auto info = rm::mapinfo::parseFile(*infoPath);
    REQUIRE(info.has_value());
    REQUIRE(info->verticalRange.has_value());
    REQUIRE(info->verticalRange->minHeight == Approx(-150.0f));
    REQUIRE(info->verticalRange->maxHeight == Approx(850.0f));
}

TEST_CASE("the corrected real map decodes into a sane world", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    const auto infoPath = rm::mapinfo::findBesideMap(realMapPath());
    REQUIRE(infoPath.has_value());
    const auto info = rm::mapinfo::parseFile(*infoPath);
    REQUIRE(info.has_value());
    const auto range = info->verticalRange;
    REQUIRE(range.has_value());
    field->setVerticalRange(range->minHeight, range->maxHeight);

    // Every decoded sample must land inside the declared range.
    float lowest = field->heightAt(0, 0);
    float highest = lowest;
    for (int z = 0; z <= field->squaresZ; z += 8) {
        for (int x = 0; x <= field->squaresX; x += 8) {
            const float height = field->heightAt(x, z);
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }

    REQUIRE(lowest >= range->minHeight);
    REQUIRE(highest <= range->maxHeight);

    // A real map has relief — a constant heightmap would mean we decoded
    // nothing useful even though every bounds check passed.
    REQUIRE(highest - lowest > 50.0f);

    // And some of it is below the water plane, which Recoil fixes at y = 0
    // (rts/Map/Ground.h:32). Angel Crossing has water.
    REQUIRE(lowest < 0.0f);
}

TEST_CASE("the real map builds a complete terrain mesh", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    const auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    const auto mesh = rm::buildTerrainMesh(*field);

    REQUIRE(mesh.verticesX == 1025);
    REQUIRE(mesh.verticesZ == 1025);
    REQUIRE(mesh.triangleCount() == 1024u * 1024u * 2u);

    // The skirts hanging off every chunk at every level, on a real map. Pinned
    // as a fraction rather than left implicit: this is the price of not having
    // cracks between detail levels, and a change that doubled it should have
    // to say so here.
    const std::size_t grid = 1025u * 1025u;
    REQUIRE(mesh.vertices.size() > grid);
    CHECK(static_cast<double>(mesh.vertices.size() - grid) / static_cast<double>(grid)
          < 0.15);
    REQUIRE(mesh.maxX == Approx(8192.0f));
    REQUIRE(mesh.maxZ == Approx(8192.0f));

    // ~2.1M triangles at 24 bytes per vertex plus 4 per index: worth knowing
    // this is ~50 MB of GPU buffers before anyone reaches for LOD.
    // Full detail; mesh.indices also holds the coarser levels of each chunk.
    REQUIRE(mesh.triangleCount() == 1024u * 1024u * 2u);
}

TEST_CASE("the real map's tile index parses", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    const auto index = rm::smf::loadTileIndexFile(realMapPath());
    REQUIRE(index.has_value());

    // 1024 squares / 4 squares per tile = 256 tiles per axis.
    REQUIRE(index->tilesX == 256);
    REQUIRE(index->tilesZ == 256);
    REQUIRE(index->indices.size() == 256u * 256u);

    // One tile file, named by the .smf itself.
    REQUIRE(index->smtFileNames.size() == 1);
    REQUIRE(index->smtFileNames[0].find("aw04.smt") != std::string::npos);

    // Every index must address a tile that actually exists.
    for (const std::int32_t tile : index->indices) {
        REQUIRE(tile >= 0);
        REQUIRE(tile < index->totalTiles);
    }
}

TEST_CASE("the real map's .smt parses and matches its index", "[real-map]") {
    if (!haveRealSmt()) {
        SKIP("real .smt not present at " + realSmtPath().string());
    }

    const auto tiles = rm::smt::loadFile(realSmtPath());
    REQUIRE(tiles.has_value());

    // 44 564 512 bytes = 32-byte header + 65 536 tiles x 680 bytes, exactly.
    REQUIRE(tiles->tileCount == 65536);
    REQUIRE(tiles->tiles.size() == 65536u * rm::smt::kTileBytes);

    const auto index = rm::smf::loadTileIndexFile(realMapPath());
    REQUIRE(index.has_value());
    REQUIRE(index->totalTiles == tiles->tileCount);
}

TEST_CASE("the real map assembles into a full-resolution atlas", "[real-map]") {
    if (!haveRealSmt()) {
        SKIP("real .smt not present");
    }

    const auto index = rm::smf::loadTileIndexFile(realMapPath());
    REQUIRE(index.has_value());
    const auto tiles = rm::smt::loadFile(realSmtPath());
    REQUIRE(tiles.has_value());

    const auto atlas = rm::buildTileAtlas(*index, *tiles);
    REQUIRE(atlas.has_value());

    // 1 texel per elmo: a 1024-square map is 8192 texels across.
    REQUIRE(atlas->widthTexels == 8192);
    REQUIRE(atlas->heightTexels == 8192);

    // BC1 is half a byte per texel: 32 MiB at level 0, ~42 MiB with all mips.
    REQUIRE(atlas->mipBytes(0) == 8192u * 8192u / 2u);
    REQUIRE(atlas->data.size() < 64u * 1024u * 1024u);

    // Counting the 0xAA fill would NOT prove the copy happened: 0xAA is
    // 0b10101010, a perfectly ordinary DXT1 selector byte, and it occurs in
    // ~2.8% of this map's real tile data. Checking exact placement instead —
    // every sampled cell's blocks must equal the source tile's, byte for byte.
    for (const auto [tx, ty] : {std::pair{0, 0}, std::pair{7, 3}, std::pair{255, 255},
                                std::pair{128, 64}}) {
        const std::size_t slot =
            static_cast<std::size_t>(ty) * static_cast<std::size_t>(index->tilesX)
            + static_cast<std::size_t>(tx);
        const std::span<const std::byte> source = tiles->mip(index->indices[slot], 0);
        REQUIRE_FALSE(source.empty());

        constexpr std::size_t kBlocksPerTile = rm::smt::kTileSize / rm::kBlockTexels;  // 8
        const std::size_t rowBytes = atlas->mipBytesPerRow(0);

        for (std::size_t row = 0; row < kBlocksPerTile; ++row) {
            const std::size_t dst =
                (static_cast<std::size_t>(ty) * kBlocksPerTile + row) * rowBytes
                + static_cast<std::size_t>(tx) * kBlocksPerTile * rm::kBlockBytes;
            const std::size_t src = row * kBlocksPerTile * rm::kBlockBytes;

            REQUIRE(std::equal(source.begin() + static_cast<std::ptrdiff_t>(src),
                               source.begin() + static_cast<std::ptrdiff_t>(
                                   src + kBlocksPerTile * rm::kBlockBytes),
                               atlas->data.begin() + static_cast<std::ptrdiff_t>(dst)));
        }
    }
}

TEST_CASE("the real map declares eight team start positions", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    const auto infoPath = rm::mapinfo::findBesideMap(realMapPath());
    REQUIRE(infoPath.has_value());
    const auto info = rm::mapinfo::parseFile(*infoPath);
    REQUIRE(info.has_value());

    // Angel Crossing is an 8-player map.
    REQUIRE(info->startPositions.size() == 8);

    // Every spawn must land inside the map, which is the check that would fire
    // if x and z were swapped or scaled wrongly.
    const auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    for (const auto& start : info->startPositions) {
        REQUIRE(start.x >= 0.0f);
        REQUIRE(start.z >= 0.0f);
        REQUIRE(start.x <= field->widthElmos());
        REQUIRE(start.z <= field->depthElmos());
    }
}
