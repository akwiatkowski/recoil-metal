#include "core/sim/SpatialGrid.hpp"

#include "core/sim/UnitStore.hpp"

#include <algorithm>

namespace rm::sim {
namespace {

/// A cell coordinate pair packed into one sortable key.
///
/// IT ONLY HAS TO BE A CONSISTENT BIJECTION, not an order-preserving one — and that is worth
/// stating because the first version of this function biased each half by `0x80000000` to make
/// a cell at x = -1 sort below one at x = 0. That looked necessary and is not: the array is
/// sorted by this key and every lookup searches by the same key, so any total order groups each
/// cell's entries contiguously, which is all `lower_bound` needs. Nothing reads the array in
/// spatial order.
///
/// Found by deleting the bias and watching the whole suite stay green — including the case
/// written specifically to catch it. Removed rather than kept with a corrected comment, because
/// an operation nothing depends on is an operation a later reader has to reason about.
[[nodiscard]] std::uint64_t packCell(std::int32_t cx, std::int32_t cz) noexcept {
    const auto ux = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx));
    const auto uz = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
    return (ux << 32) | uz;
}

} // namespace

void SpatialGrid::rebuild(const UnitStore& store, Fx cellSize) {
    cellSize_ = std::max(cellSize, kMinCellSize);

    entries_.clear();
    entries_.reserve(store.slotCount());

    const std::span<const Transform> transforms = store.transforms();
    for (UnitIndex slot = 0; slot < transforms.size(); ++slot) {
        const Fx x = transforms[slot].x;
        const Fx z = transforms[slot].z;
        entries_.push_back(Entry{
            .cell = packCell((x / cellSize_).floorToInt(), (z / cellSize_).floorToInt()),
            .slot = slot,
            .x = x,
            .z = z,
        });
    }

    // By cell, then by slot. The second key is what makes the sort a total order rather than
    // merely a grouping — two units in one cell must come out in a defined sequence, or a scan
    // over the array is not reproducible.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.cell != b.cell ? a.cell < b.cell : a.slot < b.slot;
    });
}

void SpatialGrid::gather(Fx x, Fx z, Fx radius, bool exact) {
    result_.clear();
    if (entries_.empty()) {
        return;
    }

    const Fx reach = std::max(radius, Fx{});
    const std::int32_t x0 = ((x - reach) / cellSize_).floorToInt();
    const std::int32_t x1 = ((x + reach) / cellSize_).floorToInt();
    const std::int32_t z0 = ((z - reach) / cellSize_).floorToInt();
    const std::int32_t z1 = ((z + reach) / cellSize_).floorToInt();

    // The distance test happens HERE, with the entry in hand, rather than as a pass over the
    // slot numbers afterwards: the entry carries its own position, so filtering costs nothing
    // extra, and a later pass would have to find each slot's position again.
    const auto take = [&](const Entry& entry) {
        // On the GROUND — the same measure `groundDistanceElmos` uses, so a range is a
        // footprint on the map and a unit on a cliff is no harder to shoot than one on the flat.
        if (!exact || fxHypot(entry.x - x, entry.z - z) <= reach) {
            result_.push_back(entry.slot);
        }
    };

    // WIDTHS AS 64-BIT, because a huge radius on a fine grid overflows a 32-bit span count and
    // an overflowed count compares small — which would send the cell path off to walk four
    // billion cells rather than falling through to the scan below.
    const auto cellsWide = static_cast<std::int64_t>(x1) - x0 + 1;
    const auto cellsDeep = static_cast<std::int64_t>(z1) - z0 + 1;

    // NEVER WORSE THAN BRUTE FORCE. Walking the cell range costs a binary search per cell, so
    // once there are more cells than units the whole-array scan is cheaper — and this is what
    // stops the cell size from being a number that has to be right. A 2,048-elmo weapon range
    // on a 32-elmo grid is 4,096 cells whatever is in them; a match with 200 units scans 200
    // entries instead, and gets the same answer in the same order.
    if (cellsWide * cellsDeep >= static_cast<std::int64_t>(entries_.size())) {
        for (const Entry& entry : entries_) {
            take(entry);
        }
    } else {
        for (std::int32_t cz = z0; cz <= z1; ++cz) {
            for (std::int32_t cx = x0; cx <= x1; ++cx) {
                const std::uint64_t key = packCell(cx, cz);
                const auto begin = std::lower_bound(
                    entries_.begin(), entries_.end(), key,
                    [](const Entry& e, std::uint64_t k) { return e.cell < k; });
                for (auto it = begin; it != entries_.end() && it->cell == key; ++it) {
                    take(*it);
                }
            }
        }
    }

    // ASCENDING SLOT ORDER, which is part of the contract: it makes the answer identical to the
    // brute-force scan this replaces, including its order, so every tie-break that relied on
    // meeting the lowest slot first still does — and it makes the two paths above
    // indistinguishable to a caller.
    std::sort(result_.begin(), result_.end());
}

std::span<const UnitIndex> SpatialGrid::within(Fx x, Fx z, Fx radius) {
    gather(x, z, radius, /*exact=*/true);
    return result_;
}

std::span<const UnitIndex> SpatialGrid::candidates(Fx x, Fx z, Fx radius) {
    gather(x, z, radius, /*exact=*/false);
    return result_;
}

} // namespace rm::sim
