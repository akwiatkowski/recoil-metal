#include <catch2/catch_test_macros.hpp>

#include "core/ui/CommandPanel.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>

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
    CHECK(rm::ui::kCommandDescriptors[5].kind == CommandKind::Guard);
    CHECK(rm::ui::kCommandDescriptors[6].kind == rm::sim::CommandKind::Dive);
    CHECK(rm::ui::kCommandDescriptors[7].kind == CommandKind::Overcharge);
    CHECK(rm::ui::kCommandDescriptors[8].kind == CommandKind::Repair);
    CHECK(rm::ui::kCommandDescriptors[9].kind == CommandKind::Assist);
    CHECK_FALSE(rm::ui::kCommandDescriptors[10].kind.has_value());
    CHECK(rm::ui::kCommandDescriptors[10].action == rm::ui::RackAction::AutoExpand);
    CHECK(rm::ui::rackSlotFor(rm::ui::RackAction::AutoExpand) == 10);
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
    CHECK(enabled(available, CommandKind::Guard));
    CHECK(enabled(available, CommandKind::Overcharge));
    CHECK(enabled(available, CommandKind::Reclaim));
    CHECK(enabled(available, CommandKind::Repair));
    CHECK(enabled(available, CommandKind::Assist));
    CHECK_FALSE(available[6]);
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
    CHECK(enabled(factoryAvailable, CommandKind::Guard));
    CHECK(enabled(factoryAvailable, CommandKind::Assist));

    rm::unitdef::UnitDef engineer;
    engineer.buildRate = 5.0f;
    const std::array<const rm::unitdef::UnitDef*, 1> engineerSelection{&engineer};
    const rm::ui::CommandAvailability engineerAvailable =
        rm::ui::commandAvailability(engineerSelection);
    CHECK(enabled(engineerAvailable, CommandKind::Reclaim));
    CHECK(enabled(engineerAvailable, CommandKind::Repair));
    CHECK(enabled(engineerAvailable, CommandKind::Guard));
    CHECK(enabled(engineerAvailable, CommandKind::Assist));

    rm::unitdef::UnitDef commander;
    commander.categories = {"COMMAND"};
    const std::array<const rm::unitdef::UnitDef*, 1> commanderSelection{&commander};
    const rm::ui::CommandAvailability commanderAvailable =
        rm::ui::commandAvailability(commanderSelection);
    CHECK_FALSE(enabled(commanderAvailable, CommandKind::Reclaim));
    CHECK(enabled(commanderAvailable, CommandKind::Guard));
    CHECK_FALSE(enabled(commanderAvailable, CommandKind::Assist));  // no build arm to lend

    // URL0107's shape: primarily a combat unit, but its BuildRate=1 repair arm can assist.
    rm::unitdef::UnitDef combatBuilder;
    combatBuilder.buildRate = 1.0f;
    combatBuilder.categories = {"DIRECTFIRE", "LAND", "MOBILE", "TECH1"};
    combatBuilder.weapons.push_back(weapon(false));
    const std::array<const rm::unitdef::UnitDef*, 1> combatSelection{&combatBuilder};
    const rm::ui::CommandAvailability combatAvailable =
        rm::ui::commandAvailability(combatSelection);
    CHECK(enabled(combatAvailable, CommandKind::Attack));
    CHECK(enabled(combatAvailable, CommandKind::Guard));
}

TEST_CASE("auto-expand is offered to field builders only, and its card reads on or off") {
    rm::unitdef::UnitDef engineer;
    engineer.buildRate = 5.0f;
    engineer.speedElmosPerSecond = 10.0f;
    rm::unitdef::UnitDef factory;
    factory.buildRate = 5.0f;  // builds, but cannot walk to a deposit
    factory.categories = {"FACTORY"};
    rm::unitdef::UnitDef tank;
    tank.speedElmosPerSecond = 10.0f;

    const std::size_t slot = rm::ui::rackSlotFor(rm::ui::RackAction::AutoExpand);
    const std::array<const rm::unitdef::UnitDef*, 1> engineerOnly{&engineer};
    CHECK(rm::ui::commandAvailability(engineerOnly)[slot]);
    const std::array<const rm::unitdef::UnitDef*, 2> immobileOrUnarmed{&factory, &tank};
    CHECK_FALSE(rm::ui::commandAvailability(immobileOrUnarmed)[slot]);

    const rm::ui::CommandDescriptor& cell = rm::ui::kCommandDescriptors[slot];
    const std::array<const rm::unitdef::UnitDef*, 2> mixed{&engineer, &tank};
    const auto off = rm::ui::commandCard(cell, mixed, false);
    CHECK(off.title == "AUTO MEX");
    CHECK(off.rows[0].value == "OFF");
    CHECK(off.rows[1].value == "1 OF 2 UNITS");
    CHECK(rm::ui::commandCard(cell, mixed, true).rows[0].value == "ON");
    CHECK(rm::ui::commandCard(cell, immobileOrUnarmed).rows[0].value
          == "SELECTION CANNOT DO THIS");
}

TEST_CASE("an authored retail command page distinguishes repair from reclaim") {
    rm::unitdef::UnitDef mantis;
    mantis.speedElmosPerSecond = 10.0f;
    mantis.buildRate = 1.0f;
    mantis.weapons.push_back(weapon(false));
    mantis.commandCapsDeclared = true;
    mantis.commandCaps = {"RULEUCC_Attack", "RULEUCC_Guard", "RULEUCC_Move",
                          "RULEUCC_Patrol", "RULEUCC_Repair", "RULEUCC_Stop"};

    const std::array<const rm::unitdef::UnitDef*, 1> selection{&mantis};
    const rm::ui::CommandAvailability available = rm::ui::commandAvailability(selection);
    CHECK(enabled(available, CommandKind::Move));
    CHECK(enabled(available, CommandKind::Attack));
    CHECK(enabled(available, CommandKind::Patrol));
    CHECK(enabled(available, CommandKind::Stop));
    CHECK(enabled(available, CommandKind::Guard));
    CHECK(enabled(available, CommandKind::Repair));
    CHECK_FALSE(enabled(available, CommandKind::Reclaim));
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
    rm::unitdef::UnitDef mobile;
    mobile.speedElmosPerSecond = 10;
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&mobile};
    const rm::ui::InfoCard ready = rm::ui::commandCard(move, selection);
    REQUIRE(ready.rows.size() == 3);
    CHECK(ready.title == "MOVE");
    CHECK(ready.rows[0].value == "READY");
    CHECK(ready.rows[1].value == "1 OF 1 UNITS");

    CHECK(rm::ui::commandCard(move, {}).rows[0].value == "SELECT A UNIT");
    CHECK(rm::ui::commandCard(move, selection, true).rows[0].value == "TARGETING");
}

TEST_CASE("command explanations distinguish unsupported actions and mixed selections", "[ui][command-reason]") {
    rm::unitdef::UnitDef mobile;
    mobile.speedElmosPerSecond = 10;
    rm::unitdef::UnitDef building;
    const std::array<const rm::unitdef::UnitDef*, 3> mixed{&mobile, nullptr, &building};
    const auto& move = rm::ui::kCommandDescriptors[1];
    const auto card = rm::ui::commandCard(move, mixed);
    CHECK(card.rows[0].value == "READY");
    CHECK(card.rows[1].value == "1 OF 2 UNITS");
    mobile.commandCapsDeclared = true;  // mobile, but the blueprint forbids movement
    const auto disabled = rm::ui::commandCard(move, mixed);
    CHECK(disabled.rows[0].value == "SELECTION CANNOT DO THIS");
    CHECK(disabled.rows.back().value == "SELECT A UNIT WITH THIS COMMAND");
    const auto unsupported = rm::ui::commandCard(rm::ui::CommandDescriptor{}, mixed);
    CHECK(unsupported.rows[0].value == "NOT IMPLEMENTED");
    CHECK(unsupported.rows.back().value == "NO UNIT CAN USE THIS YET");
}

TEST_CASE("toggle availability follows authored toggle caps", "[ui][toggles]") {
    rm::unitdef::UnitDef shield;
    shield.toggleCapsDeclared = true;
    shield.toggleCaps = {"RULEUTC_IntelToggle", "RULEUTC_ShieldToggle"};
    rm::unitdef::UnitDef plain;
    const std::array<const rm::unitdef::UnitDef*, 3> mixed{&shield, nullptr, &plain};
    const rm::ui::ToggleAvailability present = rm::ui::toggleAvailability(mixed);
    // Undeclared tables vote nothing: the plain structure contributes no toggles.
    CHECK(present[0]);
    CHECK(present[3]);
    CHECK(std::ranges::count(present, true) == 2);
    CHECK(std::ranges::none_of(rm::ui::toggleAvailability({}), std::identity{}));
}

TEST_CASE("order overrides merge unanimously across the selection", "[ui][toggles]") {
    rm::unitdef::UnitDef first;
    first.orderOverrides["RULEUTC_ShieldToggle"] = {"shield-dome", "toggle_shield_dome"};
    rm::unitdef::UnitDef second = first;
    rm::unitdef::UnitDef silent;
    const std::array<const rm::unitdef::UnitDef*, 3> unanimous{&first, &second, &silent};
    const auto merged = rm::ui::orderOverrides(unanimous);
    REQUIRE(merged.size() == 1);
    CHECK(merged.at("RULEUTC_ShieldToggle").bitmapId == "shield-dome");
    CHECK(merged.at("RULEUTC_ShieldToggle").helpText == "toggle_shield_dome");

    // One disagreeing help text drops the key; units without it do not vote.
    second.orderOverrides["RULEUTC_ShieldToggle"].helpText = "toggle_other";
    CHECK(rm::ui::orderOverrides(unanimous).empty());
}

TEST_CASE("the command page fills dead order slots with toggles", "[ui][toggles]") {
    // An immobile unarmed structure: only Stop lives, so Dive's slot takes the
    // retail Shield toggle at its preferred position — present but disabled.
    rm::unitdef::UnitDef shield;
    shield.toggleCapsDeclared = true;
    shield.toggleCaps = {"RULEUTC_ShieldToggle"};
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&shield};
    const rm::ui::CommandPage page = rm::ui::commandPage(selection);
    REQUIRE(page[6].toggle.has_value());
    CHECK(page[6].toggle == 0);
    CHECK(page[6].name == "SHIELD");
    CHECK(page[6].icon == "shield");
    CHECK_FALSE(page[6].enabled);
    CHECK(page[4].name == "STOP");
    CHECK(page[4].enabled);
    CHECK_FALSE(page[4].toggle.has_value());
}

TEST_CASE("orders keep their slots when a toggle wants them too", "[ui][toggles]") {
    // A field builder: Auto-expand lives on slot 10, so the Special toggle that
    // shares retail's eleventh slot stays out of the page. Intel takes the dead
    // Overcharge slot beside it.
    rm::unitdef::UnitDef builder;
    builder.speedElmosPerSecond = 10.0f;
    builder.buildRate = 5.0f;
    builder.toggleCapsDeclared = true;
    builder.toggleCaps = {"RULEUTC_IntelToggle", "RULEUTC_SpecialToggle"};
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&builder};
    const rm::ui::CommandPage page = rm::ui::commandPage(selection);
    CHECK(page[10].name == "AUTO MEX");
    CHECK(page[10].enabled);
    CHECK_FALSE(page[10].toggle.has_value());
    REQUIRE(page[7].toggle.has_value());
    CHECK(page[7].name == "INTEL");
    CHECK_FALSE(page[7].enabled);
}

TEST_CASE("overrides relabel the cells they agree on", "[ui][toggles]") {
    rm::unitdef::UnitDef shield;
    shield.toggleCapsDeclared = true;
    shield.toggleCaps = {"RULEUTC_ShieldToggle"};
    shield.orderOverrides["RULEUTC_ShieldToggle"] = {"shield-dome", "toggle_shield_dome"};
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&shield};
    const rm::ui::CommandPage page = rm::ui::commandPage(selection);
    REQUIRE(page[6].toggle.has_value());
    CHECK(page[6].name == "toggle_shield_dome");
    CHECK(page[6].icon == "shield-dome");
}

TEST_CASE("toggle cards report present-but-unsupported actions", "[ui][toggles]") {
    rm::unitdef::UnitDef shield;
    shield.toggleCapsDeclared = true;
    shield.toggleCaps = {"RULEUTC_ShieldToggle"};
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&shield};
    const rm::ui::InfoCard card = rm::ui::toggleCard(rm::ui::kToggleDescriptors[0], selection);
    CHECK(card.title == "SHIELD TOGGLE");
    REQUIRE(card.rows.size() == 3);
    CHECK(card.rows[0].value == "NOT IMPLEMENTED");
    CHECK(card.rows[1].value == "1 OF 1 UNITS");

    const rm::ui::CommandPage page = rm::ui::commandPage(selection);
    const rm::ui::InfoCard routed = rm::ui::commandInspector(page, 6, selection);
    CHECK(routed.title == "SHIELD TOGGLE");
    const rm::ui::InfoCard order = rm::ui::commandInspector(page, 4, selection);
    CHECK(order.title == "STOP");
}

TEST_CASE("the retail shield's authored toggle reaches its rack cell", "[ui][toggles][corpus]") {
    // End to end through the real blueprint: UEB4301 authors ShieldToggle plus an
    // override for it, so its rack shows the dome bitmap and help key — disabled.
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        home ? std::filesystem::path{home} / "projects/llm/input/faf/units" : std::filesystem::path{};
    if (!std::filesystem::is_directory(root)) SKIP("no retail unit corpus");
    const auto def = rm::unitbp::loadFile(root / "UEB4301/UEB4301_unit.bp");
    REQUIRE(def);
    CHECK(def->toggleCapsDeclared);
    CHECK(def->hasToggleCap("RULEUTC_ShieldToggle"));
    const std::array<const rm::unitdef::UnitDef*, 1> selection{&*def};
    const rm::ui::CommandPage page = rm::ui::commandPage(selection);
    REQUIRE(page[6].toggle.has_value());
    CHECK(page[6].name == "toggle_shield_dome");
    CHECK(page[6].icon == "shield-dome");
    CHECK_FALSE(page[6].enabled);
}
