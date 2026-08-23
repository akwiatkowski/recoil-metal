#include "core/data/ArmorDefs.hpp"

#include <catch2/catch_test_macros.hpp>

#include "core/unit/Armor.hpp"

#include <string_view>

using rm::ArmorClass;
using rm::data::ArmorTable;
using rm::data::armorTableFromSource;
using rm::kDefaultArmor;
using rm::sim::Mag;
using rm::unitdef::damageFromMatrix;
using rm::unitdef::DamageProfile;

namespace {

/// The shipped retail table, verbatim from `lua.scd:lua/armordefinition.lua` — `#` comments,
/// trailing commas and all.
///
/// COPIED RATHER THAN LOADED, because the shipped archive lives on an external drive and the
/// asset-gated tests skip without it. The point of this fixture is the *format*, and the format
/// is what a reader gets wrong.
constexpr std::string_view kRetail = R"LUA(
#****************************************************************************
#**  File     :  /lua/armordefinition.lua
#****************************************************************************
#
# Armor Type Definitions
#

armordefinition = {

    {   # Armor Type Name
        'Default',

        # Armor Definition
        'Normal 1.0',
    },

    {   # Armor Type Name
        'Normal',

        # Armor Definition
        'Normal 1.0',
    },

    {   # Armor Type Name
        'Light',

        # Armor Definition
        'Normal 1.0',
    },
    {   # Armor Type Name
        'Commander',

        # Armor Definition
        'Normal 1.0',
        'Overcharge 0.033333',
        'Deathnuke 0.05',
    },
    {   # Armor Type Name
        'Structure',

        # Armor Definition
        'Normal 1.0',
        'Overcharge 0.066666',
        'Deathnuke 0.01',
    },
    {
        # Armor Type name
        'Experimental',

        # Armor Definition
        'ExperimentalFootfall 0.0',
    },
}
)LUA";

} // namespace

TEST_CASE("the shipped retail armour table parses") {
    const ArmorTable table = armorTableFromSource(kRetail);

    // Six content classes plus the engine's shield pseudo-class. `Default` is class 0 rather
    // than another entry, while `Shield` must exist even though retail does not author a block
    // for it: damage profiles need a stable class to distinguish bubble damage from hull damage.
    CHECK(table.registry.size() == 7);
    for (const std::string_view name :
         {"Default", "Normal", "Light", "Commander", "Structure", "Experimental", "Shield"}) {
        CHECK(table.registry.knows(name));
    }

    // **FIVE non-1.0 entries in the whole of retail Forged Alliance.** Every `Normal 1.0` row is
    // dropped, because 1.0 is what an absent row already means. If this number moves, the game's
    // balance moved with it.
    CHECK(table.multipliers.size() == 5);
}

TEST_CASE("retail and FAF ship materially different armour tables") {
    // THE REASON THIS IS PARSED AND NOT HARDCODED. `ADR-033` was written against FAF's copy;
    // the engine mounts retail. They disagree on the class LIST and on the numbers, and a
    // constant compiled in would be silently wrong for one of them.
    const ArmorTable retail = armorTableFromSource(kRetail);

    // FAF's copy, reduced to the rows that differ. Same file name, same format, different game.
    constexpr std::string_view kFaf = R"LUA(
armordefinition = {
    { 'Default', 'Normal 1.0', },
    { 'Structure', 'Normal 1.0', 'Overcharge 0.25', 'Deathnuke 0.032', },
    { 'TMD', 'Normal 1.0', 'Overcharge 0.25', 'TacticalMissile 0.55', },
}
)LUA";
    const ArmorTable faf = armorTableFromSource(kFaf);

    // FAF knows a class retail has never heard of.
    CHECK_FALSE(retail.registry.knows("TMD"));
    CHECK(faf.registry.knows("TMD"));

    // And a structure takes nearly four times as much Overcharge damage under FAF: 0.25 against
    // retail's 0.066666. Asserted through the profile, which is where it would actually bite.
    const auto overchargeAgainstStructure = [](const ArmorTable& table) {
        const DamageProfile profile =
            damageFromMatrix(Mag::fromInt(12000), "Overcharge", table.multipliers);
        return profile.against(table.registry.classFor("Structure"));
    };
    CHECK(overchargeAgainstStructure(retail) < overchargeAgainstStructure(faf));
    CHECK(overchargeAgainstStructure(retail).floorToInt() == 799);   // 12000 * 0.066666
    CHECK(overchargeAgainstStructure(faf).floorToInt() == 3000);     // 12000 * 0.25
}

TEST_CASE("a row of exactly 1.0 is dropped rather than stored") {
    // Every class in retail states `Normal 1.0` explicitly, and storing those would spend an
    // override slot per class to say "full damage" — which is what `base` already says. This is
    // what keeps the shipped table down to 5 entries instead of 11.
    const ArmorTable table = armorTableFromSource(kRetail);
    for (const rm::unitdef::ArmorMultiplier& row : table.multipliers) {
        CHECK(row.multiplier != 1.0f);
    }
}

TEST_CASE("the table survives content it cannot read") {
    // A caller mounting content with no armour definition, or a corrupt one, must get the
    // pre-armour engine rather than a crash or an empty registry that resolves nothing.
    for (const std::string_view broken : {"", "armordefinition = {", "notatable = 3",
                                          "armordefinition = { { }, }"}) {
        const ArmorTable table = armorTableFromSource(broken);
        CHECK(table.registry.size() == 1);  // `default` only
        CHECK(table.multipliers.empty());
        CHECK(table.registry.classFor("Structure") == kDefaultArmor);
    }
}

TEST_CASE("a malformed row costs one multiplier, not the file") {
    // Refusing the whole file would cost every armour class in the game; skipping the row costs
    // one multiplier. The playable failure is the first one.
    constexpr std::string_view kSource = R"LUA(
armordefinition = {
    { 'Structure', 'Overcharge', 'Deathnuke 0.01', 'Nonsense', },
}
)LUA";
    const ArmorTable table = armorTableFromSource(kSource);

    CHECK(table.registry.knows("Structure"));
    REQUIRE(table.multipliers.size() == 1);
    CHECK(table.multipliers[0].damageType == "Deathnuke");
}

TEST_CASE("multipliers resolve against the registry that numbered them") {
    // The two halves are returned together precisely because a row names a class by index, and
    // an index is meaningless against a different registry. This is that contract, asserted.
    const ArmorTable table = armorTableFromSource(kRetail);
    const ArmorClass structure = table.registry.classFor("Structure");
    const ArmorClass commander = table.registry.classFor("Commander");

    REQUIRE(structure != kDefaultArmor);
    REQUIRE(commander != kDefaultArmor);
    CHECK(structure != commander);

    // A commander's death blast against a building: retail's `Structure / Deathnuke 0.01`.
    const DamageProfile deathnuke =
        damageFromMatrix(Mag::fromInt(45000), "Deathnuke", table.multipliers);
    CHECK(deathnuke.against(structure).floorToInt() == 450);
    // And against another commander, which retail rates far higher: 0.05.
    CHECK(deathnuke.against(commander).floorToInt() == 2250);
    // Anything the table says nothing about takes it in full.
    CHECK(deathnuke.against(table.registry.classFor("Light")) == Mag::fromInt(45000));
}
