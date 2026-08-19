#include "core/unit/UnitDef.hpp"

#include "core/sim/Movement.hpp"

#include <algorithm>
#include <cctype>
#include <numbers>
#include <system_error>
#include <utility>

namespace {

/// Circle divisions in a full turn — SPRING_MAX_HEADING << 1
/// (rts/System/SpringMath.h:16-17). Turn rates are authored in these per frame.
constexpr float kCircleDivisions = 65536.0f;

/// Reads a number, or leaves the default alone. Absence is ordinary: a building
/// has no speed, and most definitions omit most fields.
[[nodiscard]] float numberOr(const rm::lua::Value& table, std::string_view key,
                             float fallback) noexcept {
    const std::optional<double> value = table.numberAt(key);
    return value ? static_cast<float>(*value) : fallback;
}

} // namespace

namespace rm::unitdef {

float UnitDef::footprintRadiusElmos() const noexcept {
    const int squares = std::max(footprintSquaresX, footprintSquaresZ);
    return 0.5f * static_cast<float>(squares) * static_cast<float>(kSquareSize);
}

std::optional<MotionType> motionTypeFromName(std::string_view name) noexcept {
    // Spelled out rather than derived from the string's tail, because the names
    // do not disambiguate on any suffix: Amphibious is a prefix of
    // AmphibiousFloating, and a `starts_with` chain would read the longer one as
    // the shorter and quietly let a floating unit sink.
    struct Named {
        std::string_view name;
        MotionType type;
    };
    static constexpr Named kNames[] = {
        {"RULEUMT_None", MotionType::None},
        {"RULEUMT_Land", MotionType::Land},
        {"RULEUMT_Air", MotionType::Air},
        {"RULEUMT_Water", MotionType::Water},
        {"RULEUMT_Hover", MotionType::Hover},
        {"RULEUMT_Amphibious", MotionType::Amphibious},
        {"RULEUMT_AmphibiousFloating", MotionType::AmphibiousFloating},
        {"RULEUMT_SurfacingSub", MotionType::SurfacingSub},
    };
    for (const Named& known : kNames) {
        if (known.name == name) {
            return known.type;
        }
    }
    return std::nullopt;
}

bool travelsOnGround(MotionType motion) noexcept {
    switch (motion) {
        case MotionType::Land:
        case MotionType::Hover:
        case MotionType::Amphibious:
        case MotionType::AmphibiousFloating:
            return true;
        case MotionType::None:
        case MotionType::Air:
        case MotionType::Water:
        case MotionType::SurfacingSub:
            return false;
    }
    return false;  // an enum value from outside the set is not a licence to walk
}

std::expected<std::vector<UnitDef>, lua::ParseError> loadFileAll(
    const std::filesystem::path& path) {
    auto parsed = lua::parseTableFile(path.string());
    if (!parsed) {
        return std::unexpected{parsed.error()};
    }

    // Every top-level entry must be a table — a file maps unit NAMES to unit
    // definitions. That one rule does all the discriminating: a file which
    // generates its units leaves a plain settings table as its first literal,
    // whose entries are scalars, and is refused rather than yielding a unit
    // called "maxacc".
    if (!parsed->isTable() || parsed->fields.empty()) {
        return std::unexpected{lua::ParseError{
            "expected `return { name = { ... } }`; found no named entries", 0}};
    }

    std::vector<UnitDef> defs;
    defs.reserve(parsed->fields.size());

    for (const lua::Field& entry : parsed->fields) {
        // A unit with no name is not a unit. BAR has one file whose first
        // literal is a template keyed by the empty string — `[""] = { ... }` —
        // with the real units built from it further down.
        if (entry.key.empty()) {
            return std::unexpected{
                lua::ParseError{"a unit definition entry has an empty name", 0}};
        }
        if (!entry.value.isTable()) {
            return std::unexpected{lua::ParseError{
                "entry \"" + entry.key + "\" is not a unit definition table", 0}};
        }

        const lua::Value& table = entry.value;

        UnitDef def;
        def.name = entry.key;
        if (const std::optional<std::string_view> object = table.stringAt("objectname")) {
            def.modelPath = std::string{*object};
        }

        def.speedElmosPerSecond = numberOr(table, "speed", 0.0f);

        // Circle divisions per frame to radians per second.
        const float turnRate = numberOr(table, "turnrate", 0.0f);
        def.turnRateRadiansPerSecond = turnRate / kCircleDivisions * 2.0f
                                     * std::numbers::pi_v<float>
                                     * static_cast<float>(sim::kTicksPerSecond);

        def.maxSlopeDegrees = numberOr(table, "maxslope", 0.0f);
        def.maxWaterDepthElmos = numberOr(table, "maxwaterdepth", 0.0f);
        def.health = numberOr(table, "health", 0.0f);

        if (const lua::Value* flies = table.find("canfly")) {
            def.canFly = flies->asBoolean().value_or(false);
        }

        // SPRING_FOOTPRINT_SCALE, applied here so nothing downstream has to
        // know that the file's units are half of the engine's
        // (UnitDef.cpp:671-672). The lower clamp is the engine's too, and it
        // earns its keep: several BAR definitions declare `footprintx = 0`,
        // which without the max would be a unit occupying no space at all.
        constexpr int kFootprintScale = 2;
        const auto footprint = [&table, kFootprintScale](std::string_view key) {
            return std::max(kFootprintScale,
                            static_cast<int>(numberOr(table, key, 1.0f)) * kFootprintScale);
        };
        def.footprintSquaresX = footprint("footprintx");
        def.footprintSquaresZ = footprint("footprintz");
        def.collisionRadiusElmos = def.footprintRadiusElmos();

        // BAR does not name a motion class, so it is inferred from the fields it
        // does state. Coarser than the eight classes the other family
        // distinguishes — nothing here says "ship" — but it makes the same
        // question answerable of both, which is the point.
        def.motion = def.canFly    ? MotionType::Air
                     : def.isMobile() ? MotionType::Land
                                      : MotionType::None;

        defs.push_back(std::move(def));
    }

    return defs;
}

std::expected<UnitDef, lua::ParseError> loadFile(const std::filesystem::path& path) {
    auto all = loadFileAll(path);
    if (!all) {
        return std::unexpected{all.error()};
    }
    return all->front();
}

std::filesystem::path resolveModel(const vfs::AssetSearch& search,
                                   std::string_view objectName) {
    if (objectName.empty() || search.empty()) {
        return {};
    }

    // Definitions name a path (`Units/ARMPW.s3o`) whose case matches neither the
    // file nor, always, the directory it sits in. Try the conventional
    // `objects3d/<name>` path first, then fall back to a case-insensitive
    // basename search across all roots.
    const std::filesystem::path objectPath{objectName};
    if (std::filesystem::path candidate = search.resolve(std::filesystem::path{"objects3d"}
                                                         / objectPath);
        !candidate.empty()) {
        return candidate;
    }

    return search.resolveByName(objectPath.filename().string());
}

} // namespace rm::unitdef
