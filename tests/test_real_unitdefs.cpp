// Unit definitions read against the WHOLE BAR corpus.
//
// Same posture as every other loader here: a synthetic fixture and its parser
// can share a misreading, so the real files are the authority. The corpus is
// not committed (it is game content), so these SKIP when it is absent.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Movement.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path barRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home}
             / "projects/llm/games/forged-alliance-reborn/reference/BAR";
    }
    return {};
}

} // namespace

TEST_CASE("every BAR unit definition parses", "[corpus]") {
    const std::filesystem::path units = barRoot() / "units";
    if (units.empty() || !std::filesystem::exists(units)) {
        SKIP("no BAR unit corpus at " + units.string());
    }

    std::vector<std::string> failures;
    std::vector<std::string> refused;
    std::size_t total = 0;
    std::size_t unitCount = 0;
    std::size_t mobile = 0;
    std::size_t flying = 0;
    std::size_t withModel = 0;
    std::vector<std::string> turnless;

    // Every name the corpus declares, so a duplicate — two files claiming the
    // same unit — shows up rather than silently shadowing.
    std::map<std::string, std::filesystem::path> byName;
    std::vector<std::string> duplicates;

    for (const auto& entry : std::filesystem::recursive_directory_iterator{units}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }
        ++total;

        const auto defs = rm::unitdef::loadFileAll(entry.path());
        if (!defs) {
            // Refusing is CORRECT for the handful of files that generate their
            // units in a loop rather than declaring one: this reader parses Lua
            // data and does not evaluate Lua programs, and a guess there would
            // invent a unit named after whichever key came first.
            refused.push_back(entry.path().filename().string() + ": "
                              + defs.error().message);
            continue;
        }

        for (const rm::unitdef::UnitDef& unit : *defs) {
        const rm::unitdef::UnitDef* def = &unit;
        ++unitCount;

        if (def->name.empty()) {
            failures.push_back(entry.path().filename().string() + ": empty name");
        }
        if (!def->modelPath.empty()) {
            ++withModel;
        }
        if (def->isMobile()) {
            ++mobile;
            if (def->canFly) {
                ++flying;
            } else if (def->turnRateRadiansPerSecond <= 0.0f) {
                // Recorded rather than asserted. Aircraft genuinely have no
                // turn rate — they carry a `turnradius` instead — and a
                // handful of novelty units (dice, xmasball, a volcano
                // projectile) move without one either. Asserting per unit here
                // would be asserting that BAR contains no jokes.
                turnless.push_back(def->name);
            }
        }

        // Footprints are always at least the engine's minimum.
        CHECK(def->footprintSquaresX >= 2);
        CHECK(def->footprintSquaresZ >= 2);

        const auto [it, inserted] = byName.emplace(def->name, entry.path());
        if (!inserted) {
            duplicates.push_back(def->name);
        }
        }
    }

    INFO("parsed " << total << " files -> " << unitCount << " units, " << mobile << " mobile (" << flying
                   << " flying), " << withModel << " naming a model, " << turnless.size()
                   << " ground units with no turn rate, " << refused.size()
                   << " refused as generated");
    for (const std::string& failure : failures) {
        INFO(failure);
    }
    for (std::size_t i = 0; i < refused.size() && i < 8; ++i) {
        INFO("refused: " << refused[i]);
    }

    CHECK(failures.empty());
    CHECK(duplicates.empty());
    CHECK(total > 900);   // 968 at the time of writing
    // 955 of 968 are plain data. The rest build their units in a loop; if this
    // grows, BAR has moved to generating definitions and a reader that only
    // parses data is no longer enough.
    // 36 of 968 files are refused, and all three reasons are the reader's
    // contract rather than a gap: a value that needs evaluating
    // (`builddistance = range`), an expression calling into the engine
    // (`100 * Spring.GetModOptions()...`), or a file that builds its units in a
    // loop and so declares none literally. Guessing at any of them would invent
    // a number, which is the failure mode core/lua exists to avoid.
    //
    // The bound is loose on purpose: it catches the reader silently starting to
    // refuse ordinary units, not BAR gaining another scripted one.
    CHECK(refused.size() < 60);
    CHECK(unitCount > 900);  // 943 at the time of writing
    CHECK(mobile > 200);  // most of BAR is buildings, but plenty moves
    CHECK(flying > 20);
    // The turn-rate-less ground units are the joke ones; if this grows, the
    // loader has started missing a field real units use.
    CHECK(turnless.size() < 40);
}

TEST_CASE("known BAR units carry the values this engine hardcoded", "[corpus]") {
    // The four constants the sim shipped with came from reading armpw.lua by
    // hand. This is the check that the loader agrees with that reading — and
    // the reason those constants can now be retired.
    const std::filesystem::path pawn = barRoot() / "units/ArmBots/armpw.lua";
    if (!std::filesystem::exists(pawn)) {
        SKIP("no BAR unit corpus at " + pawn.string());
    }

    const auto def = rm::unitdef::loadFile(pawn);
    REQUIRE(def.has_value());

    CHECK(def->name == "armpw");
    CHECK(def->speedElmosPerSecond == rm::sim::kDefaultSpeedElmosPerSecond);
    CHECK(def->maxSlopeDegrees == 17.0f);
    CHECK(def->maxWaterDepthElmos == 12.0f);
    CHECK(def->turnRateRadiansPerSecond
          == Catch::Approx(rm::sim::kDefaultTurnRateRadiansPerSecond).margin(0.001));
}

TEST_CASE("a unit definition's model resolves on disk", "[corpus]") {
    const std::filesystem::path objects = barRoot() / "objects3d";
    const std::filesystem::path pawn = barRoot() / "units/ArmBots/armpw.lua";
    if (!std::filesystem::exists(objects) || !std::filesystem::exists(pawn)) {
        SKIP("no BAR corpus at " + barRoot().string());
    }

    const auto def = rm::unitdef::loadFile(pawn);
    REQUIRE(def.has_value());

    // `Units/ARMPW.s3o` against `objects3d/Units/armpw.s3o` — the case differs,
    // which is the whole reason resolution is not a path join.
    const std::filesystem::path model = rm::unitdef::resolveModel(objects, def->modelPath);
    REQUIRE_FALSE(model.empty());
    CHECK(model.extension() == ".s3o");
    CHECK(std::filesystem::exists(model));
}
