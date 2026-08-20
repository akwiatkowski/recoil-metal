#include "core/map/HeightField.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitDef.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

// Properties a match must have however its units are STORED.
//
// WHY THIS FILE EXISTS. `docs/golden-p1.log` verifies that a change altered nothing, by
// comparing per-tick state hashes. It is the right tool for a refactor that preserves
// behaviour, and it is useless for one that does not — and P1.4 does not: moving from
// per-batch storage to a flat store changes which unit wins a targeting tie
// (`nearestTarget` breaks ties on the lower batch then the lower instance), which changes
// the match. The hash will disagree, correctly, and so cannot tell "reordered as expected"
// from "broke".
//
// So this is the other half of the net. Everything asserted here is true of ANY correct
// match — no assertion mentions an order, a batch, a slot, or a count of units in a
// particular place. A migration that reorders units keeps every one of them; a migration
// that loses a unit, resurrects a corpse, double-reports a death, leaks a projectile or
// lets health go negative breaks one immediately.
//
// Written before P1.4 rather than after, because a net built after the fall is a post-mortem.

using rm::sim::Army;
using rm::sim::Health;
using rm::sim::SkirmishGroup;
using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::unitdef::WeaponRole;

namespace {

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 200;
    field.squaresZ = 200;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] Weapon turretedGun(float damage, float rangeElmos) {
    Weapon weapon;
    weapon.label = "test gun";
    weapon.role = WeaponRole::DirectFire;
    weapon.turreted = true;
    weapon.damage = damage;
    weapon.maxRangeElmos = rangeElmos;
    weapon.rateOfFire = 1.0f;
    weapon.muzzleVelocityElmosPerSecond = 200.0f;
    return weapon;
}

/// One batch's storage. Held by the caller because a SkirmishGroup is spans into it — the
/// shape that P1.4 replaces, which is exactly why the assertions below must not depend on it.
struct Batch {
    std::vector<rm::UnitInstance> instances;
    std::vector<rm::sim::MoveState> motion;
    std::vector<Health> health;
    UnitDef def;

    void add(float x, float z, int army, float hp) {
        rm::UnitInstance instance{};
        instance.position = {x, 0.0f, z};
        instance.scale = 1.0f;
        instances.push_back(instance);

        rm::sim::MoveState state;
        state.armyIndex = army;
        state.radiusElmos = 4.0f;
        motion.push_back(state);

        health.push_back(Health{.current = hp,
                                .maximum = hp,
                                .reloadRemaining = std::vector<int>(def.weapons.size(), 0)});
    }

    [[nodiscard]] SkirmishGroup group() {
        return SkirmishGroup{
            .instances = instances, .motion = motion, .health = health, .def = &def};
    }
};

/// A fight that actually resolves: two armies of gunners inside each other's range.
struct Fight {
    Batch red;
    Batch blue;
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<rm::sim::Projectile> projectiles;
    std::vector<rm::sim::Construction> building;
    std::vector<rm::sim::Economy> economies;
    std::vector<int> commandersEver;

    Fight() {
        red.def.name = "red_gun";
        red.def.weapons.push_back(turretedGun(40.0f, 300.0f));
        blue.def.name = "blue_gun";
        blue.def.weapons.push_back(turretedGun(40.0f, 300.0f));

        for (int i = 0; i < 5; ++i) {
            red.add(0.0f, static_cast<float>(i) * 30.0f, 0, 300.0f);
            blue.add(200.0f, static_cast<float>(i) * 30.0f, 1, 300.0f);
        }

        economies.assign(2, rm::sim::Economy{});
        commandersEver.assign(2, 0);  // no commanders: the win condition sits out
    }

    [[nodiscard]] std::vector<SkirmishGroup> groups() { return {red.group(), blue.group()}; }

    [[nodiscard]] rm::sim::Match match() {
        return rm::sim::Match{
            .armies = armies,
            .economies = economies,
            .projectiles = &projectiles,
            .building = &building,
            .commandersEver = commandersEver,
        };
    }
};

/// Every (batch, instance) pair in a set of groups, as a flat list of health values — used
/// to state totals without naming where a unit lives.
[[nodiscard]] float totalHealth(const std::vector<SkirmishGroup>& groups) {
    float total = 0.0f;
    for (const SkirmishGroup& group : groups) {
        for (const Health& h : group.health) {
            total += h.current;
        }
    }
    return total;
}

[[nodiscard]] std::size_t livingUnits(const std::vector<SkirmishGroup>& groups) {
    std::size_t alive = 0;
    for (const SkirmishGroup& group : groups) {
        for (const Health& h : group.health) {
            alive += h.alive() ? 1u : 0u;
        }
    }
    return alive;
}

} // namespace

TEST_CASE("health stays within its bounds for every unit, every tick") {
    // The cheapest invariant and the one most likely to catch an indexing mistake: if a
    // migration crosses two units' health with each other's maximum, or applies damage to
    // the wrong slot, a value leaves its range.
    const rm::HeightField field = flatField();
    Fight fight;

    for (int tick = 0; tick < 200; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);

        for (const SkirmishGroup& group : groups) {
            for (const Health& h : group.health) {
                REQUIRE(h.current >= 0.0f);
                REQUIRE(h.current <= h.maximum);
            }
        }
    }
}

TEST_CASE("the dead stay dead") {
    // A resurrected corpse is the signature failure of a storage migration: a slot reused
    // or an index crossed puts health back into a unit that had none.
    const rm::HeightField field = flatField();
    Fight fight;
    std::set<std::pair<std::size_t, std::size_t>> everDead;

    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);

        for (std::size_t g = 0; g < groups.size(); ++g) {
            for (std::size_t i = 0; i < groups[g].health.size(); ++i) {
                const bool alive = groups[g].health[i].alive();
                if (!alive) {
                    everDead.insert({g, i});
                } else {
                    REQUIRE_FALSE(everDead.contains({g, i}));
                }
            }
        }
    }
    REQUIRE_FALSE(everDead.empty());  // the fight actually killed somebody
}

TEST_CASE("a death is reported exactly once, ever") {
    // The caller leaves a permanent scorch mark per entry, so a corpse reported twice
    // scorches the same ground twice — and a corpse reported never leaves none at all.
    const rm::HeightField field = flatField();
    Fight fight;
    std::set<std::pair<std::size_t, std::size_t>> reported;

    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        const rm::sim::TickReport report = rm::sim::tickSkirmish(groups, match, field);

        for (const rm::sim::Death& death : report.died) {
            const auto key = std::pair{death.ref.batch, death.ref.instance};
            REQUIRE(reported.insert(key).second);  // never seen before
        }
    }

    REQUIRE(reported.size() == 10 - livingUnits(fight.groups()));
}

TEST_CASE("a reported death carries the radius it had while alive") {
    // The wreck is sized from it, and retiring a unit is what zeroes it — so a report that
    // sampled after retirement would leave a scorch mark of size zero. Stated as an
    // invariant because it is easy to reintroduce by moving one line.
    const rm::HeightField field = flatField();
    Fight fight;
    bool sawADeath = false;

    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        const rm::sim::TickReport report = rm::sim::tickSkirmish(groups, match, field);
        for (const rm::sim::Death& death : report.died) {
            sawADeath = true;
            REQUIRE(death.radiusElmos > 0.0f);
        }
    }
    REQUIRE(sawADeath);
}

TEST_CASE("a corpse stops shoving the living") {
    // `retireDead` zeroes the radius so the dead stop participating in collisions. If a
    // migration retires the wrong slot, a living unit is un-shoved and a dead one is not.
    const rm::HeightField field = flatField();
    Fight fight;

    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);

        for (const SkirmishGroup& group : groups) {
            for (std::size_t i = 0; i < group.health.size(); ++i) {
                if (!group.health[i].alive()) {
                    REQUIRE(group.motion[i].radiusElmos == 0.0f);
                }
            }
        }
    }
}

TEST_CASE("total health only ever falls") {
    // No unit heals in this engine yet, so the sum is monotonic. A migration that read a
    // stale copy of health and wrote it back would show up here as a rise.
    const rm::HeightField field = flatField();
    Fight fight;
    float previous = totalHealth(fight.groups());

    for (int tick = 0; tick < 200; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);

        const float now = totalHealth(groups);
        REQUIRE(now <= previous);
        previous = now;
    }
    REQUIRE(previous < 5 * 300.0f * 2);  // damage actually happened
}

TEST_CASE("projectiles do not leak") {
    // Every shot either lands or expires. A list that only grows is a frame-rate bug with a
    // long fuse, and the lifetime cap exists precisely because nothing despawns on leaving
    // the map.
    const rm::HeightField field = flatField();
    Fight fight;
    std::size_t highWater = 0;

    for (int tick = 0; tick < 400; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);
        highWater = std::max(highWater, fight.projectiles.size());
    }

    // Ten gunners firing once a second cannot have hundreds in flight at 10 Hz unless
    // something is failing to retire them.
    REQUIRE(highWater < 100);

    // And once the fight is over, the sky clears.
    for (int tick = 0; tick < 400; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);
    }
    REQUIRE(fight.projectiles.empty());
}

TEST_CASE("shots fired are only ever attributed to a live shooter") {
    // A dead unit that keeps firing is what you get when retirement and the firing pass
    // disagree about which slot is which.
    const rm::HeightField field = flatField();
    Fight fight;

    std::size_t totalShots = 0;
    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        const rm::sim::TickReport report = rm::sim::tickSkirmish(groups, match, field);
        totalShots += report.shotsFired;

        if (livingUnits(groups) == 0) {
            // Nobody left: no further shot may be attributed to anyone.
            const rm::sim::TickReport after = rm::sim::tickSkirmish(groups, match, field);
            REQUIRE(after.shotsFired == 0);
            break;
        }
    }
    REQUIRE(totalShots > 0);
}

TEST_CASE("the match is decided at most once") {
    // `matchEnded` is true on the ONE tick that decided it, and `Match::over` is what keeps
    // it that way. A migration that rebuilt the match state each tick would re-announce.
    const rm::HeightField field = flatField();
    Fight fight;
    fight.commandersEver.assign(2, 1);  // give the win condition something to decide

    int endings = 0;
    for (int tick = 0; tick < 400; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        const rm::sim::TickReport report = rm::sim::tickSkirmish(groups, match, field);
        if (report.matchEnded) {
            ++endings;
        }
        if (match.over) {
            break;
        }
    }
    REQUIRE(endings <= 1);
}

TEST_CASE("a defeated army stays defeated") {
    const rm::HeightField field = flatField();
    Fight fight;
    fight.commandersEver.assign(2, 1);

    std::vector<bool> everDefeated(2, false);
    for (int tick = 0; tick < 400; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        (void)rm::sim::tickSkirmish(groups, match, field);

        for (const Army& army : fight.armies) {
            const auto i = static_cast<std::size_t>(army.index);
            if (army.defeated) {
                everDefeated[i] = true;
            }
            REQUIRE_FALSE((everDefeated[i] && !army.defeated));
        }
    }
}

TEST_CASE("an economy never banks more than it can store, nor funds more than it has") {
    const rm::HeightField field = flatField();
    Fight fight;
    for (rm::sim::Economy& economy : fight.economies) {
        economy.storage = rm::sim::Resources{.mass = 650.0f, .energy = 5000.0f};
        economy.incomePerSecond = rm::sim::Resources{.mass = 5.0f, .energy = 20.0f};
    }

    for (int tick = 0; tick < 300; ++tick) {
        std::vector<SkirmishGroup> groups = fight.groups();
        rm::sim::Match match = fight.match();
        match.baseStorage = rm::sim::Resources{.mass = 650.0f, .energy = 5000.0f};
        (void)rm::sim::tickSkirmish(groups, match, field);

        for (const rm::sim::Economy& economy : fight.economies) {
            REQUIRE(economy.stored.mass >= 0.0f);
            REQUIRE(economy.stored.energy >= 0.0f);
            REQUIRE(economy.stored.mass <= economy.storage.mass + 0.001f);
            REQUIRE(economy.stored.energy <= economy.storage.energy + 0.001f);
            REQUIRE(economy.fundedFraction >= 0.0f);
            REQUIRE(economy.fundedFraction <= 1.0f);
        }
    }
}

TEST_CASE("a match with nobody in it does nothing, rather than deciding something") {
    // The empty case, which bit us once already: with no armies, survivorCount is zero,
    // which is also <= 1, and a naive win condition declares a draw on tick one.
    const rm::HeightField field = flatField();

    std::vector<Army> none;
    std::vector<rm::sim::Economy> noEconomies;
    std::vector<int> noCommanders;
    std::vector<rm::sim::Projectile> noShots;
    std::vector<SkirmishGroup> noGroups;

    rm::sim::Match match{
        .armies = none,
        .economies = noEconomies,
        .projectiles = &noShots,
        .building = nullptr,
        .commandersEver = noCommanders,
    };

    const rm::sim::TickReport report = rm::sim::tickSkirmish(noGroups, match, field);
    REQUIRE(report.died.empty());
    REQUIRE(report.shotsFired == 0);
    REQUIRE(report.finished.empty());
}
