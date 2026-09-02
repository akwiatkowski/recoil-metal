#include "core/sim/Combat.hpp"

#include "core/unit/BuildTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// `scaledDamage` used to live here — it rescaled a whole `DamageProfile` so the blast could be
// weakened once for everybody before the target loop ran. Nothing needs that now: shield
// absorption became per target (`C-110`) and applies as one `Fx` factor at the point of use,
// and the distance falloff it also served went away with `C-061`. Removed rather than left
// for a future caller, because a helper whose only meaning was "weaken the shot for the whole
// blast" is exactly the model this file no longer has.

[[nodiscard]] Fx fraction(Mag numerator, Mag denominator) noexcept {
    if (denominator <= Mag{}) {
        return Fx{};
    }
    return Fx::fromRaw(saturate((FxWide{numerator.raw()} << kFxFractionalBits)
                                / denominator.raw()));
}

// Collision time needs more precision than a world coordinate. A one-raw-unit position gap
// over a long sweep disappears in Q18.14, widening both the 10% endpoint and the tie set.
using SweepFraction = std::int64_t;
inline constexpr int kSweepFractionalBits = 31;
inline constexpr SweepFraction kSweepOne = SweepFraction{1} << kSweepFractionalBits;
// ponytail: Q31 distinguishes one raw step across shipped maps; use rational timing only if
// full-range Fx sweeps become a supported input.
inline constexpr SweepFraction kSweepExtension = kSweepOne / 10;

[[nodiscard]] Fx interpolateSweep(Fx from, Fx to, SweepFraction fraction) noexcept {
    const Fx delta = to - from;
    const FxWide displacement =
        roundShift(FxWide{delta.raw()} * fraction, kSweepFractionalBits);
    return Fx::fromRaw(saturate(FxWide{from.raw()} + displacement));
}

struct SweptHit {
    UnitIndex slot = 0;
    SweepFraction fraction = 0;
    bool proximityFallback = false;
    bool shield = false;
};

[[nodiscard]] Mag damageTarget(UnitIndex target, const unitdef::DamageProfile& damage,
                               int byArmy, UnitStore& store, std::span<const Army> armies,
                               const UnitCatalog* catalog, UnitId by, EventQueue* events,
                               unitdef::TargetLayerMask targetLayers);

/// Whether the original three-dimensional tick motion is strictly below retail's 0.01-elmo
/// sweep threshold (`C-168`). Compare the exact rational in raw units because Q18.14 cannot
/// represent 0.01: 10,000 * lengthRaw^2 < 16,384^2.
[[nodiscard]] bool usesProximityFallback(std::array<Fx, 3> from,
                                         std::array<Fx, 3> to) noexcept {
    constexpr std::uint64_t thresholdDenominator = 100u;  // 0.01 elmo is exactly 1/100.
    constexpr std::uint64_t oneElmoRaw = std::uint64_t{1} << kFxFractionalBits;

    std::uint64_t lengthSquaredRaw = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const FxWide deltaRaw = FxWide{to[axis].raw()} - from[axis].raw();
        const auto magnitudeRaw = static_cast<std::uint64_t>(
            deltaRaw < 0 ? -deltaRaw : deltaRaw);
        // One component at or above the exact threshold rejects the whole vector. Bounding it
        // first keeps all following squares far from integer overflow.
        if (magnitudeRaw * thresholdDenominator >= oneElmoRaw) {
            return false;
        }
        lengthSquaredRaw += magnitudeRaw * magnitudeRaw;
    }
    return thresholdDenominator * thresholdDenominator * lengthSquaredRaw
           < oneElmoRaw * oneElmoRaw;
}

/// Overlap of a three-dimensional sphere and an axis-aligned collision box. Collision's
/// radius-one fallback is strict; an area-damage sphere includes its rim (`C-061`, `C-168`).
[[nodiscard]] bool sphereBoxOverlap(std::array<Fx, 3> centre,
                                    std::array<Fx, 3> minimum,
                                    std::array<Fx, 3> maximum, Fx radius,
                                    bool inclusive) noexcept {
    if (radius <= Fx{}) {
        return false;
    }
    const auto radiusRaw = static_cast<std::uint64_t>(radius.raw());
    const std::uint64_t radiusSquaredRaw = radiusRaw * radiusRaw;
    std::uint64_t distanceSquaredRaw = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        Fx gap{};
        if (centre[axis] < minimum[axis]) {
            gap = minimum[axis] - centre[axis];
        } else if (centre[axis] > maximum[axis]) {
            gap = centre[axis] - maximum[axis];
        }
        if (gap > radius) {
            return false;
        }
        const auto gapRaw = static_cast<std::uint64_t>(gap.raw());
        const std::uint64_t gapSquaredRaw = gapRaw * gapRaw;
        // Keep the sum bounded by radiusSquaredRaw, avoiding overflow even at Fx's limit.
        if (gapSquaredRaw > radiusSquaredRaw - distanceSquaredRaw) {
            return false;
        }
        distanceSquaredRaw += gapSquaredRaw;
    }
    return inclusive || distanceSquaredRaw < radiusSquaredRaw;
}

/// Earliest intersection of a line with an axis-aligned collision box, inside the supplied
/// fraction interval. Ordinary segments use 0..1; projectile collision extends that interval.
[[nodiscard]] std::optional<SweepFraction> segmentBoxEntry(
    std::array<Fx, 3> from, std::array<Fx, 3> to, std::array<Fx, 3> minimum,
    std::array<Fx, 3> maximum, SweepFraction minimumFraction,
    SweepFraction maximumFraction) noexcept {
    SweepFraction enter = minimumFraction;
    SweepFraction exit = maximumFraction;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const FxWide fromRaw = from[axis].raw();
        const FxWide deltaRaw = FxWide{to[axis].raw()} - fromRaw;
        if (deltaRaw == 0) {
            if (from[axis] < minimum[axis] || from[axis] > maximum[axis]) {
                return std::nullopt;
            }
            continue;
        }
        SweepFraction first =
            ((FxWide{minimum[axis].raw()} - fromRaw) * kSweepOne) / deltaRaw;
        SweepFraction last =
            ((FxWide{maximum[axis].raw()} - fromRaw) * kSweepOne) / deltaRaw;
        if (first > last) {
            std::swap(first, last);
        }
        enter = std::max(enter, first);
        exit = std::min(exit, last);
        if (enter > exit) {
            return std::nullopt;
        }
    }
    return enter;
}

/// Earliest point an extended flight segment enters a spherical ordinary shield.  The search is
/// fixed-point throughout: choosing the first Q31 fraction keeps shield and body ordering part
/// of deterministic simulation state rather than depending on a platform floating-point root.
[[nodiscard]] std::optional<SweepFraction> segmentSphereEntry(
    std::array<Fx, 3> from, std::array<Fx, 3> to, std::array<Fx, 3> centre, Fx radius,
    SweepFraction minimumFraction, SweepFraction maximumFraction) noexcept {
    if (radius <= Fx{}) {
        return std::nullopt;
    }

    // `FxWide` is deliberately enough for coordinate deltas but not for their products at the
    // edge of the Fx domain.  The sweep's closest-point calculation therefore widens before
    // multiplying, as `sphereBoxOverlap` does for its squared comparison.
    using Product = __int128_t;
    Product dot{};
    Product lengthSquared{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const Product offset = Product{from[axis].raw()} - centre[axis].raw();
        const Product delta = Product{to[axis].raw()} - from[axis].raw();
        dot += offset * delta;
        lengthSquared += delta * delta;
    }
    if (lengthSquared == 0) {
        return std::nullopt;
    }

    const auto contains = [&](SweepFraction fraction) {
        Product distanceSquared{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const Product coordinate = interpolateSweep(from[axis], to[axis], fraction).raw();
            const Product delta = coordinate - centre[axis].raw();
            distanceSquared += delta * delta;
        }
        const Product radiusRaw = radius.raw();
        return distanceSquared <= radiusRaw * radiusRaw;
    };

    if (contains(minimumFraction)) {
        return minimumFraction;
    }

    const Product closestRaw = (-dot * kSweepOne) / lengthSquared;
    const Product boundedClosest = std::clamp(
        closestRaw, Product{minimumFraction}, Product{maximumFraction});
    const SweepFraction closest = static_cast<SweepFraction>(boundedClosest);
    if (!contains(closest)) {
        return std::nullopt;
    }

    SweepFraction outside = minimumFraction;
    SweepFraction inside = closest;
    while (inside - outside > 1) {
        const SweepFraction middle = outside + (inside - outside) / 2;
        if (contains(middle)) {
            inside = middle;
        } else {
            outside = middle;
        }
    }
    return inside;
}

/// First hostile body hit by this tick's extended 3D segment, or by C-168's radius-one sphere
/// at the old position when the motion is below 0.01 elmo. Blast radius belongs to damage after
/// impact, not the projectile's physical body.
[[nodiscard]] std::optional<SweptHit> firstStruck(const Projectile& shot,
                                                  std::array<Fx, 3> from,
                                                  std::array<Fx, 3> to,
                                                  const UnitStore& store,
                                                  std::span<const Army> armies,
                                                  const UnitCatalog* catalog) {
    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    std::optional<SweptHit> best;

    const Fx half = Fx::fromRatio(1, 2);
    const Fx extension = Fx::fromRatio(1, 10);
    const Fx middleX = from[0] + (to[0] - from[0]) * half;
    const Fx middleZ = from[2] + (to[2] - from[2]) * half;
    const Fx spanX = std::max(from[0], to[0]) - std::min(from[0], to[0]);
    const Fx spanZ = std::max(from[2], to[2]) - std::min(from[2], to[2]);
    // The targets are boxes, so their centres occupy a SQUARE around the swept line rather
    // than its circumscribed circle. One fraction LSB covers a slab boundary rounded into the
    // accepted interval, and one position LSB covers the rounded midpoint.
    const Fx roundingGuard = Fx::fromRaw(1);
    const bool proximityFallback = usesProximityFallback(from, to);
    const Fx largestColliderRadius =
        std::max(largestRadius(store), catalog != nullptr ? catalog->largestShieldRadius() : Fx{});
    const Fx reach = proximityFallback
                        ? largestColliderRadius + kFxOne
                        : std::max(spanX, spanZ) * (half + extension + roundingGuard)
                              + largestColliderRadius + roundingGuard;
    const SweepFraction minimumFraction = -kSweepExtension;
    const SweepFraction maximumFraction = kSweepOne + kSweepExtension;
    const Fx queryX = proximityFallback ? from[0] : middleX;
    const Fx queryZ = proximityFallback ? from[2] : middleZ;

    for (const UnitIndex slot : store.space().candidates(queryX, queryZ, reach)) {
        if (!shootable(shot.firedByArmy, store, slot, armies)) {
            continue;
        }
        if (slot >= transforms.size() || slot >= motion.size()) {
            continue;
        }

        // An active ordinary bubble is its own swept collision primitive.  It is considered
        // before the owner's body and is deliberately independent of unit target layers: the
        // native shield primitive occupies its own collision layer (`C-169`).
        if (!proximityFallback && catalog != nullptr) {
            const UnitCatalog::ShieldInfo& shield = catalog->shield(store.typeAt(slot));
            if (shield.exists() && store.health()[slot].shield.active()) {
                std::array<Fx, 3> centre = positionOf(transforms[slot]);
                centre[1] += shield.verticalOffsetElmos;
                const std::optional<SweepFraction> hit = segmentSphereEntry(
                    from, to, centre, shield.radiusElmos, minimumFraction, maximumFraction);
                if (hit && (!best || *hit < best->fraction)) {
                    best = SweptHit{
                        .slot = slot,
                        .fraction = *hit,
                        .proximityFallback = false,
                        .shield = true,
                    };
                }
            }
        }
        if (slot >= motion.size()
            || ((static_cast<std::uint8_t>(shot.targetLayers)
                 & static_cast<std::uint8_t>(motion[slot].airborne
                                                 ? unitdef::TargetLayerMask::Air
                                                 : unitdef::TargetLayerMask::Surface))
                == 0)) {
            continue;
        }
        const Fx radius = motion[slot].radiusElmos;
        if (radius <= Fx{}) {
            continue;
        }
        const std::array<Fx, 3> feet{
            transforms[slot].x, transforms[slot].y, transforms[slot].z};
        Fx height = catalog != nullptr ? catalog->intel(store.typeAt(slot)).eyeHeight : Fx{};
        if (height <= Fx{}) {
            height = radius * Fx::fromInt(2);
        }
        const std::array<Fx, 3> minimum{feet[0] - radius, feet[1], feet[2] - radius};
        const std::array<Fx, 3> maximum{feet[0] + radius, feet[1] + height, feet[2] + radius};
        std::optional<SweepFraction> hit;
        if (proximityFallback) {
            if (sphereBoxOverlap(from, minimum, maximum, kFxOne, false)) {
                hit = SweepFraction{0};
            }
        } else {
            hit = segmentBoxEntry(
                from, to, minimum, maximum, minimumFraction, maximumFraction);
        }
        if (hit && (!best || *hit < best->fraction)) {
            best = SweptHit{
                .slot = slot,
                .fraction = *hit,
                .proximityFallback = proximityFallback,
                .shield = false,
            };
        }
    }
    return best;
}

/// First place a flight segment enters terrain. Grid-line cuts keep every interval inside one
/// bilinear height-field cell. Along a straight segment that surface's clearance is quadratic,
/// so its endpoints and stationary point are enough to prove whether the shot crossed it.
[[nodiscard]] std::optional<SweepFraction> terrainEntry(
    std::array<Fx, 3> from, std::array<Fx, 3> to, const Terrain& terrain) noexcept {
    const auto clearanceAt = [&](SweepFraction fraction) {
        const Fx x = interpolateSweep(from[0], to[0], fraction);
        const Fx y = interpolateSweep(from[1], to[1], fraction);
        const Fx z = interpolateSweep(from[2], to[2], fraction);
        return y - terrain.heightAt(x, z);
    };
    if (clearanceAt(SweepFraction{}) <= Fx{}) {
        return SweepFraction{};
    }

    std::vector<SweepFraction> cuts{SweepFraction{}, kSweepOne};
    const auto addCuts = [&](Fx start, Fx finish, int squares) {
        const Fx delta = finish - start;
        if (delta == Fx{}) {
            return;
        }
        constexpr FxRaw squareRaw = Fx::fromInt(rm::kSquareSize).raw();
        const auto gridFloor = [](Fx coordinate) {
            int cell = coordinate.raw() / squareRaw;
            if (coordinate.raw() < 0 && coordinate.raw() % squareRaw != 0) {
                --cell;
            }
            return cell;
        };
        const int first =
            std::clamp(gridFloor(std::min(start, finish)) + 1, 0, squares);
        const int last = std::clamp(gridFloor(std::max(start, finish)), 0, squares);
        for (int line = first; line <= last; ++line) {
            const Fx boundary = Fx::fromInt(line * rm::kSquareSize);
            const SweepFraction fraction =
                ((FxWide{boundary.raw()} - start.raw()) * kSweepOne) / delta.raw();
            if (fraction > SweepFraction{} && fraction < kSweepOne) {
                cuts.push_back(fraction);
            }
        }
    };
    addCuts(from[0], to[0], terrain.field().squaresX);
    addCuts(from[2], to[2], terrain.field().squaresZ);
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    const Fx two = Fx::fromInt(2);
    const auto firstRoot = [&](SweepFraction clear, SweepFraction blocked) {
        // Terrain and entity hits must use the same time precision. Returning the first Q31
        // blocked sample keeps a body one position raw unit behind the surface behind it.
        while (blocked - clear > 1) {
            const SweepFraction middle = clear + (blocked - clear) / 2;
            if (clearanceAt(middle) > Fx{}) {
                clear = middle;
            } else {
                blocked = middle;
            }
        }
        return blocked;
    };

    for (std::size_t i = 1; i < cuts.size(); ++i) {
        const SweepFraction begin = cuts[i - 1];
        const SweepFraction end = cuts[i];
        const Fx atBegin = clearanceAt(begin);
        if (atBegin <= Fx{}) {
            return begin;
        }

        const SweepFraction middle = begin + (end - begin) / 2;
        const Fx atMiddle = clearanceAt(middle);
        const Fx atEnd = clearanceAt(end);

        // q(u) = a*u^2 + b*u + q(0), reconstructed at u = 0, 1/2 and 1.
        const Fx a = two * (atEnd + atBegin - two * atMiddle);
        const Fx b = atEnd - atBegin - a;
        if (a > Fx{}) {
            const Fx stationary = -b / (two * a);
            if (stationary > Fx{} && stationary < kFxOne) {
                const SweepFraction fraction = begin + roundShift(
                    FxWide{end - begin} * stationary.raw(), kFxFractionalBits);
                if (clearanceAt(fraction) <= Fx{}) {
                    return firstRoot(begin, fraction);
                }
            }
        }
        if (atEnd <= Fx{}) {
            return firstRoot(begin, end);
        }
    }
    return std::nullopt;
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

namespace {

/// Retail's range/arc classification, `0x006dc5c0` (`C-167`). Lower is better.
///
/// **0** in range and within the arc; **1** inside the minimum radius; **2** outside the
/// firing arc; **3** out of maximum range *or* beyond the weapon's height reach — the engine
/// folds "too far" and "cannot elevate" into one class, so a weapon that cannot look up
/// treats a target above it as unreachable rather than merely distant.
///
/// Distance is 2-D and squared, ignoring Y, exactly as the engine computes it.
enum class ReachClass : int { InRange = 0, TooClose = 1, OutsideArc = 2, CannotReach = 3 };

// Retail initializes an unidentified radar contact's priority row to this sentinel (`C-158`):
// it remains acquirable but loses every authored row and compares by score against other blips.
constexpr std::size_t kUnidentifiedPriorityRow = 9999;

[[nodiscard]] ReachClass classifyReach(const unitdef::Weapon& weapon, Fx groundDistance,
                                        Fx heightDifference, std::optional<Brad> heading = std::nullopt,
                                        std::optional<Brad> targetBearing = std::nullopt) noexcept {
    if (groundDistance > weapon.maxRange) {
        return ReachClass::CannotReach;
    }
    // Zero means unlimited: no shipped weapon states 0, and a literal zero would stop every
    // weapon shooting anything not exactly level with it.
    if (weapon.maxHeightDifference > Fx{} && heightDifference > weapon.maxHeightDifference) {
        return ReachClass::CannotReach;
    }
    if (groundDistance <= weapon.minRange) {
        return ReachClass::TooClose;
    }
    if (heading && targetBearing && weapon.arcRangeBrads < unitdef::kHalfTurnBrads) {
        const Brad arcCentre = static_cast<Brad>(static_cast<std::uint16_t>(*heading)
                                                 + static_cast<std::uint16_t>(weapon.arcCentreBrads));
        if (headingError(arcCentre, *targetBearing)
            > static_cast<std::uint32_t>(weapon.arcRangeBrads)) {
            return ReachClass::OutsideArc;
        }
    }
    return ReachClass::InRange;
}

/// Whether the active order is an explicit Attack, even when its target has gone stale.
///
/// A stale explicit order must hold fire rather than fall through into automatic acquisition.
/// That separation also keeps player intent from consulting automatic-target state.
[[nodiscard]] bool hasExplicitAttackOrder(UnitIndex slot, const UnitStore& store) noexcept {
    const std::span<const CommandQueue> orders = store.orders();
    return slot < orders.size() && orders[slot].active() != nullptr
           && orders[slot].active()->kind() == CommandKind::Attack;
}

/// The live entity target an active Attack order explicitly forces, if it has one.
///
/// An entity-target Attack is distinct from automatic acquisition: the command has already
/// named its desired target, so it must not be replaced by a nearer hostile at fire time.
[[nodiscard]] std::optional<UnitId> explicitAttackTarget(UnitIndex slot,
                                                          const UnitStore& store) noexcept {
    const std::span<const CommandQueue> orders = store.orders();
    if (slot >= orders.size()) {
        return std::nullopt;
    }
    const QueuedCommand* order = orders[slot].active();
    if (!hasExplicitAttackOrder(slot, store) || !store.alive(order->target())) {
        return std::nullopt;
    }
    return order->target();
}

/// Whether an explicit Attack's target passes the same fire gates as an acquired target,
/// including a Seen contact when fog of war is active.
[[nodiscard]] bool canShootExplicitTarget(UnitId target, std::array<Fx, 3> from, int fromArmy,
                                           const unitdef::Weapon& weapon,
                                           const UnitStore& store,
                                           std::span<const Army> armies,
                                           const UnitCatalog& catalog,
                                           const Intel* intel) noexcept {
    if (!shootable(fromArmy, store, target.index, armies)) {
        return false;
    }
    const std::span<const MoveState> motion = store.motion();
    if (target.index >= motion.size() || !weapon.canTarget(motion[target.index].airborne)) {
        return false;
    }
    if (intel != nullptr) {
        const Army* mine = armyFor(fromArmy, armies);
        if (mine == nullptr
            || contactKindForUnit(mine->alliance, target.index, store, catalog, armies, *intel)
                   != ContactKind::Seen) {
            return false;
        }
    }
    const Transform& transform = store.transforms()[target.index];
    const std::array<Fx, 3> to = positionOf(transform);
    const Fx heightDifference = to[1] > from[1] ? to[1] - from[1] : from[1] - to[1];
    const ReachClass reach = classifyReach(weapon, groundDistanceElmos(from, to), heightDifference);
    return reach != ReachClass::CannotReach && reach != ReachClass::TooClose;
}

/// Which priority row a candidate matches, or `npos` for none.
///
/// The row index is retail's one true lexicographic sort key (`C-157`): a row-0 match beats a
/// row-1 match however far away it is.
///
[[nodiscard]] std::size_t priorityRow(const unitdef::Weapon& weapon,
                                       const unitdef::UnitDef& candidate) {
    if (weapon.targetPriorities.empty()) {
        return std::numeric_limits<std::size_t>::max();
    }
    for (std::size_t row = 0; row < weapon.targetPriorities.size(); ++row) {
        if (unitdef::matchesExpression(
                unitdef::CategoryExpression{weapon.targetPriorities[row]}, candidate)) {
            return row;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

/// Whether a candidate survives the weapon's category restrictions. This intentionally uses the
/// common expression matcher: restrictions have the same AND-within, OR-across semantics as the
/// other FA category expressions rather than treating their text as special targeting layers.
[[nodiscard]] bool passesTargetRestrictions(const unitdef::Weapon& weapon,
                                            const unitdef::UnitDef& candidate) {
    if (weapon.targetRestrictOnlyAllow
        && !unitdef::matchesExpression(
            unitdef::CategoryExpression{*weapon.targetRestrictOnlyAllow}, candidate)) {
        return false;
    }
    return !weapon.targetRestrictOnlyDisallow
           || !unitdef::matchesExpression(
               unitdef::CategoryExpression{*weapon.targetRestrictOnlyDisallow}, candidate);
}

} // namespace

std::optional<UnitId> nearestTarget(std::array<Fx, 3> from, int fromArmy,
                                      const unitdef::Weapon& weapon, const UnitStore& store,
                                      std::span<const Army> armies, const Intel* intel,
                                      const UnitCatalog* catalog, std::optional<Brad> heading,
                                      std::optional<UnitId> incumbent) {
    if (!weapon.fires() || weapon.targetPriorities.empty()) {
        return std::nullopt;
    }

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();

    struct Candidate {
        UnitIndex slot;
        std::size_t row;
        std::uint64_t score;
    };

    const auto classifyCandidate = [&](UnitIndex slot) -> std::optional<Candidate> {
        if (!shootable(fromArmy, store, slot, armies)) {
            return std::nullopt;
        }
        if (store.doNotTarget(store.idAt(slot))) {
            return std::nullopt;
        }
        if (slot >= motion.size() || !weapon.canTarget(motion[slot].airborne)) {
            return std::nullopt;
        }
        const unitdef::UnitDef* def = catalog != nullptr ? catalog->def(store.typeAt(slot))
                                                           : nullptr;
        if (def != nullptr && def->hasCategory("BENIGN")) {
            return std::nullopt;
        }
        if (def != nullptr && !passesTargetRestrictions(weapon, *def)) {
            return std::nullopt;
        }

        bool prioritiesApply = true;
        if (intel != nullptr) {
            const Army* mine = armyFor(fromArmy, armies);
            if (mine == nullptr || catalog == nullptr) {
                return std::nullopt;
            }
            const std::optional<ContactKind> contact =
                contactKindForUnit(mine->alliance, slot, store, *catalog, armies, *intel);
            if (!contact || (*contact != ContactKind::Seen && *contact != ContactKind::Radar)) {
                return std::nullopt;
            }
            prioritiesApply = *contact == ContactKind::Seen
                           || intel->hasSeenEver(mine->alliance, store.idAt(slot));
        }

        const Fx dx = transforms[slot].x - from[0];
        const Fx dz = transforms[slot].z - from[2];
        const Fx distance = fxHypot(dx, dz);
        const Fx dy = transforms[slot].y > from[1] ? transforms[slot].y - from[1]
                                                    : from[1] - transforms[slot].y;
        const ReachClass reach = classifyReach(weapon, distance, dy, heading,
                                                bearingTo(from, positionOf(transforms[slot])));
        if (reach == ReachClass::CannotReach || reach == ReachClass::TooClose) {
            return std::nullopt;
        }

        const std::size_t row = !prioritiesApply ? kUnidentifiedPriorityRow
                              : def != nullptr  ? priorityRow(weapon, *def)
                                                : 0;
        if (row == std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }

        const FxWide dxRaw = dx.raw();
        const FxWide dzRaw = dz.raw();
        std::uint64_t score = static_cast<std::uint64_t>(dxRaw * dxRaw)
                            + static_cast<std::uint64_t>(dzRaw * dzRaw);
        if (reach != ReachClass::InRange) {
            score *= 4;
        }
        return Candidate{.slot = slot, .row = row, .score = score};
    };

    // An incumbent is evaluated before the grid walk, so insertion order cannot dislodge it on
    // an equal score. The same predicate clears stale, dead, invisible, restricted, or
    // out-of-range handles by declining to seed the comparison.
    std::optional<Candidate> best;
    if (incumbent && store.alive(*incumbent)) {
        best = classifyCandidate(incumbent->index);
    }

    // THE GRID, rather than every slot in the store (§7 P5.2). The query radius is the weapon's
    // own reach, so a scan that used to be over every unit in the match is now over the handful
    // within range — and `minRange` is still applied below, because a dead zone is a hole in the
    // middle of the disc and not a smaller disc.
    for (const UnitIndex slot : store.space().within(from[0], from[2], weapon.maxRange)) {
        const std::optional<Candidate> candidate = classifyCandidate(slot);
        if (!candidate) {
            continue;
        }
        if (!best || candidate->row < best->row
            || (candidate->row == best->row && candidate->score < best->score)) {
            best = candidate;
        }
    }

    if (!best) {
        return std::nullopt;
    }
    return store.idAt(best->slot);
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
    // Converted once at load — `firingToleranceBradsFromDegrees`, in the unit layer where
    // content's floats belong. The tick only ever compares integers; it used to re-run the
    // degrees→brad lround on every ready shot, which was both wasted work and a float
    // expression the no-float check could not see.
    return static_cast<std::int64_t>(headingError(yaw, bearing))
           <= static_cast<std::int64_t>(weapon.firingToleranceBrads);
}

std::size_t aimAtTargets(UnitStore& store, const UnitCatalog& catalog,
                         std::span<const Army> armies, const Intel* intel) {
    const std::span<Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<const Health> healths = store.health();

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

        const bool hasExplicitAttack = hasExplicitAttackOrder(slot, store);
        const std::optional<UnitId> forced = explicitAttackTarget(slot, store);

        // Only the weapons that need the hull pointed are worth turning for, and the longest
        // reach among them decides how far away a unit bothers to aim. Per unit now rather
        // than per batch, because the definition is per unit — the loop is over a handful of
        // weapons and costs nothing next to the sweep below.
        // Query the real weapons. Combining the longest range with a union of target masks
        // invents a gun the unit does not own (for example long surface range plus short AA).
        std::optional<UnitId> target;
        Fx targetDistance{};
        for (std::size_t w = 0; w < def->weapons.size(); ++w) {
            const unitdef::Weapon& weapon = def->weapons[w];
            if (!weapon.fires() || weapon.turreted) {
                continue;
            }
            const std::array<Fx, 3> from = positionOf(transforms[slot]);
            std::optional<UnitId> candidate;
            if (hasExplicitAttack) {
                candidate = forced && canShootExplicitTarget(*forced, from, motion[slot].armyIndex,
                                                              weapon, store, armies, catalog, intel)
                                ? forced
                                : std::nullopt;
            } else {
                const std::optional<UnitId> incumbent =
                    slot < healths.size() && w < healths[slot].automaticTargets.size()
                        ? std::optional<UnitId>{healths[slot].automaticTargets[w]}
                        : std::nullopt;
                candidate = nearestTarget(from, motion[slot].armyIndex, weapon, store, armies,
                                          intel, &catalog, transforms[slot].heading, incumbent);
            }
            if (!candidate) {
                continue;
            }
            const Fx distance = groundDistanceElmos(positionOf(transforms[slot]),
                                                    positionOf(transforms[candidate->index]));
            if (!target || distance < targetDistance
                || (distance == targetDistance && candidate->index < target->index)) {
                target = candidate;
                targetDistance = distance;
            }
        }
        if (!target) {
            continue;
        }

        const std::array<Fx, 3> at = positionOf(transforms[target->index]);
        const Brad bearing = bearingTo(positionOf(transforms[slot]), at);
        const std::uint32_t error = headingError(transforms[slot].heading, bearing);
        // Aimed exactly: nothing to turn. (This used to read `error <= 1e-4f`, a float
        // literal against an unsigned integer — invisible to the no-float check because it
        // names neither `float` nor a cast, and degenerate either way: an integral error is
        // below 1e-4 only when it is zero.)
        if (error == 0) {
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
                        EventQueue* events, const Intel* intel) {
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
        const bool hasExplicitAttack = hasExplicitAttackOrder(slot, store);
        const std::optional<UnitId> forced = explicitAttackTarget(slot, store);
        if (!hasExplicitAttack) {
            health.automaticTargets.resize(def->weapons.size());
        }

        for (std::size_t w = 0; w < def->weapons.size(); ++w) {
            const unitdef::Weapon& weapon = def->weapons[w];
            if (!weapon.fires()) {
                // A MANUAL weapon's reload still counts down here, where every reload
                // does — `fireOvercharge` only checks readiness, and a cooldown that
                // ticked only while an order was held would punish the second click for
                // the first one's timing.
                if (weapon.manuallyFired() && health.reloadRemaining[w] > 0) {
                    --health.reloadRemaining[w];
                }
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

            const std::optional<UnitId> target = hasExplicitAttack
                ? (forced && canShootExplicitTarget(*forced, from, army, weapon, store, armies,
                                                    catalog, intel)
                       ? forced
                       : std::nullopt)
                : nearestTarget(from, army, weapon, store, armies, intel, &catalog,
                                transforms[slot].heading, health.automaticTargets[w]);
            if (!hasExplicitAttack) {
                health.automaticTargets[w] = target.value_or(UnitId{});
            }
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
            if (weapon.beam) {
                // A BEAM DELIVERS NOW: no projectile, no flight, the damage lands where
                // the target stands this tick. The event carries both ends because the
                // beam is the line between them, and it is all a renderer needs.
                emit(events, Event{
                                 .kind = EventKind::BeamFired,
                                 .unit = store.idAt(slot),
                                 .instigator = *target,
                                 .army = army,
                                 .amount = weapon.damage,
                                 .at = to,
                                 .at2 = {from[0],
                                         from[1]
                                             + (weapon.muzzleHeight > Fx{}
                                                    ? weapon.muzzleHeight
                                                    : kMuzzleHeight),
                                         from[2]},
                             });
                if (weapon.damageRadius <= Fx{}) {
                    (void)damageTarget(target->index, rates.damage, army, store, armies,
                                       &catalog, store.idAt(slot), events,
                                       weapon.targetLayers);
                } else {
                    damageArea(to, weapon.damageRadius, rates.damage, army, store, armies,
                               &catalog, store.idAt(slot), events, weapon.targetLayers);
                }
            } else {
                // THE SIMULTANEOUS SALVO (11 §3.3): a salvo size above one with NO delay
                // means every muzzle fires on the same trigger pull — Moho sets
                // numMuzzlesFiring to the whole rack. It is a different mechanic from the
                // timed burst below (delay > 0), and reading it as one shot per reload
                // cost those 106 weapons up to half their authored damage.
                const int volley = weapon.bursts() ? 1 : rates.burstSize;
                for (int shot = 0; shot < volley; ++shot) {
                    projectiles.push_back(
                        launch(from, to, weapon, army, rate, rates.muzzlePerTick,
                               rates.damage, store.idAt(slot)));
                }

                emit(events, Event{
                                 .kind = EventKind::WeaponFired,
                                 .unit = store.idAt(slot),
                                 .instigator = *target,
                                 .army = army,
                                 .amount = weapon.damage,
                                 .at = from,
                             });
            }
            ++fired;

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
            if (weapon.bursts()) {
                if (health.burstRemaining[w] == 0) {
                    health.burstRemaining[w] = rates.burstSize;
                }
                --health.burstRemaining[w];
                health.reloadRemaining[w] = health.burstRemaining[w] > 0
                                                ? static_cast<int>(rates.burstDelayTicks)
                                                : static_cast<int>(rates.reloadTicks);
            } else {
                // The volley above delivered the whole rack at once; a plain reload
                // follows. Letting the burst cycle run here with a zero delay would fire
                // again next tick and deliver the salvo twice over.
                health.burstRemaining[w] = 0;
                health.reloadRemaining[w] = static_cast<int>(rates.reloadTicks);
            }
        }
    }

    return fired;
}

std::size_t fireOvercharge(UnitStore& store, const UnitCatalog& catalog,
                           std::span<const Army> armies,
                           std::vector<Projectile>& projectiles,
                           std::span<Economy> economies, TickRate rate, EventQueue* events) {
    std::size_t fired = 0;

    const std::span<const Transform> transforms = store.transforms();
    const std::span<Health> healths = store.health();
    const std::span<CommandQueue> orders = store.orders();

    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || slot >= healths.size() || !healths[slot].alive()) {
            continue;
        }
        QueuedCommand* head = orders[slot].activeMutable();
        if (head == nullptr || head->kind() != CommandKind::Overcharge
            || !store.alive(head->target())) {
            continue;  // no order, or a spent/expired one advanceOrders will retire
        }

        const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }

        const int army = armyAt(store, slot);
        const int theirArmy = armyAt(store, head->target().index);
        const Army* mine = armyFor(army, armies);
        const Army* theirs = armyFor(theirArmy, armies);
        if (mine == nullptr || theirs == nullptr || !hostile(*mine, *theirs)) {
            // A target that stopped being shootable — captured maps aside, a defeated
            // army — retires the order the same way a fired one does.
            head->setTarget(UnitId{});
            continue;
        }

        for (std::size_t w = 0; w < def->weapons.size(); ++w) {
            const unitdef::Weapon& weapon = def->weapons[w];
            if (!weapon.manuallyFired()) {
                continue;
            }
            if (!weapon.canTarget(store.motion()[head->target().index].airborne)) {
                continue;
            }
            healths[slot].reloadRemaining.resize(def->weapons.size(), 0);
            if (healths[slot].reloadRemaining[w] > 0) {
                continue;  // still cooling from the last click; `fireWeapons` ticks it
            }

            const std::array<Fx, 3> from = positionOf(transforms[slot]);
            const std::array<Fx, 3> to = positionOf(transforms[head->target().index]);
            if (groundDistanceElmos(from, to) > weapon.maxRange) {
                continue;  // the pursuit is still closing
            }

            // THE ENERGY GATE, and the mechanic: the shot costs `EnergyRequired` from the
            // army's store the tick it fires, and a short bar holds the shot rather than
            // spending what is not there.
            if (army < 0 || static_cast<std::size_t>(army) >= economies.size()) {
                continue;
            }
            Economy& economy = economies[static_cast<std::size_t>(army)];
            if (economy.stored.energy < weapon.energyRequired) {
                continue;
            }
            economy.stored.energy -= weapon.energyRequired;

            const UnitCatalog::WeaponRates& rates = catalog.weaponRates(store.typeAt(slot), w);
            projectiles.push_back(launch(from, to, weapon, army, rate, rates.muzzlePerTick,
                                         rates.damage, store.idAt(slot)));
            healths[slot].reloadRemaining[w] = static_cast<int>(rates.reloadTicks);
            emit(events, Event{
                             .kind = EventKind::WeaponFired,
                             .unit = store.idAt(slot),
                             .instigator = head->target(),
                             .army = army,
                             .amount = weapon.damage,
                             .at = from,
                         });
            ++fired;

            // ONE SHOT PER ORDER: forgetting the target is what completes it —
            // `advanceOrders` retires a targetless overcharge like any arrival.
            head->setTarget(UnitId{});
            break;
        }
    }

    return fired;
}

void tickShields(UnitStore& store, const UnitCatalog& catalog, EventQueue* events) {
    const std::span<Health> health = store.health();
    for (UnitIndex slot = 0; slot < health.size(); ++slot) {
        if (!store.slotAlive(slot) || !health[slot].alive()) {
            continue;
        }
        ShieldState& state = health[slot].shield;
        const UnitCatalog::ShieldInfo& shield = catalog.shield(store.typeAt(slot));
        if (!shield.exists() || state.maximum <= Mag{}) {
            continue;
        }
        if (state.rechargeRemaining > 0) {
            --state.rechargeRemaining;
            if (state.rechargeRemaining == 0) {
                state.current = state.maximum;
                emit(events, Event{.kind = EventKind::ShieldRestored,
                                   .unit = store.idAt(slot),
                                   .army = armyAt(store, slot),
                                   .at = positionOf(store.transforms()[slot])});
            }
            continue;
        }
        if (state.current >= state.maximum) {
            state.current = state.maximum;
            continue;
        }
        if (state.regenDelayRemaining > 0) {
            --state.regenDelayRemaining;
            continue;
        }
        state.current = std::min(state.maximum, state.current + shield.regenPerTick);
        if (state.current == state.maximum) {
            emit(events, Event{.kind = EventKind::ShieldRestored,
                               .unit = store.idAt(slot),
                               .army = armyAt(store, slot),
                               .at = positionOf(store.transforms()[slot])});
        }
    }
}

Projectile launch(std::array<Fx, 3> from, std::array<Fx, 3> to,
                  const unitdef::Weapon& weapon, int byArmy, TickRate rate, Fx muzzlePerTick,
                  const unitdef::DamageProfile& damage, UnitId firedBy) {
    Projectile shot;
    shot.firedBy = firedBy;
    // The model's own muzzle when the app resolved one, the old constant when not —
    // Weapon::muzzleHeight's contract.
    const Fx muzzle = weapon.muzzleHeight > Fx{} ? weapon.muzzleHeight : kMuzzleHeight;
    shot.position = {from[0], from[1] + muzzle, from[2]};
    shot.damage = damage;
    shot.damageRadiusElmos = weapon.damageRadius;
    shot.targetLayers = weapon.targetLayers;
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
               EventQueue* events, const UnitCatalog* catalog) {
    // The scalar form, kept because it is what two dozen call sites mean — most of them tests
    // asserting blast reach, which is a property of the geometry and has nothing to say
    // about armour. A flat profile answers the same for every class, so this is not an
    // approximation of the call below: it is the same call with a table that has no entries.
    return damageArea(centre, radiusElmos, unitdef::flatDamage(damage), byArmy, store, armies,
                       catalog, by, events);
}

namespace {

Mag damageTargets(std::array<Fx, 3> centre, Fx radiusElmos,
                  const unitdef::DamageProfile& damage, int byArmy, UnitStore& store,
                   std::span<const Army> armies, const UnitCatalog* catalog, UnitId by,
                   EventQueue* events, unitdef::TargetLayerMask targetLayers, bool damageFriendly,
                   std::optional<UnitIndex> exactTarget,
                   std::optional<UnitIndex> impactTarget) {
    Mag dealt{};

    const std::span<const Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    const std::span<Health> healths = store.health();
    const auto damageable = [&](UnitIndex slot) {
        if (!damageFriendly) {
            return shootable(byArmy, store, slot, armies);
        }
        if (slot >= healths.size() || !healths[slot].alive()) {
            return false;
        }
        const Army* source = armyFor(byArmy, armies);
        const Army* target = armyFor(armyAt(store, slot), armies);
        return source != nullptr && !source->defeated
               && target != nullptr && !target->defeated
               && store.idAt(slot) != by;
    };

    // A blast is a sphere against collision boxes, not a centre-only ground circle. A square
    // box corner is at sqrt(2) body radii, so two radii are a cheap conservative broadphase;
    // `sphereBoxOverlap` below remains the answer. Point hits retain their old tolerance.
    const Fx largestBodyRadius = largestRadius(store);
    const Fx reach = radiusElmos > Fx{}
                       ? radiusElmos + largestBodyRadius * Fx::fromInt(2)
                       : std::max(kFxOne, largestBodyRadius);

    // Retail builds one shield list for the whole area blast (`C-143`): the blast sphere must
    // intersect the dome, while a blast centre inside the dome's radius minus 0.1 skips it.
    // Every admitted dome pays once, even if no target beneath it is hit. Coverage is still
    // checked per target, so one admitted dome can shelter every hull below it for that price.
    //
    // Radius-zero damage retains the existing point-hit bridge until projectiles can collide
    // with shield entities: it gathers nearby domes and charges only those covering a target.
    //
    // WHAT IS NOT CHANGED, deliberately: the reduction stays a proportional rescale rather
    // than becoming a flat subtraction, even though retail subtracts flat. `DamageProfile`
    // holds damage already resolved per armour class (`ADR-033`), so `against(c) = A·M(c)`,
    // and scaling by `(A−S)/A` yields `(A−S)·M(c)` — exactly retail's subtract-then-multiply.
    // The two are the same arithmetic in different coordinates; a literal flat subtraction on
    // a resolved profile would be the thing that is wrong.
    struct BlastShield {
        UnitIndex slot;
        std::array<Fx, 3> centre;
        Fx radius;
        Mag absorb;      ///< decided once from the whole blast, spent across every target
        bool covered;    ///< radius-zero compatibility: whether this dome covered the hit
    };
    std::vector<BlastShield> shields;
    const bool areaBlast = radiusElmos > Fx{};

    // What a bubble sees, as opposed to what a hull sees. Computed from the ORIGINAL profile,
    // once: every bubble in the blast faces the same shot.
    const Mag shieldIncoming =
        catalog != nullptr ? damage.against(catalog->armor().classFor("Shield")) : Mag{};

    if (catalog != nullptr && shieldIncoming > Mag{} && catalog->largestShieldRadius() > Fx{}) {
        // A bubble whose CENTRE is far outside the blast can still cover a target inside it,
        // so the search has to be widened by the largest bubble's radius. Querying at the
        // blast radius alone is how the old code came to miss shields it should have found.
        // `reach` already includes collision-box corners. Add only the distance from a covered
        // body to the generator at the centre of its shield bubble.
        const Fx search = reach + catalog->largestShieldRadius();
        for (const UnitIndex slot : store.space().within(centre[0], centre[2], search)) {
            if (!damageable(slot) || slot >= healths.size()) {
                continue;
            }
            const UnitCatalog::ShieldInfo& shield = catalog->shield(store.typeAt(slot));
            if (!shield.exists() || !healths[slot].shield.active()) {
                continue;
            }
            std::array<Fx, 3> shieldCentre = positionOf(transforms[slot]);
            shieldCentre[1] += shield.verticalOffsetElmos;
            if (areaBlast) {
                const Fx insideRadius = shield.radiusElmos - Fx::fromRatio(1, 10);
                // A degenerate box is an exact point-distance test using the helper's widened
                // raw squares; ordinary Fx multiplication would saturate above ~362 elmos.
                const bool centreInside =
                    (insideRadius == Fx{} && centre == shieldCentre)
                    || sphereBoxOverlap(centre, shieldCentre, shieldCentre, insideRadius, true);
                if (centreInside
                    || !sphereBoxOverlap(centre, shieldCentre, shieldCentre,
                                         shield.radiusElmos + radiusElmos, true)) {
                    continue;
                }
            }
            shields.push_back(BlastShield{
                .slot = slot,
                .centre = shieldCentre,
                .radius = shield.radiusElmos,
                .absorb = std::min(healths[slot].shield.current, shieldIncoming),
                .covered = false,
            });
        }
    }

    /// How much the bubbles over `at` take off this shot.
    ///
    /// Summed across every covering bubble rather than stopping at the first, because
    /// overlapping shields stacking is a real Forged Alliance mechanic and the old `break`
    /// silently gave it away — a probe put 150 damage into two overlapping 100-point domes and
    /// drained one to zero while the other never fired.
    const auto absorbedOver = [&shields](std::array<Fx, 3> at) {
        Mag total{};
        for (BlastShield& bubble : shields) {
            const Fx dx = at[0] - bubble.centre[0];
            const Fx dy = at[1] - bubble.centre[1];
            const Fx dz = at[2] - bubble.centre[2];
            if (fxSqrt(dx * dx + dy * dy + dz * dz) > bubble.radius) {
                continue;
            }
            bubble.covered = true;
            total += bubble.absorb;
        }
        return total;
    };

    const auto damageOne = [&](UnitIndex slot) {
        if (exactTarget && slot != *exactTarget) {
            return;
        }
        if (!damageable(slot)) {
            return;
        }
        if (slot >= motion.size()
            || ((static_cast<std::uint8_t>(targetLayers)
                 & static_cast<std::uint8_t>(motion[slot].airborne
                                                  ? unitdef::TargetLayerMask::Air
                                                  : unitdef::TargetLayerMask::Surface))
                == 0)) {
            return;
        }

        const Fx distance = groundDistanceElmos(centre, positionOf(transforms[slot]));

        // WITHIN THE RADIUS IS WITHIN THE RADIUS — no distance falloff, because Forged
        // Alliance has none.
        //
        // This used to be `1 - distance/radius`, which is what Total Annihilation and Spring
        // do and what most people assume every RTS does. Supreme Commander does not. Traced
        // through the retail executable: `DealDamage` (`0x0073dbc0`) is reached from the
        // sphere worker (`0x0073e100`) and the ring worker (`0x0073e5b0`), and between the
        // spatial query and that call the *only* thing that touches the amount is shield
        // absorption — a flat subtraction with no distance term. The target-to-centre delta is
        // computed, but it is written into the damage record as a DIRECTION and never
        // multiplied in. See `docs/fa-exe-analysis-plan.md`, claim `C-061`.
        //
        // The correction matters more than it looks. Under linear falloff the average victim
        // of a blast took roughly half the stated damage; now every one of them takes all of
        // it, so blast weapons hit far harder, more units die per shot, and the wreck and
        // state-hash consequences follow. Blueprint damage numbers finally mean what the
        // blueprint says.
        Fx share{};
        if (radiusElmos <= Fx{}) {
            // A point hit. Only what is essentially AT the centre takes it, and the
            // tolerance is the unit's own radius rather than zero — a shot aimed at
            // a unit's position that lands a tenth of an elmo away has hit it.
            const Fx tolerance =
                std::max(kFxOne, slot < motion.size() ? motion[slot].radiusElmos : kFxOne);
            share = distance <= tolerance ? kFxOne : Fx{};
        } else {
            // Uniform wherever the sphere overlaps the current collision primitive, including
            // the rim. Rechecking at delivery keeps a delayed splash at its recorded point: it
            // neither follows the body originally struck nor misses one that moved into it.
            const Fx bodyRadius = motion[slot].radiusElmos;
            Fx height = catalog != nullptr
                          ? catalog->intel(store.typeAt(slot)).eyeHeight
                          : Fx{};
            if (height <= Fx{}) {
                height = bodyRadius * Fx::fromInt(2);
            }
            const std::array<Fx, 3> feet = positionOf(transforms[slot]);
            const std::array<Fx, 3> minimum{
                feet[0] - bodyRadius, feet[1], feet[2] - bodyRadius};
            const std::array<Fx, 3> maximum{
                feet[0] + bodyRadius, feet[1] + height, feet[2] + bodyRadius};
            share = bodyRadius > Fx{}
                            && sphereBoxOverlap(centre, minimum, maximum, radiusElmos, true)
                      ? kFxOne
                      : Fx{};
        }

        if (share <= Fx{}) {
            return;
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
        // ORDER USED TO MATTER HERE and no longer does, which is worth saying rather than
        // silently deleting: the armour lookup ran first and a distance falloff scaled what it
        // left. `C-061` removed the falloff, so the share is now 1 or 0 and the two operations
        // commute. The lookup stays inside the loop because the armour CLASS still varies per
        // target — a shell landing between a tank and a bunker hits two classes.
        // WHAT THE BUBBLES OVER **THIS** TARGET TAKE OFF THE SHOT. Tested against the target's
        // own position, which is the whole of `C-110`: a unit is sheltered when it is under a
        // dome, not when the explosion happens to be.
        Fx shielded = kFxOne;
        if (shieldIncoming > Mag{}) {
            const Mag absorbed =
                std::min(shieldIncoming, absorbedOver(positionOf(transforms[slot])));
            if (absorbed >= shieldIncoming) {
                return;   // fully covered: this target takes nothing at all
            }
            shielded = fraction(shieldIncoming - absorbed, shieldIncoming);
        }

        const Mag wanted = damage.against(armor) * share * shielded;
        // Retail returns after mitigation when the final amount is non-positive. Besides
        // preventing negative damage from healing, this matters for the shipped 0.0 armour
        // multipliers: no `UnitDamaged` callback is raised for a blow that became nothing.
        if (wanted <= Mag{}) {
            return;
        }
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
                         // Retail publishes the incoming amount before health clips overkill.
                         .amount = wanted,
                         .at = positionOf(transforms[slot]),
                     });
    };

    // The grid returns ascending slots. Retain the contacted handle in this merge so slot order
    // stays stable, but damageOne rechecks its CURRENT primitive against the recorded sphere.
    bool impactHandled = false;
    for (const UnitIndex slot : store.space().within(centre[0], centre[2], reach)) {
        if (impactTarget && !impactHandled && *impactTarget < slot) {
            damageOne(*impactTarget);
            impactHandled = true;
        }
        damageOne(slot);
        impactHandled = impactHandled || (impactTarget && slot == *impactTarget);
    }
    if (impactTarget && !impactHandled) {
        damageOne(*impactTarget);
    }

    // THE BUBBLES ARE CHARGED LAST, and each exactly once however many targets it sheltered —
    // retail decides a shield's absorption up front and runs the shield itself through the
    // damage path afterwards (`C-062`). Charging inside the target loop instead would drain a
    // dome once per unit standing under it, which is a different and much harsher game.
    //
    // Area-blast admission itself incurs the charge. Radius-zero compatibility still requires
    // actual coverage because it has no blast sphere to intersect with the dome.
    for (const BlastShield& bubble : shields) {
        if ((!areaBlast && !bubble.covered) || bubble.absorb <= Mag{}) {
            continue;
        }
        Health& owner = healths[bubble.slot];
        const UnitCatalog::ShieldInfo& shield = catalog->shield(store.typeAt(bubble.slot));
        const Mag absorbed = std::min(owner.shield.current, bubble.absorb);
        owner.shield.current -= absorbed;
        owner.shield.regenDelayRemaining = shield.regenDelay;
        dealt += absorbed;
        emit(events, Event{.kind = EventKind::ShieldDamaged,
                           .unit = store.idAt(bubble.slot),
                           .instigator = by,
                           .army = armyAt(store, bubble.slot),
                           .amount = absorbed,
                           .at = centre});

        if (owner.shield.current <= Mag{}) {
            owner.shield.current = Mag{};
            owner.shield.regenDelayRemaining = 0;
            owner.shield.rechargeRemaining = shield.recharge;
            emit(events, Event{.kind = EventKind::ShieldCollapsed,
                               .unit = store.idAt(bubble.slot),
                               .instigator = by,
                               .army = armyAt(store, bubble.slot),
                               .at = centre});
        }
    }

    return dealt;
}

[[nodiscard]] Mag damageTarget(UnitIndex target, const unitdef::DamageProfile& damage,
                               int byArmy, UnitStore& store, std::span<const Army> armies,
                               const UnitCatalog* catalog, UnitId by, EventQueue* events,
                               unitdef::TargetLayerMask targetLayers) {
    if (target >= store.transforms().size()) {
        return Mag{};
    }
    return damageTargets(positionOf(store.transforms()[target]), Fx{}, damage, byArmy, store,
                          armies, catalog, by, events, targetLayers, false, target, std::nullopt);
}

} // namespace

Mag damageArea(std::array<Fx, 3> centre, Fx radiusElmos,
               const unitdef::DamageProfile& damage, int byArmy, UnitStore& store,
               std::span<const Army> armies, const UnitCatalog* catalog, UnitId by,
               EventQueue* events, unitdef::TargetLayerMask targetLayers) {
    return damageTargets(centre, radiusElmos, damage, byArmy, store, armies, catalog, by,
                          events, targetLayers, false, std::nullopt, std::nullopt);
}

void advanceProjectiles(std::vector<Projectile>& projectiles, UnitStore& store,
                        std::span<const Army> armies, const Terrain& terrain, TickRate rate,
                        EventQueue* events, const UnitCatalog* catalog) {
    const Fx gravityPerTickSquared = projectileGravityPerTickSquared(rate);

    for (Projectile& shot : projectiles) {
        // Retail detects contact in one MotionTick and invokes Impact at the start of the next.
        // This branch runs before lifetime and motion so the projectile remains exactly at the
        // recorded point for that intervening tick (`C-173`).
        if (shot.pendingImpact != ImpactType::Invalid) {
            const UnitId target = store.alive(shot.impactTarget)
                                    ? shot.impactTarget
                                    : UnitId{};
            emit(events, Event{
                             .kind = EventKind::ProjectileImpact,
                             .unit = target,
                             .instigator = shot.firedBy,
                             .army = shot.firedByArmy,
                             // The BASE, because an event says what was thrown rather than
                             // what each target took — `UnitDamaged` carries the latter.
                             .amount = shot.damage.base,
                             .at = shot.position,
                             .impactType = shot.pendingImpact,
                         });
            if (shot.damageRadiusElmos <= Fx{}) {
                if (target.generation != 0) {
                    (void)damageTarget(target.index, shot.damage, shot.firedByArmy, store,
                                       armies, catalog, shot.firedBy, events,
                                       shot.targetLayers);
                }
            } else {
                const std::optional<UnitIndex> impactTarget = target.generation != 0
                                                                ? std::optional<UnitIndex>{
                                                                      target.index}
                                                                : std::nullopt;
                (void)damageTargets(shot.position, shot.damageRadiusElmos, shot.damage,
                                     shot.firedByArmy, store, armies, catalog, shot.firedBy,
                                     events, shot.targetLayers, false, std::nullopt, impactTarget);
            }

            // Recoil Metal has no Lua projectile lifecycle yet, so retain its established
            // destroy-after-impact policy after reproducing the native one-tick timing.
            shot.pendingImpact = ImpactType::Invalid;
            shot.impactTarget = {};
            shot.ticksRemaining = 0;
            continue;
        }

        if (shot.ticksRemaining <= 0) {
            continue;
        }
        --shot.ticksRemaining;

        const std::array<Fx, 3> oldVelocity = shot.velocity;
        if (shot.arc != unitdef::BallisticArc::None) {
            shot.velocity[1] -= gravityPerTickSquared;
        }

        const std::array<Fx, 3> from = shot.position;
        // Velocity is already per tick. Retail integrates the average before and after this
        // tick's acceleration; the delta form avoids overflowing `old + new` before halving.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const Fx averageVelocity =
                oldVelocity[axis]
                + (shot.velocity[axis] - oldVelocity[axis]) * Fx::fromRatio(1, 2);
            shot.position[axis] += averageVelocity;
        }

        // TWO ways a shot ends, and both are needed.
        //
        // It HITS something: the first hostile body on the extended sweep, or a body inside
        // C-168's old-position sphere when motion is too small for a sweep.
        //
        // Or it reaches the GROUND, which is what a miss does. Height is the test there
        // rather than proximity, because a near miss must land rather than fly on and hit
        // whatever happens to be behind it.
        const std::optional<SweptHit> struck =
            firstStruck(shot, from, shot.position, store, armies, catalog);
        const std::optional<SweepFraction> groundHit =
            terrainEntry(from, shot.position, terrain);
        // Retail skips the fallback entity query entirely once this tick has a surface hit.
        const bool hitUnit =
            struck
            && (!groundHit
                || (!struck->proximityFallback
                    && struck->fraction < *groundHit));

        if (!hitUnit && !groundHit) {
            if (shot.ticksRemaining <= 0) {
                shot.pendingImpact = shot.position[1] < terrain.waterLevel()
                                       ? ImpactType::Underwater
                                       : ImpactType::Air;
                shot.impactTarget = {};
            }
            continue;
        }

        if (hitUnit) {
            if (struck->proximityFallback) {
                shot.position = from;
            } else {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    shot.position[axis] =
                        interpolateSweep(from[axis], shot.position[axis], struck->fraction);
                }
            }
            shot.impactTarget = store.idAt(struck->slot);
            if (struck->shield) {
                shot.pendingImpact = ImpactType::Shield;
            } else if (shot.position[1] < terrain.waterLevel()) {
                shot.pendingImpact = ImpactType::UnitUnderwater;
            } else {
                shot.pendingImpact = store.motion()[struck->slot].airborne
                                       ? ImpactType::UnitAir
                                       : ImpactType::Unit;
            }
        } else {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                shot.position[axis] =
                    interpolateSweep(from[axis], shot.position[axis], *groundHit);
            }
            shot.position[1] = terrain.heightAt(shot.position[0], shot.position[2]);
            shot.pendingImpact = ImpactType::Terrain;
            shot.impactTarget = {};
        }
    }

    // A zero-lifetime shot with an impact still has one tick of work left. Everything else at
    // zero is spent, removed after the pass so the list is not resized while it is walked.
    std::erase_if(projectiles, [](const Projectile& shot) {
        return shot.ticksRemaining <= 0 && shot.pendingImpact == ImpactType::Invalid;
    });
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
        dealt += damageTargets(at, blast->outerRingRadius, profile(blast->outerRingDamage), byArmy,
                                store, armies, catalog, by, events, blast->targetLayers,
                                blast->damageFriendly, std::nullopt, std::nullopt);
        dealt += damageTargets(at, blast->innerRingRadius, profile(blast->innerRingDamage), byArmy,
                                store, armies, catalog, by, events, blast->targetLayers,
                                blast->damageFriendly, std::nullopt, std::nullopt);
        return dealt;
    }

    return damageTargets(at, blast->damageRadius, profile(blast->damage), byArmy, store, armies,
                         catalog, by, events, blast->targetLayers, blast->damageFriendly,
                         std::nullopt, std::nullopt);
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
