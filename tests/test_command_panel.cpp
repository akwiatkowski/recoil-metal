#include <catch2/catch_test_macros.hpp>

#include "core/ui/CommandPanel.hpp"

#include <algorithm>
#include <array>

using rm::sim::CommandKind;

namespace {

[[nodiscard]] rm::unitdef::Weapon weapon(bool manual) {
    return rm::unitdef::Weapon{
        .role = rm::unitdef::WeaponRole::DirectFire,
        .damage = rm::sim::Mag::fromInt(1),
        .maxRange = rm::sim::Fx::fromInt(100),
        .rateOfFire = 1.0f,
        .manualFire = manual,
        .energyRequired = manual ? rm::sim::Mag::fromInt(100) : rm::sim::Mag{},
    };
}

[[nodiscard]] bool enabled(const rm::ui::CommandAvailability& available, CommandKind kind) {
    for (std::size_t slot = 0; slot < rm::ui::kCommandDescriptors.size(); ++slot) {
        if (rm::ui::kCommandDescriptors[slot].kind == kind) {
            return available[slot];
        }
    }
    return false;
}

} // namespace

TEST_CASE("the command rack keeps Forged Alliance's fixed 4x3 positions") {
    STATIC_REQUIRE(rm::ui::kCommandDescriptors.size() == 12);
    CHECK(rm::ui::kCommandDescriptors[0].kind == CommandKind::AttackMove);
    CHECK(rm::ui::kCommandDescriptors[1].kind == CommandKind::Move);
    CHECK(rm::ui::kCommandDescriptors[2].kind == CommandKind::Attack);
    CHECK(rm::ui::kCommandDescriptors[3].kind == CommandKind::Patrol);
    CHECK(rm::ui::kCommandDescriptors[4].kind == CommandKind::Stop);
    CHECK(rm::ui::kCommandDescriptors[5].kind == CommandKind::Assist);
    CHECK_FALSE(rm::ui::kCommandDescriptors[6].kind.has_value());
    CHECK(rm::ui::kCommandDescriptors[7].kind == CommandKind::Overcharge);
    CHECK(rm::ui::kCommandDescriptors[8].kind == CommandKind::Repair);
    CHECK_FALSE(rm::ui::kCommandDescriptors[9].kind.has_value());
    CHECK_FALSE(rm::ui::kCommandDescriptors[10].kind.has_value());
    CHECK(rm::ui::kCommandDescriptors[11].kind == CommandKind::Reclaim);

    CHECK(std::ranges::none_of(rm::ui::kCommandDescriptors, [](const auto& descriptor) {
        return descriptor.kind == CommandKind::Build;
    }));
}

TEST_CASE("an empty selection disables the complete command rack") {
    const rm::ui::CommandAvailability available = rm::ui::commandAvailability({});
    CHECK(std::ranges::none_of(available, std::identity{}));
}

TEST_CASE("command availability is the union of selected unit capabilities") {
    rm::unitdef::UnitDef tank;
    tank.speedElmosPerSecond = 10.0f;
    tank.weapons.push_back(weapon(false));

    rm::unitdef::UnitDef engineer;
    engineer.buildRate = 5.0f;

    rm::unitdef::UnitDef commander;
    commander.buildRate = 5.0f;
    commander.weapons.push_back(weapon(true));

    const std::array<const rm::unitdef::UnitDef*, 4> selection{
        nullptr, &tank, &engineer, &commander};
    const rm::ui::CommandAvailability available = rm::ui::commandAvailability(selection);

    CHECK(enabled(available, CommandKind::AttackMove));
    CHECK(enabled(available, CommandKind::Move));
    CHECK(enabled(available, CommandKind::Attack));
    CHECK(enabled(available, CommandKind::Patrol));
    CHECK(enabled(available, CommandKind::Stop));
    CHECK(enabled(available, CommandKind::Assist));
    CHECK(enabled(available, CommandKind::Overcharge));
    CHECK(enabled(available, CommandKind::Reclaim));
    CHECK(enabled(available, CommandKind::Repair));
    CHECK_FALSE(available[6]);
    CHECK_FALSE(available[9]);
    CHECK_FALSE(available[10]);
}

TEST_CASE("an immobile unarmed non-builder can only stop") {
    const rm::unitdef::UnitDef structure;
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&structure};
    const rm::ui::CommandAvailability available = rm::ui::commandAvailability(selection);

    CHECK(enabled(available, CommandKind::Stop));
    CHECK(std::ranges::count(available, true) == 1);
}

TEST_CASE("ordinary and manual weapons enable only their own attack semantics") {
    rm::unitdef::UnitDef tank;
    tank.weapons.push_back(weapon(false));
    const std::array<const rm::unitdef::UnitDef*, 1> tankSelection{&tank};
    const rm::ui::CommandAvailability tankAvailable =
        rm::ui::commandAvailability(tankSelection);
    CHECK(enabled(tankAvailable, CommandKind::Attack));
    CHECK_FALSE(enabled(tankAvailable, CommandKind::Overcharge));

    rm::unitdef::UnitDef commander;
    commander.weapons.push_back(weapon(true));
    const std::array<const rm::unitdef::UnitDef*, 1> commanderSelection{&commander};
    const rm::ui::CommandAvailability commanderAvailable =
        rm::ui::commandAvailability(commanderSelection);
    CHECK_FALSE(enabled(commanderAvailable, CommandKind::Attack));
    CHECK(enabled(commanderAvailable, CommandKind::Overcharge));

    commander.weapons.front().energyRequired = {};
    CHECK_FALSE(enabled(rm::ui::commandAvailability(commanderSelection),
                        CommandKind::Overcharge));
}

TEST_CASE("an immobile factory and a field builder can assist") {
    rm::unitdef::UnitDef factory;
    factory.buildRate = 5.0f;
    factory.categories = {"FACTORY"};
    const std::array<const rm::unitdef::UnitDef*, 1> factorySelection{&factory};
    const rm::ui::CommandAvailability factoryAvailable =
        rm::ui::commandAvailability(factorySelection);
    CHECK(enabled(factoryAvailable, CommandKind::Reclaim));
    CHECK(enabled(factoryAvailable, CommandKind::Repair));
    CHECK(enabled(factoryAvailable, CommandKind::Assist));

    rm::unitdef::UnitDef engineer;
    engineer.buildRate = 5.0f;
    const std::array<const rm::unitdef::UnitDef*, 1> engineerSelection{&engineer};
    const rm::ui::CommandAvailability engineerAvailable =
        rm::ui::commandAvailability(engineerSelection);
    CHECK(enabled(engineerAvailable, CommandKind::Reclaim));
    CHECK(enabled(engineerAvailable, CommandKind::Repair));
    CHECK(enabled(engineerAvailable, CommandKind::Assist));

    rm::unitdef::UnitDef commander;
    commander.categories = {"COMMAND"};
    const std::array<const rm::unitdef::UnitDef*, 1> commanderSelection{&commander};
    const rm::ui::CommandAvailability commanderAvailable =
        rm::ui::commandAvailability(commanderSelection);
    CHECK_FALSE(enabled(commanderAvailable, CommandKind::Reclaim));
    CHECK(enabled(commanderAvailable, CommandKind::Assist));

    // URL0107's shape: primarily a combat unit, but its BuildRate=1 repair arm can assist.
    rm::unitdef::UnitDef combatBuilder;
    combatBuilder.buildRate = 1.0f;
    combatBuilder.categories = {"DIRECTFIRE", "LAND", "MOBILE", "TECH1"};
    combatBuilder.weapons.push_back(weapon(false));
    const std::array<const rm::unitdef::UnitDef*, 1> combatSelection{&combatBuilder};
    const rm::ui::CommandAvailability combatAvailable =
        rm::ui::commandAvailability(combatSelection);
    CHECK(enabled(combatAvailable, CommandKind::Attack));
    CHECK(enabled(combatAvailable, CommandKind::Assist));
}

TEST_CASE("the command rack is a fixed 4 by 3 fitting with dead gutters") {
    const rm::ui::FrameLayout frame = rm::ui::frameLayout(
        rm::ui::UiViewport::authored(1280.0f, 720.0f));
    const rm::ui::CommandRackLayout rack = rm::ui::commandRackLayout(frame, true);
    REQUIRE(rack.visible);
    CHECK(rack.rect.x == frame.commands.x);
    CHECK(rack.rect.y == frame.commands.y);
    CHECK(rack.rect.width == frame.commands.width);
    CHECK(rack.rect.height == frame.commands.height);

    for (std::size_t slot = 0; slot < rm::ui::kCommandSlots; ++slot) {
        const auto origin = rm::ui::commandCellOrigin(rack, slot);
        CHECK(rm::ui::commandSlotAt(rack, origin[0] + rack.cellWidth * 0.5f,
                                    origin[1] + rack.cellHeight * 0.5f)
              == slot);
    }

    const auto first = rm::ui::commandCellOrigin(rack, 0);
    CHECK_FALSE(rm::ui::commandSlotAt(rack, first[0] + rack.cellWidth + 1.0f,
                                      first[1] + rack.cellHeight * 0.5f));
    CHECK_FALSE(rm::ui::commandSlotAt(rack, rack.gridX, rack.rect.y + 4.0f));
    CHECK(rm::ui::insideCommandRack(rack, rack.rect.x + 1.0f, rack.rect.y + 1.0f));
}

TEST_CASE("the command rack is absent without a selection") {
    const rm::ui::FrameLayout frame = rm::ui::frameLayout(
        rm::ui::UiViewport::authored(1280.0f, 720.0f));
    const rm::ui::CommandRackLayout rack = rm::ui::commandRackLayout(frame, false);
    CHECK_FALSE(rack.visible);
    CHECK_FALSE(rm::ui::insideCommandRack(rack, frame.commands.x, frame.commands.y));
}

TEST_CASE("command inspector distinguishes ready disabled and targeting states") {
    const rm::ui::CommandDescriptor& move = rm::ui::kCommandDescriptors[1];
    const rm::ui::InfoCard ready = rm::ui::commandCard(move, true);
    REQUIRE(ready.rows.size() == 2);
    CHECK(ready.title == "MOVE");
    CHECK(ready.rows[0].value == "READY");
    CHECK(ready.rows[1].value == "WORLD / UNIT");

    CHECK(rm::ui::commandCard(move, false).rows[0].value == "UNAVAILABLE");
    CHECK(rm::ui::commandCard(move, true, true).rows[0].value == "TARGETING");
}
