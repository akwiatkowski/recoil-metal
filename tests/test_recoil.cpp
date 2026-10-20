// Recoil slides: a barrel kicks back along its aim axis when its weapon fires,
// then runs home at the authored return speed.
//
// The rig is one subtree flag per bone (the recoil bone and everything bolted to
// it); the motion is one scalar per instance (0 at rest, 1 fully kicked) that the
// shader multiplies by the batch's travel distance. Kick on WeaponFired, decay by
// return speed — both caller-side, both deterministic.
#include "core/model/BuilderAim.hpp"

#include "app/Match.hpp"
#include "core/data/MoveDef.hpp"
#include "core/map/HeightField.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/FxMatchers.hpp"
#include "app/SceneBuild.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/vfs/Vfs.hpp"

#include <cstdlib>
#include <filesystem>

using Catch::Approx;
#include <cstdint>

namespace {

[[nodiscard]] rm::Model gunModel() {
    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {
        rm::ModelBone{.name = "Hull", .parent = -1},
        rm::ModelBone{.name = "Turret", .parent = 0, .offset = {{0.0f, 1.0f, 0.0f}},
                      .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Barrel", .parent = 1, .globalOffset = {{0.0f, 1.0f, 0.0f}}},
        rm::ModelBone{.name = "Muzzle", .parent = 2, .offset = {{0.0f, 0.0f, 3.0f}},
                      .globalOffset = {{0.0f, 1.0f, 3.0f}}},
        rm::ModelBone{.name = "Rack", .parent = 2, .offset = {{0.5f, 0.0f, 1.0f}},
                      .globalOffset = {{0.5f, 1.0f, 1.0f}}},
        rm::ModelBone{.name = "Tele", .parent = 4, .offset = {{0.0f, 0.0f, 0.5f}},
                      .globalOffset = {{0.5f, 1.0f, 1.5f}}},
    };
    return model;
}

} // namespace

TEST_CASE("a recoil rig marks only the rack subtree", "[recoil]") {
    const std::vector<std::uint32_t> flags = rm::resolveRecoilFlags(gunModel(), "Rack");
    REQUIRE(flags.size() == 6);
    CHECK(flags[0] == 0U);
    CHECK(flags[1] == 0U);
    CHECK(flags[2] == 0U);
    CHECK(flags[3] == 0U);
    CHECK(flags[4] == rm::kBuilderRecoilBone);
    // The telescope descends from the rack: it rides the rack's slide AND
    // carries its own channel — retail's two stacked manipulators.
    CHECK(flags[5] == rm::kBuilderRecoilBone);
}

TEST_CASE("recoil bone names match case-insensitively", "[recoil]") {
    CHECK(rm::resolveRecoilFlags(gunModel(), "NoSuchBone").empty());
    rm::Model model = gunModel();
    model.bones[4].name = "rack";
    const std::vector<std::uint32_t> flags = rm::resolveRecoilFlags(model, "Rack");
    REQUIRE(flags.size() == 6);
    CHECK(flags[4] == rm::kBuilderRecoilBone);
}

TEST_CASE("recoil decays linearly home and never goes negative", "[recoil]") {
    // A quarter of the travel per step: four steps home from a full kick.
    CHECK(rm::stepRecoil(1.0f, 0.25f) == Approx(0.75f));
    CHECK(rm::stepRecoil(0.75f, 0.25f) == Approx(0.5f));
    CHECK(rm::stepRecoil(0.1f, 0.25f) == 0.0f);
    CHECK(rm::stepRecoil(0.0f, 0.25f) == 0.0f);
}

TEST_CASE("a telescope rig marks its own subtree with its own bit", "[recoil]") {
    // The telescope is a second CSlideManipulator in retail, not a second
    // distance on the rack's — its subtree carries bit 7 beside the rack's
    // bit 2, and a bone inside both subtrees slides by the sum.
    const std::vector<std::uint32_t> flags =
        rm::resolveTelescopeFlags(gunModel(), "Rack");
    REQUIRE(flags.size() == 6);
    CHECK(flags[4] == rm::kBuilderTelescopeBone);
    CHECK(flags[5] == rm::kBuilderTelescopeBone);
    CHECK(rm::resolveTelescopeFlags(gunModel(), "NoSuchBone").empty());
    CHECK(rm::resolveTelescopeFlags(gunModel(), "").empty());
}

TEST_CASE("the default return speed finishes the slide inside the firing interval",
          "[recoil]") {
    // defaultweapons.lua:62 — only 6 of ~206 recoil weapons author
    // RackRecoilReturnSpeed; the rest ride
    //   abs(dist / ((1/RateOfFire) - MuzzleChargeDelay)) * 1.25
    // "so that it finishes returning just as the next shot is ready".
    //
    // UEL0201's Striker: dist 2 mesh, RoF 1, no charge delay → 2.5 mesh/s,
    // which is 1.25 of the 2-mesh travel per second → 0.125 of travel per
    // 10 Hz tick (home in 8 ticks, 80% of the 1 s interval).
    rm::unitdef::Weapon striker;
    striker.recoilDistanceMesh = -2.0f;
    striker.rateOfFire = 1.0f;
    const rm::RecoilSpec strikerSpec =
        rm::resolveRecoilSpec(striker, 1.0f, 10.0f);
    CHECK(strikerSpec.rackDistanceElmos == Approx(-2.0f));  // SIGNED, as authored
    CHECK(strikerSpec.rackReturnPerTick == Approx(0.125f));
    CHECK(strikerSpec.telescopeDistanceElmos == 0.0f);      // no telescope bone

    // XSL0111's missile rack: dist 1, RoF 0.15, charge delay 0.1 →
    // 1/(6.667-0.1)*1.25 = 0.1903 mesh/s → 0.019 of travel per tick.
    rm::unitdef::Weapon rack;
    rack.recoilDistanceMesh = -1.0f;
    rack.rateOfFire = 0.15f;
    rack.muzzleChargeDelaySeconds = 0.1f;
    const rm::RecoilSpec rackSpec = rm::resolveRecoilSpec(rack, 1.0f, 10.0f);
    CHECK(rackSpec.rackReturnPerTick == Approx(0.019034f).epsilon(0.001));

    // An authored speed wins over the formula (the 6 weapons that state one).
    rm::unitdef::Weapon authored = striker;
    authored.recoilReturnSpeedMeshPerSecond = 10.0f;
    CHECK(rm::resolveRecoilSpec(authored, 1.0f, 10.0f).rackReturnPerTick
          == Approx(0.5f));  // 10 mesh/s over 2 mesh of travel, per 10 Hz tick

    // Lua edge cases: RateOfFire 0 divides by zero and yields speed 0 — the
    // slide stays kicked forever; a charge delay past the interval yields inf
    // — the slide snaps home in one step.
    rm::unitdef::Weapon frozen = striker;
    frozen.rateOfFire = 0.0f;
    CHECK(rm::resolveRecoilSpec(frozen, 1.0f, 10.0f).rackReturnPerTick == 0.0f);
    rm::unitdef::Weapon instant = striker;
    instant.muzzleChargeDelaySeconds = 1.0f;  // window 0 → inf speed
    CHECK(rm::resolveRecoilSpec(instant, 1.0f, 10.0f).rackReturnPerTick == 1.0f);
}

TEST_CASE("the telescope channel takes the rack distance when unstated", "[recoil]") {
    // `v.TelescopeRecoilDistance or bp.RackRecoilDistance` (defaultweapons.lua:283):
    // absent falls back to the rack's goal; authored stands on its own — and
    // both channels run home at the SAME speed, so the longer travel takes
    // proportionally longer per tick.
    rm::unitdef::Weapon gun;
    gun.recoilDistanceMesh = -15.0f;
    gun.rateOfFire = 0.1f;
    gun.telescopeBone = "Barrel_B02";
    const rm::RecoilSpec fallback = rm::resolveRecoilSpec(gun, 1.0f, 10.0f);
    CHECK(fallback.telescopeDistanceElmos == Approx(-15.0f));
    CHECK(fallback.telescopeReturnPerTick == Approx(fallback.rackReturnPerTick));

    gun.telescopeDistanceMesh = -20.0f;  // UEB2302's authored telescope
    const rm::RecoilSpec spec = rm::resolveRecoilSpec(gun, 1.0f, 10.0f);
    CHECK(spec.telescopeDistanceElmos == Approx(-20.0f));
    // The formula uses the LARGER travel: dist = max(15,20) = 20 →
    // 20/(10-0)*1.25 = 2.5 mesh/s for BOTH sliders. Per tick: rack returns
    // 2.5/15/10 of its travel, the telescope 2.5/20/10 — the longer travel
    // takes proportionally longer, exactly like retail's shared SetSpeed.
    CHECK(spec.rackReturnPerTick == Approx(0.016667f).epsilon(0.001));
    CHECK(spec.telescopeReturnPerTick == Approx(0.0125f));
}

TEST_CASE("a positive recoil distance keeps its sign — the slide goes forwards",
          "[recoil]") {
    // UEL0203 authors +0.1: the sign is data, not a convention. The old
    // magnitude-only path would have kicked it backwards like every other gun.
    rm::unitdef::Weapon gun;
    gun.recoilDistanceMesh = 0.1f;
    gun.rateOfFire = 3.0f;
    const rm::RecoilSpec spec = rm::resolveRecoilSpec(gun, 0.56f, 10.0f);
    CHECK(spec.rackDistanceElmos == Approx(0.056f));
    CHECK(spec.rackDistanceElmos > 0.0f);
}

TEST_CASE("a fired gun draws its barrel back, then runs it home", "[recoil]") {
    // End to end through the match: two enemies in range, the shots kick the
    // slide to full and the ticks run it home. Fully synthetic like the turret
    // scene test — the batch carries a resolved rack, the sim fires the gun.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});

    rm::unitdef::UnitDef tank;
    tank.name = "test_recoil_tank";
    tank.motion = rm::unitdef::MotionType::Land;
    tank.speedElmosPerSecond = 10.0f;
    tank.health = rm::sim::Mag::fromInt(100);
    tank.categories = {"LAND"};
    rm::unitdef::Weapon gun;
    gun.label = "test recoil gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"LAND"}};
    gun.damage = rm::test::mag(10.0f);
    gun.maxRange = rm::test::fx(200.0f);
    gun.rateOfFire = 1.0f;
    gun.muzzleVelocityElmosPerSecond = 100.0f;
    gun.turreted = true;
    gun.turretYawBone = "Turret";
    gun.turretPitchBone = "Barrel";
    gun.muzzleBone = "Muzzle";
    gun.turretYawSpeedRadPerSecond = 2.0f;
    gun.turretPitchSpeedRadPerSecond = 1.5f;
    gun.recoilBone = "Rack";
    gun.recoilDistanceMesh = -2.0f;   // signed, as the corpus authors it
    gun.recoilReturnSpeedMeshPerSecond = 2.0f;
    gun.telescopeBone = "Tele";
    gun.telescopeDistanceMesh = -4.0f;
    tank.weapons.push_back(gun);
    scene.definitions.push_back(tank);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(tank), 1.0f);

    scene.models.push_back(gunModel());
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .turretAim = rm::resolveTurretAim(scene.models.back(),
                                          rm::TurretAimSpec{
                                              .yawBone = {.name = "Turret"},
                                              .pitchBone = {.name = "Barrel"},
                                              .muzzleBone = {.name = "Muzzle"},
                                              .yawSlew = 2.0f,
                                              .pitchSlew = 1.5f,
                                          }),
        .turretWeapon = 0,
        .recoilFlags = [&] {
            // Rack AND telescope channels, OR'd like resolveTurretRig does.
            std::vector<std::uint32_t> flags =
                rm::resolveRecoilFlags(scene.models.back(), "Rack");
            const std::vector<std::uint32_t> tele =
                rm::resolveTelescopeFlags(scene.models.back(), "Tele");
            for (std::size_t bone = 0; bone < tele.size(); ++bone) {
                flags[bone] |= tele[bone];
            }
            return flags;
        }(),
        .recoilDistanceElmos = -2.0f,   // signed, as authored
        .recoilReturnPerTick = 0.1f,
        .telescopeDistanceElmos = -4.0f,
        .telescopeReturnPerTick = 0.05f,  // same speed, twice the travel
    });
    REQUIRE(!scene.batches.back().recoilFlags.empty());
    scene.setBatchForType(type, 0);

    (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(0.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(tank, 0),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });
    (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(50.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(tank, 1),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });

    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();
    // Sample the slide EVERY tick: retail's law is kick-to-full on the fire
    // tick (`SetSpeed(-1)`), one tick held (`WaitTicks(1)`), then the run home
    // — the decay-then-kick order in advanceMatch is what makes a slide
    // kicked this tick read exactly 1.0 before it starts back.
    float firstKick = -1.0f;
    float afterHold = -1.0f;
    float teleKick = -1.0f;
    for (int tick = 0; tick < 8; ++tick) {
        (void)rm::app::advanceMatch(runner, tick, 0.0f);
        for (const auto& kv : scene.recoilShown) {
            const rm::RecoilSlide& slide = kv.second;
            if (slide.rack > 0.0f && firstKick < 0.0f) {
                firstKick = slide.rack;
                teleKick = slide.telescope;
            } else if (firstKick >= 0.0f && afterHold < 0.0f
                       && slide.rack < firstKick) {
                afterHold = slide.rack;
            }
        }
    }
    REQUIRE(runner.shotsFired > 0);
    // The instant kick: full travel the tick the shot leaves, both channels.
    CHECK(firstKick == Approx(1.0f));
    CHECK(teleKick == Approx(1.0f));
    // The run home at the authored rate: 0.1 of travel per tick, so the first
    // decayed sample reads 0.9 — the one-tick hold is the 1.0 above surviving
    // its own tick's decay pass.
    CHECK(afterHold == Approx(0.9f));

    scene.publish(7);
    scene.publish(7);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 2);
    // Kicked but not yet home: a shot left within the last second and the
    // return takes a full one. The telescope channel rides beside the rack's.
    bool kicked = false;
    bool teleKicked = false;
    for (const rm::UnitInstance& instance : scene.batches[0].instances) {
        if (instance.recoil > 0.0f) {
            kicked = true;
        }
        if (instance.recoilTelescope > 0.0f) {
            teleKicked = true;
        }
    }
    CHECK(kicked);
    CHECK(teleKicked);
}
TEST_CASE("a fired rack plays its authored reload clip", "[recoil]") {
    // `defaultweapons.lua:202-210` (`PlayFxRackSalvoReloadSequence`): a weapon
    // whose blueprint names `AnimationReload` plays that clip on the unit's
    // animator when the rack salvo reloads. The corpus proves the shape — 2
    // shipped weapons name one (`XSL0111`, `XEL0306`) — and the parse already
    // lands in `Weapon::animationReload`; the draw path must load it the way
    // it loads the walk and unpack clips.
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        home ? std::filesystem::path{home} / "projects/llm/input/faf" : std::filesystem::path{};
    if (!std::filesystem::is_directory(root / "units/XSL0111")) SKIP("retail corpus unavailable");
    rm::vfs::Vfs content;
    content.mountDirectory(root);
    rm::app::UnitScene scene;
    const auto launcher = rm::unitbp::loadFile(root / "units/XSL0111/XSL0111_unit.bp");
    REQUIRE(launcher.has_value());
    REQUIRE(!launcher->weapons.empty());
    const rm::sca::Animation* resolved =
        rm::app::loadReloadAnimation(scene, content, *launcher);
    REQUIRE(resolved != nullptr);
    // The Laanse rack's reload is a real clip: nonzero duration, nonzero bones.
    CHECK(resolved->duration > 0.0f);
    CHECK(!resolved->boneNames.empty());
    // A gun with no authored reload draws with none.
    const rm::unitdef::UnitDef plain{};
    CHECK(rm::app::loadReloadAnimation(scene, content, plain) == nullptr);
}
TEST_CASE("registering the Laanse wires its reload clip into the batch", "[recoil]") {
    // `loadReloadAnimation` exists and loads (the case above), but no spawn
    // path ever calls it — unpack and walk ride every batch, the reload clip
    // rides none. `ensureDrawableType` must wire it the way it wires unpack.
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        home ? std::filesystem::path{home} / "projects/llm/input/faf" : std::filesystem::path{};
    if (!std::filesystem::is_directory(root / "units/XSL0111")) SKIP("retail corpus unavailable");
    rm::vfs::Vfs content;
    content.mountDirectory(root);
    rm::app::UnitScene scene;
    const auto type = rm::app::ensureDrawableType(scene, content, "/units/XSL0111/XSL0111_unit.bp");
    REQUIRE(type.has_value());
    const std::size_t batch = scene.batchOf(*type);
    REQUIRE(batch != rm::app::UnitScene::kNoBatch);
    // The Laanse names `AnimationReload` on its cannon; the batch must carry it.
    REQUIRE(scene.batches[batch].reloadAnimation != nullptr);
    CHECK(scene.batches[batch].reloadAnimation->duration > 0.0f);
}
TEST_CASE("a --units crowd of Laanses keeps the reload clip", "[recoil]") {
    // The third spawn path: `spawnCommanders` and `ensureDrawableType` both
    // call `loadReloadAnimation`, but the `--units` crowd loop in
    // `resolveUnits` wires unpack and never reload — the Laanse crowd draws
    // with no clip. Same corpus proof as the registering case above.
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        home ? std::filesystem::path{home} / "projects/llm/input/faf" : std::filesystem::path{};
    if (!std::filesystem::is_directory(root / "units/XSL0111")) SKIP("retail corpus unavailable");
    rm::vfs::Vfs content;
    content.mountDirectory(root);
    rm::vfs::AssetSearch search;
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    const rm::app::UnitOptions request{.modelPath = "/units/XSL0111/XSL0111_unit.bp",
                                       .count = 1};
    rm::app::UnitScene scene = rm::app::resolveUnits(
        std::span<const rm::app::UnitOptions>{&request, 1}, field, {}, false, 0.0f,
        search, content);
    REQUIRE(!scene.batches.empty());
    REQUIRE(scene.batches[0].reloadAnimation != nullptr);
    CHECK(scene.batches[0].reloadAnimation->duration > 0.0f);
}




TEST_CASE("a fired rack stamps its reload clip from the fire tick", "[recoil]") {
    // The stored-but-never-played half of the Laanse slice: `ensureDrawableType`
    // wires `reloadAnimation` into the batch, but no gather path ever stamps
    // it — the clip rides no pose. A fire tick must start the clip's clock on
    // the slot, and gather must stamp the clip's phase, one-shot like deploy.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});

    rm::unitdef::UnitDef launcher;
    launcher.name = "test_reload_rack";
    launcher.motion = rm::unitdef::MotionType::Land;
    launcher.health = rm::sim::Mag::fromInt(100);
    launcher.categories = {"LAND"};
    scene.definitions.push_back(launcher);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(launcher), 1.0f);

    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {rm::ModelBone{.name = "Root", .parent = -1}};
    scene.models.push_back(std::move(model));
    scene.animations.push_back(rm::sca::Animation{
        .name = "test_reload",
        .duration = 2.0f,
        .boneNames = {"Root"},
        .boneParents = {-1},
        .frames = {rm::sca::Frame{.time = 0.0f, .bones = {rm::sca::Key{}}},
                   rm::sca::Frame{.time = 2.0f, .bones = {rm::sca::Key{}}}},
    });
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .reloadAnimation = &scene.animations.back(),
    });
    scene.setBatchForType(type, 0);

    const rm::sim::UnitId first = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(0.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(launcher, 0),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });
    // The match half: a WeaponFired inside the tick's own frame starts the
    // clock. Emitting BEFORE the tick dies in `beginFrame`, so tick once to
    // open the frame, emit into it, and tick the SAME tick again — the
    // second `beginFrame` is a no-op and the consumer reads the emit.
    rm::app::PassabilitySet passability{field, false, 0.0f};
    rm::vfs::Vfs content;
    rm::app::MatchRunner runner =
        rm::app::makeMatchRunner(scene, field, passability, content, {}, {});
    runner.scripts.clear();
    (void)rm::app::advanceMatch(runner, 99, 9.9f);
    scene.events.emit(rm::sim::Event{
        .kind = rm::sim::EventKind::WeaponFired,
        .unit = first,
        .army = 0,
    });
    (void)rm::app::advanceMatch(runner, 99, 9.9f);
    REQUIRE(scene.reloadTick.contains(first.index));
    CHECK(scene.reloadTick[first.index] == 99);
    scene.publish(109);
    scene.publish(109);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 1);
    // Ten ticks at ten hertz into a two-second reload: half reloaded.
    CHECK(scene.batches[0].instances[0].animationPhase == Approx(0.5f));
}

TEST_CASE("a built unit plays its unfold once from completion", "[recoil]") {
    // The deploy path end to end with a hand-made fold: two frames over two
    // seconds, batch flagged one-shot, deployed ten ticks ago. Gather must
    // stamp half a cycle — and a unit built later starts later, not over.
    rm::HeightField field;
    field.squaresX = 128;
    field.squaresZ = 128;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    rm::app::UnitScene scene;
    scene.armies = rm::sim::freeForAll(2);
    scene.players = rm::sim::onePlayerPerArmy(2, 0);
    scene.economies.assign(2, rm::sim::Economy{});

    rm::unitdef::UnitDef hut;
    hut.name = "test_hut";
    hut.motion = rm::unitdef::MotionType::Land;
    hut.health = rm::sim::Mag::fromInt(100);
    hut.categories = {"LAND"};
    scene.definitions.push_back(hut);
    const rm::UnitTypeIndex type =
        scene.catalog.add(&scene.definitions.back(), rm::app::gAppTickRate);
    scene.setTypeTraits(type, rm::data::moveDefFor(hut), 1.0f);

    rm::Model model;
    model.family = rm::Family::SupremeCommander;
    model.bones = {rm::ModelBone{.name = "Root", .parent = -1}};
    scene.models.push_back(std::move(model));
    scene.animations.push_back(rm::sca::Animation{
        .name = "test_unfold",
        .duration = 2.0f,
        .boneNames = {"Root"},
        .boneParents = {-1},
        .frames = {rm::sca::Frame{.time = 0.0f, .bones = {rm::sca::Key{}}},
                   rm::sca::Frame{.time = 2.0f, .bones = {rm::sca::Key{}}}},
    });
    scene.batches.push_back(rm::UnitBatch{
        .model = &scene.models.back(),
        .unpackAnimation = &scene.animations.back(),
        .unpackOneshot = true,
    });
    scene.setBatchForType(type, 0);

    const rm::sim::UnitId first = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = {.x = rm::sim::fxFromFloat(0.0f), .z = rm::sim::fxFromFloat(0.0f)},
        .motion = rm::app::motionFor(hut, 0),
        .health = rm::sim::initialHealth(rm::sim::Mag::fromInt(100)),
    });
    scene.deployedTick[first.index] = 100;
    scene.publish(110);
    scene.publish(110);
    scene.gatherForDrawing(1.0f, nullptr, {}, 0.0f);
    REQUIRE(scene.batches[0].instances.size() == 1);
    // Ten ticks at ten hertz into a two-second fold: half unfolded.
    CHECK(scene.batches[0].instances[0].animationPhase == Approx(0.5f));
}
