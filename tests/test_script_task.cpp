#include <catch2/catch_test_macros.hpp>

#include "core/sim/CommandQueue.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"

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
