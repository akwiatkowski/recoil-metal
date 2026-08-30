#include "core/sim/StateHash.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace rm::sim {
namespace {

// FNV-1a, 64-bit. Chosen for being short enough to read in one sitting and having no
// tuning constants to get subtly wrong — the properties that matter here are that it is
// deterministic and that it is OURS, not that it is fast or cryptographic. A faster hash
// would save time we are not short of; a library hash would be a dependency whose version
// silently changes what "the same match" means.
inline constexpr StateHash kOffsetBasis = 14695981039346656037ULL;
inline constexpr StateHash kPrime = 1099511628211ULL;

void feed(StateHash& h, std::uint64_t value) noexcept {
    // Byte at a time, low to high, so the result does not depend on the host's endianness.
    // The cross-architecture claim is the whole point; hashing a `std::uint64_t`'s bytes in
    // memory order would make arm64 and x86-64 disagree on identical state.
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFULL;
        h *= kPrime;
    }
}

// `feed(StateHash&, float)` USED TO BE HERE, and its absence is the result rather than a
// tidy-up (PLAN2.md §7 P10.0).
//
// It hashed a float's bit pattern, normalising negative zero, because `Construction::position`
// was a `std::array<float, 3>` — the last float in sim state, and one the float ban could not
// see because it arrived through a `fxToFloat` call rather than a declaration. With that field
// fixed point, nothing hashed is a float any more, and `-Wunused-function` says so at every
// build: **re-introducing a float into hashed state now fails to compile until someone puts
// this function back, which is a deliberate act rather than an oversight.**
//
// That is a stronger guarantee than the grep in `check_no_sim_floats.sh` gives, and it is worth
// keeping. If you are here because you need to hash a float: do not. Convert at the boundary.

void feed(StateHash& h, int value) noexcept {
    feed(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

void feed(StateHash& h, std::size_t value) noexcept {
    feed(h, static_cast<std::uint64_t>(value));
}

void feed(StateHash& h, bool value) noexcept { feed(h, value ? 1ULL : 0ULL); }

/// Fixed-point values are fed as their RAW INTEGERS, which is the whole point of having them:
/// there is no bit pattern to normalise and no tolerance to worry about, because two runs that
/// agree agree exactly.
void feed(StateHash& h, Fx value) noexcept {
    feed(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.raw())));
}

void feed(StateHash& h, Mag value) noexcept {
    feed(h, static_cast<std::uint64_t>(value.raw()));
}

void feed(StateHash& h, const std::array<Fx, 3>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
    feed(h, v[2]);
}

void feed(StateHash& h, const std::array<Fx, 2>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
}

/// An angle. Fed as its raw 16 bits — there is no wrapping to normalise, because the type
/// cannot hold an unwrapped angle.
void feed(StateHash& h, Brad value) noexcept { feed(h, static_cast<std::uint64_t>(value)); }

void feed(StateHash& h, const Resources& r) noexcept {
    feed(h, r.mass);
    feed(h, r.energy);
}

// A unit's position and orientation.
//
// The presentation exclusions this used to carry — `animationPhase` and `teamColour` — are
// gone, and not because the rule changed: the store no longer holds them. `UnitInstance` is
// built at draw time now (P2.2), so the fields that could have caused a false divergence are
// not reachable from here at all. That paragraph in the header is history rather than a
// caveat, which is the outcome it predicted.
void feedTransform(StateHash& h, const Transform& unit) noexcept {
    feed(h, unit.x);
    feed(h, unit.y);
    feed(h, unit.z);
    feed(h, unit.heading);
    feed(h, unit.pitch);
    feed(h, unit.roll);
}

void feedMotion(StateHash& h, const MoveState& motion) noexcept {
    feed(h, motion.armyIndex);
    feed(h, motion.destinationX);
    feed(h, motion.destinationZ);
    feed(h, motion.moving);
    // Derived from the already-hashed unit type, but still execution state: feeding only the
    // true case catches a broken spawn invariant without changing every historical ground hash.
    if (motion.airborne) {
        feed(h, motion.airborne);
    }
    if (motion.surfaceWater) {
        feed(h, motion.surfaceWater);
    }
    feed(h, motion.speedPerTick);
    feed(h, motion.turnPerTick);
    feed(h, motion.radiusElmos);
    feed(h, motion.distanceTravelledElmos);
    // The route still to walk. Fed with its length, so a unit that has consumed a waypoint
    // differs from one that has not even when the remaining waypoints coincide.
    feed(h, motion.path.size());
    for (const std::array<Fx, 2>& waypoint : motion.path) {
        feed(h, waypoint);
    }
    feed(h, motion.pathIndex);
}

void feedHealth(StateHash& h, const Health& health) noexcept {
    feed(h, health.current);
    feed(h, health.maximum);
    // Preserve every historical unshielded hash while covering all new execution state.
    if (health.shield.maximum > Mag{}) {
        feed(h, true);
        feed(h, health.shield.current);
        feed(h, health.shield.maximum);
        feed(h, static_cast<std::size_t>(health.shield.regenDelayRemaining));
        feed(h, static_cast<std::size_t>(health.shield.rechargeRemaining));
    }
    feed(h, health.reloadRemaining.size());
    for (const int remaining : health.reloadRemaining) {
        feed(h, remaining);
    }

    // Where each weapon is WITHIN its burst. Fed for the same reason the reload is: two units
    // with identical health and identical reloads but different burst positions will fire
    // different numbers of shots over the next second, so a hash that could not tell them apart
    // would report a match as identical while it diverged (§7 P3.5).
    feed(h, health.burstRemaining.size());
    for (const int remaining : health.burstRemaining) {
        feed(h, remaining);
    }

    // WHO LAST HIT IT. State, not provenance — unlike a command's issuing player, this decides
    // something: it is the instigator a `UnitDestroyed` event names, and a consumer that awards
    // a kill, plays a sound or scores a match reads it. Two runs that disagree about who is
    // shooting whom have diverged, whatever the health totals say.
    feed(h, static_cast<std::size_t>(health.lastHitBy.index));
    feed(h, static_cast<std::size_t>(health.lastHitBy.generation));

    // VETERANCY, and only when the unit has any — which keeps every historical hash of a match
    // whose units never promoted byte-identical, the same trick the unshielded case above uses.
    // Fed because it decides things: two units with equal health but different levels have
    // different maxima and heal at different rates, so they diverge from the next tick on.
    if (health.veterancy.kills != 0 || health.veterancy.level != 0) {
        feed(h, true);
        feed(h, health.veterancy.kills);
        feed(h, health.veterancy.level);
    }
}

/// A unit's outstanding orders (§7 P4.1).
///
/// FED, because a queue is sim state in the strongest sense: two units standing in the same
/// place with the same health will end up in different places if one of them has three
/// waypoints left and the other none. A hash blind to the queue would call a match identical
/// right up to the tick the routes diverged, which is the failure mode the whole harness exists
/// to prevent — it would report the divergence late, at the movement, rather than at its cause.
///
/// Field by field rather than as a struct, on the same rule as everything else here: `Command`
/// is trivially copyable but its padding is unspecified, and hashing padding would make the
/// result depend on the compiler.
///
/// AND NOT EVERY FIELD. `tick` and `player` are PROVENANCE — when the order was given and who
/// gave it — and neither can change what the unit does with it. Feeding them would make the
/// hash disagree about two matches that will play out identically, which is the same category
/// of mistake as hashing a screen position. It would also make §7 P4.2's test impossible to
/// state: "the scripted opponent's order and a synthetic click produce identical hashes" is
/// only true if the hash is blind to which of them issued it. The `CommandLog` keeps both, and
/// that is where provenance belongs.
void feedOrders(StateHash& h, const CommandQueue& orders) noexcept {
    feed(h, orders.size());
    for (const Command& command : orders.orders()) {
        feed(h, static_cast<std::uint8_t>(command.kind));
        feed(h, static_cast<std::size_t>(command.unit.index));
        feed(h, static_cast<std::size_t>(command.unit.generation));
        feed(h, command.targetX);
        feed(h, command.targetZ);
        // Invalid means "no target" and predates targeted commands. Feeding nothing for it
        // preserves old hashes; a real generational handle joins the hash at acquisition.
        if (command.target.generation != 0) {
            feed(h, static_cast<std::size_t>(command.target.index));
            feed(h, static_cast<std::size_t>(command.target.generation));
        }
        feed(h, static_cast<std::size_t>(command.buildType));
        // Unlike the input tick, this clock changes how a rotating patrol accepts its next
        // waypoint: the oldest command marks the lap boundary even after it leaves the head.
        feed(h, static_cast<std::uint64_t>(command.creationSerial));
    }
}

} // namespace

StateHash hashMatch(const UnitStore& store, const Match& match) {
    StateHash h = kOffsetBasis;

    // Slot count first, so a store that grew differs even if the new slot is empty — a spawn
    // that produced nothing is still a different match.
    feed(h, store.slotCount());
    // The next accepted command consumes this value. It is match state even while no live
    // command currently exposes it, and clearing or recycling a queue must not rewind it.
    feed(h, static_cast<std::uint64_t>(store.nextCommandSerial()));

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();
    const std::span<const CommandQueue> orders = store.orders();

    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        feedTransform(h, transforms[slot]);
        feedMotion(h, motion[slot]);
        feedHealth(h, healths[slot]);
        feedOrders(h, orders[slot]);
        // The type, and who is in the slot. The type stands in for the definition — a def is
        // content loaded from disk and identical across two runs of the same log by
        // construction, so hashing its contents would fingerprint the install rather than the
        // match. The generation says whether this is the same occupant.
        feed(h, static_cast<std::size_t>(store.typeAt(slot)));
        feed(h, static_cast<std::size_t>(store.idAt(slot).generation));
        // WHETHER THE SLOT IS OCCUPIED, which the generation cannot say on its own. Death is
        // a tombstone: `kill` leaves every array untouched and deliberately does NOT advance
        // the generation mirror, because a stale mirror is what makes a dead unit's handle
        // fail `alive`. So without this, a death changed nothing here — and a match that had
        // lost a unit hashed identically to one that had not. In practice the values usually
        // moved too (`retireDead` zeroes the radius, and health reached zero to get there),
        // which is why this went unnoticed until a test killed a unit at full health.
        feed(h, store.slotAlive(slot));
    }

    // The armies. There is no `colour` to skip any more (§7 P6.3): a palette entry
    // is not a fact about the match.
    feed(h, match.armies.size());
    for (const Army& army : match.armies) {
        feed(h, army.index);
        feed(h, static_cast<int>(army.faction));
        feed(h, army.alliance);
        feed(h, army.defeated);
    }

    feed(h, match.economies.size());
    for (const Economy& economy : match.economies) {
        feed(h, economy.stored);
        feed(h, economy.storage);
        feed(h, economy.incomePerTick);
        feed(h, economy.upkeepPerTick);
        feed(h, economy.fundedFraction);
        // The two bucket ratios and which resource bound them (`C-159`), plus upkeep's share
        // of the carry-forward. `massIsBinding` decides which ratio each consumer is granted
        // at, so it changes behaviour and belongs here.
        feed(h, economy.multiResourceFunded);
        feed(h, economy.singleResourceFunded);
        feed(h, economy.massIsBinding);
        feed(h, economy.upkeepAllocated);
        // The allied gift in flight. It carries between ticks and is spent next tick, so it
        // is live state by the same argument as the carry-forward above — and the sharing
        // flag decides whether it is ever produced.
        feed(h, economy.sharedIn);
        feed(h, economy.sharesOverflow);
    }

    // WHAT EACH SIDE CAN SEE (ADR-037). Hashed rather than treated as a derived cache, and
    // the reason is that it is not one: sight decides who may be shot at, so two runs whose
    // grids differ are two runs that will fight differently a tick later. Catching that here
    // names the tick it happened on instead of the tick it became visible in the outcome.
    //
    // The COUNTS, not just whether a square is covered. A square seen by two units and a
    // square seen by one are the same to a shooter and different to the sim — the second is
    // one death away from being dark — and a hash that could not tell them apart would miss
    // a divergence in who is standing where.
    //
    // Both styles feed the same field, so switching `VisionStyle` invalidates a recorded
    // hash log. That is correct: it is a different match.
    feed(h, match.intel != nullptr);
    if (match.intel != nullptr) {
        feed(h, static_cast<int>(match.intel->style()));
        feed(h, match.intel->alliances());
        for (std::size_t alliance = 0; alliance < match.intel->alliances(); ++alliance) {
            for (std::size_t kind = 0; kind < kIntelKindCount; ++kind) {
                const IntelGrid& grid =
                    match.intel->grid(static_cast<int>(alliance), static_cast<IntelKind>(kind));
                feed(h, grid.counts().size());
                for (const std::uint16_t count : grid.counts()) {
                    feed(h, count);
                }
            }
            // The hidden family after the senses, same discipline: derived state, hashed so
            // a divergence in stealth-field stamping is caught by the same gate as one in
            // sight. Growing the format invalidates recorded logs, correctly — the golden
            // was re-recorded when these arrived.
            for (std::size_t kind = 0; kind < kHiddenKindCount; ++kind) {
                const IntelGrid& grid = match.intel->hiddenGrid(
                    static_cast<int>(alliance), static_cast<HiddenKind>(kind));
                feed(h, grid.counts().size());
                for (const std::uint16_t count : grid.counts()) {
                    feed(h, count);
                }
            }
        }
    }

    // Shots in flight. Nullable because a decorative crowd has no projectile list, and
    // "no list" has to hash differently from "an empty list" — the first is a scene with no
    // combat, the second a match between two ticks of it.
    feed(h, match.projectiles != nullptr);
    if (match.projectiles != nullptr) {
        feed(h, match.projectiles->size());
        for (const Projectile& shot : *match.projectiles) {
            feed(h, shot.position);
            feed(h, shot.velocity);
            // THE WHOLE TABLE, field by field. A profile is trivially copyable but its padding
            // is unspecified, so it is fed like every other struct here rather than as bytes.
            feed(h, shot.damage.base);
            feed(h, static_cast<std::size_t>(shot.damage.overrideCount));
            for (std::uint8_t i = 0; i < shot.damage.overrideCount; ++i) {
                feed(h, static_cast<std::size_t>(shot.damage.overrideArmor[i]));
                feed(h, shot.damage.overrideDamage[i]);
            }
            feed(h, shot.damageRadiusElmos);
            // Surface-only is the historical FA projectile state. Keep its byte stream stable,
            // while every other mask carries a marker and value that distinguish future rules.
            if (shot.targetLayers != unitdef::TargetLayerMask::Surface) {
                feed(h, true);
                feed(h, static_cast<int>(shot.targetLayers));
            }
            feed(h, shot.firedByArmy);
            // The shooter's identity is live execution state: at impact it becomes
            // `lastHitBy`, which decides kill attribution. Two runs differing only here
            // hash identically for the whole flight — up to thirty seconds — before the
            // divergence surfaces one hop downstream. Fed like every other UnitId.
            feed(h, static_cast<std::size_t>(shot.firedBy.index));
            feed(h, static_cast<std::size_t>(shot.firedBy.generation));
            feed(h, static_cast<int>(shot.arc));
            feed(h, shot.ticksRemaining);
            feed(h, static_cast<int>(shot.pendingImpact));
            feed(h, static_cast<std::size_t>(shot.impactTarget.index));
            feed(h, static_cast<std::size_t>(shot.impactTarget.generation));
        }
    }

    feed(h, match.building != nullptr);
    if (match.building != nullptr) {
        feed(h, match.building->size());
        for (const Construction& work : *match.building) {
            feed(h, work.armyIndex);
            feed(h, work.position);
            feed(h, work.cost);
            feed(h, work.buildTimeRemaining);
            feed(h, work.totalBuildTime);
            feed(h, work.buildPerTick);
            feed(h, work.blueprintIndex);
            feed(h, static_cast<std::uint64_t>(work.upgradeOf.index));
            feed(h, static_cast<std::uint64_t>(work.upgradeOf.generation));
            // The founder and the help. `builder` is real state (set at order time);
            // `assistPerTick` is derived each tick from hashed orders and positions, so
            // feeding it costs nothing and catches a divergence in the assist scan itself.
            feed(h, static_cast<std::uint64_t>(work.builder.index));
            feed(h, static_cast<std::uint64_t>(work.builder.generation));
            feed(h, work.assistPerTick);
            // THE CARRY-FORWARD AND THE CACHED RATIO (`C-159`, `C-162`). Both are live sim
            // state, not derived: `allocated` is a residue that survives into next tick's
            // outstanding demand, and `fundedLastTick` is what next tick's progress is
            // multiplied by. Leaving them out would let two runs diverge in build speed while
            // the fingerprint said they agreed — the exact failure this log exists to catch.
            feed(h, work.allocated);
            feed(h, work.fundedLastTick);
        }
    }

    // THE WRECKS, since reclaim reads them back into a rule — the moment FeatureStore.hpp's
    // old note said they would have to be here. How much of a wreck is LEFT depends on who
    // reclaimed it and when, which no unit's row records; two matches that differ only in a
    // half-drained wreck must hash apart on the tick they diverged, not when the mass gets
    // spent. Slot liveness and generation for the same reason the units feed them: a
    // reclaimed-and-replaced slot is a different occupant.
    feed(h, match.features != nullptr);
    if (match.features != nullptr) {
        feed(h, match.features->size());
        const std::span<const Feature> features = match.features->all();
        for (UnitIndex slot = 0; slot < features.size(); ++slot) {
            const Feature& wreck = features[slot];
            feed(h, wreck.at);
            feed(h, wreck.radiusElmos);
            feed(h, static_cast<std::size_t>(wreck.fromType));
            feed(h, wreck.armyIndex);
            feed(h, wreck.massRemaining);
            feed(h, wreck.energyRemaining);
            feed(h, wreck.reclaimPerBuildRate);
            feed(h, match.features->slotAlive(slot));
            feed(h, static_cast<std::size_t>(match.features->idAt(slot).generation));
        }
    }

    feed(h, match.commandersEver.size());
    for (const int ever : match.commandersEver) {
        feed(h, ever);
    }
    feed(h, match.baseStorage);
    feed(h, match.over);

    return h;
}

} // namespace rm::sim
