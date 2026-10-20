#include <catch2/catch_test_macros.hpp>

#include "core/sim/CommandQueue.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/vfs/Vfs.hpp"
#include "core/blueprint/BlueprintMesh.hpp"
#include "app/ScriptHost.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdint>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
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
    void onPreCreate(rm::sim::UnitId, std::string_view task,
                     std::span<const std::uint8_t>,
                     rm::sim::ScriptTaskState&) noexcept override {
        events.push_back("pre-create:" + std::string{task});
    }

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
                          rm::sim::ScriptTaskState&) override {
        events.push_back("tick:" + std::string{task});
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
        destroyedAiResult = state.aiResult;
    }

    std::deque<std::int32_t> results;
    std::vector<std::string> events;
    rm::sim::UnitId createdUnit{};
    std::vector<std::uint8_t> seenCommandData;
    std::vector<std::uint8_t> destroyedState;
    std::int32_t destroyedAiResult = 0;

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
TEST_CASE("C-252/C-454: a finished script task reports Success, an aborted one Fail") {
    // `C-252`: `EnhanceTask`'s `OnDestroy` reports `AIRESULT.Success/Fail`;
    // `C-454` names the values (Success=1, Fail=2, writer `SetAIResult`
    // `0x0062a040`). The native host owns the write: the dispatcher's
    // terminal outcome lands in the task state's `aiResult` cell.
    Fixture done;
    done.tasks.results = {
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done),
    };
    REQUIRE(done.issue("EnhanceTask"));
    CHECK(done.advance() == 1);
    CHECK(done.roster.store.orders()[done.unit.index].empty());
    CHECK(done.tasks.events.back() == "destroy:EnhanceTask");
    CHECK(done.tasks.destroyedAiResult == 1);

    Fixture failed;
    failed.tasks.results = {
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Abort),
    };
    REQUIRE(failed.issue("EnhanceTask"));
    CHECK(failed.advance() == 1);
    CHECK(failed.roster.store.orders()[failed.unit.index].empty());
    CHECK(failed.tasks.events.back() == "destroy:EnhanceTask");
    CHECK(failed.tasks.destroyedAiResult == 2);
}

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
    CHECK(sleeping.aiResult == 0);
    CHECK(sleeping.opaque == std::vector<std::uint8_t>{0xCA, 0xFE});
    CHECK(fixture.tasks.seenCommandData == std::vector<std::uint8_t>{1, 2, 3});

    CHECK(fixture.advance() == 0);
    REQUIRE(fixture.roster.store.orders()[fixture.unit.index].active() != nullptr);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].active()->scriptState().sleepBeats == 0);

    CHECK(fixture.advance() == 0);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].empty());
    CHECK(fixture.tasks.events
          == std::vector<std::string>{"pre-create:TargetLocation", "create:TargetLocation",
                                      "tick:TargetLocation", "tick:TargetLocation",
                                      "tick:TargetLocation",
                                      "destroy:TargetLocation"});
    CHECK(fixture.tasks.destroyedState == std::vector<std::uint8_t>{0xCA, 0xFE});
}

TEST_CASE("C-478: script-task Delay normalizes to Suspend inside dispatch") {
    // `C-478` corrects `C-451`'s arm labeling: the true -4 Delay arm
    // (`0x0040934c`[0]) just returns -2 — Delay normalizes to Suspend inside
    // TaskTick and the Tick driver performs the single suspend-ring insert.
    // `C-306`'s "requeue at far end of current pass" stays refuted: a Delay
    // parks the task suspended with NO end-of-beat re-tick, exactly like a
    // Suspend, and only an explicit resume lets it run again.
    Fixture fixture;
    fixture.tasks.results = {
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Delay),
        static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Abort),
    };

    REQUIRE(fixture.issue("EnhanceTask"));
    CHECK(fixture.advance() == 1);
    rm::sim::QueuedCommand* active =
        fixture.roster.store.orders()[fixture.unit.index].activeMutable();
    REQUIRE(active != nullptr);
    CHECK(active->scriptState().suspended);
    CHECK(fixture.tasks.events.size() == 3); // pre-create, create, one tick — no end-of-beat re-tick

    active->scriptState().suspended = false;
    CHECK(fixture.advance() == 0);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].empty());
    CHECK(fixture.tasks.events.back() == "destroy:EnhanceTask");
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
    CHECK(fixture.tasks.events.size() == 3); // pre-create, create, one tick — C-478: Delay parks, no end-of-beat re-tick

    active->scriptState().suspended = false;
    CHECK(fixture.advance() == 0);
    CHECK(fixture.roster.store.orders()[fixture.unit.index].active() != nullptr);
    CHECK(fixture.tasks.events.back() == "tick:EnhanceTask"); // resumed: Suspend result parks again
    fixture.roster.store.orders()[fixture.unit.index].activeMutable()->scriptState().suspended = false;
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

TEST_CASE("C-228: the Lua-backed ScriptTaskHost loads a fake task and fires each lifecycle hook once") {
    // `C-228` PARTIAL: the sim's `ScriptTaskHost` port (`src/core/sim/ScriptTask.hpp`) is the
    // seam at which a real Lua runtime must drive `OnCreate` / `TaskTick(result)` /
    // `OnDestroy`. `app::ScriptHost` (`src/app/ScriptHost.cpp`) is a Lua-backed implementation
    // for it; this case stands up a VFS, registers a one-task fixture under the same `/lua/`
    // prefix retail uses, and drives the full `applyCommand` + `advanceOrders` pipeline. The
    // hooks themselves are the task's own Lua side: a `TypeClass` whose `OnCreate` and
    // `OnDestroy` push counters onto per-instance tables, and whose `TaskTick(result)`
    // returns `Done` so the dispatcher retires the queue and `OnDestroy` fires immediately.
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "scriptable";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    // Side-channel counters: the host puts the source on the live Lua `_G`, so a per-class
    // counter recorded by the test's own source gives the exactly-once contract.
    host.registerTask("C228Probe", rm::app::ScriptTaskRegistration{
        .source =
            "local log = {pre = 0, create = 0, tick = 0, destroy = 0}\n"
            "local probe = {}\n"
            "function probe:OnCreate()\n"
            "  log.create = log.create + 1\n"
            "end\n"
            "function probe:TaskTick(result)\n"
            "  log.tick = log.tick + 1\n"
            "  log.lastResult = result\n"
            "  return -1  -- ScriptTaskStatus::Done, retires the queue.\n"
            "end\n"
            "function probe:OnDestroy()\n"
            "  log.destroy = log.destroy + 1\n"
            "end\n"
            "TypeClass = Class(probe) {\n"
            "  OnCreate = probe.OnCreate,\n"
            "  TaskTick = probe.TaskTick,\n"
            "  OnDestroy = probe.OnDestroy,\n"
            "}\n"
            "G_c228log = log\n",
    });

    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};

    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C228Probe"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    // First advance: dispatcher fires `onPreCreate` (engine-side hook) then `onCreate`
    // (which constructs the Lua instance and runs OnPreCreate/OnCreate in order), then
    // TaskTick returns Done so the queue retires.
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    // Second advance closes the cycle and `onDestroy` fires during `eraseAt`.
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                 nullptr, &host) == 0);
    CHECK(roster.store.orders()[unit.index].empty());
    std::string joined;
    for (const auto& err : host.errors()) {
        if (!joined.empty()) joined += " | ";
        joined += err;
    }
    WARN("host errors: " << joined);
    REQUIRE(joined.empty());
}

TEST_CASE("C-318: a registered ScriptModule/ScriptClass fires OnPreCreate, OnCreate, OnDestroy in order") {
    // `C-318` PARTIAL: the blueprint→script binding the retail engine resolves with
    // `ScriptModule`/`ScriptClass` (`src/core/blueprint/BlueprintMesh.cpp:79-101`,
    // `scriptBindingFor`) extends to the `OnPreCreate`/`OnCreate`/`OnDestroy` lifecycle.
    // The host's `OnPreCreate` is the engine-side hoisted pre-setup hook; `OnCreate` is
    // the post-setup hook; both reach Lua exactly once, in that order, with `OnDestroy`
    // at queue retirement. The fixture ships a real `<bpid>_script.lua` file, instantiates
    // its `TypeClass`, walks the host through create/tick/destroy, and reads back what each
    // hook recorded.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c318";
    std::filesystem::create_directories(scratch / "units/c318");
    const auto scriptPath = scratch / "units/c318/c318_script.lua";
    {
        std::ofstream file(scriptPath);
        file <<
            "local m = {calls = {}}\n"
            "function m:OnPreCreate()\n"
            "  table.insert(self.calls, 'pre')\n"
            "end\n"
            "function m:OnCreate()\n"
            "  table.insert(self.calls, 'create')\n"
            "end\n"
            "function m:TaskTick(result)\n"
            "  table.insert(self.calls, 'tick:' .. tostring(result))\n"
            "  return -1  -- Done; clears the queue so OnDestroy fires.\n"
            "end\n"
            "function m:OnDestroy()\n"
            "  table.insert(self.calls, 'destroy')\n"
            "end\n"
            "TypeClass = Class(m) {\n"
            "  OnPreCreate = m.OnPreCreate,\n"
            "  OnCreate = m.OnCreate,\n"
            "  TaskTick = m.TaskTick,\n"
            "  OnDestroy = m.OnDestroy,\n"
            "}\n"
            "G_c318calls = m.calls\n";
    }

    rm::vfs::Vfs content;
    content.mountDirectory(scratch);
    // Bind the same way retail does: the blueprint's `scriptBindingFor` picks the
    // `<bpid>_script.lua` by suffix convention; the host is told the name the engine
    // will dispatch under.
    const rm::blueprint::ScriptBinding binding = rm::blueprint::scriptBindingFor(
        "/units/c318/c318_unit.bp", "", "", "/lua/sim/unit.lua", "Unit");
    CHECK(binding.module == "/units/c318/c318_script.lua");
    CHECK(binding.className == "TypeClass");

    rm::app::ScriptHost host{content};
    host.registerTask("C318Unit", rm::app::ScriptTaskRegistration{
        .modulePath = "/units/c318/c318_script.lua",
    });

    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c318_unit";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C318Unit"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                 nullptr, &host) == 0);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());

    std::filesystem::remove_all(scratch);
}

TEST_CASE("C-318: ScriptHost runs a shipped-shape state task through ChangeState") {
    // `C-318` state-machine half (`mohodata/lua/system/class.lua:327`): shipped
    // tasks are state machines — `EnhanceTask` opens in `Stopping` and reaches
    // `Enhancing` through `ChangeState(self, self.Enhancing)`. The host used to
    // alias `State = Class` with no `ChangeState` at all, so a task in the
    // shipped shape died inside `OnCreate` and the queue retired at once.
    // `ChangeState` must switch dispatch to the new state while instance fields
    // survive the switch: the `Stopping` beat arms the instance and waits, and
    // only the `Enhancing` beat — reading that same field — may retire the
    // queue. Either half broken leaves the queue standing after two beats.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C318States", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  OnCreate = function(self) ChangeState(self, self.Stopping) end,\n"
            "  Stopping = State {\n"
            "    TaskTick = function(self)\n"
            "      self.armed = true\n"
            "      ChangeState(self, self.Enhancing)\n"
            "      return 1\n"
            "    end,\n"
            "  },\n"
            "  Enhancing = State {\n"
            "    TaskTick = function(self)\n"
            "      if self.armed ~= true then return 1 end\n"
            "      return -1\n"
            "    end,\n"
            "  },\n"
            "}\n",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c318_states";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C318States"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].active() != nullptr);
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                 nullptr, &host) == 0);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());
}

TEST_CASE("C-311: ScriptHost concatenates the base task with its hook copies") {
    // `C-311` (`SCR_LuaDoFileConcat`, `0x004d4de0`): every load collects the
    // base file plus every registered hookdir's copy of the same path and
    // `lua_load`s them as ONE chunk — a hook shares the original's locals and
    // environment. The sim task host must do the same when its modulePath
    // resolves through the VFS, in hook registration order.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c311";
    std::filesystem::create_directories(scratch / "lua/sim/tasks");
    std::filesystem::create_directories(scratch / "schook/lua/sim/tasks");
    {
        std::ofstream base(scratch / "lua/sim/tasks/C311Probe.lua");
        base << "local seen = {}\n"
                "function seen:TaskTick(result)\n"
                "  return 1\n"
                "end\n"
                "TypeClass = Class(seen) {\n"
                "  TaskTick = seen.TaskTick,\n"
                "}\n";
    }
    {
        std::ofstream hook(scratch / "schook/lua/sim/tasks/C311Probe.lua");
        hook << "TypeClass.TaskTick = function(self, result) return -1 end\n";
    }

    rm::vfs::Vfs content;
    content.mountDirectory(scratch);
    content.addHookDirectory("/schook");
    rm::app::ScriptHost host{content};
    host.registerTask("C311Probe", rm::app::ScriptTaskRegistration{
        .modulePath = "/lua/sim/tasks/C311Probe.lua",
    });

    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c311_probe";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C311Probe"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());

    std::filesystem::remove_all(scratch);
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
TEST_CASE("the RegenAura skips allies outside retail's tier filter", "[enhancement][script-task]") {
    // `XSL0001_script.lua:259` (`RegenBuffThread`): the aura queries
    // `BUILTBYTIER3FACTORY + BUILTBYQUANTUMGATE + NEEDMOBILEBUILD` — a T1 tank
    // (built by a T1 factory: `UAL0201_unit.bp` carries `BUILTBYTIER1FACTORY`,
    // not the T3 tag) is in radius but ineligible, while a T3-tagged peer is
    // lent. The sim's current same-army-mobile check heals both.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef sera;
    sera.name = "test_sera_acu";
    sera.health = Mag::fromInt(1000);
    sera.categories = {"COMMAND", "MOBILE"};
    sera.speedElmosPerSecond = 20.0f;
    const auto auraParams = rm::lua::parseTable(
        "{ Radius=15, RegenPerSecond=0.0055555555, RegenFloor=0, RegenCeiling=75 }");
    REQUIRE(auraParams);
    sera.enhancements.push_back({.name = "RegenAura", .slot = "RCH",
        .buildCostMass = Mag::fromInt(1), .buildCostEnergy = Mag::fromInt(1),
        .buildTime = Fx::fromInt(1), .parameters = *auraParams});
    rm::unitdef::UnitDef t1;
    t1.name = "test_t1_tank";
    t1.health = Mag::fromInt(100);
    // NB: sorted — `hasCategory` binary-searches (`UnitBlueprint.cpp:877`
    // sorts real blueprints; the hand-built def sorts by hand).
    t1.categories = {"BUILTBYTIER1FACTORY", "LAND", "MOBILE"};
    t1.speedElmosPerSecond = 20.0f;
    rm::unitdef::UnitDef t3;
    t3.name = "test_t3_bot";
    t3.health = Mag::fromInt(100);
    t3.categories = {"BUILTBYTIER3FACTORY", "LAND", "MOBILE"};
    t3.speedElmosPerSecond = 20.0f;
    const auto seraUnit = roster.add(roster.addType(sera), 0, 0, 0, 1000);
    const auto t1Unit = roster.add(roster.addType(t1), 10, 0, 0, 100);
    const auto t3Unit = roster.add(roster.addType(t3), 20, 0, 0, 100);
    roster.store.health()[t1Unit.index].current = Mag::fromInt(50);
    roster.store.health()[t3Unit.index].current = Mag::fromInt(50);
    REQUIRE(installEnhancement(roster.store, roster.catalog, seraUnit, "RegenAura"));
    tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(roster.store.health()[t1Unit.index].current == Mag::fromInt(50));
    CHECK(roster.store.health()[t3Unit.index].current > Mag::fromInt(50));
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
TEST_CASE("an installed enhancement's production income feeds the army", "[enhancement][script-task]") {
    // `C-255`: `ResourceAllocation` carries `ProductionPerSecondMass/Energy` —
    // the script's `SetProductionPerSecond*(bp + base)`. Installed, the
    // enhancement's fields add to the unit's own output per tick; before
    // install there is nothing. UEL0001 bp: 14 mass/s, 3300 energy/s, i.e.
    // 1.4 mass and 330 energy a tick at 10 Hz.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.buildRate = 10;
    def.health = Mag::fromInt(100);
    const auto parameters = rm::lua::parseTable(
        "{ ProductionPerSecondMass=14, ProductionPerSecondEnergy=3300 }");
    REQUIRE(parameters);
    def.enhancements.push_back({.name="ResourceAllocation", .slot="RCH",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1), .parameters=*parameters});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    CHECK(enhancementProductionPerTick(roster.store, roster.catalog, unit.index).mass
          == Mag{});
    CHECK(enhancementProductionPerTick(roster.store, roster.catalog, unit.index).energy
          == Mag{});
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "ResourceAllocation"));
    CHECK(enhancementProductionPerTick(roster.store, roster.catalog, unit.index).mass
          == magFromFloat(1.4f));
    CHECK(enhancementProductionPerTick(roster.store, roster.catalog, unit.index).energy
          == magFromFloat(330.0f));
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
TEST_CASE("ShieldHeavy install grants the shield toggle cap", "[enhancement][script-task]") {
    // `C-255`: `UAL0001_script.lua`'s `ShieldHeavy` branch runs
    // `AddToggleCap('RULEUTC_ShieldToggle')` before forking the heavy-shield
    // thread; the table modeled only the bubble, so the toggle never arrived.
    using namespace rm::sim;
    rm::test::Roster roster;
    roster.rate = TickRate{10};
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.health = Mag::fromInt(100);
    def.enhancements.push_back({.name="ShieldHeavy", .slot="Back",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1),
        .parameters=*rm::lua::parseTable("{}")});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);
    REQUIRE_FALSE(unitHasToggleCap(roster.store, roster.catalog, unit.index,
                                   "RULEUTC_ShieldToggle"));
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "ShieldHeavy"));
    CHECK(unitHasToggleCap(roster.store, roster.catalog, unit.index,
                           "RULEUTC_ShieldToggle"));
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
    // Retail's `StopSiloBuild` (UEL0001_script.lua:447) also drops the owner's
    // queued builds — the half-paid missile bill dies with the swap.
    std::vector<SiloBuild> siloQueue{
        {.owner = unit, .slot = 0},
        {.owner = UnitId{9999, 1}, .slot = 0}};
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "TacticalNukeMissile", siloAmmo, &siloQueue));
    CHECK(siloAmmo[0].stored == 0);
    CHECK(siloAmmo[0].elapsedTicks == 0);
    CHECK(siloAmmo[1].stored == 2);  // another owner's record is untouched
    CHECK(siloQueue.size() == 1);
    CHECK(siloQueue[0].owner == UnitId{9999, 1});
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
          == Fx::fromInt(1));
    CHECK(weaponDamageModFor(roster.store, roster.catalog, unit.index, gun)
          == Mag{});

    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "CoolingUpgrade"));
    // `NewMaxRadius` is authored in ogrids — 50 ogrids = 400 elmos.
    CHECK(weaponMaxRangeFor(roster.store, roster.catalog, unit.index, gun)
          == Fx::fromInt(400));
    CHECK(weaponRateOfFireFor(roster.store, roster.catalog, unit.index, gun)
          == Fx::fromInt(2));
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
TEST_CASE("EMPCharge re-enables the disintegrator's STUN buff", "[enhancement][script-task]") {
    // `C-255`: `URL0301_script.lua:270-275` — the stock `RightDisintegrator`
    // `OnCreate` runs `DisableBuff('STUN')` on its own weapon; installing
    // `EMPCharge` runs `ReEnableBuff('STUN')`, its Remove `DisableBuff`s again
    // (`weapon.lua:442-467`: the `Disabledbf` list filters the blueprint's
    // `Buffs` out of the damage table). The sim models the registry
    // per-weapon: the authored STUN buff lands disabled, and only the
    // enhancement flips it back on.
    using namespace rm::sim;
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_cybran_acu";
    def.health = Mag::fromInt(100);
    rm::unitdef::Weapon disintegrator;
    disintegrator.label = "RightDisintegrator";
    disintegrator.role = rm::unitdef::WeaponRole::DirectFire;
    disintegrator.maxRange = Fx::fromInt(200);
    disintegrator.rateOfFire = 1.0f;
    disintegrator.damage = Mag::fromInt(100);
    disintegrator.stunBuffed = true;
    def.weapons = {disintegrator};
    def.enhancements.push_back({.name="EMPCharge", .slot="Torso"});
    def.enhancements.push_back({.name="EMPChargeRemove", .slot="Torso",
        .prerequisite="EMPCharge", .removes={"EMPCharge", "EMPChargeRemove"}});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);

    // Stock: the authored STUN buff starts disabled by the script's OnCreate.
    CHECK_FALSE(weaponStunEnabledFor(roster.store, roster.catalog, unit.index,
                                     def.weapons[0]));
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "EMPCharge"));
    CHECK(weaponStunEnabledFor(roster.store, roster.catalog, unit.index,
                               def.weapons[0]));
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit, "EMPChargeRemove"));
    CHECK_FALSE(weaponStunEnabledFor(roster.store, roster.catalog, unit.index,
                                     def.weapons[0]));
}
TEST_CASE("CoolingUpgrade retunes only RightRipper's rate, not the MLG's", "[enhancement][script-task]") {
    // Retail `URL0001_script.lua:440-448`: the install `ChangeRateOfFire`s
    // only `RightRipper` (the MLG and OverCharge get `ChangeMaxRadius`
    // alone); the Remove restores rate on `RightRipper` only. An installed
    // CoolingUpgrade must leave an MLG-labelled weapon's authored rate alone.
    using namespace rm::sim;
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "test_cybran_acu";
    def.health = Mag::fromInt(100);
    rm::unitdef::Weapon mlg;
    mlg.label = "MLG";
    mlg.role = rm::unitdef::WeaponRole::DirectFire;
    mlg.maxRange = Fx::fromInt(22);
    mlg.rateOfFire = 3.0f;
    def.weapons = {mlg};
    const auto params = rm::lua::parseTable(
        "{ NewMaxRadius=30, NewRateOfFire=2.0 }");
    REQUIRE(params);
    def.enhancements.push_back({.name="CoolingUpgrade", .slot="RCH",
        .buildCostMass=Mag::fromInt(1), .buildCostEnergy=Mag::fromInt(1),
        .buildTime=Fx::fromInt(1), .parameters=*params});
    const auto unit = roster.add(roster.addType(def), 40, 40, 0, 100);
    REQUIRE(installEnhancement(roster.store, roster.catalog, unit,
                               "CoolingUpgrade"));
    CHECK(weaponRateOfFireFor(roster.store, roster.catalog, unit.index, mlg)
          == fxFromFloat(3.0f));
}
TEST_CASE("C-388: ScriptHost resolves TaskName by sprintf path with ScriptTask.lua fallback") {
    // `C-388` (`0x00629530`): the loader reads `TaskName`, sprintfs
    // `/lua/sim/tasks/%s.lua`, and on a miss logs `Can't find task %s, using
    // ScriptTask directly` then loads `/lua/sim/ScriptTask.lua`. The host must
    // resolve an unregistered task name through the VFS the same way, with the
    // same `C-311` hook concat on both branches.
    const auto runTask = [](rm::vfs::Vfs& content, std::string_view task) {
        rm::app::ScriptHost host{content};
        const auto field = flatField();
        const rm::sim::Terrain terrain{field};
        const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
        rm::test::Roster roster;
        rm::unitdef::UnitDef def;
        def.name = "c388_probe";
        const auto type = roster.addType(def);
        const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
        auto armies = rm::sim::freeForAll(1);
        std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
        REQUIRE(rm::sim::applyCommand(
            rm::sim::CommandIssue{.source = 0,
                                  .id = roster.store.allocateCommandId(0).value(),
                                  .player = 0,
                                  .kind = rm::sim::CommandKind::Script,
                                  .units = {unit},
                                  .scriptTask = std::string{task}},
            roster.store, roster.catalog, players, armies, terrain,
            [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
            nullptr, &host));
        const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
        REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                       nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                       nullptr, &host) == 1);
        CHECK(roster.store.orders()[unit.index].empty());
        return host.errors().empty();
    };
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c388";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "lua/sim/tasks");
    {
        std::ofstream direct(scratch / "lua/sim/tasks/C388Named.lua");
        direct << "TypeClass = Class({}) {\n"
                  "  TaskTick = function(self, result) return -1 end,\n"
                  "}\n";
    }
    {
        rm::vfs::Vfs content;
        content.mountDirectory(scratch);
        CHECK(runTask(content, "C388Named"));
    }
    std::filesystem::remove(scratch / "lua/sim/tasks/C388Named.lua");
    {
        // `C-388` fallback is the sim base `/lua/sim/ScriptTask.lua`.
        std::ofstream fallback(scratch / "lua/sim/ScriptTask.lua");
        fallback << "TypeClass = Class({}) {\n"
                    "  TaskTick = function(self, result) return -1 end,\n"
                    "}\n";
    }
    {
        rm::vfs::Vfs content;
        content.mountDirectory(scratch);
        CHECK(runTask(content, "C388Missing"));
    }
    std::filesystem::remove_all(scratch);
}

TEST_CASE("C-388: the ScriptTask.lua fallback resolves at /lua/sim/ScriptTask.lua") {
    // `C-388` (`0x00629530`, `.rdata 0x00e705a0`): the miss branch loads
    // `/lua/sim/ScriptTask.lua` — the sim base, the same path the corpus
    // carries (`mohodata/lua/sim/ScriptTask.lua`) and the same path
    // `/lua/sim/tasks/EnhanceTask.lua` imports. The host's fallback must
    // resolve there, not under `/lua/sim/tasks/`.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c388-base-path";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "lua/sim");
    {
        std::ofstream fallback(scratch / "lua/sim/ScriptTask.lua");
        fallback << "TypeClass = Class({}) {\n"
                    "  TaskTick = function(self, result) return -1 end,\n"
                    "}\n";
    }
    rm::vfs::Vfs content;
    content.mountDirectory(scratch);
    rm::app::ScriptHost host{content};
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c388_base_path";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C388NoSuchTask"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());
}
TEST_CASE("C-228: ScriptHost binds GetUnit/SetAIResult so the shipped EnhanceTask runs") {
    // `C-228` (`/lua/sim/tasks/EnhanceTask.lua:11-80`): the shipped task's first
    // two beats touch ONLY four natives — `self:GetUnit():SetWorkProgress(0)`,
    // `:SetUnitState('Enhancing'/'Upgrading',true)`, `IsMobile`+`IsMoving` on
    // the `Stopping` beat, and `:OnWorkBegin(Enhancement)` before `Repeat`
    // (`ChangeState` → `Stopping`). The host currently binds no task natives,
    // so the real file dies in `OnCreate` with `attempt to call a nil value
    // (method 'GetUnit')` and the queue retires at once. This case runs the
    // shipped-syntax task through `ensureInstance` + `taskTick` and requires
    // the `Stopping` beat to admit an immobile unit — the task's own `Repeat`
    // result proves the natives ran; a missing binding retires the queue Done.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("EnhanceProbe", rm::app::ScriptTaskRegistration{
        .source =
            "TASKSTATUS = { Done = -1, Suspend = -2, Abort = -3, Delay = -4,"
            " Repeat = 0, Wait = 1 }\n"
            "AIRESULT = { Unknown = 0, Success = 1, Fail = 2, Ignored = 3 }\n"
            "EnhanceTask = Class({}) {\n"
            "  OnCreate = function(self, commandData)\n"
            "    self.CommandData = commandData\n"
            "    self:GetUnit():SetWorkProgress(0.0)\n"
            "    self:GetUnit():SetUnitState('Enhancing', true)\n"
            "    self:GetUnit():SetUnitState('Upgrading', true)\n"
            "    self.LastProgress = 0\n"
            "    ChangeState(self, self.Stopping)\n"
            "  end,\n"
            "  Stopping = State {\n"
            "    TaskTick = function(self)\n"
            "      local unit = self:GetUnit()\n"
            "      if unit:IsMobile() and unit:IsMoving() then\n"
            "        unit:GetNavigator():AbortMove()\n"
            "        return TASKSTATUS.Wait\n"
            "      else\n"
            "        unit:OnWorkBegin(self.CommandData.Enhancement)\n"
            "        ChangeState(self, self.Enhancing)\n"
            "        return TASKSTATUS.Repeat\n"
            "      end\n"
            "    end,\n"
            "  },\n"
            "  Enhancing = State {\n"
            "    TaskTick = function(self) return 1 end,\n"
            "  },\n"
            "}\n"
            "TypeClass = EnhanceTask\n",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "enhance_probe";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    const std::vector<std::uint8_t> data{static_cast<std::uint8_t>('E')};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "EnhanceProbe",
                              .scriptData = data},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    // `OnCreate` must survive the natives (a nil `GetUnit` retires the queue
    // Done here instead); the `Stopping` beat then returns `Repeat` (0), which
    // the dispatcher loops into the `Enhancing` beat's `Wait` (1) on the same
    // advance. Waiting — not empty — proves the shipped shape ran.
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                 nullptr, &host) == 1);
    CHECK_FALSE(roster.store.orders()[unit.index].empty());
    CHECK(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                 nullptr, &host) == 0);
    for (const auto& err : host.errors()) WARN("lua: " << err);
    CHECK(host.errors().empty());
}


TEST_CASE("C-313: ScriptHost publishes __active_mods and hook-concats mod task copies") {
    // `C-313` (`0x0052fece`): `__active_mods` is built natively from
    // `gameInfo.GameMods`, and `C-311`'s concat walks base -> `/schook` ->
    // `__active_mods` order. The sim task host currently honours only the
    // `/schook` layer: a mod's `hookdir` copy of the task file never
    // concatenates, and `_G.__active_mods` stays nil inside the task chunk.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c313";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "lua/sim/tasks");
    std::filesystem::create_directories(scratch / "ChessMod/hook/lua/sim/tasks");
    // The mod hook appends a marker global the base file never sets; the base
    // chunk reads `__active_mods` into `G_c313mods`. Both must be visible
    // after dispatch: hook concat (`C-311`) + published table (`C-313`). A
    // hook that fails to concatenate (or a table left nil) leaves the queue
    // sleeping instead of retired.
    {
        std::ofstream base(scratch / "lua/sim/tasks/C313Probe.lua");
        base << "TypeClass = Class({}) {\n"
                "  TaskTick = function(self, result)\n"
                "    if G_c313hooked ~= true then return 1 end\n"
                "    if G_c313mods == nil then return 1 end\n"
                "    if G_c313mods[1] == nil then return 1 end\n"
                "    if G_c313mods[1].uid ~= 'chess' then return 1 end\n"
                "    return -1\n"
                "  end,\n"
                "}\n"
                "G_c313mods = __active_mods\n";
    }
    {
        std::ofstream hook(scratch / "ChessMod/hook/lua/sim/tasks/C313Probe.lua");
        hook << "G_c313hooked = true\n";
    }

    // The stock tree mounts at `/`; the mod mounts from its own directory
    // (`C-268`), so its files answer under `/mods/ChessMod/...` and its
    // hookdir joins the concat. `mountDirectory` walks the whole scratch
    // tree, so the mod's hook file is staged OUTSIDE the mounted root under
    // a sibling directory the stock walk never sees.
    std::filesystem::create_directories(scratch / "stock/lua/sim/tasks");
    std::filesystem::rename(scratch / "lua/sim/tasks/C313Probe.lua",
                            scratch / "stock/lua/sim/tasks/C313Probe.lua");
    rm::vfs::Vfs content;
    content.mountDirectory(scratch / "stock");
    rm::vfs::ActiveMod mod;
    mod.uid = "chess";
    mod.name = "ChessMod";
    mod.location = scratch / "ChessMod";
    REQUIRE(content.mountMod(mod));
    rm::app::ScriptHost host{content};
    host.setActiveMods({mod});
    host.registerTask("C313Probe", rm::app::ScriptTaskRegistration{
        .modulePath = "/lua/sim/tasks/C313Probe.lua",
    });

    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c313_probe";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C313Probe"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());

    std::filesystem::remove_all(scratch);
}

TEST_CASE("C-311: ScriptHost import and doscript concatenate hook copies") {
    // `C-311` (`SCR_LuaDoFileConcat`, `0x004d4de0`): EVERY load is hook-aware,
    // not just the task's own modulePath — `import` and `doscript` collect the
    // base file plus each registered hookdir's copy of the same path and run
    // them as ONE chunk, so a hook shares the original's locals. The helper's
    // hook rewrites the module-local answer; the doscript hook rewrites the
    // global. A host that reads only base files leaves the queue sleeping.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c311-import";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "lua/sim/tasks");
    std::filesystem::create_directories(scratch / "schook/lua/sim/tasks");
    {
        std::ofstream helper(scratch / "lua/sim/tasks/C311Helper.lua");
        helper << "answer = 1\n";
    }
    {
        std::ofstream helperHook(scratch / "schook/lua/sim/tasks/C311Helper.lua");
        helperHook << "answer = 42\n";
    }
    {
        std::ofstream script(scratch / "lua/sim/tasks/C311Script.lua");
        script << "G_c311script = 'base'\n";
    }
    {
        std::ofstream scriptHook(scratch / "schook/lua/sim/tasks/C311Script.lua");
        scriptHook << "G_c311script = 'hooked'\n";
    }
    {
        std::ofstream task(scratch / "lua/sim/tasks/C311Use.lua");
        task << "local helper = import('/lua/sim/tasks/C311Helper.lua')\n"
                "doscript('/lua/sim/tasks/C311Script.lua')\n"
                "TypeClass = Class({}) {\n"
                "  TaskTick = function(self, result)\n"
                "    if helper.answer ~= 42 then return 1 end\n"
                "    if G_c311script ~= 'hooked' then return 1 end\n"
                "    return -1\n"
                "  end,\n"
                "}\n";
    }

    rm::vfs::Vfs content;
    content.mountDirectory(scratch);
    content.addHookDirectory("/schook");
    rm::app::ScriptHost host{content};
    host.registerTask("C311Use", rm::app::ScriptTaskRegistration{
        .modulePath = "/lua/sim/tasks/C311Use.lua",
    });

    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c311_use";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C311Use"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());

    std::filesystem::remove_all(scratch);
}
TEST_CASE("C-313: ScriptHost keeps ui_only mods out of the sim session") {
    // `C-313` (`lua/mods.lua`): the sim session carries only sim mods —
    // `ui_only` entries are appended separately in the user session. A UI
    // mod's hook copy must not concatenate into a sim task chunk, and its
    // entry must not appear in the sim's `__active_mods`.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-c313-uionly";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "stock/lua/sim/tasks");
    std::filesystem::create_directories(scratch / "UiMod/hook/lua/sim/tasks");
    {
        std::ofstream base(scratch / "stock/lua/sim/tasks/C313Ui.lua");
        base << "G_c313uimods = __active_mods\n"
                "TypeClass = Class({}) {\n"
                "  TaskTick = function(self, result)\n"
                "    if G_c313uimods == nil then return -1 end\n"
                "    if G_c313uimods[1] ~= nil then return 1 end\n"
                "    if G_c313uihooked == true then return 1 end\n"
                "    return -1\n"
                "  end,\n"
                "}\n";
    }
    {
        std::ofstream hook(scratch / "UiMod/hook/lua/sim/tasks/C313Ui.lua");
        hook << "G_c313uihooked = true\n";
    }
    rm::vfs::Vfs content;
    content.mountDirectory(scratch / "stock");
    rm::vfs::ActiveMod mod;
    mod.uid = "uimod";
    mod.name = "UiMod";
    mod.location = scratch / "UiMod";
    mod.uiOnly = true;
    REQUIRE(content.mountMod(mod));
    // A caller-side UI mod mount must not join the sim hook concat.
    CHECK(content.hooksFor("/lua/sim/tasks/C313Ui.lua").empty());
    rm::app::ScriptHost host{content};
    host.setActiveMods({mod});
    host.registerTask("C313Ui", rm::app::ScriptTaskRegistration{
        .modulePath = "/lua/sim/tasks/C313Ui.lua",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c313_ui";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C313Ui"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());
    std::filesystem::remove_all(scratch);
}
TEST_CASE("C-228: ScriptHost decodes scriptData bytes into the OnCreate commandData table") {
    // `C-228` (`ScriptTask.lua:61-63`): retail's N->L `OnCreate(commandData:TABLE)`
    // stores the order table as `self.CommandData` — the shipped `EnhanceTask`
    // reads `self.CommandData.Enhancement` on every beat (`EnhanceTask.lua:28,40,76`).
    // The native intake carries the order table's bytes (`C-387`'s shared `Issue*`
    // parser + `0x009741c0` Lua-object decode); the live producer encodes the
    // enhancement name as raw bytes (`Match.cpp:1147`, `EnhancementTasks::taskTick`
    // decodes `std::string(data.begin(), data.end())`). The host currently pushes
    // an EMPTY table (`ScriptHost.cpp:620`), so `self.CommandData.Enhancement`
    // reads nil and the shipped shape can never name its work. This case issues
    // non-empty scriptData and requires the instance to see it as `Enhancement`.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C228Data", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  OnCreate = function(self, commandData)\n"
            "    self.CommandData = commandData\n"
            "    G_c228enh = commandData.Enhancement\n"
            "  end,\n"
            "  TaskTick = function(self, result)\n"
            "    if G_c228enh ~= 'AdvancedEngineering' then return 1 end\n"
            "    return -1\n"
            "  end,\n"
            "}\n",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c228_data";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    const std::string name = "AdvancedEngineering";
    const std::vector<std::uint8_t> data{name.begin(), name.end()};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C228Data",
                              .scriptData = data},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.errors().empty());
}
TEST_CASE("C-436: ScriptHost runs the shipped Enhancing loop on funded beats to Done") {
    // `C-436` (`/lua/sim/tasks/EnhanceTask.lua:44-84`): the shipped `Enhancing`
    // beat accrues ONLY when funded (`obtained > 0`), scales by
    // `(1 / (WorkItemBuildTime / GetBuildRate())) * obtained * SecondsPerTick()`,
    // edge-triggers the 0.25/0.50/0.75 milestones off the pre-tick
    // `LastProgress`, and finishes via `OnWorkEnd` + `Success` at >= 1.0.
    // The host currently starves the loop three ways: `GetResourceConsumed`
    // is a hard zero, `SecondsPerTick` is a number field the shipped call
    // `SecondsPerTick()` cannot invoke, and `WorkItemBuildTime` is absent —
    // so the task errors out on its first `Enhancing` beat and the queue
    // retires Done with no progress. This case publishes a funded economy
    // (build time 1.0 s at rate 1, fully funded) and requires ten 0.1 beats
    // to complete through the shipped shape.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C436Funded", rm::app::ScriptTaskRegistration{
        .source =
            "TASKSTATUS = { Done = -1, Suspend = -2, Abort = -3, Delay = -4,"
            " Repeat = 0, Wait = 1 }\n"
            "AIRESULT = { Unknown = 0, Success = 1, Fail = 2, Ignored = 3 }\n"
            "C436Task = Class({}) {\n"
            "  OnCreate = function(self, commandData)\n"
            "    self.CommandData = commandData\n"
            "    self:GetUnit():SetWorkProgress(0.0)\n"
            "    self.LastProgress = 0\n"
            "    ChangeState(self, self.Stopping)\n"
            "  end,\n"
            "  OnDestroy = function(self)\n"
            "    self:GetUnit():SetWorkProgress(0.0)\n"
            "    if self.Success then\n"
            "      self:SetAIResult(AIRESULT.Success)\n"
            "    else\n"
            "      self:SetAIResult(AIRESULT.Fail)\n"
            "      self:GetUnit():OnWorkFail(self.CommandData.Enhancement)\n"
            "    end\n"
            "  end,\n"
            "  Stopping = State {\n"
            "    TaskTick = function(self)\n"
            "      local unit = self:GetUnit()\n"
            "      if unit:IsMobile() and unit:IsMoving() then\n"
            "        unit:GetNavigator():AbortMove()\n"
            "        return TASKSTATUS.Wait\n"
            "      else\n"
            "        unit:OnWorkBegin(self.CommandData.Enhancement)\n"
            "        ChangeState(self, self.Enhancing)\n"
            "        return TASKSTATUS.Repeat\n"
            "      end\n"
            "    end,\n"
            "  },\n"
            "  Enhancing = State {\n"
            "    TaskTick = function(self)\n"
            "      local unit = self:GetUnit()\n"
            "      if unit:IsPaused() then return TASKSTATUS.Wait end\n"
            "      local current = unit.WorkProgress\n"
            "      local obtained = unit:GetResourceConsumed()\n"
            "      if obtained > 0 then\n"
            "        local frac = ( 1 / ( unit.WorkItemBuildTime / unit:GetBuildRate()) )"
            "          * obtained * SecondsPerTick()\n"
            "        current = current + frac\n"
            "        unit.WorkProgress = current\n"
            "      end\n"
            "      if( ( self.LastProgress < 0.25 and current >= 0.25 ) or\n"
            "          ( self.LastProgress < 0.50 and current >= 0.50 ) or\n"
            "          ( self.LastProgress < 0.75 and current >= 0.75 ) ) then\n"
            "        unit:OnBuildProgress(self.LastProgress,current)\n"
            "      end\n"
            "      self.LastProgress = current\n"
            "      unit:SetWorkProgress(current)\n"
            "      if( current < 1.0 ) then return TASKSTATUS.Wait end\n"
            "      G_c436final = current\n"
            "      self.Success = true\n"
            "      return TASKSTATUS.Done\n"
            "    end,\n"
            "  },\n"
            "}\n"
            "TypeClass = C436Task\n",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c436_funded";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    const std::string name = "AdvancedEngineering";
    const std::vector<std::uint8_t> data{name.begin(), name.end()};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C436Funded",
                              .scriptData = data},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    // Retail's `OnWorkBegin` derives `WorkItemBuildTime` from the blueprint;
    // this stub host takes the test's value: 1.0 s of work at rate 1, fully
    // funded, so each beat accrues exactly one 0.1 s tick of progress.
    host.publishTaskFunding(unit, 1.0, 1.0);
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    auto advance = [&] {
        return rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                       nullptr, nullptr, nullptr, nullptr, nullptr, armies,
                                       nullptr, nullptr, &host);
    };
    // `OnCreate` + `Stopping` (Repeat loops into `Enhancing` on the same
    // advance): the first funded beat banks 0.1 and waits.
    REQUIRE(advance() == 1);
    CHECK_FALSE(roster.store.orders()[unit.index].empty());
    CHECK(host.taskWorkProgress(unit) == Catch::Approx(0.1));
    for (int i = 0; i < 4; ++i) REQUIRE(advance() == 0);
    CHECK(host.taskWorkProgress(unit) == Catch::Approx(0.5));
    int beats = 0;
    while (!roster.store.orders()[unit.index].empty() && beats < 8) {
        REQUIRE(advance() == 0);
        ++beats;
    }
    CHECK(roster.store.orders()[unit.index].empty());
    // The shipped `OnDestroy` zeroes `WorkProgress` AFTER `Success`, so the
    // post-completion table reads 0 — the completion-time global `G_c436final`
    // (written on the retiring beat before `OnWorkEnd`) proves the funded
    // loop banked ten 0.1 beats to >= 1.0.
    CHECK(host.taskGlobalNumber("G_c436final") >= 1.0);
    for (const auto& err : host.errors()) WARN("lua: " << err);
    CHECK(host.errors().empty());
}

TEST_CASE("C-228: ScriptHost runs the REAL EnhanceTask.lua with a moho stub base") {
    // `C-228`/`C-388` (`0x00629530`): retail resolves an unregistered `Script`
    // task name through the VFS as `/lua/sim/tasks/<TaskName>.lua` — not a
    // registered inline source. This case mounts a scratch VFS carrying the
    // REAL `EnhanceTask.lua` (copied verbatim from `build/re-fa/corpus`, which
    // is gitignored and absent on fresh checkouts, hence SKIP) plus the real
    // `ScriptTask.lua` base, and requires the host to run the shipped
    // `OnCreate` -> `Stopping` -> `Enhancing` shape to `Done` on funded beats.
    // The one retail precondition the host cannot meet is `ScriptTask.lua:58`'s
    // `Class(moho.ScriptTask_Methods)` base: `moho` is an engine global, so the
    // host seeds a stub `moho.ScriptTask_Methods` engine-object table the way
    // the FAF sandbox seeds its `moho.*_methods` tables. Fail-first: the host
    // has no `moho` global, so the real file errors with `attempt to index a
    // nil value (global 'moho')` and the queue retires at once.
    const std::filesystem::path corpus = std::filesystem::path{"build/re-fa/corpus"};
    const std::filesystem::path taskFile =
        corpus / "lua/lua/sim/tasks/EnhanceTask.lua";
    const std::filesystem::path baseFile =
        corpus / "mohodata/lua/sim/ScriptTask.lua";
    if (!std::filesystem::exists(taskFile) || !std::filesystem::exists(baseFile)) {
        SKIP(std::string{"requires extracted retail corpus at "} + corpus.string());
    }
    const std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / "rm-script-host-real-enhance";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "lua/sim/tasks");
    std::filesystem::copy_file(taskFile, scratch / "lua/sim/tasks/EnhanceTask.lua");
    std::filesystem::copy_file(baseFile, scratch / "lua/sim/ScriptTask.lua");
    rm::vfs::Vfs content;
    content.mountDirectory(scratch);
    rm::app::ScriptHost host{content};
    // NOTE: no registerTask — the task resolves through the `C-388` VFS path.
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "real_enhance";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    const std::string name = "AdvancedEngineering";
    const std::vector<std::uint8_t> data{name.begin(), name.end()};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "EnhanceTask",
                              .scriptData = data},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    host.publishTaskFunding(unit, 1.0, 1.0);
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    auto advance = [&] {
        return rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                       nullptr, nullptr, nullptr, nullptr, nullptr, armies,
                                       nullptr, nullptr, &host);
    };
    REQUIRE(advance() == 1);
    CHECK_FALSE(roster.store.orders()[unit.index].empty());
    CHECK(host.taskWorkProgress(unit) == Catch::Approx(0.1));
    int beats = 0;
    while (!roster.store.orders()[unit.index].empty() && beats < 12) {
        REQUIRE(advance() == 0);
        ++beats;
    }
    CHECK(roster.store.orders()[unit.index].empty());
    CHECK(host.taskWorkProgress(unit) == Catch::Approx(0.0));
    for (const auto& err : host.errors()) WARN("lua: " << err);
    CHECK(host.errors().empty());
    std::filesystem::remove_all(scratch);
}
TEST_CASE("C-318: ScriptHost onDestroy calls the current state's OnDestroy") {
    // `C-318` (`mohodata/lua/system/class.lua`): `ChangeState` moves method
    // lookup to the new state. `taskTick` already re-reads the live
    // `TaskTick` every beat; `onDestroy` still calls the creation-time
    // `__rm_hooks.destroy`, so a state-local `OnDestroy` never fires.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C318Destroy", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  OnCreate = function(self) ChangeState(self, self.Enhancing) end,\n"
            "  OnDestroy = function(self) G_c318d = 1 end,\n"
            "  TaskTick = function(self) return 1 end,\n"
            "  Enhancing = State {\n"
            "    TaskTick = function(self) return 1 end,\n"
            "    OnDestroy = function(self) G_c318d = 2 end,\n"
            "  },\n"
            "}\n",
    });
    const auto field = flatField();
    const rm::sim::Terrain terrain{field};
    const auto grid = rm::sim::buildPassability(field, 0.0f, 60.0f, 0.0f);
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c318_destroy";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    auto armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Player> players{{.index = 0, .army = 0}};
    REQUIRE(rm::sim::applyCommand(
        rm::sim::CommandIssue{.source = 0,
                              .id = roster.store.allocateCommandId(0).value(),
                              .player = 0,
                              .kind = rm::sim::CommandKind::Script,
                              .units = {unit},
                              .scriptTask = "C318Destroy"},
        roster.store, roster.catalog, players, armies, terrain,
        [](rm::sim::UnitId) { return nullptr; }, roster.rate, nullptr, nullptr, nullptr,
        nullptr, &host));
    const std::vector<const rm::sim::PassabilityGrid*> grids{&grid};
    REQUIRE(rm::sim::advanceOrders(roster.store, roster.catalog, terrain, grids, roster.rate,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, armies, nullptr,
                                   nullptr, &host) == 1);
    roster.store.orders()[unit.index].clear();
    CHECK(host.taskGlobalNumber("G_c318d") == 2.0);
    // The live state's OnDestroy must win over the creation-time hook.
    REQUIRE(host.errors().empty());
}
TEST_CASE("C-452: ScriptHost TaskTick error aborts the thread, retires Fail") {
    // `C-452` (`0x00409253`-`0x0040927e`): the TaskTick error path is
    // WARN-and-Abort-thread (return -3), not a quiet Done (-1). The direct
    // `taskTick` return is the observable edge: the dispatcher maps -3 to
    // the abort arm (`AIRESULT.Fail`) and -1 to the finish arm (`Success`).
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C452Boom", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  TaskTick = function(self) error('boom') end,\n"
            "}\n",
    });
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c452_boom";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    rm::sim::ScriptTaskState state;
    host.onCreate(unit, "C452Boom", {}, state);
    REQUIRE(host.errors().empty());
    const std::int32_t result = host.taskTick(unit, "C452Boom", {}, state);
    CHECK(result == static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Abort));
    CHECK_FALSE(host.errors().empty());
}
TEST_CASE("C-228: ScriptHost Navigator AbortMove records the abort on the unit") {
    // `C-228` (`EnhanceTask.lua` `Stopping` beat): a mobile unit still under
    // way is halted first via `unit:GetNavigator():AbortMove()`. The host's
    // `GetNavigator` hands back a one-method table whose `AbortMove` is a
    // silent no-op, so the shipped halt leaves no trace the test can observe.
    // This case aborts mid-tick and requires the owning unit table to carry
    // the abort afterwards.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C228Abort", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  TaskTick = function(self)\n"
            "    local unit = self:GetUnit()\n"
            "    unit:GetNavigator():AbortMove()\n"
            "    G_c228aborted = (unit.MoveAborted == true) and 1 or 0\n"
            "    return -1\n"
            "  end,\n"
            "}\n",
    });
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c228_abort";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    rm::sim::ScriptTaskState state;
    host.onCreate(unit, "C228Abort", {}, state);
    REQUIRE(host.errors().empty());
    CHECK(host.taskTick(unit, "C228Abort", {}, state)
          == static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done));
    CHECK(host.taskGlobalNumber("G_c228aborted") == 1.0);
}
TEST_CASE("C-228: ScriptHost binds OnTargetLocation so TargetLocation OnCreate survives") {
    // `TargetLocation.lua:14`: `OnCreate` calls
    // `self:GetUnit():OnTargetLocation(commandData.Location)`. The host's
    // unit table binds no such method, so the real file dies in
    // construction with `attempt to call a nil value (method
    // 'OnTargetLocation')`. This case requires the call to land and record
    // its argument on the unit table.
    rm::vfs::Vfs content;
    rm::app::ScriptHost host{content};
    host.registerTask("C228Target", rm::app::ScriptTaskRegistration{
        .source =
            "TypeClass = Class({}) {\n"
            "  OnCreate = function(self)\n"
            "    self:GetUnit():OnTargetLocation('TGT')\n"
            "    G_c228tgt = (self:GetUnit().TargetLocation == 'TGT') and 1 or 0\n"
            "  end,\n"
            "  TaskTick = function(self) return -1 end,\n"
            "}\n",
    });
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "c228_target";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    rm::sim::ScriptTaskState state;
    const std::string taskName = "C228Target";
    host.onCreate(unit, std::string_view{taskName}, {}, state);
    for (const auto& err : host.errors()) WARN("lua: " << err);
    REQUIRE(host.errors().empty());
    CHECK(host.taskGlobalNumber("G_c228tgt") == 1.0);
}
