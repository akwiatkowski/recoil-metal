#include "core/unit/Armor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using rm::ArmorClass;
using rm::unitdef::ArmorMultiplier;
using rm::unitdef::ArmorRegistry;
using rm::unitdef::DamageProfile;
using rm::unitdef::damageFromMatrix;
using rm::unitdef::flatDamage;
using rm::unitdef::kDefaultArmor;
using rm::sim::Mag;

namespace {

// Supreme Commander's own armour classes, in the order `lua/armordefinition.lua` lists them.
//
// DELIBERATELY UNSORTED AND MIXED-CASE, because that is the point of several tests below: the
// registry must not care what order a file happens to be written in, and the corpus really does
// spell one class two ways (`14-blueprint-census.md §8.7`).
constexpr std::array<std::string_view, 9> kFaClasses{
    "Default", "Normal", "Light",         "Commander", "Structure",
    "Experimental", "ExperimentalStructure", "ASF", "TMD",
};

/// The non-1.0 rows of `armordefinition.lua:48-118`, resolved against a registry.
///
/// TEN ENTRIES. That count is the measurement `ADR-033` rests on, so it is written out here in
/// full rather than summarised: if the table ever grows, this array is what makes the growth
/// visible instead of silently changing what `kMaxOverrides` needs to be.
[[nodiscard]] std::vector<ArmorMultiplier> faMatrix(const ArmorRegistry& registry) {
    const ArmorClass structure = registry.classFor("Structure");
    const ArmorClass expStructure = registry.classFor("ExperimentalStructure");
    const ArmorClass experimental = registry.classFor("Experimental");
    const ArmorClass asf = registry.classFor("ASF");
    const ArmorClass tmd = registry.classFor("TMD");

    return {
        {structure, "Overcharge", 0.25f},
        {structure, "Deathnuke", 0.032f},
        {expStructure, "Overcharge", 0.25f},
        {expStructure, "Deathnuke", 0.032f},
        {expStructure, "ExperimentalFootfall", 0.0f},
        {experimental, "ExperimentalFootfall", 0.0f},
        {asf, "CzarBeam", 0.25f},
        {tmd, "Overcharge", 0.25f},
        {tmd, "Deathnuke", 0.032f},
        {tmd, "TacticalMissile", 0.55f},
    };
}

} // namespace

TEST_CASE("default is always armour class zero") {
    const ArmorRegistry empty;
    CHECK(empty.size() == 1);
    CHECK(empty.classFor("default") == kDefaultArmor);
    CHECK(empty.name(kDefaultArmor) == "default");

    // Listing it explicitly must not create a second one. Supreme Commander's table opens with
    // `Default`, so this is the ordinary case rather than a defensive one.
    const ArmorRegistry withDefault = ArmorRegistry::fromNames(kFaClasses);
    CHECK(withDefault.size() == kFaClasses.size());  // 9 names, one of which IS default
    CHECK(withDefault.classFor("Default") == kDefaultArmor);
}

TEST_CASE("armour class numbering does not depend on the order names were seen") {
    // THE PROPERTY THAT MAKES THIS SAFE FOR A REPLAY. A first-seen interning scheme would give
    // these two registries different numbering, so a content edit that merely reordered a file
    // would change what a recorded match means.
    constexpr std::array<std::string_view, 4> forwards{"asf", "commander", "structure", "tmd"};
    constexpr std::array<std::string_view, 4> backwards{"tmd", "structure", "commander", "asf"};

    const ArmorRegistry a = ArmorRegistry::fromNames(forwards);
    const ArmorRegistry b = ArmorRegistry::fromNames(backwards);

    REQUIRE(a.size() == b.size());
    for (const std::string_view name : forwards) {
        CHECK(a.classFor(name) == b.classFor(name));
    }
    // And they are genuinely distinct classes, not all falling back to default.
    CHECK(a.classFor("asf") != a.classFor("tmd"));
    CHECK(a.classFor("asf") != kDefaultArmor);
}

TEST_CASE("armour class names are case-folded, which Recoil's are not") {
    // The measured case: `Structure` on 222 units and `STRUCTURE` on one
    // (`14-blueprint-census.md §8.7`). Recoil's case-sensitive keys would give that one unit a
    // private class with no multipliers defined against it — so it would take full Overcharge
    // damage where every other building takes a quarter.
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);

    const ArmorClass structure = registry.classFor("Structure");
    CHECK(registry.classFor("STRUCTURE") == structure);
    CHECK(registry.classFor("structure") == structure);
    CHECK(structure != kDefaultArmor);

    // And the two spellings do not become two classes.
    constexpr std::array<std::string_view, 2> bothSpellings{"Structure", "STRUCTURE"};
    CHECK(ArmorRegistry::fromNames(bothSpellings).size() == 2);  // default + structure
}

TEST_CASE("an unknown armour class falls back to default rather than to nothing") {
    // Recoil's behaviour (`WeaponDef.cpp:432-437` skips keys it does not know), and the right
    // one for content: a blueprint naming a class the game does not define is a typo or a
    // half-installed mod, and "ordinary armour" is playable where "immune" is not.
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    CHECK(registry.classFor("NoSuchClass") == kDefaultArmor);

    // `knows` is what an importer uses to tell a typo from a genuine `default`, which
    // `classFor` alone cannot do.
    CHECK_FALSE(registry.knows("NoSuchClass"));
    CHECK(registry.knows("Default"));
    CHECK(registry.knows("tmd"));
}

TEST_CASE("a profile with no overrides answers the same for every class") {
    const DamageProfile flat = flatDamage(Mag::fromInt(100));
    CHECK(flat.overrideCount == 0);
    CHECK(flat.against(kDefaultArmor) == Mag::fromInt(100));
    CHECK(flat.against(7) == Mag::fromInt(100));
    CHECK(flat.harmful());
}

TEST_CASE("an override applies to its own class and to no other") {
    DamageProfile profile = flatDamage(Mag::fromInt(100));
    REQUIRE(profile.addOverride(3, Mag::fromInt(25)));

    CHECK(profile.against(3) == Mag::fromInt(25));
    CHECK(profile.against(4) == Mag::fromInt(100));
    CHECK(profile.against(kDefaultArmor) == Mag::fromInt(100));
}

TEST_CASE("a profile refuses what it cannot represent instead of dropping it") {
    DamageProfile profile = flatDamage(Mag::fromInt(100));

    // An override against class 0 is what `base` already means. Two ways to say one thing is
    // how they come to disagree.
    CHECK_FALSE(profile.addOverride(kDefaultArmor, Mag::fromInt(5)));
    CHECK(profile.overrideCount == 0);

    // Re-stating a class overwrites rather than spending a second slot.
    REQUIRE(profile.addOverride(2, Mag::fromInt(10)));
    REQUIRE(profile.addOverride(2, Mag::fromInt(20)));
    CHECK(profile.overrideCount == 1);
    CHECK(profile.against(2) == Mag::fromInt(20));

    // And a full profile SAYS SO. This is the signal that `kMaxOverrides` needs re-measuring
    // against BAR content, and it is the reason it is a `bool` rather than `void`.
    for (ArmorClass armor = 3; armor < 3 + DamageProfile::kMaxOverrides; ++armor) {
        profile.addOverride(armor, Mag::fromInt(1));
    }
    CHECK(profile.overrideCount == DamageProfile::kMaxOverrides);
    CHECK_FALSE(profile.addOverride(200, Mag::fromInt(1)));
}

TEST_CASE("a profile is harmful when any of its overrides is, not only its base") {
    DamageProfile antiShield;
    antiShield.base = Mag{};
    REQUIRE(antiShield.addOverride(5, Mag::fromInt(500)));

    // FA's `FAF_AntiShield` damage type: nothing to a hull, a great deal to a shield. A
    // `harmful()` that only looked at `base` would classify it as a weapon that does nothing.
    CHECK(antiShield.harmful());
    CHECK(antiShield.against(kDefaultArmor) == Mag{});
    CHECK(antiShield.against(5) == Mag::fromInt(500));
}

TEST_CASE("Forged Alliance's damage matrix transposes into a profile") {
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    const std::vector<ArmorMultiplier> matrix = faMatrix(registry);

    const ArmorClass structure = registry.classFor("Structure");
    const ArmorClass normal = registry.classFor("Normal");

    // An Overcharge shot: quarter damage to a structure, full damage to everything else. This
    // is `02 §9.6`'s "clean transpose" made into an assertion.
    const DamageProfile overcharge =
        damageFromMatrix(Mag::fromInt(12000), "Overcharge", matrix);
    CHECK(overcharge.against(structure) == Mag::fromInt(3000));
    CHECK(overcharge.against(normal) == Mag::fromInt(12000));
    CHECK(overcharge.against(kDefaultArmor) == Mag::fromInt(12000));

    // Three classes carry an Overcharge multiplier — Structure, ExperimentalStructure, TMD —
    // and that 3 is the worst case in the whole game. It is what sizes `kMaxOverrides`.
    CHECK(overcharge.overrideCount == 3);
}

TEST_CASE("the overwhelmingly common weapon acquires no overrides at all") {
    // 454 of the 494 shipped weapons are `DamageType = "Normal"` (`02 §9.6`), and Normal's
    // multiplier is 1.0 against every class — so flattening the matrix leaves them exactly the
    // scalar they were. This is the measurement that makes the sparse form right rather than
    // merely compact, so it is a test and not a comment.
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    const std::vector<ArmorMultiplier> matrix = faMatrix(registry);

    const DamageProfile ordinary = damageFromMatrix(Mag::fromInt(250), "Normal", matrix);
    CHECK(ordinary.overrideCount == 0);
    CHECK(ordinary == flatDamage(Mag::fromInt(250)));

    // A damage type the table says nothing about is the same story.
    const DamageProfile unlisted = damageFromMatrix(Mag::fromInt(250), "EMP", matrix);
    CHECK(unlisted.overrideCount == 0);
}

TEST_CASE("an unlisted armour/damage pair means full damage, not none") {
    // The reading that would break the game if it were backwards. `Experimental` lists ONLY
    // `ExperimentalFootfall 0.0`; if an absent row meant 0.0, every experimental unit would be
    // immune to every gun in the game.
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    const std::vector<ArmorMultiplier> matrix = faMatrix(registry);
    const ArmorClass experimental = registry.classFor("Experimental");

    const DamageProfile shell = damageFromMatrix(Mag::fromInt(400), "Normal", matrix);
    CHECK(shell.against(experimental) == Mag::fromInt(400));

    // And the one row it does state is honoured: a footfall does nothing to it.
    const DamageProfile footfall =
        damageFromMatrix(Mag::fromInt(4000), "ExperimentalFootfall", matrix);
    CHECK(footfall.against(experimental) == Mag{});
}

TEST_CASE("a multiplier is applied to the raw magnitude, not through a fixed-point rounding") {
    // `Mag * Fx` would round 0.032 into Q18.14 first — 524/16384 = 0.031982 — and apply a
    // 0.06 % error to a commander's death blast. Scaling the raw and rounding once is exact to
    // the last representable step, and this is what says so.
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    const std::vector<ArmorMultiplier> matrix = faMatrix(registry);
    const ArmorClass structure = registry.classFor("Structure");

    // A commander's death blast against a structure: 45,000 x 0.032, which is 1,440 EXACTLY.
    // The row is picked because the answer is checkable by hand rather than only by
    // construction.
    const DamageProfile deathnuke = damageFromMatrix(Mag::fromInt(45000), "Deathnuke", matrix);
    CHECK(deathnuke.against(structure).floorToInt() == 1440);

    // And here is what the fixed-point route would have produced, so the claim is a comparison
    // rather than an assertion about our own output. `Mag * Fx` must round the multiplier into
    // Q18.14 first: 0.032 becomes 524/16384 = 0.031982..., and 45,000 of anything is enough for
    // that 0.06 % to cross a whole unit.
    const Mag throughFixedPoint = Mag::fromInt(45000) * rm::sim::Fx::fromRatio(32, 1000);
    CHECK(throughFixedPoint.floorToInt() == 1439);
    CHECK(deathnuke.against(structure) != throughFixedPoint);
}

TEST_CASE("the damage type is matched without regard to case") {
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    const std::vector<ArmorMultiplier> matrix = faMatrix(registry);
    const ArmorClass structure = registry.classFor("Structure");

    // Same reason class names are folded: the corpus spells things more than one way, and a
    // case-sensitive match would silently give a weapon full damage against everything.
    const DamageProfile shouted = damageFromMatrix(Mag::fromInt(12000), "OVERCHARGE", matrix);
    CHECK(shouted.against(structure) == Mag::fromInt(3000));
}
