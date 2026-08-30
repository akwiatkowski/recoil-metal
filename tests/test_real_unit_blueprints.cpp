// Unit blueprints read against the WHOLE retail Supreme Commander corpus.
//
// Same posture as every other loader here: a synthetic fixture and its parser can
// share a misreading, so the real files are the authority. 568 of them, which is a
// corpus in the sense the models (1148) and maps (60) are.
//
// Assets are never committed (AGENT.md rule 3); they are extracted once from the
// retail install's units.scd, and these SKIP when that has not been done:
//
//   python3 -c "import zipfile,os;z=zipfile.ZipFile('.../gamedata/units.scd');\
//     [z.extract(i, os.path.expanduser('~/projects/llm/input/faf')) \
//      for i in z.infolist() if i.filename.lower().endswith(('.scm','.sca','.bp'))]"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/Scmap.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/unit/BuildTree.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "support/FxMatchers.hpp"

namespace {

using rm::unitdef::MotionType;

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

/// Every `<ID>_unit.bp` under the extracted corpus.
[[nodiscard]] std::vector<std::filesystem::path> blueprints() {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path root = unitRoot();
    if (root.empty() || !std::filesystem::exists(root)) {
        return found;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        constexpr std::string_view kTail = "_unit.bp";
        if (name.size() > kTail.size()
            && name.compare(name.size() - kTail.size(), kTail.size(), kTail) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

} // namespace

TEST_CASE("every retail unit blueprint parses into a definition", "[corpus]") {
    const std::vector<std::filesystem::path> files = blueprints();
    if (files.empty()) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }

    // The count itself is an assertion. Retail Forged Alliance ships 568 unit
    // blueprints (580 `.bp` in units.scd, the other twelve being projectiles and
    // props that live beside them), and a corpus that has quietly shrunk to a
    // handful would otherwise pass everything below.
    CHECK(files.size() == 568);

    std::vector<std::string> failures;
    std::map<MotionType, int> byMotion;
    std::map<std::string, std::filesystem::path> byId;
    std::vector<std::string> duplicates;
    std::vector<std::string> firingWithoutTargets;

    int mobile = 0;
    int statesMeshName = 0;
    int byConvention = 0;
    int resolvedByConvention = 0;
    int statesZeroScale = 0;
    int ordinaryShields = 0;
    float fastest = 0.0f;
    float largestRadius = 0.0f;

    for (const std::filesystem::path& path : files) {
        const auto def = rm::unitbp::loadFile(path);
        if (!def) {
            failures.push_back(path.filename().string() + ": " + def.error().message);
            continue;
        }

        byMotion[def->motion]++;
        if (def->isMobile()) {
            ++mobile;
        }
        if (def->shield.exists()) {
            ++ordinaryShields;
        }
        if (def->meshToElmos == 0.0f) {
            ++statesZeroScale;
        }
        fastest = std::max(fastest, def->speedElmosPerSecond);
        largestRadius = std::max(largestRadius, def->collisionRadiusElmos);
        for (const rm::unitdef::Weapon& weapon : def->weapons) {
            if (weapon.fires()
                && weapon.targetLayers == rm::unitdef::TargetLayerMask::None) {
                firingWithoutTargets.push_back(def->name + ":" + weapon.label);
            }
        }

        // Ids are what the rest of the content refers to a unit by, so two files
        // claiming one would mean a reference that cannot be resolved.
        if (const auto [it, inserted] = byId.emplace(def->name, path); !inserted) {
            duplicates.push_back(def->name + " in " + path.filename().string() + " and "
                                 + it->second.filename().string());
        }

        // HOW this unit names its mesh. Split from whether the mesh is on disk,
        // because the first is a fact about the format and the second is a fact
        // about which archives happen to be extracted here.
        if (def->modelPath.empty()) {
            ++byConvention;
            if (!rm::unitbp::resolveMesh(*def, path).empty()) {
                ++resolvedByConvention;
            }
        } else {
            ++statesMeshName;
            // Every one of them is a `.scm` path. See resolveMesh's header for the
            // `PlaceholderMeshName` trap that suggests otherwise.
            CHECK(def->modelPath.find(".scm") != std::string::npos);
        }

        // Invariants that hold for every unit, whatever it is. A definition
        // failing one of these has been misread, not merely under-specified.
        CHECK(!def->name.empty());
        CHECK(rm::test::asFloat(def->health) > 0.0f);
        CHECK(def->collisionRadiusElmos > 0.0f);
        CHECK(def->footprintSquaresX >= 1);
        CHECK(def->footprintSquaresZ >= 1);
        CHECK(def->speedElmosPerSecond >= 0.0f);
        CHECK(def->turnRateRadiansPerSecond >= 0.0f);
        // NOT `> 0`: two blueprints state a scale of zero, asserted below.
        CHECK(def->meshToElmos >= 0.0f);
    }

    CHECK(failures.empty());
    if (!failures.empty()) {
        for (const std::string& f : failures) {
            WARN(f);
        }
    }
    CHECK(duplicates.empty());
    CHECK(firingWithoutTargets.empty());

    // The motion census, which is the fact this milestone leans on hardest: the
    // class is the only thing a blueprint says about where a unit may go, and
    // these counts are what the mapping in UnitBlueprint.cpp was written against.
    CHECK(byMotion[MotionType::None] == 374);
    CHECK(byMotion[MotionType::Air] == 60);
    CHECK(byMotion[MotionType::Land] == 50);
    CHECK(byMotion[MotionType::Water] == 27);
    CHECK(byMotion[MotionType::Hover] == 19);
    CHECK(byMotion[MotionType::Amphibious] == 17);
    CHECK(byMotion[MotionType::SurfacingSub] == 13);
    CHECK(byMotion[MotionType::AmphibiousFloating] == 8);

    // `MaxSpeed` is stated by exactly the units that move, so "mobile" and "not
    // RULEUMT_None" should be the same set bar the two None-with-a-speed oddities
    // the corpus carries (a pair of blueprints declaring a speed they cannot use).
    CHECK(mobile == 196);

    // How the corpus names its geometry — a format fact, so it holds whatever is
    // extracted.
    CHECK(byConvention == 543);
    CHECK(statesMeshName == 25);

    // ...and how much of it is on disk. This one DOES depend on the extraction:
    // every `.scm` in units.scd is here, so all but twenty of the conventional
    // ones resolve. The twenty are placeholder and marker units with no geometry
    // of their own, which is the same legitimate answer an emitter prop gives.
    CHECK(resolvedByConvention == 523);

    // Two state a scale of zero, read as stated rather than corrected to one.
    CHECK(statesZeroScale == 2);
    CHECK(ordinaryShields == 19);

    // Sanity on the extremes, in the engine's units. The fastest thing in the
    // game cruises at 30 ogrids/s (the old 20.5 maximum was Physics.MaxSpeed, which is an
    // aircraft's landing speed), and the largest collision box is the
    // 21-ogrid Aeon Paragon-class structure.
    CHECK(fastest == Catch::Approx(30.0f * rm::scmap::kElmosPerOgrid));
    CHECK(largestRadius == Catch::Approx(0.5f * 21.0f * rm::scmap::kElmosPerOgrid));
}

TEST_CASE("UEB4302 discriminates the native footprint fallback", "[corpus]") {
    // This shipped blueprint is the C-109 discriminator: SizeX=1.75, no Footprint.SizeX,
    // SkirtSizeX=3 and SkirtOffsetX=-0.5. The native fallback ceil(1.75)=2 makes the
    // resulting skirt centred; a guessed footprint of one does not.
    const std::filesystem::path path = unitRoot() / "UEB4302" / "UEB4302_unit.bp";
    if (!std::filesystem::exists(path)) {
        SKIP("no UEB4302 blueprint at " + path.string());
    }
    const auto def = rm::unitbp::loadFile(path);
    REQUIRE(def.has_value());

    CHECK(def->footprintSquaresX == 2);
    CHECK(def->skirtSquaresX == Catch::Approx(3.0f));
    CHECK(def->skirtCentreOffsetSquaresX == Catch::Approx(0.0f));
}

TEST_CASE("a UEF medium tank reads as the vehicle it is", "[corpus]") {
    const std::filesystem::path path = unitRoot() / "UEL0201/UEL0201_unit.bp";
    if (!std::filesystem::exists(path)) {
        SKIP("no UEL0201 blueprint at " + path.string());
    }

    const auto def = rm::unitbp::loadFile(path);
    REQUIRE(def.has_value());

    CHECK(def->name == "UEL0201");
    CHECK(def->motion == MotionType::Land);
    CHECK(def->speedElmosPerSecond == Catch::Approx(3.4f * 8.0f));
    CHECK(rm::test::asFloat(def->health) == Catch::Approx(300.0f));
    CHECK(def->collisionRadiusElmos == Catch::Approx(3.6f));
    CHECK(def->meshToElmos == Catch::Approx(0.56f));

    // The mesh is found by convention, and note the case: the file on disk is
    // `UEL0201_LOD0.scm` in capitals while its own LOD 1 is lower case.
    const std::filesystem::path mesh = rm::unitbp::resolveMesh(*def, path);
    REQUIRE_FALSE(mesh.empty());
    CHECK(std::filesystem::exists(mesh));
}

TEST_CASE("retail T1 air factories expose scouts, interceptors and bombers", "[corpus]") {
    const auto read = [](std::string_view id) {
        const std::string name{id};
        return rm::unitbp::loadFile(unitRoot() / name / (name + "_unit.bp"));
    };
    if (!std::filesystem::exists(unitRoot() / "UEB0102/UEB0102_unit.bp")) {
        SKIP("no T1 air factory blueprints at " + unitRoot().string());
    }
    const auto tank = read("UEL0201");
    REQUIRE(tank.has_value());

    struct AirTree {
        const char* commander;
        const char* factory;
        const char* scout;
        const char* interceptor;
        const char* bomber;
    };
    constexpr AirTree trees[] = {
        {"UEL0001", "UEB0102", "UEA0101", "UEA0102", "UEA0103"},
        {"UAL0001", "UAB0102", "UAA0101", "UAA0102", "UAA0103"},
        {"URL0001", "URB0102", "URA0101", "URA0102", "URA0103"},
        {"XSL0001", "XSB0102", "XSA0101", "XSA0102", "XSA0103"},
    };
    for (const AirTree& tree : trees) {
        const auto commander = read(tree.commander);
        const auto factory = read(tree.factory);
        const auto scout = read(tree.scout);
        const auto interceptor = read(tree.interceptor);
        const auto bomber = read(tree.bomber);
        REQUIRE(commander.has_value());
        REQUIRE(factory.has_value());
        REQUIRE(scout.has_value());
        REQUIRE(interceptor.has_value());
        REQUIRE(bomber.has_value());

        CHECK(rm::unitdef::matchesExpression(commander->buildableCategory, *factory));
        CHECK(rm::unitdef::matchesExpression(factory->buildableCategory, *scout));
        CHECK(rm::unitdef::matchesExpression(factory->buildableCategory, *interceptor));
        CHECK(rm::unitdef::matchesExpression(factory->buildableCategory, *bomber));
        CHECK_FALSE(rm::unitdef::matchesExpression(factory->buildableCategory, *tank));
    }
}

TEST_CASE("retail T1 interceptors and bombers keep their target domains", "[corpus]") {
    struct AirPair {
        const char* interceptor;
        const char* bomber;
    };
    constexpr AirPair pairs[] = {
        {"UEA0102", "UEA0103"},
        {"UAA0102", "UAA0103"},
        {"URA0102", "URA0103"},
        {"XSA0102", "XSA0103"},
    };
    if (!std::filesystem::exists(unitRoot() / "UEA0102/UEA0102_unit.bp")) {
        SKIP("no T1 air blueprints at " + unitRoot().string());
    }

    for (const AirPair& pair : pairs) {
        const std::string interceptorId{pair.interceptor};
        const std::string bomberId{pair.bomber};
        const auto interceptor = rm::unitbp::loadFile(
            unitRoot() / interceptorId / (interceptorId + "_unit.bp"));
        const auto bomber =
            rm::unitbp::loadFile(unitRoot() / bomberId / (bomberId + "_unit.bp"));
        REQUIRE(interceptor.has_value());
        REQUIRE(bomber.has_value());

        const auto firing = [](const rm::unitdef::Weapon& weapon) { return weapon.fires(); };
        REQUIRE(std::any_of(interceptor->weapons.begin(), interceptor->weapons.end(), firing));
        REQUIRE(std::any_of(bomber->weapons.begin(), bomber->weapons.end(), firing));
        for (const rm::unitdef::Weapon& weapon : interceptor->weapons) {
            if (weapon.fires()) {
                CHECK(weapon.canTarget(true));
                CHECK_FALSE(weapon.canTarget(false));
            }
        }
        for (const rm::unitdef::Weapon& weapon : bomber->weapons) {
            if (weapon.fires()) {
                CHECK(weapon.canTarget(false));
                CHECK_FALSE(weapon.canTarget(true));
            }
        }
    }
}

TEST_CASE("an aircraft uses its Air source row instead of unioning unused rows", "[corpus]") {
    const std::filesystem::path path = unitRoot() / "DAA0206/DAA0206_unit.bp";
    if (!std::filesystem::exists(path)) {
        SKIP("no DAA0206 blueprint at " + path.string());
    }
    const auto def = rm::unitbp::loadFile(path);
    REQUIRE(def.has_value());
    const auto weapon = std::find_if(def->weapons.begin(), def->weapons.end(),
                                     [](const rm::unitdef::Weapon& candidate) {
                                         return candidate.fires();
                                     });
    REQUIRE(weapon != def->weapons.end());
    CHECK(weapon->canTarget(false));
    CHECK_FALSE(weapon->canTarget(true));
}

TEST_CASE("the retail UEF T2 shield keeps its authored bubble and energy drain", "[corpus]") {
    const std::filesystem::path path = unitRoot() / "UEB4202/UEB4202_unit.bp";
    if (!std::filesystem::exists(path)) {
        SKIP("no UEB4202 blueprint at " + path.string());
    }
    const auto def = rm::unitbp::loadFile(path);
    REQUIRE(def.has_value());
    CHECK(def->shield.maximum == rm::sim::Mag::fromInt(9000));
    CHECK(def->shield.radiusElmos == rm::sim::Fx::fromInt(104));
    CHECK(def->shield.verticalOffsetElmos == rm::sim::Fx::fromInt(-24));
    CHECK(def->shield.regenPerSecond == Catch::Approx(120.0f));
    CHECK(def->shield.regenDelay.value == Catch::Approx(1.1f));
    CHECK(def->shield.rechargeDelay.value == Catch::Approx(15.1f));
    CHECK(def->upkeepEnergyPerSecond == Catch::Approx(200.0f));
    rm::sim::UnitCatalog catalog;
    const rm::UnitTypeIndex type = catalog.add(&*def, rm::sim::TickRate{});
    CHECK(rm::test::asFloat(catalog.rates(type).upkeepEnergyPerTick) == Catch::Approx(20.0f));
}

TEST_CASE("the economy the blueprints state is the economy the game plays", "[corpus]") {
    // The numbers milestone 19 is built on, checked against the units the plan names
    // rather than against a range — a reader that found zeros everywhere would pass any
    // bounds check.
    struct Expected {
        const char* id;
        float mass;
        float energy;
        float buildTime;
        float producesMass;
        float producesEnergy;
    };
    // From the retail blueprints, read by hand.
    const Expected cases[] = {
        {"UEB1103", 36.0f, 360.0f, 60.0f, 2.0f, 0.0f},    // Mass Extractor
        {"UEB1101", 75.0f, 750.0f, 125.0f, 0.0f, 20.0f},  // Power Generator
        {"UEB0101", 240.0f, 2100.0f, 300.0f, 0.0f, 0.0f}, // Land Factory
    };

    for (const Expected& expected : cases) {
        const std::filesystem::path path =
            unitRoot() / expected.id / (std::string{expected.id} + "_unit.bp");
        if (!std::filesystem::exists(path)) {
            SKIP(std::string{"no "} + expected.id + " blueprint");
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());

        CHECK(rm::test::asFloat(def->buildCostMass) == Catch::Approx(expected.mass));
        CHECK(rm::test::asFloat(def->buildCostEnergy) == Catch::Approx(expected.energy));
        CHECK(rm::test::asFloat(def->buildTime) == Catch::Approx(expected.buildTime));
        CHECK(def->producesMassPerSecond == Catch::Approx(expected.producesMass));
        CHECK(def->producesEnergyPerSecond == Catch::Approx(expected.producesEnergy));
    }

    // A commander builds at 10, which is what makes the extractor above a six-second job.
    const std::filesystem::path acu = unitRoot() / "UEL0001/UEL0001_unit.bp";
    if (std::filesystem::exists(acu)) {
        const auto def = rm::unitbp::loadFile(acu);
        REQUIRE(def.has_value());
        CHECK(def->buildRate == Catch::Approx(10.0f));
        CHECK(def->isBuilder());
    }
}

TEST_CASE("the intel radii the corpus declares are the ones the loader reads", "[corpus]") {
    // ADR-037's census, checked rather than quoted — and it is checked HERE, through the
    // loader, precisely because counting these by grep gets it wrong: `JamRadius` is a
    // nested `{Max, Min}` table, so a naive non-greedy match for the Intel block stops
    // at its closing brace and loses every tag after it on five blueprints.
    //
    // These counts are what decided the first pass carries Vision, WaterVision, Radar and
    // Sonar and stops: the other nine FA intel types are declared too rarely to shape the
    // design, Omni's 17 being the closest call and needing rules of its own anyway.
    const std::vector<std::filesystem::path> files = blueprints();
    if (files.empty()) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }

    int vision = 0;
    int waterVision = 0;
    int radar = 0;
    int sonar = 0;
    float widestRadar = 0.0f;

    for (const std::filesystem::path& path : files) {
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());

        vision += (def->visionRadiusElmos > 0.0f) ? 1 : 0;
        waterVision += (def->waterVisionRadiusElmos > 0.0f) ? 1 : 0;
        radar += (def->radarRadiusElmos > 0.0f) ? 1 : 0;
        sonar += (def->sonarRadiusElmos > 0.0f) ? 1 : 0;
        widestRadar = std::max(widestRadar, def->radarRadiusElmos);
    }

    // Units that SEE, as against units that merely declare the tag. 391 blueprints carry
    // a VisionRadius and 36 of those carry a zero — walls, wreckage, the shields — so the
    // count below is the smaller number. A reader that defaulted a missing block to some
    // sight radius would push this to 568; one that failed to find the block would push
    // it to 0. Radar and sonar are never declared as zero: a unit either has the sense or
    // does not mention it.
    CHECK(vision == 355);
    CHECK(waterVision == 62);
    CHECK(radar == 57);
    CHECK(sonar == 67);

    // The Cybran T3 radar, 600 ogrids. Worth pinning because it is the number that says
    // radar is a different ORDER of thing from sight: 4800 elmos against the widest
    // vision on any unit, which is 800.
    CHECK(widestRadar == Catch::Approx(4800.0f));
}

TEST_CASE("named units carry the intel their blueprints state", "[corpus]") {
    struct Expected {
        const char* id;
        float vision;      ///< elmos, so the blueprint's ogrids x 8
        float waterVision;
        float radar;
        float sonar;
    };
    // Read by hand out of the retail blueprints.
    const Expected cases[] = {
        // The Cybran ACU: sees 26 ogrids on land and under water, hears 60, no radar.
        {"URL0001", 208.0f, 208.0f, 0.0f, 480.0f},
        // A medium tank states VisionRadius and nothing else — the common shape.
        {"UEL0201", 160.0f, 0.0f, 0.0f, 0.0f},
        // The Cybran T3 radar: a huge radar radius on a unit that barely sees.
        {"URB3104", 240.0f, 0.0f, 4800.0f, 0.0f},
    };

    for (const Expected& expected : cases) {
        const std::filesystem::path path =
            unitRoot() / expected.id / (std::string{expected.id} + "_unit.bp");
        if (!std::filesystem::exists(path)) {
            SKIP(std::string{"no "} + expected.id + " blueprint");
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());

        CHECK(def->visionRadiusElmos == Catch::Approx(expected.vision));
        CHECK(def->waterVisionRadiusElmos == Catch::Approx(expected.waterVision));
        CHECK(def->radarRadiusElmos == Catch::Approx(expected.radar));
        CHECK(def->sonarRadiusElmos == Catch::Approx(expected.sonar));
    }
}
