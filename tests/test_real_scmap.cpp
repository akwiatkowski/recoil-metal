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

#include "core/lua/LuaTable.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/map/Scmap.hpp"
#include "core/texture/Dds.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include <algorithm>
#include <cmath>
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

// --- start positions, from the _save.lua beside each map --------------------
//
// The corpus is not homogeneous, and that turns out to be the whole point of
// testing against it: 54 of the 61 stock maps are skirmish maps with ARMY_<n>
// start markers, and 7 are campaign maps with none at all — their armies are
// named for factions ('Player', 'Seraphim') and spawned by the mission script.
// Zero start positions is therefore a correct answer for a campaign map, and
// these tests assert the distinction rather than averaging over it.

namespace {

/// ScenarioInfo.type from the _scenario.lua beside a map — 'skirmish' or
/// 'campaign'. Empty when the file is missing or says nothing.
[[nodiscard]] std::string scenarioType(const std::filesystem::path& scmapPath) {
    const std::filesystem::path scenarioPath =
        scmapPath.parent_path() / (scmapPath.stem().string() + "_scenario.lua");

    std::error_code ec;
    if (!std::filesystem::is_regular_file(scenarioPath, ec)) {
        return {};
    }

    const auto scenario = rm::lua::parseTableFile(scenarioPath.string());
    if (!scenario) {
        return {};
    }
    const auto type = scenario->stringAt("type");
    return type.has_value() ? std::string{*type} : std::string{};
}

} // namespace

TEST_CASE("every stock map's _save.lua parses, and numbers its teams contiguously") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    std::size_t checked = 0;

    for (const std::filesystem::path& path : maps) {
        INFO("map: " << path.filename().string());

        const auto savePath = rm::scenario::findSaveBesideMap(path);
        REQUIRE(savePath.has_value());

        // Parsing is the contract here, independent of what the file holds:
        // these are 61 real files averaging 200 kB of Lua, and they are what
        // the data-constructor support in the reader exists to survive.
        const auto positions = rm::scenario::loadStartPositionsFile(*savePath);
        INFO("save: " << savePath->filename().string());
        REQUIRE(positions.has_value());

        // Teams must come out as a contiguous 0..n-1 run: a gap would mean an
        // army marker was dropped, which on screen is one team simply missing.
        for (std::size_t i = 0; i < positions->size(); ++i) {
            REQUIRE((*positions)[i].team == static_cast<int>(i));
        }

        ++checked;
    }

    // Pinned so a half-mounted drive cannot pass as a clean run over nothing.
    REQUIRE(checked == maps.size());
    REQUIRE(checked >= 55);
}

TEST_CASE("skirmish maps have start positions and campaign maps have none") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    std::size_t skirmish = 0;
    std::size_t campaign = 0;

    for (const std::filesystem::path& path : maps) {
        const auto savePath = rm::scenario::findSaveBesideMap(path);
        REQUIRE(savePath.has_value());
        const auto positions = rm::scenario::loadStartPositionsFile(*savePath);
        REQUIRE(positions.has_value());

        const std::string type = scenarioType(path);
        INFO("map: " << path.filename().string() << " (" << type << ")");

        if (type == "skirmish") {
            // Every skirmish map is playable, so every one has at least two.
            REQUIRE(positions->size() >= 2);
            ++skirmish;
        } else if (type == "campaign") {
            REQUIRE(positions->empty());
            ++campaign;
        }
    }

    // Both arms must actually have run — a rule that only ever sees one kind of
    // map is not evidence about the other.
    REQUIRE(skirmish >= 50);
    REQUIRE(campaign >= 5);
}

TEST_CASE("start positions land inside their own map, well spread") {
    // The units check: the file stores ogrids and the renderer wants elmos. Get
    // the factor of 8 wrong and every start sits in one corner (or eight map
    // widths away), which no synthetic test would catch.
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;  // the oversized ones, covered above
        }
        const auto savePath = rm::scenario::findSaveBesideMap(path);
        REQUIRE(savePath.has_value());
        const auto positions = rm::scenario::loadStartPositionsFile(*savePath);
        REQUIRE(positions.has_value());
        if (positions->empty()) {
            continue;  // campaign maps
        }

        INFO("map: " << path.filename().string());
        const float width = map->field.widthElmos();
        const float depth = map->field.depthElmos();

        for (const rm::mapinfo::StartPosition& start : *positions) {
            INFO("team " << start.team << " at " << start.x << ", " << start.z);
            REQUIRE(start.x >= 0.0f);
            REQUIRE(start.z >= 0.0f);
            REQUIRE(start.x <= width);
            REQUIRE(start.z <= depth);
        }

        // Not merely inside, but spread out: starts clustered within a few
        // elmos of each other would satisfy the bounds check above and still be
        // a wrong decode. Real maps put opposing teams most of a map apart.
        float maxSeparation = 0.0f;
        for (const auto& a : *positions) {
            for (const auto& b : *positions) {
                maxSeparation =
                    std::max(maxSeparation, std::abs(a.x - b.x) + std::abs(a.z - b.z));
            }
        }
        REQUIRE(maxSeparation > width / 8.0f);
    }
}

TEST_CASE("on skirmish maps the army markers ARE the playable armies") {
    // Justifies not reading _scenario.lua at all (ScenarioSave.hpp). Many stock
    // maps declare an extra ARMY_9 NEUTRAL_CIVILIAN, and the claim is that it
    // never has a marker — so the marker set already is the playable set.
    // Asserted against the file that would know, rather than assumed.
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    std::size_t compared = 0;

    for (const std::filesystem::path& path : maps) {
        // Campaign maps name armies for factions and place none of them by
        // marker, so this comparison is meaningless there.
        if (scenarioType(path) != "skirmish") {
            continue;
        }

        const std::filesystem::path scenarioPath =
            path.parent_path() / (path.stem().string() + "_scenario.lua");

        INFO("map: " << path.filename().string());
        const auto scenario = rm::lua::parseTableFile(scenarioPath.string());
        REQUIRE(scenario.has_value());

        // ScenarioInfo.Configurations.standard.teams[n].armies
        const rm::lua::Value* teams = scenario->path("Configurations", "standard", "teams");
        REQUIRE(teams != nullptr);
        REQUIRE_FALSE(teams->items.empty());

        std::size_t declaredArmies = 0;
        for (const rm::lua::Value& team : teams->items) {
            const rm::lua::Value* armies = team.find("armies");
            REQUIRE(armies != nullptr);
            declaredArmies += armies->items.size();
        }

        const auto savePath = rm::scenario::findSaveBesideMap(path);
        REQUIRE(savePath.has_value());
        const auto positions = rm::scenario::loadStartPositionsFile(*savePath);
        REQUIRE(positions.has_value());

        REQUIRE(positions->size() == declaredArmies);
        ++compared;
    }

    REQUIRE(compared >= 50);
}

// --- the ground splat -------------------------------------------------------

TEST_CASE("every stock map names a base ground layer and ships both masks") {
    // The splat is a recipe, not an image: without the base layer or either mask
    // there is nothing to assemble, and the renderer must fall back rather than
    // draw a plausible-looking wrong thing.
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    std::size_t withUpperStrata = 0;

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;  // the oversized ones
        }
        INFO("map: " << path.filename().string());

        REQUIRE_FALSE(map->albedo[0].empty());
        REQUIRE(map->albedo[0].scale > 0.0f);
        REQUIRE_FALSE(map->maskA.empty());
        REQUIRE_FALSE(map->maskB.empty());

        // A layer with a path must have a usable tile size; a scale of zero
        // would divide by zero when the shader builds the layer UV.
        for (const rm::scmap::TextureRef& layer : map->albedo) {
            if (!layer.empty()) {
                REQUIRE(layer.scale > 0.0f);
            }
        }

        for (std::size_t i = 5; i <= 8; ++i) {
            if (!map->albedo[i].empty()) {
                ++withUpperStrata;
                break;
            }
        }
    }

    // Mask B is not decoration: without it these maps lose half their strata.
    REQUIRE(withUpperStrata >= 10);
}

TEST_CASE("the embedded masks decode as half-resolution BGRA8") {
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;
        }
        INFO("map: " << path.filename().string());

        for (const std::vector<std::byte>* blob : {&map->maskA, &map->maskB}) {
            const auto mask = rm::dds::load(*blob);
            REQUIRE(mask.has_value());
            // Uncompressed, and specifically BGRA — the order Metal takes
            // natively, so the masks upload without a conversion pass.
            REQUIRE(mask->format == rm::dds::Format::Bgra8);
            // Half the map's resolution on both axes, on every stock map.
            REQUIRE(mask->width == map->field.squaresX / 2);
            REQUIRE(mask->height == map->field.squaresZ / 2);
        }
    }
}

TEST_CASE("the embedded normal map is DXT5, in one tile or four") {
    // Tiling is what a naive parser trips over: every map up to 2048 squares
    // stores one tile, and the three 4096-square maps store four 2048 ones.
    //
    // This test used to assert a count of exactly one, because those three maps
    // were refused for size before ever reaching here — with a comment saying
    // so, and that the day it stopped being true this should say so. Terrain
    // decimation lifted the size limit, and it duly did.
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;
        }
        INFO("map: " << path.filename().string());

        // Four tiles means a 2x2 arrangement, so each covers half the map on
        // each axis; one tile covers all of it.
        const std::size_t tiles = map->normalMapTiles.size();
        REQUIRE((tiles == 1 || tiles == 4));
        const int perAxis = tiles == 4 ? 2 : 1;

        for (const auto& tile : map->normalMapTiles) {
            const auto normals = rm::dds::load(tile);
            REQUIRE(normals.has_value());
            REQUIRE(normals->format == rm::dds::Format::Bc3);
            REQUIRE(normals->width == map->field.squaresX / perAxis);
            REQUIRE(normals->height == map->field.squaresZ / perAxis);
        }
    }
}

TEST_CASE("every stratum texture a stock map names is on disk") {
    // The layer textures are NOT in the map file — they are paths into env.scd,
    // extracted once (see README). This is the test that catches a map naming
    // something the extraction missed, which would otherwise show up as one
    // stratum silently absent from the blend.
    const auto maps = corpus();
    if (maps.empty()) {
        SKIP("retail Forged Alliance maps not mounted");
    }

    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        SKIP("no HOME");
    }
    const std::filesystem::path root = std::filesystem::path{home} / "projects/llm/input/faf";

    std::error_code ec;
    if (!std::filesystem::is_directory(root / "env", ec)) {
        SKIP("extracted Forged Alliance env textures not present");
    }

    std::set<std::string> missing;
    std::size_t checked = 0;

    for (const std::filesystem::path& path : maps) {
        const auto map = rm::scmap::loadFile(path);
        if (!map) {
            continue;
        }

        for (const rm::scmap::TextureRef& layer : map->albedo) {
            if (layer.empty()) {
                continue;
            }
            std::string relative = layer.path;
            if (relative.front() == '/') {
                relative.erase(0, 1);
            }
            if (!std::filesystem::is_regular_file(root / relative, ec)) {
                missing.insert(layer.path);
                continue;
            }
            // Present is not enough — it has to decode, since a stratum that
            // fails to load drops out of the blend without an error on screen.
            const auto texture = rm::dds::loadFile(root / relative);
            INFO("layer: " << layer.path);
            REQUIRE(texture.has_value());
            ++checked;
        }
    }

    if (!missing.empty()) {
        std::string report = std::to_string(missing.size()) + " stratum textures missing:";
        for (const std::string& name : missing) {
            report += "\n  " + name;
        }
        FAIL(report);
    }
    REQUIRE(checked > 300);
}
