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

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z) {
        const rm::UnitTypeIndex type = registerType(def);
        rm::sim::MoveState motion{};
        motion.armyIndex = 0;
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
    CHECK(fixture->option("UEB1103")->name == "Mass Extractor");

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

TEST_CASE("affordability dims what the bank cannot cover, at real prices",
          "[corpus][ui][build]") {
    std::unique_ptr<Fixture> fixture = makeFixture();
    if (!fixture) {
        SKIP("no Supreme Commander unit blueprints at " + unitRoot().string());
    }
    // 100 mass: a 36-mass extractor is buildable now; the land factory is not — and the
    // tray says so by dimming rather than hiding, so the layout never reflows.
    fixture->scene.economies[0].stored.mass = rm::sim::magFromFloat(100.0f);
    const rm::sim::UnitId commander = fixture->spawn(*fixture->real("UEL0001"), 300.0f, 300.0f);
    rm::app::gatherBuildOptions(fixture->scene, commander, rm::ui::neutralTheme(),
                                 fixture->options, fixture->who);

    REQUIRE(fixture->option("UEB1103") != nullptr);
    REQUIRE(fixture->option("UEB0101") != nullptr);
    CHECK(fixture->option("UEB1103")->affordable);
    CHECK_FALSE(fixture->option("UEB0101")->affordable);
}
