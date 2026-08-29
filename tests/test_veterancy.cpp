// Veterancy: kill credit, promotion, and what a level is worth.
//
// EVERY EXPECTED NUMBER HERE IS READ OFF RETAIL, not chosen to make the implementation pass.
// The sources are recorded as claims `C-024`, `C-028` and `C-029` in
// `docs/fa-exe-analysis-plan.md`, and trace back to the shipped `lua/game.lua`,
// `lua/sim/Unit.lua`, `lua/sim/BuffDefinitions.lua` and `lua/sim/Buff.lua` in the owned
// Forged Alliance corpus.
//
// The three cases worth having are the three that an implementation written from memory, or
// ported from FAF, gets wrong: levels that compound, a promotion that does not heal, and a
// veteran whose guns hit harder.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Veterancy.hpp"

#include "core/sim/Events.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

using rm::sim::Health;
using rm::sim::UnitId;
using rm::test::Roster;
using rm::unitdef::UnitDef;

namespace {

/// A type with a known maximum health and, optionally, a hull regeneration rate.
[[nodiscard]] UnitDef bodyDef(float maxHealth, float regenPerSecond = 0.0f) {
    UnitDef def;
    def.name = "test_body";
    def.health = rm::test::mag(maxHealth);
    def.regenPerSecond = regenPerSecond;
    return def;
}

/// Gives `unit` exactly `kills` kills, one at a time, the way the tick does.
///
/// One at a time rather than by setting the counter, because the promotion is a side effect
/// of crossing a threshold and a test that set the field would not exercise it.
void award(Roster& roster, UnitId unit, int kills) {
    for (int i = 0; i < kills; ++i) {
        (void)rm::sim::creditKill(roster.store, roster.catalog, unit, nullptr);
    }
}

[[nodiscard]] const Health& healthOf(const Roster& roster, UnitId unit) {
    return roster.store.health()[unit.index];
}

/// Wounds a unit down to `current`, leaving its maximum where it was.
///
/// `Roster::add` takes ONE health figure and uses it for both the current and the maximum
/// (`sim::initialHealth`), so a unit added with 400 is a unit whose maximum is 400 — not a
/// 1,000-health unit that has taken 600. Every case below that cares about healing needs the
/// second thing, and doing it through this helper is what stops the distinction being lost
/// again the next time a case is added.
void wound(Roster& roster, UnitId unit, float current) {
    roster.store.health()[unit.index].current = rm::test::mag(current);
}

} // namespace

TEST_CASE("default veterancy thresholds are retail's 25/100/250/500/1000") {
    // `Game.VeteranDefault`, `lua/game.lua:11-17`. Checked at the boundary on both sides,
    // because an off-by-one here is a level that arrives one kill early or late and would
    // never be noticed in play.
    const auto& fallback = rm::sim::kVeterancyKillThresholds;
    CHECK(rm::sim::veterancyLevelFor(0, fallback) == 0);
    CHECK(rm::sim::veterancyLevelFor(24, fallback) == 0);
    CHECK(rm::sim::veterancyLevelFor(25, fallback) == 1);
    CHECK(rm::sim::veterancyLevelFor(99, fallback) == 1);
    CHECK(rm::sim::veterancyLevelFor(100, fallback) == 2);
    CHECK(rm::sim::veterancyLevelFor(249, fallback) == 2);
    CHECK(rm::sim::veterancyLevelFor(250, fallback) == 3);
    CHECK(rm::sim::veterancyLevelFor(499, fallback) == 3);
    CHECK(rm::sim::veterancyLevelFor(500, fallback) == 4);
    CHECK(rm::sim::veterancyLevelFor(999, fallback) == 4);
    CHECK(rm::sim::veterancyLevelFor(1000, fallback) == 5);

    // Five is the ceiling: `table.getsize(VeteranDefault)` is 5 and `SetVeterancy` rejects
    // anything above it. No number of kills produces a sixth level.
    CHECK(rm::sim::veterancyLevelFor(1'000'000, fallback) == 5);
}

TEST_CASE("a blueprint's own Veteran table overrides the default") {
    // THE COMMON CASE, not the exception: 193 of the 568 shipped blueprints state one, and
    // they are much shorter than the default. These are UAA0102's actual numbers, an
    // interceptor that reaches level 1 on its second kill rather than its twenty-fifth.
    const rm::sim::VeterancyThresholds interceptor{2, 4, 6, 8, 10};
    CHECK(rm::sim::veterancyLevelFor(1, interceptor) == 0);
    CHECK(rm::sim::veterancyLevelFor(2, interceptor) == 1);
    CHECK(rm::sim::veterancyLevelFor(10, interceptor) == 5);

    // A partial table caps where it stops rather than promoting on a missing level: a zero
    // threshold would otherwise be satisfied by any kill count at all.
    const rm::sim::VeterancyThresholds partial{5, 10, 0, 0, 0};
    CHECK(rm::sim::veterancyLevelFor(10, partial) == 2);
    CHECK(rm::sim::veterancyLevelFor(1'000'000, partial) == 2);
}

TEST_CASE("a unit promotes on its own blueprint's thresholds") {
    Roster roster;
    UnitDef def = bodyDef(1000.0f);
    def.veterancyKills = {2, 4, 6, 8, 10};
    const rm::UnitTypeIndex type = roster.addType(def);
    const UnitId interceptor = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);

    award(roster, interceptor, 2);
    CHECK(healthOf(roster, interceptor).veterancy.level == 1);
    CHECK(healthOf(roster, interceptor).maximum == rm::test::mag(1100.0f));

    award(roster, interceptor, 8);
    CHECK(healthOf(roster, interceptor).veterancy.level == 5);
    CHECK(healthOf(roster, interceptor).maximum == rm::test::mag(1500.0f));
}

TEST_CASE("veteran levels replace each other rather than compounding") {
    // THE CASE THIS FILE EXISTS FOR (`C-028`). Retail recomputes each buff from the blueprint
    // base and the veterancy buffs declare `Stacks = 'REPLACE'`, so level 3 is base x 1.3.
    // An implementation that applied each level's multiplier in turn would reach
    // 1.1 x 1.2 x 1.3 = 1.716, which is 32% too much health and would read as a balance
    // problem rather than as a bug.
    const rm::sim::Mag base = rm::test::mag(1000.0f);

    CHECK(rm::sim::veterancyMaxHealth(base, 0) == rm::test::mag(1000.0f));
    CHECK(rm::sim::veterancyMaxHealth(base, 1) == rm::test::mag(1100.0f));
    CHECK(rm::sim::veterancyMaxHealth(base, 2) == rm::test::mag(1200.0f));
    CHECK(rm::sim::veterancyMaxHealth(base, 3) == rm::test::mag(1300.0f));
    CHECK(rm::sim::veterancyMaxHealth(base, 4) == rm::test::mag(1400.0f));
    CHECK(rm::sim::veterancyMaxHealth(base, 5) == rm::test::mag(1500.0f));
}

TEST_CASE("a kill promotes the killer and heals it by the increase") {
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(bodyDef(1000.0f));

    // Damaged first, so the heal is visible: a unit at full health would be capped at its
    // new maximum either way and the case would prove nothing.
    const UnitId veteran = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);
    wound(roster, veteran, 400.0f);

    award(roster, veteran, 24);
    CHECK(healthOf(roster, veteran).veterancy.level == 0);
    CHECK(healthOf(roster, veteran).maximum == rm::test::mag(1000.0f));
    CHECK(healthOf(roster, veteran).current == rm::test::mag(400.0f));

    // The 25th kill promotes. `Buff.lua` raises the maximum by 100 and then heals by exactly
    // that increase (`AdjustHealth(unit, val - oldmax)`), so current health goes to 500 —
    // NOT to 400, and not to full.
    award(roster, veteran, 1);
    CHECK(healthOf(roster, veteran).veterancy.kills == 25);
    CHECK(healthOf(roster, veteran).veterancy.level == 1);
    CHECK(healthOf(roster, veteran).maximum == rm::test::mag(1100.0f));
    CHECK(healthOf(roster, veteran).current == rm::test::mag(500.0f));

    // Straight to level 5 by kills, and the maximum is still 1.5x the BLUEPRINT base.
    award(roster, veteran, 975);
    CHECK(healthOf(roster, veteran).veterancy.level == 5);
    CHECK(healthOf(roster, veteran).maximum == rm::test::mag(1500.0f));
}

TEST_CASE("promotion emits one event carrying the level reached") {
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(bodyDef(1000.0f));
    const UnitId veteran = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);

    rm::sim::EventQueue events;
    for (int i = 0; i < 100; ++i) {
        (void)rm::sim::creditKill(roster.store, roster.catalog, veteran, &events);
    }

    // A hundred kills crosses two thresholds, so exactly two promotions — not a hundred, and
    // not one per level up to the current one.
    std::vector<rm::sim::Event> promotions;
    for (const rm::sim::Event& event : events.all()) {
        if (event.kind == rm::sim::EventKind::UnitVeteranPromoted) {
            promotions.push_back(event);
        }
    }
    REQUIRE(promotions.size() == 2);
    CHECK(promotions[0].amount == rm::sim::Mag::fromInt(1));
    CHECK(promotions[1].amount == rm::sim::Mag::fromInt(2));
    CHECK(promotions[1].unit == veteran);
}

TEST_CASE("a dead killer earns nothing") {
    Roster roster;
    const rm::UnitTypeIndex type = roster.addType(bodyDef(1000.0f));
    const UnitId ghost = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);

    // Zero health but not yet retired, which is the state `retireDead` genuinely passes here
    // when both sides of a duel die on the same tick. Promoting it would heal it back above
    // zero and resurrect it.
    roster.store.health()[ghost.index].current = rm::sim::Mag{};

    CHECK_FALSE(rm::sim::creditKill(roster.store, roster.catalog, ghost, nullptr));
    CHECK(healthOf(roster, ghost).veterancy.kills == 0);
    CHECK(healthOf(roster, ghost).current == rm::sim::Mag{});

    // And a handle whose unit is gone entirely — the ordinary stale-`lastHitBy` case.
    roster.store.kill(ghost);
    CHECK_FALSE(rm::sim::creditKill(roster.store, roster.catalog, ghost, nullptr));
}

TEST_CASE("regeneration heals the hull at the blueprint rate plus the veteran bonus") {
    // Ten health a second at ten ticks a second is one a tick, which keeps the arithmetic
    // here readable rather than a test of the rate conversion.
    Roster roster;
    roster.rate = rm::sim::TickRate{10};
    const rm::UnitTypeIndex type = roster.addType(bodyDef(1000.0f, 10.0f));
    const UnitId unit = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);
    wound(roster, unit, 500.0f);

    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(healthOf(roster, unit).current == rm::test::mag(501.0f));

    // `VeterancyRegen2` adds 4 a second on top of the blueprint's 10, so 14 a second and
    // 1.4 a tick. Add, not multiply: the regen buffs state `Mult = 1`.
    award(roster, unit, 100);
    REQUIRE(healthOf(roster, unit).veterancy.level == 2);
    const rm::sim::Mag before = healthOf(roster, unit).current;
    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(healthOf(roster, unit).current - before == rm::test::mag(1.4f));
}

TEST_CASE("a blueprint's Buffs.Regen replaces the default veteran regeneration") {
    // 192 of 568 shipped blueprints do this, and they REPLACE rather than add: both buffs
    // carry `BuffType = 'VETERANCYREGEN'` with `Stacks = 'REPLACE'`. UAA0102's ladder is
    // 1/2/3/4/5, half the default.
    Roster roster;
    roster.rate = rm::sim::TickRate{10};
    UnitDef def = bodyDef(1000.0f, 0.0f);
    def.veterancyKills = {2, 4, 6, 8, 10};
    def.veterancyRegenPerSecond = {1, 2, 3, 4, 5};
    const rm::UnitTypeIndex type = roster.addType(def);
    const UnitId unit = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);
    wound(roster, unit, 500.0f);

    // Level 2 on this ladder is 2 a second, not the default's 4 — and the type states no
    // `RegenRate`, so 2 a second is the whole of it: 0.2 a tick at 10 Hz.
    award(roster, unit, 4);
    REQUIRE(healthOf(roster, unit).veterancy.level == 2);
    const rm::sim::Mag before = healthOf(roster, unit).current;
    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);
    CHECK(healthOf(roster, unit).current - before == rm::test::mag(0.2f));
}

TEST_CASE("the shipped blueprints' own veterancy tables are read") {
    // AGAINST THE REAL FILES, because everything above builds its `UnitDef` by hand and so
    // proves the rules without proving that the parser ever fills them in. `Veteran` and
    // `Buffs` are both TOP-LEVEL sections rather than parts of `Defense`, which is exactly the
    // kind of thing a hand-built fixture cannot catch.
    const std::filesystem::path root = [] {
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path{home} / "projects/llm/input/faf/units";
        }
        return std::filesystem::path{};
    }();
    if (root.empty() || !std::filesystem::exists(root)) {
        SKIP("no extracted unit corpus at " + root.string());
    }

    SECTION("a unit that states both tables") {
        // UAA0102, the Aeon interceptor: `Veteran` 2/4/6/8/10 and `Buffs.Regen` 1/2/3/4/5,
        // both well below the engine defaults.
        const std::filesystem::path path = root / "UAA0102" / "UAA0102_unit.bp";
        if (!std::filesystem::exists(path)) {
            SKIP("no " + path.string());
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());
        CHECK(def->veterancyKills == rm::sim::VeterancyThresholds{2, 4, 6, 8, 10});
        CHECK(def->veterancyRegenPerSecond == rm::sim::VeterancyRegen{1, 2, 3, 4, 5});
    }

    SECTION("the commander states its own, and they are not the defaults") {
        // Checked because it was ASSUMED otherwise and the assumption was wrong: the UEF
        // commander looked like an obvious "states nothing" case and in fact states both, at
        // 20/40/60/80/100 kills with 3/6/9/12/15 regeneration. Pinned so the next reader does
        // not have to rediscover it.
        const std::filesystem::path path = root / "UEL0001" / "UEL0001_unit.bp";
        if (!std::filesystem::exists(path)) {
            SKIP("no " + path.string());
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());
        CHECK(def->veterancyKills == rm::sim::VeterancyThresholds{20, 40, 60, 80, 100});
        CHECK(def->veterancyRegenPerSecond == rm::sim::VeterancyRegen{3, 6, 9, 12, 15});
    }

    SECTION("a unit that states neither keeps the engine defaults") {
        // 374 of the 568 shipped units state neither table — mostly structures and campaign
        // props. They must fall back to `Game.VeteranDefault` rather than to zeroes, which
        // would promote on the first kill and grant no veteran regeneration at all.
        const std::filesystem::path path = root / "DAA0206" / "DAA0206_unit.bp";
        if (!std::filesystem::exists(path)) {
            SKIP("no " + path.string());
        }
        const auto def = rm::unitbp::loadFile(path);
        REQUIRE(def.has_value());
        CHECK(def->veterancyKills == rm::sim::kVeterancyKillThresholds);
        CHECK(def->veterancyRegenPerSecond == rm::sim::kVeterancyRegenPerSecond);
    }
}

TEST_CASE("regeneration never exceeds the maximum and never revives the dead") {
    Roster roster;
    roster.rate = rm::sim::TickRate{10};
    const rm::UnitTypeIndex type = roster.addType(bodyDef(1000.0f, 1000.0f));
    const UnitId nearlyFull = roster.add(type, 0.0f, 0.0f, 0, 1000.0f);
    wound(roster, nearlyFull, 999.0f);
    const UnitId corpse = roster.add(type, 50.0f, 0.0f, 0, 1000.0f);
    roster.store.health()[corpse.index].current = rm::sim::Mag{};

    rm::sim::tickRegeneration(roster.store, roster.catalog, roster.rate);

    CHECK(healthOf(roster, nearlyFull).current == rm::test::mag(1000.0f));
    // A unit at zero is dead and stays dead: regeneration is for the living, and healing a
    // corpse before `retireDead` sees it would silently undo a kill.
    CHECK(healthOf(roster, corpse).current == rm::sim::Mag{});
}
