#include "core/unit/UnitBlueprint.hpp"

#include "core/blueprint/BlueprintMesh.hpp"
#include "core/map/Scmap.hpp"
#include "core/sim/Pathfinding.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <fstream>
#include <string>
#include <system_error>

namespace rm::unitbp {
namespace {

/// How steep a Supreme Commander ground unit may climb, and how deep it may
/// wade, per motion class.
///
/// THESE ARE OURS, and they have to be: a `.bp` states no slope limit and no
/// wading depth for any of the 568 units. See ADR-027 for the reasoning, the
/// alternatives rejected, and what is still missing. The only slope figure the format
/// carries anywhere is `Footprint.MaxSlope`, a GRADIENT rather than an angle —
/// 0.25 in 57 blueprints and 0.5 in one, all of them aircraft, where it bounds
/// the ground an aeroplane may land on rather than the ground a tank may climb.
///
/// So the limit is the engine's own, and the number is already here:
/// `sim::kDefaultMaxSlopeDegrees` of 17 means 25.5 real degrees once the engine's
/// 1.5 factor is applied (Pathfinding.hpp:37-44), which is a gradient of 0.48.
/// That sits sensibly either side of the game's own figures — steeper than the
/// 0.25 an aircraft needs to land on, shallower than the 1.0 of a wall — and it
/// is what every BAR ground unit in the corpus authorises to within a degree or
/// two, so the two content families agree without either being bent to fit.
/// Nothing is invented that the engine did not already assume.
///
/// Depth is the part the class really decides, and there the classes differ in
/// kind rather than degree.
struct GroundLimits {
    float maxSlopeDegrees;     ///< in the authored 0..60 scale, NOT real degrees
    float maxWaterDepthElmos;  ///< 0 means "does not enter water at all"
};

/// Deep enough that no stock map's sea floor exceeds it, for the classes that
/// travel under or across water without caring how deep it is.
///
/// A number rather than an infinity because it is compared against a height
/// difference, and `inf` would make a NaN out of any map whose water level and
/// terrain happened to agree exactly. The stock maps' deepest water is under 400
/// elmos, so this is not a limit any of them reach.
constexpr float kAnyDepthElmos = 100000.0f;

[[nodiscard]] GroundLimits limitsFor(unitdef::MotionType motion) noexcept {
    switch (motion) {
        // Ground only. Water of ANY depth is out — Supreme Commander's land units
        // do not ford, which is the sharpest difference from BAR, where a wading
        // depth of 12 elmos is the norm and a shoreline is passable.
        case unitdef::MotionType::Land:
            return {sim::kDefaultMaxSlopeDegrees, 0.0f};

        // Over the ground, and over the water's surface, so depth is irrelevant
        // rather than generous.
        case unitdef::MotionType::Hover:
        case unitdef::MotionType::AmphibiousFloating:
        // Along the seabed, so it is the SLOPE of the sea floor that decides,
        // which is the same test with the depth limit lifted.
        case unitdef::MotionType::Amphibious:
            return {sim::kDefaultMaxSlopeDegrees, kAnyDepthElmos};

        // Not ground movers at all; `travelsOnGround` is what callers should ask.
        // Zeroes rather than a guess, so a caller that ignores that and builds a
        // grid anyway gets an empty one instead of a ship driving inland.
        case unitdef::MotionType::None:
        case unitdef::MotionType::Air:
        case unitdef::MotionType::Water:
        case unitdef::MotionType::SurfacingSub:
            return {0.0f, 0.0f};
    }
    return {0.0f, 0.0f};
}

/// A game-relative path made joinable onto a real root. Same reason the prop
/// reader has one: a leading slash makes the join resolve to the filesystem root
/// and throw the root away.
[[nodiscard]] std::string withoutLeadingSlash(std::string_view gameRelativePath) {
    std::string relative{gameRelativePath};
    if (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.erase(0, 1);
    }
    return relative;
}

[[nodiscard]] float numberOr(const lua::Value& table, std::string_view key,
                             float fallback) noexcept {
    const std::optional<double> value = table.numberAt(key);
    return value ? static_cast<float>(*value) : fallback;
}

/// The unit id a blueprint's file name carries: `UEL0201_unit.bp` -> `UEL0201`.
///
/// The file name is the authority because nothing inside is: not one of the 568
/// blueprints has a `BlueprintId`, and `General.UnitName` is display text
/// ("Mech Marine") present in only 306 of them. The id is what the game keys on
/// and what every reference to a unit elsewhere in the content spells.
[[nodiscard]] std::string idFromFileName(std::string_view path) {
    const std::string stem = std::filesystem::path{path}.stem().string();
    if (stem.size() > blueprint::kUnitSuffix.size()) {
        return stem.substr(0, stem.size() - blueprint::kUnitSuffix.size());
    }
    return stem;
}

} // namespace

std::expected<unitdef::UnitDef, lua::ParseError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected{lua::ParseError{"cannot open \"" + path.string() + "\"", 0}};
    }
    const std::string source{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return load(source, path.string());
}

std::expected<unitdef::UnitDef, lua::ParseError> load(std::string_view source,
                                                      std::string_view vfsPath) {
    auto parsed = lua::parseTable(source);
    if (!parsed) {
        return std::unexpected{parsed.error()};
    }
    if (!parsed->isTable() || parsed->fields.empty()) {
        return std::unexpected{
            lua::ParseError{"expected `UnitBlueprint { ... }` with named entries", 0}};
    }

    unitdef::UnitDef def;
    def.name = idFromFileName(vfsPath);

    // --- physics -----------------------------------------------------------
    //
    // The one table whose absence is a real failure. Every shipped blueprint has
    // it, and without a MotionType there is nothing to say about where the unit
    // goes — which is a different thing from a unit that cannot move.
    const lua::Value* physics = parsed->path("Physics");
    if (physics == nullptr) {
        return std::unexpected{lua::ParseError{
            "unit blueprint \"" + def.name + "\" states no Physics table", 0}};
    }

    const std::optional<std::string_view> motionName = physics->stringAt("MotionType");
    if (!motionName) {
        return std::unexpected{lua::ParseError{
            "unit blueprint \"" + def.name + "\" states no Physics.MotionType", 0}};
    }
    const std::optional<unitdef::MotionType> motion = unitdef::motionTypeFromName(*motionName);
    if (!motion) {
        return std::unexpected{lua::ParseError{
            "unit blueprint \"" + def.name + "\" has an unknown MotionType \""
                + std::string{*motionName} + "\"", 0}};
    }
    def.motion = *motion;
    def.canFly = (def.motion == unitdef::MotionType::Air);

    // Ogrids per second to elmos per second. `MaxSpeed` is present in exactly the
    // 196 blueprints that move — `RULEUMT_None` accounts for 374 of the rest —
    // so its absence is a building rather than a missing field, and zero is the
    // honest reading.
    def.speedElmosPerSecond = numberOr(*physics, "MaxSpeed", 0.0f) * scmap::kElmosPerOgrid;

    // Degrees per second to radians. No 65536 and no tick rate: this family
    // authors the quantity in the unit a person would use, which is worth noting
    // precisely because the BAR loader beside it needs three constants for the
    // same field.
    def.turnRateRadiansPerSecond = numberOr(*physics, "TurnRate", 0.0f)
                                 * std::numbers::pi_v<float> / 180.0f;

    const GroundLimits limits = limitsFor(def.motion);
    def.maxSlopeDegrees = limits.maxSlopeDegrees;
    def.maxWaterDepthElmos = limits.maxWaterDepthElmos;

    // --- size --------------------------------------------------------------
    //
    // `SizeX`/`SizeY`/`SizeZ` sit at the file's ROOT, not under `Footprint`, and
    // all 568 state them. They are the collision box in ogrids and they are
    // fractional — a medium tank is 0.7 x 0.9 — so the radius keeps that
    // precision rather than being rounded into squares.
    const float sizeX = numberOr(*parsed, "SizeX", 0.0f);
    const float sizeZ = numberOr(*parsed, "SizeZ", 0.0f);
    def.collisionRadiusElmos = 0.5f * std::max(sizeX, sizeZ) * scmap::kElmosPerOgrid;

    // The BUILD footprint is a separate, whole-square field, and only 363 of the
    // 568 have one — essentially the structures, which are what occupies a grid.
    // For the rest it is derived from the collision size, rounded UP so that a
    // unit never claims less ground than it stands on, and floored at one square
    // because a unit occupying no squares at all would be placeable inside a wall.
    const lua::Value* footprint = parsed->path("Footprint");
    const auto squaresFrom = [](float ogrids) {
        return std::max(1, static_cast<int>(std::ceil(ogrids)));
    };
    def.footprintSquaresX = footprint != nullptr && footprint->numberAt("SizeX")
                              ? squaresFrom(numberOr(*footprint, "SizeX", 1.0f))
                              : squaresFrom(sizeX);
    def.footprintSquaresZ = footprint != nullptr && footprint->numberAt("SizeZ")
                              ? squaresFrom(numberOr(*footprint, "SizeZ", 1.0f))
                              : squaresFrom(sizeZ);

    // --- economy -----------------------------------------------------------
    if (const lua::Value* economy = parsed->path("Economy")) {
        def.buildCostMass = numberOr(*economy, "BuildCostMass", 0.0f);
        def.buildCostEnergy = numberOr(*economy, "BuildCostEnergy", 0.0f);
        def.buildTime = numberOr(*economy, "BuildTime", 0.0f);
        def.buildRate = numberOr(*economy, "BuildRate", 0.0f);
        def.producesMassPerSecond = numberOr(*economy, "ProductionPerSecondMass", 0.0f);
        def.producesEnergyPerSecond = numberOr(*economy, "ProductionPerSecondEnergy", 0.0f);
        def.upkeepEnergyPerSecond =
            numberOr(*economy, "MaintenanceConsumptionPerSecondEnergy", 0.0f);
        def.storageMass = numberOr(*economy, "StorageMass", 0.0f);
        def.storageEnergy = numberOr(*economy, "StorageEnergy", 0.0f);
    }

    // --- weapons -----------------------------------------------------------
    //
    // A Lua ARRAY, so the entries are positional. 247 of the 568 units carry one;
    // `Weapon::fires()` is what decides which entries are guns, because 99 of the 494
    // are a unit's own death explosion.
    if (const lua::Value* weapons = parsed->path("Weapon")) {
        def.weapons = unitdef::weaponsFrom(*weapons);
    }

    // --- the rest ----------------------------------------------------------
    if (const lua::Value* defense = parsed->path("Defense")) {
        def.health = numberOr(*defense, "MaxHealth", 0.0f);
    }

    // --- display -----------------------------------------------------------
    //
    // `modelPath` holds what the file SAYS, unresolved, exactly as it holds BAR's
    // `objectname` — so it is `MeshName` when there is one and empty when the
    // mesh is found by convention instead. `resolveMesh` turns either into a path.
    if (const lua::Value* display = parsed->path("Display")) {
        // Scale, in the same place a prop states it. 567 of 568 say so; the one
        // that does not keeps the default of 1, which is what "no scaling stated"
        // means. Two state ZERO, which is faithfully a mesh scaled to nothing —
        // they are unused Seraphim civilian entries, and inventing a 1 for them
        // would be inventing content.
        def.meshToElmos = numberOr(*display, "UniformScale", 1.0f) * scmap::kElmosPerOgrid;

        if (const lua::Value* lods = display->path("Mesh", "LODs");
            lods != nullptr && !lods->items.empty()) {
            if (const std::optional<std::string_view> named = lods->items.front().stringAt("MeshName")) {
                def.modelPath = std::string{*named};
            }
        }
    }

    return def;
}

std::string resolveMeshInVfs(const unitdef::UnitDef& def, std::string_view blueprintVfsPath,
                             const vfs::Vfs& content, std::size_t level) {
    // A `MeshName` is a VFS path already, so it needs no root and no joining —
    // which is what it was all along. Only the filesystem form has to be told where
    // the game is, because an extracted tree is not the game's own namespace.
    const std::string candidate =
        def.modelPath.empty()
            ? blueprint::meshBeside(blueprintVfsPath, blueprint::kUnitSuffix, level)
                  .generic_string()
            : def.modelPath;

    if (candidate.empty() || !content.contains(candidate)) {
        return {};
    }
    return candidate;
}

std::filesystem::path resolveMesh(const unitdef::UnitDef& def,
                                  const std::filesystem::path& blueprintPath,
                                  const std::filesystem::path& root) {
    const std::filesystem::path candidate = [&]() -> std::filesystem::path {
        if (def.modelPath.empty()) {
            // The common case, 530 of 568: no MeshName, so the file name is the
            // whole of the lookup.
            return blueprint::meshBeside(blueprintPath, blueprint::kUnitSuffix, 0);
        }

        // A path inside the game's virtual filesystem, so it begins with a slash
        // that has to go before joining — or the result resolves to the real
        // filesystem root and silently discards `root`, which is one of the few
        // filesystem mistakes that yields a plausible path rather than an error.
        // Without a root there is nothing to join it to.
        if (root.empty()) {
            return {};
        }
        return root / withoutLeadingSlash(def.modelPath);
    }();

    std::error_code ec;
    if (candidate.empty() || !std::filesystem::is_regular_file(candidate, ec)) {
        return {};
    }
    return candidate;
}

} // namespace rm::unitbp
