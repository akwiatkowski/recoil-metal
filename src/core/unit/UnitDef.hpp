#pragma once

#include "core/lua/LuaTable.hpp"
#include "core/vfs/AssetSearch.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace rm::unitdef {

// What a unit *is*, read from the game's own unit definitions rather than
// hardcoded here.
//
// Until now the sim moved everything at BAR's Pawn's speed, because one unit
// had been read by hand. A BAR unit definition is a flat Lua data table —
//
//     return { armpw = { speed = 87, maxslope = 17, objectname = "Units/ARMPW.s3o", ... } }
//
// — which is exactly what core/lua parses, so this is a data slice rather than
// a behavioural one, and it follows the same rule as every other loader here:
// formats convert, behaviour gets reimplemented.
//
// Only the fields this engine can act on are read. A definition carries dozens
// more (weapons, build options, costs, categories) and there is nothing to do
// with them yet; adding a field here should mean something reads it.
struct UnitDef {
    std::string name;       ///< the table's own key, e.g. "armpw"
    std::string modelPath;  ///< `objectname`, e.g. "Units/ARMPW.s3o"

    /// Elmos per second. Recoil's modern `speed` field is already per second;
    /// only the legacy `maxVelocity` is per frame (UnitDef.cpp:442-443).
    float speedElmosPerSecond = 0.0f;

    /// Radians per second, converted out of the circle divisions per frame the
    /// file is authored in — `turnrate / 65536 * 2*pi * 30`
    /// (SpringMath.h:16-17, GlobalConstants.h:52).
    float turnRateRadiansPerSecond = 0.0f;

    /// As authored, in the degrees the passability grid expects.
    float maxSlopeDegrees = 0.0f;
    float maxWaterDepthElmos = 0.0f;

    /// Footprint in heightmap SQUARES, already scaled by the engine's
    /// SPRING_FOOTPRINT_SCALE of 2 (UnitDef.cpp:671-672, GlobalConstants.h:17)
    /// — so a `footprintx` of 2 in the file is 4 squares, or 32 elmos, here.
    int footprintSquaresX = 0;
    int footprintSquaresZ = 0;

    float health = 0.0f;

    /// Whether this unit flies.
    ///
    /// Worth carrying because an aircraft is not a ground unit with wings: BAR's
    /// flyers set `canfly` and carry a `turnradius` INSTEAD of a `turnrate`, so
    /// they have no turn rate to read at all. They also have no business being
    /// routed over a ground passability grid.
    bool canFly = false;

    /// Whether this definition describes something that moves. Buildings share
    /// the format and simply have no speed, and treating one as a stationary
    /// unit with a zero turn rate is more useful than rejecting the file.
    [[nodiscard]] bool isMobile() const noexcept { return speedElmosPerSecond > 0.0f; }

    /// Footprint radius in elmos — half the larger side. What a collision test
    /// wants, and the one derived quantity worth keeping next to its source.
    [[nodiscard]] float footprintRadiusElmos() const noexcept;
};

/// Reads every unit a `.lua` definition file declares.
///
/// Usually one, keyed by name — `return { armpw = { ... } }` — so the name comes
/// from the file's CONTENTS rather than its filename; the two agree throughout
/// BAR, but only one of them is what the engine keys on. Twenty-six BAR files
/// declare several units at once, which is why this returns a list.
///
/// Refuses a file whose top-level table mixes in non-table entries. That is the
/// shape of the thirteen BAR files which BUILD their units in a loop
/// (`local def = { maxacc = 0, ... }` and so on) — Lua programs, which this
/// reader is documented not to evaluate. Without the check, the first key in
/// such a file ("maxacc") would be read as a unit's name.
[[nodiscard]] std::expected<std::vector<UnitDef>, lua::ParseError> loadFileAll(
    const std::filesystem::path& path);

/// The first unit a file declares — the common case, where there is only one.
[[nodiscard]] std::expected<UnitDef, lua::ParseError> loadFile(const std::filesystem::path& path);

/// Finds the model file a definition names, searching the asset roots.
///
/// Resolution is by BASENAME and case-insensitive: definitions say
/// `Units/ARMPW.s3o` while the file on disk is `armpw.s3o`, and the
/// subdirectory in the name does not always match the one it lives in either.
/// Returns an empty path when nothing matches.
[[nodiscard]] std::filesystem::path resolveModel(const vfs::AssetSearch& search,
                                                 std::string_view objectName);

} // namespace rm::unitdef
