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

[[nodiscard]] rm::unitdef::UnitDef personalShieldDef() {
    rm::unitdef::UnitDef def = plainDef("personal_shield");
    def.shield.maximum = rm::sim::Mag::fromInt(100);
    def.shield.shape = rm::unitdef::ShieldShape::Box;
    def.shield.boxHalfExtentsElmos = {
        rm::sim::Fx::fromInt(4), rm::sim::Fx::fromInt(4), rm::sim::Fx::fromInt(4)};
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

TEST_CASE("an allied shield absorbs a death blast only when it damages friendlies") {
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    const auto detonate = [&](bool damageFriendly) {
        rm::test::Roster roster;
        const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
        const rm::UnitTypeIndex targetType = roster.addType(plainDef());
        const rm::sim::UnitId dying = roster.add(targetType, 0.0f, 0.0f, 0, 100.0f);
        const rm::sim::UnitId generator = roster.add(shieldType, 160.0f, 0.0f, 0, 100.0f);
        const rm::sim::UnitId ally = roster.add(targetType, 100.0f, 0.0f, 0, 100.0f);

        rm::unitdef::UnitDef exploding = plainDef("exploding");
        rm::unitdef::Weapon death;
        death.role = rm::unitdef::WeaponRole::Death;
        death.damage = rm::sim::Mag::fromInt(40);
        death.damageRadius = rm::sim::Fx::fromInt(100);
        death.damageFriendly = damageFriendly;
        exploding.weapons.push_back(death);
        (void)rm::sim::explodeOnDeath(exploding, rm::test::at(0, 0, 0), 0, roster.store,
                                      armies, dying, nullptr, &roster.catalog);
        return std::pair{roster.health(generator).shield.current, roster.health(ally).current};
    };

    const auto hostileOnly = detonate(false);
    CHECK(hostileOnly.first == rm::sim::Mag::fromInt(100));
    CHECK(hostileOnly.second == rm::sim::Mag::fromInt(100));

    const auto friendly = detonate(true);
    CHECK(friendly.first == rm::sim::Mag::fromInt(60));
    CHECK(friendly.second == rm::sim::Mag::fromInt(100));
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

TEST_CASE("overlapping bubbles both absorb, and their protection stacks") {
    // THIS TEST USED TO ASSERT THE OPPOSITE, under the name "the lowest-slot active bubble
    // owns an overlapping impact". It pinned a `break` after the first absorbing shield, so a
    // shot into two overlapping domes drained one and left the other untouched. Overlapping
    // shields stacking is a real Forged Alliance mechanic, and retail walks the whole world
    // shield list rather than stopping at one (`C-062`, `C-110`).
    //
    // 150 into two full 100-point domes. Each decides its absorption independently and takes
    // 150 capped at its own strength, so 200 is taken off a 150-point shot and the unit
    // between them is untouched. Under the old rule it took 50.
    //
    // Both domes paying 100 for a 150-point shot is not a rounding artefact — it is retail's
    // model, where each shield's absorption is decided up front and it is charged its own
    // amount whatever the others did. It is also why stacking shields is worth doing.
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId first = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId second = roster.add(shieldType, 40.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId sheltered = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // The positive-radius blast centre is outside both domes, while its sphere reaches the
    // sheltered unit. This exercises C-143's area-blast admission rather than point-hit
    // compatibility.
    (void)rm::sim::damageArea(
        rm::test::at(20, 0, 100), rm::sim::Fx::fromInt(100),
        rm::unitdef::flatDamage(rm::sim::Mag::fromInt(150)), 0, roster.store, armies,
        &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(sheltered).current) == Approx(100.0f));
    CHECK(roster.health(first).shield.current == rm::sim::Mag{});
    CHECK(roster.health(second).shield.current == rm::sim::Mag{});
}

TEST_CASE("a personal shield contains by its box instead of its nominal sphere") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(personalShieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId owner = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId neighbour = roster.add(targetType, 6.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // The blast intersects the 8x8x8 personal-shield box from outside. The owner's
    // position is contained; the neighbour is only two elmos beyond its X face.
    (void)rm::sim::damageArea(rm::test::at(0, 0, 20), rm::sim::Fx::fromInt(25),
                              rm::sim::Mag::fromInt(40), 0, roster.store, armies, {}, nullptr,
                              &roster.catalog);

    CHECK(roster.health(owner).current == rm::sim::Mag::fromInt(100));
    CHECK(roster.health(neighbour).current == rm::sim::Mag::fromInt(60));
    CHECK(roster.health(owner).shield.current == rm::sim::Mag::fromInt(60));
}

TEST_CASE("an area blast that starts inside a bubble bypasses it") {
    // Retail removes the dome from this blast's shield list before considering individual
    // targets (`C-143(a)`). This is not merely per-target coverage: nobody under the dome is
    // sheltered from a blast whose centre is already inside it.
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId inside = roster.add(targetType, 40.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId outside = roster.add(targetType, 150.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // Radius 200 reaches all three; the dome's radius is 80, so it covers only the first two.
    (void)rm::sim::damageArea(rm::test::at(0, 0, 0), rm::test::fx(200.0f),
                              rm::sim::Mag::fromInt(40), 0, roster.store, armies, {}, nullptr,
                              &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(inside).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(outside).current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));
}

TEST_CASE("inside-origin rejection uses the shield radius minus one tenth") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::sim::Fx blastX{};
    bool shieldAdmitted = false;
    SECTION("inside the shrunken bound") {
        blastX = rm::sim::Fx::fromInt(80) - rm::sim::Fx::fromRatio(3, 20);
    }
    SECTION("on the shrunken bound") {
        blastX = rm::sim::Fx::fromInt(80) - rm::sim::Fx::fromRatio(1, 10);
    }
    SECTION("outside the shrunken bound but inside the authored radius") {
        blastX = rm::sim::Fx::fromInt(80) - rm::sim::Fx::fromRatio(1, 20);
        shieldAdmitted = true;
    }

    roster.transform(target).x = blastX;
    roster.reindex();
    (void)rm::sim::damageArea({blastX, {}, {}}, rm::sim::Fx::fromInt(1),
                              rm::sim::Mag::fromInt(10), 0, roster.store, armies, {}, nullptr,
                              &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(target).current)
          == Approx(shieldAdmitted ? 100.0f : 90.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current)
          == Approx(shieldAdmitted ? 90.0f : 100.0f));
}

TEST_CASE("a one-tenth-radius dome rejects an area blast at its exact centre") {
    rm::test::Roster roster;
    rm::unitdef::UnitDef tinyShield = shieldDef();
    tinyShield.shield.radiusElmos = rm::sim::Fx::fromRatio(1, 10);
    const rm::UnitTypeIndex shieldType = roster.addType(std::move(tinyShield));
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    (void)rm::sim::damageArea(rm::test::at(0, 0, 0), rm::sim::Fx::fromInt(1),
                              rm::sim::Mag::fromInt(10), 0, roster.store, armies, {}, nullptr,
                              &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(90.0f));
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(90.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));
}

TEST_CASE("a blast outside a bubble still spares what is under it") {
    // The mirror error, and the one that made shields useless in the situation they exist for.
    // With coverage measured from the blast, an explosion far from the generator never woke
    // the dome, so a unit standing directly under it took the shot at full strength.
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId sheltered = roster.add(targetType, 20.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // Centred 200 elmos away — far outside the dome — but wide enough to reach under it.
    (void)rm::sim::damageArea(rm::test::at(200, 0, 0), rm::test::fx(250.0f),
                              rm::sim::Mag::fromInt(40), 0, roster.store, armies, {}, nullptr,
                              &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(sheltered).current) == Approx(100.0f));  // was 60
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(60.0f));
}

TEST_CASE("a point hit makes bubbles that cover no target pay nothing") {
    // Radius-zero damage is the compatibility path until projectiles collide with shield
    // entities. Unlike a positive-radius area blast, it charges only domes that covered an
    // actual target.
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::sim::UnitId first = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId second = roster.add(shieldType, 40.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // Midway between them and well outside either unit's own body: a point hit that strikes
    // no unit at all.
    (void)rm::sim::damageArea(
        rm::test::at(20, 0, 0), rm::sim::Fx{}, rm::unitdef::flatDamage(rm::sim::Mag::fromInt(10)),
        0, roster.store, armies, &roster.catalog);

    CHECK(rm::test::asFloat(roster.health(first).shield.current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(second).shield.current) == Approx(100.0f));
}

TEST_CASE("an intersecting bubble pays even when the area blast reaches no hull") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    // A radius-20 blast at zero is exactly tangent to the first radius-80 dome. The second is
    // one fixed-point unit beyond tangency; both remain inside the conservative spatial query.
    const rm::sim::UnitId intersecting = roster.add(shieldType, 100.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId nearby = roster.add(shieldType, 100.0f, 0.0f, 1, 100.0f);
    roster.transform(nearby).x = rm::sim::Fx::fromInt(100) + rm::sim::Fx::fromRaw(1);
    roster.reindex();
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    rm::sim::EventQueue events;

    const rm::sim::Mag dealt = rm::sim::damageArea(
        rm::test::at(0, 0, 0), rm::test::fx(20.0f), rm::sim::Mag::fromInt(10), 0, roster.store,
        armies, {}, &events, &roster.catalog);

    CHECK(rm::test::asFloat(dealt) == Approx(10.0f));
    CHECK(rm::test::asFloat(roster.health(intersecting).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(nearby).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(intersecting).shield.current) == Approx(90.0f));
    CHECK(rm::test::asFloat(roster.health(nearby).shield.current) == Approx(100.0f));
    int shieldDamageEvents = 0;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::ShieldDamaged) {
            ++shieldDamageEvents;
            CHECK(event.unit == intersecting);
            CHECK(rm::test::asFloat(event.amount) == Approx(10.0f));
        }
    }
    CHECK(shieldDamageEvents == 1);
}

TEST_CASE("a disjoint distant dome is not admitted when fixed-point squares saturate") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    // 485 > blast radius 400 + shield radius 80, but the conservative query reaches 488.
    // Squaring 485 as an Fx saturates, so admission must compare widened raw squares instead.
    const rm::sim::UnitId generator = roster.add(shieldType, 485.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    const rm::sim::Mag dealt = rm::sim::damageArea(
        rm::test::at(0, 0, 0), rm::sim::Fx::fromInt(400), rm::sim::Mag::fromInt(10), 0,
        roster.store, armies, {}, nullptr, &roster.catalog);

    CHECK(dealt == rm::sim::Mag{});
    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));
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
    // THE PROPERTY IS UNCHANGED; THE SETUP HAD TO MOVE. This used to put the BLAST 100 elmos
    // above a dome of radius 80 and check that the shot got through. That worked only because
    // coverage was tested against the blast's position — the bug `C-110` removed. Now that
    // coverage is tested against the TARGET, a unit sitting at the dome's centre is sheltered
    // however high the explosion was, and the old assertions inverted.
    //
    // So the height moves to where the question actually lives: the TARGET goes up, into the
    // region a cylinder would cover and a sphere would not.
    //
    // It also has to move sideways, which is the part worth explaining. Impact tests use
    // GROUND distance — height is deliberately not cover (`Combat.hpp`) — so a target directly
    // over the generator would put the point hit on the generator as well, and the generator's
    // own bubble would absorb for the generator. That is correct behaviour and it would hide
    // the geometry under test. Offsetting by 60 elmos keeps the target inside the dome's
    // horizontal footprint while leaving the generator well outside the blast.
    //
    //   horizontal 60 <= radius 80          a cylinder shelters it
    //   sqrt(60^2 + 60^2) = 84.9 > 80       a sphere does not
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 60.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    // `Roster::add` places things on the ground, so the height is set here rather than hidden
    // in a helper.
    roster.store.transforms()[target.index].y = rm::test::fx(60.0f);

    (void)rm::sim::damageArea(
        rm::test::at(60, 60, 0), rm::sim::Fx{}, rm::sim::Mag::fromInt(40), 0,
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

TEST_CASE("a direct projectile sweep strikes a bubble before its owner's hull") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::sim::UnitId generator = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::sim::Projectile shot;
    // The sweep enters the radius-80 bubble at z=-80, before reaching its owner at z=0.
    shot.position = rm::test::at(0, 1, -100);
    shot.velocity = rm::test::at(0, 0, 120);
    shot.damage = rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};

    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = -100.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::sim::EventQueue events;

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, &events, &roster.catalog);

    REQUIRE(shots.size() == 1);
    CHECK(shots.front().pendingImpact == rm::sim::ImpactType::Shield);
    CHECK(shots.front().impactTarget == generator);
    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));

    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, &events, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(60.0f));
    CHECK(rm::test::asFloat(roster.health(generator).current) == Approx(100.0f));
    REQUIRE(events.count(rm::sim::EventKind::ProjectileImpact) == 1);
    CHECK(events.all().front().impactType == rm::sim::ImpactType::Shield);
}

TEST_CASE("a projectile outside a personal shield box misses its bounding sphere") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(personalShieldDef());
    const rm::sim::UnitId owner = roster.add(shieldType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::sim::Projectile shot;
    // x=5 lies outside both the box's x=4 face and the owner's radius-4 hull, but
    // inside the box's radius-6.9 spherical broadphase bound.
    shot.position = rm::test::at(5, 1, -10);
    shot.velocity = rm::test::at(0, 0, 20);
    shot.damage = rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40));
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};

    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = -100.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);

    REQUIRE(shots.size() == 1);
    CHECK(shots.front().pendingImpact == rm::sim::ImpactType::Invalid);
    CHECK(roster.health(owner).shield.current == rm::sim::Mag::fromInt(100));
}

TEST_CASE("a corner impact outside the bubble does not admit it for a covered target") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex shieldType = roster.addType(shieldDef());
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    // The target centre is 79.9 elmos from this generator, inside its radius-80 sphere.
    // Its (-4,-4) collision-box corner is 85.6 elmos from the generator: the radius-one blast
    // hits the hull but does not intersect the radius-80 dome. Per C-143, target coverage alone
    // cannot admit that shield.
    const rm::sim::UnitId generator =
        roster.add(shieldType, 56.5f, 56.5f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 0.0f, 0.0f, 1, 100.0f);
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::sim::Projectile shot;
    shot.position = rm::test::at(-14, 1, -14);
    shot.velocity = rm::test::at(20, 0, 20);
    shot.damage = rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40));
    shot.damageRadiusElmos = rm::sim::Fx::fromInt(1);
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};

    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = -100.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);

    CHECK(shots.empty());
    CHECK(rm::test::asFloat(roster.health(generator).shield.current) == Approx(100.0f));
    CHECK(rm::test::asFloat(roster.health(target).current) == Approx(60.0f));
}

TEST_CASE("a proximity-fallback impact discovers the far-side covering bubble") {
    rm::test::Roster roster;
    rm::unitdef::UnitDef smallShield = shieldDef();
    smallShield.shield.radiusElmos = rm::sim::Fx::fromInt(2);
    const rm::UnitTypeIndex shieldType = roster.addType(smallShield);
    const rm::UnitTypeIndex targetType = roster.addType(plainDef());
    const rm::sim::UnitId generator =
        roster.add(shieldType, 3.0f, 0.0f, 1, 100.0f);
    const rm::sim::UnitId target = roster.add(targetType, 1.0f, 0.0f, 1, 100.0f);
    // Keep both physical bodies tiny. The radius-one fallback at x=0 overlaps the target's
    // near face, while the generator two elmos beyond its centre is not itself struck.
    roster.motion(generator).radiusElmos = rm::sim::Fx::fromRaw(1);
    roster.motion(target).radiusElmos = rm::sim::Fx::fromRaw(1);
    roster.reindex();
    const std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);

    rm::sim::Projectile shot;
    // Coplanar with the shield centre, so blast radius 1 + shield radius 2 is exact tangency.
    shot.position = {rm::sim::Fx{}, rm::sim::Fx{}, rm::sim::Fx{}};
    shot.damage = rm::unitdef::flatDamage(rm::sim::Mag::fromInt(40));
    // The radius-one collision fallback can detect a body farther away than a smaller blast
    // overlaps. C-170's DamageArea ignores that collision target, so use a one-elmo blast to
    // keep this case about discovering the far-side shield rather than following the target.
    shot.damageRadiusElmos = rm::sim::Fx::fromInt(1);
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    shot.firedByArmy = 0;
    shot.ticksRemaining = 2;
    std::vector<rm::sim::Projectile> shots{shot};

    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = -100.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);
    rm::sim::advanceProjectiles(shots, roster.store, armies, rm::sim::Terrain{field},
                                roster.rate, nullptr, &roster.catalog);

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
