// Playability contracts for the retail unit corpus.
//
// The local Supreme Commander wiki mirror is WEB-tier evidence for what a unit is for; the
// shipped blueprints remain BP-R authority for the executable details. Each row below names a
// factory product and the minimum capability that makes its advertised role useful. Extending
// coverage to another factory or tier is data entry: add its products to the table and factory
// list, without writing another test harness.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/Weapon.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rm::unitdef::Role;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;

struct UnitCapability {
    std::string_view factory;
    std::string_view unit;
    Role role;
    bool auxiliaryBuilder = false;
};

// The complete roster produced by each retail T1 land factory. Unit names and roles were
// cross-checked against reference/supcom-wiki; ids and build membership come from units.scd.
constexpr std::array kT1LandUnits{
    UnitCapability{"UEB0101", "UEL0101", Role::Scout},
    UnitCapability{"UEB0101", "UEL0103", Role::Artillery},
    UnitCapability{"UEB0101", "UEL0104", Role::AntiAir},
    UnitCapability{"UEB0101", "UEL0105", Role::Builder},
    UnitCapability{"UEB0101", "UEL0106", Role::Raider},
    UnitCapability{"UEB0101", "UEL0201", Role::Raider},

    UnitCapability{"UAB0101", "UAL0101", Role::Scout},
    UnitCapability{"UAB0101", "UAL0103", Role::Artillery},
    UnitCapability{"UAB0101", "UAL0104", Role::AntiAir},
    UnitCapability{"UAB0101", "UAL0105", Role::Builder},
    UnitCapability{"UAB0101", "UAL0106", Role::Raider},
    UnitCapability{"UAB0101", "UAL0201", Role::Raider},

    UnitCapability{"URB0101", "URL0101", Role::Scout},
    UnitCapability{"URB0101", "URL0103", Role::Artillery},
    UnitCapability{"URB0101", "URL0104", Role::AntiAir},
    UnitCapability{"URB0101", "URL0105", Role::Builder},
    UnitCapability{"URB0101", "URL0106", Role::Raider},
    UnitCapability{"URB0101", "URL0107", Role::Raider, true},

    UnitCapability{"XSB0101", "XSL0101", Role::Scout},
    UnitCapability{"XSB0101", "XSL0103", Role::Artillery},
    UnitCapability{"XSB0101", "XSL0104", Role::AntiAir},
    UnitCapability{"XSB0101", "XSL0105", Role::Builder},
    UnitCapability{"XSB0101", "XSL0201", Role::Raider},
};

constexpr std::array<std::string_view, 4> kT1LandFactories{
    "UEB0101", "UAB0101", "URB0101", "XSB0101"};

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

[[nodiscard]] std::filesystem::path blueprintPath(std::string_view id) {
    const std::string name{id};
    return unitRoot() / name / (name + "_unit.bp");
}

[[nodiscard]] bool hasFiringWeapon(const UnitDef& unit, bool airborne) {
    return std::ranges::any_of(unit.weapons, [airborne](const Weapon& weapon) {
        return weapon.fires() && weapon.canTarget(airborne);
    });
}

void checkPlayableCapability(const UnitDef& unit, Role role) {
    CHECK(rm::unitdef::roleOf(unit) == role);

    switch (role) {
    case Role::Scout:
        CHECK(unit.visionRadiusElmos > 0.0f);
        CHECK(unit.radarRadiusElmos > 0.0f);
        break;
    case Role::Builder:
        CHECK(unit.isBuilder());
        CHECK_FALSE(unit.buildableCategory.empty());
        break;
    case Role::AntiAir:
        CHECK(hasFiringWeapon(unit, true));
        break;
    case Role::Artillery:
    case Role::Raider:
        CHECK(hasFiringWeapon(unit, false));
        break;
    default:
        FAIL("T1 land capability table contains a role without a playability contract");
    }
}

} // namespace

TEST_CASE("every retail T1 land factory product has its playable core capability",
          "[corpus][capability]") {
    if (!std::filesystem::exists(blueprintPath(kT1LandFactories.front()))) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }

    std::vector<UnitDef> units;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{unitRoot()}) {
        if (!entry.is_regular_file()
            || !entry.path().filename().string().ends_with("_unit.bp")) {
            continue;
        }
        const auto unit = rm::unitbp::loadFile(entry.path());
        REQUIRE(unit.has_value());
        units.push_back(*unit);
    }
    REQUIRE(units.size() == 568);

    const auto findUnit = [&units](std::string_view id) {
        return std::ranges::find(units, id, &UnitDef::name);
    };

    for (const UnitCapability& expected : kT1LandUnits) {
        INFO(expected.unit);
        const auto unit = findUnit(expected.unit);
        REQUIRE(unit != units.end());

        CHECK(unit->name == expected.unit);
        CHECK(unit->hasCategory("MOBILE"));
        CHECK(unit->hasCategory("LAND"));
        CHECK(unit->hasCategory("TECH1"));
        CHECK(unit->hasCategory("BUILTBYTIER1FACTORY"));
        CHECK(unit->speedElmosPerSecond > 0.0f);
        CHECK(unit->visionRadiusElmos > 0.0f);
        CHECK(unit->buildTime > rm::sim::Mag{});
        checkPlayableCapability(*unit, expected.role);
        if (expected.auxiliaryBuilder) {
            CHECK(unit->isBuilder());
        }
    }

    // Match each factory's complete result, not only the expected inclusions. A new retail
    // product or a category-parser regression therefore makes the table ask for a conscious
    // capability row instead of silently escaping coverage.
    for (const std::string_view factoryId : kT1LandFactories) {
        INFO(factoryId);
        const auto factory = findUnit(factoryId);
        REQUIRE(factory != units.end());

        std::vector<std::string_view> actual;
        std::vector<std::string_view> expected;
        for (const UnitDef& candidate : units) {
            if (candidate.hasCategory("TECH1") && candidate.hasCategory("MOBILE")
                && candidate.hasCategory("LAND")
                && rm::unitdef::matchesExpression(factory->buildableCategory, candidate)) {
                actual.push_back(candidate.name);
            }
        }
        for (const UnitCapability& candidate : kT1LandUnits) {
            if (candidate.factory == factoryId) {
                expected.push_back(candidate.unit);
            }
        }
        std::ranges::sort(actual);
        std::ranges::sort(expected);
        CHECK(actual == expected);
    }
}
