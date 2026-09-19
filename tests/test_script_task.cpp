#include <catch2/catch_test_macros.hpp>

#include "core/sim/CommandQueue.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 8;
    field.squaresZ = 8;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

class RecordingTasks final : public rm::sim::ScriptTaskHost {
public:
    void onCreate(rm::sim::UnitId unit, std::string_view task,
                  std::span<const std::uint8_t> commandData,
                  rm::sim::ScriptTaskState& state) override {
        events.push_back("create:" + std::string{task});
        createdUnit = unit;
        seenCommandData.assign(commandData.begin(), commandData.end());
        state.opaque = {0xCA, 0xFE};
    }

    std::int32_t taskTick(rm::sim::UnitId, std::string_view task,
                          std::span<const std::uint8_t>,
                          rm::sim::ScriptTaskState& state) override {
        events.push_back("tick:" + std::string{task});
        ++state.aiResult;
        REQUIRE_FALSE(results.empty());
        const std::int32_t result = results.front();
        results.pop_front();
        return result;
    }

    void onDestroy(rm::sim::UnitId, std::string_view task,
                   std::span<const std::uint8_t>,
                   rm::sim::ScriptTaskState& state) override {
        events.push_back("destroy:" + std::string{task});
        destroyedState = state.opaque;
    }

    std::deque<std::int32_t> results;
    std::vector<std::string> events;
    rm::sim::UnitId createdUnit{};
    std::vector<std::uint8_t> seenCommandData;
    std::vector<std::uint8_t> destroyedState;
};

struct Fixture {
    Fixture() : terrain(field), grid(rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f)) {
        rm::unitdef::UnitDef def;
        def.name = "scriptable";
        type = roster.addType(def);
        unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    }

    [[nodiscard]] rm::sim::ApplyCommandResult issue(std::string task,
                                                     std::vector<std::uint8_t> data = {},
                                                     bool queued = false) {
        return rm::sim::applyCommand(
            rm::sim::CommandIssue{.source = 0,
                                  .id = rm::commandId(0, nextId++),
                                  .player = 0,
                                  .kind = rm::sim::CommandKind::Script,
                                  .queued = queued,
                                  .units = {unit},
                                  .scriptTask = std::move(task),
                                  .scriptData = std::move(data)},
            roster.store, roster.catalog, players, armies, terrain,
            [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
            nullptr, &tasks);
    }

    [[nodiscard]] std::size_t advance() {
        const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
        return rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                      nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                      nullptr, &tasks);
    }

    rm::HeightField field = flatField();
    rm::sim::Terrain terrain;
    rm::sim::PassabilityGrid grid;
    rm::test::Roster roster;
    rm::UnitTypeIndex type{};
    rm::sim::UnitId unit{};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    RecordingTasks tasks;
    std::uint32_t nextId = 1;
};

} // namespace

TEST_CASE("script tasks repeat, sleep, finish, and destroy through native dispatch") {
    Fixture fixture;
    fixture.tasks.results = {
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Repeat),
        2,
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done),
    };

    REQUIRE(fixture.issue("TargetLocation", {1, 2, 3}));
    CHECK(fixture.advance() == 1);
    REQUIRE(fixture.roster.store.orders()[fixture.unit.index].active() != nullptr);
    const rm::sim::ScriptTaskState& sleeping =
        fixture.roster.store.orders()[fixture.unit.index].active()->scriptState();
    CHECK(sleeping.created);
    CHECK(sleeping.sleepBeats == 1);
    CHECK(sleeping.aiResult == 2);
    CHECK(sleeping.opaque == std::vector<std::uint8_t>{0xCA, 0xFE});
    CHECK(fixture.tasks.seenCommandData == std::vector<std::uint8_t>{1, 2, 3});

    CHECK(fixture.advance() == 0);
    REQUIRE(fixture.roster.store.orders()[fixture.unit.index].active() != nullptr);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].active()->scriptState().sleepBeats == 0);

    CHECK(fixture.advance() == 0);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].empty());
    CHECK(fixture.tasks.events
          == std::vector<std::string>{"create:TargetLocation", "tick:TargetLocation",
                                      "tick:TargetLocation", "tick:TargetLocation",
                                      "destroy:TargetLocation"});
    CHECK(fixture.tasks.destroyedState == std::vector<std::uint8_t>{0xCA, 0xFE});
}

TEST_CASE("script task delay, suspension, resumption, and abort preserve lifecycle") {
    Fixture fixture;
    fixture.tasks.results = {
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Delay),
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Suspend),
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Abort),
    };

    REQUIRE(fixture.issue("EnhanceTask"));
    CHECK(fixture.advance() == 1);
    rm::sim::QueuedCommand* active =
        fixture.roster.store.orders()[fixture.unit.index].activeMutable();
    REQUIRE(active != nullptr);
    CHECK(active->scriptState().suspended);
    CHECK(fixture.tasks.events.size() == 3); // create, ordinary tick, end-of-beat tick

    active->scriptState().suspended = false;
    CHECK(fixture.advance() == 0);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].empty());
    CHECK(fixture.tasks.events.back() == "destroy:EnhanceTask");
}

TEST_CASE("replacing an active script task invokes OnDestroy exactly once") {
    Fixture fixture;
    fixture.tasks.results = {static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::NextBeat)};
    REQUIRE(fixture.issue("TargetLocation"));
    REQUIRE(fixture.advance() == 1);

    REQUIRE(rm::sim::applyCommand(
        rm::sim::Command{.player = 0, .kind = rm::sim::CommandKind::Move,
                         .unit = fixture.unit, .targetX = rm::sim::Fx::fromInt(60),
                         .targetZ = rm::sim::Fx::fromInt(40)},
        fixture.roster.store, fixture.roster.catalog, fixture.players, fixture.armies,
        fixture.terrain, fixture.grid, fixture.roster.rate, nullptr, nullptr, nullptr, nullptr,
        &fixture.tasks));
    CHECK(std::ranges::count(fixture.tasks.events, "destroy:TargetLocation") == 1);
}

TEST_CASE("script task identity and execution state survive save and command-log round trips") {
    Fixture fixture;
    fixture.tasks.results = {static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::NextBeat)};
    REQUIRE(fixture.issue("TargetLocation", {9, 8, 7}));
    REQUIRE(fixture.advance() == 1);

    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 12, .random = random.snapshot(), .units = fixture.roster.store.snapshot()});
    const std::optional<rm::sim::SaveState> saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    rm::sim::UnitStore restored{saved->units};
    const rm::sim::QueuedCommand* task = restored.orders()[fixture.unit.index].active();
    REQUIRE(task != nullptr);
    CHECK(task->payload().scriptTask == "TargetLocation");
    CHECK(task->payload().scriptData == std::vector<std::uint8_t>{9, 8, 7});
    CHECK(task->scriptState().created);
    CHECK(task->scriptState().opaque == std::vector<std::uint8_t>{0xCA, 0xFE});

    rm::sim::CommandLog log;
    REQUIRE(log.record(rm::sim::CommandIssue{.tick = 12,
                                             .source = 0,
                                             .id = rm::commandId(0, 77),
                                             .player = 0,
                                             .kind = rm::sim::CommandKind::Script,
                                             .units = {fixture.unit},
                                             .scriptTask = "Target Location",
                                             .scriptData = {0, 0xA5, 0xFF}}));
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "rm-script-command-log.txt";
    REQUIRE(rm::sim::writeCommandLog(log, path.string()));
    const std::optional<rm::sim::CommandLog> replay = rm::sim::readCommandLog(path.string());
    REQUIRE(replay.has_value());
    CHECK(std::ranges::equal(log.all(), replay->all()));
    std::filesystem::remove(path);
}

TEST_CASE("opaque script execution state contributes to the match hash") {
    Fixture fixture;
    fixture.tasks.results = {static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::NextBeat)};
    REQUIRE(fixture.issue("TargetLocation"));
    REQUIRE(fixture.advance() == 1);

    rm::sim::UnitStore changed{fixture.roster.store.snapshot()};
    changed.orders()[fixture.unit.index].activeMutable()->scriptState().opaque.push_back(1);
    rm::sim::Match match{.armies = fixture.armies, .economies = {}, .passability = {}};
    CHECK(rm::sim::hashMatch(fixture.roster.store, match) != rm::sim::hashMatch(changed, match));
}

TEST_CASE("native enhancement task installs only after funded work and cancels without refund", "[enhancement][script-task]") {
    using namespace rm::sim;
    const auto field = flatField();
    const Terrain terrain{field};
    const auto grid = buildPassability(field, 0, 60, 0);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(100);
    const auto parameters = rm::lua::parseTable("{ NewBuildRate=30, NewHealth=20, NewRegenRate=2 }");
    REQUIRE(parameters);
    def.enhancements.push_back({.name="AdvancedEngineering", .slot="LCH",
        .buildCostMass=Mag::fromInt(8), .buildCostEnergy=Mag::fromInt(80),
        .buildTime=Fx::fromInt(8), .parameters=*parameters});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);
    auto armies = freeForAll(1);
    std::vector<Player> players{{.index=0,.army=0}};
    std::vector<Economy> economies(1);
    std::vector<EnhancementWork> work;
    EnhancementTasks tasks(roster.store, roster.catalog, work);
    const std::vector<const PassabilityGrid*> grids{&grid};
    Match match{.armies=armies, .economies=economies, .enhancements=&work,
        .passability=grids, .scriptTasks=&tasks, .baseStorage={Mag::fromInt(100),Mag::fromInt(1000)}};
    const std::string name = "AdvancedEngineering";
    const auto issue = [&] {
        return applyCommand(CommandIssue{.source=0,.id=roster.store.allocateCommandId(0).value(),
            .player=0,.kind=CommandKind::Script,.units={unit},.scriptTask="EnhanceTask",
            .scriptData={name.begin(),name.end()}}, roster.store, roster.catalog, players, armies,
            terrain, [&](UnitId) { return &grid; }, roster.rate, nullptr, nullptr, nullptr, nullptr, &tasks);
    };
    REQUIRE(issue());
    (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(work.size() == 1);
    CHECK(work[0].buildTimeRemaining == Mag::fromInt(8));
    CHECK(roster.store.enhancements()[unit.index].empty());
    // C-253's retail bug: the mass line drains at the ENERGY cost — 80 over
    // 8 s at 10/s build rate is 10 mass and 10 energy per funded tick, so the
    // full 80/80 bank covers exactly the eight beats the install needs.
    economies[0].stored = {Mag::fromInt(80),Mag::fromInt(80)};
    (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    roster.store.orders()[unit.index].clear();
    CHECK(work.empty());
    CHECK(economies[0].stored.mass == Mag::fromInt(70));
    CHECK(roster.store.enhancements()[unit.index].empty());
    REQUIRE(issue());
    economies[0].stored = {Mag::fromInt(80),Mag::fromInt(80)};
    (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    const SaveState saved{.units=roster.store.snapshot(), .enhancements=work};
    const auto decoded = SaveState::decode(SaveState::encode(saved));
    REQUIRE(decoded);
    UnitStore restored(decoded->units);
    auto resumedWork = decoded->enhancements;
    auto resumedEconomies = economies;
    EnhancementTasks resumedTasks(restored, roster.catalog, resumedWork);
    Match resumed = match;
    resumed.economies = resumedEconomies;
    resumed.enhancements = &resumedWork;
    resumed.scriptTasks = &resumedTasks;
    for (int tick = 0; tick < 8; ++tick) {
        (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
        (void)tickSkirmish(restored, roster.catalog, resumed, terrain);
        CHECK(hashMatch(roster.store, match) == hashMatch(restored, resumed));
    }
    CHECK(work.empty());
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(roster.store.enhancements()[unit.index].at("LCH") == name);
    CHECK(roster.store.health()[unit.index].maximum == Mag::fromInt(120));
    CHECK(economies[0].stored.mass == Mag{});
    CHECK(economies[0].stored.energy == Mag{});
    roster.store.orders()[unit.index].clear();
}

TEST_CASE("enhancement removal uninstalls and restores health", "[enhancement][script-task]") {
    using namespace rm::sim;
    const auto field = flatField();
    const Terrain terrain{field};
    const auto grid = buildPassability(field, 0, 60, 0);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(100);
    const auto parameters = rm::lua::parseTable("{ NewBuildRate=30, NewHealth=20, NewRegenRate=2 }");
    REQUIRE(parameters);
    def.enhancements.push_back({.name="AdvancedEngineering", .slot="LCH",
        .buildCostMass=Mag::fromInt(8), .buildCostEnergy=Mag::fromInt(80),
        .buildTime=Fx::fromInt(8), .parameters=*parameters});
    def.enhancements.push_back({.name="AdvancedEngineeringRemove", .slot="LCH",
        .prerequisite="AdvancedEngineering",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1),
        .removes={"AdvancedEngineering", "AdvancedEngineeringRemove"}});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);
    auto armies = freeForAll(1);
    std::vector<Player> players{{.index=0,.army=0}};
    std::vector<Economy> economies(1);
    std::vector<EnhancementWork> work;
    EnhancementTasks tasks(roster.store, roster.catalog, work);
    const std::vector<const PassabilityGrid*> grids{&grid};
    Match match{.armies=armies, .economies=economies, .enhancements=&work,
        .passability=grids, .scriptTasks=&tasks, .baseStorage={Mag::fromInt(100),Mag::fromInt(1000)}};
    const auto issue = [&](std::string name) {
        return applyCommand(CommandIssue{.source=0,.id=roster.store.allocateCommandId(0).value(),
            .player=0,.kind=CommandKind::Script,.units={unit},.scriptTask="EnhanceTask",
            .scriptData={name.begin(),name.end()}}, roster.store, roster.catalog, players, armies,
            terrain, [&](UnitId) { return &grid; }, roster.rate, nullptr, nullptr, nullptr, nullptr, &tasks);
    };
    // Removing what was never installed is refused in-task, not at intake: the
    // prerequisite must occupy the slot (Unit.lua:1992-2003). The order aborts
    // on its first dispatch, creating no work and consuming a tick.
    REQUIRE(issue("AdvancedEngineeringRemove"));
    (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    CHECK(work.empty());
    CHECK(roster.store.orders()[unit.index].empty());

    REQUIRE(issue("AdvancedEngineering"));
    economies[0].stored = {Mag::fromInt(1000),Mag::fromInt(1000)};
    for (int tick = 0; tick < 12; ++tick) {
        (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    REQUIRE(issue("AdvancedEngineeringRemove"));
    economies[0].stored = {Mag::fromInt(1),Mag::fromInt(1)};
    (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    REQUIRE(issue("AdvancedEngineeringRemove"));
    economies[0].stored = {Mag::fromInt(1000),Mag::fromInt(1000)};
    for (int tick = 0; tick < 4; ++tick) {
        (void)tickSkirmish(roster.store, roster.catalog, match, terrain);
    }
    CHECK(roster.store.health()[unit.index].maximum == Mag::fromInt(100));
    CHECK(roster.store.health()[unit.index].current == Mag::fromInt(100));
    CHECK(work.empty());
}

TEST_CASE("the UI's enhancement replacement is the Remove-then-install pair", "[enhancement][script-task]") {
    // `C-251`/`C-378` (`construction.lua:947-981`): ordering an enhancement
    // whose slot is already occupied issues TWO `UNITCOMMAND_Script`
    // `EnhanceTask` commands — `<occupant>Remove` then the new id — each with
    // the clear-queue flag. An empty slot, or one holding the new id's own
    // prerequisite, takes the single command; re-ordering the installed id is
    // a no-op. The 0.5 s spacing between the pair is presentation; the sim
    // contract is the sequence, which is what this pins.
    using namespace rm::sim;
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(100);
    const auto parameters = rm::lua::parseTable("{}");
    REQUIRE(parameters);
    const auto spec = [&](std::string name, std::string slot,
                          std::string prerequisite = "",
                          std::vector<std::string> removes = {}) {
        return rm::unitdef::EnhancementSpec{
            .name = std::move(name), .slot = std::move(slot),
            .prerequisite = std::move(prerequisite),
            .buildCostMass = Mag::fromInt(1), .buildCostEnergy = Mag::fromInt(1),
            .buildTime = Fx::fromInt(1), .removes = std::move(removes),
            .parameters = *parameters};
    };
    // LCH chain: AdvancedEngineering -> T3Engineering; RCH: HeatSink alone.
    // ChronoDampener shares LCH with no prerequisite — the cross-chain
    // replacement case.
    def.enhancements.push_back(spec("AdvancedEngineering", "LCH"));
    def.enhancements.push_back(spec("T3Engineering", "LCH", "AdvancedEngineering"));
    def.enhancements.push_back(spec("ChronoDampener", "LCH"));
    def.enhancements.push_back(spec("AdvancedEngineeringRemove", "LCH",
                                    "AdvancedEngineering",
                                    {"AdvancedEngineering", "AdvancedEngineeringRemove"}));
    def.enhancements.push_back(spec("HeatSink", "RCH"));
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    // Empty slot: the single command.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "AdvancedEngineering")
          == std::vector<std::string>{"AdvancedEngineering"});
    // Unknown id: nothing to issue.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "NoSuchEnhancement").empty());

    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "AdvancedEngineering"));
    // Already installed: retail's click is a no-op.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "AdvancedEngineering").empty());
    // The chain's own next step replaces its prerequisite directly — one
    // command, no Remove.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "T3Engineering")
          == std::vector<std::string>{"T3Engineering"});
    // A different slot is unaffected by the LCH occupant.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "HeatSink")
          == std::vector<std::string>{"HeatSink"});
    // Occupied by something else: `<occupant>Remove` then the new id — the
    // two-command protocol, in order.
    CHECK(enhancementOrderSequence(roster.store, roster.catalog, unit,
                                   "ChronoDampener")
          == std::vector<std::string>{"AdvancedEngineeringRemove",
                                      "ChronoDampener"});
}

TEST_CASE("regen writes are last-writer-wins, incoherence included", "[enhancement][script-task]") {
    // `C-258`/`C-380`: retail lets `SetRegenRate` and the Regen-buff recompute
    // fight over one field, and parity means reproducing the incoherence. A
    // `SetRegenRate` enhancement (name-keyed: DamageStablization,
    // SelfRepairSystem, SystemIntegrityCompensator) supplies the WHOLE rate,
    // erasing active adds; its `XxxRemove` runs `RevertRegenRate`, which drops
    // the override AND the adds; and any later regen-touching install is a
    // buff event that recomputes base+adds, erasing the write state.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(1000);
    def.regenPerSecond = 10.0f;  // 1.0 per tick at 10 Hz
    const auto overrideParams = rm::lua::parseTable("{ NewRegenRate=200 }");
    const auto addParams = rm::lua::parseTable("{ NewRegenRate=4 }");
    REQUIRE(overrideParams);
    REQUIRE(addParams);
    const auto spec = [&](std::string name, std::string slot,
                          std::string prerequisite,
                          std::vector<std::string> removes,
                          const rm::lua::Value& parameters) {
        return rm::unitdef::EnhancementSpec{
            .name = std::move(name), .slot = std::move(slot),
            .prerequisite = std::move(prerequisite),
            .buildCostMass = Mag::fromInt(1), .buildCostEnergy = Mag::fromInt(1),
            .buildTime = Fx::fromInt(1), .removes = std::move(removes),
            .parameters = parameters};
    };
    // DamageStablization is the name-keyed `SetRegenRate` writer (retail's own
    // typo); AdvancedEngineering's NewRegenRate is a Regen buff ADD. Separate
    // slots: the write and the add coexist, which is exactly the incoherence —
    // neither knows the other touched the rate.
    def.enhancements.push_back(spec("DamageStablization", "RCH", "", {}, *overrideParams));
    const auto emptyParams = rm::lua::parseTable("{}");
    REQUIRE(emptyParams);
    def.enhancements.push_back(spec("DamageStablizationRemove", "RCH", "DamageStablization",
                                    {"DamageStablization", "DamageStablizationRemove"},
                                    *emptyParams));
    def.enhancements.push_back(spec("AdvancedEngineering", "LCH", "", {}, *addParams));
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 1000);
    auto& health = roster.store.health()[unit.index];
    health.current = Mag::fromInt(500);  // wounded, so regen is observable

    const auto healOnce = [&] {
        const Mag before = health.current;
        tickRegeneration(roster.store, roster.catalog, roster.rate);
        return health.current - before;
    };
    // Bare blueprint rate: 10/s -> 1.0/tick.
    CHECK(healOnce() == magFromFloat(1.0f));

    // The Regen ADD lands first: base 1.0 + 4/s (0.4/tick) = 1.4.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "AdvancedEngineering"));
    CHECK(health.regenWrite == Health::RegenWrite::None);
    CHECK(healOnce() == magFromFloat(1.4f));

    // The direct write erases the standing add: exactly 200/s = 20/tick, not
    // 20.4 — the incoherence, reproduced rather than fixed.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "DamageStablization"));
    CHECK(health.regenWrite == Health::RegenWrite::Overridden);
    CHECK(healOnce() == magFromFloat(20.0f));

    // `RevertRegenRate` erases the write AND the still-installed add: the unit
    // heals at the bare blueprint rate even though AdvancedEngineering's Regen
    // buff is still on it.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "DamageStablizationRemove"));
    CHECK(health.regenWrite == Health::RegenWrite::Reverted);
    CHECK(healOnce() == magFromFloat(1.0f));

    // And the symmetric half: the next regen-touching install is a buff event
    // that recomputes base+adds, resurrecting the erased add — the write state
    // never survives a recompute.
    roster.store.enhancements()[unit.index].clear();
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "AdvancedEngineering"));
    CHECK(health.regenWrite == Health::RegenWrite::None);
    CHECK(healOnce() == magFromFloat(1.4f));
}

TEST_CASE("an installed enhancement's maintenance drain bills the army", "[enhancement][script-task]") {
    // `C-255`: `Shield`/`CloakingGenerator` carry
    // `MaintenanceConsumptionPerSecondEnergy` — the script's
    // `SetEnergyMaintenanceConsumptionOverride` + `SetMaintenanceConsumptionActive`.
    // Installed, the drain sums into the army's upkeep demand; removed, it
    // stops. 250/s at 10 Hz is 25 a tick.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(100);
    const auto parameters = rm::lua::parseTable(
        "{ MaintenanceConsumptionPerSecondEnergy=250 }");
    REQUIRE(parameters);
    def.enhancements.push_back({.name="Shield", .slot="Back",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1), .parameters=*parameters});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    CHECK(enhancementMaintenancePerTick(roster.store, roster.catalog, unit.index)
          == Mag{});
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "Shield"));
    CHECK(enhancementMaintenancePerTick(roster.store, roster.catalog, unit.index)
          == magFromFloat(25.0f));
}

TEST_CASE("enhancement weapon labels gate firing until the branch enables them", "[enhancement][script-task]") {
    using namespace rm::sim;
    // `C-255`/`C-379`: `SetWeaponEnabledByLabel` — the label is not the
    // enhancement's name (`TacticalMissile` enables `TacMissile`), and a
    // sibling branch can disable it again (`TacticalNukeMissile` silences
    // `TacMissile` while arming `TacNukeMissile`).
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = Mag::fromInt(100);
    rm::unitdef::Weapon tac;
    tac.label = "TacMissile";
    tac.role = rm::unitdef::WeaponRole::DirectFire;
    tac.maxRange = Fx::fromInt(50);
    tac.rateOfFire = 1.0f;
    tac.damage = Mag::fromInt(10);
    tac.targetPriorities = {{"ALLUNITS"}};
    rm::unitdef::Weapon nuke = tac;
    nuke.label = "TacNukeMissile";
    def.weapons = {tac, nuke};
    def.enhancements.push_back({.name="TacticalMissile", .slot="RCH"});
    def.enhancements.push_back({.name="TacticalNukeMissile", .slot="RCH",
        .prerequisite="TacticalMissile"});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    // Neither label is enabled before any enhancement installs — and the
    // def-level `enabledByEnhancement` flag is false for both, so this gate is
    // the script table's, not the flag's.
    CHECK_FALSE(def.weapons[0].enabledByEnhancement);
    CHECK_FALSE(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                                     def.weapons[0]));
    CHECK_FALSE(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                                     def.weapons[1]));
    CHECK_FALSE(weaponFiresFor(roster.store, roster.catalog, unit.index,
                              def.weapons[0]));

    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "TacticalMissile"));
    CHECK(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                               def.weapons[0]));
    CHECK_FALSE(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                                     def.weapons[1]));
    CHECK(weaponFiresFor(roster.store, roster.catalog, unit.index, def.weapons[0]));

    // The nuke swap: TacMissile goes dark, TacNukeMissile arms.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "TacticalNukeMissile"));
    CHECK_FALSE(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                                     def.weapons[0]));
    CHECK(weaponEnabledForUnit(roster.store, roster.catalog, unit.index,
                               def.weapons[1]));

    // The gate reaches acquisition: an ungated weapon finds the hostile, the
    // gated one does not.
    rm::unitdef::UnitDef target;
    target.name = "target";
    target.health = Mag::fromInt(100);
    const auto enemy = roster.add(roster.addType(target), 60, 40, 1, 100);
    auto armies = freeForAll(2);
    CHECK(nearestTarget(rm::test::at(40, 0, 40), 0, unit.index, def.weapons[1],
                        roster.store, armies, nullptr, &roster.catalog) == enemy);
    CHECK_FALSE(nearestTarget(rm::test::at(40, 0, 40), 0, unit.index, def.weapons[0],
                              roster.store, armies, nullptr, &roster.catalog)
                    .has_value());
}

TEST_CASE("enhancement shield, intel, pod, and silo effects apply on install", "[enhancement][script-task]") {
    // `C-255`/`C-379`: the remaining `CreateEnhancement` branches —
    // `CreatePersonalShield`/`CreateShield`/`DestroyShield`,
    // `SetIntelRadius`/`EnableUnitIntel`, `CreateUnitHPR` pods, and the silo
    // swap's `RemoveTacticalSiloAmmo`/`StopSiloBuild` — are native effects,
    // not just cap edits.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = Mag::fromInt(100);
    def.visionRadiusElmos = 20.0f;
    def.omniRadiusElmos = 10.0f;
    const auto shieldParams = rm::lua::parseTable(
        "{ ShieldMaxHealth=500, ShieldRegenRate=10, ShieldRechargeTime=20, "
        "  ShieldRegenStartTime=5, ShieldSize=10, ShieldVerticalOffset=-1 }");
    const auto sensorParams = rm::lua::parseTable(
        "{ NewVisionRadius=80, NewOmniRadius=60, NewJammerRadius=40 }");
    REQUIRE(shieldParams);
    REQUIRE(sensorParams);
    const auto spec = [&](std::string name, std::string slot,
                          const rm::lua::Value& parameters) {
        return rm::unitdef::EnhancementSpec{
            .name = std::move(name), .slot = std::move(slot),
            .buildCostMass = Mag::fromInt(1), .buildCostEnergy = Mag::fromInt(1),
            .buildTime = Fx::fromInt(1), .parameters = parameters};
    };
    def.enhancements.push_back(spec("Shield", "Back", *shieldParams));
    def.enhancements.push_back({.name="ShieldRemove", .slot="Back",
        .prerequisite="Shield",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1),
        .removes={"Shield", "ShieldRemove"},
        .parameters=*rm::lua::parseTable("{}")});
    def.enhancements.push_back(spec("EnhancedSensors", "RCH", *sensorParams));
    def.enhancements.push_back(spec("RadarJammer", "LCH", *sensorParams));
    def.enhancements.push_back(spec("LeftPod", "Pod", *rm::lua::parseTable("{}")));
    def.enhancements.push_back({.name="LeftPodRemove", .slot="Pod",
        .prerequisite="LeftPod",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1),
        .removes={"LeftPod", "LeftPodRemove"},
        .parameters=*rm::lua::parseTable("{}")});
    def.enhancements.push_back(spec("TacticalNukeMissile", "Silo",
                                    *rm::lua::parseTable("{}")));
    // The pod's own blueprint — `CreateUnitHPR` spawns a real unit of it.
    rm::unitdef::UnitDef pod;
    pod.name = "UEA0001";
    pod.health = Mag::fromInt(50);
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);
    (void)roster.addType(pod);

    // --- Shield: `CreatePersonalShield` stands the bubble up charging. ---
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "Shield"));
    const auto& health = roster.store.health()[unit.index];
    CHECK(health.shield.maximum == Mag::fromInt(500));
    CHECK(health.shield.current == Mag{});          // ChargingUp: zero until charged
    CHECK(health.shield.rechargeRemaining == 201);  // 20 s at 10 Hz + faWaitSeconds's extra tick
    CHECK(shieldFor(roster.store, roster.catalog, unit.index).maximum
          == Mag::fromInt(500));
    // `ShieldRemove` runs `DestroyShield` — the bubble is gone even though the
    // enhancement record is what carried it.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "ShieldRemove"));
    CHECK(health.shield.maximum == Mag{});
    CHECK_FALSE(shieldFor(roster.store, roster.catalog, unit.index).exists());

    // --- Intel: `SetIntelRadius` overrides vision/omni/jammer. ---
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "EnhancedSensors"));
    const auto radii = intelRadiiFor(roster.store, roster.catalog, unit.index);
    // `NewVisionRadius`/`NewOmniRadius` are authored in ogrids — 8 elmos each.
    CHECK(radii.vision == Fx::fromInt(640));
    CHECK(radii.omni == Fx::fromInt(480));
    // `RadarJammer` adds `EnableUnitIntel('Jammer')` + its own radius.
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "RadarJammer"));
    const auto jammed = intelRadiiFor(roster.store, roster.catalog, unit.index);
    CHECK(jammed.jamRadius == Fx::fromInt(320));
    CHECK(roster.store.intelEnabled(unit, IntelType::Jammer));

    // --- Pods: `CreateUnitHPR` spawns and attaches; `XxxRemove` kills. ---
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "LeftPod"));
    const auto children = roster.store.childrenOf(unit);
    REQUIRE(children.size() == 1);
    CHECK(roster.catalog.def(roster.store.typeAt(children[0].index))->name
          == "UEA0001");
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "LeftPodRemove"));
    CHECK_FALSE(roster.store.alive(children[0]));

    // --- Silo: `TacticalNukeMissile` purges the owner's ammo records. ---
    std::vector<SiloAmmo> siloAmmo{
        {.owner = unit, .stored = 3, .elapsedTicks = 40},
        {.owner = UnitId{9999, 1}, .stored = 2, .elapsedTicks = 10}};
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "TacticalNukeMissile", siloAmmo));
    CHECK(siloAmmo[0].stored == 0);
    CHECK(siloAmmo[0].elapsedTicks == 0);
    CHECK(siloAmmo[1].stored == 2);  // another owner's record is untouched
}

TEST_CASE("enhancement stat mods override range, rate, and damage", "[enhancement][script-task]") {
    // `C-255`: `ChangeMaxRadius`/`ChangeRateOfFire` are absolute writes,
    // `AddDamageMod` adds — each keyed to the weapon LABEL the script names.
    using namespace rm::sim;
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = Mag::fromInt(100);
    rm::unitdef::Weapon gun;
    gun.label = "RightRipper";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.maxRange = Fx::fromInt(30);
    gun.rateOfFire = 1.0f;
    gun.damage = Mag::fromInt(10);
    rm::unitdef::Weapon disintegrator = gun;
    disintegrator.label = "RightDisintegrator";
    def.weapons = {gun, disintegrator};
    const auto params = rm::lua::parseTable(
        "{ NewMaxRadius=50, NewRateOfFire=2.0 }");
    const auto dmgParams = rm::lua::parseTable("{ NewDamageMod=5 }");
    REQUIRE(params);
    REQUIRE(dmgParams);
    def.enhancements.push_back({.name="CoolingUpgrade", .slot="RCH",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1), .parameters=*params});
    def.enhancements.push_back({.name="FocusConvertor", .slot="LCH",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1), .parameters=*dmgParams});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    // Before install: the authored values.
    CHECK(weaponMaxRangeFor(roster.store, roster.catalog, unit.index, gun)
          == Fx::fromInt(30));
    CHECK(weaponRateOfFireFor(roster.store, roster.catalog, unit.index, gun)
          == 1.0f);
    CHECK(weaponDamageModFor(roster.store, roster.catalog, unit.index, gun)
          == Mag{});

    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "CoolingUpgrade"));
    // `NewMaxRadius` is authored in ogrids — 50 ogrids = 400 elmos.
    CHECK(weaponMaxRangeFor(roster.store, roster.catalog, unit.index, gun)
          == Fx::fromInt(400));
    CHECK(weaponRateOfFireFor(roster.store, roster.catalog, unit.index, gun)
          == 2.0f);
    // `AddDamageMod` is `FocusConvertor`'s branch — and it lands on
    // `RightDisintegrator`, not `RightRipper`.
    CHECK(weaponDamageModFor(roster.store, roster.catalog, unit.index,
                             disintegrator) == Mag{});
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "FocusConvertor"));
    CHECK(weaponDamageModFor(roster.store, roster.catalog, unit.index,
                             disintegrator) == Mag::fromInt(5));
    CHECK(weaponDamageModFor(roster.store, roster.catalog, unit.index, gun)
          == Mag{});
}
