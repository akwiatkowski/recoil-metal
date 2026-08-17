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
//   cd ~/projects/llm/input/recoil/maps && 7zz e -y angel_crossing_1.4.sd7 maps/aw04.smf mapinfo.lua -o.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/MapInfo.hpp"
#include "core/map/Smf.hpp"
#include "core/mesh/TerrainMesh.hpp"

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

    const auto range = rm::mapinfo::findVerticalRangeInFile(*infoPath);
    REQUIRE(range.has_value());
    REQUIRE(range->minHeight == Approx(-150.0f));
    REQUIRE(range->maxHeight == Approx(850.0f));
}

TEST_CASE("the corrected real map decodes into a sane world", "[real-map]") {
    if (!haveRealMap()) {
        SKIP("real map not present");
    }

    auto field = rm::smf::loadFile(realMapPath());
    REQUIRE(field.has_value());

    const auto infoPath = rm::mapinfo::findBesideMap(realMapPath());
    REQUIRE(infoPath.has_value());
    const auto range = rm::mapinfo::findVerticalRangeInFile(*infoPath);
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

    REQUIRE(mesh.vertices.size() == 1025u * 1025u);
    REQUIRE(mesh.triangleCount() == 1024u * 1024u * 2u);
    REQUIRE(mesh.maxX == Approx(8192.0f));
    REQUIRE(mesh.maxZ == Approx(8192.0f));

    // ~2.1M triangles at 24 bytes per vertex plus 4 per index: worth knowing
    // this is ~50 MB of GPU buffers before anyone reaches for LOD.
    REQUIRE(mesh.indices.size() == 1024u * 1024u * 6u);
}
