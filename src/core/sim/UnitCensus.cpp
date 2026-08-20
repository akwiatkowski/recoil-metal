#include "core/sim/UnitCensus.hpp"

#include "core/sim/Movement.hpp"

#include <algorithm>

namespace rm::sim {
namespace {

/// The half-open range of `keys_` matching every key in `[low, high]`.
struct Range {
    std::size_t begin = 0;
    std::size_t end = 0;
};

[[nodiscard]] Range rangeOf(const std::vector<UnitCensus::Key>& keys, UnitCensus::Key low,
                            UnitCensus::Key high) {
    const auto first = std::lower_bound(keys.begin(), keys.end(), low);
    const auto last = std::upper_bound(first, keys.end(), high);
    return Range{static_cast<std::size_t>(first - keys.begin()),
                 static_cast<std::size_t>(last - keys.begin())};
}

} // namespace

void UnitCensus::rebuild(const UnitStore& store) {
    units_.clear();
    keys_.clear();

    // One pass to collect, then one sort. Reserving the live count avoids the regrowth that
    // would otherwise happen mid-battle, when this runs every tick.
    std::vector<std::pair<Key, UnitId>> entries;
    entries.reserve(store.liveCount());

    const std::span<const MoveState> motion = store.motion();
    for (UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
        if (!store.slotAlive(slot)) {
            continue;
        }
        const int army = motion[slot].armyIndex;
        if (army < 0) {
            // `kNoArmy`. A decorative crowd owns nothing and is owned by nobody; bucketing
            // it under a fabricated team would put it in somebody's unit census.
            continue;
        }
        entries.emplace_back(key(static_cast<TeamIndex>(army), store.typeAt(slot)),
                             store.idAt(slot));
    }

    // Sorted by (key, slot). The slot tiebreak is what makes the order total, and therefore
    // the rebuild reproducible — see the header.
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second.index < b.second.index;
    });

    units_.reserve(entries.size());
    keys_.reserve(entries.size());
    for (const auto& [k, id] : entries) {
        keys_.push_back(k);
        units_.push_back(id);
    }
}

std::span<const UnitId> UnitCensus::of(TeamIndex team, UnitTypeIndex type) const {
    const Key k = key(team, type);
    const Range r = rangeOf(keys_, k, k);
    return std::span<const UnitId>{units_}.subspan(r.begin, r.end - r.begin);
}

std::span<const UnitId> UnitCensus::ofTeam(TeamIndex team) const {
    // Every type of one team is one contiguous run, because the key holds the team in its
    // high half — so this is the same binary search over a wider bound rather than a
    // separate structure.
    const Range r = rangeOf(keys_, key(team, 0),
                            key(team, static_cast<UnitTypeIndex>(0xFFFF)));
    return std::span<const UnitId>{units_}.subspan(r.begin, r.end - r.begin);
}

} // namespace rm::sim
