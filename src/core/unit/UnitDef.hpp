#pragma once

#include "core/lua/LuaTable.hpp"
#include "core/unit/Weapon.hpp"
#include "core/vfs/AssetSearch.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unitdef {

// What a unit moves through, and this engine's authority on where it may go.
//
// Supreme Commander's own names, because it is the family that HAS this concept:
// a `.bp` states `Physics.MotionType = 'RULEUMT_Land'` and states no slope or
// wading limit at all, so the class is the whole of what the file says about
// passability. All 568 shipped unit blueprints carry one, and these eight values
// are the complete set (None 374, Air 60, Land 50, Water 27, Hover 19,
// Amphibious 17, SurfacingSub 13, AmphibiousFloating 8).
//
// BAR definitions describe the same thing differently — a per-unit `maxslope`
// and `maxwaterdepth`, plus `canfly` — so its loader fills this in from what it
// does say, and everything downstream asks one question of both families.
enum class MotionType : std::uint8_t {
    None,                ///< immobile. Buildings, and 374 of the 568 blueprints
    Land,                ///< ground only; water of any depth is out
    Air,                 ///< flies, and is not subject to a ground grid at all
    Water,               ///< surface ships — water ONLY, and cannot come ashore
    Hover,               ///< ground and water surface alike
    Amphibious,          ///< ground, and the seabed underneath the water
    AmphibiousFloating,  ///< ground, and floating on the surface
    SurfacingSub,        ///< submarines: submerged, surfacing to fire
};

/// The motion type a `.bp`'s `RULEUMT_*` string names, or nullopt for a string
/// this does not know. Nothing in the retail corpus is unknown — the check
/// exists so a mod that invents a class says so rather than being read as
/// immobile, which would look like a unit that simply refuses to move.
[[nodiscard]] std::optional<MotionType> motionTypeFromName(std::string_view name) noexcept;

/// Whether a motion type moves over ground the passability grid describes.
///
/// The grid answers "may a ground unit stand here", so this is the question of
/// whether that grid is the right tool at all. False for `Air` (no ground
/// involved), for `None` (nothing moves), and for `Water` and `SurfacingSub` —
/// those two need the INVERSE of the grid, water deep enough rather than
/// shallow enough, which this engine cannot yet express. Routing a ship over the
/// ground grid would path it across dry land, so it is refused rather than
/// approximated.
[[nodiscard]] bool travelsOnGround(MotionType motion) noexcept;

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
    ///
    /// The BUILD-grid quantity: whole squares a unit occupies. A `.scmap` unit
    /// states this separately from its collision size and only when it has one
    /// (363 of 568, essentially the structures), so for the rest this is derived
    /// from the size below rather than read.
    int footprintSquaresX = 0;
    int footprintSquaresZ = 0;

    /// How much room the unit takes up, in elmos, for collision and separation.
    ///
    /// STORED rather than derived from the footprint squares above, because the
    /// two families disagree about whether the quantity is an integer. BAR's
    /// footprint is whole squares and this is half its larger side, exactly as
    /// before. Supreme Commander states `SizeX`/`SizeZ` in fractional ogrids —
    /// 418 of 568 are fractional and 154 are under a single ogrid, the smallest
    /// 0.01 — so rounding them into squares would inflate the smallest unit's
    /// radius from 0.04 elmos to 4, a hundredfold, and pack a crowd of them
    /// against a spacing none of them needs.
    float collisionRadiusElmos = 0.0f;

    /// What this unit moves through. See MotionType: for the Supreme Commander
    /// family it is read from the file, for BAR it is inferred from the fields
    /// that family does state.
    MotionType motion = MotionType::None;

    // --- economy -----------------------------------------------------------
    //
    // What it costs to make and what it makes. All four are stated by essentially every
    // shipped unit (718 state a cost, 703 a build time), which is why they are read
    // unconditionally rather than behind a check.

    /// What building this costs in total, and how many build units of work it takes.
    /// A builder's `buildRate` divided into `buildTime` gives the seconds.
    float buildCostMass = 0.0f;
    float buildCostEnergy = 0.0f;
    float buildTime = 0.0f;

    /// How fast this unit builds, in build units per second. 143 units state one — the
    /// commanders, engineers and factories. Zero means it cannot build.
    float buildRate = 0.0f;

    /// What it produces per second once standing. 37 units make mass and 33 make energy;
    /// a mass extractor is 2 a second, a power generator 20.
    float producesMassPerSecond = 0.0f;
    float producesEnergyPerSecond = 0.0f;

    /// What it costs to RUN, per second, once standing. Energy only: 104 units state
    /// `MaintenanceConsumptionPerSecondEnergy` and not one states a mass counterpart.
    ///
    /// A mass extractor burns 2 a second against the 2 mass it makes, so an economy that
    /// ignores this runs richer than the game's — and it is what makes power generation a
    /// decision rather than a formality.
    float upkeepEnergyPerSecond = 0.0f;

    /// How much of each it lets its owner hold. 62 units state storage.
    float storageMass = 0.0f;
    float storageEnergy = 0.0f;

    /// Whether this unit can build anything at all.
    [[nodiscard]] bool isBuilder() const noexcept { return buildRate > 0.0f; }

    /// What this unit shoots with. Empty for the 321 shipped units that shoot nothing,
    /// and for every BAR unit — that family states its weapons in a shape this engine
    /// does not read yet, and an empty list is the honest report of that.
    std::vector<Weapon> weapons;

    /// The unit's longest reach, in elmos, over the weapons it will actually fire.
    /// Zero for something unarmed. What a targeting sweep needs before it looks at
    /// individual weapons.
    [[nodiscard]] float maxWeaponRangeElmos() const noexcept;

    /// The factor that takes the MESH's own coordinates to elmos.
    ///
    /// One number holding what is two conversions in the file, deliberately.
    /// A `.s3o` is authored in elmos already, so BAR's is 1. A `.scm` is authored
    /// in whatever the artist used and the blueprint's `Display.UniformScale`
    /// takes it to OGRIDS, which are 8 elmos each — so a medium tank's 0.07 is
    /// really 0.56, and applying only the first step leaves it an eighth of its
    /// size. That mistake has already been made once here, on the props
    /// (AGENT.md), and it presents as a scatter of specks rather than as a scale
    /// bug. Combining the two removes the chance to make it again.
    ///
    /// Cross-checked against the mesh: UEL0201's geometry is 8.09 x 11.93 units,
    /// which at 0.07 is 0.57 x 0.84 ogrids against the 0.7 x 0.9 collision box the
    /// same blueprint declares — a box slightly larger than the model it holds,
    /// which is what a collision box should be.
    float meshToElmos = 1.0f;

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

    /// Footprint radius in elmos — half the larger side.
    ///
    /// The BAR derivation, kept because it is how that family's `collisionRadiusElmos`
    /// is arrived at and a test asserts the two agree. Prefer the stored radius:
    /// this one has no answer for a unit whose size was never whole squares.
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
