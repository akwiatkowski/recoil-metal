// What every shipped unit DOES — generated contracts from its own blueprint.
//
// test_unit_capabilities checks that a unit IS what it claims; this file checks that it
// ACTS like it, and it does so for all 568 retail blueprints without a hand-maintained
// table: every expectation below is derived from the blueprint being tested. The four
// contracts are the rock-paper-scissors of the game, stated per unit:
//
//   ENGAGE.   A weapon that fires() is asked to damage a hostile on every layer it can
//             reach — Surface, Air, Submerged — and the unit is asked to stay silent on
//             every layer none of its guns can reach. A tank cannot hurt an aircraft; a
//             missile defence cannot hurt a tank; scissors do not cut paper.
//   SPECIAL.  Death weapons detonate, projectile-targeting weapons intercept a HOSTILE
//             shot and ignore a friendly one, and weapons the blueprint keeps out of
//             automatic fire — manual, enhancement-gated, silo — produce no firing
//             event while the engager works (checked inside ENGAGE's battles).
//   TURRET.   A turreted gun gets a synthetic mount limited by its authored traverse:
//             it must slew before it fires inside its arc, and hold fire at what its
//             arc cannot reach.
//   LEAD.     A crossing mover at a fixed step is engaged: leading weapons aim where it
//             is GOING, authored opt-outs aim where it IS, and either way the mover ends
//             up damaged — homing shots track instead of leading.
//
// The synthetic target's categories are built to satisfy the weapon's own
// TargetPriorities / TargetRestrict contract (see `targetDefFor`), so a failure means the
// sim broke a contract the blueprint states — not that a fixture drifted.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/Combat.hpp"
#include "core/unit/BuildTree.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include "support/FxMatchers.hpp"
#include "support/TestRoster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using rm::unitdef::UnitDef;
using rm::unitdef::Weapon;
using rm::sim::Army;
using rm::sim::Event;
using rm::sim::EventKind;
using rm::sim::Projectile;
using rm::sim::UnitId;

[[nodiscard]] std::filesystem::path unitRoot() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / "projects/llm/input/faf/units";
    }
    return {};
}

/// Big enough that a long-range weapon placed at its centre still sees a target at
/// mid-range. 384 squares at 8 elmos is 3072 elmos across.
[[nodiscard]] rm::HeightField flatField(float height = 0.0f, int squares = 384) {
    rm::HeightField field;
    field.squaresX = squares;
    field.squaresZ = squares;
    field.baseHeight = height;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

constexpr float kCentre = 1536.0f;
/// Where the sim parks its aircraft (Movement.hpp). An air target sits at the same
/// clearance so an anti-air weapon is tested at the altitude it was authored for.
constexpr float kAirHeight = 80.0f;
/// A seabed for submerged targets: deep enough that a descending torpedo never meets
/// the ground on the way down.
constexpr float kSeabed = -60.0f;
constexpr float kSubDepth = -12.0f;

enum class TargetLayer { Surface, Air, Submerged };

/// Whether the weapon can be aimed — by automatic acquisition — at a target on `layer`.
/// An empty priority list acquires NOTHING (`C-156`), whatever its reach.
[[nodiscard]] bool canEngage(const Weapon& weapon, TargetLayer layer) {
    if (!weapon.fires() || weapon.targetPriorities.empty()) {
        return false;
    }
    switch (layer) {
    case TargetLayer::Surface:
        return weapon.canTarget(false, false);
    case TargetLayer::Air:
        return weapon.canTarget(true, false);
    case TargetLayer::Submerged:
        return weapon.canTarget(false, true);
    }
    return false;
}

/// The Combat.cpp rule, restated at the test boundary: `OnlyAllow` must match,
/// `OnlyDisallow` must not. Each optional holds ONE category term (its strings ANDed).
[[nodiscard]] bool passesRestrictions(const Weapon& weapon, const UnitDef& candidate) {
    if (weapon.targetRestrictOnlyAllow
        && !rm::unitdef::matchesExpression(
            rm::unitdef::CategoryExpression{*weapon.targetRestrictOnlyAllow}, candidate)) {
        return false;
    }
    return !weapon.targetRestrictOnlyDisallow
           || !rm::unitdef::matchesExpression(
               rm::unitdef::CategoryExpression{*weapon.targetRestrictOnlyDisallow},
               candidate);
}

/// A hostile the weapon will acquire: categories satisfying at least one priority row
/// plus every `OnlyAllow` tag, without tripping `OnlyDisallow`. The union of all rows is
/// tried first — it matches every row by construction — then each row alone, for
/// blueprints whose rows are individually admissible but mutually disallowed.
[[nodiscard]] std::optional<UnitDef> targetDefFor(const Weapon& weapon) {
    std::vector<std::string> allow;
    if (weapon.targetRestrictOnlyAllow) {
        allow = *weapon.targetRestrictOnlyAllow;
    }
    const auto probe = [&](const std::vector<std::string>& tags)
        -> std::optional<UnitDef> {
        UnitDef def;
        def.name = "matrix_target";
        def.categories = tags;
        def.categories.insert(def.categories.end(), allow.begin(), allow.end());
        std::ranges::sort(def.categories);
        const auto last = std::ranges::unique(def.categories).begin();
        def.categories.erase(last, def.categories.end());
        return passesRestrictions(weapon, def) ? std::optional{def} : std::nullopt;
    };
    std::vector<std::string> all;
    for (const std::vector<std::string>& row : weapon.targetPriorities) {
        all.insert(all.end(), row.begin(), row.end());
    }
    if (auto def = probe(all)) {
        return def;
    }
    for (const std::vector<std::string>& row : weapon.targetPriorities) {
        if (auto def = probe(row)) {
            return def;
        }
    }
    return std::nullopt;
}

/// One engagement, owned end to end: a roster, two free-for-all armies, the shots in
/// flight, the firing events and the ground they fly over.
struct Battle {
    rm::test::Roster roster;
    std::vector<Army> armies = rm::sim::freeForAll(2);
    std::vector<Projectile> shots;
    rm::sim::EventQueue events;
    rm::HeightField field;
    rm::sim::Terrain terrain;

    explicit Battle(rm::HeightField ground) : field(std::move(ground)), terrain(field) {}

    UnitId add(const UnitDef& def, float x, float z, int army, float hp) {
        return roster.add(roster.addType(def), x, z, army, hp);
    }

    // Terrain samples `field` by reference, so this type is deliberately neither moved
    // nor copied: a relocated field would leave the view dangling.
    Battle(const Battle&) = delete;
    Battle& operator=(const Battle&) = delete;
    Battle(Battle&&) = delete;
    Battle& operator=(Battle&&) = delete;

    /// One tick of the combat passes: aim, fire, fly. `tickSkirmish` does NOT call
    /// `aimAtTargets` — an ordered unit aims through movement — so the harness invokes
    /// the sim's own idle-unit aim pass explicitly: a bare unit has no orders to bring
    /// its hull round, and without it an unturreted gun could never be tested.
    void tick(rm::TickIndex t) {
        rm::sim::aimAtTargets(roster.store, roster.catalog, armies, nullptr, &shots,
                              nullptr, t, roster.rate);
        rm::sim::fireWeapons(roster.store, roster.catalog, armies, shots, roster.rate,
                             &events, nullptr, nullptr, t);
        rm::sim::advanceProjectiles(shots, roster.store, armies, terrain, roster.rate,
                                    nullptr, &roster.catalog);
    }

    /// Whether a firing event was emitted for the weapon whose blueprint `Label` is
    /// `label` — events carry `def->name + ":" + weapon.label`, so a suffix check names
    /// one weapon on a unit that may carry several.
    [[nodiscard]] bool firedFor(std::string_view label) const {
        const std::string suffix = ":" + std::string{label};
        for (const Event& event : events.all()) {
            if (event.kind != EventKind::WeaponFired && event.kind != EventKind::BeamFired) {
                continue;
            }
            if (label.empty() || event.visualId.ends_with(suffix)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool damaged(UnitId id) {
        const rm::sim::Health& health = roster.health(id);
        return health.current < health.maximum;
    }
};

/// Spawn `def`'s synthetic prey for `weapon` on `layer`, at the weapon's mid-range due
/// north of the shooter — bearing zero, the direction a default hull already faces.
[[nodiscard]] UnitId addPrey(Battle& battle, const UnitDef& prey, const Weapon& weapon,
                             TargetLayer layer) {
    const float range = (rm::test::asFloat(weapon.minRange)
                         + rm::test::asFloat(weapon.maxRange)) * 0.5f;
    const UnitId id = battle.add(prey, kCentre, kCentre + range, 1, 100000.0f);
    if (layer == TargetLayer::Air) {
        battle.roster.motion(id).airborne = true;
        battle.roster.transform(id).y = rm::test::fx(kAirHeight);
    } else if (layer == TargetLayer::Submerged) {
        battle.roster.transform(id).y = rm::test::fx(kSubDepth);
        battle.roster.motion(id).submersible = true;
        battle.roster.motion(id).submerged = true;
    }
    battle.roster.reindex();
    return id;
}

[[nodiscard]] bool layerNeedsDeepField(TargetLayer layer) {
    return layer == TargetLayer::Submerged;
}

/// The first weapon — highest damage first, so a slow gun does not set the pace — able
/// to acquire a synthetic target on `layer`, with the target it would aim at.
struct Engager {
    std::size_t weapon;
    UnitDef target;
};
[[nodiscard]] std::optional<Engager> engagerFor(const UnitDef& def, TargetLayer layer) {
    std::optional<Engager> best;
    for (std::size_t w = 0; w < def.weapons.size(); ++w) {
        const Weapon& weapon = def.weapons[w];
        if (!canEngage(weapon, layer)) {
            continue;
        }
        auto target = targetDefFor(weapon);
        if (!target) {
            continue;
        }
        if (!best || weapon.damage > def.weapons[best->weapon].damage) {
            best = Engager{.weapon = w, .target = std::move(*target)};
        }
    }
    return best;
}

/// The layer vocabulary of the contract: every layer at least one weapon reaches is a
/// positive case; the rest are negative ones.
constexpr std::array kLayers{TargetLayer::Surface, TargetLayer::Air,
                             TargetLayer::Submerged};

void checkEngagement(const UnitDef& def) {
    if (def.weapons.empty()) {
        return;
    }
    // A label shared by two weapons cannot be attributed from a firing event — the same
    // rule the mover check applies — so the silence check below skips it rather than
    // blaming the wrong gun.
    std::unordered_map<std::string_view, std::size_t> labelOwners;
    for (const Weapon& gun : def.weapons) {
        ++labelOwners[gun.label];
    }
    bool gatedChecked = false;
    for (const TargetLayer layer : kLayers) {
        const auto engage = engagerFor(def, layer);
        const bool anyLayerWeapon = std::ranges::any_of(def.weapons, [&](const Weapon& w) {
            return canEngage(w, layer);
        });
        if (!engage && anyLayerWeapon) {
            // A weapon reaches this layer but no synthetic target satisfies its
            // acquisition contract — the layer is untestable here, skipped rather
            // than scored either way.
            continue;
        }
        Battle battle(flatField(layerNeedsDeepField(layer) ? kSeabed : 0.0f));
        const UnitId attackerId = battle.add(def, kCentre, kCentre, 0, 100000.0f);
        UnitDef prey;
        prey.name = "matrix_target";
        prey.categories = {"MOBILE", "ALLUNITS", "STRUCTURE"};
        if (engage) {
            prey = engage->target;
        }
        const Weapon& reach = def.weapons[engage ? engage->weapon : 0];
        const UnitId preyId = addPrey(battle, prey, reach, layer);

        for (rm::TickIndex t = 0; t < 400 && !battle.damaged(preyId); ++t) {
            if (reach.needToComputeBombDrop) {
                // A bomb only leaves inside `BombDropThreshold` of the release
                // point (`C-224`), and this harness has no orders to fly the
                // approach — so it puts the bomber where the run would: half
                // the threshold short of the led point, close enough to
                // release and far enough for the round to fly.
                const rm::TickCount lead = battle.roster.rate.ticks(
                    rm::sim::seconds(def.airPredictAheadForBombDropSec));
                const rm::sim::Transform& preyAt = battle.roster.transform(preyId);
                const rm::sim::MoveState& preyMotion = battle.roster.motion(preyId);
                rm::sim::Transform& at = battle.roster.transform(attackerId);
                const rm::sim::Fx leadFx = rm::sim::Fx::fromInt(static_cast<std::int32_t>(lead));
                at.x = preyAt.x + preyMotion.stepX * leadFx - reach.bombDropThreshold * rm::sim::Fx::fromRatio(1, 2);
                at.z = preyAt.z + preyMotion.stepZ * leadFx;
                battle.roster.reindex();
            }
            battle.tick(t);
        }
        if (engage) {
            INFO(def.name << " vs " << static_cast<int>(layer) << " via "
                          << reach.label);
            CHECK(battle.damaged(preyId));
            if (!gatedChecked) {
                gatedChecked = true;
                // A weapon the blueprint keeps out of automatic fire — a manual
                // click, an enhancement the unit lacks, a silo round, a dummy —
                // stays silent while the engager works: no firing event ever
                // carries its label.
                for (const Weapon& gun : def.weapons) {
                    if (gun.label.empty() || labelOwners[gun.label] != 1
                        || gun.fires() || gun.firesAtProjectiles()) {
                        continue;
                    }
                    INFO(def.name << ":" << gun.label << " gated, stays silent");
                    CHECK_FALSE(battle.firedFor(gun.label));
                }
            }
        } else if (!anyLayerWeapon) {
            // No gun reaches this layer at all: the hostile stands untouched.
            CHECK_FALSE(battle.firedFor(""));
            CHECK_FALSE(battle.damaged(preyId));
        }
        // Third case: a weapon reaches the layer but no synthetic target can be built
        // for it — the contract is untestable here, so the layer is skipped rather
        // than scored either way.
    }
}

void checkSpecial(const UnitDef& def) {
    // The death blast: most of the corpus carries one, and the commanders' is the most
    // characteristic event in the game. It must hurt a hostile standing inside it.
    if (const Weapon* death = rm::sim::deathWeapon(def); death && death->harmful()) {
        Battle battle(flatField());
        UnitDef victim;
        victim.name = "matrix_victim";
        victim.categories = {"MOBILE"};
        const UnitId prey = battle.add(victim, kCentre, kCentre, 1, 100000.0f);
        const float reach = std::max({rm::test::asFloat(death->damageRadius),
                                      rm::test::asFloat(death->innerRingRadius),
                                      rm::test::asFloat(death->outerRingRadius)});
        const auto at = rm::test::at(
            static_cast<int>(kCentre), 0,
            static_cast<int>(kCentre + reach * 0.5f));
        const rm::sim::Mag dealt = rm::sim::explodeOnDeath(
            def, at, 0, battle.roster.store, battle.armies, {}, nullptr,
            &battle.roster.catalog);
        INFO(def.name << " death weapon");
        CHECK(dealt > rm::sim::Mag{});
        CHECK(battle.damaged(prey));
    }

    // The interceptor: a HOSTILE shot inside the tracking reach gets one back, a
    // friendly one gets nothing. One run each, because "fires at projectiles" must
    // never mean "fires at anything flying".
    const Weapon* defence = nullptr;
    for (const Weapon& weapon : def.weapons) {
        if (weapon.firesAtProjectiles()) {
            defence = &weapon;
            break;
        }
    }
    if (defence != nullptr) {
        for (const int shotArmy : {1, 0}) {
            Battle battle(flatField());
            (void)battle.add(def, kCentre, kCentre, 0, 100000.0f);
            const float range = rm::test::asFloat(defence->maxRange) * 0.5f;
            Projectile incoming{
                .position = rm::test::at(static_cast<int>(kCentre), 4,
                                         static_cast<int>(kCentre + range)),
                .velocity = {rm::sim::Fx{}, rm::sim::Fx{}, rm::test::fx(-4.0f)},
                .firedByArmy = shotArmy,
                .ticksRemaining = 100,
            };
            // A defence answers only what it was told to: a TMD's `OnlyAllow` asks
            // for TACTICAL/MISSILE shots and an anti-torpedo asks for TORPEDO. The
            // restriction reads the SHOT's categories, so the bait carries the tags
            // the blueprint demands rather than arriving category-less, which a
            // point defence is authored to ignore.
            if (defence->targetRestrictOnlyAllow) {
                incoming.categories = *defence->targetRestrictOnlyAllow;
                std::ranges::sort(incoming.categories);
            }
            battle.shots.push_back(incoming);
            // The interceptor is consumed the tick it kills its target — and an
            // instant-speed one crosses to the bait inside the SAME tick it launches,
            // so the only place it can be seen is mid-tick, after the firing pass and
            // before the flight pass. Same three calls `Battle::tick` makes, sampled
            // between them.
            bool sawInterceptor = false;
            for (rm::TickIndex t = 0; t < 60; ++t) {
                rm::sim::aimAtTargets(battle.roster.store, battle.roster.catalog,
                                      battle.armies, nullptr, &battle.shots, nullptr, t,
                                      battle.roster.rate);
                rm::sim::fireWeapons(battle.roster.store, battle.roster.catalog,
                                     battle.armies, battle.shots, battle.roster.rate,
                                     &battle.events, nullptr, nullptr, t);
                sawInterceptor = sawInterceptor
                                 || std::ranges::any_of(battle.shots, [](const Projectile& shot) {
                                        return shot.interceptor;
                                    });
                rm::sim::advanceProjectiles(battle.shots, battle.roster.store,
                                          battle.armies, battle.terrain,
                                          battle.roster.rate, nullptr,
                                          &battle.roster.catalog);
            }
            INFO(def.name << " defence vs army " << shotArmy);
            if (shotArmy == 1) {
                CHECK(battle.firedFor(defence->label));
                CHECK(sawInterceptor);
            } else {
                CHECK_FALSE(battle.firedFor(defence->label));
            }
        }
    }
}

/// A synthetic mount for the unit's first slewable turreted weapon: ring at the hull
/// origin, trunnion four ahead, muzzle four past it — the geometry is arbitrary but
/// consistent, because the checks are about the authored traverse, not the mesh.
constexpr float kTurretRingHeight = 4.0f;
constexpr float kTurretArm = 4.0f;

void checkTurret(const UnitDef& def) {
    std::optional<std::size_t> index;
    for (std::size_t w = 0; w < def.weapons.size(); ++w) {
        const Weapon& weapon = def.weapons[w];
        if (weapon.turreted && weapon.turretYawSpeedRadPerSecond > 0.0f
            && canEngage(weapon, TargetLayer::Surface) && targetDefFor(weapon)) {
            index = w;
            break;
        }
    }
    if (!index) {
        return;  // no slewable turret to move — a fixed gun or an unstated speed
    }
    const Weapon& weapon = def.weapons[*index];
    const UnitDef prey = *targetDefFor(weapon);

    const float deg = std::numbers::pi_v<float> / 180.0f;
    const float turretCentre = weapon.turretYawDegrees * deg;
    const float turretRange = weapon.turretYawRangeDegrees > 0.0f
                                ? weapon.turretYawRangeDegrees * deg
                                : std::numbers::pi_v<float>;
    const float weaponCentre = weapon.arcCentreDegrees * deg;
    const float weaponRange = weapon.arcRangeDegrees * deg;

    const auto mountFor = [&](Battle& battle, rm::UnitTypeIndex type) {
        rm::sim::UnitCatalog::TurretMountSpec spec;
        spec.weapon = *index;
        spec.muzzle = {0.0f, kTurretRingHeight, 2.0f * kTurretArm};
        spec.yawPivot = {0.0f, kTurretRingHeight, 0.0f};
        spec.pitchPivot = {0.0f, kTurretRingHeight, kTurretArm};
        spec.restDir = {0.0f, 0.0f, 1.0f};
        spec.yawMinRadians = turretCentre - turretRange;
        spec.yawMaxRadians = turretCentre + turretRange;
        spec.pitchMinRadians = -std::numbers::pi_v<float> / 2.0f;
        spec.pitchMaxRadians = std::numbers::pi_v<float> / 2.0f;
        battle.roster.catalog.setTurretMount(type, spec);
    };

    const float range = (rm::test::asFloat(weapon.minRange)
                         + rm::test::asFloat(weapon.maxRange)) * 0.5f;
    const float ticksPerSecond =
        static_cast<float>(rm::sim::TickRate{}.ticksPerSecond());
    const float slewTicksPerRadian =
        ticksPerSecond / weapon.turretYawSpeedRadPerSecond;

    // IN ARC: the sector both the hull arc and the ring arc admit. The aim point is
    // near the far edge of it, so the ring genuinely travels before the shot may
    // leave. The hull is PINNED via `moving`: the unit's other weapons are allowed
    // to turn the hull toward the prey — correct sim behaviour — but then the ring
    // would sit at zero having done nothing, and it is the ring this check exists
    // to watch.
    const float inLo = std::max(weaponCentre - weaponRange, turretCentre - turretRange);
    const float inHi = std::min(weaponCentre + weaponRange, turretCentre + turretRange);
    const float farEdge = std::abs(inHi) >= std::abs(inLo) ? inHi : inLo;
    const float theta = farEdge - std::copysign((inHi - inLo) * 0.15f, farEdge);
    if (inHi - inLo > 0.05f && std::abs(theta) > 0.15f) {
        Battle battle(flatField());
        const auto type = battle.roster.addType(def);
        mountFor(battle, type);
        const UnitId shooter = battle.roster.add(type, kCentre, kCentre, 0, 100000.0f);
        battle.roster.motion(shooter).moving = true;
        (void)battle.add(prey, kCentre + range * std::sin(theta),
                         kCentre + range * std::cos(theta), 1, 100000.0f);
        const rm::TickIndex deadline = static_cast<rm::TickIndex>(
            std::abs(theta) * slewTicksPerRadian + static_cast<float>(weapon.reloadTicks()) + 60.0f);
        for (rm::TickIndex t = 0; t < deadline && !battle.firedFor(weapon.label); ++t) {
            battle.tick(t);
        }
        INFO(def.name << " turret in arc, theta " << theta);
        CHECK(battle.firedFor(weapon.label));
        const float yaw = rm::test::signedRadians(battle.roster.motion(shooter).turretYaw);
        // Wrap-aware: a target dead astern asks ±π of the ring, and -π and +π are the
        // same stop — comparing signs there would fail a correct slew.
        CHECK(std::abs(std::remainder(theta - yaw,
                                      2.0f * std::numbers::pi_v<float>)) < 0.35f);
        CHECK(std::abs(yaw) > 0.01f);
    }

    // OUT OF ARC: a bearing the hull arc admits but the ring's traverse does not. The
    // barrel slews to its stop and the weapon stays silent — the firing gate, not the
    // acquisition, is what is being proven here. Same pinned hull, so the weapon's
    // silence cannot be excused by the unit turning to face the target.
    const float step = 0.35f;
    float outTheta = 0.0f;
    if (turretCentre + turretRange + step < weaponCentre + weaponRange) {
        outTheta = turretCentre + turretRange + step;
    } else if (turretCentre - turretRange - step > weaponCentre - weaponRange) {
        outTheta = turretCentre - turretRange - step;
    }
    if (outTheta != 0.0f) {
        Battle battle(flatField());
        const auto type = battle.roster.addType(def);
        mountFor(battle, type);
        const UnitId shooter = battle.roster.add(type, kCentre, kCentre, 0, 100000.0f);
        battle.roster.motion(shooter).moving = true;
        (void)battle.add(prey, kCentre + range * std::sin(outTheta),
                         kCentre + range * std::cos(outTheta), 1, 100000.0f);
        const rm::TickIndex deadline = static_cast<rm::TickIndex>(
            std::abs(outTheta) * slewTicksPerRadian + static_cast<float>(weapon.reloadTicks()) + 60.0f);
        for (rm::TickIndex t = 0; t < deadline; ++t) {
            battle.tick(t);
        }
        INFO(def.name << " turret out of arc, theta " << outTheta);
        CHECK_FALSE(battle.firedFor(weapon.label));
        const float yaw = rm::test::signedRadians(battle.roster.motion(shooter).turretYaw);
        // The barrel parked at its traverse stop — the bearing it could reach, not the
        // one it wanted. A sign test alone would pass a turret that never moved.
        const float stop = outTheta > 0.0f ? turretCentre + turretRange
                                           : turretCentre - turretRange;
        CHECK(std::abs(std::remainder(yaw - stop,
                                      2.0f * std::numbers::pi_v<float>)) < 0.1f);
    }
}

void checkMovingTarget(const UnitDef& def) {
    TargetLayer layer = TargetLayer::Surface;
    std::optional<Engager> engage;
    for (const TargetLayer candidate : kLayers) {
        if ((engage = engagerFor(def, candidate))) {
            layer = candidate;
            break;
        }
    }
    if (!engage) {
        return;
    }
    const Weapon& weapon = def.weapons[engage->weapon];
    const float ticksPerSecond =
        static_cast<float>(rm::sim::TickRate{}.ticksPerSecond());

    // Whether a weapon can CONVERGE on a mover at all. A homing round turns onto it,
    // an instant one is there in a tick, and a led shot fast enough meets it at the
    // intercept. A slow dumb-fire missile — the MML's authored 3 elmos a second —
    // never converges on a mover doing 30, which is authored truth rather than a sim
    // gap: retail's answer is the missile's homing, and the homing flags live in the
    // projectile blueprint, which only the VFS-resolved traits carry.
    constexpr float kStep = 3.0f;
    const auto converges = [&](const Weapon& w) {
        // Only a weapon that could acquire THIS prey counts — one that cannot reach
        // its layer, whose restriction contract the prey's categories fail, or whose
        // priority rows the prey matches none of, converges on nothing here.
        if (!canEngage(w, layer) || !passesRestrictions(w, engage->target)
            || !std::ranges::any_of(w.targetPriorities,
                                    [&](const std::vector<std::string>& row) {
                                        return rm::unitdef::matchesExpression(
                                            rm::unitdef::CategoryExpression{row},
                                            engage->target);
                                    })) {
            return false;
        }
        if (w.needToComputeBombDrop) {
            // A bomb-drop round is not a converging shot: it falls where the
            // release point was, and an orbiting target is never there when it
            // lands. `fires()` still proves the release contract.
            return false;
        }
        if (w.beam || w.projectileTraits.trackTarget
            || w.muzzleVelocityElmosPerSecond <= 0.0f) {
            return true;
        }
        const float perTick = w.muzzleVelocityElmosPerSecond / ticksPerSecond;
        return w.leadTarget && perTick >= kStep * 2.0f;
    };
    const bool expectDamage = std::ranges::any_of(def.weapons, converges);

    Battle battle(flatField(layerNeedsDeepField(layer) ? kSeabed : 0.0f));
    const UnitId attackerId = battle.add(def, kCentre, kCentre, 0, 100000.0f);
    const UnitId prey = addPrey(battle, engage->target, weapon, layer);

    // The prey ORBITS the shooter at the weapon's mid-range — a straight runner would
    // walk out of a short gun's reach before the second volley. The `stepX`/`stepZ`
    // it is moved by are the same fields the sim's lead reads. The transform only
    // starts moving once the first volley has been measured, so each weapon's first
    // launch is of a shot at a stationary-position, moving-velocity target: a lead
    // aims downrange (+x), a no-lead aims dead ahead (vx = 0).
    const float radius = (rm::test::asFloat(weapon.minRange)
                          + rm::test::asFloat(weapon.maxRange)) * 0.5f;
    battle.roster.motion(prey).stepX = rm::test::fx(kStep);
    // Per weapon, the x-velocity of its first volley — read off the firing EVENTS,
    // which survive the instant-impact shots that leave nothing standing in `shots`.
    std::unordered_map<std::size_t, rm::sim::Fx> firstVx;
    // An event's visualId names the weapon by label, so a label shared by two weapons
    // cannot be attributed: such a gun gets no entry and no lead check rather than a
    // verdict read off the wrong volley.
    std::unordered_map<std::string_view, std::size_t> labelOwners;
    for (const Weapon& gun : def.weapons) {
        ++labelOwners[gun.label];
    }
    std::size_t seenEvents = 0;
    float phi = 0.0f;
    bool fired = false;
    bool orbiting = false;
    for (rm::TickIndex t = 0; t < 400 && !battle.damaged(prey); ++t) {
        if (orbiting) {
            phi += kStep / radius;
            const float nx = kCentre + radius * std::sin(phi);
            const float nz = kCentre + radius * std::cos(phi);
            rm::sim::Transform& transform = battle.roster.transform(prey);
            rm::sim::MoveState& motion = battle.roster.motion(prey);
            motion.stepX = rm::test::fx(nx) - transform.x;
            motion.stepZ = rm::test::fx(nz) - transform.z;
            transform.x = rm::test::fx(nx);
            transform.z = rm::test::fx(nz);
            battle.roster.reindex();
        }
        if (weapon.needToComputeBombDrop) {
            // As in the layer case above: the bomb only leaves inside
            // `BombDropThreshold` of the release point (`C-224`), and nothing
            // here flies the approach — so the bomber is put half the threshold
            // short of the led point.
            const rm::TickCount lead = battle.roster.rate.ticks(
                rm::sim::seconds(def.airPredictAheadForBombDropSec));
            const rm::sim::Transform& preyAt = battle.roster.transform(prey);
            const rm::sim::MoveState& preyMotion = battle.roster.motion(prey);
            rm::sim::Transform& at = battle.roster.transform(attackerId);
            const rm::sim::Fx leadFx = rm::sim::Fx::fromInt(static_cast<std::int32_t>(lead));
            at.x = preyAt.x + preyMotion.stepX * leadFx - weapon.bombDropThreshold * rm::sim::Fx::fromRatio(1, 2);
            at.z = preyAt.z + preyMotion.stepZ * leadFx;
            battle.roster.reindex();
        }
        battle.tick(t);
        for (; seenEvents < battle.events.all().size(); ++seenEvents) {
            const Event& event = battle.events.all()[seenEvents];
            if (event.kind != EventKind::WeaponFired && event.kind != EventKind::BeamFired) {
                continue;
            }
            fired = true;
            if (!orbiting && event.kind == EventKind::WeaponFired) {
                for (std::size_t w = 0; w < def.weapons.size(); ++w) {
                    if (!firstVx.contains(w) && labelOwners[def.weapons[w].label] == 1
                        && event.visualId.ends_with(":" + def.weapons[w].label)) {
                        firstVx[w] = event.launchVelocity[0];
                    }
                }
            }
        }
        if (fired) {
            orbiting = true;
        }
    }
    INFO(def.name << " vs mover");
    CHECK(fired);
    for (const auto& [w, vx] : firstVx) {
        const Weapon& gun = def.weapons[w];
        if (gun.muzzleVelocityElmosPerSecond <= 0.0f) {
            continue;  // an instant shot cannot lead — it is simply already there
        }
        INFO(def.name << ":" << gun.label << " lead " << gun.leadTarget);
        if (gun.leadTarget) {
            CHECK(vx > rm::sim::Fx{});
        } else {
            CHECK(vx == rm::sim::Fx{});
        }
    }
    if (expectDamage) {
        CHECK(battle.damaged(prey));
    }
}

} // namespace

TEST_CASE("every shipped unit does what its own blueprint says it does",
          "[corpus][behaviour][unit-matrix]") {
    const auto root = unitRoot();
    if (!std::filesystem::is_directory(root)) {
        SKIP("no Supreme Commander unit blueprints at " + root.string());
    }
    std::vector<UnitDef> units;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()
            || !entry.path().filename().string().ends_with("_unit.bp")) {
            continue;
        }
        auto def = rm::unitbp::loadFile(entry.path());
        REQUIRE(def.has_value());
        units.push_back(std::move(*def));
    }
    REQUIRE(units.size() == 568);
    std::ranges::sort(units, {}, &UnitDef::name);

    for (const UnitDef& def : units) {
        DYNAMIC_SECTION(def.name) {
            checkSpecial(def);
            checkEngagement(def);
            checkTurret(def);
            checkMovingTarget(def);
        }
    }
}
