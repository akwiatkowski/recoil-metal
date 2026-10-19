// Supreme Commander unit blueprints, read into the shared UnitDef.
//
// The conversions are what this file is really about — ogrids to elmos, degrees
// to radians, a motion class to a slope limit — and every one of them is a place
// a plausible-looking wrong number can hide. The corpus test beside this one
// (test_real_unit_blueprints.cpp) checks all 568 shipped files; these pin the
// arithmetic against values worked out by hand.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Pathfinding.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "support/FxMatchers.hpp"

using Catch::Approx;
using rm::unitdef::MotionType;

TEST_CASE("malformed enhancement work data is rejected rather than silently omitted",
          "[unitbp][enhancement]") {
    const auto result = rm::unitbp::load(R"(
        UnitBlueprint { Physics={MotionType='RULEUMT_None'}, Enhancements = { Broken = {
            BuildCostMass=1, BuildCostEnergy=1, BuildTime='900', Slot='LCH'
        } } }
    )", "/units/TEST/TEST_unit.bp");
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("Broken") != std::string::npos);
}

namespace {

// Blueprints are read from disk rather than from a string, because that is the
// entry point the app uses and the file NAME carries the unit's id — there being
// no BlueprintId in any of the 568 shipped files. A temporary file is the honest
// way to test that.
class Blueprint {
public:
    explicit Blueprint(std::string fileName, std::string_view source)
        : path_{std::filesystem::temp_directory_path() / "rm_unitbp_test" / fileName} {
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream out{path_};
        out << source;
    }
    Blueprint(const Blueprint&) = delete;
    Blueprint& operator=(const Blueprint&) = delete;
    Blueprint(Blueprint&&) = delete;
    Blueprint& operator=(Blueprint&&) = delete;
    ~Blueprint() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// A medium tank's real numbers, from units/UEL0201/UEL0201_unit.bp.
constexpr std::string_view kMediumTank = R"(
UnitBlueprint {
    Defense = { MaxHealth = 300 },
    Display = { UniformScale = 0.07 },
    Physics = {
        MaxSpeed = 3.4,
        MotionType = 'RULEUMT_Land',
        TurnRate = 90,
    },
    SizeX = 0.7,
    SizeY = 0.55,
    SizeZ = 0.9,
}
)";

} // namespace

TEST_CASE("a unit blueprint's numbers arrive in the engine's own units") {
    const Blueprint bp{"UEL0201_unit.bp", kMediumTank};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    // The id is the FILE NAME with its suffix removed. Nothing inside says it.
    CHECK(def->name == "UEL0201");

    // 3.4 ogrids/s x 8 elmos/ogrid.
    CHECK(def->speedElmosPerSecond == Approx(27.2f));
    CHECK(def->isMobile());

    // 90 degrees/s in radians — no 65536 and no tick rate, unlike BAR's.
    CHECK(def->turnRateRadiansPerSecond == Approx(std::numbers::pi_v<float> / 2.0f));

    CHECK(rm::test::asFloat(def->health) == Approx(300.0f));
    CHECK(def->motion == MotionType::Land);
    CHECK_FALSE(def->canFly);

    // Half the larger side, 0.9 ogrids, in elmos: 0.5 * 0.9 * 8.
    CHECK(def->collisionRadiusElmos == Approx(3.6f));
    // The Cybran random-offset box keeps BOTH plan axes (0.7/0.9 ogrids ->
    // 5.6/7.2 elmos); the radius above keeps only the larger half.
    CHECK(def->sizeXElmos == Approx(5.6f));
    CHECK(def->sizeZElmos == Approx(7.2f));
    CHECK(def->sizeYElmos == Approx(4.4f));
    CHECK(def->collisionOffsetYElmos == Approx(0.0f));  // unauthored: pad level

    // 0.07 to ogrids and 8 to elmos, as ONE factor.
    CHECK(def->meshToElmos == Approx(0.56f));
}

TEST_CASE("the walk clip a blueprint declares is the one that plays") {
    // `Display.AnimationWalk` is a path, not a convention: bots whose cycles
    // are numbered (the UEF ACU's a001) are invisible to a name scan, so the
    // declaration is the only way they walk. The rate is the clip's playback
    // rate at top speed — the divisor of the stride the walk phase paces by.
    const Blueprint bp{"UEL0001_unit.bp", R"(
        UnitBlueprint {
            Display = {
                AnimationWalk = '/units/uel0001/uel0001_a001.sca',
                AnimationWalkRate = 1.7,
            },
            Physics = { MaxSpeed = 1.7, MotionType = 'RULEUMT_Land' },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->animationWalk == "/units/uel0001/uel0001_a001.sca");
    CHECK(def->animationWalkRate == Catch::Approx(1.7f));
}

TEST_CASE("a blueprint that declares no walk clip keeps the defaults") {
    const Blueprint bp{"UEL0201_unit.bp", kMediumTank};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->animationWalk.empty());
    CHECK(def->animationWalkRate == Catch::Approx(1.0f));
}

TEST_CASE("the display name arrives with its localisation tag stripped") {
    // `Description` is the generic type name — "Mass Extractor", "Gatling Bot" — present in
    // 567 of the 568 shipped blueprints, and the text after the `<LOC key>` prefix is the
    // built-in English fallback. That fallback is the name; no string database is needed.
    const Blueprint tagged{"UEB1103_unit.bp", R"(
UnitBlueprint {
    Description = '<LOC ueb1103_desc>Mass Extractor',
    Physics = { MotionType = 'RULEUMT_None' },
})"};
    const auto withTag = rm::unitbp::loadFile(tagged.path());
    REQUIRE(withTag.has_value());
    CHECK(withTag->description == "Mass Extractor");

    // A description with no tag at all is already the name.
    const Blueprint bare{"XXB0001_unit.bp", R"(
UnitBlueprint {
    Description = 'Test Structure',
    Physics = { MotionType = 'RULEUMT_None' },
})"};
    const auto withoutTag = rm::unitbp::loadFile(bare.path());
    REQUIRE(withoutTag.has_value());
    CHECK(withoutTag->description == "Test Structure");

    // The one blueprint with no Description keeps an empty name rather than inventing one —
    // the interface falls back to the id, which is what it showed for everything before.
    const Blueprint none{"XXB0002_unit.bp", kMediumTank};
    const auto missing = rm::unitbp::loadFile(none.path());
    REQUIRE(missing.has_value());
    CHECK(missing->description.empty());

    // A malformed tag — opened, never closed — yields nothing rather than the tag itself:
    // "<LOC ueb" drawn on a button is worse than the id the empty answer falls back to.
    const Blueprint broken{"XXB0003_unit.bp", R"(
UnitBlueprint {
    Description = '<LOC ueb1103_desc',
    Physics = { MotionType = 'RULEUMT_None' },
})"};
    const auto malformed = rm::unitbp::loadFile(broken.path());
    REQUIRE(malformed.has_value());
    CHECK(malformed->description.empty());
}

TEST_CASE("intel radii arrive in elmos, from the ogrids the Intel block states") {
    // The Intel block is what a `.bp` says about what a unit can SEE, and the four
    // radii read here are the ones the corpus actually declares: VisionRadius on
    // 391 of the shipped units, WaterVisionRadius on 70, SonarRadius on 67,
    // RadarRadius on 57. Ogrids like every other distance in the family, so x8 —
    // the ACU's VisionRadius of 26 is 208 elmos, which is the number to check
    // against BAR's `sightdistance`, already stated in elmos.
    const Blueprint bp{"URL0001_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land' },
            Intel = {
                VisionRadius = 26,
                WaterVisionRadius = 26,
                RadarRadius = 0,
                SonarRadius = 60,
            },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    CHECK(def->visionRadiusElmos == Approx(208.0f));
    CHECK(def->waterVisionRadiusElmos == Approx(208.0f));
    CHECK(def->sonarRadiusElmos == Approx(480.0f));

    // Stated as zero, and kept as zero. A radius of nothing is a unit that carries
    // no radar, which is most of them — not a unit whose radar we failed to read.
    CHECK(def->radarRadiusElmos == Approx(0.0f));
}

TEST_CASE("a blueprint with no Intel block sees nothing rather than everything") {
    // 177 of the 568 declare no VisionRadius. Defaulting a missing radius to
    // anything but zero would give a wreck, a prop and a nuke silo the sight of a
    // scout — and the failure would show as the enemy's whole base being visible
    // for reasons nobody could trace back to a missing table.
    const Blueprint bp{"UEB0101_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    CHECK(def->visionRadiusElmos == Approx(0.0f));
    CHECK(def->waterVisionRadiusElmos == Approx(0.0f));
    CHECK(def->radarRadiusElmos == Approx(0.0f));
    CHECK(def->sonarRadiusElmos == Approx(0.0f));
}

TEST_CASE("a fractional size keeps its precision instead of rounding to squares") {
    // 418 of the 568 shipped sizes are fractional and 154 are under a single
    // ogrid, the smallest 0.01 — which as whole squares would be a radius of 4
    // elmos instead of 0.04, a hundredfold, and would space a crowd of them out
    // by a distance none of them needs.
    const Blueprint bp{"TINY_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 0.01, SizeZ = 0.01,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->collisionRadiusElmos == Approx(0.04f));

    // The BUILD footprint is whole squares by nature, and rounds UP: a unit must
    // never claim less ground than it stands on, and zero squares would let it be
    // placed inside a wall.
    CHECK(def->footprintSquaresX == 1);
    CHECK(def->footprintSquaresZ == 1);
}

TEST_CASE("a structure's own build footprint wins over its collision size") {
    // 363 of 568 state one, essentially the structures — which are the things
    // that occupy a grid. Where it exists it is the authority.
    const Blueprint bp{"UEB0101_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 5, SizeZ = 5 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 4.15, SizeZ = 4.35,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->footprintSquaresX == 5);
    CHECK(def->footprintSquaresZ == 5);
    // ...and the collision radius still comes from the collision box.
    CHECK(def->collisionRadiusElmos == Approx(0.5f * 4.35f * 8.0f));
    CHECK_FALSE(def->isMobile());
    CHECK(def->motion == MotionType::None);
}

TEST_CASE("retail derives zero footprints and clamps skirt geometry") {
    // The native blueprint constructor stores footprint dimensions as zero, then
    // `REntityBlueprint::OnInitBlueprint` replaces each zero (whether omitted or explicit)
    // with ceil(top-level Size). `ComputeDerivedQuantities` subsequently prevents a skirt
    // from being smaller than that footprint. See C-109 in the EXE-analysis ledger.
    const Blueprint bp{"UEB4302_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 0, SizeZ = 0 },
            Physics = {
                MotionType = 'RULEUMT_None',
                SkirtOffsetX = 5,
                SkirtOffsetZ = -2,
                SkirtSizeX = 3,
                SkirtSizeZ = 8,
            },
            SizeX = 1.75,
            SizeZ = 10,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    CHECK(def->footprintSquaresX == 2);
    CHECK(def->footprintSquaresZ == 10);
    CHECK(def->skirtSquaresX == Approx(3.0f));
    CHECK(def->skirtSquaresZ == Approx(10.0f));
    // Positive X is discarded, then (3 - 2) / 2 shifts the wider skirt by half a square.
    // Negative Z survives; the size clamp itself contributes no shift on that axis.
    CHECK(def->skirtCentreOffsetSquaresX == Approx(0.5f));
    CHECK(def->skirtCentreOffsetSquaresZ == Approx(-2.0f));
}
TEST_CASE("Physics.FlattenSkirt parses to the unit def") {
    // Retail's native bool slot at blueprint_layout.tsv:73 (RUnitBlueprintPhysics+0x278,
    // "If true, terrain under building's skirt will be flattened") — currently unparsed.
    const Blueprint flat{"UEB0101_unit.bp", R"(
        UnitBlueprint {
            Defense = { MaxHealth = 100 },
            Physics = { MotionType = 'RULEUMT_None', FlattenSkirt = true },
        }
    )"};
    const auto withFlag = rm::unitbp::loadFile(flat.path());
    REQUIRE(withFlag.has_value());
    CHECK(withFlag->flattenSkirt);
    const Blueprint plain{"UEB0102_unit.bp", R"(
        UnitBlueprint {
            Defense = { MaxHealth = 100 },
            Physics = { MotionType = 'RULEUMT_None' },
        }
    )"};
    const auto withoutFlag = rm::unitbp::loadFile(plain.path());
    REQUIRE(withoutFlag.has_value());
    CHECK_FALSE(withoutFlag->flattenSkirt);
}


TEST_CASE("a wreck's value and the builder's reach arrive from the economy tables") {
    // The value chain is FA's `Unit.lua:1090-1146`: a wreck holds
    // `BuildCostMass * MassMult` and `BuildCostEnergy * EnergyMult`.
    // Measured across the corpus: 504 of 568 blueprints state a Wreckage table and every
    // one says MassMult = 0.9, EnergyMult = 0, ReclaimTimeMultiplier = 1.
    const Blueprint bp{"UEL0106_unit.bp", R"(
        UnitBlueprint {
            Economy = {
                BuildCostMass = 200,
                BuildCostEnergy = 1000,
                MaxBuildDistance = 5,
            },
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 0.6, SizeZ = 0.6,
            Wreckage = {
                EnergyMult = 0.5,
                MassMult = 0.9,
                HealthMult = 0.9,
                ReclaimTimeMultiplier = 1,
            },
            Defense = { MaxHealth = 1000 },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    // 200 * 0.9 and 1000 * 0.5, computed at parse time so the sim never multiplies floats.
    CHECK(rm::test::asFloat(def->wreckMass) == Approx(180.0f));
    CHECK(rm::test::asFloat(def->wreckEnergy) == Approx(500.0f));
    CHECK(rm::test::asFloat(def->wreckHealth) == Approx(900.0f));

    // FA Prop.lua:153-162 divides the work budget by 10 before the native task multiplies it
    // back by 10. The inverses leave 10 / ReclaimTimeMultiplier value per BuildRate-second.
    CHECK(rm::sim::fxToFloat(def->reclaimPerBuildRate) == Approx(10.0f));

    // 5 ogrids x 8 elmos — reclaim, repair and build all share this reach.
    CHECK(def->buildDistanceElmos == Approx(40.0f));
}

TEST_CASE("no Wreckage table means nothing to reclaim, and the reach defaults to 5") {
    // The ACUs and the walls state no Wreckage table, and `Unit.lua:1762-1765` leaves no
    // wreck for them at all: `MassMult or 0` (`:1769`). The build reach defaults to 5
    // ogrids — `blueprints-units.lua:250` spells the fallback out.
    const Blueprint bp{"UEL0001_unit.bp", R"(
        UnitBlueprint {
            Economy = { BuildCostMass = 18000 },
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 0.75, SizeZ = 0.75,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(rm::test::asFloat(def->wreckMass) == Approx(0.0f));
    CHECK(rm::test::asFloat(def->wreckEnergy) == Approx(0.0f));
    CHECK(def->buildDistanceElmos == Approx(40.0f));
}

TEST_CASE("the motion class decides where a unit may go, since the file does not") {
    // A `.bp` states no slope limit and no wading depth for ANY of the 568. The
    // only slope figure the format carries is Footprint.MaxSlope, a gradient on
    // aircraft landing sites. So these come from the class.
    struct Case {
        std::string_view motion;
        MotionType expected;
        bool onGround;
        bool entersWater;
    };
    const Case cases[] = {
        {"RULEUMT_Land", MotionType::Land, true, false},
        {"RULEUMT_Hover", MotionType::Hover, true, true},
        {"RULEUMT_Amphibious", MotionType::Amphibious, true, true},
        {"RULEUMT_AmphibiousFloating", MotionType::AmphibiousFloating, true, true},
        {"RULEUMT_Air", MotionType::Air, false, false},
        {"RULEUMT_Water", MotionType::Water, false, false},
        {"RULEUMT_SurfacingSub", MotionType::SurfacingSub, false, false},
        {"RULEUMT_None", MotionType::None, false, false},
    };

    for (const Case& c : cases) {
        // `C-218 (3)`: a class with `MaxSpeed == 0` reads as `None`, so every
        // mobile class states a nonzero speed here — the table pins the class
        // mapping, the (3) case pins the zero gate.
        const std::string source = std::string{"UnitBlueprint { SizeX = 1, SizeZ = 1, Physics = { "}
                                 + "MotionType = '" + std::string{c.motion} + "', MaxSpeed = 1 } }";
        const Blueprint bp{"X_unit.bp", source};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        CHECK(def->motion == c.expected);
        CHECK(rm::unitdef::travelsOnGround(def->motion) == c.onGround);

        if (c.onGround) {
            // The engine's own limit, which is also what BAR's ground units
            // authorise — the two families agree without either being bent.
            CHECK(def->maxSlopeDegrees == Approx(rm::sim::kDefaultMaxSlopeDegrees));
        }
        // Land is the sharp one: Supreme Commander's land units do not ford at
        // all, where a BAR unit wades 12 elmos and a shoreline is passable.
        CHECK((def->maxWaterDepthElmos > 0.0f) == c.entersWater);
    }
}

TEST_CASE("an aircraft is known by its motion class") {
    const Blueprint bp{"UEA0101_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 10, TurnRate = 180 },
            Air = { MaxAirspeed = 15 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->canFly);
    CHECK_FALSE(rm::unitdef::travelsOnGround(def->motion));
    CHECK(def->speedElmosPerSecond == Approx(120.0f));
}

TEST_CASE("FA weapon target layers distinguish interceptors from bombers") {
    SECTION("an interceptor may attack air and not the ground") {
        const Blueprint bp{"UEA0102_unit.bp", R"(
            UnitBlueprint {
                Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 0.5 },
                Air = { MaxAirspeed = 18 },
                SizeX = 1, SizeZ = 1,
                Weapon = {
                    {
                        WeaponCategory = 'Anti Air', Damage = 10, MaxRadius = 20,
                        RateOfFire = 1, CannotAttackGround = true,
                        FireTargetLayerCapsTable = {
                            Air = 'Air|Land', Land = 'Air|Land',
                        },
                        TargetRestrictOnlyAllow = 'AIR',
                    },
                },
            }
        )"};

        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        REQUIRE(def->weapons.size() == 1);
        CHECK(def->weapons[0].canTarget(true));
        CHECK_FALSE(def->weapons[0].canTarget(false));
    }

    SECTION("a bomber may attack the surface and not air") {
        const Blueprint bp{"UEA0103_unit.bp", R"(
            UnitBlueprint {
                Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 0.5 },
                Air = { MaxAirspeed = 12 },
                SizeX = 1, SizeZ = 1,
                Weapon = {
                    {
                        WeaponCategory = 'Bomb', Damage = 100, MaxRadius = 20,
                        RateOfFire = 1,
                        FireTargetLayerCapsTable = {
                            Air = 'Land|Water|Seabed',
                            Land = 'Air|Land|Water|Seabed',
                        },
                    },
                },
            }
        )"};

        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        REQUIRE(def->weapons.size() == 1);
        CHECK(def->weapons[0].canTarget(false));
        CHECK_FALSE(def->weapons[0].canTarget(true));
    }
}

TEST_CASE("FA weapon firing arcs are converted to binary radians at load") {
    const Blueprint bp{"weapon_arc_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Direct Fire', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                    HeadingArcCenter = 90,
                    HeadingArcRange = 30,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 1);
    CHECK(def->weapons[0].arcCentreBrads == 16384);  // 90 / 360 of one 65,536-brad turn
    CHECK(def->weapons[0].arcRangeBrads == 5461);    // rounded 30 / 360 of one turn
}

TEST_CASE("FA weapon priority rows, height reach, and rescan cadence parse") {
    // `C-229`/`C-093`: the remaining WP-27 weapon keys — `TargetPriorities`
    // rows, `MaxHeightDiff` reach, `SlavedToBody`, `TargetCheckInterval`.
    const Blueprint bp{"weapon_priorities_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Direct Fire', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                    TargetPriorities = { 'TECH3 MOBILE', 'LAND' },
                    MaxHeightDiff = 2.5,
                    SlavedToBody = true,
                    TargetCheckInterval = 0.5,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 1);
    const auto& weapon = def->weapons[0];
    REQUIRE(weapon.targetPriorities.size() == 2);
    CHECK(weapon.targetPriorities[0].size() == 2);  // 'TECH3 MOBILE' is one row
    CHECK(weapon.targetPriorities[1].size() == 1);
    CHECK(weapon.maxHeightDifference == rm::test::fx(2.5f * 8.0f));  // ogrids → elmos;
    CHECK(weapon.slavedToBody);
    CHECK(weapon.targetCheckIntervalSeconds == 0.5f);
    CHECK_FALSE(weapon.autoInitiateAttackCommand);  // `C-493`: absent means unset
}

TEST_CASE("FA weapon TrackingRadius is dimensionless and defaults without shortening range") {
    const Blueprint bp{"tracking_radius_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                { WeaponCategory = 'Defense', Damage = 10, MaxRadius = 20, RateOfFire = 1 },
                { WeaponCategory = 'Defense', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                  TrackingRadius = 0.75 },
                { WeaponCategory = 'Defense', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                  TrackingRadius = 2 },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 3);
    CHECK(def->weapons[0].trackingRadius == rm::sim::Fx::fromInt(1));
    CHECK(def->weapons[1].trackingRadius == rm::test::fx(0.75f));
    CHECK(def->weapons[2].trackingRadius == rm::sim::Fx::fromInt(2));
}

TEST_CASE("FA weapon target restrictions retain complete category expressions") {
    const Blueprint bp{"target_restrictions_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Direct Fire', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                    TargetRestrictOnlyAllow = 'NAVAL MOBILE',
                    TargetRestrictOnlyDisallow = 'NAVAL EXPERIMENTAL',
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 1);
    REQUIRE(def->weapons[0].targetRestrictOnlyAllow.has_value());
    REQUIRE(def->weapons[0].targetRestrictOnlyDisallow.has_value());
    CHECK(*def->weapons[0].targetRestrictOnlyAllow
          == std::vector<std::string>{"NAVAL", "MOBILE"});
    CHECK(*def->weapons[0].targetRestrictOnlyDisallow
          == std::vector<std::string>{"NAVAL", "EXPERIMENTAL"});
}

TEST_CASE("FA point defence weapons retain their projectile target type") {
    const Blueprint bp{"point_defence_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Defense', TargetType = 'RULEWTT_Projectile',
                    Damage = 10, MaxRadius = 20, RateOfFire = 1,
                },
                {
                    WeaponCategory = 'Direct Fire', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 2);
    CHECK(def->weapons[0].targetsProjectiles);
    CHECK_FALSE(def->weapons[0].fires());
    CHECK(def->weapons[0].firesAtProjectiles());
    CHECK_FALSE(def->weapons[1].targetsProjectiles);
    CHECK(def->weapons[1].fires());
}

TEST_CASE("FA silo weapons retain their counted-projectile metadata") {
    const Blueprint bp{"UEB4302_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Defense', CountedProjectile = true,
                    NukeWeapon = false, MaxProjectileStorage = 7,
                },
                { WeaponCategory = 'Direct Fire' },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 2);

    []<typename Weapon>(const Weapon& counted, const Weapon& ordinary) {
        if constexpr (requires {
                          counted.countedProjectile;
                          counted.nukeWeapon;
                          counted.maxProjectileStorage;
                      }) {
            CHECK(counted.countedProjectile);
            // ART-S001 /units/UEB4302/UEB4302_unit.bp:Weapon[1].NukeWeapon is absent.
            // C-082: a counted weapon without NukeWeapon occupies the tactical silo slot.
            CHECK_FALSE(counted.nukeWeapon);
            CHECK(counted.maxProjectileStorage == 7);
            CHECK_FALSE(ordinary.countedProjectile);
            CHECK_FALSE(ordinary.nukeWeapon);
            CHECK(ordinary.maxProjectileStorage == 0);
        } else {
            FAIL("Weapon must retain CountedProjectile, NukeWeapon, and MaxProjectileStorage");
        }
    }(def->weapons[0], def->weapons[1]);
}

TEST_CASE("an ordinary FA shield reads its capacity, radius and recovery timing") {
    const Blueprint bp{"UEB4202_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 2, SizeZ = 2,
            Defense = {
                MaxHealth = 500,
                Shield = {
                    ShieldMaxHealth = 9000,
                    ShieldSize = 26,
                    ShieldVerticalOffset = -3,
                    ShieldRegenRate = 120,
                    ShieldRegenStartTime = 1,
                    ShieldRechargeTime = 15,
                    ShieldEnergyDrainRechargeTime = 5,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->shield.maximum == rm::sim::Mag::fromInt(9000));
    CHECK(def->shield.radiusElmos == rm::sim::Fx::fromInt(104));
    CHECK(def->shield.verticalOffsetElmos == rm::sim::Fx::fromInt(-24));
    CHECK(def->shield.regenPerSecond == Approx(120.0f));
    CHECK(def->shield.regenDelay.value == Approx(1.1f));
    CHECK(def->shield.rechargeDelay.value == Approx(15.1f));
}

TEST_CASE("a personal shield imports its retail box collision shape") {
    const Blueprint bp{"UAL0202_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Defense = {
                MaxHealth = 500,
                Shield = {
                    PersonalShield = true,
                    ShieldMaxHealth = 1750,
                    ShieldSize = 3,
                    CollisionSizeX = 2,
                    CollisionSizeY = 3,
                    CollisionSizeZ = 4,
                    CollisionCenterX = 0.25,
                    CollisionCenterY = 0.5,
                    CollisionCenterZ = -0.25,
                    ShieldRegenRate = 2,
                    ShieldRegenStartTime = 1,
                    ShieldRechargeTime = 75,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->shield.exists());
    CHECK(def->shield.shape == rm::unitdef::ShieldShape::Box);
    CHECK(def->shield.maximum == rm::sim::Mag::fromInt(1750));
    CHECK(def->shield.boxHalfExtentsElmos[0] == rm::sim::Fx::fromInt(8));
    CHECK(def->shield.boxHalfExtentsElmos[1] == rm::sim::Fx::fromInt(12));
    CHECK(def->shield.boxHalfExtentsElmos[2] == rm::sim::Fx::fromInt(16));
    CHECK(def->shield.collisionCenterElmos[0] == rm::sim::Fx::fromInt(2));
    CHECK(def->shield.collisionCenterElmos[1] == rm::sim::Fx::fromInt(4));
    CHECK(def->shield.collisionCenterElmos[2] == rm::sim::Fx::fromInt(-2));
}

TEST_CASE("bubble coverage flags parse without dropping the shield") {
    // Previously either flag discarded the whole Shield block. Both now parse
    // with ordinary geometry and their flags recorded for the coverage rules.
    const Blueprint bp{"SYN0301_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 2, SizeZ = 2,
            Defense = {
                MaxHealth = 500,
                Shield = {
                    PersonalBubble = true,
                    TransportShield = true,
                    ShieldMaxHealth = 9000,
                    ShieldSize = 26,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->shield.exists());
    CHECK(def->shield.personalBubble);
    CHECK(def->shield.transportShield);
    CHECK(def->shield.maximum == rm::sim::Mag::fromInt(9000));
    CHECK(def->shield.radiusElmos == rm::sim::Fx::fromInt(104));
}

TEST_CASE("a blueprint with no Physics table is refused, not read as a building") {
    // A building has a Physics table stating RULEUMT_None. A blueprint with none
    // at all is a file this reader has misunderstood, and defaulting it to
    // immobile would present as a unit that mysteriously will not move.
    const Blueprint bp{"BROKEN_unit.bp", "UnitBlueprint { SizeX = 1, SizeZ = 1 }"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE_FALSE(def.has_value());
    CHECK(def.error().message.find("Physics") != std::string::npos);
}

TEST_CASE("an unknown motion class is refused rather than treated as immobile") {
    // Nothing in the retail corpus is unknown; a mod inventing a class should
    // say so rather than ship a unit that silently never moves.
    const Blueprint bp{"MODDED_unit.bp", R"(
        UnitBlueprint { SizeX = 1, SizeZ = 1, Physics = { MotionType = 'RULEUMT_Teleport' } }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE_FALSE(def.has_value());
    CHECK(def.error().message.find("RULEUMT_Teleport") != std::string::npos);
}

TEST_CASE("a missing blueprint is a parse error, not a crash") {
    const auto def = rm::unitbp::loadFile("/nonexistent/NOPE_unit.bp");
    REQUIRE_FALSE(def.has_value());
}

TEST_CASE("a blueprint names no mesh, so an absent one resolves to nothing") {
    // The convention is `X_unit.bp` beside `X_lod0.scm`. No file, no path — and
    // emphatically not a guess, since a path that does not exist would be handed
    // to the model loader as if it might.
    const Blueprint bp{"ALONE_unit.bp", kMediumTank};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->modelPath.empty());  // by convention, so nothing to state
    CHECK(rm::unitbp::resolveMesh(*def, bp.path()).empty());
}

TEST_CASE("MeshName is kept as the file states it, unresolved") {
    // Two forms, told apart by the extension, and `modelPath` holds either
    // verbatim — exactly as it holds BAR's `objectname`. Resolution is a separate
    // step because it touches the filesystem.
    const Blueprint asPath{"OPC1001_unit.bp", R"(
        UnitBlueprint {
            Display = { Mesh = { LODs = { { MeshName = '/Env/Props/Warehouse_lod0.scm' } } } },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto byPath = rm::unitbp::loadFile(asPath.path());
    REQUIRE(byPath.has_value());
    CHECK(byPath->modelPath == "/Env/Props/Warehouse_lod0.scm");
    // A VFS path with no root to join it to resolves to nothing rather than to
    // the filesystem root, which is what dropping the leading slash would do.
    CHECK(rm::unitbp::resolveMesh(*byPath, asPath.path()).empty());

    const Blueprint asId{"XAC8101_unit.bp", R"(
        UnitBlueprint {
            Display = { Mesh = { LODs = { { MeshName = 'UEC1101' } } } },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto byId = rm::unitbp::loadFile(asId.path());
    REQUIRE(byId.has_value());
    CHECK(byId->modelPath == "UEC1101");
}

TEST_CASE("a scale of zero is read as stated, not corrected to one") {
    // Two shipped blueprints do this — unused Seraphim civilian entries. A mesh
    // scaled to nothing is what the file says; inventing a 1 would be inventing
    // content, and the corpus test records the count so it cannot drift unnoticed.
    const Blueprint bp{"XSC9010_unit.bp", R"(
        UnitBlueprint {
            Display = { UniformScale = 0 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->meshToElmos == Approx(0.0f));
}

TEST_CASE("the strategic icon's name arrives, and its absence stays empty") {
    const Blueprint named{"UEL0201_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Land' },
    StrategicIconName = 'icon_land1_directfire',
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->strategicIcon == "icon_land1_directfire");

    // 18 of 568 state none; they keep the plain square, which is the honest fallback.
    const Blueprint bare{"XXB0004_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->strategicIcon.empty());
}

TEST_CASE("the build-menu sort priorities arrive, and their absence stays zero") {
    // `BuildIconSortPriority` is what retail's construction menu orders cells by, with
    // `StrategicIconSortPriority` the fallback (`construction.lua`'s SortFunc reads
    // `bp.BuildIconSortPriority or bp.StrategicIconSortPriority`). Both are root scalars —
    // XRL0305 states 20 and 115.
    const Blueprint named{"XXB0006_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Land' },
    BuildIconSortPriority = 20,
    StrategicIconSortPriority = 115,
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->buildIconSortPriority == 20);
    CHECK(def->strategicIconSortPriority == 115);

    const Blueprint bare{"XXB0007_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->buildIconSortPriority == 0);
    CHECK(none->strategicIconSortPriority == 0);
}

TEST_CASE("a guard scan radius arrives in elmos, and its unread neighbour stays unread") {
    // DEA0202 states `AI = { GuardReturnRadius = 100, GuardScanRadius = 80 }`. The scan
    // radius is ogrids like every sibling range (`C-183`); the return radius is
    // deliberately not read anywhere — retail never reads it either.
    const Blueprint named{"XXB0005_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Land' },
    AI = { GuardReturnRadius = 100, GuardScanRadius = 80 },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(rm::sim::fxToFloat(def->guardScanRadiusElmos) == Approx(640.0f));
}

TEST_CASE("retail command capabilities keep true and false distinct") {
    // URL0107's discriminator: its auxiliary arm repairs but cannot reclaim. The order page
    // receives these true entries from GetUnitCommandData rather than inferring both from
    // BuildRate=1 (`lua/ui/game/orders.lua:SetAvailableOrders`).
    const Blueprint named{"URL0107_unit.bp", R"(
UnitBlueprint {
    General = {
        CommandCaps = {
            RULEUCC_Attack = true,
            RULEUCC_Reclaim = false,
            RULEUCC_Repair = true,
        },
    },
    Physics = { MotionType = 'RULEUMT_Land' },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->commandCapsDeclared);
    CHECK(def->hasCommandCap("RULEUCC_Attack"));
    CHECK_FALSE(def->hasCommandCap("RULEUCC_Reclaim"));
    CHECK(def->hasCommandCap("RULEUCC_Repair"));
    CHECK(def->commandCaps.size() == 2);

    const Blueprint bare{"XXB0012_unit.bp", kMediumTank};
    const auto fallback = rm::unitbp::loadFile(bare.path());
    REQUIRE(fallback.has_value());
    CHECK_FALSE(fallback->commandCapsDeclared);
    CHECK(fallback->commandCaps.empty());
}

TEST_CASE("a missile redirector arrives with radius and rate, or neither") {
    // URL0303 states `Defense = { AntiMissile = { AttachBone = 'Turret_Muzzle',
    // Radius = 5, RedirectRateOfFire = 1 } }` (`C-088`). Radius is ogrids; the rate is
    // shots per second. AttachBone is presentation and is not read.
    const Blueprint named{"XXB0006_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Land' },
    Defense = { AntiMissile = { Radius = 5, RedirectRateOfFire = 1 } },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(rm::sim::fxToFloat(def->antiMissileRadiusElmos) == Approx(40.0f));
    CHECK(def->antiMissileRatePerSecond == Approx(1.0f));

    const Blueprint bare{"XXB0007_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->antiMissileRadiusElmos == rm::sim::Fx{});
    CHECK(none->antiMissileRatePerSecond == 0.0f);
}

TEST_CASE("mesh extents and build-effect bones arrive as the file states them") {
    // UEL0105's shape: `MeshExtentsX/Z` in ogrids become elmos like every other length, and
    // `General.BuildBones.BuildEffectBones` keeps its names in file order for the model to
    // resolve. A unit that does not build has no bones and no extents rather than defaults.
    const Blueprint named{"XXB0010_unit.bp", R"(
UnitBlueprint {
    General = {
        BuildBones = {
            AimBone = 0,
            BuildEffectBones = { 'Turret_Muzzle', 'Left_Arm' },
            PitchBone = 'Turret_Barrel',
            PitchMax = 45,
            PitchSlew = 120,
            YawBone = 'Turret',
            YawMin = -70,
        },
    },
    Physics = { MotionType = 'RULEUMT_Land', MeshExtentsX = 1.5, MeshExtentsY = 2, MeshExtentsZ = 0.75 },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->meshExtentsXElmos == Approx(12.0f));
    CHECK(def->meshHeightElmos == Approx(16.0f));
    CHECK(def->meshExtentsZElmos == Approx(6.0f));
    REQUIRE(def->buildEffectBones.size() == 2);
    CHECK(def->buildEffectBones[0] == "Turret_Muzzle");
    CHECK(def->buildEffectBones[1] == "Left_Arm");
    REQUIRE(def->builderArm.exists());
    CHECK(def->builderArm.yawBone.name == "Turret");
    CHECK(def->builderArm.pitchBone.name == "Turret_Barrel");
    CHECK(def->builderArm.aimBone.index == 0);
    CHECK(def->builderArm.yawMinDegrees == Approx(-70.0f));
    CHECK(def->builderArm.yawMaxDegrees == Approx(180.0f));
    CHECK(def->builderArm.pitchMaxDegrees == Approx(45.0f));
    // The Cybran random-offset box: root `SizeX`/`SizeZ` kept whole (the radius
    // keeps only the larger half) plus the `CollisionOffsetY` lift
    // (`Unit.lua:GetRandomOffset`, URL0107: 0.15 ogrids -> 1.2 elmos).
    CHECK(def->sizeXElmos == Approx(0.0f));  // XXB0010 states no Size keys
    CHECK(def->sizeZElmos == Approx(0.0f));
    CHECK(def->collisionOffsetYElmos == Approx(0.0f));

    const Blueprint bare{"XXB0011_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->meshExtentsXElmos == 0.0f);
    CHECK(none->buildEffectBones.empty());
    CHECK_FALSE(none->builderArm.exists());
}

TEST_CASE("a winged mover arrives with its control block, or nothing") {
    // URA0102's `Air` table (`C-221`, `C-244`): the proportional and damping gains, the
    // climb authority, the two timers, winged combat inputs, and both Physics heights in
    // ogrids turned into elmos. The real interceptor scenario also checks combat tuning
    // through the parser and spawn path; three-axis banking remains outside this model.
    const Blueprint named{"XXB0008_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Air', Elevation = 18, AttackElevation = 4 },
    Air = {
        KMove = 1, KMoveDamping = 1, KLift = 3, KLiftDamping = 2.5, LiftFactor = 7,
        MaxAirspeed = 15, MinAirspeed = 10, Winged = true,
        AutoLandTime = 1, FuelUseTime = 500,
    },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->airKMove == Approx(1.0f));
    CHECK(def->airKMoveDamping == Approx(1.0f));
    CHECK(def->airKLift == Approx(3.0f));
    CHECK(def->airKLiftDamping == Approx(2.5f));
    CHECK(def->airLiftFactor == Approx(7.0f));
    CHECK(def->airWinged);
    CHECK(def->airMinSpeedElmosPerSecond == Approx(80.0f));
    CHECK(def->airAttackElevationElmos == Approx(32.0f));
    CHECK(def->airAutoLandTimeSec == Approx(1.0f));
    CHECK(def->airFuelUseTimeSec == Approx(500.0f));
    CHECK(def->elevationElmos == Approx(144.0f));

    const Blueprint bare{"XXB0009_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->airKMove == 0.0f);
    CHECK(none->airKLiftDamping == 0.0f);
    CHECK(none->airLiftFactor == 0.0f);
    CHECK_FALSE(none->airWinged);
    CHECK(none->airMinSpeedElmosPerSecond == 0.0f);
    CHECK(none->airAttackElevationElmos == 0.0f);
    CHECK(none->airFuelUseTimeSec == 0.0f);
    CHECK(none->elevationElmos == 0.0f);
}

TEST_CASE("a missing minimum airspeed derives from maximum airspeed") {
    const Blueprint named{"XXB0012_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_Air' },
    Air = { MaxAirspeed = 12, Winged = true },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->airMinSpeedElmosPerSecond == Approx(96.0f));
}

TEST_CASE("toggle caps and order overrides arrive as authored", "[unitbp][toggles]") {
    const Blueprint named{"XEB0011_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_None' },
    General = {
        ToggleCaps = { RULEUTC_ShieldToggle = true, RULEUTC_IntelToggle = false },
        OrderOverrides = {
            RULEUTC_ShieldToggle = { bitmapId = 'shield-dome', helpText = 'toggle_shield_dome' },
            RULEUCC_Transport = { bitmapId = 'deploy' },
            Broken = 3,
        },
    },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->toggleCapsDeclared);
    CHECK(def->toggleCaps == std::vector<std::string>{"RULEUTC_ShieldToggle"});
    CHECK(def->hasToggleCap("RULEUTC_ShieldToggle"));
    CHECK_FALSE(def->hasToggleCap("RULEUTC_IntelToggle"));
    REQUIRE(def->orderOverrides.size() == 2);
    CHECK(def->orderOverrides.at("RULEUTC_ShieldToggle").bitmapId == "shield-dome");
    CHECK(def->orderOverrides.at("RULEUTC_ShieldToggle").helpText == "toggle_shield_dome");
    CHECK(def->orderOverrides.at("RULEUCC_Transport").bitmapId == "deploy");
    CHECK(def->orderOverrides.at("RULEUCC_Transport").helpText.empty());
}

TEST_CASE("an absent general table declares no toggles and no overrides", "[unitbp][toggles]") {
    const Blueprint named{"XEB0012_unit.bp", R"(
UnitBlueprint {
    Physics = { MotionType = 'RULEUMT_None' },
})"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK_FALSE(def->toggleCapsDeclared);
    CHECK(def->toggleCaps.empty());
    CHECK(def->orderOverrides.empty());
}

TEST_CASE("weapon manipulator specs arrive as authored", "[unitbp][manipulators]") {
    const Blueprint bp{"manipulator_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
            Weapon = {
                {
                    WeaponCategory = 'Direct Fire', Damage = 10, MaxRadius = 20, RateOfFire = 1,
                    TurretBoneYaw = 'Turret', TurretBonePitch = 'Turret_Barrel',
                    TurretYawSpeed = 100, TurretPitchSpeed = 60,
                    RackBones = {
                        {
                            RackBone = 'Turret_Barrel',
                            MuzzleBones = { 'Turret_Muzzle' },
                            HideMuzzle = true,
                            TelescopeBone = 'Turret_Barrel_Tele',
                            TelescopeRecoilDistance = -6,
                        },
                    },
                    RackRecoilDistance = -2,
                    RackRecoilReturnSpeed = 10,
                    MuzzleChargeDelay = 0.4,
                    AnimationReload = '/units/x/x_areload.sca',
                    WeaponUnpackAnimation = '/units/x/x_Aopen.sca',
                    WeaponUnpackAnimationRate = 0.4,
                    WeaponUnpackAnimatorPrecedence = 3,
                },
            },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    REQUIRE(def->weapons.size() == 1);
    const rm::unitdef::Weapon& gun = def->weapons[0];
    CHECK(gun.turretYawBone == "Turret");
    CHECK(gun.turretPitchBone == "Turret_Barrel");
    CHECK(gun.turretYawSpeedRadPerSecond == Catch::Approx(100.0f * std::numbers::pi_v<float> / 180.0f));
    CHECK(gun.turretPitchSpeedRadPerSecond == Catch::Approx(60.0f * std::numbers::pi_v<float> / 180.0f));
    CHECK(gun.recoilBone == "Turret_Barrel");
    CHECK(gun.recoilDistanceMesh == Catch::Approx(-2.0f));
    CHECK(gun.hideMuzzle);
    CHECK(gun.recoilReturnSpeedMeshPerSecond == Catch::Approx(10.0f));
    CHECK(gun.telescopeBone == "Turret_Barrel_Tele");
    REQUIRE(gun.telescopeDistanceMesh.has_value());
    CHECK(*gun.telescopeDistanceMesh == Catch::Approx(-6.0f));
    // Raw authored seconds for the recoil-return formula — NOT the
    // faWaitSeconds-corrected duration a charge phase would consume.
    CHECK(gun.muzzleChargeDelaySeconds == Catch::Approx(0.4f));
    CHECK(gun.animationReload == "/units/x/x_areload.sca");
    CHECK(gun.weaponUnpackAnimation == "/units/x/x_Aopen.sca");
    CHECK(gun.weaponUnpackAnimationRate == Catch::Approx(0.4f));
    CHECK(gun.weaponUnpackAnimatorPrecedence == 3);
    CHECK(gun.animationCharge.empty());
}

TEST_CASE("an air staging pad reads its refuel terms from the blueprint", "[unitbp][airstaging]") {
    // UEB5202's authored block: the multiplier C-223 says the pad applies to the
    // mover's drain rate, the radius that bounds its reach, and the authored
    // slot count (DockingSlots — dead in the retail executable per C-225, but
    // the only capacity the file states, and it matches the pad's attach bones).
    const Blueprint bp{"UEB5202_unit.bp", R"(
        UnitBlueprint {
            AI = {
                RefuelingMultiplier = 50,
                StagingPlatformScanRadius = 300,
            },
            Categories = { 'AIRSTAGINGPLATFORM', 'STRUCTURE' },
            Physics = { MotionType = 'RULEUMT_None' },
            Transport = {
                DockingSlots = 4,
                RepairRate = 0.1,
            },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK(def->isAirStagingPad());
    CHECK(def->refuelingMultiplier == Catch::Approx(50.0f));
    CHECK(rm::sim::fxToFloat(def->stagingScanRadiusElmos) == Catch::Approx(2400.0f));
    CHECK(def->transport.dockingSlots == 4);
}

TEST_CASE("a unit with no staging block is not a pad", "[unitbp][airstaging]") {
    const Blueprint bp{"UEL0303_unit.bp", R"(
        UnitBlueprint {
            Categories = { 'MOBILE', 'LAND' },
            Physics = { MotionType = 'RULEUMT_Land' },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK_FALSE(def->isAirStagingPad());
    CHECK(def->refuelingMultiplier == 0.0f);
}

TEST_CASE("C-218 (1): one zeroed tensor axis discards the other two authored values") {
    // `C-218`'s first explicit-0-vs-absent site (`fa-exe-analysis-plan.md`, `0x00518C8B`):
    // the guard is on the PRODUCT, so one zero discards the other two authored
    // values and all three become `(1/12)(a^2+b^2)` box inertia. Corpus census:
    // 0 of the 568 shipped units state any `InertiaTensor` axis, so every retail
    // unit rides the derived box — absence reads exactly like explicit zero.
    const Blueprint absent{"XXB0401_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto bare = rm::unitbp::loadFile(absent.path());
    REQUIRE(bare.has_value());
    // Absent reads exactly like explicit zero: SizeX=1, SizeY absent (0),
    // SizeZ=1, so Ix=(0+1)/12, Iy=(1+1)/12, Iz=(1+0)/12.
    CHECK(bare->inertiaTensorX == Catch::Approx(1.0f / 12.0f));
    CHECK(bare->inertiaTensorY == Catch::Approx(2.0f / 12.0f));
    CHECK(bare->inertiaTensorZ == Catch::Approx(1.0f / 12.0f));

    // One explicit zero poisons the other two authored axes: all three fall back
    // to the box derivation instead of reading any axis verbatim.
    const Blueprint oneZero{"XXB0402_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 1,
                InertiaTensorX = 0, InertiaTensorY = 4, InertiaTensorZ = 9,
            },
            SizeX = 1, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto poisoned = rm::unitbp::loadFile(oneZero.path());
    REQUIRE(poisoned.has_value());
    CHECK(poisoned->inertiaTensorX != 0.0f);
    CHECK(poisoned->inertiaTensorX == poisoned->inertiaTensorY);
    CHECK(poisoned->inertiaTensorY == poisoned->inertiaTensorZ);

    // All three authored nonzero: read verbatim, no derivation.
    const Blueprint explicitUnit{"XXB0403_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 1,
                InertiaTensorX = 2, InertiaTensorY = 4, InertiaTensorZ = 9,
            },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(explicitUnit.path());
    REQUIRE(def.has_value());
    CHECK(def->inertiaTensorX == 2.0f);
    CHECK(def->inertiaTensorY == 4.0f);
    CHECK(def->inertiaTensorZ == 9.0f);
}

TEST_CASE("C-218 (2): an AltFootprint is parsed into its own struct and never merged") {
    // `C-218`'s second explicit-0-vs-absent site: `Engine/Core/Blueprints/EntityBlueprint.lua:5`
    // declares `AltFootprint? FootprintBlueprint`, and `Sim/Unit.lua:128-131` exposes it
    // via `Unit:ForceAltFootprint(state)`. The two footprints carry the same five fields
    // (SizeX/Z, MaxSlope, MinWaterDepth, OccupancyCaps); the read MUST land in their own
    // struct, because silent merging with the regular footprint would deny the runtime
    // toggle its choice.
    const Blueprint named{"XXB0501_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None', AltMotionType = 'RULEUMT_Land' },
            Footprint = { SizeX = 5, SizeZ = 5 },
            AltFootprint = {
                SizeX = 8, SizeZ = 6, MaxSlope = 0.25,
                MinWaterDepth = 1.5, OccupancyCaps = 3,
            },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(named.path());
    REQUIRE(def.has_value());
    CHECK(def->hasAltFootprint);

    // 8 and 6 ogrids read as a ceil-rounded integer square count.
    CHECK(def->altFootprint.squaresX == 8);
    CHECK(def->altFootprint.squaresZ == 6);
    CHECK(def->altFootprint.maxSlopeDegrees == 0.25f);
    CHECK(def->altFootprint.minWaterDepthElmos == 12.0f);     // 1.5 × 8
    CHECK(def->altFootprint.occupancyCaps == 3);

    // And critically, the REGULAR footprint is NOT overwritten. The two are siblings,
    // not a merge; this is what makes the runtime toggle work. Absence on the regular
    // footprint now derives `layerTable[None] = 0` per `C-219` (not the old -1).
    CHECK(def->footprintSquaresX == 5);
    CHECK(def->footprintSquaresZ == 5);
    CHECK(def->occupancyCaps == 0);
}

TEST_CASE("C-218 (2): a blueprint with no AltFootprint keeps `hasAltFootprint` false") {
    const Blueprint bp{"XXB0502_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 5, SizeZ = 5 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK_FALSE(def->hasAltFootprint);
    // The struct stays at its constructor defaults — never silently filled from elsewhere.
    CHECK(def->altFootprint.squaresX == 0);
    CHECK(def->altFootprint.squaresZ == 0);
    CHECK(def->altFootprint.occupancyCaps == -1);
}

TEST_CASE("C-218 (2b): an AltFootprint without AltMotionType is overwritten by Footprint") {
    // `C-218` site (2) (`0x00525D77`): the entire `AltFootprint` struct is
    // overwritten by `Footprint` whenever `AltMotionType` is absent. The two
    // footprints are NOT siblings — the alt table only survives when the file
    // also states `Physics.AltMotionType`.
    const Blueprint noAltMotion{"XXB0503_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            Footprint = { SizeX = 5, SizeZ = 5 },
            AltFootprint = { SizeX = 8, SizeZ = 6 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto overwritten = rm::unitbp::loadFile(noAltMotion.path());
    REQUIRE(overwritten.has_value());
    CHECK_FALSE(overwritten->hasAltFootprint);
    CHECK(overwritten->altFootprint.squaresX == 5);
    CHECK(overwritten->altFootprint.squaresZ == 5);
    const Blueprint withAltMotion{"XXB0504_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None', AltMotionType = 'RULEUMT_Land' },
            Footprint = { SizeX = 5, SizeZ = 5 },
            AltFootprint = { SizeX = 8, SizeZ = 6 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto kept = rm::unitbp::loadFile(withAltMotion.path());
    REQUIRE(kept.has_value());
    CHECK(kept->hasAltFootprint);
    CHECK(kept->altFootprint.squaresX == 8);
    CHECK(kept->altFootprint.squaresZ == 6);
}

TEST_CASE("C-218 (4): an authored OccupancyCaps wins and an explicit 0 stays 0") {
    // `C-218`'s fourth explicit-0-vs-absent site: `Footprint.OccupancyCaps` is a layer mask
    // (entity schema `EntityBlueprint.lua:87`); a rock props file ships `OccupancyCaps = 3`
    // (Land|Air), a crystal ships `= 0` ("occupies no layer"). Absence now derives
    // `layerTable[MotionType]` per `C-219` (pinned in the next test); this test pins that
    // an authored key always wins, so an explicit zero is never overwritten by derivation.
    const Blueprint propLike{"XXB0601_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 1, SizeZ = 1, OccupancyCaps = 3 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto landAir = rm::unitbp::loadFile(propLike.path());
    REQUIRE(landAir.has_value());
    CHECK(landAir->occupancyCaps == 3);

    // An explicit zero is read as zero — the crystal case: a feature that occupies NO
    // layer is meaningful input, and reading it as "absent" would silently overwrite it
    // with the layer-derived default, which is a foot-gun.
    const Blueprint crystalLike{"XXB0602_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 1, SizeZ = 1, OccupancyCaps = 0 },
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto explicitZero = rm::unitbp::loadFile(crystalLike.path());
    REQUIRE(explicitZero.has_value());
    CHECK(explicitZero->occupancyCaps == 0);
    CHECK(explicitZero->occupancyCaps != -1);  // explicit zero, NOT the absent sentinel

    // Absence derives `layerTable[MotionType]` per `C-219` — here `None` → 0, the same
    // integer answer retail's pass writes; only an authored key (including an explicit
    // zero) bypasses the derivation.
    const Blueprint withNoFootprint{"XXB0603_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto absent = rm::unitbp::loadFile(withNoFootprint.path());
    REQUIRE(absent.has_value());
    CHECK(absent->occupancyCaps == 0);
}

TEST_CASE("C-218 (6): AttackElevation falls back to Elevation when the file states neither") {
    // `C-218`'s third explicit-0-vs-absent site chain: retail derives `AttackElevation`
    // from `Physics.Elevation` when the file states no `AttackElevation`. The previous
    // loader left both fields read independently, and a fighter that authors only its
    // cruise height was getting an attack altitude of 0 — the worst answer because 0 is
    // also "occupy the runway".
    SECTION("AttackElevation stated: wins") {
        const Blueprint bp{"XXB0701_unit.bp", R"(
            UnitBlueprint {
                Physics = {
                    MotionType = 'RULEUMT_Air',
                    Elevation = 18, AttackElevation = 4,
                },
                Air = { MaxAirspeed = 12, Winged = true },
            }
        )"};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        CHECK(def->airAttackElevationElmos == 32.0f);  // 4 × 8 (AttackElevation wins)
        CHECK(def->elevationElmos == 144.0f);          // 18 × 8
    }
    SECTION("AttackElevation absent: chain to Elevation") {
        const Blueprint bp{"XXB0702_unit.bp", R"(
            UnitBlueprint {
                Physics = { MotionType = 'RULEUMT_Air', Elevation = 18 },
                Air = { MaxAirspeed = 12, Winged = true },
            }
        )"};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        // Chain: missing AttackElevation → fall back to Elevation.
        CHECK(def->airAttackElevationElmos == 144.0f);   // 18 × 8
        CHECK(def->elevationElmos == 144.0f);
    }
    SECTION("both absent: zero, the runway altitude") {
        const Blueprint bp{"XXB0703_unit.bp", R"(
            UnitBlueprint {
                Physics = { MotionType = 'RULEUMT_Air' },
                Air = { MaxAirspeed = 12, Winged = true },
            }
        )"};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        CHECK(def->airAttackElevationElmos == 0.0f);
        CHECK(def->elevationElmos == 0.0f);
    }
}

TEST_CASE("C-218 (6): CatchUpAcc derives from max(MaxAcceleration, MaxBrake)") {
    // `C-218`'s sixth explicit-0-vs-absent chain: `Physics.CatchUpAcc`
    // (`UnitBlueprint.lua:1112-1113`) — the elmos-per-tick² a lagging mover applies.
    // An authored value is read verbatim; an absent key derives as
    // `max(MaxAcceleration, MaxBrake)`; only a file stating neither keeps zero.
    const Blueprint authored{"XXB0704_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 3.4,
                MaxAcceleration = 3.4, MaxBrake = 3.4, CatchUpAcc = 10,
            },
            SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->catchUpAcc == Catch::Approx(10.0f));

    // Discriminator: UEL0201 authors `MaxAcceleration = MaxBrake = 3.4` and no
    // `CatchUpAcc`, so its derived value is 3.4, not 0.
    const Blueprint derived{"XXB0705b_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 3.4,
                MaxAcceleration = 3.4, MaxBrake = 3.4,
            },
            SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,
        }
    )"};
    const auto derivedDef = rm::unitbp::loadFile(derived.path());
    REQUIRE(derivedDef.has_value());
    CHECK(derivedDef->catchUpAcc == Catch::Approx(3.4f));

    // Only a file stating neither accel limit keeps zero.
    const Blueprint bare{"XXB0705_unit.bp", kMediumTank};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->catchUpAcc == 0.0f);
}

TEST_CASE("C-218 (6): StartTurnDistance derives from SizeZ x 3 when unauthored") {
    // `C-218`'s sixth explicit-0-vs-absent chain: `Air.StartTurnDistance`
    // (`UnitBlueprint.lua:254-255`) — the elmos from a moving target at which a
    // flyer begins to align. Changelog 2026-02-14-3830 tunes it freely
    // (UAA0102: 10 → 5 ogrids); an authored value wins, an absent key derives as
    // `SizeZ x 3` ogrids in elmos. Discriminator: DAA0206 (the Mercy,
    // SizeZ = 1.4) derives 4.2 ogrids = 33.6 elmos.
    const Blueprint authored{"XXB0706_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, StartTurnDistance = 5 },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->startTurnDistanceElmos == Catch::Approx(5.0f * 8.0f));

    const Blueprint bare{"XXB0707_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->startTurnDistanceElmos == Catch::Approx(4.2f * 8.0f));
}
TEST_CASE("C-218: BuildOnLayerCaps reads the authored LAYER allow-set") {
    // `C-218`'s constructor-default tail (`BuildOnLayerCaps 1`): the allow-set
    // retail's placement consults alongside the motion class. Every shipped unit
    // authors it (census: 568/568); a land tank allows Land only, a naval
    // factory Water only. The loader reads each `LAYER_* = true` entry; a
    // default-allow-land keeps placement total when the table is absent.
    const Blueprint tank{"XXB0710_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 3.4,
                BuildOnLayerCaps = {
                    LAYER_Air = false, LAYER_Land = true, LAYER_Orbit = false,
                    LAYER_Seabed = false, LAYER_Sub = false, LAYER_Water = false,
                },
            },
            SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,
        }
    )"};
    const auto tankDef = rm::unitbp::loadFile(tank.path());
    REQUIRE(tankDef.has_value());
    CHECK(tankDef->buildOnLand);
    CHECK_FALSE(tankDef->buildOnWater);

    const Blueprint yard{"XXB0711_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_None',
                BuildOnLayerCaps = {
                    LAYER_Air = false, LAYER_Land = false, LAYER_Orbit = false,
                    LAYER_Seabed = false, LAYER_Sub = false, LAYER_Water = true,
                },
            },
            SizeX = 4, SizeY = 2, SizeZ = 6,
        }
    )"};
    const auto yardDef = rm::unitbp::loadFile(yard.path());
    REQUIRE(yardDef.has_value());
    CHECK(yardDef->buildOnWater);
    CHECK_FALSE(yardDef->buildOnLand);
}
TEST_CASE("C-411: CirclingTurnMult parses with a 1 default when unauthored") {
    // `C-411` (`Air+0x60`, schema `0x00e62768` "Adjust turning ability when in
    // circling mode"): the XSA0203 gunship authors 3, the UEA0104 gunship 1, and
    // an absent key defaults to 1 — ordinary turns are untouched. Unitless, never
    const Blueprint authored{"XXB0708_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, CirclingTurnMult = 3 },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->airCirclingTurnMult == Catch::Approx(3.0f));
    const Blueprint bare{"XXB0709_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->airCirclingTurnMult == Catch::Approx(1.0f));
}
TEST_CASE("C-224: TightTurnMultiplier parses with a 1 default when unauthored") {
    // `C-224` (`Air+0x1c`, schema `0x00527b24` "Additional turning multiplier
    // ability during a tight turn manuver"): the UEA0102 fighter authors 1.02,
    // the XAA0306 0, and an absent key defaults to 1 — ordinary turns keep
    // their authored gain. Unitless, never damped. Currently fails: the key is
    // never read, so the authored 1.02 decodes as the 1.0 default.
    const Blueprint authored{"XXB0712_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, TightTurnMultiplier = 1.02 },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->airTightTurnMultiplier == Catch::Approx(1.02f));
    const Blueprint bare{"XXB0713_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->airTightTurnMultiplier == Catch::Approx(1.0f));
}
TEST_CASE("C-244: KRollDamping parses with a 0 default when unauthored") {
    // `C-244` (`Air+0x5c`, schema `0x00527ca0` "Controller damping parameter
    // for roll changes"): the UEA0102 fighter authors 1, ten hulls author 2,
    // four author 4, and an absent key defaults to 0 — the unauthored state
    // of all fifteen non-winged flyers. Unitless, read at spawn.
    const Blueprint authored{"XXB0716_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, KRoll = 2, KRollDamping = 1 },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->airKRollDamping == Catch::Approx(1.0f));
    const Blueprint bare{"XXB0717_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->airKRollDamping == Catch::Approx(0.0f));
}
TEST_CASE("C-412: HoverOverAttack parses with a false default when unauthored") {
    // `C-412` (`Air+0x79`, schema `0x00527d5c` "Whether unit should hover over the
    // target directly to attack... used for cases like the C.Z.A.R"): the UAA0310
    // CZAR authors true, ordinary winged aircraft omit it, and an absent key
    // defaults to false — ordinary attacks keep the state machine. Currently
    // fails: the key is never read, so the authored true decodes as false.
    const Blueprint authored{"XXB0714_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, HoverOverAttack = true },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->airHoverOverAttack);
    const Blueprint bare{"XXB0715_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK_FALSE(none->airHoverOverAttack);
}
TEST_CASE("C-499: FlyInWater parses with a false default when unauthored") {
    // `C-499` (`Air+0x36a`, `0x006c3696` source select): a seaplane authors
    // true and measures clearance against the deep ground; ordinary flyers
    // omit it and clear the surface. No shipped unit states it, so absent
    // must decode false.
    const Blueprint authored{"XXB0716_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true, FlyInWater = true },
            SizeX = 0.8, SizeY = 1, SizeZ = 1,
        }
    )"};
    const auto def = rm::unitbp::loadFile(authored.path());
    REQUIRE(def.has_value());
    CHECK(def->airFlyInWater);
    const Blueprint bare{"XXB0717_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', MaxSpeed = 1 },
            Air = { MaxAirspeed = 12, Winged = true },
            SizeX = 0.8, SizeY = 0.25, SizeZ = 1.4,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK_FALSE(none->airFlyInWater);
}

TEST_CASE("C-218 (3): a stated motion class with MaxSpeed 0 reads as immobile") {
    // `C-218`'s third explicit-0-vs-absent site (`fa-exe-analysis-plan.md`, corroborated
    // by `RUnitBlueprint::IsMobile` being literally `bp[0x290] != 0`): retail forces
    // `Physics.MotionType` to `RULEUMT_None` when `MaxSpeed == 0`, so an explicit zero
    // is indistinguishable from an absent key. A mod blueprint stating a motion class
    // but no speed is a structure wearing a mover's class, not a mover. Currently
    // fails: the stated class survives with a zero speed.
    const Blueprint zero{"XXB0801_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 0 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto still = rm::unitbp::loadFile(zero.path());
    REQUIRE(still.has_value());
    CHECK(still->motion == MotionType::None);
    CHECK_FALSE(still->isMobile());
    CHECK(still->speedElmosPerSecond == 0.0f);
    // Absence agrees: no MaxSpeed key is the same immobile answer.
    const Blueprint absent{"XXB0802_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land' },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto gone = rm::unitbp::loadFile(absent.path());
    REQUIRE(gone.has_value());
    CHECK(gone->motion == MotionType::None);
    CHECK_FALSE(gone->isMobile());
}

TEST_CASE("C-219: absent OccupancyCaps derives from the motion-class layer table",
          "[unitbp][occupancy]") {
    // `C-219` (`fa-exe-analysis-plan.md`, `ComputeDerivedQuantities` at `0x00525D60`,
    // table at `0x00FB8B9C`): `Footprint.OccupancyCaps = layerTable[MotionType]` — a
    // unit's pathable layer set is derived, not authored, which is why 0 of the 568
    // shipped blueprints state `OccupancyCaps`. An authored value still wins; absence
    // derives from the (already MaxSpeed-gated) motion class. Currently fails: absence
    // reads as the -1 sentinel instead of the derived mask.
    struct Row {
        const char* motion;
        int wantCaps;
    };
    // Bit semantics per `C-219`/`C-202`: Land 1, Seabed 2, Sub 4, Water 8, Air 0x10.
    const Row rows[] = {
        {"RULEUMT_None", 0x0},
        {"RULEUMT_Land", 0x1},
        {"RULEUMT_Air", 0x10},
        {"RULEUMT_Water", 0x8},
        {"RULEUMT_Hover", 0x9},               // Land|Water
        {"RULEUMT_Amphibious", 0x3},          // Land|Seabed
        {"RULEUMT_AmphibiousFloating", 0x9},  // Land|Water
        {"RULEUMT_SurfacingSub", 0xC},        // Sub|Water
    };
    for (const Row& row : rows) {
        const std::string name =
            std::string("XXC219") + row.motion + "_unit.bp";
        const std::string source =
            "UnitBlueprint { Physics = { MotionType = '" + std::string(row.motion) +
            "', MaxSpeed = 1 }, SizeX = 1, SizeZ = 1, }";
        const Blueprint bp{name, source};
        const auto def = rm::unitbp::loadFile(bp.path());
        REQUIRE(def.has_value());
        CHECK(def->occupancyCaps == row.wantCaps);
    }
    // An authored mask still wins over the derived one, including an explicit zero
    // (the crystal case from the `C-218 (4)` test: "occupies no layer").
    const Blueprint stated{"XXC219stated_unit.bp", R"(
        UnitBlueprint {
            Footprint = { SizeX = 1, SizeZ = 1, OccupancyCaps = 3 },
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 1 },
            SizeX = 1, SizeZ = 1,
        }
    )"};
    const auto kept = rm::unitbp::loadFile(stated.path());
    REQUIRE(kept.has_value());
    CHECK(kept->occupancyCaps == 3);
}

TEST_CASE("C-218: absent MaxSpeedReverse and BackUpDistance read as the -1 sentinel") {
    // `C-218`'s distinguishing counter-examples (`fa-exe-analysis-plan.md`): unlike the
    // zero-guarded family, `MaxSpeedReverse` and `BackUpDistance` get `-1.0` from the
    // constructor and use `< 0` tests, so an absent key is NOT the same as an explicit
    // zero. UEL0201 authors `MaxSpeedReverse = 0` (cannot reverse); absence must read
    // -1 (the "unstated" sentinel), not 0. Currently fails: the keys are unread.
    const Blueprint absent{"XXB0901_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Land', MaxSpeed = 3.4 },
            SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,
        }
    )"};
    const auto none = rm::unitbp::loadFile(absent.path());
    REQUIRE(none.has_value());
    CHECK(none->maxSpeedReverseOgridsPerSecond == Catch::Approx(-1.0f));
    CHECK(none->backUpDistanceOgrids == Catch::Approx(-1.0f));

    // An explicit zero stays zero: 73 shipped units author `MaxSpeedReverse = 0`
    // (UEL0201 among them), which is "no reverse", not "unstated".
    const Blueprint zero{"XXB0902_unit.bp", R"(
        UnitBlueprint {
            Physics = {
                MotionType = 'RULEUMT_Land', MaxSpeed = 3.4,
                MaxSpeedReverse = 0, BackUpDistance = 0,
            },
            SizeX = 0.7, SizeY = 0.5, SizeZ = 0.9,
        }
    )"};
    const auto stated = rm::unitbp::loadFile(zero.path());
    REQUIRE(stated.has_value());
    CHECK(stated->maxSpeedReverseOgridsPerSecond == Catch::Approx(0.0f));
    CHECK(stated->backUpDistanceOgrids == Catch::Approx(0.0f));
}

TEST_CASE("C-218: MaxGroundVariation reads the authored tolerance, defaulting to retail's 1.0 ogrid") {
    // `C-218`'s closing constructor defaults (`fa-exe-analysis-plan.md`): retail
    // constructs `Physics.MaxGroundVariation` as 1.0, and the engine's own schema
    // description is "Maximum elevation difference across skirt for build site"
    // (`blueprint_layout.tsv`, RUnitBlueprintPhysics+0x28c). Ogrids like every
    // sibling length: UEB5101 (wall) authors 50, i.e. 400 elmos. Currently fails:
    // the key is unread.
    const Blueprint authored{"XXB0903_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None', MaxGroundVariation = 50 },
            SizeX = 1, SizeY = 0.5, SizeZ = 1,
        }
    )"};
    const auto wall = rm::unitbp::loadFile(authored.path());
    REQUIRE(wall.has_value());
    CHECK(wall->maxGroundVariationElmos == Catch::Approx(400.0f));

    const Blueprint bare{"XXB0904_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_None' },
            SizeX = 1, SizeY = 0.5, SizeZ = 1,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->maxGroundVariationElmos == Catch::Approx(8.0f));
}

TEST_CASE("Physics.FuelRechargeRate parses the authored refuel scalar, defaulting to zero") {
    // Native slot RUnitBlueprintPhysics+0x308 (`blueprint_layout.tsv:109`, registrar
    // `0x00527898`, "Unit fuels up at this rate per second"). Shipped UEA0103 (T1
    // bomber) authors `FuelRechargeRate = 5` under `Physics` beside `FuelUseTime`;
    // 55 shipped unit blueprints author the key, all under `Physics`. Currently
    // fails: the key is unread.
    const Blueprint authored{"XXB0905_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', FuelUseTime = 400, FuelRechargeRate = 5 },
            Air = { CanFly = true },
            SizeX = 1, SizeY = 0.5, SizeZ = 1,
        }
    )"};
    const auto bomber = rm::unitbp::loadFile(authored.path());
    REQUIRE(bomber.has_value());
    CHECK(bomber->airFuelRechargeRatePerSec == Catch::Approx(5.0f));

    const Blueprint bare{"XXB0906_unit.bp", R"(
        UnitBlueprint {
            Physics = { MotionType = 'RULEUMT_Air', FuelUseTime = 400 },
            Air = { CanFly = true },
            SizeX = 1, SizeY = 0.5, SizeZ = 1,
        }
    )"};
    const auto none = rm::unitbp::loadFile(bare.path());
    REQUIRE(none.has_value());
    CHECK(none->airFuelRechargeRatePerSec == Catch::Approx(0.0f));
}
