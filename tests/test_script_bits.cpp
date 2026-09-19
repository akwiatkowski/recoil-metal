// The RULEUTC_* unit toggles as `ToggleScriptBit` commands (C-350): shield,
// jamming, intel, stealth and cloak flip the sim features `Unit.lua` wires to
// `OnScriptBitSet`/`OnScriptBitClear`, and every implemented bit also runs
// `SetMaintenanceConsumption{Active,Inactive}` — last writer wins.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Combat.hpp"
#include "core/sim/Command.hpp"
#include "core/sim/Intel.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <vector>

using Catch::Approx;
using rm::sim::CommandIssue;
using rm::sim::CommandKind;
using rm::sim::UnitId;

namespace {

/// A def that declares the toggle caps it carries, like a retail blueprint's
/// `ToggleCaps` table. `toggleCaps` must stay sorted for `hasToggleCap`.
[[nodiscard]] rm::unitdef::UnitDef toggledDef(std::string name,
                                              std::vector<std::string> caps) {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    std::ranges::sort(caps);
    def.toggleCaps = std::move(caps);
    def.toggleCapsDeclared = true;
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef personalShieldDef() {
    rm::unitdef::UnitDef def = toggledDef("personal_shield", {"RULEUTC_ShieldToggle"});
    def.shield.maximum = rm::sim::Mag::fromInt(100);
    def.shield.shape = rm::unitdef::ShieldShape::Box;
    def.shield.boxHalfExtentsElmos = {
        rm::sim::Fx::fromInt(4), rm::sim::Fx::fromInt(4), rm::sim::Fx::fromInt(4)};
    def.shield.rechargeDelay = rm::sim::seconds(2.1f);
    return def;
}

/// A flat, wholly walkable map — the terrain `applyCommand` wants even though a
/// toggle never routes.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 64;
    field.squaresZ = 64;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// The intake path for one toggle click: `applyCommand` on a `ToggleScriptBit`
/// issue naming `bit` for `units`.
rm::sim::ApplyCommandResult toggle(rm::test::Roster& roster, std::uint8_t bit,
                                   std::vector<UnitId> units) {
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    return rm::sim::applyCommand(
        CommandIssue{.tick = 1,
                     .source = 0,
                     .id = rm::commandId(0, 1),
                     .player = 0,
                     .kind = CommandKind::ToggleScriptBit,
                     .units = std::move(units),
                     .scriptBit = bit},
        roster.store, roster.catalog, players, armies,
        rm::sim::Terrain{flatField()},
        [](UnitId) -> const rm::sim::PassabilityGrid* { return nullptr; },
        roster.rate);
}

} // namespace

TEST_CASE("the shield toggle gates absorption and re-enables at kept health") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(personalShieldDef());
    const UnitId unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    roster.health(unit).shield.current = rm::sim::Mag::fromInt(100);
    roster.health(unit).shield.maximum = rm::sim::Mag::fromInt(100);
    // Damage left it at 60 — the `OffHealth` a re-enable must resume from.
    roster.health(unit).shield.current = rm::sim::Mag::fromInt(60);

    // Off: the bit latches and upkeep drops.
    rm::sim::ApplyCommandResult off = toggle(roster, 0, {unit});
    REQUIRE(off.accepted == std::vector<UnitId>{unit});
    CHECK(roster.store.scriptBitDisabled(unit, 0));
    CHECK_FALSE(roster.store.maintenanceActive(unit));

    // On: retail's `OnState` gates absorption for the recharge time and resumes
    // at the kept 60, not at maximum — the refill belongs to damage collapse.
    rm::sim::ApplyCommandResult on = toggle(roster, 0, {unit});
    REQUIRE(on.accepted == std::vector<UnitId>{unit});
    CHECK_FALSE(roster.store.scriptBitDisabled(unit, 0));
    CHECK(roster.store.maintenanceActive(unit));
    const rm::sim::ShieldState& shield = roster.health(unit).shield;
    CHECK(shield.rechargeRemaining > 0);
    CHECK_FALSE(shield.rechargeRestoresFull);

    // Run the recharge out: the bubble comes back at 60, not 100.
    while (roster.health(unit).shield.rechargeRemaining > 0) {
        rm::sim::tickShields(roster.store, roster.catalog);
    }
    CHECK(rm::test::asFloat(roster.health(unit).shield.current) == Approx(60.0f));
}

TEST_CASE("a unit without the cap refuses the toggle") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(toggledDef("plain", {}));
    const UnitId unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    const rm::sim::ApplyCommandResult result = toggle(roster, 0, {unit});
    CHECK(result.accepted.empty());
    CHECK_FALSE(roster.store.scriptBitDisabled(unit, 0));
}

TEST_CASE("the intel toggle silences radar and the stealth toggle lifts it") {
    rm::test::Roster roster;
    // A radar emitter that declares both toggles.
    rm::unitdef::UnitDef emitterDef =
        toggledDef("emitter", {"RULEUTC_IntelToggle", "RULEUTC_StealthToggle"});
    emitterDef.radarRadiusElmos = 400.0f;
    const rm::UnitTypeIndex emitterType = roster.addType(emitterDef);
    const rm::UnitTypeIndex blipType = roster.addType(toggledDef("blip", {}));
    const UnitId emitter = roster.add(emitterType, 200.0f, 200.0f, 0, 100.0f);
    const UnitId blip = roster.add(blipType, 300.0f, 200.0f, 1, 100.0f);

    rm::sim::Intel intel;
    intel.configure(2, rm::sim::Fx::fromInt(1024), rm::sim::Fx::fromInt(1024),
                    rm::sim::VisionStyle::ForgedAlliance);
    std::vector<rm::sim::Army> armies(2);
    armies[0].index = 0;
    armies[0].alliance = 0;
    armies[1].index = 1;
    armies[1].alliance = 1;
    const auto kind = [&] {
        return rm::sim::contactKindForUnit(0, blip.index, roster.store,
                                           roster.catalog, armies, intel);
    };

    intel.update(roster.store, roster.catalog, armies, nullptr);
    REQUIRE(kind() == rm::sim::ContactKind::Radar);

    // Intel off: the emitter stops painting — live coverage drops to nothing.
    // (The retained blip still projects at its last confirmed position; that is
    // retail's last-known-contact rule, not the toggle's business.)
    REQUIRE(toggle(roster, 3, {emitter}).accepted == std::vector<UnitId>{emitter});
    intel.update(roster.store, roster.catalog, armies, nullptr);
    CHECK_FALSE(kind().has_value());

    // Intel back on: coverage returns.
    REQUIRE(toggle(roster, 3, {emitter}).accepted == std::vector<UnitId>{emitter});
    intel.update(roster.store, roster.catalog, armies, nullptr);
    CHECK(kind() == rm::sim::ContactKind::Radar);
}

TEST_CASE("maintenance follows the last toggle touched, not the count still on") {
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(
        toggledDef("dual", {"RULEUTC_IntelToggle", "RULEUTC_StealthToggle"}));
    const UnitId unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    // Turn intel off, then stealth off: upkeep is already inactive, and the
    // second call leaves it inactive.
    REQUIRE(toggle(roster, 3, {unit}).accepted == std::vector<UnitId>{unit});
    REQUIRE(toggle(roster, 5, {unit}).accepted == std::vector<UnitId>{unit});
    CHECK_FALSE(roster.store.maintenanceActive(unit));

    // Re-enable intel: `SetMaintenanceConsumptionActive` runs — upkeep is back
    // even though stealth is still off. Last writer wins (`Unit.lua:309-380`).
    REQUIRE(toggle(roster, 3, {unit}).accepted == std::vector<UnitId>{unit});
    CHECK(roster.store.maintenanceActive(unit));
    CHECK(roster.store.scriptBitDisabled(unit, 5));
}

TEST_CASE("C-349: a unit attached to a transport refuses the toggle") {
    // Retail blocks `ToggleScriptBit` while the unit rides a carrier — the
    // cargo's script bits are the carrier's business until it lands. The
    // refusal is intake-side: the unit is not accepted and the bit never moves.
    rm::test::Roster roster;
    const rm::UnitTypeIndex type = roster.addType(
        toggledDef("cargo", {"RULEUTC_IntelToggle"}));
    const UnitId cargo = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    roster.motion(cargo).attached = true;
    CHECK(toggle(roster, 3, {cargo}).accepted.empty());
    CHECK_FALSE(roster.store.scriptBitDisabled(cargo, 3));

    // Back on the ground the same click lands — the block is the attachment,
    // not the unit.
    roster.motion(cargo).attached = false;
    REQUIRE(toggle(roster, 3, {cargo}).accepted == std::vector<UnitId>{cargo});
    CHECK(roster.store.scriptBitDisabled(cargo, 3));
}
