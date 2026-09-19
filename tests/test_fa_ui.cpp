// Player-perspective coverage for ui claims (see docs/fa-exe-analysis-plan.md).
// Filled by the coverage-test wave; each TEST_CASE cites its claim IDs.
//
// What this file pins that the unit tests do not: the CLICK path. The command
// rack, the production panel and the economy window are all tested as pure
// view/hit-test functions elsewhere — but a player never calls those. A player
// clicks a cell, and the claim is that the cell's command reaches the sim
// through the one sink (C-329) and that the window's rows mirror live sim
// state (C-340/C-343). Those are scenario tests: spawn, click, tick, observe.
#include "app/Match.hpp"
#include "app/Interface.hpp"
#include "app/SceneBuild.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/Scmap.hpp"
#include "core/sim/StateHash.hpp"
#include "core/ui/CommandPanel.hpp"
#include "core/ui/EconomyWindow.hpp"
#include "core/ui/Hud.hpp"
#include "core/ui/ProductionPanel.hpp"
#include "core/ui/Viewport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace {

rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

// The same harness test_unit_scenarios.cpp uses: a scene with two armies, the
// player driving army 0, and spawn/runner helpers that go through the real
// catalog and command intake.
struct Scenario {
    rm::HeightField field;
    rm::app::UnitScene scene;
    rm::app::PassabilitySet passability;
    rm::vfs::Vfs content;

    explicit Scenario(rm::HeightField terrain = flatField())
        : field(std::move(terrain)), passability(field, false, 0) {
        scene.armies = rm::sim::freeForAll(2);
        scene.players = rm::sim::onePlayerPerArmy(2, 0);
        scene.playerArmy = 0;
        scene.economies.resize(2);
        scene.commandersEver.assign(2, 0);
    }

    rm::UnitTypeIndex registerType(const rm::unitdef::UnitDef& def) {
        scene.definitions.push_back(def);
        const auto type = scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
        scene.setTypeTraits(type, rm::data::moveDefFor(def), def.meshToElmos);
        const std::string path = "/units/" + def.name + "/" + def.name + "_unit.bp";
        scene.setPathForType(type, path);
        scene.typeForBlueprint.emplace(path, type);
        return type;
    }

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z, int army = 0) {
        const auto type = registerType(def);
        const auto id = scene.store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(def, army),
            .health = rm::sim::initialHealth(def.health),
        });
        return id;
    }

    rm::app::MatchRunner runner() {
        auto result = rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
        result.match.baseStorage = rm::app::kStartingStorage;
        result.scripts.clear();
        return result;
    }
};

/// A mobile combat unit: the kind of thing a player selects and orders.
/// `motion` must be Land — the default None makes the traversal grid
/// impassable and a move order completes instantly without moving.
[[nodiscard]] rm::unitdef::UnitDef tankDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_tank";
    def.categories = {"LAND", "MOBILE"};
    def.motion = rm::unitdef::MotionType::Land;
    def.speedElmosPerSecond = 30.0f;
    def.health = rm::sim::Mag::fromInt(100);
    rm::unitdef::Weapon gun;
    gun.maxRange = rm::sim::fxFromFloat(240.0f);
    gun.rateOfFire = 1.0f;
    gun.damage = rm::sim::Mag::fromInt(50);
    def.weapons = {gun};
    return def;
}

/// An immobile factory: a builder that is not mobile, so `gatherProduction`
/// answers for it and the economy window lists it as a producer.
/// (`categories` must stay sorted — `hasCategory` is a binary search.)
[[nodiscard]] rm::unitdef::UnitDef factoryDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_factory";
    def.description = "Test Land Factory";
    def.categories = {"FACTORY", "STRUCTURE"};
    def.buildRate = 10.0f;
    def.buildableCategory = {{"PRODUCT"}};
    def.health = rm::sim::Mag::fromInt(1000);
    return def;
}

/// A mass fabricator: a producer that is NOT a builder, so its Repair order
/// slot is dead and the production toggle fills it — the shape every retail
/// fabricator has (`RULEUTC_ProductionToggle` authored, no build arm).
[[nodiscard]] rm::unitdef::UnitDef fabricatorDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_fab";
    def.categories = {"MASSFABRICATION", "STRUCTURE"};
    def.producesMassPerSecond = 1.0f;
    def.upkeepEnergyPerSecond = 150.0f;
    def.health = rm::sim::Mag::fromInt(500);
    return def;
}

/// A mobile field engineer, for the `I` idle-selector half of C-367.
[[nodiscard]] rm::unitdef::UnitDef engineerDef() {
    rm::unitdef::UnitDef def;
    def.name = "test_engineer";
    def.categories = {"ENGINEER", "LAND", "MOBILE"};
    def.motion = rm::unitdef::MotionType::Land;
    def.buildRate = 10.0f;
    def.speedElmosPerSecond = 20.0f;
    def.health = rm::sim::Mag::fromInt(100);
    return def;
}

/// The definitions behind a selection, the way `commandPage` wants them.
[[nodiscard]] std::vector<const rm::unitdef::UnitDef*> defsOf(
    const rm::app::UnitScene& scene, std::span<const rm::sim::UnitId> selection) {
    std::vector<const rm::unitdef::UnitDef*> defs;
    for (const rm::sim::UnitId id : selection) {
        if (scene.store.alive(id)) {
            defs.push_back(scene.catalog.def(scene.store.typeAt(id.index)));
        }
    }
    return defs;
}

} // namespace

TEST_CASE("C-329/C-365: a rack cell's command reaches the sim through the one sink",
          "[fa-ui]") {
    // The player-facing half of the single-sink claim: the cell the rack shows
    // (commandPage -> CommandKind, the C-365 cap->kind map) is submitted through
    // `submitCommand` — the same intake every other UI order uses — and lands in
    // the semantic command log the replay consumes (C-150).
    Scenario job;
    const rm::sim::UnitId tank = job.spawn(tankDef(), 300, 300);
    const std::vector<rm::sim::UnitId> selection{tank};
    auto runner = job.runner();

    // Give the tank a move order first, so STOP has something to clear.
    REQUIRE(rm::app::issueMove(job.scene, tank, 0, 0, rm::sim::fxFromFloat(600.0f),
                               rm::sim::fxFromFloat(300.0f)));
    (void)rm::app::advanceMatch(runner, 0, 0);
    REQUIRE_FALSE(job.scene.store.orders()[tank.index].empty());

    // The rack page for this selection: STOP is slot 4 in the fixed FA layout
    // (kCommandDescriptors), enabled because the tank can stop.
    const rm::ui::CommandPage page = rm::ui::commandPage(defsOf(job.scene, selection));
    constexpr std::size_t kStopSlot = 4;  // "STOP" in kCommandDescriptors
    REQUIRE(page[kStopSlot].enabled);
    REQUIRE_FALSE(page[kStopSlot].toggle.has_value());
    const rm::sim::CommandKind kind =
        page[kStopSlot].order.value_or(*rm::ui::kCommandDescriptors[kStopSlot].kind);
    REQUIRE(kind == rm::sim::CommandKind::Stop);

    // The click: what WindowedSession does with an enabled non-toggle cell.
    REQUIRE(rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
        .tick = 1,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = 0,
        .player = 0,
        .kind = kind,
        .units = selection,
    }).has_value());
    (void)rm::app::advanceMatch(runner, 1, 0);

    // Observable outcome: the queue is cleared, and the order is in the log —
    // one sink, one record, replayable.
    CHECK(job.scene.store.orders()[tank.index].empty());
    const std::span<const rm::sim::CommandIssue> logged =
        job.scene.commands.at(1, rm::sim::CommandPhase::PreTick);
    REQUIRE(logged.size() == 1);
    CHECK(logged.front().kind == rm::sim::CommandKind::Stop);
    CHECK(logged.front().units == selection);
}

TEST_CASE("C-333: the production toggle cell pauses the selection's producers",
          "[fa-ui]") {
    // The one RULEUTC toggle backed by sim state: a fabricator's PRODUCTION
    // cell (retail slot 8, Repair's dead order slot — a fab has no build arm,
    // so Repair never lights there) issues ToggleProduction, and the unit's
    // productionPaused flag is what the sim and the card both read.
    Scenario job;
    rm::unitdef::UnitDef fab = fabricatorDef();
    // The cap is what puts the toggle on the page — authored, like the retail
    // fabricators that declare RULEUTC_ProductionToggle.
    fab.toggleCapsDeclared = true;
    fab.toggleCaps = {"RULEUTC_ProductionToggle"};
    const rm::sim::UnitId unit = job.spawn(fab, 300, 300);
    const std::vector<rm::sim::UnitId> selection{unit};
    auto runner = job.runner();

    const rm::ui::CommandPage page = rm::ui::commandPage(defsOf(job.scene, selection));
    // Find the cell the page resolved the production toggle into — the test
    // follows the page, not a hardcoded slot, because slot borrowing is the
    // page's own business (C-333's 1:1 map is pinned in test_command_panel).
    const auto cell = std::ranges::find_if(page, [](const rm::ui::CommandPageCell& c) {
        return c.toggle && rm::ui::kToggleDescriptors[*c.toggle].cap
                              == "RULEUTC_ProductionToggle";
    });
    REQUIRE(cell != page.end());
    REQUIRE(cell->enabled);

    // The click, as the session issues it for the one backed toggle.
    REQUIRE(rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
        .tick = 0,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = 0,
        .player = 0,
        .kind = rm::sim::CommandKind::ToggleProduction,
        .units = selection,
    }).has_value());
    (void)rm::app::advanceMatch(runner, 0, 0);
    CHECK(job.scene.store.productionPaused(unit));

    // A second click on the same cell resumes — the toggle, not a one-way stop.
    REQUIRE(rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
        .tick = 1,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = 0,
        .player = 0,
        .kind = rm::sim::CommandKind::ToggleProduction,
        .units = selection,
    }).has_value());
    (void)rm::app::advanceMatch(runner, 1, 0);
    CHECK_FALSE(job.scene.store.productionPaused(unit));
}

TEST_CASE("C-332/C-340: the economy window's pause cell pauses the producer it names",
          "[fa-ui]") {
    // The full round trip a player sees: the window's row is gathered FROM the
    // sim (C-340's sim->UI mirror), the pause cell hit-tests to that row, and
    // the click issues ToggleProduction back through the sink (C-332's pause
    // control — a window cell, not a rack button, matching retail's checkbox).
    Scenario job;
    const rm::sim::UnitId fab = job.spawn(factoryDef(), 300, 300);
    auto runner = job.runner();

    rm::ui::EconomyWindowView view;
    rm::app::gatherEconomyWindow(job.scene, {}, -1.0f, std::nullopt, view);
    const auto row = std::ranges::find_if(view.rows, [&](const rm::ui::EconRowView& r) {
        return std::ranges::find(r.members, fab) != r.members.end();
    });
    REQUIRE(row != view.rows.end());
    REQUIRE(row->pausable);
    CHECK(row->pausedCount == 0);

    // The click lands on the row's pause cell, addressed the way the session
    // addresses it: display-row index inside the drawn page.
    const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(1280, 720));
    const rm::ui::Rect rect = rm::ui::economyWindowRect(frame, view.rows.size());
    const auto displayRow = static_cast<std::size_t>(row - view.rows.begin());
    const rm::ui::Rect cell = rm::ui::econPauseCellRect(rect, displayRow);
    const float px = cell.x + cell.width / 2.0f;
    const float py = cell.y + cell.height / 2.0f;
    const auto hit = rm::ui::econPauseAt(rect, view, px, py);
    REQUIRE(hit.has_value());
    REQUIRE(*hit == displayRow);

    REQUIRE(rm::app::submitCommand(job.scene, rm::sim::CommandIssue{
        .tick = 0,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = 0,
        .player = 0,
        .kind = rm::sim::CommandKind::ToggleProduction,
        .units = view.rows[*hit].members,
    }).has_value());
    (void)rm::app::advanceMatch(runner, 0, 0);
    CHECK(job.scene.store.productionPaused(fab));

    // And the mirror reports it back: the same gather now shows the row paused.
    rm::app::gatherEconomyWindow(job.scene, {}, -1.0f, std::nullopt, view);
    const auto after = std::ranges::find_if(view.rows, [&](const rm::ui::EconRowView& r) {
        return std::ranges::find(r.members, fab) != r.members.end();
    });
    REQUIRE(after != view.rows.end());
    CHECK(after->pausedCount == 1);
    CHECK(after->allPaused());
}

TEST_CASE("C-366: the repeat cell's ProcessInfo reaches the factory's standing order",
          "[fa-ui]") {
    // Retail's SetRepeatQueue rides UserUnit::ProcessInfo, the unfiltered
    // (name,value) forwarder; ours is the typed ToggleFactoryRepeat command —
    // same player-facing contract: click the cell, the factory repeats.
    Scenario job;
    const rm::sim::UnitId fab = job.spawn(factoryDef(), 300, 300);
    auto runner = job.runner();

    const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(1280, 720));
    const rm::ui::Rect rect = rm::ui::productionPanelRect(frame);
    const rm::ui::Rect repeat = rm::ui::productionRepeatRect(rect);
    REQUIRE(rect.width > 0.0f);

    // The view the click is addressed against — the sim's own queue, gathered.
    REQUIRE(rm::app::gatherProduction(job.scene, fab).has_value());
    CHECK_FALSE(job.scene.store.factoryRepeat(fab));

    REQUIRE(rm::app::submitProductionControl(job.scene, fab, 0, 0, frame,
                                             repeat.x + 1.0f, repeat.y + 1.0f));
    (void)rm::app::advanceMatch(runner, 0, 0);
    CHECK(job.scene.store.factoryRepeat(fab));
    CHECK(rm::app::gatherProduction(job.scene, fab)->repeat);

    // The same cell toggles it back off.
    REQUIRE(rm::app::submitProductionControl(job.scene, fab, 0, 1, frame,
                                             repeat.x + 1.0f, repeat.y + 1.0f));
    (void)rm::app::advanceMatch(runner, 1, 0);
    CHECK_FALSE(job.scene.store.factoryRepeat(fab));
}

TEST_CASE("C-367: the idle selectors never offer a dead unit", "[fa-ui]") {
    // `+idle` is the implemented half of UI_SelectByCategory's flag alphabet:
    // the I/C keys answer the player's idle engineers and idle mobile combat
    // units across the whole map. The edge that would be a real bug is a dead
    // unit answering — the player would select a corpse.
    Scenario job;
    const rm::sim::UnitId tank = job.spawn(tankDef(), 300, 300);
    const rm::sim::UnitId engineer = job.spawn(engineerDef(), 400, 300);

    CHECK(rm::app::idleMobileCombatUnits(job.scene) == std::vector<rm::sim::UnitId>{tank});
    CHECK(rm::app::idleFieldEngineers(job.scene) == std::vector<rm::sim::UnitId>{engineer});

    job.scene.store.kill(tank);
    job.scene.store.kill(engineer);
    CHECK(rm::app::idleMobileCombatUnits(job.scene).empty());
    CHECK(rm::app::idleFieldEngineers(job.scene).empty());
}
