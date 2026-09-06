// The UI journey for building the initial economy, against REAL content.
//
// test_build_options.cpp proves each link with a synthetic corpus; this file proves the
// CHAIN with the retail blueprints — because "the tray offers a mass extractor for 36" is
// only a fact about the game if 36 came out of UEB1103_unit.bp and not out of a fixture.
// Selected commander -> tray options -> a cell click hit-tests back to the option -> the
// armed build passes the placement test and lands in the sim as a construction of that
// blueprint, at that spot, through the same issueBuild the windowed tray calls.
//
// Gated on the extracted corpus at ~/projects/llm/input/faf/units, the same way
// test_real_unit_blueprints.cpp is — SKIP, not fail, on a machine without it.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Interface.hpp"
#include "app/Match.hpp"
#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"

#include "core/sim/Pathfinding.hpp"
#include "core/ui/BuildPanel.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using rm::app::UnitScene;

namespace {

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// The scene the tray runs against, assembled from RETAIL definitions — the app's own
/// split: a contiguous corpus for the roster, the scene's deque for catalog-registered
/// types (the catalog holds pointers; a reallocating vector would dangle them).
struct Fixture {
    UnitScene scene;
    std::vector<rm::unitdef::UnitDef> corpus;
    std::vector<std::string> ids;
    rm::HeightField field = flatField();
    rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::sim::TickRate rate{};
    rm::app::BuildSelection who;
    std::vector<rm::ui::BuildOption> options;

    [[nodiscard]] bool loadReal(const char* id) {
        const std::filesystem::path path =
            unitRoot() / id / (std::string{id} + "_unit.bp");
        auto def = rm::unitbp::loadFile(path);
        if (!def) {
            return false;
        }
        ids.emplace_back(id);
        corpus.push_back(std::move(*def));
        return true;
    }

    /// Registers a corpus def as a live catalog type — what `resolveBuildable` does when
    /// the tray arms a cell, including the movement traits used for build placement.
    rm::UnitTypeIndex registerType(const rm::unitdef::UnitDef& def) {
        scene.definitions.push_back(def);
        const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back(), rate);
        scene.setTypeTraits(type, rm::data::moveDefFor(def), def.meshToElmos);
        return type;
    }

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z, int army = 0) {
        const rm::UnitTypeIndex type = registerType(def);
        rm::sim::MoveState motion{};
        motion.armyIndex = army;
        rm::sim::Transform transform{};
        transform.x = rm::sim::fxFromFloat(x);
        transform.z = rm::sim::fxFromFloat(z);
        return scene.store.spawn({
            .type = type,
            .transform = transform,
            .motion = motion,
            .health = rm::sim::Health{.current = rm::sim::magFromFloat(12000.0f),
                                      .maximum = rm::sim::magFromFloat(12000.0f)},
        });
    }

    [[nodiscard]] const rm::unitdef::UnitDef* real(std::string_view id) const {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == id) {
                return &corpus[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const rm::ui::BuildOption* option(std::string_view id) const {
        const auto found =
            std::find_if(options.begin(), options.end(),
                         [&](const rm::ui::BuildOption& o) { return o.id == id; });
        return found != options.end() ? &*found : nullptr;
    }
};

/// The initial-economy cast: the UEF commander and everything the opening builds, plus two
/// decoys the tray must exclude (a T2 extractor, a tank).
[[nodiscard]] std::unique_ptr<Fixture> makeFixture() {
    auto fixture = std::make_unique<Fixture>();
    for (const char* id :
         {"UEL0001", "UEB1103", "UEB1101", "UEB0101", "UEB1105", "UEB1201", "UEL0201"}) {
        if (!fixture->loadReal(id)) {
            return nullptr;
        }
    }
    fixture->scene.roster = rm::data::Roster::build(fixture->corpus, fixture->ids);
    fixture->scene.armies.push_back(
        rm::sim::Army{.index = 0, .faction = rm::sim::Faction::Uef});
    fixture->scene.economies.resize(1);
    fixture->scene.economies[0].stored.mass = rm::sim::magFromFloat(650.0f);
    fixture->scene.economies[0].stored.energy = rm::sim::magFromFloat(5000.0f);
    fixture->scene.players = rm::sim::onePlayerPerArmy(1, 0);
    fixture->scene.playerArmy = 0;
    return fixture;
}

} // namespace

TEST_CASE("a real commander's tray offers the initial economy, priced by the blueprints",
          "[corpus][ui][build]") {
    std::unique_ptr<Fixture> fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    const rm::sim::UnitId commander = fixture->spawn(*fixture->real("UEL0001"), 300.0f, 300.0f);
    rm::app::gatherBuildOptions(fixture->scene, commander, rm::ui::neutralTheme(),
                                 fixture->options, fixture->who);

    // The whole opening is on the tray: extractor, power, storage, factory — and each cell
    // carries the blueprint's OWN price and display name, which is the claim a synthetic
    // fixture cannot make.
    for (const char* id : {"UEB1103", "UEB1101", "UEB1105", "UEB0101"}) {
        const rm::ui::BuildOption* offered = fixture->option(id);
        INFO(id);
        REQUIRE(offered != nullptr);
        const rm::unitdef::UnitDef* def = fixture->real(id);
        CHECK(offered->massCost
              == Approx(rm::sim::magToFloat(def->buildCostMass)).margin(0.01));
        CHECK_FALSE(offered->name.empty());
    }
    CHECK(fixture->option("UEB1103")->name == "T1 Mass Extractor");

    // The decoys stay off it: T2 needs a T2 engineer, and a tank is not a structure.
    CHECK(fixture->option("UEB1201") == nullptr);
    CHECK(fixture->option("UEL0201") == nullptr);
}

TEST_CASE("a tray cell hit-tests to its option and the click becomes that construction",
          "[corpus][ui][build]") {
    std::unique_ptr<Fixture> fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    const rm::sim::UnitId commander = fixture->spawn(*fixture->real("UEL0001"), 300.0f, 300.0f);
    rm::app::gatherBuildOptions(fixture->scene, commander, rm::ui::neutralTheme(),
                                 fixture->options, fixture->who);
    REQUIRE_FALSE(fixture->options.empty());

    // The click's first leg: a point in the mex's own cell resolves to the mex's index —
    // the same layout and hit-test the windowed tray uses.
    const rm::ui::FrameLayout frame =
        rm::ui::frameLayout(rm::ui::UiViewport::full(1280.0f, 720.0f));
    const rm::ui::BuildPanelLayout panel =
        rm::ui::buildPanelLayout(frame, fixture->options.size());
    std::size_t mexIndex = 0;
    while (fixture->options[mexIndex].id != "UEB1103") {
        ++mexIndex;
        REQUIRE(mexIndex < fixture->options.size());
    }
    const std::array<float, 2> origin = rm::ui::buildCellOrigin(panel, mexIndex);
    CHECK(rm::ui::buildOptionAt(panel, fixture->options.size(),
                                origin[0] + panel.cellWidth * 0.5f,
                                origin[1] + panel.cellHeight * 0.5f)
          == std::optional<std::size_t>{mexIndex});

    // The second leg: the armed build's ground truth and the order itself. The site is
    // free by the same test the ghost colours by, and issueBuild — the tray's own exit —
    // lands a construction of that blueprint at that spot.
    const rm::UnitTypeIndex mexType = fixture->registerType(*fixture->real("UEB1103"));
    const float clickX = 400.0f;
    const float clickZ = 300.0f;
    fixture->scene.resourceDeposits.push_back({rm::unitdef::BuildRestriction::MassDeposit,
        rm::sim::fxFromFloat(clickX), rm::sim::fxFromFloat(clickZ)});
    CHECK(rm::sim::sitePlaceable(fixture->grid, rm::sim::fxFromFloat(clickX),
                                 rm::sim::fxFromFloat(clickZ), rm::sim::fxFromFloat(3.0f)));
    REQUIRE(rm::app::issueBuild(fixture->scene, commander,
                                 rm::app::playerDriving(fixture->scene, 0), 0, mexType,
                                 rm::sim::fxFromFloat(clickX), rm::sim::fxFromFloat(clickZ)));
    const auto dispatched = rm::app::dispatchCommands(
        fixture->scene, fixture->field, fixture->passability, 0,
        rm::sim::CommandPhase::PreTick);
    REQUIRE(dispatched.size() == 1);
    REQUIRE(dispatched.front().result.accepted == std::vector{commander});
    REQUIRE(fixture->scene.building.size() == 1);
    const rm::sim::Construction& work = fixture->scene.building.front();
    CHECK(work.armyIndex == 0);
    CHECK(work.blueprintIndex == mexType);
    CHECK(rm::sim::fxToFloat(work.position[0]) == Approx(clickX));
    CHECK(rm::sim::fxToFloat(work.position[2]) == Approx(clickZ));
    // Costed from the real blueprint, not from anything the UI carried.
    CHECK(rm::sim::magToFloat(work.cost.mass)
          == Approx(rm::sim::magToFloat(fixture->real("UEB1103")->buildCostMass)));
}

TEST_CASE("a real T2 upgrade starts with an empty bank and progresses from income",
          "[corpus][ui][build]") {
    auto fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    REQUIRE(fixture->loadReal("UEB0201"));
    fixture->scene.roster = rm::data::Roster::build(fixture->corpus, fixture->ids);
    fixture->scene.economies[0].stored = {};
    const auto factory = fixture->spawn(*fixture->real("UEB0101"), 300.0f, 300.0f);
    const auto successor = fixture->registerType(*fixture->real("UEB0201"));
    rm::app::gatherBuildOptions(fixture->scene, factory, rm::ui::neutralTheme(),
                                fixture->options, fixture->who);
    const auto* option = fixture->option("UEB0201");
    REQUIRE(option != nullptr);
    CHECK_FALSE(option->affordable);
    CHECK(rm::ui::buildOptionAction(*option, fixture->who.role)
          == rm::ui::BuildOptionAction::SubmitAtBuilder);
    REQUIRE(rm::app::issueBuild(fixture->scene, factory, 0, 0, successor,
                                rm::sim::Fx::fromInt(300), rm::sim::Fx::fromInt(300)));
    rm::vfs::Vfs content;
    auto runner = rm::app::makeMatchRunner(fixture->scene, fixture->field,
                                          fixture->passability, content, {}, {});
    runner.scripts.clear();
    for (int tick = 0; tick < 10; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    REQUIRE(fixture->scene.building.size() == 1);
    CHECK(fixture->scene.building.front().upgradeOf == factory);
    CHECK_FALSE(fixture->scene.building.front().finished());
    const auto stalled = fixture->scene.building.front().buildTimeRemaining;
    for (int tick = 10; tick < 20; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    CHECK(fixture->scene.building.front().buildTimeRemaining == stalled);
    const auto stalledCard = rm::app::constructionCard(fixture->scene, factory);
    REQUIRE(stalledCard);
    CHECK(stalledCard->title == "UPGRADING");
    CHECK(stalledCard->rows.back().value == "STALLED");

    // A real commander supplies resource flow; its bank is still empty when it arrives.
    (void)fixture->spawn(*fixture->real("UEL0001"), 700.0f, 700.0f);
    for (int tick = 20; tick < 40; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    CHECK(fixture->scene.building.front().buildTimeRemaining < stalled);
    const auto fundedCard = rm::app::constructionCard(fixture->scene, factory);
    REQUIRE(fundedCard);
    CHECK(*fundedCard->progress > *stalledCard->progress);
    CHECK(fixture->scene.economies[0].stored.mass < fixture->real("UEB0201")->buildCostMass);
}

TEST_CASE("HUD flow includes construction spending from a single power generator",
          "[corpus][ui][economy-flow]") {
    auto fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    REQUIRE(fixture->loadReal("UEB0201"));
    fixture->scene.economies[0].stored.energy = {};
    const auto factory = fixture->spawn(*fixture->real("UEB0101"), 300.0f, 300.0f);
    const auto generator = fixture->spawn(*fixture->real("UEB1101"), 400.0f, 300.0f);
    const auto successor = fixture->registerType(*fixture->real("UEB0201"));
    REQUIRE(rm::app::issueBuild(fixture->scene, factory, 0, 0, successor,
                                rm::sim::Fx::fromInt(300), rm::sim::Fx::fromInt(300)));
    rm::vfs::Vfs content;
    auto runner = rm::app::makeMatchRunner(fixture->scene, fixture->field,
                                          fixture->passability, content, {}, {});
    runner.scripts.clear();
    auto& economy = fixture->scene.economies[0];
    const auto tickAndCheckFlow = [&](int tick) {
        const auto before = economy.stored;
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
        const auto hud = rm::app::hudStateFrom(fixture->scene, 0, rm::ui::GameProfile::Fa);
        const float hz = static_cast<float>(fixture->rate.ticksPerSecond());
        // No gifts, reclaim or overflow in this fixture: net flow must match the bank delta.
        CHECK(hud.resources[0].gauge.net()
              == Approx(rm::sim::magToFloat(economy.stored.mass - before.mass) * hz).margin(0.001));
        CHECK(hud.resources[1].gauge.net()
              == Approx(rm::sim::magToFloat(economy.stored.energy - before.energy) * hz).margin(0.001));
        CHECK(hud.resources[1].gauge.incomePerSecond == Approx(20).margin(0.001));
    };
    for (int tick = 0; tick < 10; ++tick) tickAndCheckFlow(tick);
    REQUIRE(fixture->scene.building.size() == 1);
    CHECK(economy.stored.energy < rm::sim::Mag::fromInt(1));
    CHECK(economy.fundedFraction < rm::sim::kFxOne);
    std::vector<rm::ui::RosterTile> tiles;
    rm::app::gatherRoster(fixture->scene, std::array{generator}, tiles);
    REQUIRE(tiles.size() == 1);
    auto card = rm::ui::rosterTileCard(tiles.front());
    REQUIRE(card.rows.size() == 3);
    CHECK(card.rows[0].label == "MASS /s");
    CHECK(card.rows[1].label == "ENERGY /s");
    CHECK(card.rows[1].value == "+20 / -0");
    rm::app::gatherRoster(fixture->scene, std::array{factory}, tiles);
    card = rm::ui::rosterTileCard(tiles.front());
    REQUIRE(card.rows.size() == 3);
    CHECK(card.rows[1].value == "+0 / -20");
    REQUIRE(rm::app::submitCommand(fixture->scene,
        {.tick = 10, .source = 0, .player = 0,
         .kind = rm::sim::CommandKind::Stop, .units = {factory}}));
    for (int tick = 10; tick < 20; ++tick) tickAndCheckFlow(tick);
    CHECK(fixture->scene.building.empty());
    CHECK(economy.stored.energy >= rm::sim::Mag::fromInt(19));
    rm::app::gatherRoster(fixture->scene, std::array{factory}, tiles);
    CHECK(rm::ui::rosterTileCard(tiles.front()).rows[1].value == "+0 / -0");
}

TEST_CASE("enemy construction cannot spend or borrow the player's resources",
          "[corpus][ui][economy-isolation]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    REQUIRE(fixture->loadReal("UEB0201"));
    fixture->scene.armies.push_back({.index = 1, .alliance = 1});
    fixture->scene.economies.resize(2);
    fixture->scene.players = rm::sim::onePlayerPerArmy(2, 0);
    fixture->scene.economies[0].stored.energy = {};
    auto& enemy = fixture->scene.economies[1];
    bool funded = true;
    SECTION("enemy pays from its own bank") {
        enemy.stored = {.mass = rm::sim::Mag::fromInt(500),
                        .energy = rm::sim::Mag::fromInt(5000)};
    }
    SECTION("empty enemy bank cannot borrow from a rich player") {
        funded = false;
        fixture->scene.economies[0].stored.energy = rm::sim::Mag::fromInt(4000);
    }
    (void)fixture->spawn(*fixture->real("UEB1101"), 100, 100);
    const auto factory = fixture->spawn(*fixture->real("UEB0101"), 700, 700, 1);
    const auto successor = fixture->registerType(*fixture->real("UEB0201"));
    REQUIRE(rm::app::issueBuild(fixture->scene, factory, 1, 0, successor,
                                rm::sim::Fx::fromInt(700), rm::sim::Fx::fromInt(700)));
    rm::vfs::Vfs content;
    auto runner = rm::app::makeMatchRunner(fixture->scene, fixture->field,
                                          fixture->passability, content, {}, {});
    runner.scripts.clear();
    for (int tick = 0; tick < 20; ++tick) {
        const auto before = fixture->scene.economies[0].stored;
        (void)rm::app::advanceMatch(runner, tick, 0);
        const auto& player = fixture->scene.economies[0];
        CHECK(player.stored.mass == before.mass);
        CHECK(player.stored.energy == before.energy + rm::sim::Mag::fromInt(2));
        CHECK(player.usageLastTick.mass == rm::sim::Mag{});
        CHECK(player.usageLastTick.energy == rm::sim::Mag{});
        CHECK(enemy.sharedIn.mass == rm::sim::Mag{});
        CHECK(enemy.sharedIn.energy == rm::sim::Mag{});
    }
    REQUIRE(fixture->scene.building.size() == 1);
    CHECK(fixture->scene.building.front().armyIndex == 1);
    CHECK((enemy.usageLastTick.energy > rm::sim::Mag{}) == funded);
    CHECK((fixture->scene.building.front().fundedLastTick > rm::sim::Fx{}) == funded);
}

TEST_CASE("a short bank marks but does not disable a real build option",
          "[corpus][ui][build]") {
    std::unique_ptr<Fixture> fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    // 100 mass: the bank covers a 36-mass extractor but not the land factory's full price.
    // Both remain valid actions because construction is paid from flow after it starts.
    fixture->scene.economies[0].stored.mass = rm::sim::magFromFloat(100.0f);
    const rm::sim::UnitId commander = fixture->spawn(*fixture->real("UEL0001"), 300.0f, 300.0f);
    rm::app::gatherBuildOptions(fixture->scene, commander, rm::ui::neutralTheme(),
                                 fixture->options, fixture->who);

    REQUIRE(fixture->option("UEB1103") != nullptr);
    REQUIRE(fixture->option("UEB0101") != nullptr);
    CHECK(fixture->option("UEB1103")->affordable);
    CHECK_FALSE(fixture->option("UEB0101")->affordable);
    CHECK(rm::ui::buildOptionAction(*fixture->option("UEB0101"), "commander")
          == rm::ui::BuildOptionAction::ArmPlacement);
}

TEST_CASE("mass extractor refuses a site without a deposit", "[corpus][resource-deposit]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("retail corpus unavailable");
    const auto commander = fixture->spawn(*fixture->real("UEL0001"), 300.0f, 300.0f);
    const auto mex = fixture->registerType(*fixture->real("UEB1103"));
    SECTION("no deposits") {}
    SECTION("a hydrocarbon deposit is not a mass deposit") {
        fixture->scene.resourceDeposits.push_back({rm::unitdef::BuildRestriction::HydrocarbonDeposit,
            rm::sim::Fx::fromInt(400), rm::sim::Fx::fromInt(300)});
    }
    SECTION("a nearby deposit does not authorize an off-centre order") {
        fixture->scene.resourceDeposits.push_back({rm::unitdef::BuildRestriction::MassDeposit,
            rm::sim::Fx::fromInt(408), rm::sim::Fx::fromInt(300)});
    }
    REQUIRE(rm::app::issueBuild(fixture->scene, commander,
        rm::app::playerDriving(fixture->scene, 0), 0, mex,
        rm::sim::fxFromFloat(400.0f), rm::sim::fxFromFloat(300.0f)));
    const auto results = rm::app::dispatchCommands(fixture->scene, fixture->field,
        fixture->passability, 0, rm::sim::CommandPhase::PreTick);
    REQUIRE(results.size() == 1);
    CHECK(results.front().result.accepted.empty());
    CHECK(fixture->scene.building.empty());
}

TEST_CASE("resource sites are marked and matching placement snaps to their centres",
          "[corpus][resource-deposit]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("retail corpus unavailable");
    const auto mex = fixture->registerType(*fixture->real("UEB1103"));
    const auto generator = fixture->registerType(*fixture->real("UEB1101"));
    fixture->scene.resourceDeposits = {
        {rm::unitdef::BuildRestriction::MassDeposit, rm::sim::Fx::fromInt(400), rm::sim::Fx::fromInt(300)},
        {rm::unitdef::BuildRestriction::HydrocarbonDeposit, rm::sim::Fx::fromInt(420), rm::sim::Fx::fromInt(300)}};
    const std::array<float, 2> cursor{417, 301};
    CHECK(rm::app::snapResourceSite(fixture->scene, mex, cursor) == std::array<float, 2>{400, 300});
    CHECK(rm::app::snapResourceSite(fixture->scene, generator, cursor) == cursor);
    CHECK(rm::app::snapResourceSite(fixture->scene, mex, {500, 300}) == std::array<float, 2>{500, 300});
    std::vector<rm::ui::MinimapPip> pips;
    rm::app::appendMinimapPips(pips, fixture->scene);
    REQUIRE(pips.size() == 2);
    CHECK(pips[0].worldX == 400);
    CHECK(pips[0].colour != pips[1].colour);
    std::vector<rm::DecalVertex> decals;
    rm::app::appendResourceDeposits(decals, fixture->scene, fixture->field);
    CHECK_FALSE(decals.empty());
    REQUIRE(fixture->loadReal("UEB1102"));
    CHECK(fixture->real("UEB1102")->buildRestriction == rm::unitdef::BuildRestriction::HydrocarbonDeposit);
}

TEST_CASE("factory trays expose all unlocked tiers with the newest units first",
          "[corpus][ui][factory-tiers]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("retail corpus unavailable");
    for (const auto* id : {"UEB0201", "UEB0301", "UEL0105", "UEL0202", "UEL0303"}) {
        REQUIRE(fixture->loadReal(id));
    }
    fixture->scene.roster = rm::data::Roster::build(fixture->corpus, fixture->ids);
    const char* factoryId = "UEB0101";
    const char* productId = "UEL0105";
    int tier = 1;
    SECTION("T1 keeps higher-tier units locked") {}
    SECTION("T2 exposes T2 units and retains T1") {
        factoryId = "UEB0201"; productId = "UEL0202"; tier = 2;
    }
    SECTION("T3 exposes T3 units and retains earlier tiers") {
        factoryId = "UEB0301"; productId = "UEL0303"; tier = 3;
    }
    const auto factory = fixture->spawn(*fixture->real(factoryId), 400, 400);
    rm::app::gatherBuildOptions(fixture->scene, factory, rm::ui::neutralTheme(),
        fixture->options, fixture->who);
    CHECK(fixture->option("UEL0105") != nullptr);
    CHECK((fixture->option("UEL0202") != nullptr) == (tier >= 2));
    CHECK((fixture->option("UEL0303") != nullptr) == (tier >= 3));
    const auto firstProduct = std::ranges::find_if(fixture->options,
        [](const auto& option) { return !option.upgrade; });
    REQUIRE(firstProduct != fixture->options.end());
    CHECK(firstProduct->name.starts_with("T" + std::to_string(tier) + " "));
    REQUIRE(fixture->option(productId));
    const auto index = static_cast<std::size_t>(fixture->option(productId) - fixture->options.data());
    const auto frame = rm::ui::frameLayout(rm::ui::UiViewport::full(1280, 720));
    const auto firstPage = rm::ui::buildPanelLayout(frame, fixture->options.size());
    REQUIRE(index < firstPage.shown); // Newly unlocked units are immediately visible.
    const auto cell = rm::ui::buildCellOrigin(firstPage, index);
    CHECK(rm::ui::buildOptionAt(firstPage, fixture->options.size(),
        cell[0] + firstPage.cellWidth / 2, cell[1] + firstPage.cellHeight / 2) == index);
    const auto product = fixture->registerType(*fixture->real(productId));
    REQUIRE(rm::app::issueBuild(fixture->scene, factory, 0, 0, product,
        rm::sim::Fx::fromInt(400), rm::sim::Fx::fromInt(400)));
    const auto dispatched = rm::app::dispatchCommands(fixture->scene, fixture->field,
        fixture->passability, 0, rm::sim::CommandPhase::PreTick);
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().result.accepted == std::vector{factory});
}

TEST_CASE("building facings stay on the cardinal grid", "[ui][building-facing]") {
    const auto field = flatField();
    for (int x : {100, 250, 500, 700, 900}) {
        for (int z : {100, 350, 600, 800, 900}) {
            const auto facing = rm::app::structureFacing(field,
                rm::sim::Fx::fromInt(x), rm::sim::Fx::fromInt(z));
            CHECK(facing % rm::sim::kBradQuarterTurn == 0);
        }
    }
}

TEST_CASE("engineer trays follow the authored construction tiers", "[corpus][ui][engineer-tiers]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("retail corpus unavailable");
    for (const auto* id : {"UEL0105", "UEL0208", "UEL0309", "UEB1301", "UEB4202"})
        REQUIRE(fixture->loadReal(id));
    fixture->scene.roster = rm::data::Roster::build(fixture->corpus, fixture->ids);
    const char* engineerId = "UEL0105";
    int tier = 1;
    SECTION("T1") {}
    SECTION("T2") { engineerId = "UEL0208"; tier = 2; }
    SECTION("T3") { engineerId = "UEL0309"; tier = 3; }
    const auto engineer = fixture->spawn(*fixture->real(engineerId), 400, 400);
    rm::app::gatherBuildOptions(fixture->scene, engineer, rm::ui::neutralTheme(),
        fixture->options, fixture->who);
    REQUIRE(fixture->option("UEB1101"));
    CHECK((fixture->option("UEB1201") != nullptr) == (tier >= 2));
    CHECK((fixture->option("UEB1301") != nullptr) == (tier >= 3));
    CHECK((fixture->option("UEB4202") != nullptr) == (tier >= 2)); // Shield, outside the old role list.
    CHECK_FALSE(fixture->option("UEL0201"));
    REQUIRE_FALSE(fixture->options.empty());
    CHECK(fixture->options.front().name.starts_with("T" + std::to_string(tier) + " "));
    const auto* productId = tier == 3 ? "UEB1301" : tier == 2 ? "UEB1201" : "UEB1101";
    const auto type = fixture->registerType(*fixture->real(productId));
    REQUIRE(rm::app::issueBuild(fixture->scene, engineer, 0, 0, type,
        rm::sim::Fx::fromInt(480), rm::sim::Fx::fromInt(400)));
    const auto dispatched = rm::app::dispatchCommands(fixture->scene, fixture->field,
        fixture->passability, 0, rm::sim::CommandPhase::PreTick);
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().result.accepted == std::vector{engineer});
}

TEST_CASE("extractors offer their next upgrade and no unrelated construction",
          "[corpus][ui][extractor-upgrades]") {
    auto fixture = makeFixture();
    if (!fixture) SKIP("retail corpus unavailable");
    REQUIRE(fixture->loadReal("UEB1202"));
    REQUIRE(fixture->loadReal("UEB1302"));
    fixture->scene.roster = rm::data::Roster::build(fixture->corpus, fixture->ids);
    const char* from = "UEB1103";
    const char* to = "UEB1202";
    SECTION("T1 to T2") {}
    SECTION("T2 to T3") { from = "UEB1202"; to = "UEB1302"; }
    const auto extractor = fixture->spawn(*fixture->real(from), 400, 400);
    std::vector<rm::sim::UnitId> candidates;
    rm::app::gatherBuilderCandidates(fixture->scene, std::array{extractor}, candidates);
    CHECK(candidates == std::vector{extractor});
    rm::app::gatherBuildOptions(fixture->scene, extractor, rm::ui::neutralTheme(),
        fixture->options, fixture->who);
    REQUIRE(fixture->options.size() == 1);
    CHECK(fixture->options.front().id == to);
    CHECK(fixture->options.front().upgrade);
    CHECK_FALSE(rm::app::gatherProduction(fixture->scene, extractor));
    const auto upgrade = fixture->registerType(*fixture->real(to));
    REQUIRE(rm::app::issueBuild(fixture->scene, extractor, 0, 0, upgrade,
        rm::sim::Fx::fromInt(400), rm::sim::Fx::fromInt(400)));
    const auto dispatched = rm::app::dispatchCommands(fixture->scene, fixture->field,
        fixture->passability, 0, rm::sim::CommandPhase::PreTick);
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().result.accepted == std::vector{extractor});
    REQUIRE(fixture->scene.building.size() == 1);
    CHECK(fixture->scene.building.front().isUpgrade());
}
