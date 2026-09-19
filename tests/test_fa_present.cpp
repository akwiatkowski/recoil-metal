// Player-perspective coverage for present claims (see docs/fa-exe-analysis-plan.md):
// WP-41/42's observable edges — pose as sim state (C-293), whether presentation can
// reach back into the sim (C-297), and the positional audio request wire (C-373).
//
// The claim texts describe retail internals (11 manipulator classes, SAudioRequest
// layout) that mostly have no counterpart here; what a player can observe is the
// boundary itself: aim state is sim-serialized so a replay aims identically,
// particles cannot move the sim's random stream, and a shot sounds where it
// happened. (WP-37's rows — wreck blocking, decal writes, water scorch — are
// covered in test_fa_terrain.cpp.)
#include "app/SceneBuild.hpp"
#include "core/audio/CueEvents.hpp"
#include "core/audio/Mixer.hpp"
#include "core/scene/Particles.hpp"
#include "core/sim/RandomStream.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"
#include "core/unit/UnitDef.hpp"
#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

TEST_CASE("C-293: turret pose is sim-serialized state, not a render cache",
          "[fa-present][pose]") {
    // The claim's observable half: manipulator state is sim-serialized, so a saved
    // match restores a turret mid-slew and a replay hashes it. Our pose is the
    // turret aim on `MoveState` — firing gates on it, which is what makes it sim
    // state rather than presentation (Movement.hpp:355).
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "posed_tank";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    roster.store.motion()[unit.index].turretYaw = 12345;
    roster.store.motion()[unit.index].turretPitch = 678;
    roster.store.motion()[unit.index].turretYaw2 = 22222;
    roster.store.motion()[unit.index].turretPitch2 = 333;
    roster.store.motion()[unit.index].turretMuzzlePhase = 1;

    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 7, .random = random.snapshot(), .units = roster.store.snapshot()});
    const auto saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    const rm::sim::UnitStore restored{saved->units};
    CHECK(restored.motion()[unit.index].turretYaw == 12345);
    CHECK(restored.motion()[unit.index].turretPitch == 678);
    CHECK(restored.motion()[unit.index].turretYaw2 == 22222);
    CHECK(restored.motion()[unit.index].turretPitch2 == 333);
    CHECK(restored.motion()[unit.index].turretMuzzlePhase == 1);

    // And it is hashed: two matches differing only in where a barrel points are
    // different matches — the property that keeps a desynced aim visible.
    rm::sim::UnitStore turned{roster.store.snapshot()};
    turned.motion()[unit.index].turretYaw = 999;
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    rm::sim::Match match{.armies = armies, .economies = economies};
    CHECK(rm::sim::hashMatch(roster.store, match) != rm::sim::hashMatch(turned, match));
}

TEST_CASE("C-297: particle emission draws no sim randomness",
          "[fa-present][particles]") {
    // C-297's property: effect code never draws the sim RNG, so a replay's hash
    // cannot depend on how much dust the renderer kicked up. Our emitters take
    // their own seed — the check is that the match's stream is untouched by a
    // second of emission. (The snapshot is a value copy of the MT19937 state, so
    // drawing from the copies compares the streams without advancing either.)
    rm::sim::RandomStream simRandom{std::uint32_t{42}};
    auto before = simRandom.snapshot();

    const rm::HeightField field = flatField();
    std::vector<rm::Particle> particles;
    const std::vector<rm::DustEmitter> emitters{
        {.position = {100.0f, 0.0f, 100.0f},
         .moving = true,
         .topSpeedElmosPerSecond = 40.0f,
         .radiusElmos = 10.0f},
    };
    float debt = 0.0f;
    std::uint32_t seed = 7;
    for (int frame = 0; frame < 60; ++frame) {
        rm::emitDust(particles, emitters, field, 1.0f / 60.0f, debt, seed);
    }
    REQUIRE_FALSE(particles.empty());  // emission happened — the stream still did not move
    auto after = simRandom.snapshot();
    CHECK(after() == before());
}

TEST_CASE("C-373: a positional audio request carries where the shot happened",
          "[fa-present][audio]") {
    // The claim's wire (C-373): sim-side `Entity::PlaySound` enqueues a positioned
    // request the user-side drains. Our channel is the event queue — `event.at` —
    // and the observable contract is that distance decides audibility: a shot
    // inside the listener's cutoff sounds, the same shot beyond it costs nothing.
    rm::audio::Mixer mixer;
    mixer.setListener(0.0f, 0.0f, 600.0f);
    mixer.setCutoffElmos(3000.0f);

    const std::vector<rm::sim::Event> events{
        {.kind = rm::sim::EventKind::WeaponFired,
         .at = rm::test::at(2900, 0, 0)},
        {.kind = rm::sim::EventKind::WeaponFired,
         .at = rm::test::at(3100, 0, 0)},
    };
    rm::audio::playForEvents(mixer, events);
    CHECK(mixer.activeVoices() == 1);  // the near shot sounds; the far one never starts
}

TEST_CASE("C-294/C-303: the manipulator list is precedence-sorted and bone visibility is serialized sim state",
          "[fa-present][manipulator]") {
    // C-294: `IAniManipulator::SetPrecedence` (`0x642610`) re-sorts the actor's
    // list (`0x6417b0`) — insertion order is not execution order. C-303:
    // `Unit::HideBone`/`ShowBone` write `CAniPoseBone+0x48` (`0x6d820b`), so the
    // mask is sim state: it saves, it loads, and it hashes.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "posed_bot";
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);

    // Inserted out of order on purpose: retail's foot-plant registers at
    // precedence 10 (`Unit.lua:2665`), the builder arm at 5.
    REQUIRE(roster.store.addManipulator(
        unit, {.kind = rm::sim::ManipulatorKind::Collision, .precedence = 10}));
    REQUIRE(roster.store.addManipulator(
        unit, {.kind = rm::sim::ManipulatorKind::StorageSlide, .precedence = 0}));
    REQUIRE(roster.store.addManipulator(
        unit, {.kind = rm::sim::ManipulatorKind::BuilderArm, .precedence = 5}));
    {
        const auto& list = roster.store.manipulators()[unit.index];
        REQUIRE(list.size() == 3);
        CHECK(list[0].kind == rm::sim::ManipulatorKind::StorageSlide);
        CHECK(list[1].kind == rm::sim::ManipulatorKind::BuilderArm);
        CHECK(list[2].kind == rm::sim::ManipulatorKind::Collision);
    }

    REQUIRE(roster.store.setBoneHidden(unit, 3, true));
    CHECK(roster.store.boneHiddenAt(unit.index, 3));
    CHECK_FALSE(roster.store.boneHiddenAt(unit.index, 0));

    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 7, .random = random.snapshot(), .units = roster.store.snapshot()});
    const auto saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    const rm::sim::UnitStore restored{saved->units};
    {
        const auto& list = restored.manipulators()[unit.index];
        REQUIRE(list.size() == 3);
        CHECK(list[0].kind == rm::sim::ManipulatorKind::StorageSlide);
        CHECK(list[1].kind == rm::sim::ManipulatorKind::BuilderArm);
        CHECK(list[2].kind == rm::sim::ManipulatorKind::Collision);
        CHECK(list[2].precedence == 10);
    }
    CHECK(restored.boneHiddenAt(unit.index, 3));
    CHECK_FALSE(restored.boneHiddenAt(unit.index, 0));

    // A hidden bone is a different match: the mask is hashed like the turret
    // pose above it, because a replay that cannot see the difference replays
    // the wrong unit.
    rm::sim::UnitStore shown{roster.store.snapshot()};
    REQUIRE(shown.setBoneHidden(unit, 3, false));
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    rm::sim::Match match{.armies = armies, .economies = economies};
    CHECK(rm::sim::hashMatch(roster.store, match) != rm::sim::hashMatch(shown, match));
}

TEST_CASE("C-304: the storage manipulator slides a bone by the stored fraction",
          "[fa-present][manipulator]") {
    // `CStorageManipulator` slides a bone as a function of stored
    // `U4EconResource` (`manip+0xa8`) — the pods that fill and empty with the
    // army's MASS/ENERGY. The observable half: the offset is deterministic
    // fixed point, follows the fraction, and serializes.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "storage_silo";
    // The capacity is AUTHORED, not set on the economy: `recomputeIncome`
    // rebuilds `storage` from standing units every tick, so a hand-set cap
    // would be wiped before the second reading.
    def.storageMass = rm::sim::Mag::fromInt(1000);
    const auto type = roster.addType(def);
    const auto unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(roster.store.addManipulator(unit,
        {.kind = rm::sim::ManipulatorKind::StorageSlide,
         .slideResource = 0,  // mass
         .slideBone = 2,
         .slideRange = rm::sim::Fx::fromInt(8)}));

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    economies[0].storage.mass = rm::sim::Mag::fromInt(1000);
    economies[0].stored.mass = rm::sim::Mag::fromInt(500);
    rm::sim::Match match{.armies = armies, .economies = economies};

    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 0);
    // Half full is half the travel: 8 elmos × 500/1000.
    CHECK(roster.store.manipulators()[unit.index][0].slideOffset
          == rm::sim::Fx::fromInt(4));

    // And it follows the fraction — draining the store slides the bone back.
    economies[0].stored.mass = rm::sim::Mag::fromInt(250);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 1);
    CHECK(roster.store.manipulators()[unit.index][0].slideOffset
          == rm::sim::Fx::fromInt(2));

    // The computed offset is serialized pose state (C-293): a save mid-slide
    // restores the bone where it was.
    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 7, .random = random.snapshot(), .units = roster.store.snapshot()});
    const auto saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    const rm::sim::UnitStore restored{saved->units};
    REQUIRE(restored.manipulators()[unit.index].size() == 1);
    CHECK(restored.manipulators()[unit.index][0].slideOffset
          == rm::sim::Fx::fromInt(2));
}

TEST_CASE("C-296/C-298: emitters are sim objects that spawn on fire and die with their unit",
          "[fa-present][effects]") {
    // `CEffectManagerImpl` is a `Sim` member (`+0x8C0`) ticked in `AdvanceBeat`
    // (C-296, `0x65fda0`), and `CreateEmitterAtBone` (`0x677800`) attaches
    // emitters to bones sim-side (C-298). The bounded slice: a shot spawns a
    // record at the muzzle bone, the record lives in the match's pool, and it
    // dies with its unit — the state retail serializes through `SerEffects`.
    rm::test::Roster roster;
    rm::unitdef::UnitDef gunDef;
    gunDef.name = "test_gunner";
    rm::unitdef::Weapon gun;
    gun.label = "test gun";
    gun.role = rm::unitdef::WeaponRole::DirectFire;
    gun.targetPriorities = {{"LAND"}};
    gun.turreted = true;
    gun.damage = rm::test::mag(10.0f);
    gun.maxRange = rm::test::fx(400.0f);
    gun.rateOfFire = 1.0f;
    gun.muzzleVelocityElmosPerSecond = 200.0f;
    gunDef.weapons.push_back(gun);
    rm::unitdef::UnitDef targetDef;
    targetDef.name = "test_target";
    targetDef.categories = {"LAND"};

    const auto gunner = roster.add(roster.addType(gunDef), 0.0f, 0.0f, 0, 500.0f);
    (void)roster.add(roster.addType(targetDef), 200.0f, 0.0f, 1, 500.0f);

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::SimEmitter> effects;
    rm::sim::EventQueue events;
    const std::vector<int> commandersEver(2, 0);
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &projectiles,
                         .events = &events,
                         .commandersEver = commandersEver,
                         .effects = &effects};

    events.beginFrame(0);
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                              roster.rate, 0);
    REQUIRE(report.shotsFired == 1);
    // One emitter per shot, attached at the muzzle bone, named by the effect id.
    REQUIRE(effects.size() == 1);
    CHECK(effects[0].unit == gunner);
    CHECK(effects[0].bone == rm::sim::kMuzzleBone);
    CHECK(effects[0].effect == "test_gunner:test gun");
    CHECK(effects[0].alive);
    CHECK_FALSE(effects[0].hidden);
    // The emission is reported with its viewer mask (C-374's wire).
    REQUIRE(events.count(rm::sim::EventKind::EffectEmitted) == 1);

    // `C-303`'s gate: hiding the bone the emitter rides suppresses it — the
    // record stays alive, the attachment point reports hidden.
    REQUIRE(roster.store.setBoneHidden(gunner, rm::sim::kMuzzleBone, true));
    events.beginFrame(1);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 1);
    REQUIRE_FALSE(effects.empty());
    CHECK(effects[0].hidden);

    // The pool is serialized sim state (C-296's `SerEffects`): a save carries
    // the live records, bone and effect id included.
    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 7, .random = random.snapshot(), .units = roster.store.snapshot(),
         .effects = effects});
    const auto saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    REQUIRE(saved->effects.has_value());
    REQUIRE(saved->effects->size() == effects.size());
    CHECK((*saved->effects)[0].unit == effects[0].unit);
    CHECK((*saved->effects)[0].bone == effects[0].bone);
    CHECK((*saved->effects)[0].effect == effects[0].effect);
    CHECK((*saved->effects)[0].hidden == effects[0].hidden);

    // And it dies with its unit: the sweep retires the record the same tick
    // the corpse is reported, so a recycled slot never inherits the flash.
    roster.store.health()[gunner.index].current = rm::sim::Mag{};
    events.beginFrame(2);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 2);
    CHECK(effects.empty());
}

TEST_CASE("C-301/C-372: the collision manipulator fires contact events on transitions",
          "[fa-present][manipulator]") {
    // `CCollisionManipulator::ManipulatorUpdate` (`0x63e760`) fires
    // `OnAnimTerrainCollision`/`OnNotAnimTerrainCollision`/`OnAnimCollision`
    // (`0xe71308`–`0xe71320`) when the animated volume touches terrain or
    // another unit. With no skeleton, the bounded slice's honest triggers are
    // the surface-resting flag and the collision pass's own overlap test —
    // fired on the transitions the serialized latches record.
    rm::test::Roster roster;
    rm::unitdef::UnitDef def;
    def.name = "walker";
    const auto type = roster.addType(def);
    const auto walker = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    (void)roster.add(type, 43.0f, 40.0f, 0, 100.0f);  // overlapping bystander
    REQUIRE(roster.store.addManipulator(
        walker, {.kind = rm::sim::ManipulatorKind::Collision}));

    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(1);
    std::vector<rm::sim::Economy> economies(1);
    rm::sim::EventQueue events;
    const std::vector<int> commandersEver(1, 0);
    rm::sim::Match match{.armies = armies, .economies = economies,
                         .events = &events, .commandersEver = commandersEver};

    // First contact: a walker rests on the ground and overlaps its neighbour,
    // so both edges fire once — and only once, because the latches serialize.
    events.beginFrame(0);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 0);
    CHECK(events.count(rm::sim::EventKind::AnimTerrainCollision) == 1);
    CHECK(events.count(rm::sim::EventKind::AnimCollision) == 1);

    // Steady state: still touching, nothing re-fires.
    events.beginFrame(1);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 1);
    CHECK(events.count(rm::sim::EventKind::AnimTerrainCollision) == 0);
    CHECK(events.count(rm::sim::EventKind::AnimCollision) == 0);

    // The leaving edge: lifting the walker onto a carrier ends BOTH contacts —
    // cargo rides its carrier, not the ground, and the rack is not a volume.
    const auto carrier = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    REQUIRE(roster.store.attach(carrier, walker));
    events.beginFrame(2);
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain,
                                roster.rate, 2);
    CHECK(events.count(rm::sim::EventKind::AnimTerrainCollisionEnd) == 1);

    // The latches are serialized pose state: a save mid-contact restores them,
    // so a restored match does not re-fire the entry callbacks.
    rm::sim::RandomStream random{std::uint32_t{1}};
    const std::vector<std::byte> bytes = rm::sim::SaveState::encode(
        {.tick = 7, .random = random.snapshot(), .units = roster.store.snapshot()});
    const auto saved = rm::sim::SaveState::decode(bytes);
    REQUIRE(saved.has_value());
    const rm::sim::UnitStore restored{saved->units};
    REQUIRE(restored.manipulators()[walker.index].size() == 1);
    CHECK_FALSE(restored.manipulators()[walker.index][0].inTerrainContact);
}
