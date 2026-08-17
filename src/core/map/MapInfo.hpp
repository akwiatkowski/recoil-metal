#pragma once

#include "core/lua/LuaTable.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rm::mapinfo {

// The vertical range a map's terrain spans, in elmos.
struct VerticalRange {
    float minHeight;
    float maxHeight;
};

// A team's spawn point, in elmos. Only X and Z are given — the Y comes from
// sampling the heightmap, which is what the engine does too.
struct StartPosition {
    int team = 0;
    float x = 0.0f;
    float z = 0.0f;
};

// The parts of mapinfo.lua the renderer currently needs.
//
// WHY THIS EXISTS: the .smf binary header is not the authority on its own
// contents. mapinfo.lua overrides the vertical range entirely
// (rts/Map/MapInfo.cpp:405-418 -> SMFReadMap.cpp:133-158) and can rename the
// tile files (smf.smtFileName0..N, MapInfo.cpp:420-427 consumed at
// SMFGroundTextures.cpp:122-127).
//
// Not academic: BAR's "Angel Crossing 1.4" ships a header saying
// minHeight=850, maxHeight=-150 — inverted — and corrects it in Lua. Honour
// only the header and the map renders upside down, plausibly enough that
// nothing flags it.
//
// Backed by core/lua, which parses Lua *data* and refuses Lua *programs*. A
// mapinfo.lua that computes a value yields a ParseError and the caller keeps
// the header's values; see LuaTable.hpp for why that is the honest failure.
struct MapInfo {
    std::optional<VerticalRange> verticalRange;

    /// smf.smtFileName0, 1, ... in index order. Empty means "use the names
    /// embedded in the .smf tile section".
    std::vector<std::string> smtFileNames;

    /// smf.mapfile, when present (e.g. "maps/aw04.smf").
    std::optional<std::string> mapFile;

    /// `teams[N].startPos` in team order, contiguous from team 0. Real maps
    /// declare one per supported player: Angel Crossing has eight.
    std::vector<StartPosition> startPositions;

    /// The whole parsed table, for keys this struct does not model yet
    /// (water, lighting, splats — milestone 3+).
    lua::Value root;
};

[[nodiscard]] std::expected<MapInfo, lua::ParseError> parse(std::string_view lua);

[[nodiscard]] std::expected<MapInfo, lua::ParseError> parseFile(
    const std::filesystem::path& path);

/// Locates the mapinfo.lua belonging to an .smf, if one is on disk. Checks
/// beside the map file and one directory up, covering both a flat extraction
/// and the `maps/foo.smf` + `mapinfo.lua` layout used inside .sd7 archives.
[[nodiscard]] std::optional<std::filesystem::path> findBesideMap(
    const std::filesystem::path& smfPath);

} // namespace rm::mapinfo
