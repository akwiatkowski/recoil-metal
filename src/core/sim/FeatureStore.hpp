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
// today — nothing shoots a wreck, nothing collides with one, no blast damages one. Building the
// shared id space before anything asks a question through it would be a refactor of every
// `UnitId` in the sim in exchange for nothing observable. `FeatureId` is its own handle from
// the same `IdPool`, so the merge later is a rename plus a tag rather than a redesign.
//
// WHAT A FEATURE IS NOT, yet: reclaimable, targetable, or an obstacle. It is a record and a
// thing to draw. Reclaim is what §6.6 says depends on this, and it depends on the economy
// reading features, not on the features being any different.
//
// NOT IN THE STATE HASH, deliberately, and the reasoning is worth keeping because the default
// answer for sim state is the opposite. A feature is derived one-for-one from a death, and
// deaths ARE hashed — the unit's liveness, its last position and its killer all are. So two runs
// that produced different wrecks have already diverged somewhere the hash can see, and feeding
// the features would add a second reading of the same fact. That stops being true the moment
// anything reads a feature back into a rule (reclaim, an obstacle, a targetable hulk), and it
// should be hashed on the same commit that does.

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
};

/// Everything on the map that is not a unit.
///
/// SEPARATE STORAGE from `UnitStore`, which is what §6.6 asks for — "separate storage per kind"
/// — and not merely convenient: a feature has no health, no orders, no reload and no motion, so
/// putting one in the unit arrays would mean six columns of nothing per wreck, and a wreck is
/// the most numerous object a long match produces.
///
/// CALLER-OWNED, handed to the tick through `Match`, like the projectile list. Null for a scene
/// that has nothing to leave behind.
class FeatureStore {
public:
    /// Adds a feature and returns its handle.
    [[nodiscard]] FeatureId add(const Feature& feature);

    [[nodiscard]] std::span<const Feature> all() const noexcept { return features_; }
    [[nodiscard]] std::size_t size() const noexcept { return features_.size(); }

    /// One feature, or null for a stale handle.
    [[nodiscard]] const Feature* find(FeatureId id) const noexcept;

    void clear() noexcept;

private:
    /// APPEND-ONLY, and that is a decision rather than an omission. Nothing removes a feature:
    /// a wreck is permanent, because a wreck IS the record of what happened here and a
    /// battlefield that tidied itself up would lose it. When reclaim arrives it will need
    /// removal, and the handle is generational so that day does not invalidate anything.
    std::vector<Feature> features_;
    IdPool ids_;
};

} // namespace rm::sim
