#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

} // namespace

TEST_CASE("the match runner services same-army path requests on later beats") {
    const rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});
    scene.commandersEver.assign(2, 0);

    rm::unitdef::UnitDef tank;
    tank.name = "test_tank";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 10.0f;
    scene.definitions.push_back(tank);
    const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);

    const auto spawn = [&](float z) {
        return scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(200.0f), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(tank, 0),
            .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
        });
    };
    const rm::sim::UnitId first = spawn(200.0f);
    const rm::sim::UnitId second = spawn(300.0f);

    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner = rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();

    REQUIRE(rm::app::issueMove(scene, first, 0, 0, rm::sim::fxFromFloat(500.0f),
                                rm::sim::fxFromFloat(200.0f)));
    REQUIRE(rm::app::issueMove(scene, second, 0, 0, rm::sim::fxFromFloat(500.0f),
                                rm::sim::fxFromFloat(300.0f)));

    (void)rm::app::advanceMatch(runner, 0, 0.0f);
    CHECK(scene.store.motion()[first.index].path.empty());
    CHECK(scene.store.motion()[second.index].path.empty());

    (void)rm::app::advanceMatch(runner, 1, 0.0f);
    CHECK_FALSE(scene.store.motion()[first.index].path.empty());
    CHECK(scene.store.motion()[second.index].path.empty());

    (void)rm::app::advanceMatch(runner, 2, 0.0f);
    CHECK_FALSE(scene.store.motion()[second.index].path.empty());
}
