#pragma once

#include "core/Types.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rm::sim {

// Who owns what, and how many of it — answered without walking the store.
//
// Named a census rather than an index because `rm::UnitIndex` is already the width alias in
// core/Types.hpp, and because a census is what it answers: how many of what, per side.
//
// WHY THIS EXISTS. Three separate things in this engine ask "how many X does army Y have",
// and all three currently answer it by scanning every unit in the match, every tick:
//
//   the win condition        `countCommanders` walks every batch and every instance to
//                            count living commanders (main.mm)
//   the scripted opponent    `standingFor` builds an `ArmyView` the same way, once a second
//   the HUD                  the same counts again, for the readout
//
// Recoil solved this twenty years ago with `unitsByDefs[team][defID]`
// (Sim/Units/UnitHandler.h) and the win is not subtle: an O(1) lookup instead of an O(units)
// scan, three times per tick.
//
// HOW THIS DIFFERS FROM RECOIL (PLAN2.md §6.2). Theirs is
// `vector<vector<vector<CUnit*>>>` — a MAX_TEAMS × def-count grid of vectors holding raw
// pointers. That is a large sparse allocation, most of it empty, and the pointers mean every
// death needs bookkeeping in the index or it dangles.
//
// Ours is one flat vector of handles sorted on a packed (team, type) key, plus a small range
// table. One allocation, contiguous iteration, no dangling — a handle that outlives its unit
// fails to resolve instead of naming a stranger — and a deterministic order for free, which
// matters because the replay hash is the success criterion.
//
// REBUILT, NOT MAINTAINED. Incremental upkeep on every spawn and death is more code, more
// invariants, and a new way for the index to disagree with the store. A rebuild is a sort
// over live units, which at the scales this engine targets is cheaper than the scans it
// replaces — and it cannot drift, because it is derived rather than kept.
class UnitCensus {
public:
    /// A packed (team, type) lookup key. Team in the high half, type in the low, so that
    /// sorting by the key groups a team's units together and orders types within it — which
    /// is what makes "everything army 3 owns" a contiguous range as well.
    using Key = std::uint32_t;

    [[nodiscard]] static constexpr Key key(TeamIndex team, UnitTypeIndex type) noexcept {
        return (static_cast<Key>(team) << 16) | static_cast<Key>(type);
    }

    /// Rebuilds from the store. Only live units are indexed; a unit with no army
    /// (`kNoArmy` — a decorative crowd, which owns nothing and is owned by nobody) is
    /// skipped rather than bucketed under a made-up team.
    void rebuild(const UnitStore& store);

    /// Every live unit of one type belonging to one team, in slot order.
    [[nodiscard]] std::span<const UnitId> of(TeamIndex team, UnitTypeIndex type) const;

    /// How many. The question that used to cost a scan.
    [[nodiscard]] std::size_t count(TeamIndex team, UnitTypeIndex type) const {
        return of(team, type).size();
    }

    /// Everything one team owns, whatever its type — contiguous because the key puts team
    /// in the high half.
    [[nodiscard]] std::span<const UnitId> ofTeam(TeamIndex team) const;

    [[nodiscard]] std::size_t countOfTeam(TeamIndex team) const { return ofTeam(team).size(); }

    /// Live units indexed, across all teams. Excludes the army-less, so this is not
    /// necessarily `store.liveCount()`.
    [[nodiscard]] std::size_t size() const noexcept { return units_.size(); }

private:
    /// Live handles, sorted by (key, slot). Slot breaks ties so the order is total and the
    /// rebuild is reproducible — without it two units of the same team and type could come
    /// back in either order and the index would stop being deterministic.
    std::vector<UnitId> units_;

    /// The key each entry in `units_` was sorted under, parallel to it. Kept rather than
    /// recomputed because recomputing needs the store, and a lookup should not.
    std::vector<Key> keys_;
};

} // namespace rm::sim
