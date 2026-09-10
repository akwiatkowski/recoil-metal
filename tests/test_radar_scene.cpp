// Headless radar scene: two sides, a radar in range of the enemy commander.
//
// What the player sees on the minimap, without a window: with radar coverage the
// enemy commander plots as a blip near (never exactly at) its position; without
// coverage nothing plots at all. Then a T2-style artillery either opens fire on
// the radar contact or stays silent when there is none.
#include "app/Interface.hpp"
#include "app/Match.hpp"

#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/FxMatchers.hpp"

using Catch::Approx;

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

[[nodiscard]] rm::unitdef::UnitDef radarTower() {
    rm::unitdef::UnitDef def;
    def.name = "test_radar";
    def.motion = rm::unitdef::MotionType::Land;
    def.visionRadiusElmos = 0.0f;  // blind: only the dish sees
    def.radarRadiusElmos = 400.0f;
    def.categories = {"LAND", "STRUCTURE"};
    def.health = rm::sim::Mag::fromInt(500);
    def.collisionRadiusElmos = 2.0f;
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef enemyCommander() {
    rm::unitdef::UnitDef def;
    def.name = "test_commander";
    def.motion = rm::unitdef::MotionType::Land;
    def.speedElmosPerSecond = 10.0f;
    def.visionRadiusElmos = 0.0f;
    def.health = rm::sim::Mag::fromInt(1000);
    // Sorted: hasCategory binary-searches, and production blueprints arrive sorted.
    def.categories = {"COMMAND", "LAND"};
    def.collisionRadiusElmos = 2.0f;
    return def;
}

[[nodiscard]] rm::unitdef::UnitDef artillery(bool withRadar) {
    rm::unitdef::UnitDef def;
    def.name = "test_arty";
    def.motion = rm::unitdef::MotionType::Land;
    def.visionRadiusElmos = 0.0f;  // blind: fires on radar contacts or not at all
    def.categories = {"ARTILLERY", "LAND", "STRUCTURE"};
    def.health = rm::sim::Mag::fromInt(500);
    def.collisionRadiusElmos = 2.0f;
    if (withRadar) {
        def.radarRadiusElmos = 400.0f;
    }
    rm::unitdef::Weapon gun;
    gun.label = "test arty gun";
    gun.role = rm::unitdef::WeaponRole::Artillery;
    gun.targetPriorities = {{"LAND"}};
    gun.damage = rm::test::mag(50.0f);
    gun.maxRange = rm::test::fx(350.0f);
    gun.rateOfFire = 1.0f;
    gun.muzzleVelocityElmosPerSecond = 100.0f;
    gun.arc = rm::unitdef::BallisticArc::High;
    // Turreted like the real thing (UEB2302): static guns cannot turn their hull,
    // so an unturreted one would wait on a facing gate it can never satisfy.
    gun.turreted = true;
    gun.turretYawBone = "Turret";
    gun.turretPitchBone = "Barrel";
    gun.muzzleBone = "Muzzle";
    gun.turretYawSpeedRadPerSecond = 2.0f;
    gun.turretPitchSpeedRadPerSecond = 1.5f;
    def.weapons.push_back(gun);
    return def;
}

struct RadarScene {
    rm::HeightField field = flatField();
    rm::app::UnitScene scene;
    rm::sim::UnitId enemy{};
    rm::sim::UnitId gun{};
    std::size_t shotsFired = 0;
};

/// A 1v1 at 250 elmos separation: inside radar (400) and artillery (350) reach,
/// outside every vision radius (all zero). `withRadar` decides whether army 0
/// fields a dish.
[[nodiscard]] RadarScene setup(bool withRadar, bool withArty) {
    RadarScene job;
    job.scene.armies = rm::sim::freeForAll(2);
    job.scene.players = rm::sim::onePlayerPerArmy(2, 0);
    job.scene.playerArmy = 0;
    job.scene.economies.assign(2, rm::sim::Economy{});
    job.scene.intel.configure(2, rm::sim::fxFromFloat(job.field.widthElmos()),
                              rm::sim::fxFromFloat(job.field.depthElmos()),
                              rm::sim::VisionStyle::ForgedAlliance);

    auto add = [&](rm::unitdef::UnitDef def) {
        job.scene.definitions.push_back(def);
        const rm::UnitTypeIndex type = job.scene.catalog.add(
            &job.scene.definitions.back(), rm::app::gAppTickRate);
        job.scene.setTypeTraits(type, rm::data::moveDefFor(def), 1.0f);
        return std::make_pair(type, def);
    };
    const auto [radarType, radarDef] = add(radarTower());
    const auto [foeType, foeDef] = add(enemyCommander());
    const auto [artyType, artyDef] = add(artillery(withRadar));

    auto spawn = [&](rm::UnitTypeIndex type, const rm::unitdef::UnitDef& def, int army,
                     float x, float z) {
        return job.scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = rm::app::motionFor(def, army),
            .health = rm::sim::initialHealth(def.health),
        });
    };
    if (withRadar) {
        (void)spawn(radarType, radarDef, 0, 0.0f, 0.0f);
    }
    if (withArty) {
        job.gun = spawn(artyType, artyDef, 0, 20.0f, 0.0f);
    }
    job.enemy = spawn(foeType, foeDef, 1, 250.0f, 0.0f);
    rm::app::PassabilitySet passability{job.field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner = rm::app::makeMatchRunner(
        job.scene, job.field, passability, content, {}, {});
    runner.scripts.clear();
    for (int tick = 0; tick < 120; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
    }
    job.shotsFired = runner.shotsFired;
    job.scene.publish(119);
    job.scene.publish(119);
    return job;
}

[[nodiscard]] bool hasBlipNear(const std::vector<rm::ui::MinimapPip>& pips, float x, float z) {
    // Blip colour, not team colour: radar reports a position without an identity.
    for (const auto& pip : pips) {
        const float dx = pip.worldX - x;
        const float dz = pip.worldZ - z;
        if (dx * dx + dz * dz < 100.0f * 100.0f && pip.size < 3.0f) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("radar coverage plots the enemy commander as a red blip", "[radar]") {
    RadarScene job = setup(/*withRadar=*/true, /*withArty=*/false);
    std::vector<rm::ui::MinimapPip> pips;
    rm::app::appendMinimapPips(pips, job.scene);
    CHECK(hasBlipNear(pips, 250.0f, 0.0f));
    // Red, not khaki and not a team colour: every blip is hostile (allies are
    // Seen exactly), so the dot says enemy and nothing else.
    bool red = false;
    for (const auto& pip : pips) {
        const float dx = pip.worldX - 250.0f;
        const float dz = pip.worldZ - 0.0f;
        if (dx * dx + dz * dz < 100.0f * 100.0f && pip.size < 3.0f) {
            CHECK(pip.colour[0] > 0.8f);
            CHECK(pip.colour[1] < 0.4f);
            CHECK(pip.colour[2] < 0.4f);
            red = true;
        }
    }
    CHECK(red);
}

TEST_CASE("without radar nothing plots the enemy commander", "[radar]") {
    RadarScene job = setup(/*withRadar=*/false, /*withArty=*/false);
    std::vector<rm::ui::MinimapPip> pips;
    rm::app::appendMinimapPips(pips, job.scene);
    CHECK_FALSE(hasBlipNear(pips, 250.0f, 0.0f));
}

TEST_CASE("artillery opens fire on a radar contact, silent without one", "[radar]") {
    // Firing, not hitting: shells fly at the blip, and the blip is up to 96
    // elmos off the truth — so hits are luck and shots are the signal.
    RadarScene covered = setup(/*withRadar=*/true, /*withArty=*/true);
    CHECK(covered.shotsFired > 0);

    RadarScene blind = setup(/*withRadar=*/false, /*withArty=*/true);
    CHECK(blind.shotsFired == 0);
}

TEST_CASE("a radar contact resolves to the live enemy for the muzzle", "[radar]") {
    RadarScene job = setup(/*withRadar=*/true, /*withArty=*/true);
    const auto slot = static_cast<rm::UnitIndex>(job.gun.index);
    const auto* def = job.scene.catalog.def(job.scene.store.typeAt(slot));
    REQUIRE(def != nullptr);
    REQUIRE(def->weapons.size() == 1);
    const auto& gun = def->weapons[0];
    REQUIRE(gun.fires());
    const auto from = std::array<rm::sim::Fx, 3>{rm::sim::fxFromFloat(20.0f), {}, {}};
    CHECK(rm::sim::nearestTarget(from, 0, gun, job.scene.store, job.scene.armies,
                                  &job.scene.intel, &job.scene.catalog)
          == job.enemy);
}
