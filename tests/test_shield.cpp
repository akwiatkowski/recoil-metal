// Ordinary FA bubble shields: absorption, collapse and recovery.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/data/ArmorDefs.hpp"
#include "core/sim/Combat.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] rm::unitdef::UnitDef plainDef(std::string name = "target") {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef shieldDef() {
    rm::unitdef::UnitDef def = plainDef("shield_generator");
    def.shield.maximum = rm::sim::Mag::fromInt(100);
    def.shield.radiusElmos = rm::sim::Fx::fromInt(80);
    def.shield.regenPerSecond = 10.0f;
    // Already imported durations: FA's WaitSeconds adds one 100 ms authoring tick.
    def.shield.regenDelay = rm::sim::seconds(1.1f);
    def.shield.rechargeDelay = rm::sim::seconds(2.1f);
    return def;
}

[[nodiscard]] const rm::sim::Event* eventOf(const rm::sim::EventQueue& events,
                                             rm::sim::EventKind kind) {
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == kind) {
            return &event;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("a bubble absorbs damage before every hull beneath it") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId protectedUnit = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    rm::sim::EventQueue events;

    const rm::sim::Mag dealt = rm::sim::damageArea(
        rm::test::at(20, 0, 0), rm::sim::Fx{}, rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40)),
        0, roster.store, armies, &roster.catalog, {}, &events);

    CHECK(rm::test::asFloat(dealt) == Approx(40.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(protectedUnit).current) == Approx(100.0f));
    const rm::sim::Event* absorbed = eventOf(events, rm::sim::EventKind::ShieldDamaged);
    REQUIRE(absorbed != nullptr);
    CHECK(absorbed->unit == generator);
    CHECK(rm::test::asFloat(absorbed->amount) == Approx(40.0f));
    CHECK(eventOf(events, rm::sim::EventKind::UnitDamaged) == nullptr);
}

TEST_CASE("shield overkill collapses the bubble and leaks only the remainder") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId protectedUnit = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    roster.health(generator).shield.current = rm::sim::Mag::fromInt(25);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    rm::sim::EventQueue events;

    const rm::sim::Mag dealt = rm::sim::damageArea(
        rm::test::at(20, 0, 0), rm::sim::Fx{}, rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40)),
        0, roster.store, armies, &roster.catalog, {}, &events);

    CHECK(rm::test::asFloat(dealt) == Approx(40.0f));
    CHECK(roster.health(generator).shield.current == rm::sim::Mag{});
    CHECK(rm::test::asFloat(roster.health(protectedUnit).current) == Approx(85.0f));
    CHECK(eventOf(events, rm::sim::EventKind::ShieldCollapsed) != nullptr);
}

TEST_CASE("the lowest-slot active bubble owns an overlapping impact") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::sim::UnitId first = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId second = roster.add(shieldType, 40.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    (void)rm::sim::damageArea(
        rm::test::at(20, 0, 0), rm::sim::Fx{}, rm::unitdef::flatDamage(rm::sim::Mag::fromInt(10)),
        0, roster.store, armies, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(first).shield.current) == Approx(90.0f));
    CHECK(rm::test::asFloat(roster.health(second).shield.current) == Approx(100.0f));
}

TEST_CASE("damage outside a bubble reaches hull normally") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    (void)roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId outside = roster.add(targetType, 100.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    (void)rm::sim::damageArea(
        rm::test::at(100, 0, 0), rm::sim::Fx{}, rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40)),
        0, roster.store, armies, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(outside).current) == Approx(60.0f));
}

TEST_CASE("a bubble is a sphere rather than an infinite vertical cylinder") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    (void)rm::sim::damageArea(
        rm::test::at(0, 100, 0), rm::sim::Fx{}, rm::sim::Mag::fromInt(40), 0,
        roster.store, armies, {}, nullptr, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(60.0f));
}

TEST_CASE("shield-class damage controls absorption and proportional hull leakage") {
    rm::test::Roster roster;
    // Retail does not declare a Shield block. The content importer supplies that engine-level
    // pseudo-class so a real mounted catalog, not just a synthetic registry, can address it.
    const rm::data::ArmorTable armor = rm::data::armorTableFromSource(
        "armordefinition = { { 'Normal', 'Normal 1.0', }, }");
    REQUIRE(armor.registry.knows("Shield"));
    roster.catalog.setArmor(armor.registry, armor.multipliers);
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    roster.health(generator).shield.current = rm::sim::Mag::fromInt(40);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::unitdef::DamageProfile damage =
        rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40));
    REQUIRE(damage.addOverride(armor.registry.classFor("Shield"), rm::sim::Mag::fromInt(80)));
    (void)rm::sim::damageArea(rm::test::at(20, 0, 0), rm::sim::Fx{}, damage, 0,
                              roster.store, armies, &roster.catalog);

    CHECK(roster.health(generator).shield.current == rm::sim::Mag{});
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(80.0f));
}

TEST_CASE("a projectile impact reaches the same bubble gate") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 50.0f, 50.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 0.0f, 100.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::unitdef::Weapon weapon;
    weapon.damage = rm::sim::Mag::fromInt(40);
    weapon.maxRange = rm::sim::Fx::fromInt(200);
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    std::vector<rm::sim::Projectile> shots{rm::sim::launch(
        rm::test::at(0, 0, 0), rm::test::at(0, 0, 100), weapon, 0, roster.rate,
        roster.rate.perTick(weapon.muzzleVelocityElmosPerSecond),
        rm::unitdef::flatDamage(weapon.damage))};
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});

    for (int tick = 0; tick < 100 && !shots.empty(); ++tick) {
        rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                    roster.rate, nullptr, &roster.catalog);
    }

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(100.0f));
}

TEST_CASE("a damaged shield waits, regenerates, and a collapsed one returns full") {
    SECTION("partial damage") {
        rm::test::Roster roster;
        const rm::UnitTypeIndex type = roster.addType(shieldDef());
        const rm::sim::UnitId generator = roster.add(type, 0.0f, 0.0f, 1, 100.0f);
        roster.health(generator).shield.current = rm::sim::Mag::fromInt(50);
        roster.health(generator).shield.regenDelayRemaining = roster.catalog.shield(type).regenDelay;

        for (int tick = 0; tick < 11; ++tick) {
            rm::sim::tickShields(roster.store, roster.catalog);
        }
        CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(50.0f));
        rm::sim::tickShields(roster.store, roster.catalog);
        CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(51.0f));
    }

    SECTION("damage collapse") {
        rm::test::Roster roster;
        const rm::UnitTypeIndex type = roster.addType(shieldDef());
        const rm::sim::UnitId generator = roster.add(type, 0.0f, 0.0f, 1, 100.0f);
        roster.health(generator).shield.current = rm::sim::Mag{};
        roster.health(generator).shield.rechargeRemaining = roster.catalog.shield(type).recharge;
        rm::sim::EventQueue events;

        for (int tick = 0; tick < 21; ++tick) {
            rm::sim::tickShields(roster.store, roster.catalog, &events);
        }
        CHECK(roster.health(generator).shield.current == roster.health(generator).shield.maximum);
        CHECK(eventOf(events, rm::sim::EventKind::ShieldRestored) != nullptr);
    }
}
