#include "core/map/MapInfo.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace {

[[nodiscard]] std::string toLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    std::transform(text.begin(), text.end(), std::back_inserter(lowered),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

/// Case-insensitive child lookup.
///
/// Lua itself is case-sensitive, but map authors are not consistent: the engine
/// documents these keys as minHeight/maxHeight while every real file writes
/// minheight/maxheight. Matching loosely here costs nothing and avoids silently
/// ignoring an override because of its capitalisation — which would put us back
/// to rendering the map upside down.
[[nodiscard]] const rm::lua::Value* findLoose(const rm::lua::Value& table,
                                              std::string_view key) {
    if (!table.isTable()) {
        return nullptr;
    }
    const std::string wanted = toLower(key);
    for (const auto& field : table.fields) {
        if (toLower(field.key) == wanted) {
            return &field.value;
        }
    }
    return nullptr;
}

} // namespace

namespace rm::mapinfo {

std::expected<MapInfo, lua::ParseError> parse(std::string_view luaSource) {
    auto root = lua::parseTable(luaSource);
    if (!root) {
        return std::unexpected(root.error());
    }

    MapInfo info;

    const lua::Value* smf = findLoose(*root, "smf");
    if (smf != nullptr) {
        const lua::Value* minValue = findLoose(*smf, "minheight");
        const lua::Value* maxValue = findLoose(*smf, "maxheight");

        // Both or neither. A half-override would mix a Lua minimum with a
        // header maximum and produce a plausible but wrong vertical scale.
        if (minValue != nullptr && maxValue != nullptr) {
            const auto minHeight = minValue->asNumber();
            const auto maxHeight = maxValue->asNumber();
            if (minHeight.has_value() && maxHeight.has_value()) {
                info.verticalRange = VerticalRange{static_cast<float>(*minHeight),
                                                   static_cast<float>(*maxHeight)};
            }
        }

        if (const lua::Value* mapFile = findLoose(*smf, "mapfile")) {
            if (const auto text = mapFile->asString()) {
                info.mapFile = std::string{*text};
            }
        }

        // smtFileName0, smtFileName1, ... contiguous from zero; the engine only
        // applies them when the count matches the .smf's embedded list
        // (SMFGroundTextures.cpp:122-127), so a gap ends the sequence.
        for (int index = 0;; ++index) {
            const std::string key = "smtfilename" + std::to_string(index);
            const lua::Value* entry = findLoose(*smf, key);
            if (entry == nullptr) {
                break;
            }
            const auto text = entry->asString();
            if (!text.has_value()) {
                break;
            }
            info.smtFileNames.emplace_back(*text);
        }
    }

    // teams = { [0] = { startPos = { x = 1500, z = 1500 } }, ... }
    // Bracketed numeric keys are stored under their decimal spelling by the Lua
    // reader, so team 0 is the child named "0".
    if (const lua::Value* teams = findLoose(*root, "teams")) {
        for (int team = 0;; ++team) {
            const lua::Value* entry = findLoose(*teams, std::to_string(team));
            if (entry == nullptr) {
                break;
            }
            const lua::Value* startPos = findLoose(*entry, "startpos");
            if (startPos == nullptr) {
                break;
            }
            const lua::Value* x = findLoose(*startPos, "x");
            const lua::Value* z = findLoose(*startPos, "z");
            if (x == nullptr || z == nullptr) {
                break;
            }
            const auto xv = x->asNumber();
            const auto zv = z->asNumber();
            if (!xv.has_value() || !zv.has_value()) {
                break;
            }
            info.startPositions.push_back(StartPosition{team, static_cast<float>(*xv),
                                                        static_cast<float>(*zv)});
        }
    }

    info.root = std::move(*root);
    return info;
}

std::expected<MapInfo, lua::ParseError> parseFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(
            lua::ParseError{"could not open \"" + path.string() + "\"", 0});
    }

    const std::string contents{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
    return parse(contents);
}

std::optional<std::filesystem::path> findBesideMap(const std::filesystem::path& smfPath) {
    const std::filesystem::path directory = smfPath.parent_path();

    const std::filesystem::path candidates[] = {
        directory / "mapinfo.lua",
        directory.parent_path() / "mapinfo.lua",
    };

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return std::nullopt;
}

} // namespace rm::mapinfo
