// Player-facing fog: presentation must show exactly what the intel contact model knows.
#include <catch2/catch_test_macros.hpp>

#include "app/Interface.hpp"
#include "app/Scene.hpp"
#include "core/sim/Snapshot.hpp"

#include <vector>

namespace {

struct FogFixture {
    rm::unitdef::UnitDef watcher;
    rm::unitdef::UnitDef target;
    rm::app::UnitScene scene;
    rm::sim::UnitId targetId;
    rm::UnitTypeIndex targetType = 0;

    FogFixture(bool targetCloaked, bool targetFreeIntel, float vision = 240.0f,
               float radar = 0.0f) {
        watcher.visionRadiusElmos = vision;
        watcher.radarRadiusElmos = radar;
        target.cloak = targetCloaked;
        target.freeIntel = targetFreeIntel;

        const rm::UnitTypeIndex watcherType = scene.catalog.add(&watcher);
        targetType = scene.catalog.add(&target);
        scene.armies = {
            rm::sim::Army{.index = 0, .alliance = 0},
            rm::sim::Army{.index = 1, .alliance = 1},
        };
        scene.playerArmy = 0;
        scene.intel.configure(2, rm::sim::Fx::fromInt(1024), rm::sim::Fx::fromInt(1024),
                              rm::sim::VisionStyle::ForgedAlliance);

        spawn(watcherType, 0, 100, 100);
        targetId = spawn(targetType, 1, 200, 100);
        scene.intel.update(scene.store, scene.catalog, scene.armies, nullptr);
        scene.publish(1);
    }

    rm::sim::UnitId spawn(rm::UnitTypeIndex type, int army, int x, int z) {
        rm::sim::UnitStore::Spawn request;
        request.type = type;
        request.motion.armyIndex = army;
        request.transform.x = rm::sim::Fx::fromInt(x);
        request.transform.z = rm::sim::Fx::fromInt(z);
        request.health.current = rm::sim::Mag::fromInt(100);
        request.health.maximum = request.health.current;
        return scene.store.spawn(request);
    }
};

} // namespace

TEST_CASE("a cloaked enemy does not leak through the minimap's raw vision check") {
    FogFixture fixture{/*targetCloaked=*/true, /*targetFreeIntel=*/false};
    std::vector<rm::ui::MinimapPip> pips;

    rm::app::appendMinimapPips(pips, fixture.scene);

    REQUIRE(pips.size() == 1);
    CHECK(pips.front().worldX == 100.0f);
    CHECK_FALSE(fixture.scene.visibleToViewer(fixture.targetId));
}

TEST_CASE("free intel reaches presentation even outside ordinary sight") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/true};
    fixture.scene.store.transforms()[1].x = rm::sim::Fx::fromInt(900);
    fixture.scene.intel.update(fixture.scene.store, fixture.scene.catalog,
                               fixture.scene.armies, nullptr);
    fixture.scene.publish(2);
    std::vector<rm::ui::MinimapPip> pips;

    rm::app::appendMinimapPips(pips, fixture.scene);

    REQUIRE(pips.size() == 2);
    CHECK(pips.back().worldX == 900.0f);
    CHECK(fixture.scene.visibleToViewer(fixture.targetId));
}

TEST_CASE("radar contributes one anonymous displaced blip and no exact enemy pip") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false,
                       /*vision=*/0.0f, /*radar=*/240.0f};
    std::vector<rm::ui::MinimapPip> pips;

    rm::app::appendMinimapPips(pips, fixture.scene);

    REQUIRE(pips.size() == 2);
    CHECK(pips[0].worldX == 100.0f);
    CHECK(pips[1].worldX != 200.0f);
    REQUIRE(fixture.scene.contactScratch.size() == 2);
    CHECK(fixture.scene.contactScratch.back().isBlip());
}

TEST_CASE("an observer gets exact units without duplicate sensor guesses") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false,
                       /*vision=*/0.0f, /*radar=*/240.0f};
    fixture.scene.playerArmy = rm::sim::kNoArmy;
    std::vector<rm::ui::MinimapPip> pips;

    rm::app::appendMinimapPips(pips, fixture.scene);

    REQUIRE(pips.size() == 2);
    CHECK(pips[1].worldX == 200.0f);
    CHECK(fixture.scene.contactScratch.empty());
}

TEST_CASE("the player HUD counts own units, not hidden enemy production") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false};

    CHECK(rm::app::hudStateFrom(fixture.scene, 0.0f, rm::ui::GameProfile::Fa).unitsAlive == 1);
    const rm::ui::MatchState bar =
        rm::app::hudStateFrom(fixture.scene, 0.0f, rm::ui::GameProfile::Bar);
    CHECK(bar.resources[0].name == "METAL");
    CHECK(bar.resources[1].name == "ENERGY");
    fixture.scene.playerArmy = rm::sim::kNoArmy;
    CHECK(rm::app::hudStateFrom(fixture.scene, 0.0f, rm::ui::GameProfile::Fa).unitsAlive == 2);
}

TEST_CASE("slot reuse cannot make a stale unit generation visible") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false};
    fixture.scene.refreshViewerContacts();
    REQUIRE(fixture.scene.visibleToViewer(fixture.targetId));

    const rm::sim::UnitId stale = fixture.targetId;
    fixture.scene.store.kill(stale);
    fixture.targetId = fixture.spawn(fixture.targetType, 1, 210, 100);
    fixture.scene.intel.update(fixture.scene.store, fixture.scene.catalog,
                               fixture.scene.armies, nullptr);
    fixture.scene.publish(2);

    CHECK(fixture.targetId.index == stale.index);
    CHECK(fixture.targetId.generation != stale.generation);
    CHECK_FALSE(fixture.scene.visibleToViewer(stale));
    CHECK(fixture.scene.visibleToViewer(fixture.targetId));
}

TEST_CASE("republishing the same tick invalidates presentation contacts") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false};
    REQUIRE(fixture.scene.visibleToViewer(fixture.targetId));

    fixture.scene.store.transforms()[fixture.targetId.index].x = rm::sim::Fx::fromInt(900);
    fixture.scene.intel.update(fixture.scene.store, fixture.scene.catalog,
                               fixture.scene.armies, nullptr);
    fixture.scene.publish(1);  // headless march likewise republishes without advancing its tick

    CHECK_FALSE(fixture.scene.visibleToViewer(fixture.targetId));
}

TEST_CASE("an unset event unit is not mistaken for an unseen slot's zero marker") {
    FogFixture fixture{/*targetCloaked=*/false, /*targetFreeIntel=*/false};

    CHECK_FALSE(fixture.scene.visibleToViewer(rm::sim::UnitId{}));
}
