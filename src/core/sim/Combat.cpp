#include "core/sim/Combat.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rm::sim {
namespace {


/// The army that owns the unit in a slot, or kNoArmy when the slot is past the end.
[[nodiscard]] int armyAt(const UnitStore& store, UnitIndex slot) noexcept {
    const std::span<const MoveState> motion = store.motion();
    if (slot >= motion.size()) {
        return kNoArmy;
    }
    return motion[slot].armyIndex;
}

/// The Army record for an index, or null. An index with no army is not an error: the
/// scene may hold unowned units, and they take no part in a fight.
[[nodiscard]] const Army* armyFor(int index, std::span<const Army> armies) noexcept {
    for (const Army& army : armies) {
        if (army.index == index) {
            return &army;
        }
    }
    return nullptr;
}

/// Whether `attackerArmy` may shoot the unit in `slot`.
///
/// Liveness is HEALTH, not the store's handle: health reaching zero is what makes a unit
/// dead to the sim, and `retireDead` is what later takes its handle away. Testing the handle
/// here would make a unit untargetable one tick before it was reported dead.
[[nodiscard]] bool shootable(int attackerArmy, const UnitStore& store, UnitIndex slot,
                            std::span<const Army> armies) noexcept {
    const std::span<const Health> health = store.health();
    if (slot >= health.size() || !health[slot].alive()) {
        return false;
    }
    const Army* mine = armyFor(attackerArmy, armies);
    const Army* theirs = armyFor(armyAt(store, slot), armies);
    if (mine == nullptr || theirs == nullptr) {
        return false;  // an unowned unit is nobody's target
    }
    return hostile(*mine, *theirs);
}

/// The biggest collision radius in the store.
///
/// Needed because two of the queries below have a PER-TARGET tolerance — a big unit is easier
/// to hit than a small one — and a spatial query needs one radius that bounds them all. Scanned
/// rather than cached: it is one pass over a contiguous array, against a projectile pass that
/// already touches every shot.
[[nodiscard]] Fx largestRadius(const UnitStore& store) noexcept {
    Fx largest{};
    for (const MoveState& state : store.motion()) {
        largest = std::max(largest, state.radiusElmos);
    }
    return largest;
}

/// The nearest hostile unit a shot has reached, or nothing.
///
/// The tolerance is the TARGET's own size plus the shot's blast radius, so a big unit is
/// easier to hit than a small one and a shell with a wide blast need not touch. A floor of
/// one tick of travel stops a fast shot stepping cleanly over a unit between two ticks,
/// which is the discrete-time version of a bullet passing through a wall.
[[nodiscard]] std::optional<UnitIndex> nearestStruck(const Projectile& shot,
                                                    const UnitStore& store,
                                                    std::span<const Army> armies) {
    // The velocity is already per tick, so its length IS one tick of travel — the
    // multiplication by a tick length that used to be here is gone.
    const Fx travelPerTick =
        fxSqrt(shot.velocity[0] * shot.velocity[0] + shot.velocity[1] * shot.velocity[1]
               + shot.velocity[2] * shot.velocity[2]);

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    std::optional<UnitIndex> best;
    Fx bestDistance{};

    // THE WIDEST tolerance any target could have, so the query is a superset of what the loop
    // below accepts: the per-target tolerance uses that target's own radius, and the largest
    // radius in the store bounds every one of them. Asking for less would miss a big unit
    // sitting just outside a small one's tolerance.
    const Fx reach = std::max(travelPerTick, largestRadius(store) + shot.damageRadiusElmos);

    for (const UnitIndex slot : store.space().within(shot.position[0], shot.position[2], reach)) {
        if (!shootable(shot.firedByArmy, store, slot, armies)) {
            continue;
        }
        const Fx size = slot < motion.size() ? motion[slot].radiusElmos : Fx{};
        const Fx tolerance = std::max(travelPerTick, size + shot.damageRadiusElmos);

        const Fx distance = groundDistanceElmos(shot.position, positionOf(transforms[slot]));
        if (distance > tolerance) {
            continue;
        }
        // Strictly nearer, so a tie falls to the lower slot. The grid returns slots in
        // ascending order, which is the order the full scan this replaces walked them in —
        // so the tie-break is the same one and the same unit is struck.
        if (!best || distance < bestDistance) {
            best = slot;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

Fx projectileGravityPerTickSquared(TickRate rate) noexcept {
    // Per second squared -> per tick squared: divide by the rate twice. `perTick` does it
    // once; `fromRatio(1, ticksPerSecond)` does it again.
    return rate.perTick(kProjectileGravityElmosPerSecond2)
           * Fx::fromRatio(1, static_cast<std::int32_t>(rate.ticksPerSecond()));
}

std::array<Fx, 3> positionOf(const Transform& transform) noexcept {
    return {transform.x, transform.y, transform.z};
}

Fx groundDistanceElmos(std::array<Fx, 3> from, std::array<Fx, 3> to) noexcept {
    return fxHypot(to[0] - from[0], to[2] - from[2]);
}

std::optional<UnitId> nearestTarget(std::array<Fx, 3> from, int fromArmy,
                                    const unitdef::Weapon& weapon, const UnitStore& store,
                                    std::span<const Army> armies) {
    if (!weapon.fires()) {
        return std::nullopt;
    }

    const std::span<const Transform> transforms = store.transforms();

    std::optional<UnitIndex> best;
    Fx bestDistance{};

    // THE GRID, rather than every slot in the store (§7 P5.2). The query radius is the weapon's
    // own reach, so a scan that used to be over every unit in the match is now over the handful
    // within range — and `minRange` is still applied below, because a dead zone is a hole in the
    // middle of the disc and not a smaller disc.
    for (const UnitIndex slot : store.space().within(from[0], from[2], weapon.maxRange)) {
        if (!shootable(fromArmy, store, slot, armies)) {
            continue;
        }

        const Fx distance = groundDistanceElmos(from, positionOf(transforms[slot]));
        if (distance > weapon.maxRange || distance < weapon.minRange) {
            continue;
        }

        // Strictly nearer, so a tie falls to the LOWER SLOT — the iteration order. That is
        // what makes the same scene pick the same target twice, which a screenshot depends
        // on.
        //
        // It used to fall to the lower batch and then the lower instance, and slot order is
        // not the same order: batches grouped one model together, and slots are spawn order
        // interleaved across models. So this tie-break picks a different unit than it used
        // to, and the match plays out differently — deliberately, and documented in PLAN2.md
        // §7 P1.4. The property that mattered (the same scene decides the same way every
        // run) is unchanged.
        if (!best || distance < bestDistance) {
            best = slot;
            bestDistance = distance;
        }
    }

    if (!best) {
        return std::nullopt;
    }
    return store.idAt(*best);
}

Brad bearingTo(std::array<Fx, 3> from, std::array<Fx, 3> to) noexcept {
    // The axis order IS the convention: measured from +Z toward +X, which is what the vertex
    // shader does with a yaw. `fxBearing` takes its arguments in the same order so the
    // convention lives in one signature rather than in comments at every call.
    return fxBearing(to[0] - from[0], to[2] - from[2]);
}

std::uint32_t headingError(Brad from, Brad to) noexcept {
    // Two lines where the float version needed eight. The difference of two `Brad` in 16 bits
    // IS the shortest way round — [-32,768, 32,767) is exactly [-half turn, +half turn) — so
    // the wrapping the loops above did by hand falls out of two's complement.
    const auto signed16 = static_cast<std::int16_t>(static_cast<std::uint16_t>(to)
                                                    - static_cast<std::uint16_t>(from));
    return static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(signed16)));
}

bool canFireAt(const unitdef::Weapon& weapon, Brad yaw, Brad bearing) noexcept {
    if (weapon.turreted) {
        return true;
    }
    // Degrees to binary radians: a full turn is 65,536, so a degree is 65,536/360 = 182.04.
    // Rounded rather than truncated, and computed in a wider integer so a tolerance of 180
    // degrees does not overflow on the way.
    const auto degrees = static_cast<std::int64_t>(
        std::lround(std::max(0.0f, weapon.firingToleranceDegrees) * (65536.0 / 360.0)));
    return static_cast<std::int64_t>(headingError(yaw, bearing)) <= degrees;
}

std::size_t aimAtTargets(UnitStore& store, const UnitCatalog& catalog,
                         std::span<const Army> armies) {
    const std::span<Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    std::size_t turned = 0;
    for (UnitIndex slot = 0; slot < transforms.size() && slot < motion.size(); ++slot) {
        if (motion[slot].moving) {
            continue;  // an order is already deciding where this one points
        }
        if (motion[slot].radiusElmos <= Fx{}) {
            continue;  // retired
        }

        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }

        // Only the weapons that need the hull pointed are worth turning for, and the longest
        // reach among them decides how far away a unit bothers to aim. Per unit now rather
        // than per batch, because the definition is per unit — the loop is over a handful of
        // weapons and costs nothing next to the sweep below.
        Fx reach{};
        for (const unitdef::Weapon& weapon : def->weapons) {
            if (weapon.fires() && !weapon.turreted) {
                reach = std::max(reach, weapon.maxRange);
            }
        }
        if (reach <= Fx{}) {
            continue;
        }

        // The nearest thing any of its hull-aimed weapons could reach. Built as a stand-in
        // weapon rather than looping over the real ones, because the answer is the same for
        // all of them and the sweep WAS the expensive part — this loop asked `nearestTarget`
        // once per unturreted hull per tick, and `nearestTarget` scanned every unit in the
        // match. It goes through the grid now (§7 P5.2), which is where most of the O(n²) in
        // this file lived.
        unitdef::Weapon sweep;
        sweep.role = unitdef::WeaponRole::DirectFire;
        sweep.damage = Mag::fromInt(1);
        sweep.rateOfFire = 1.0f;
        sweep.maxRange = reach;

        const std::optional<UnitId> target =
            nearestTarget(positionOf(transforms[slot]), motion[slot].armyIndex, sweep, store,
                          armies);
        if (!target) {
            continue;
        }

        const std::array<Fx, 3> at = positionOf(transforms[target->index]);
        const Brad bearing = bearingTo(positionOf(transforms[slot]), at);
        const std::uint32_t error = headingError(transforms[slot].heading, bearing);
        if (error <= 1e-4f) {
            continue;
        }

        // Turned at the unit's OWN rate, so a slow hull is slow to bring its gun to bear —
        // which is the whole reason `turnrate` is read off the blueprint.
        const std::int32_t step = motion[slot].turnPerTick;
        const auto delta = static_cast<std::int32_t>(static_cast<std::int16_t>(
            static_cast<std::uint16_t>(bearing)
            - static_cast<std::uint16_t>(transforms[slot].heading)));
        transforms[slot].heading = static_cast<Brad>(
            static_cast<std::uint16_t>(transforms[slot].heading)
            + static_cast<std::uint16_t>(std::clamp(delta, -step, step)));
        ++turned;
    }
    return turned;
}

std::size_t fireWeapons(UnitStore& store, const UnitCatalog& catalog,
                        std::span<const Army> armies,
                        std::vector<Projectile>& projectiles, TickRate rate,
                        EventQueue* events) {
    std::size_t fired = 0;

    const std::span<const Transform> transforms = store.transforms();
    const std::span<Health> healths = store.health();

    for (UnitIndex slot = 0; slot < transforms.size(); ++slot) {
        if (slot >= healths.size() || !healths[slot].alive()) {
            continue;  // the dead do not shoot
        }

        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr || def->weapons.empty()) {
            continue;
        }

        Health& health = healths[slot];
        health.reloadRemaining.resize(def->weapons.size(), 0);
        health.burstRemaining.resize(def->weapons.size(), 0);

        const int army = armyAt(store, slot);
        const std::array<Fx, 3> from = positionOf(transforms[slot]);

        for (std::size_t w = 0; w < def->weapons.size(); ++w) {
            const unitdef::Weapon& weapon = def->weapons[w];
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

            const std::optional<UnitId> target =
                nearestTarget(from, army, weapon, store, armies);
            if (!target) {
                continue;
            }

            const std::array<Fx, 3> to = positionOf(transforms[target->index]);

            // Pointing at it? A turreted weapon always is; an unturreted one has to be
            // brought round, which `aimAtTargets` does. The reload is NOT consumed while
            // turning: a unit that spent its shot waiting to line up would fire far more
            // slowly than its blueprint says.
            if (!canFireAt(weapon, transforms[slot].heading, bearingTo(from, to))) {
                continue;
            }

            // The weapon's derived per-tick rates come from the catalog, which computed them
            // when it learned the type — not from the weapon, which only knows its authored
            // per-second figures (§5.1).
            const UnitCatalog::WeaponRates& rates =
                catalog.weaponRates(store.typeAt(slot), w);
            projectiles.push_back(
                launch(from, to, weapon, army, rate, rates.muzzlePerTick, rates.damage,
                       store.idAt(slot)));
            ++fired;

            emit(events, Event{
                             .kind = EventKind::WeaponFired,
                             .unit = store.idAt(slot),
                             .instigator = *target,
                             .army = army,
                             .amount = weapon.damage,
                             .at = from,
                         });

            // THE BURST (§7 P3.5). A trigger-pull owes `burstSize` shots: the gap after any but
            // the last of them is the burst delay, and only the last one starts a real reload.
            // For the 462 weapons that do not burst, `burstSize` is 1 and `burstDelayTicks` is
            // the reload, so this reduces to exactly what it replaced.
            //
            // A burst is NOT abandoned when the target dies or walks out of range: the loop
            // above simply finds no target and this state persists, so the weapon resumes its
            // burst on the next thing it sees. That is the game's behaviour too — the salvo is
            // a coroutine that has already committed to its count — and it is also the only
            // version that is order-independent, since a burst that reset on a lost target
            // would depend on which shooter's projectile resolved first.
            if (health.burstRemaining[w] == 0) {
                health.burstRemaining[w] = rates.burstSize;
            }
            --health.burstRemaining[w];
            health.reloadRemaining[w] = health.burstRemaining[w] > 0
                                            ? static_cast<int>(rates.burstDelayTicks)
                                            : static_cast<int>(rates.reloadTicks);
        }
    }

    return fired;
}

Projectile launch(std::array<Fx, 3> from, std::array<Fx, 3> to,
                  const unitdef::Weapon& weapon, int byArmy, TickRate rate, Fx muzzlePerTick,
                  const unitdef::DamageProfile& damage, UnitId firedBy) {
    Projectile shot;
    shot.firedBy = firedBy;
    shot.position = {from[0], from[1] + kMuzzleHeight, from[2]};
    shot.damage = damage;
    shot.damageRadiusElmos = weapon.damageRadius;
    shot.firedByArmy = byArmy;
    shot.arc = weapon.arc;
    shot.ticksRemaining = static_cast<int>(rate.ticks(kProjectileLifetime));

    // Aimed from the MUZZLE at the target's middle, not from foot to foot: a shot that
    // leaves four elmos up and is aimed level would sail over its target.
    const Fx dx = to[0] - shot.position[0];
    const Fx dy = (to[1] + kMuzzleHeight * Fx::fromRatio(1, 2)) - shot.position[1];
    const Fx dz = to[2] - shot.position[2];
    const Fx ground = fxHypot(dx, dz);

    // A muzzle velocity of zero is a weapon whose shot is instantaneous — 111 of the 494
    // state one. Rather than special-case an instant hit, it is given a speed that
    // crosses its own maximum range in a single tick, so one code path carries every
    // shot and the arithmetic below never divides by zero.
    const Fx speedPerTick = muzzlePerTick > Fx{} ? muzzlePerTick : weapon.maxRange;

    if (ground <= Fx{}) {
        // Straight up, or at something in the same spot. Neither is worth a division.
        shot.velocity = {Fx{}, dy >= Fx{} ? speedPerTick : -speedPerTick, Fx{}};
        return shot;
    }

    // Flight time IN TICKS, which is the unit everything below is in. The per-second version
    // of this divided by a per-second speed; the shape is identical with the unit of time
    // already folded in.
    const Fx flightTicks = ground / speedPerTick;
    const Fx gravityPerTickSquared = projectileGravityPerTickSquared(rate);

    // FLAT: point at the target and let it fly. ARCED: the same horizontal velocity,
    // with the vertical component chosen so gravity brings it down exactly where the
    // target is — which is the closed-form solution rather than an iteration, because
    // the flight time is already known from the horizontal distance.
    //
    //   y = y0 + vy*t - g*t^2/2   solved for vy at t = flightSeconds
    const Fx vy = shot.arc == unitdef::BallisticArc::None
                    ? dy / flightTicks
                    : dy / flightTicks
                          + Fx::fromRatio(1, 2) * gravityPerTickSquared * flightTicks;

    shot.velocity = {dx / flightTicks, vy, dz / flightTicks};
    return shot;
}

Mag damageArea(std::array<Fx, 3> centre, Fx radiusElmos, Mag damage, int byArmy,
               UnitStore& store, std::span<const Army> armies, UnitId by,
               EventQueue* events) {
    // The scalar form, kept because it is what two dozen call sites mean — most of them tests
    // asserting the falloff curve, which is a property of the geometry and has nothing to say
    // about armour. A flat profile answers the same for every class, so this is not an
    // approximation of the call below: it is the same call with a table that has no entries.
    return damageArea(centre, radiusElmos, unitdef::flatDamage(damage), byArmy, store, armies,
                      nullptr, by, events);
}

Mag damageArea(std::array<Fx, 3> centre, Fx radiusElmos, const unitdef::DamageProfile& damage,
               int byArmy, UnitStore& store, std::span<const Army> armies,
               const UnitCatalog* catalog, UnitId by, EventQueue* events) {
    Mag dealt{};

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<Health> healths = store.health();

    // A point hit still reaches as far as the biggest unit's own radius — see the tolerance
    // below — so the query radius is the blast's, or that, whichever is larger.
    const Fx reach = radiusElmos > Fx{} ? radiusElmos
                                        : std::max(kFxOne, largestRadius(store));

    for (const UnitIndex slot : store.space().within(centre[0], centre[2], reach)) {
        if (!shootable(byArmy, store, slot, armies)) {
            continue;
        }

        const Fx distance = groundDistanceElmos(centre, positionOf(transforms[slot]));

        Fx share{};
        if (radiusElmos <= Fx{}) {
            // A point hit. Only what is essentially AT the centre takes it, and the
            // tolerance is the unit's own radius rather than zero — a shot aimed at
            // a unit's position that lands a tenth of an elmo away has hit it.
            const Fx tolerance =
                std::max(kFxOne, slot < motion.size() ? motion[slot].radiusElmos : kFxOne);
            share = distance <= tolerance ? kFxOne : Fx{};
        } else {
            // Linear from full at the centre to nothing at the rim.
            share = std::max(Fx{}, kFxOne - distance / radiusElmos);
        }

        if (share <= Fx{}) {
            continue;
        }

        // WHAT THIS WEAPON DOES TO THIS TARGET (PLAN2.md §7 P10.1). The armour class comes from
        // the catalog, which is where a type's content was resolved; with no catalog every
        // target is `kDefaultArmor` and a flat profile answers `base` — the pre-P10.1 engine,
        // exactly.
        //
        // LOOKED UP PER TARGET rather than hoisted, because it genuinely varies within one
        // blast: a shell landing between a tank and a bunker hits two armour classes, and that
        // is the whole point of the table.
        const ArmorClass armor =
            catalog != nullptr ? catalog->armorOf(store.typeAt(slot)) : kDefaultArmor;

        // The share is a fraction of the blast, so it is geometry: `Fx`. Multiplying a `Mag`
        // by an `Fx` is how a rate or a fraction becomes an amount, and it is the one mixed
        // operation the two types have.
        //
        // AND THE ORDER MATTERS: the armour lookup happens first, then the falloff scales what
        // armour left. The other way round would scale the base by distance and then look up a
        // table keyed on a number that no longer means what the table's keys mean.
        const Mag wanted = damage.against(armor) * share;
        const Mag applied = std::min(healths[slot].current, wanted);
        healths[slot].current -= applied;
        dealt += applied;

        // WHO DID IT, recorded on the unit rather than carried in the event alone — because the
        // event that needs it most is the DEATH, and a death is noticed a pass later by
        // `retireDead`, which sees only that health reached zero. Overwritten by each hit, so
        // the kill goes to whoever landed the last blow: the same rule Recoil's attacker triple
        // follows, and the only one that does not need a damage ledger per unit.
        healths[slot].lastHitBy = by;

        emit(events, Event{
                         .kind = EventKind::UnitDamaged,
                         .unit = store.idAt(slot),
                         .instigator = by,
                         .army = armyAt(store, slot),
                         .amount = applied,
                         .at = positionOf(transforms[slot]),
                     });
    }

    return dealt;
}

void advanceProjectiles(std::vector<Projectile>& projectiles, UnitStore& store,
                        std::span<const Army> armies, const Terrain& terrain, TickRate rate,
                        EventQueue* events, const UnitCatalog* catalog) {
    const Fx gravityPerTickSquared = projectileGravityPerTickSquared(rate);

    for (Projectile& shot : projectiles) {
        if (shot.ticksRemaining <= 0) {
            continue;
        }
        --shot.ticksRemaining;

        if (shot.arc != unitdef::BallisticArc::None) {
            shot.velocity[1] -= gravityPerTickSquared;
        }

        // No scaling: the velocity is already what one tick of flight covers.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            shot.position[axis] += shot.velocity[axis];
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
        const std::optional<UnitIndex> struck = nearestStruck(shot, store, armies);
        const Fx ground = terrain.heightAt(shot.position[0], shot.position[2]);

        if (!struck && shot.position[1] > ground) {
            continue;
        }

        if (struck) {
            shot.position = positionOf(store.transforms()[*struck]);
        } else {
            shot.position[1] = ground;
        }
        emit(events, Event{
                         .kind = EventKind::ProjectileImpact,
                         .unit = struck ? store.idAt(*struck) : UnitId{},
                         .instigator = shot.firedBy,
                         .army = shot.firedByArmy,
                         // The BASE, because an event says what was thrown rather than what
                         // each target took — the per-target figure is a `UnitDamaged` per hit.
                         .amount = shot.damage.base,
                         .at = shot.position,
                     });
        damageArea(shot.position, shot.damageRadiusElmos, shot.damage, shot.firedByArmy, store,
                   armies, catalog, shot.firedBy, events);
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

Mag explodeOnDeath(const unitdef::UnitDef& def, std::array<Fx, 3> at, int byArmy,
                     UnitStore& store, std::span<const Army> armies, UnitId by,
                     EventQueue* events, const UnitCatalog* catalog) {
    const unitdef::Weapon* blast = deathWeapon(def);
    if (blast == nullptr || !blast->harmful()) {
        return Mag{};
    }

    // TWO RINGS when the weapon states them — the four commanders do — applied outer first
    // and inner second. The order matters: `damageArea` falls off to nothing at its rim, so
    // running the wide weak ring first and the narrow strong one after means anything close
    // takes both, which is what a nested blast should do. Reversed, the outer ring would be
    // finishing off things the inner one had already flattened.
    // The death weapon's own damage table, RESOLVED HERE rather than read from `weaponRates`.
    // A death blast happens a few times a match, so recomputing the transpose costs nothing and
    // saves threading a weapon index through the death report; firing is the hot path and reads
    // the profile computed once at load. `profileFor` falls back to a flat table when there is
    // no catalog, which is the pre-P10.1 behaviour.
    const auto profile = [&](Mag amount) {
        return catalog != nullptr ? catalog->profileFor(*blast, amount)
                                  : unitdef::flatDamage(amount);
    };

    if (blast->hasRings()) {
        Mag dealt{};
        dealt += damageArea(at, blast->outerRingRadius, profile(blast->outerRingDamage), byArmy,
                            store, armies, catalog, by, events);
        dealt += damageArea(at, blast->innerRingRadius, profile(blast->innerRingDamage), byArmy,
                            store, armies, catalog, by, events);
        return dealt;
    }

    return damageArea(at, blast->damageRadius, profile(blast->damage), byArmy, store, armies,
                      catalog, by, events);
}

std::vector<UnitId> deadUnits(const UnitStore& store) {
    const std::span<const Health> healths = store.health();
    std::vector<UnitId> dead;
    for (UnitIndex slot = 0; slot < healths.size(); ++slot) {
        if (healths[slot].maximum > Mag{} && !healths[slot].alive()) {
            dead.push_back(store.idAt(slot));
        }
    }
    return dead;
}

} // namespace rm::sim
