// The script-object lifecycle seam (WP-03): a handle resolves fresh, names its unit's two
// destruction moments the way retail's resolvers do, and never follows a slot's next owner.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/ScriptObject.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/StateHash.hpp"

#include "support/TestRoster.hpp"

#include <cstdint>
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

struct Fixture {
    Fixture() {
        rm::unitdef::UnitDef def;
        def.name = "scripted";
        type = roster.addType(def);
        unit = roster.add(type, 40.0f, 40.0f, 0, 100.0f);
    }

    [[nodiscard]] rm::sim::Match match() {
        return rm::sim::Match{.armies = armies,
                              .economies = economies,
                              .projectiles = &shots,
                              .commandersEver = commandersEver};
    }

    /// One full skirmish tick, which is where `retireDead` releases dead handles.
    void tick() {
        rm::sim::Match current = match();
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, current, terrain, roster.rate);
    }

    [[nodiscard]] rm::StateHash hash() {
        const rm::sim::Match current = match();
        return rm::sim::hashMatch(roster.store, current);
    }

    rm::HeightField field = flatField();
    rm::sim::Terrain terrain{field};
    rm::test::Roster roster;
    rm::UnitTypeIndex type{};
    rm::sim::UnitId unit{};
    std::vector<rm::sim::Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Economy> economies{2};
    std::vector<rm::sim::Projectile> shots;
    std::vector<int> commandersEver{0, 0};
};

using rm::sim::UnitStore;

} // namespace

TEST_CASE("a live handle resolves to its slot for both resolver families", "[script-handle]") {
    Fixture fixture;
    const auto resolved = fixture.roster.store.resolve(fixture.unit);
    CHECK(resolved.state == UnitStore::HandleState::Alive);
    CHECK(resolved.slot == fixture.unit.index);
    REQUIRE(rm::sim::requireAlive(fixture.roster.store, fixture.unit).has_value());
    CHECK(*rm::sim::requireAlive(fixture.roster.store, fixture.unit) == fixture.unit.index);
    CHECK(rm::sim::lifecycleSlot(fixture.roster.store, fixture.unit) == fixture.unit.index);
    CHECK_FALSE(rm::sim::beenDestroyed(fixture.roster.store, fixture.unit));
}

TEST_CASE("a unit dead this tick is Destroyed: readable by lifecycle queries, refused by the rest",
          "[script-handle]") {
    // Retail C-046: `BeenDestroyed` is true once death is queued, while the object still
    // exists for the death callbacks to read. Here that is the window between health
    // reaching zero and `retireDead` at the tick's end.
    Fixture fixture;
    fixture.roster.health(fixture.unit).current = rm::sim::Mag{};

    const auto resolved = fixture.roster.store.resolve(fixture.unit);
    CHECK(resolved.state == UnitStore::HandleState::Destroyed);
    CHECK(resolved.slot == fixture.unit.index);
    CHECK(fixture.roster.store.alive(fixture.unit)); // the handle itself is not released yet
    CHECK(rm::sim::beenDestroyed(fixture.roster.store, fixture.unit));
    CHECK(rm::sim::lifecycleSlot(fixture.roster.store, fixture.unit) == fixture.unit.index);
    const auto strict = rm::sim::requireAlive(fixture.roster.store, fixture.unit);
    REQUIRE_FALSE(strict.has_value());
    CHECK(strict.error() == rm::sim::kDestroyedObjectError);
    CHECK(strict.error() == "Game object has been destroyed");
}

TEST_CASE("after retirement the handle is Stale and both resolver families fail",
          "[script-handle]") {
    Fixture fixture;
    fixture.roster.health(fixture.unit).current = rm::sim::Mag{};
    fixture.tick(); // retireDead releases the handle at the end of this tick

    CHECK_FALSE(fixture.roster.store.alive(fixture.unit));
    CHECK(fixture.roster.store.resolve(fixture.unit).state == UnitStore::HandleState::Stale);
    CHECK_FALSE(rm::sim::lifecycleSlot(fixture.roster.store, fixture.unit).has_value());
    CHECK_FALSE(rm::sim::requireAlive(fixture.roster.store, fixture.unit).has_value());
    CHECK(rm::sim::beenDestroyed(fixture.roster.store, fixture.unit));
}

TEST_CASE("a reused slot cannot resurrect the old handle", "[script-handle]") {
    // Retail relies on a purge delay for this (C-004); the generation does it structurally,
    // and the seam is where a Lua host would otherwise have found the slot's new occupant.
    Fixture fixture;
    fixture.roster.health(fixture.unit).current = rm::sim::Mag{};
    fixture.tick();
    const rm::sim::UnitId successor = fixture.roster.add(fixture.type, 48.0f, 40.0f, 1, 100.0f);
    REQUIRE(successor.index == fixture.unit.index); // the pool reuses the freed slot
    REQUIRE(successor.generation != fixture.unit.generation);

    CHECK(fixture.roster.store.resolve(successor).state == UnitStore::HandleState::Alive);
    CHECK(fixture.roster.store.resolve(fixture.unit).state == UnitStore::HandleState::Stale);
    CHECK_FALSE(rm::sim::lifecycleSlot(fixture.roster.store, fixture.unit).has_value());
    CHECK(rm::sim::lifecycleSlot(fixture.roster.store, successor) == successor.index);
}

TEST_CASE("resolving is a pure read: it changes no state and no hash", "[script-handle]") {
    Fixture fixture;
    const auto before = fixture.hash();
    (void)fixture.roster.store.resolve(fixture.unit);
    (void)rm::sim::requireAlive(fixture.roster.store, fixture.unit);
    (void)rm::sim::lifecycleSlot(fixture.roster.store, rm::sim::UnitId{.index = 7, .generation = 3});
    CHECK(fixture.hash() == before);
}
