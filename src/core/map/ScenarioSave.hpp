#pragma once

#include "core/lua/LuaTable.hpp"
#include "core/map/MapInfo.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
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

/// One unit a `_save.lua` pre-places — `C-273`'s unit-tree spawn input.
/// `Scenario.Armies.<ARMY_n>.Units` is a tree of `GROUP` tables whose leaves
/// carry `type` (a blueprint id like 'ueb5101') and `Position` (a 3-vector in
/// ogrids). The tree is walked recursively; groups are structure, not units.
struct SavedUnit {
    std::string army;                 ///< the ARMY_<n> key it belongs to
    std::string type;                 ///< blueprint id, lowercase as authored
    std::array<float, 3> position{};  ///< elmos, converted from ogrids
    std::array<float, 3> orientation{};
};

/// Every pre-placed unit a `_save.lua` declares, across every army.
///
/// Stock skirmish maps ship empty `Units` trees — the armies exist, the
/// `INITIAL` group is empty — so an empty result is ordinary, not an error.
/// A malformed tree (a unit entry without a type or position) fails loudly:
/// silently dropping a pre-placed army is the bug this reader exists to
/// prevent.
[[nodiscard]] std::expected<std::vector<SavedUnit>, lua::ParseError>
loadArmyUnits(std::string_view lua);

/// The `ScenarioInfo.Options` table of a `<map>_scenario.lua`, verbatim.
///
/// WHY A MAP AND NOT A STRUCT. The table is open-ended: the engine itself reads
/// `InitialEnergy`/`InitialMass`/`InitialResearch`/`Difficulty`/`UnitCap`/
/// `PreBuiltUnits`/`NoRushOption`/`NoRushRadius`/`FogOfWar`/`TeamLock` with
/// defaults at session create (`0x8e7fef`-`0x8e809b`), `victory.lua` reads
/// `Victory`, `SimUtils.lua` reads `DoNotShareUnitCap`, `aibrain.lua` reads
/// `TeamSpawn`, and a mod can add its own. A struct would silently drop every
/// key it did not name; the map keeps the file's own spelling for all of them.
///
/// Every value is stored as TEXT — numbers and booleans flattened to their
/// source spelling — because the consumers compare strings (`'explored'`,
/// `'demoralization'`) or `tonumber()` them, and both directions survive a
/// string. Nested tables are skipped: no known consumer reads one.
///
/// EMPTY IS ORDINARY: no stock `_scenario.lua` declares `Options` at all — the
/// lobby writes it into `ScenarioInfo` at session create, so a map loaded
/// without a lobby has none. Callers treat absent keys as "the lobby default",
/// which is what the engine's own defaults encode.
struct ScenarioOptions {
    std::map<std::string, std::string, std::less<>> values;

    /// The option's stored spelling, or nullopt when the table does not name it.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const noexcept;

    /// Whether `key` is stored and its value equals `expected`, ignoring case —
    /// the lobby writes lowercase and a scenario author has no reason to agree.
    [[nodiscard]] bool is(std::string_view key, std::string_view expected) const noexcept;
};

/// Reads `ScenarioInfo.Options` out of a `<map>_scenario.lua` source.
///
/// The first table literal in the file is `ScenarioInfo`'s value — the same
/// convention `loadMarkers` relies on for `Scenario` — so this is a lookup, not
/// a walk. A file with no `Options` table is NOT an error: it is every stock
/// skirmish map, and the answer is an empty `ScenarioOptions`.
[[nodiscard]] std::expected<ScenarioOptions, lua::ParseError> loadScenarioOptions(
    std::string_view lua);

/// Locates the `_scenario.lua` belonging to a `.scmap`, the same convention as
/// `findSaveBesideMap`: `SCMP_009.scmap` -> `SCMP_009_scenario.lua`, else any
/// single `*_scenario.lua` in the directory.
[[nodiscard]] std::optional<std::filesystem::path> findScenarioBesideMap(
    const std::filesystem::path& scmapPath);

} // namespace rm::scenario
