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
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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

    int mobile = 0;
    int statesMeshName = 0;
    int byConvention = 0;
    int resolvedByConvention = 0;
    int statesZeroScale = 0;
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
        if (def->meshToElmos == 0.0f) {
            ++statesZeroScale;
        }
        fastest = std::max(fastest, def->speedElmosPerSecond);
        largestRadius = std::max(largestRadius, def->collisionRadiusElmos);

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
        CHECK(def->health > 0.0f);
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

    // Sanity on the extremes, in the engine's units. The fastest thing in the
    // game is an air unit at 20.5 ogrids/s, and the largest collision box is the
    // 21-ogrid Aeon Paragon-class structure.
    CHECK(fastest == Catch::Approx(20.5f * rm::scmap::kElmosPerOgrid));
    CHECK(largestRadius == Catch::Approx(0.5f * 21.0f * rm::scmap::kElmosPerOgrid));
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
    CHECK(def->health == Catch::Approx(300.0f));
    CHECK(def->collisionRadiusElmos == Catch::Approx(3.6f));
    CHECK(def->meshToElmos == Catch::Approx(0.56f));

    // The mesh is found by convention, and note the case: the file on disk is
    // `UEL0201_LOD0.scm` in capitals while its own LOD 1 is lower case.
    const std::filesystem::path mesh = rm::unitbp::resolveMesh(*def, path);
    REQUIRE_FALSE(mesh.empty());
    CHECK(std::filesystem::exists(mesh));
}
