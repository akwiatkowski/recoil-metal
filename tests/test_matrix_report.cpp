// Offline AI-matrix reporting: aggregation order, kill tallying, JSON shape.
//
// Headless by construction — every function here takes plain data and a canned
// resolver, so no content, no map, and no match is needed to prove the report
// orders tech before price, credits only enemy kills, and escapes its strings.
#include <catch2/catch_test_macros.hpp>

#include "app/MatrixReport.hpp"

#include <sstream>
#include <string>
#include <vector>

using rm::app::MatrixArmyKills;
using rm::app::MatrixBuilt;
using rm::app::MatrixDoc;
using rm::app::MatrixKill;
using rm::app::MatrixSnapshot;
using rm::app::MatrixSnapshotArmy;
using rm::app::MatrixTypeInfo;
using rm::app::MatrixTypeResolver;
using rm::app::summarizeMatrixBuilt;
using rm::app::tallyMatrixKills;
using rm::app::writeMatrixJson;
using rm::UnitTypeIndex;

namespace {

// Four types spanning the sort keys: a cheap T1, a mid T2, an expensive T3, and a
// techless commander. The resolver is canned so the tests prove the ORDER, not the
// catalog.
[[nodiscard]] MatrixTypeResolver resolve() {
    return [](UnitTypeIndex type) {
        switch (type) {
        case 1: return MatrixTypeInfo{.blueprint = "units/T1", .tech = 1,
                                      .mass = 50.0, .energy = 500.0, .buildTime = 100.0,
                                      .description = "Light Tank"};
        case 2: return MatrixTypeInfo{.blueprint = "units/T2", .tech = 2,
                                      .mass = 200.0, .energy = 2000.0, .buildTime = 800.0,
                                      .description = "Mass Extractor", .mobile = false};
        case 3: return MatrixTypeInfo{.blueprint = "units/T3", .tech = 3,
                                      .mass = 1000.0, .energy = 10000.0, .buildTime = 5000.0,
                                      .description = "Heavy Assault Bot", .mobile = true};
        case 4: return MatrixTypeInfo{.blueprint = "units/ACU", .tech = 0,
                                      .mass = 0.0, .energy = 0.0, .buildTime = 0.0,
                                      .description = "Armored Command Unit", .mobile = true,
                                      .commander = true};
        default: return MatrixTypeInfo{};
        }
    };
}

} // namespace

TEST_CASE("built rows order tech first, then mass, then energy", "[matrix]") {
    // A hundred T1s still lose to one T3: tech is the first key, not count or cost.
    const std::vector<MatrixBuilt> built = {
        {.type = 1, .army = 0}, {.type = 1, .army = 0}, {.type = 3, .army = 0},
        {.type = 2, .army = 0}, {.type = 4, .army = 0},
    };
    const auto rows = summarizeMatrixBuilt(built, 0, resolve());
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].info.tech == 3);
    CHECK(rows[1].info.tech == 2);
    CHECK(rows[2].info.tech == 1);
    CHECK(rows[2].count == 2);
    // The techless commander sorts last despite costing nothing to compare against.
    CHECK(rows[3].info.tech == 0);
}

TEST_CASE("built summary is per army and skips empty types", "[matrix]") {
    const std::vector<MatrixBuilt> built = {{.type = 1, .army = 0}, {.type = 2, .army = 1}};
    const auto zero = summarizeMatrixBuilt(built, 0, resolve());
    REQUIRE(zero.size() == 1);
    CHECK(zero[0].info.tech == 1);
    const auto one = summarizeMatrixBuilt(built, 1, resolve());
    REQUIRE(one.size() == 1);
    CHECK(one[0].info.tech == 2);
}

TEST_CASE("kills count enemy units only, worth is the victims' cost", "[matrix]") {
    const std::vector<MatrixKill> kills = {
        // Army 0 kills two T1s and a T2 of army 1: three kills.
        {.victimType = 1, .victimArmy = 1, .killerArmy = 0},
        {.victimType = 1, .victimArmy = 1, .killerArmy = 0},
        {.victimType = 2, .victimArmy = 1, .killerArmy = 0},
        // A self-kill and a killer-unknown death are losses, not scores.
        {.victimType = 1, .victimArmy = 0, .killerArmy = 0},
        {.victimType = 3, .victimArmy = 1, .killerArmy = -1},
    };
    const MatrixArmyKills tally = tallyMatrixKills(kills, 0, resolve());
    CHECK(tally.kills == 3);
    // Worth is what died: 2x50 + 200 mass, 2x500 + 2000 energy — not damage dealt,
    // so overkill cannot inflate it and the expensive T3 nobody killed adds nothing.
    CHECK(tally.massWorth == 300.0);
    CHECK(tally.energyWorth == 3000.0);
    CHECK(tally.buildTimeWorth == 1000.0);
    REQUIRE(tally.victims.size() == 2);
    CHECK(tally.victims[0].info.tech == 2);
    CHECK(tally.victims[1].count == 2);
}

TEST_CASE("matrix JSON carries config, outcome, rows, and snapshots", "[matrix]") {
    MatrixDoc doc;
    doc.commit = "abc123";
    doc.map = "SCMP_009";
    doc.tickCap = 36000;
    doc.ticksPlayed = 1234;
    doc.wallSeconds = 56.0;
    doc.finished = true;
    doc.hasWinner = false; // a draw: finished with no winner is a real outcome
    doc.armies.push_back(rm::app::MatrixArmyOutcome{
        .army = 0, .personality = "turtle", .faction = "uef",
        .generatedMass = 1000.0, .generatedEnergy = 2000.0, .alive = 5,
        .built = summarizeMatrixBuilt({{MatrixBuilt{.type = 1, .army = 0}}}, 0, resolve()),
        .kills = tallyMatrixKills({{MatrixKill{.victimType = 2, .victimArmy = 1, .killerArmy = 0}}},
                                  0, resolve()),
    });
    doc.snapshots.push_back(MatrixSnapshot{
        .tick = 100, .wallSeconds = 15.0,
        .armies = {MatrixSnapshotArmy{.alive = 3, .incomeMassPerSecond = 2.0,
                                      .incomeEnergyPerSecond = 20.0, .kills = 1}}});

    std::ostringstream json;
    writeMatrixJson(json, doc);
    const std::string text = json.str();
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"commit\": \"abc123\"") != std::string::npos);
    CHECK(text.find("\"winner\": null") != std::string::npos);
    CHECK(text.find("\"personality\": \"turtle\"") != std::string::npos);
    CHECK(text.find("\"kills\": 1") != std::string::npos);
    CHECK(text.find("\"tick\": 100") != std::string::npos);
}

TEST_CASE("standing rows carry the name a player reads, beside the built ledger",
          "[matrix]") {
    // The roster the console tables read: what an army has ON THE FIELD, per type, with
    // the blueprint's own description — a path is a worse name than "Mass Extractor",
    // and the console has no catalog of its own to look one up in.
    const std::vector<std::pair<UnitTypeIndex, std::size_t>> census{{2, 6}, {4, 1}};
    const auto standing = rm::app::summarizeMatrixTypes(census, resolve());
    REQUIRE(standing.size() == 2);
    CHECK(standing[0].info.description == "Mass Extractor");  // tech 2 outranks the ACU's 0
    CHECK(standing[0].count == 6);
    CHECK(standing[1].info.description == "Armored Command Unit");

    MatrixDoc doc;
    doc.armies.push_back(rm::app::MatrixArmyOutcome{
        .army = 0, .personality = "tech", .faction = "uef", .alive = 7,
        .built = summarizeMatrixBuilt({{MatrixBuilt{.type = 3, .army = 0}}}, 0, resolve()),
        .standing = standing,
    });
    std::ostringstream json;
    writeMatrixJson(json, doc);
    const std::string text = json.str();
    CHECK(text.find("\"standing\": [") != std::string::npos);
    CHECK(text.find("\"description\": \"Mass Extractor\"") != std::string::npos);
    CHECK(text.find("\"description\": \"Heavy Assault Bot\"") != std::string::npos);
    // The two flags the tables split and filter on: a power farm is not an army, and the
    // commander outprices everything without being a choice anyone made.
    CHECK(text.find("\"mobile\": false, \"commander\": false") != std::string::npos);
    CHECK(text.find("\"mobile\": true, \"commander\": true") != std::string::npos);
    // The two lists are separate facts: production never shrinks, the roster does.
    CHECK(text.find("\"standing\"") < text.find("\"built\""));
}

TEST_CASE("matrix JSON escapes hostile strings", "[matrix]") {
    MatrixTypeResolver evil = [](UnitTypeIndex) {
        return MatrixTypeInfo{.blueprint = "we\"ird\\name", .tech = 1};
    };
    const auto rows = summarizeMatrixBuilt({{MatrixBuilt{.type = 9, .army = 0}}}, 0, evil);
    std::ostringstream json;
    json << "[";
    // The escaping helper is what the doc writer uses; a blueprint with a quote and a
    // backslash must survive the round trip rather than break the document.
    rm::app::writeMatrixJsonString(json, rows[0].info.blueprint);
    json << "]";
    CHECK(json.str() == "[\"we\\\"ird\\\\name\"]");
}
