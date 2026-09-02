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

    // 0.07 to ogrids and 8 to elmos, as ONE factor.
    CHECK(def->meshToElmos == Approx(0.56f));
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
                ReclaimTimeMultiplier = 1,
            },
        }
    )"};
    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());

    // 200 * 0.9 and 1000 * 0.5, computed at parse time so the sim never multiplies floats.
    CHECK(rm::test::asFloat(def->wreckMass) == Approx(180.0f));
    CHECK(rm::test::asFloat(def->wreckEnergy) == Approx(500.0f));

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
        const std::string source = std::string{"UnitBlueprint { SizeX = 1, SizeZ = 1, Physics = { "}
                                 + "MotionType = '" + std::string{c.motion} + "' } }";
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
    CHECK_FALSE(def->weapons[1].targetsProjectiles);
    CHECK(def->weapons[1].fires());
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

TEST_CASE("a personal shield is not imported as a protective bubble") {
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
                    ShieldRegenRate = 2,
                    ShieldRegenStartTime = 1,
                    ShieldRechargeTime = 75,
                },
            },
        }
    )"};

    const auto def = rm::unitbp::loadFile(bp.path());
    REQUIRE(def.has_value());
    CHECK_FALSE(def->shield.exists());
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
