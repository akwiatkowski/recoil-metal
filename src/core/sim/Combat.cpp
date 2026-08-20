#include "core/sim/Combat.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm::sim {
namespace {

/// The army that owns a group's instance, or kNoArmy when the group has no motion state
/// for it. A group whose arrays disagree in length is a caller bug, and reading past the
/// shorter one would be a far worse answer than "nobody owns it".
[[nodiscard]] int armyAt(const CombatGroup& group, std::size_t instance) noexcept {
    if (instance >= group.motion.size()) {
        return kNoArmy;
    }
    return group.motion[instance].armyIndex;
}

/// The Army record for an index, or null. An index with no army is not an error: the
/// scene may hold unowned instances, and they take no part in a fight.
[[nodiscard]] const Army* armyFor(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) {
            return &army;
        }
    }
    return nullptr;
}

/// Whether `attacker` may shoot the unit at `instance` of `group`.
[[nodiscard]] bool shootable(int attackerArmy, const CombatGroup& group, std::size_t instance,
                            std::span<const Army> armies) noexcept {
    if (instance >= group.health.size() || !group.health[instance].alive()) {
        return false;
    }
    const Army* mine = armyFor(attackerArmy, armies);
    const Army* theirs = armyFor(armyAt(group, instance), armies);
    if (mine == nullptr || theirs == nullptr) {
        return false;  // an unowned unit is nobody's target
    }
    return hostile(*mine, *theirs);
}

/// The nearest hostile unit a shot has reached, or nothing.
///
/// The tolerance is the TARGET's own size plus the shot's blast radius, so a big unit is
/// easier to hit than a small one and a shell with a wide blast need not touch. A floor of
/// one tick of travel stops a fast shot stepping cleanly over a unit between two ticks,
/// which is the discrete-time version of a bullet passing through a wall.
[[nodiscard]] std::optional<UnitRef> nearestStruck(const Projectile& shot,
                                                   std::span<const CombatGroup> groups,
                                                   std::span<const Army> armies) {
    const float travelPerTick = std::sqrt(shot.velocity[0] * shot.velocity[0]
                                          + shot.velocity[1] * shot.velocity[1]
                                          + shot.velocity[2] * shot.velocity[2])
                              * kTickSeconds;

    std::optional<UnitRef> best;
    float bestDistance = 0.0f;

    for (std::size_t g = 0; g < groups.size(); ++g) {
        const CombatGroup& group = groups[g];
        for (std::size_t i = 0; i < group.instances.size(); ++i) {
            if (!shootable(shot.firedByArmy, group, i, armies)) {
                continue;
            }
            const float size = i < group.motion.size() ? group.motion[i].radiusElmos : 0.0f;
            const float tolerance = std::max(travelPerTick, size + shot.damageRadiusElmos);

            const float distance =
                groundDistanceElmos(shot.position, group.instances[i].position);
            if (distance > tolerance) {
                continue;
            }
            if (!best || distance < bestDistance) {
                best = UnitRef{.batch = g, .instance = i};
                bestDistance = distance;
            }
        }
    }
    return best;
}

} // namespace

float groundDistanceElmos(std::array<float, 3> from, std::array<float, 3> to) noexcept {
    const float dx = to[0] - from[0];
    const float dz = to[2] - from[2];
    return std::sqrt(dx * dx + dz * dz);
}

std::optional<UnitRef> nearestTarget(std::array<float, 3> from, int fromArmy,
                                     const unitdef::Weapon& weapon,
                                     std::span<const CombatGroup> groups,
                                     std::span<const Army> armies) {
    if (!weapon.fires()) {
        return std::nullopt;
    }

    std::optional<UnitRef> best;
    float bestDistance = 0.0f;

    for (std::size_t g = 0; g < groups.size(); ++g) {
        const CombatGroup& group = groups[g];
        for (std::size_t i = 0; i < group.instances.size(); ++i) {
            if (!shootable(fromArmy, group, i, armies)) {
                continue;
            }

            const float distance = groundDistanceElmos(from, group.instances[i].position);
            if (distance > weapon.maxRangeElmos || distance < weapon.minRangeElmos) {
                continue;
            }

            // Strictly nearer, so ties fall to the lower batch and then the lower
            // instance — the iteration order. That is what makes the same scene pick the
            // same target twice, which a screenshot depends on.
            if (!best || distance < bestDistance) {
                best = UnitRef{.batch = g, .instance = i};
                bestDistance = distance;
            }
        }
    }

    return best;
}

float bearingTo(std::array<float, 3> from, std::array<float, 3> to) noexcept {
    return std::atan2(to[0] - from[0], to[2] - from[2]);
}

float headingError(float from, float to) noexcept {
    float error = to - from;
    // Wrapped into -pi..pi, so the shorter way round is what is measured — otherwise a unit
    // one degree the wrong side of north reads as 359 degrees off and turns the long way.
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    while (error > std::numbers::pi_v<float>) {
        error -= kTwoPi;
    }
    while (error < -std::numbers::pi_v<float>) {
        error += kTwoPi;
    }
    return std::abs(error);
}

bool canFireAt(const unitdef::Weapon& weapon, float yaw, float bearing) noexcept {
    if (weapon.turreted) {
        return true;
    }
    const float tolerance =
        std::max(0.0f, weapon.firingToleranceDegrees) * std::numbers::pi_v<float> / 180.0f;
    return headingError(yaw, bearing) <= tolerance;
}

std::size_t aimAtTargets(std::span<UnitInstance> instances, std::span<const MoveState> motion,
                         const unitdef::UnitDef* def, std::span<const CombatGroup> groups,
                         std::span<const Army> armies) {
    if (def == nullptr) {
        return 0;
    }

    // Only the weapons that need the hull pointed are worth turning for, and the longest
    // reach among them decides how far away a unit bothers to aim.
    float reach = 0.0f;
    for (const unitdef::Weapon& weapon : def->weapons) {
        if (weapon.fires() && !weapon.turreted) {
            reach = std::max(reach, weapon.maxRangeElmos);
        }
    }
    if (reach <= 0.0f) {
        return 0;
    }

    std::size_t turned = 0;
    for (std::size_t i = 0; i < instances.size() && i < motion.size(); ++i) {
        if (motion[i].moving) {
            continue;  // an order is already deciding where this one points
        }
        if (motion[i].radiusElmos <= 0.0f) {
            continue;  // retired
        }

        // The nearest thing any of its hull-aimed weapons could reach. Built as a stand-in
        // weapon rather than looping over the real ones, because the answer is the same for
        // all of them and the sweep is the expensive part.
        unitdef::Weapon sweep;
        sweep.role = unitdef::WeaponRole::DirectFire;
        sweep.damage = 1.0f;
        sweep.rateOfFire = 1.0f;
        sweep.maxRangeElmos = reach;

        const std::optional<UnitRef> target =
            nearestTarget(instances[i].position, motion[i].armyIndex, sweep, groups, armies);
        if (!target) {
            continue;
        }

        const std::array<float, 3>& at =
            groups[target->batch].instances[target->instance].position;
        const float bearing = bearingTo(instances[i].position, at);
        const float error = headingError(instances[i].rotationY, bearing);
        if (error <= 1e-4f) {
            continue;
        }

        // Turned at the unit's OWN rate, so a slow hull is slow to bring its gun to bear —
        // which is the whole reason `turnrate` is read off the blueprint.
        const float step = motion[i].turnRateRadiansPerSecond * kTickSeconds;
        float delta = bearing - instances[i].rotationY;
        constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
        while (delta > std::numbers::pi_v<float>) {
            delta -= kTwoPi;
        }
        while (delta < -std::numbers::pi_v<float>) {
            delta += kTwoPi;
        }
        instances[i].rotationY += std::clamp(delta, -step, step);
        ++turned;
    }
    return turned;
}

std::size_t fireWeapons(std::span<const CombatGroup> groups, std::span<const Army> armies,
                        std::vector<Projectile>& projectiles) {
    std::size_t fired = 0;

    for (const CombatGroup& group : groups) {
        if (group.def == nullptr || group.def->weapons.empty()) {
            continue;
        }

        for (std::size_t i = 0; i < group.instances.size(); ++i) {
            if (i >= group.health.size() || !group.health[i].alive()) {
                continue;  // the dead do not shoot
            }

            Health& health = group.health[i];
            health.reloadRemaining.resize(group.def->weapons.size(), 0);

            const int army = armyAt(group, i);
            const std::array<float, 3> from = group.instances[i].position;

            for (std::size_t w = 0; w < group.def->weapons.size(); ++w) {
                const unitdef::Weapon& weapon = group.def->weapons[w];
                if (!weapon.fires()) {
                    continue;
                }

                // Tick the reload down FIRST, then fire if it is ready. Checking before
                // decrementing puts the shots reloadTicks + 1 apart, which is a rate of
                // fire 10% slow at ten ticks and 50% slow at two — a discrepancy that
                // reads as a balance complaint rather than as an off-by-one.
                //
                // The reload runs whether or not there is anything to shoot, so a unit
                // that comes into contact fires immediately rather than starting a fresh
                // reload on sighting — which is what `defaultweapons.lua` does, and the
                // difference between an ambush working and not.
                if (health.reloadRemaining[w] > 0) {
                    --health.reloadRemaining[w];
                }
                if (health.reloadRemaining[w] > 0) {
                    continue;
                }

                const std::optional<UnitRef> target =
                    nearestTarget(from, army, weapon, groups, armies);
                if (!target) {
                    continue;
                }

                const std::array<float, 3> to =
                    groups[target->batch].instances[target->instance].position;

                // Pointing at it? A turreted weapon always is; an unturreted one has to be
                // brought round, which `aimAtTargets` does. The reload is NOT consumed while
                // turning: a unit that spent its shot waiting to line up would fire far more
                // slowly than its blueprint says.
                if (!canFireAt(weapon, group.instances[i].rotationY, bearingTo(from, to))) {
                    continue;
                }

                projectiles.push_back(
                    launch(from, to, weapon, army));
                health.reloadRemaining[w] = weapon.reloadTicks();
                ++fired;
            }
        }
    }

    return fired;
}

Projectile launch(std::array<float, 3> from, std::array<float, 3> to,
                  const unitdef::Weapon& weapon, int byArmy) {
    Projectile shot;
    shot.position = {from[0], from[1] + kMuzzleHeightElmos, from[2]};
    shot.damage = weapon.damage;
    shot.damageRadiusElmos = weapon.damageRadiusElmos;
    shot.firedByArmy = byArmy;
    shot.arc = weapon.arc;
    shot.ticksRemaining = kProjectileLifetimeTicks;

    // Aimed from the MUZZLE at the target's middle, not from foot to foot: a shot that
    // leaves four elmos up and is aimed level would sail over its target.
    const float dx = to[0] - shot.position[0];
    const float dy = (to[1] + kMuzzleHeightElmos * 0.5f) - shot.position[1];
    const float dz = to[2] - shot.position[2];
    const float ground = std::sqrt(dx * dx + dz * dz);

    // A muzzle velocity of zero is a weapon whose shot is instantaneous — 111 of the 494
    // state one. Rather than special-case an instant hit, it is given a speed that
    // crosses its own maximum range in a single tick, so one code path carries every
    // shot and the arithmetic below never divides by zero.
    const float speed = weapon.muzzleVelocityElmosPerSecond > 0.0f
                          ? weapon.muzzleVelocityElmosPerSecond
                          : weapon.maxRangeElmos * static_cast<float>(kTicksPerSecond);

    if (ground <= 0.0f) {
        // Straight up, or at something in the same spot. Neither is worth a division.
        shot.velocity = {0.0f, dy >= 0.0f ? speed : -speed, 0.0f};
        return shot;
    }

    const float flightSeconds = ground / speed;

    // FLAT: point at the target and let it fly. ARCED: the same horizontal velocity,
    // with the vertical component chosen so gravity brings it down exactly where the
    // target is — which is the closed-form solution rather than an iteration, because
    // the flight time is already known from the horizontal distance.
    //
    //   y = y0 + vy*t - g*t^2/2   solved for vy at t = flightSeconds
    const float vy = shot.arc == unitdef::BallisticArc::None
                       ? dy / flightSeconds
                       : dy / flightSeconds
                             + 0.5f * kProjectileGravityElmosPerSecond2 * flightSeconds;

    shot.velocity = {dx / flightSeconds, vy, dz / flightSeconds};
    return shot;
}

float damageArea(std::array<float, 3> centre, float radiusElmos, float damage, int byArmy,
                 std::span<CombatGroup> groups, std::span<const Army> armies) {
    float dealt = 0.0f;

    for (CombatGroup& group : groups) {
        for (std::size_t i = 0; i < group.instances.size(); ++i) {
            if (!shootable(byArmy, group, i, armies)) {
                continue;
            }

            const float distance = groundDistanceElmos(centre, group.instances[i].position);

            float share = 0.0f;
            if (radiusElmos <= 0.0f) {
                // A point hit. Only what is essentially AT the centre takes it, and the
                // tolerance is the unit's own radius rather than zero — a shot aimed at
                // a unit's position that lands a tenth of an elmo away has hit it.
                const float tolerance = std::max(1.0f, group.motion.size() > i
                                                           ? group.motion[i].radiusElmos
                                                           : 1.0f);
                share = distance <= tolerance ? 1.0f : 0.0f;
            } else {
                // Linear from full at the centre to nothing at the rim.
                share = std::max(0.0f, 1.0f - distance / radiusElmos);
            }

            if (share <= 0.0f) {
                continue;
            }

            const float applied = std::min(group.health[i].current, damage * share);
            group.health[i].current -= applied;
            dealt += applied;
        }
    }

    return dealt;
}

void advanceProjectiles(std::vector<Projectile>& projectiles, std::span<CombatGroup> groups,
                        std::span<const Army> armies, const HeightField& field) {
    for (Projectile& shot : projectiles) {
        if (shot.ticksRemaining <= 0) {
            continue;
        }
        --shot.ticksRemaining;

        if (shot.arc != unitdef::BallisticArc::None) {
            shot.velocity[1] -= kProjectileGravityElmosPerSecond2 * kTickSeconds;
        }

        for (std::size_t axis = 0; axis < 3; ++axis) {
            shot.position[axis] += shot.velocity[axis] * kTickSeconds;
        }

        // TWO ways a shot ends, and both are needed.
        //
        // It HITS something: the nearest hostile within a tolerance of where the shot now
        // is. Without this a flat shot aimed level across flat ground never descends, so
        // it flies over its target and expires — a weapon that reliably misses, which is
        // worse than one that does not fire at all.
        //
        // Or it reaches the GROUND, which is what a miss does. Height is the test there
        // rather than proximity, because a near miss must land rather than fly on and hit
        // whatever happens to be behind it.
        const std::optional<UnitRef> struck = nearestStruck(shot, groups, armies);
        const float ground = field.heightAtWorld(shot.position[0], shot.position[2]);

        if (!struck && shot.position[1] > ground) {
            continue;
        }

        if (struck) {
            shot.position = groups[struck->batch].instances[struck->instance].position;
        } else {
            shot.position[1] = ground;
        }
        damageArea(shot.position, shot.damageRadiusElmos, shot.damage, shot.firedByArmy, groups,
                   armies);
        shot.ticksRemaining = 0;
    }

    // Spent and expired shots go together, after the pass rather than during it, so the
    // list is not being resized while it is walked.
    std::erase_if(projectiles,
                  [](const Projectile& shot) { return shot.ticksRemaining <= 0; });
}

const unitdef::Weapon* deathWeapon(const unitdef::UnitDef& def) noexcept {
    for (const unitdef::Weapon& weapon : def.weapons) {
        if (weapon.role == unitdef::WeaponRole::Death) {
            return &weapon;
        }
    }
    return nullptr;
}

float explodeOnDeath(const unitdef::UnitDef& def, std::array<float, 3> at, int byArmy,
                     std::span<CombatGroup> groups, std::span<const Army> armies) {
    const unitdef::Weapon* blast = deathWeapon(def);
    if (blast == nullptr || !blast->harmful()) {
        return 0.0f;
    }

    // TWO RINGS when the weapon states them — the four commanders do — applied outer first
    // and inner second. The order matters: `damageArea` falls off to nothing at its rim, so
    // running the wide weak ring first and the narrow strong one after means anything close
    // takes both, which is what a nested blast should do. Reversed, the outer ring would be
    // finishing off things the inner one had already flattened.
    if (blast->hasRings()) {
        float dealt = 0.0f;
        dealt += damageArea(at, blast->outerRingRadiusElmos, blast->outerRingDamage, byArmy,
                            groups, armies);
        dealt += damageArea(at, blast->innerRingRadiusElmos, blast->innerRingDamage, byArmy,
                            groups, armies);
        return dealt;
    }

    return damageArea(at, blast->damageRadiusElmos, blast->damage, byArmy, groups, armies);
}

std::vector<UnitRef> deadUnits(std::span<const CombatGroup> groups) {
    std::vector<UnitRef> dead;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (std::size_t i = 0; i < groups[g].health.size(); ++i) {
            if (groups[g].health[i].maximum > 0.0f && !groups[g].health[i].alive()) {
                dead.push_back(UnitRef{.batch = g, .instance = i});
            }
        }
    }
    return dead;
}

} // namespace rm::sim
