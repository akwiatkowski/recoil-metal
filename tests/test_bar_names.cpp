// BAR keeps display names outside its unit files, in language/<code>/units.json —
// `units.descriptions.armstump` is "Medium Assault Tank". This extractor reads that one
// fact out of the file's text without a JSON dependency: the file is two flat string maps,
// and a scanner that honours quoting is all "find one key" needs.
#include <catch2/catch_test_macros.hpp>

#include "core/unit/BarNames.hpp"

namespace {

constexpr std::string_view kUnitsJson = R"({
  "units": {
    "factions": { "arm": "Armada" },
    "names": {
      "armstump": "Stout",
      "armpw": "Pawn"
    },
    "descriptions": {
      "armstump": "Medium Assault Tank",
      "armpw": "Light Infantry Bot \"quoted\"",
      "armart": "Mobile Artillery"
    }
  }
})";

} // namespace

TEST_CASE("a unit's description is read out of BAR's units.json", "[unitdef][bar]") {
    CHECK(rm::unitdef::barUnitName(kUnitsJson, "armstump") == "Medium Assault Tank");
    CHECK(rm::unitdef::barUnitName(kUnitsJson, "armart") == "Mobile Artillery");
}

TEST_CASE("an escaped quote inside a name survives extraction", "[unitdef][bar]") {
    CHECK(rm::unitdef::barUnitName(kUnitsJson, "armpw") == "Light Infantry Bot \"quoted\"");
}

TEST_CASE("a missing key or a broken file yields empty rather than a guess", "[unitdef][bar]") {
    CHECK(rm::unitdef::barUnitName(kUnitsJson, "corak").empty());
    CHECK(rm::unitdef::barUnitName("", "armstump").empty());
    CHECK(rm::unitdef::barUnitName("{\"units\": {\"names\": {", "armstump").empty());

    // A key that appears only in `names` (flavour, "Stout") is NOT the answer: the engine's
    // one naming vocabulary is the TYPE name, per the FA Description decision — so a unit
    // with no description entry stays nameless and the interface falls back to its id.
    constexpr std::string_view namesOnly =
        R"({"units": {"names": {"armfly": "Fly"}, "descriptions": {}}})";
    CHECK(rm::unitdef::barUnitName(namesOnly, "armfly").empty());
}
