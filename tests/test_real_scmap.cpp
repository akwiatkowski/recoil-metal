// Validation against the REAL retail Supreme Commander map corpus.
//
// The format was decoded from these files, so they are the only thing that can
// catch a misreading — a synthetic writer would simply encode the same mistake
// the parser makes. Assets are never committed (AGENT.md rule 3) and live on an
// external drive, so these tests SKIP rather than fail when it is not mounted.
//
// The corpus is the stock maps of a retail Steam install:
//   /Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/maps/SCMP_0NN/
#include <catch2/catch_test_macros.hpp>

#include "core/map/Scmap.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kCorpusRoot =
    "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/maps";

[[nodiscard]] std::vector<std::filesystem::path> corpus() {
    std::vector<std::filesystem::path> maps;

    std::error_code ec;
    if (!std::filesystem::is_directory(kCorpusRoot, ec)) {
        return maps;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator{kCorpusRoot, ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".scmap") {
            maps.push_back(entry.path());
        }
    }

    // Directory order is filesystem-dependent; a stable order keeps a failure
    // report meaningful between runs.
    std::sort(maps.begin(), maps.end());
    return maps;
}

} // namespace

TEST_CASE("every stock .scmap decodes, and lands exactly on EOF") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    // The acid test. Nothing is seeked to, so a wrong field width anywhere
    // shifts every section after it and the last array cannot end on the last
    // byte. Passing on the whole corpus — 256 through 2048 squares, wet and
    // dry, square and not — is evidence rather than a plausible-looking dump.
    std::size_t decoded = 0;
    std::size_t refusedAsTooLarge = 0;

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);

        if (!map) {
            // The three 4096-square maps are refused deliberately, and only for
            // that reason — any other failure is a real one.
            INFO("failed: " << path.filename().string() << " — " << map.error().message);
            REQUIRE(map.error().code == rm::MapError::Code::TooLarge);
            ++refusedAsTooLarge;
            continue;
        }

        INFO("map: " << path.filename().string());
        REQUIRE(map->endsExactlyAtEof);
        ++decoded;
    }

    // Pinned so a corpus that quietly shrinks — a drive half-mounted, say —
    // cannot pass as a clean run over nothing.
    REQUIRE(decoded + refusedAsTooLarge == maps.size());
    REQUIRE(decoded >= 50);
}

TEST_CASE("decoded .scmap geometry is sane and fills a HeightField") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    std::set<int> sizesSeen;

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;  // the oversized ones, covered above
        }
        INFO("map: " << path.filename().string());

        // Corner-sampled, exactly like SMF: an N-square axis has N+1 samples.
        REQUIRE(map->field.raw.size() == map->field.sampleCount());
        REQUIRE(map->field.raw.size()
                == static_cast<std::size_t>(map->field.squaresX + 1)
                       * static_cast<std::size_t>(map->field.squaresZ + 1));

        // Every stock map uses the same fixed vertical scale — there is no
        // mapinfo.lua equivalent to override it, unlike SMF.
        REQUIRE(map->field.baseHeight == 0.0f);
        REQUIRE(map->field.heightScale > 0.0f);

        // Terrain type is full map resolution, one byte per square.
        REQUIRE(map->terrainType.size()
                == static_cast<std::size_t>(map->typesX)
                       * static_cast<std::size_t>(map->typesZ));
        REQUIRE(map->typesX == map->field.squaresX);
        REQUIRE(map->typesZ == map->field.squaresZ);

        sizesSeen.insert(map->field.squaresX);
    }

    // The corpus is not all one size — a reader that hard-coded 1024 would pass
    // a single-map test and fail here.
    REQUIRE(sizesSeen.size() > 1);
}

TEST_CASE("a .scmap heightmap builds a terrain mesh with real relief") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    // One map is enough here: this asserts the seam, not the corpus. If the
    // height scale were wrong by the 8 elmos-per-ogrid factor the map would
    // still build — just flat — so the check is on the relief, not on success.
    const auto map = rm::scmap::loadFile(maps.front());
    REQUIRE(map.has_value());

    const rm::TerrainMesh mesh = rm::buildTerrainMesh(map->field);
    REQUIRE(mesh.vertices.size() == map->field.raw.size());
    REQUIRE(mesh.maxY > mesh.minY);

    // Relief of at least half a percent of map width. FAR measured typical
    // SupCom relief at 3-6% (report 06 §8.2); half a percent is far below that
    // and still an order of magnitude above what a missing x8 would leave.
    const float relief = mesh.maxY - mesh.minY;
    REQUIRE(relief > map->field.widthElmos() * 0.005f);
}

TEST_CASE("terrain-type values are banded, not noise") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    const auto map = rm::scmap::loadFile(maps.front());
    REQUIRE(map.has_value());

    std::set<std::uint8_t> distinct(map->terrainType.begin(), map->terrainType.end());

    // A map uses a handful of terrain types in broad bands. Reading the array at
    // the wrong offset — or at the wrong width — yields either one value or
    // dozens, and both are caught here.
    REQUIRE(distinct.size() > 1);
    REQUIRE(distinct.size() < 40);
}

TEST_CASE("a buffer that is not a .scmap is refused by its magic") {
    const std::vector<std::byte> notAMap(64, std::byte{0x41});

    const auto map = rm::scmap::load(notAMap);

    REQUIRE_FALSE(map.has_value());
    REQUIRE(map.error().code == rm::MapError::Code::NotScmap);
    REQUIRE_FALSE(rm::scmap::looksLikeScmap(notAMap));
}

TEST_CASE("a truncated .scmap fails instead of reading past the end") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    // Deliberately cut inside the heightmap, which is the largest early
    // section: the loader must notice rather than resize to a bogus count.
    std::vector<std::byte> bytes;
    {
        std::ifstream file{maps.front(), std::ios::binary};
        REQUIRE(file);
        bytes.assign(4096, std::byte{});
        file.read(reinterpret_cast<char*>(bytes.data()), 4096);
    }

    const auto map = rm::scmap::load(bytes);

    REQUIRE_FALSE(map.has_value());
    REQUIRE(map.error().code == rm::MapError::Code::Truncated);
}
