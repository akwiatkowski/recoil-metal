#pragma once

#include "core/lua/LuaTable.hpp"
#include "core/map/MapInfo.hpp"

#include <array>
#include <expected>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace rm::scenario {

// Start positions for a Supreme Commander map.
//
// WHERE THESE LIVE, AND WHERE THEY DO NOT. The .scmap binary carries no start
// positions at all — unlike .smf, whose mapinfo.lua sits beside it with a
// `startPositions` table. SupCom splits the job across two text files, and the
// obvious-looking one is the wrong one:
//
//   <map>_scenario.lua  names the armies and groups them into teams, but holds
//                       no coordinates whatsoever
//   <map>_save.lua      holds the markers, positions included, under
//                       Scenario.MasterChain._MASTERCHAIN_.Markers
//
// So this reads _save.lua. Start positions are the markers keyed 'ARMY_<n>',
// whose 'position' is a VECTOR3 in ogrids.
//
// WHY _scenario.lua IS NOT ALSO READ. It would be the authority on which armies
// are playable — 27 of the 60 stock maps declare an extra `ARMY_9
// NEUTRAL_CIVILIAN` — but that army has no marker in _save.lua on any of them,
// so the marker set already IS the playable set. Checked across the whole
// corpus rather than assumed, and asserted there so the day a map disagrees is
// a test failure rather than a stray unit in a corner.
//
// Positions come back in ELMOS, converted from the ogrids the file stores, so
// they are interchangeable with the mapinfo.lua ones the SMF path produces and
// `atStartPositions` needs no idea which family it was handed.
// One entry of a map's marker table, whatever kind it is.
//
// The table is the map's own annotation of itself and it is FULL: 3695 blank markers
// across the 61 stock maps, 3508 mass deposits, 1114 defensive points, 677 transport
// markers, 524 rally points — and a complete navigation graph the game's own AI walks
// (1974 land path nodes, 2120 amphibious, 1731 air, 1062 water).
//
// Only the army markers were read before, because only spawns were needed. The mass
// deposits are what an economy is built on, so the whole table is read now and callers
// filter — which also makes the graph available as a free second opinion on this
// engine's own pathfinding.
struct Marker {
    std::string name;                 ///< the table's key, e.g. "ARMY_1" or "Mass 27"
    std::string type;                 ///< the `type` field, e.g. "Mass", "Blank Marker"
    std::array<float, 3> position{};  ///< elmos, converted from the ogrids stored

    /// Whether this marker's `type` matches, ignoring case. The corpus is consistent
    /// about capitalisation and a map from elsewhere has no reason to be.
    [[nodiscard]] bool isType(std::string_view wanted) const noexcept;
};

/// Every marker a `_save.lua` declares, in the order the file lists them.
///
/// Positions come back in ELMOS, converted from the ogrids the file stores, so they are
/// interchangeable with everything else in the engine.
///
/// A marker with no position is SKIPPED rather than fatal, unlike an army marker
/// without one: the table holds kinds this reader knows nothing about, and refusing a
/// whole map because one annotation is shaped unexpectedly would trade a working map
/// for a pedantic one. An `ARMY_<n>` marker missing its position is still an error,
/// because that one is load-bearing.
[[nodiscard]] std::expected<std::vector<Marker>, lua::ParseError> loadMarkers(
    std::string_view lua);

[[nodiscard]] std::expected<std::vector<mapinfo::StartPosition>, lua::ParseError>
loadStartPositions(std::string_view lua);

[[nodiscard]] std::expected<std::vector<mapinfo::StartPosition>, lua::ParseError>
loadStartPositionsFile(const std::filesystem::path& path);

/// Locates the `_save.lua` belonging to a `.scmap`. Stock maps name it after the
/// map file (SCMP_009.scmap -> SCMP_009_save.lua); when that is absent, any
/// single `*_save.lua` in the same directory is taken, since a map directory
/// holds exactly one map.
[[nodiscard]] std::optional<std::filesystem::path> findSaveBesideMap(
    const std::filesystem::path& scmapPath);

} // namespace rm::scenario
