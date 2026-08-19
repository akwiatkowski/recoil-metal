#include "core/map/ScenarioSave.hpp"

#include "core/map/Scmap.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using rm::lua::ParseError;
using rm::lua::Value;

constexpr std::string_view kArmyPrefix = "ARMY_";

/// The army number in a marker key like "ARMY_7", or nullopt for any other
/// marker — the corpus is full of Transport, Rally and Mass markers that share
/// the same table and must simply be ignored.
[[nodiscard]] std::optional<int> armyNumber(std::string_view key) {
    if (!key.starts_with(kArmyPrefix)) {
        return std::nullopt;
    }

    const std::string_view digits = key.substr(kArmyPrefix.size());
    if (digits.empty()) {
        return std::nullopt;
    }

    int number = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    // Anything trailing means this is not a plain ARMY_<n> — refuse it rather
    // than accept the prefix that happened to parse.
    if (ec != std::errc{} || end != digits.data() + digits.size() || number < 1) {
        return std::nullopt;
    }
    return number;
}

[[nodiscard]] std::unexpected<ParseError> fail(std::string message) {
    return std::unexpected(ParseError{std::move(message), 0});
}

} // namespace

namespace rm::scenario {

bool Marker::isType(std::string_view wanted) const noexcept {
    return std::ranges::equal(type, wanted, [](char a, char b) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        return lower(a) == lower(b);
    });
}

std::expected<std::vector<Marker>, lua::ParseError> loadMarkers(std::string_view lua) {
    const auto root = rm::lua::parseTable(lua);
    if (!root) {
        return std::unexpected(root.error());
    }

    const Value* markers = root->path("MasterChain", "_MASTERCHAIN_", "Markers");
    if (markers == nullptr || !markers->isTable()) {
        return fail("no Scenario.MasterChain._MASTERCHAIN_.Markers table: this does not look "
                    "like a Supreme Commander _save.lua");
    }

    std::vector<Marker> found;
    found.reserve(markers->fields.size());

    for (const lua::Field& entry : markers->fields) {
        const Value* position = entry.value.find("position");
        const bool usable = position != nullptr && position->isTable()
                         && position->items.size() == 3;

        if (!usable) {
            // Fatal only for the markers the engine depends on. See the header.
            if (armyNumber(entry.key).has_value()) {
                return fail("marker '" + entry.key
                            + "' has no VECTOR3 position; the _save.lua layout is not what "
                              "this reader expects");
            }
            continue;
        }

        const std::optional<double> x = position->items[0].asNumber();
        const std::optional<double> y = position->items[1].asNumber();
        const std::optional<double> z = position->items[2].asNumber();
        if (!x || !y || !z) {
            if (armyNumber(entry.key).has_value()) {
                return fail("marker '" + entry.key + "' has a non-numeric position");
            }
            continue;
        }

        // The stored Y is KEPT here, unlike in loadStartPositions, and for the same
        // reason it is dropped there: a spawn belongs on the terrain, so its height is
        // sampled — but a marker is an annotation of a place, and a caller comparing it
        // against the heightmap wants to see what the map actually said.
        found.push_back(Marker{
            .name = entry.key,
            .type = std::string{entry.value.stringAt("type").value_or("")},
            .position = {static_cast<float>(*x) * scmap::kElmosPerOgrid,
                         static_cast<float>(*y) * scmap::kElmosPerOgrid,
                         static_cast<float>(*z) * scmap::kElmosPerOgrid},
        });
    }

    return found;
}

std::expected<std::vector<mapinfo::StartPosition>, lua::ParseError> loadStartPositions(
    std::string_view lua) {
    const auto root = rm::lua::parseTable(lua);
    if (!root) {
        return std::unexpected(root.error());
    }

    // Scenario = { MasterChain = { ['_MASTERCHAIN_'] = { Markers = { ... } } } }
    const Value* markers = root->path("MasterChain", "_MASTERCHAIN_", "Markers");
    if (markers == nullptr || !markers->isTable()) {
        return fail("no Scenario.MasterChain._MASTERCHAIN_.Markers table: this does not look "
                    "like a Supreme Commander _save.lua");
    }

    std::vector<mapinfo::StartPosition> positions;

    for (const lua::Field& marker : markers->fields) {
        const std::optional<int> army = armyNumber(marker.key);
        if (!army.has_value()) {
            continue;  // Transport/Rally/Mass/... markers share this table.
        }

        // Loud rather than skipped: an ARMY_ marker whose position is missing or
        // is not a 3-vector means this reader has the format wrong, and a
        // silently absent start would show up much later as a map that spawns
        // one team short.
        const Value* position = marker.value.find("position");
        if (position == nullptr || !position->isTable() || position->items.size() != 3) {
            return fail("marker '" + marker.key
                        + "' has no VECTOR3 position; the _save.lua layout is not what this "
                          "reader expects");
        }

        const std::optional<double> x = position->items[0].asNumber();
        const std::optional<double> z = position->items[2].asNumber();
        if (!x.has_value() || !z.has_value()) {
            return fail("marker '" + marker.key + "' has a non-numeric position");
        }

        // items[1] is the marker's height, deliberately dropped: the engine
        // drops units onto the terrain and so does atStartPositions, which
        // samples the heightmap. Trusting a stored Y would float or bury a unit
        // wherever the marker and the heightmap disagree.
        positions.push_back(mapinfo::StartPosition{
            .team = *army - 1,  // ARMY_1 is team 0, matching mapinfo.lua's base
            .x = static_cast<float>(*x) * scmap::kElmosPerOgrid,
            .z = static_cast<float>(*z) * scmap::kElmosPerOgrid,
        });
    }

    // Markers appear in whatever order the map editor wrote them, so ARMY_1 is
    // rarely first in the file. Sorting makes team n the nth position, which is
    // what the team-colour palette assumes when it takes the nth entry.
    std::sort(positions.begin(), positions.end(),
              [](const mapinfo::StartPosition& a, const mapinfo::StartPosition& b) {
                  return a.team < b.team;
              });

    return positions;
}

std::expected<std::vector<mapinfo::StartPosition>, lua::ParseError> loadStartPositionsFile(
    const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return fail("could not open \"" + path.string() + "\"");
    }

    const std::string contents{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
    return loadStartPositions(contents);
}

std::optional<std::filesystem::path> findSaveBesideMap(const std::filesystem::path& scmapPath) {
    std::error_code ec;

    const std::filesystem::path directory = scmapPath.parent_path();
    const std::filesystem::path named =
        directory / (scmapPath.stem().string() + "_save.lua");
    if (std::filesystem::is_regular_file(named, ec)) {
        return named;
    }

    if (!std::filesystem::is_directory(directory, ec)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().ends_with("_save.lua")) {
            return entry.path();
        }
    }
    return std::nullopt;
}

} // namespace rm::scenario
