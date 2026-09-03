#include "core/sim/StateHash.hpp"

#include "core/map/HeightField.hpp"
#include "core/sim/Intel.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "support/FxMatchers.hpp"

using rm::sim::hashMatch;

namespace {

/// The ordinary default motion for an army. `sim::defaultMotion` plus an owner.
[[nodiscard]] rm::sim::MoveState defaultMotionFor(int army) {
    rm::sim::MoveState motion = rm::sim::defaultMotion(rm::sim::TickRate{});
    motion.armyIndex = army;
    return motion;
}

// A flat field, which is all any of this needs: the hash reads state, and none of these
// cases care what the ground looks like.
[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

// A non-flat field makes a stale post-propagation Y coordinate observable.
[[nodiscard]] rm::HeightField rampField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.resize(field.sampleCount());
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            const auto index = static_cast<std::size_t>(z)
                                   * static_cast<std::size_t>(field.verticesX())
                               + static_cast<std::size_t>(x);
            field.raw[index] = static_cast<std::uint16_t>(x * 10);
        }
    }
    return field;
}

// The smallest thing that is a match: one unit, one army, one economy.
//
// The unit lives in a `UnitStore` rather than in three parallel vectors, which is what the
// sim now takes. The three accessors below are spans INTO the store, so a test can still
// reach in and change one field — the point of most of these cases.
struct Fixture {
    rm::sim::UnitStore store;
    rm::sim::UnitCatalog catalog;
    std::vector<rm::sim::Army> armies;
    std::vector<rm::sim::Economy> economies;
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::Construction> building;
    std::vector<rm::sim::SiloAmmo> siloAmmo;
    std::vector<rm::sim::MissileRedirect> redirects;
    std::vector<int> commandersEver;

    Fixture() {
        // One type, no definition: none of these cases fire a weapon or earn anything, and
        // "registered without a def" is a case the catalog has to carry anyway.
        const rm::UnitTypeIndex type = catalog.add(nullptr);
        (void)store.spawn({
            .type = type,
            .transform = {.x = rm::test::fx(100.0f), .z = rm::test::fx(100.0f)},
            // Given the ordinary default speed, so a case that orders it somewhere and
            // expects movement gets it. A bare `MoveState{}` has a per-tick speed of zero
            // now — see `MoveState::speedPerTick` on why the default cannot name a rate.
            .motion = defaultMotionFor(0),
            .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
        });
        armies = rm::sim::freeForAll(1);
        economies.assign(1, rm::sim::Economy{});
        commandersEver.assign(1, 1);
    }

    [[nodiscard]] std::span<rm::sim::Transform> transforms() { return store.transforms(); }
    [[nodiscard]] std::span<rm::sim::MoveState> motion() { return store.motion(); }
    [[nodiscard]] std::span<rm::sim::Health> health() { return store.health(); }

    [[nodiscard]] rm::sim::Match match() {
        return rm::sim::Match{
            .armies = armies,
            .economies = economies,
            .projectiles = &projectiles,
                .building = &building,
                .siloAmmo = &siloAmmo,
                .redirects = &redirects,
            .commandersEver = commandersEver,
        };
    }

    [[nodiscard]] rm::StateHash hash() {
        rm::sim::Match m = match();
        return hashMatch(store, m);
    }
};

} // namespace

TEST_CASE("the same state hashes the same, twice running") {
    Fixture a;
    Fixture b;
    REQUIRE(a.hash() == b.hash());
    // And stable across repeated calls on one fixture — a hash that consumed state or
    // touched a counter would fail here and nowhere else.
    const rm::StateHash first = a.hash();
    REQUIRE(a.hash() == first);
    REQUIRE(a.hash() == first);
}

TEST_CASE("silo ammunition state changes the match hash") {
    Fixture fixture;
    const auto baseline = fixture.hash();
    fixture.siloAmmo.push_back({.owner = fixture.store.idAt(0),
                                .capacity = 7,
                                .totalTicks = 2400,
                                .costPerTick = {.mass = rm::sim::Mag::fromInt(1),
                                                .energy = rm::sim::Mag::fromInt(150)}});
    CHECK(fixture.hash() != baseline);
    const auto withAmmo = fixture.hash();
    ++fixture.siloAmmo.front().elapsedTicks;
    CHECK(fixture.hash() != withAmmo);
    // The slot is part of the identity: a nuke-slot record is not a tactical one (`C-081`).
    const auto withElapsed = fixture.hash();
    fixture.siloAmmo.front().slot = 1;
    CHECK(fixture.hash() != withElapsed);
    CHECK(fixture.hash() != withAmmo);
}

TEST_CASE("redirector state changes the match hash") {
    Fixture fixture;
    const auto baseline = fixture.hash();
    fixture.redirects.push_back({.owner = fixture.store.idAt(0),
                                 .radiusElmos = rm::sim::fxFromFloat(40.0f),
                                 .cooldownTicks = 10,
                                 .remaining = 4});
    CHECK(fixture.hash() != baseline);
    const auto cooling = fixture.hash();
    fixture.redirects.front().remaining = 0;
    CHECK(fixture.hash() != cooling);
}

TEST_CASE("playable-rectangle presence and every bound change the match hash") {
    Fixture unrestricted;
    Fixture bounded;
    rm::sim::Match match = bounded.match();
    match.playableRect = rm::sim::PlayableRect{
        .minX = rm::test::fx(10.0f),
        .maxX = rm::test::fx(20.0f),
        .minZ = rm::test::fx(30.0f),
        .maxZ = rm::test::fx(40.0f),
    };
    const rm::StateHash allBounds = hashMatch(bounded.store, match);

    CHECK(unrestricted.hash() != allBounds);

    const auto differsWhen = [&](auto change) {
        rm::sim::Match changed = bounded.match();
        changed.playableRect = *match.playableRect;
        change(*changed.playableRect);
        CHECK(hashMatch(bounded.store, changed) != allBounds);
    };
    differsWhen([](rm::sim::PlayableRect& rect) { rect.minX = rm::test::fx(11.0f); });
    differsWhen([](rm::sim::PlayableRect& rect) { rect.maxX = rm::test::fx(21.0f); });
    differsWhen([](rm::sim::PlayableRect& rect) { rect.minZ = rm::test::fx(31.0f); });
    differsWhen([](rm::sim::PlayableRect& rect) { rect.maxZ = rm::test::fx(41.0f); });
}

TEST_CASE("the cached air movement layer changes the hash") {
    Fixture ground;
    Fixture air;
    air.motion()[0].airborne = true;

    CHECK(ground.hash() != air.hash());
}

TEST_CASE("the cached surface-water movement layer changes the hash") {
    Fixture ground;
    Fixture surface;
    surface.motion()[0].surfaceWater = true;

    CHECK(ground.hash() != surface.hash());
}

TEST_CASE("the DoNotTarget state changes the hash") {
    Fixture ordinary;
    Fixture excluded;

    REQUIRE(excluded.store.setDoNotTarget(excluded.store.idAt(0), true));
    CHECK(ordinary.hash() != excluded.hash());
}

TEST_CASE("DoNotTarget does not collide with factory repeat in the hash") {
    Fixture repeating;
    Fixture excluded;

    REQUIRE(repeating.store.setFactoryRepeat(repeating.store.idAt(0), true));
    REQUIRE(excluded.store.setDoNotTarget(excluded.store.idAt(0), true));
    CHECK(repeating.hash() != excluded.hash());
}

TEST_CASE("visual identification history changes the hash after sight is gone") {
    Fixture fixture;
    fixture.armies = rm::sim::freeForAll(2);
    fixture.economies.assign(2, rm::sim::Economy{});
    fixture.commandersEver.assign(2, 1);

    rm::unitdef::UnitDef radar;
    radar.radarRadiusElmos = 300.0f;
    rm::unitdef::UnitDef scout;
    scout.visionRadiusElmos = 60.0f;
    rm::unitdef::UnitDef target;
    const rm::UnitTypeIndex radarType = fixture.catalog.add(&radar);
    const rm::UnitTypeIndex scoutType = fixture.catalog.add(&scout);
    const rm::UnitTypeIndex targetType = fixture.catalog.add(&target);
    const auto spawn = [&](rm::UnitTypeIndex type, float z, int army) {
        rm::sim::MoveState motion = defaultMotionFor(army);
        return fixture.store.spawn({
            .type = type,
            .transform = {.z = rm::test::fx(z)},
            .motion = motion,
            .health = {.current = rm::test::mag(100.0f), .maximum = rm::test::mag(100.0f)},
        });
    };
    (void)spawn(radarType, 0.0f, 0);
    const rm::sim::UnitId observer = spawn(scoutType, 200.0f, 0);
    (void)spawn(targetType, 200.0f, 1);

    rm::sim::Intel remembered;
    rm::sim::Intel neverSeen;
    for (rm::sim::Intel* intel : {&remembered, &neverSeen}) {
        intel->configure(2, rm::sim::Fx::fromInt(512), rm::sim::Fx::fromInt(512),
                         rm::sim::VisionStyle::ForgedAlliance);
    }

    remembered.update(fixture.store, fixture.catalog, fixture.armies, nullptr);
    fixture.store.transforms()[observer.index].z = rm::test::fx(400.0f);
    remembered.update(fixture.store, fixture.catalog, fixture.armies, nullptr);
    neverSeen.update(fixture.store, fixture.catalog, fixture.armies, nullptr);

    // Both Intel instances now have the same current coverage. Only `remembered` saw the
    // target before the scout moved, and that history changes C-158 target selection.
    rm::sim::Match match = fixture.match();
    match.intel = &remembered;
    const rm::StateHash historyHash = hashMatch(fixture.store, match);
    match.intel = &neverSeen;
    CHECK(historyHash != hashMatch(fixture.store, match));
}

TEST_CASE("shield power and recovery timers change the hash") {
    const auto hashWith = [](rm::sim::ShieldState shield) {
        Fixture fixture;
        fixture.health()[0].shield = shield;
        return fixture.hash();
    };
    const rm::sim::ShieldState full{.current = rm::sim::Mag::fromInt(100),
                                    .maximum = rm::sim::Mag::fromInt(100)};
    CHECK(hashWith(full)
          != hashWith({.current = rm::sim::Mag::fromInt(90),
                       .maximum = rm::sim::Mag::fromInt(100)}));
    CHECK(hashWith(full)
          != hashWith({.current = rm::sim::Mag::fromInt(100),
                       .maximum = rm::sim::Mag::fromInt(101)}));
    CHECK(hashWith(full)
          != hashWith({.current = rm::sim::Mag::fromInt(100),
                       .maximum = rm::sim::Mag::fromInt(100),
                       .regenDelayRemaining = 10}));
    CHECK(hashWith(full)
          != hashWith({.current = rm::sim::Mag::fromInt(100),
                       .maximum = rm::sim::Mag::fromInt(100),
                       .rechargeRemaining = 20}));
}

TEST_CASE("a projectile's target layer changes the hash") {
    Fixture surface;
    Fixture air;
    rm::sim::Projectile shot;
    shot.targetLayers = rm::unitdef::TargetLayerMask::Surface;
    surface.projectiles.push_back(shot);
    shot.targetLayers = rm::unitdef::TargetLayerMask::Air;
    air.projectiles.push_back(shot);

    CHECK(surface.hash() != air.hash());
}

TEST_CASE("a projectile's interceptor role changes the hash") {
    Fixture ordinary;
    Fixture interceptor;
    ordinary.projectiles.push_back({.ticksRemaining = 1});
    interceptor.projectiles.push_back({.interceptor = true, .ticksRemaining = 1});

    CHECK(ordinary.hash() != interceptor.hash());
}

TEST_CASE("a projectile's pending impact and target generation change the hash") {
    const auto hashWith = [](rm::sim::ImpactType impact, rm::sim::UnitId target) {
        Fixture fixture;
        fixture.projectiles.push_back({
            .ticksRemaining = 1,
            .pendingImpact = impact,
            .impactTarget = target,
        });
        return fixture.hash();
    };

    CHECK(hashWith(rm::sim::ImpactType::Invalid, {})
          != hashWith(rm::sim::ImpactType::Air, {}));
    CHECK(hashWith(rm::sim::ImpactType::Unit, {.index = 3, .generation = 1})
          != hashWith(rm::sim::ImpactType::Unit, {.index = 4, .generation = 1}));
    CHECK(hashWith(rm::sim::ImpactType::Unit, {.index = 3, .generation = 1})
          != hashWith(rm::sim::ImpactType::Unit, {.index = 3, .generation = 2}));
}

TEST_CASE("one unit moving one step changes the hash") {
    Fixture a;
    const rm::StateHash before = a.hash();
    a.transforms()[0].x += rm::test::fx(1.0f);
    REQUIRE(a.hash() != before);
}

TEST_CASE("a change too small to see is still a divergence") {
    // The point of hashing bit patterns rather than comparing with a tolerance. A sim that
    // drifts by one bit per tick has diverged, and finding that on tick 1 rather than tick
    // 100,000 is the entire value of the exercise.
    Fixture a;
    const rm::StateHash before = a.hash();
    // ONE STEP of the type, which is now the smallest change that exists. `std::nextafter`
    // is what this used to say, and it has no fixed-point counterpart: there is no gap
    // between representable values to step over, so "the smallest possible difference" is
    // literally a raw unit.
    a.transforms()[0].z = rm::sim::Fx::fromRaw(a.transforms()[0].z.raw() + 1);
    REQUIRE(a.hash() != before);
}

TEST_CASE("presentation is not reachable from the store, let alone hashed") {
    // THIS CASE CHANGED SHAPE, and the change is the point. It used to set `animationPhase`
    // and `teamColour` on a stored unit and require the hash not to move — a real risk, since
    // the store held the GPU's struct and a careless `feed` would have fingerprinted the
    // palette.
    //
    // P2.2 removed the risk rather than guarding it: the store holds `Transform`, which has no
    // presentation fields, and `UnitInstance` is built at draw time from the transform plus
    // the type's scale and the army's colour. There is nothing left to accidentally hash, and
    // a future `feed` cannot reintroduce the bug because the data is not there to feed.
    //
    // So what is asserted is the structure. If someone puts a colour or an animation phase
    // back into sim state, this stops compiling — which is a better guard than a runtime check.
    static_assert(sizeof(rm::sim::Transform) == 3 * sizeof(rm::sim::Fx) + 3 * sizeof(rm::Brad)
                      + 2,
                  "Transform should hold exactly a position and three angles; anything else "
                  "has crept in, and if it is presentation it does not belong in the store");
    SUCCEED();
}

TEST_CASE("every field the sim owns reaches the hash") {
    // One case per field group rather than one per field: what this guards against is a
    // field being added to the sim and forgotten here, which would make the hash agree
    // about a match that differs. If you added a field and this test still passes, the
    // field is not being hashed.
    SECTION("orientation") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.transforms()[0].heading = static_cast<rm::Brad>(a.transforms()[0].heading + 100);
        REQUIRE(a.hash() != before);
    }
    SECTION("slope alignment") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.transforms()[0].pitch = static_cast<rm::Brad>(a.transforms()[0].pitch + 100);
        REQUIRE(a.hash() != before);
    }
    SECTION("health") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health()[0].current -= rm::test::mag(1.0f);
        REQUIRE(a.hash() != before);
    }
    SECTION("reload") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health()[0].reloadRemaining.push_back(3);
        REQUIRE(a.hash() != before);
    }
    SECTION("automatic weapon incumbent") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.health()[0].automaticTargets.push_back(a.store.idAt(0));
        REQUIRE(a.hash() != before);
    }
    SECTION("motion order") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion()[0].moving = true;
        REQUIRE(a.hash() != before);
    }
    SECTION("route") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.motion()[0].path.push_back({rm::test::fx(10.0f), rm::test::fx(20.0f)});
        REQUIRE(a.hash() != before);
    }
    SECTION("how far along the route") {
        Fixture a;
        a.motion()[0].path.push_back({rm::test::fx(10.0f), rm::test::fx(20.0f)});
        a.motion()[0].path.push_back({rm::test::fx(30.0f), rm::test::fx(40.0f)});
        const rm::StateHash before = a.hash();
        a.motion()[0].pathIndex = 1;
        REQUIRE(a.hash() != before);
    }
    SECTION("defeat") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.armies[0].defeated = true;
        REQUIRE(a.hash() != before);
    }
    SECTION("banked resources") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.economies[0].stored.mass += rm::test::mag(1.0f);
        REQUIRE(a.hash() != before);
    }
    SECTION("the stall ratio") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.economies[0].fundedFraction = rm::sim::Fx::fromRatio(1, 2);
        REQUIRE(a.hash() != before);
    }
    SECTION("a shot in flight") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.projectiles.push_back(rm::sim::Projectile{.damage = rm::unitdef::flatDamage(rm::test::mag(10.0f)), .ticksRemaining = 30});
        REQUIRE(a.hash() != before);
    }
    SECTION("work under construction") {
        Fixture a;
        const rm::StateHash before = a.hash();
        a.building.push_back(
            rm::sim::Construction{.armyIndex = 0, .buildTimeRemaining = rm::test::mag(5.0f)});
        REQUIRE(a.hash() != before);
    }
    SECTION("the construction founder") {
        Fixture a;
        a.building.push_back(rm::sim::Construction{});
        const rm::StateHash before = a.hash();
        a.building[0].builder = rm::sim::UnitId{0, 1};
        REQUIRE(a.hash() != before);
    }
    SECTION("assistance applied this tick") {
        Fixture a;
        a.building.push_back(rm::sim::Construction{});
        const rm::StateHash before = a.hash();
        a.building[0].assistPerTick = rm::test::mag(1.0f);
        REQUIRE(a.hash() != before);
    }
    SECTION("the next command serial") {
        Fixture a;
        const rm::StateHash before = a.hash();
        (void)a.store.allocateCommandSerial();
        REQUIRE(a.hash() != before);
    }
    SECTION("a queued command's creation serial") {
        Fixture a;
        Fixture b;
        a.store.orders()[0].append(rm::sim::Command{.unit = a.store.idAt(0)});
        b.store.orders()[0].append(rm::sim::QueuedCommand{
            b.store.idAt(0),
            std::make_shared<const rm::sim::SharedCommand>(rm::sim::SharedCommand{
                .units = {b.store.idAt(0)},
                .creationSerial = 1,
            })});
        REQUIRE(a.hash() != b.hash());
    }
    SECTION("which queued command is active") {
        Fixture a;
        a.store.orders()[0].append(rm::sim::Command{.unit = a.store.idAt(0)});
        const rm::StateHash before = a.hash();
        a.store.orders()[0].markCurrentActive();
        REQUIRE(a.hash() != before);
    }
}

TEST_CASE("command hashing uses semantic identity and local execution, not ownership") {
    const auto payloadFor = [](rm::sim::UnitId first, rm::sim::UnitId second,
                               rm::CommandId id) {
        return rm::sim::SharedCommand{
            .source = rm::commandSource(id),
            .id = id,
            .kind = rm::sim::CommandKind::Move,
            .units = {first, second},
            .targetX = rm::test::fx(300.0f),
            .targetZ = rm::test::fx(300.0f),
            .creationSerial = 7,
        };
    };
    const auto addSecond = [](Fixture& fixture) {
        return fixture.store.spawn({
            .type = 0,
            .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(100.0f)},
            .motion = defaultMotionFor(0),
            .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
        });
    };

    Fixture sharedOwners;
    Fixture separateOwners;
    const rm::sim::UnitId sharedFirst = sharedOwners.store.idAt(0);
    const rm::sim::UnitId sharedSecond = addSecond(sharedOwners);
    const rm::sim::UnitId separateFirst = separateOwners.store.idAt(0);
    const rm::sim::UnitId separateSecond = addSecond(separateOwners);
    const rm::CommandId id = rm::commandId(0, 9);

    const auto shared = std::make_shared<const rm::sim::SharedCommand>(
        payloadFor(sharedFirst, sharedSecond, id));
    sharedOwners.store.orders()[sharedFirst.index].append(
        rm::sim::QueuedCommand{sharedFirst, shared});
    sharedOwners.store.orders()[sharedSecond.index].append(
        rm::sim::QueuedCommand{sharedSecond, shared});

    separateOwners.store.orders()[separateFirst.index].append(rm::sim::QueuedCommand{
        separateFirst,
        std::make_shared<const rm::sim::SharedCommand>(
            payloadFor(separateFirst, separateSecond, id))});
    separateOwners.store.orders()[separateSecond.index].append(rm::sim::QueuedCommand{
        separateSecond,
        std::make_shared<const rm::sim::SharedCommand>(
            payloadFor(separateFirst, separateSecond, id))});

    CHECK(sharedOwners.hash() == separateOwners.hash());

    Fixture differentIdentity;
    const rm::sim::UnitId differentFirst = differentIdentity.store.idAt(0);
    const rm::sim::UnitId differentSecond = addSecond(differentIdentity);
    const rm::CommandId otherId = rm::commandId(0, 10);
    const auto other = std::make_shared<const rm::sim::SharedCommand>(
        payloadFor(differentFirst, differentSecond, otherId));
    differentIdentity.store.orders()[differentFirst.index].append(
        rm::sim::QueuedCommand{differentFirst, other});
    differentIdentity.store.orders()[differentSecond.index].append(
        rm::sim::QueuedCommand{differentSecond, other});
    CHECK(sharedOwners.hash() != differentIdentity.hash());

    separateOwners.store.orders()[separateFirst.index].currentMutable()->setTargetPosition(
        rm::test::fx(301.0f), rm::test::fx(300.0f));
    CHECK(sharedOwners.hash() != separateOwners.hash());

    separateOwners.store.orders()[separateFirst.index].currentMutable()->setTargetPosition(
        rm::test::fx(300.0f), rm::test::fx(300.0f));
    separateOwners.store.orders()[separateFirst.index].markCurrentActive();
    CHECK(sharedOwners.hash() != separateOwners.hash());
}

TEST_CASE("an expired live-ID tombstone is not authoritative state") {
    Fixture clean;
    Fixture expired;
    {
        auto command = std::make_shared<rm::sim::SharedCommand>(rm::sim::SharedCommand{
            .source = 0,
            .id = rm::commandId(0, 77),
        });
        REQUIRE(expired.store.registerCommand(command));
    }

    CHECK(clean.hash() == expired.hash());
    CHECK_FALSE(expired.store.commandIdLive(rm::commandId(0, 77)));
}

TEST_CASE("unit-set permutations and duplicates produce identical command state") {
    Fixture canonical;
    Fixture shuffled;
    const rm::sim::UnitId canonicalFirst = canonical.store.idAt(0);
    const rm::sim::UnitId shuffledFirst = shuffled.store.idAt(0);
    const rm::sim::UnitId canonicalSecond = canonical.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(100.0f)},
        .motion = defaultMotionFor(0),
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::sim::UnitId shuffledSecond = shuffled.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(100.0f)},
        .motion = defaultMotionFor(0),
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};
    const rm::sim::PassabilityGrid grid = rm::sim::buildPassability(field, 0.0f);
    const std::vector<rm::sim::Player> players{rm::sim::Player{.index = 0, .army = 0}};
    const auto issue = [](std::vector<rm::sim::UnitId> units) {
        return rm::sim::CommandIssue{
            .source = 0,
            .id = rm::commandId(0, 4),
            .player = 0,
            .kind = rm::sim::CommandKind::Move,
            .units = std::move(units),
            .targetX = rm::test::fx(400.0f),
            .targetZ = rm::test::fx(400.0f),
        };
    };

    REQUIRE(rm::sim::applyCommand(
        issue({canonicalFirst, canonicalSecond}), canonical.store, canonical.catalog, players,
        canonical.armies, terrain, [&grid](rm::sim::UnitId) { return &grid; },
        rm::sim::TickRate{}));
    REQUIRE(rm::sim::applyCommand(
        issue({shuffledSecond, shuffledFirst, shuffledSecond}), shuffled.store, shuffled.catalog,
        players, shuffled.armies, terrain, [&grid](rm::sim::UnitId) { return &grid; },
        rm::sim::TickRate{}));

    CHECK(canonical.hash() == shuffled.hash());
}

TEST_CASE("losing a unit changes the hash even though the survivors match") {
    // Death is a TOMBSTONE: the slot stays and keeps its last values, so a hash built from
    // the arrays alone would say nothing happened. What separates the two states is the
    // slot's GENERATION, which `release` bumps — which is why it is hashed.
    Fixture a;
    const rm::sim::UnitId second = a.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(100.0f), .z = rm::test::fx(100.0f)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::StateHash both = a.hash();

    a.store.kill(second);
    REQUIRE(a.store.liveCount() == 1);
    REQUIRE(a.store.slotCount() == 2);  // nothing was erased
    REQUIRE(a.hash() != both);
}

TEST_CASE("two units swapping places is a different match") {
    // Order IS state. A sim whose behaviour did not depend on iteration order would be a
    // sim we could not write a replay for.
    Fixture a;
    (void)a.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(200.0f)},
        .motion = {.armyIndex = 0},
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    const rm::StateHash before = a.hash();

    std::swap(a.transforms()[0], a.transforms()[1]);
    REQUIRE(a.hash() != before);
}

TEST_CASE("no list and an empty list hash differently") {
    // A decorative crowd has no projectile list at all; a match between two ticks of combat
    // has an empty one. Those are different situations and must not collide.
    Fixture a;
    rm::sim::Match withList = a.match();
    rm::sim::Match without = a.match();
    without.projectiles = nullptr;

    REQUIRE(hashMatch(a.store, withList) != hashMatch(a.store, without));
}

TEST_CASE("a decided match hashes differently from one still being played") {
    Fixture a;
    rm::sim::Match running = a.match();
    rm::sim::Match decided = a.match();
    decided.over = true;

    REQUIRE(hashMatch(a.store, running) != hashMatch(a.store, decided));
}

TEST_CASE("pending winner confirmation changes the hash") {
    Fixture a;
    rm::sim::Match running = a.match();
    rm::sim::Match confirming = a.match();
    confirming.winnerPending = true;
    confirming.pendingWinner = 0;
    confirming.winnerStableTicks = 1;

    REQUIRE(hashMatch(a.store, running) != hashMatch(a.store, confirming));
}

TEST_CASE("negative zero is not a divergence") {
    // -0.0f == 0.0f arithmetically but their bit patterns differ. A sim that produced one
    // where it previously produced the other has not diverged, and reporting it as such
    // would be a false positive on a value that compares equal.
    Fixture a;
    const rm::StateHash positive = a.hash();
    // Negative zero has no fixed-point counterpart: `Fx` is an integer, and integers have
    // one zero. The float version needed this case because -0.0f and 0.0f have different bit
    // patterns and the hash fed bit patterns; the whole class of false divergence is gone.
    a.transforms()[0].y = rm::sim::Fx{};
    REQUIRE(a.hash() == positive);
}

TEST_CASE("the target handle changes the hash before pursuit changes movement") {
    Fixture a;
    Fixture b;
    Fixture c;
    rm::sim::Command left{.kind = rm::sim::CommandKind::Attack,
                          .unit = rm::sim::UnitId{0, 1},
                          .targetX = rm::test::fx(200.0f),
                          .targetZ = rm::test::fx(200.0f),
                          .target = rm::sim::UnitId{4, 2}};
    rm::sim::Command right = left;
    right.target = rm::sim::UnitId{5, 2};
    rm::sim::Command newer = left;
    newer.target = rm::sim::UnitId{4, 3};
    (void)a.store.orders()[0].give(left, false);
    (void)b.store.orders()[0].give(right, false);
    (void)c.store.orders()[0].give(newer, false);

    REQUIRE(a.hash() != b.hash());
    REQUIRE(a.hash() != c.hash());
}

TEST_CASE("a real tick moves the hash, and the same tick moves it the same way") {
    // The property the divergence harness is built on, end to end: two matches stepped
    // identically stay identical, tick after tick.
    const rm::HeightField field = flatField();

    Fixture a;
    Fixture b;
    a.motion()[0].moving = true;
    a.motion()[0].destinationX = rm::test::fx(400.0f);
    a.motion()[0].destinationZ = rm::test::fx(100.0f);
    b.motion()[0] = a.motion()[0];

    REQUIRE(a.hash() == b.hash());

    // Two sims stepped side by side in ONE process, which is the strongest determinism test
    // available and the payoff for having no sim globals (PLAN2.md §5.4).
    for (int tick = 0; tick < 20; ++tick) {
        rm::sim::Match ma = a.match();
        rm::sim::Match mb = b.match();
        (void)rm::sim::tickSkirmish(a.store, a.catalog, ma, rm::sim::Terrain{field});
        (void)rm::sim::tickSkirmish(b.store, b.catalog, mb, rm::sim::Terrain{field});
        REQUIRE(a.hash() == b.hash());
    }

    // ...and that it actually advanced, so the agreement above is not two frozen matches
    // agreeing about nothing.
    Fixture fresh;
    fresh.motion()[0] = a.motion()[0];
    REQUIRE(a.transforms()[0].x != fresh.transforms()[0].x);
}

TEST_CASE("a skirmish tick propagates attached transforms and detach stops propagation") {
    const rm::HeightField field = flatField();
    Fixture fixture;
    const rm::sim::UnitId parent = fixture.store.idAt(0);
    const rm::sim::UnitId child = fixture.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(200.0f)},
        .motion = defaultMotionFor(0),
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    REQUIRE(fixture.store.attach(parent, child));
    // This overlaps the propagated child. Collision moves both mobile peers, so the final
    // attachment assertion below proves the second post-collision propagation pass ran.
    (void)fixture.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(200.0f)},
        .motion = defaultMotionFor(0),
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    fixture.motion()[parent.index].moving = true;
    fixture.motion()[parent.index].destinationX = rm::test::fx(400.0f);
    fixture.motion()[parent.index].destinationZ = rm::test::fx(100.0f);

    rm::sim::Match match = fixture.match();
    (void)rm::sim::tickSkirmish(fixture.store, fixture.catalog, match, rm::sim::Terrain{field});
    CHECK(fixture.store.transforms()[child.index].x
          == fixture.store.transforms()[parent.index].x + rm::test::fx(100.0f));
    CHECK(fixture.store.transforms()[child.index].z
          == fixture.store.transforms()[parent.index].z + rm::test::fx(100.0f));

    REQUIRE(fixture.store.detach(child));
    const rm::sim::Transform detached = fixture.store.transforms()[child.index];
    fixture.motion()[child.index].radiusElmos = {};
    (void)rm::sim::tickSkirmish(fixture.store, fixture.catalog, match, rm::sim::Terrain{field});
    CHECK(fixture.store.transforms()[child.index].x == detached.x);
    CHECK(fixture.store.transforms()[child.index].z == detached.z);
}

TEST_CASE("attachment propagation retains an attached child's local height") {
    const rm::HeightField field = rampField();
    const rm::sim::Terrain terrain{field};
    Fixture fixture;
    const rm::sim::UnitId parent = fixture.store.idAt(0);
    const rm::sim::UnitId child = fixture.store.spawn({
        .type = 0,
        .transform = {.x = rm::test::fx(200.0f), .z = rm::test::fx(100.0f)},
        .motion = defaultMotionFor(0),
        .health = {.current = rm::test::mag(500.0f), .maximum = rm::test::mag(500.0f)},
    });
    fixture.motion()[child.index].radiusElmos = {};
    REQUIRE(fixture.store.attach(parent, child));
    fixture.motion()[parent.index].moving = true;
    fixture.motion()[parent.index].destinationX = rm::test::fx(400.0f);
    fixture.motion()[parent.index].destinationZ = rm::test::fx(100.0f);

    rm::sim::Match match = fixture.match();
    (void)rm::sim::tickSkirmish(fixture.store, fixture.catalog, match, terrain);

    const rm::sim::Transform& childTransform = fixture.store.transforms()[child.index];
    CHECK(childTransform.y == fixture.store.transforms()[parent.index].y);
    CHECK(childTransform.y != terrain.heightAt(childTransform.x, childTransform.z));
}
