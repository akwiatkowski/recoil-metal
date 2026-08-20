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

void feed(StateHash& h, float value) noexcept {
    // The bit pattern, not the value. Two floats that differ in the last bit are a
    // divergence, and that is exactly what this has to report.
    //
    // Negative zero is normalised to positive zero first: -0.0f == 0.0f is true arithmetic
    // but their bit patterns differ, and a sim that produced one where it previously
    // produced the other has not actually diverged. Nothing else is normalised — a NaN's
    // payload is left alone, because a NaN appearing at all is a bug this should surface
    // rather than smooth over.
    const float normalised = value == 0.0f ? 0.0f : value;
    feed(h, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(normalised)));
}

void feed(StateHash& h, int value) noexcept {
    feed(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

void feed(StateHash& h, std::size_t value) noexcept {
    feed(h, static_cast<std::uint64_t>(value));
}

void feed(StateHash& h, bool value) noexcept { feed(h, value ? 1ULL : 0ULL); }

void feed(StateHash& h, const std::array<float, 3>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
    feed(h, v[2]);
}

void feed(StateHash& h, const std::array<float, 2>& v) noexcept {
    feed(h, v[0]);
    feed(h, v[1]);
}

void feed(StateHash& h, const Resources& r) noexcept {
    feed(h, r.mass);
    feed(h, r.energy);
}

// A unit's position and orientation. `animationPhase` and `teamColour` are skipped: see the
// header — both are presentation, and hashing them would report a divergence between two
// runs that played the same match.
void feedUnit(StateHash& h, const UnitInstance& unit) noexcept {
    feed(h, unit.position);
    feed(h, unit.rotationY);
    feed(h, unit.rotationX);
    feed(h, unit.rotationZ);
    feed(h, unit.scale);
}

void feedMotion(StateHash& h, const MoveState& motion) noexcept {
    feed(h, motion.armyIndex);
    feed(h, motion.destinationX);
    feed(h, motion.destinationZ);
    feed(h, motion.moving);
    feed(h, motion.speedElmosPerSecond);
    feed(h, motion.turnRateRadiansPerSecond);
    feed(h, motion.radiusElmos);
    feed(h, motion.distanceTravelledElmos);
    // The route still to walk. Fed with its length, so a unit that has consumed a waypoint
    // differs from one that has not even when the remaining waypoints coincide.
    feed(h, motion.path.size());
    for (const std::array<float, 2>& waypoint : motion.path) {
        feed(h, waypoint);
    }
    feed(h, motion.pathIndex);
}

void feedHealth(StateHash& h, const Health& health) noexcept {
    feed(h, health.current);
    feed(h, health.maximum);
    feed(h, health.reloadRemaining.size());
    for (const int remaining : health.reloadRemaining) {
        feed(h, remaining);
    }
}

} // namespace

StateHash hashMatch(const UnitStore& store, const Match& match) {
    StateHash h = kOffsetBasis;

    // Slot count first, so a store that grew differs even if the new slot is empty — a spawn
    // that produced nothing is still a different match.
    feed(h, store.slotCount());

    const std::span<const UnitInstance> instances = store.instances();
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        feedUnit(h, instances[slot]);
        feedMotion(h, motion[slot]);
        feedHealth(h, healths[slot]);
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

    // The armies. `colour` is skipped for the same reason `teamColour` is — a palette entry
    // is not a fact about the match.
    feed(h, match.armies.size());
    for (const Army& army : match.armies) {
        feed(h, army.index);
        feed(h, static_cast<int>(army.faction));
        feed(h, army.team);
        feed(h, army.defeated);
    }

    feed(h, match.economies.size());
    for (const Economy& economy : match.economies) {
        feed(h, economy.stored);
        feed(h, economy.storage);
        feed(h, economy.incomePerSecond);
        feed(h, economy.upkeepPerSecond);
        feed(h, economy.fundedFraction);
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
            feed(h, shot.damage);
            feed(h, shot.damageRadiusElmos);
            feed(h, shot.firedByArmy);
            feed(h, static_cast<int>(shot.arc));
            feed(h, shot.ticksRemaining);
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
            feed(h, work.buildRate);
            feed(h, work.blueprintIndex);
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
