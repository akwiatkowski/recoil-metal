#include "core/unit/Armor.hpp"

#include <catch2/catch_test_macros.hpp>

#include "core/sim/Army.hpp"
#include "core/sim/Combat.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <array>
#include <string_view>
#include <vector>

using rm::ArmorClass;
using rm::kDefaultArmor;
using rm::sim::Army;
using rm::sim::Mag;
using rm::test::Roster;
using rm::unitdef::ArmorMultiplier;
using rm::unitdef::ArmorRegistry;
using rm::unitdef::DamageProfile;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;
using rm::unitdef::damageFromMatrix;
using rm::unitdef::flatDamage;

namespace {

/// A weapon that does `damage` of a stated kind, and nothing else worth reading.
///
/// The fields that are set are the ones the catalog looks at when it builds a profile; the
/// rest are left alone deliberately, so a test that starts depending on range or reload says
/// so by failing rather than by quietly using a default that means something.
[[nodiscard]] Weapon gun(float damage, std::string_view damageType) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.damage = rm::test::mag(damage);
    weapon.damageType = std::string{damageType};
    weapon.maxRange = rm::test::fx(100.0f);
    weapon.rateOfFire = 1.0f;
    return weapon;
}

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

// --- The wiring: content -> catalog -> the damage path ----------------------------------
//
// The tests above are about the TYPE. These are about the only thing that makes it worth
// having: that a weapon's table and a target's class meet in `damageArea` and change what a
// shot does.

TEST_CASE("a catalog with no armour context reproduces the pre-P10.1 engine exactly") {
    // THE PROPERTY THAT KEEPS `make verify` A STRICT CHECK across this change, and the reason
    // `setArmor` is optional rather than a constructor argument. Every existing test builds a
    // catalog and never mentions armour; all of them must keep meaning what they meant.
    Roster roster;
    UnitDef def;
    def.name = "test_target";
    def.armorType = "Structure";  // stated, but nothing resolves it
    def.weapons.push_back(gun(100.0f, "Overcharge"));
    const rm::UnitTypeIndex type = roster.addType(def);

    CHECK(roster.catalog.armorOf(type) == kDefaultArmor);
    CHECK(roster.catalog.weaponRates(type, 0).damage == flatDamage(rm::test::mag(100.0f)));
}

TEST_CASE("the catalog resolves a blueprint's armour type and transposes its weapons") {
    Roster roster;
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    roster.catalog.setArmor(registry, faMatrix(registry));

    UnitDef building;
    building.name = "test_building";
    building.armorType = "Structure";
    const rm::UnitTypeIndex structureType = roster.addType(building);

    UnitDef commander;
    commander.name = "test_acu";
    commander.armorType = "Normal";
    commander.weapons.push_back(gun(12000.0f, "Overcharge"));
    const rm::UnitTypeIndex acuType = roster.addType(commander);

    CHECK(roster.catalog.armorOf(structureType) == registry.classFor("Structure"));
    CHECK(roster.catalog.armorOf(acuType) == registry.classFor("Normal"));

    // And the weapon carries the transposed table, not the scalar.
    const rm::unitdef::DamageProfile& overcharge = roster.catalog.weaponRates(acuType, 0).damage;
    CHECK(overcharge.against(registry.classFor("Structure")) == rm::test::mag(3000.0f));
    CHECK(overcharge.against(registry.classFor("Normal")) == rm::test::mag(12000.0f));
}

TEST_CASE("an unstated damage type is read as Normal, not as nothing") {
    // 454 of 494 weapons say `Normal` explicitly and its multiplier is 1.0 everywhere, so a
    // weapon that states no type must come out identical to one that states `Normal`. Reading
    // a missing field as anything else would hand a few weapons a bonus no blueprint asked for.
    Roster roster;
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    roster.catalog.setArmor(registry, faMatrix(registry));

    UnitDef def;
    def.name = "test_gun";
    def.weapons.push_back(gun(250.0f, ""));
    def.weapons.push_back(gun(250.0f, "Normal"));
    const rm::UnitTypeIndex type = roster.addType(def);

    CHECK(roster.catalog.weaponRates(type, 0).damage
          == roster.catalog.weaponRates(type, 1).damage);
    CHECK(roster.catalog.weaponRates(type, 0).damage.overrideCount == 0);
}

TEST_CASE("one blast hits two armour classes differently") {
    // THE WHOLE POINT, in one assertion. A shell landing between a tank and a bunker is why
    // the armour lookup happens per target inside the loop rather than once outside it.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    roster.catalog.setArmor(registry, faMatrix(registry));

    UnitDef tank;
    tank.name = "test_tank";
    tank.armorType = "Normal";
    const rm::UnitTypeIndex tankType = roster.addType(tank);

    UnitDef bunker;
    bunker.name = "test_bunker";
    bunker.armorType = "Structure";
    const rm::UnitTypeIndex bunkerType = roster.addType(bunker);

    // Both at the centre of the blast, so the falloff is 1.0 for each and the only thing that
    // can differ between them is the armour table.
    const rm::sim::UnitId softTarget = roster.add(tankType, 0.0f, 0.0f, 1, 20000.0f);
    const rm::sim::UnitId hardTarget = roster.add(bunkerType, 0.0f, 0.0f, 1, 20000.0f);

    const rm::unitdef::DamageProfile overcharge =
        damageFromMatrix(rm::test::mag(12000.0f), "Overcharge", faMatrix(registry));

    const rm::sim::Mag dealt =
        rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(0.0f), overcharge, 0,
                            roster.store, armies, &roster.catalog);

    // Full damage to the tank, a quarter of it to the building — `armordefinition.lua`'s
    // `Structure / Overcharge 0.25`, arriving through the whole chain.
    CHECK(roster.health(softTarget).current == rm::test::mag(8000.0f));
    CHECK(roster.health(hardTarget).current == rm::test::mag(17000.0f));
    CHECK(dealt == rm::test::mag(15000.0f));
}

TEST_CASE("armour is applied before the falloff, not after") {
    // The order is not arbitrary and getting it backwards is invisible at the centre of a
    // blast: it only shows up off-centre, where scaling first and looking up second would key
    // the table on a number that is no longer the weapon's damage.
    //
    // Half a blast radius away, so the falloff is exactly 0.5. A structure takes
    // 12000 x 0.25 x 0.5 = 1500 either way IF the operations commute — which they do
    // arithmetically. What does NOT commute is the LOOKUP, and this pins the value so a future
    // non-linear falloff or a per-class radius cannot quietly reorder them.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    roster.catalog.setArmor(registry, faMatrix(registry));

    UnitDef bunker;
    bunker.name = "test_bunker";
    bunker.armorType = "Structure";
    const rm::sim::UnitId target = roster.add(roster.addType(bunker), 0.0f, 50.0f, 1, 20000.0f);

    const rm::unitdef::DamageProfile overcharge =
        damageFromMatrix(rm::test::mag(12000.0f), "Overcharge", faMatrix(registry));
    rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(100.0f), overcharge, 0, roster.store,
                        armies, &roster.catalog);

    CHECK(roster.health(target).current == rm::test::mag(18500.0f));
}

TEST_CASE("without a catalog every target is ordinary armour") {
    // The null-catalog path, asserted rather than assumed: it is what `explodeOnDeath` and
    // every existing call site take, so "the same as before" has to be a test.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    const ArmorRegistry registry = ArmorRegistry::fromNames(kFaClasses);
    roster.catalog.setArmor(registry, faMatrix(registry));

    UnitDef bunker;
    bunker.name = "test_bunker";
    bunker.armorType = "Structure";
    const rm::sim::UnitId target = roster.add(roster.addType(bunker), 0.0f, 0.0f, 1, 20000.0f);

    const rm::unitdef::DamageProfile overcharge =
        damageFromMatrix(rm::test::mag(12000.0f), "Overcharge", faMatrix(registry));

    // Same profile, same target, no catalog — so the Structure override is unreachable and the
    // base applies.
    rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(0.0f), overcharge, 0, roster.store,
                        armies, nullptr);
    CHECK(roster.health(target).current == rm::test::mag(8000.0f));
}

TEST_CASE("a death blast consults the armour table") {
    // THE PATH THAT WAS MISSING. P10.1 first landed with armour reaching `damageArea`'s new
    // overload and *nothing in the tick calling it* — the sim's three damage sites all used the
    // scalar overload, so the table loaded and was ignored. This is the death-blast half of the
    // fix; `advanceProjectiles` is covered by the golden replay.
    //
    // Retail Forged Alliance rates `Structure / Deathnuke` at 0.01: a commander detonating in a
    // base does one percent of its damage to the buildings. That is a large enough rule that a
    // match where it silently did not apply would look like a balance bug.
    const std::vector<Army> armies = rm::sim::freeForAll(2);

    Roster roster;
    constexpr std::array<std::string_view, 3> kClasses{"Default", "Structure", "Commander"};
    const ArmorRegistry registry = ArmorRegistry::fromNames(kClasses);
    roster.catalog.setArmor(registry, std::vector<ArmorMultiplier>{
                                          {registry.classFor("Structure"), "Deathnuke", 0.01f},
                                      });

    // A commander whose death weapon is a Deathnuke.
    UnitDef acu;
    acu.name = "test_acu";
    acu.armorType = "Commander";
    Weapon detonation;
    detonation.label = "death";
    detonation.role = WeaponRole::Death;
    detonation.damageType = "Deathnuke";
    detonation.damage = rm::test::mag(45000.0f);
    detonation.damageRadius = rm::test::fx(200.0f);
    acu.weapons.push_back(detonation);
    const rm::UnitTypeIndex acuType = roster.addType(acu);

    UnitDef bunker;
    bunker.name = "test_bunker";
    bunker.armorType = "Structure";
    const rm::UnitTypeIndex bunkerType = roster.addType(bunker);

    // A structure right at the centre of the blast, so falloff is 1.0 and the only thing that
    // can differ is the table.
    const auto damageDealt = [&](bool withCatalog) {
        Roster fresh;
        fresh.catalog.setArmor(registry, std::vector<ArmorMultiplier>{
                                             {registry.classFor("Structure"), "Deathnuke", 0.01f},
                                         });
        fresh.defs = roster.defs;  // same definitions, same type indices
        (void)fresh.addType(acu);
        (void)fresh.addType(bunker);
        (void)fresh.add(bunkerType, 0.0f, 0.0f, 1, 1000000.0f);
        return rm::sim::explodeOnDeath(acu, rm::test::at(0, 0, 0), 0, fresh.store, armies, {},
                                       nullptr, withCatalog ? &fresh.catalog : nullptr);
    };

    // Without the catalog: the full 45,000, which is what the engine did before this landed.
    CHECK(damageDealt(false) == rm::test::mag(45000.0f));
    // With it: one percent, because the target is a Structure and the blast is a Deathnuke.
    CHECK(damageDealt(true) == rm::test::mag(450.0f));

    (void)acuType;
}
