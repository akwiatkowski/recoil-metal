#pragma once

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// Things on the map that are not units: wrecks, and later trees and rocks.
//
// WHY THIS EXISTS (PLAN2.md §6.6, §7 P6.2). A death left a scorch mark, and a scorch mark had
// nowhere to live, so it became `std::vector<DecalVertex>` — GPU VERTICES — accumulated in the
// app's scene state. That is the wrong shape twice over: the record of what died here is game
// state, not geometry, and a caller that wants to know what is on the ground has to read a
// vertex buffer and reverse the triangles.
//
// A wreck is an OBJECT. It has a position, a size, and the identity of what it used to be. Make
// it one and the decal becomes a projection of it — one the renderer computes and throws away
// each frame, like every other projection — rather than the only place the fact is stored.
//
// WHAT §6.6 ASKS FOR AND THIS DOES NOT DO YET, stated plainly: one `ObjectId` space covering
// units, features and projectiles, so that targeting, collision and area damage take an
// `ObjectId` and do not care which they got. That is the right end state and it has no consumer
// for named attacks today. Area damage already enumerates the feature pool separately. Building the
// shared id space before anything asks a question through it would be a refactor of every
// `UnitId` in the sim in exchange for nothing observable. `FeatureId` is its own handle from
// the same `IdPool`, so the merge later is a rename plus a tag rather than a redesign.
//
// WHAT A FEATURE IS NOT, yet: a named attack target or an obstacle. RECLAIMABLE it now is — a wreck
// carries what reclaiming it still yields, the harvest pass drains it, and an emptied one is
// REMOVED. That ended two older decisions at once, both recorded below where they applied:
// append-only storage, and staying out of the state hash.
//
// IN THE STATE HASH since reclaim, exactly as the old note here said it would have to be: the
// moment a feature is read back into a rule, "derived one-for-one from a death" stops being
// the whole story — how much of a wreck is LEFT depends on who reclaimed it and when, which
// no unit's row records.

/// A handle to a feature. Generational, for the same reason a unit's is: a slot is reused and a
/// stale handle must fail rather than resolve to whatever moved in.
using FeatureId = UnitId;

/// What used to be here.
struct Feature {
    /// Where it is. On the ground, sampled when the feature was made — a wreck does not move,
    /// so this is a fact rather than a cache.
    std::array<Fx, 3> at{};

    /// How much ground it covers, in elmos. Taken from the collision radius of whatever died,
    /// so a commander's wreck marks more ground than a tank's.
    Fx radiusElmos{};

    /// What died here, as a type index, and whose it was.
    ///
    /// The TYPE rather than the definition: the sim holds no definitions (`UnitCatalog`), and a
    /// caller that wants a model or a mass value looks it up the same way it does for a unit.
    UnitTypeIndex fromType = 0;
    int armyIndex = kNoArmy;

    /// Wreck durability and reclaim progress. Retail starts a finished wreck at materialized
    /// fraction 1 even when `HealthMult` gives it less than maximum health. Reclaim lowers the
    /// fraction and health together; ordinary damage lowers health and re-derives value.
    Mag health{};
    Mag maximumHealth{};

    /// The unscaled `SetMaxReclaimValues` baseline retained for damage recalculation.
    Mag maximumMassReclaim{};
    Mag maximumEnergyReclaim{};

    /// What reclaiming this still yields. Set at creation from the definition's wreck value
    /// (`UnitDef::wreckMass`, the blueprint's `BuildCost × MassMult`), drained per tick by
    /// the harvest pass, and the feature is removed when both reach zero. A wreck with
    /// nothing in it — an ACU's, a wall's — is a scorch record and nothing more.
    Mag massRemaining{};
    Mag energyRemaining{};

    /// Work is the larger current resource value. Advancing one shared work bar is what makes
    /// mixed mass/energy reclaim credit both resources by exactly the same fraction.
    Mag reclaimWorkRemaining{};
    Mag reclaimWorkTotal{};
    Fx reclaimFraction = kFxOne;
    Fx damageRatio = kFxOne;

    /// The value one point of a reclaimer's BuildRate recovers per second — 10 for every
    /// wreck in the corpus. Carried on the feature rather than looked up through
    /// `fromType` because a wreck outlives content changes and, later, map props will
    /// state their own (`Prop.lua:39-47`).
    Fx maximumReclaimPerBuildRate{};
    Fx reclaimPerBuildRate{};
};

/// Everything on the map that is not a unit.
///
/// SEPARATE STORAGE from `UnitStore`, which is what §6.6 asks for — "separate storage per kind"
/// — and not merely convenient: a feature has no orders, no reload and no motion, so
/// putting one in the unit arrays would mean six columns of nothing per wreck, and a wreck is
/// the most numerous object a long match produces.
///
/// CALLER-OWNED, handed to the tick through `Match`, like the projectile list. Null for a scene
/// that has nothing to leave behind.
class FeatureStore {
public:
    /// Adds a feature and returns its handle.
    [[nodiscard]] FeatureId add(const Feature& feature);

    /// Removes one — the reclaim that emptied it, exactly as the append-only note promised.
    /// The slot is reused by a later add; the released handle goes stale, so nothing holding
    /// one resolves to whoever moves in. Removing a stale handle is a no-op.
    void remove(FeatureId id);

    /// Every slot, LIVE OR NOT, exactly like `UnitStore`'s arrays — a caller walking this
    /// must check `slotAlive`. Kept because the renderer and the hash walk slots, and a
    /// span that compacted on remove would reorder both.
    [[nodiscard]] std::span<const Feature> all() const noexcept { return features_; }
    [[nodiscard]] std::size_t size() const noexcept { return features_.size(); }

    /// One feature, or null for a stale handle.
    [[nodiscard]] const Feature* find(FeatureId id) const noexcept;

    /// The same lookup, writable — for the harvest pass, which drains what it finds.
    [[nodiscard]] Feature* findMutable(FeatureId id) noexcept;

    /// Whether the slot currently holds a live feature. The form a slot-walking pass wants.
    [[nodiscard]] bool slotAlive(UnitIndex slot) const noexcept {
        return slot < generations_.size() && ids_.alive(FeatureId{slot, generations_[slot]});
    }

    /// The handle currently occupying a slot. Stale-safe, like `UnitStore::idAt`.
    [[nodiscard]] FeatureId idAt(UnitIndex slot) const noexcept {
        return slot < generations_.size() ? FeatureId{slot, generations_[slot]} : FeatureId{};
    }

    /// Bumped by every add and remove. What the wreck-decal rebuild watches: `size()`
    /// cannot tell "one added" from "one added, one removed", and a count that missed the
    /// second would draw the reclaimed wreck forever.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    void clear() noexcept;

private:
    /// Slots are stable and reused, never compacted — the same tombstone contract as
    /// `UnitStore`, for the same reasons: stable iteration order for the hash, and spans
    /// that survive a removal mid-pass.
    std::vector<Feature> features_;
    IdPool ids_;

    /// The generation in each slot, mirrored from the pool — same as `UnitStore`.
    std::vector<Generation> generations_;

    std::uint64_t revision_ = 0;
};

} // namespace rm::sim
