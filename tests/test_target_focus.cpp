// Target focus — the per-unit "what to prefer shooting" setting of #15809.
// Authored `TargetPriorities` bound what a weapon CAN engage; the focus narrows
// that set (`AirOnly`, `EconomyOnly`) or re-orders it (`Snipe`). The cycle
// command is an unqueued authoritative action, the same shape as
// `CycleRetreatThreshold`.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/UnitStore.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

namespace {

using rm::sim::Army;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::UnitId;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] Weapon directFire(float rangeElmos,
                                std::vector<std::vector<std::string>> priorities) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.targetPriorities = std::move(priorities);
    weapon.damage = rm::test::mag(10.0f);
    weapon.maxRange = rm::test::fx(rangeElmos);
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 100.0f;
    return weapon;
}

/// An armed mobile unit — the kind of thing the cycle accepts.
[[nodiscard]] UnitDef gunnerDef() {
    UnitDef def;
    def.name = "test_gunner";
    def.categories = {"LAND", "MOBILE"};
    def.speedElmosPerSecond = 4.0f;
    def.weapons.push_back(directFire(300.0f, {{"LAND"}, {"AIR"}}));
    return def;
}

/// A gunless bystander — the cycle has nothing to set on it.
[[nodiscard]] UnitDef bystanderDef() {
    UnitDef def;
    def.name = "test_bystander";
    def.categories = {"LAND", "MOBILE"};
    def.speedElmosPerSecond = 4.0f;
    return def;
}

[[nodiscard]] CommandIssue focusIssue(const UnitId unit, const std::uint32_t serial) {
    return CommandIssue{.source = 0,
                        .id = rm::commandId(0, serial),
                        .player = 0,
                        .kind = CommandKind::CycleTargetFocus,
                        .units = {unit}};
}

} // namespace

TEST_CASE("the target focus cycles on an armed unit and refuses an unarmed one",
          "[target-focus]") {
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);

    rm::test::Roster roster;
    const rm::UnitTypeIndex gunner = roster.addType(gunnerDef());
    const rm::UnitTypeIndex bystander = roster.addType(bystanderDef());
    const UnitId armed = roster.add(gunner, 40.0f, 40.0f, 0, 500.0f);
    const UnitId unarmed = roster.add(bystander, 60.0f, 60.0f, 0, 500.0f);

    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const std::vector<Army> armies = rm::sim::freeForAll(1);

    CHECK(roster.store.targetFocus(armed) == rm::TargetFocus::Default);
    std::uint32_t serial = 1;
    for (const rm::TargetFocus expected :
         {rm::TargetFocus::Snipe, rm::TargetFocus::AirOnly,
          rm::TargetFocus::EconomyOnly, rm::TargetFocus::Default}) {
        const auto result = rm::sim::applyCommand(
            focusIssue(armed, serial++), roster.store, roster.catalog, players, armies,
            terrain, [&grid](UnitId) { return &grid; }, roster.rate);
        REQUIRE(result.accepted.size() == 1);
        CHECK(roster.store.targetFocus(armed) == expected);
    }

    // No acquiring weapon means nothing to prefer — the setting refuses rather
    // than sit armed on a unit that can never act on it.
    const auto refused = rm::sim::applyCommand(
        focusIssue(unarmed, serial), roster.store, roster.catalog, players, armies,
        terrain, [&grid](UnitId) { return &grid; }, roster.rate);
    CHECK(refused.accepted.empty());
    CHECK(roster.store.targetFocus(unarmed) == rm::TargetFocus::Default);
}

TEST_CASE("a snipe focus prefers a high-tier target over a nearer ordinary one",
          "[target-focus]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    rm::test::Roster roster;

    UnitDef ordinary;
    ordinary.name = "t1_grunt";
    ordinary.categories = {"LAND", "TECH1"};
    UnitDef elite;
    elite.name = "t3_sniper_bait";
    elite.categories = {"LAND", "TECH3"};

    const UnitId nearGrunt =
        roster.add(roster.addType(ordinary), 0.0f, 50.0f, 1, 100.0f);
    const UnitId farElite =
        roster.add(roster.addType(elite), 0.0f, 250.0f, 1, 100.0f);

    const Weapon weapon = directFire(300.0f, {{"LAND"}});

    // Unfocused acquisition takes the near one.
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                 armies, nullptr, &roster.catalog)
          == nearGrunt);
    // Sniping walks the far TECH3 past it.
    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                 armies, nullptr, &roster.catalog, std::nullopt,
                                 std::nullopt, nullptr, {}, std::nullopt, 0, {},
                                 rm::TargetFocus::Snipe)
          == farElite);
}

TEST_CASE("an air-only focus engages aircraft and declines a ground-only field",
          "[target-focus]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    rm::test::Roster roster;

    UnitDef tank;
    tank.name = "tank";
    tank.categories = {"LAND"};
    UnitDef fighter;
    fighter.name = "fighter";
    fighter.categories = {"AIR"};

    const UnitId nearTank =
        roster.add(roster.addType(tank), 0.0f, 50.0f, 1, 100.0f);
    const UnitId farFighter =
        roster.add(roster.addType(fighter), 0.0f, 200.0f, 1, 100.0f);

    const Weapon weapon = directFire(300.0f, {{"LAND"}, {"AIR"}});

    const auto picked =
        rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                               armies, nullptr, &roster.catalog, std::nullopt,
                               std::nullopt, nullptr, {}, std::nullopt, 0, {},
                               rm::TargetFocus::AirOnly);
    REQUIRE(picked.has_value());
    CHECK(*picked == farFighter);
    CHECK(*picked != nearTank);

    // A field with nothing airborne acquires nothing at all — "only" is a
    // filter, not a preference.
    rm::test::Roster groundOnly;
    (void)groundOnly.add(groundOnly.addType(tank), 0.0f, 50.0f, 1, 100.0f);
    CHECK_FALSE(rm::sim::nearestTarget(
        rm::test::at(0, 0, 0), 0, weapon, groundOnly.store, armies, nullptr,
        &groundOnly.catalog, std::nullopt, std::nullopt, nullptr, {},
        std::nullopt, 0, {}, rm::TargetFocus::AirOnly)
                    .has_value());
}

TEST_CASE("an economy-only focus walks past troops to the base",
          "[target-focus]") {
    const std::vector<Army> armies = rm::sim::freeForAll(2);
    rm::test::Roster roster;

    UnitDef tank;
    tank.name = "tank";
    tank.categories = {"LAND", "MOBILE"};
    UnitDef extractor;
    extractor.name = "mex";
    extractor.categories = {"ECONOMIC", "LAND", "STRUCTURE"};

    (void)roster.add(roster.addType(tank), 0.0f, 50.0f, 1, 100.0f);  // nearer
    const UnitId mex =
        roster.add(roster.addType(extractor), 0.0f, 200.0f, 1, 100.0f);

    const Weapon weapon = directFire(300.0f, {{"LAND"}});

    CHECK(rm::sim::nearestTarget(rm::test::at(0, 0, 0), 0, weapon, roster.store,
                                 armies, nullptr, &roster.catalog, std::nullopt,
                                 std::nullopt, nullptr, {}, std::nullopt, 0, {},
                                 rm::TargetFocus::EconomyOnly)
          == mex);
}

TEST_CASE("a save preserves the target focus", "[target-focus][save-state]") {
    rm::sim::UnitStore original;
    const UnitId unit = original.spawn({});
    REQUIRE(original.setTargetFocus(unit, rm::TargetFocus::EconomyOnly));

    rm::sim::RandomStream random{std::uint32_t{7}};
    const auto bytes = rm::sim::SaveState::encode(
        {.tick = 42, .random = random.snapshot(), .units = original.snapshot()});
    const auto restored = rm::sim::SaveState::decode(bytes);

    REQUIRE(restored.has_value());
    rm::sim::UnitStore store{restored->units};
    CHECK(store.targetFocus(unit) == rm::TargetFocus::EconomyOnly);
    CHECK(rm::sim::SaveState::encode(*restored) == bytes);
}
