#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/TickRate.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstddef>
#include <vector>

namespace rm::sim {

// What each unit TYPE is, looked up by the index a unit carries.
//
// WHY THIS EXISTS. It is the one piece that makes the batch grouping unnecessary. Until now
// a unit's definition came from its BATCH — `SkirmishGroup::def`, one def shared by every
// unit in one instanced draw — which is why every sim pass took a span of groups and looped
// `for group / for instance`. Move the def behind a per-unit type index and the groups have
// nothing left to be: the passes take the store and loop once (PLAN2.md §7 P1.4).
//
// NON-OWNING, deliberately. The definitions are loaded from the VFS and owned by whoever
// loaded them — a deque in the app, a plain object in a test. The catalog is a lookup table,
// so a test can build one from two stack `UnitDef`s and the sim never learns what a VFS is.
// The pointers must outlive the catalog, which is the same contract `SkirmishGroup::def`
// already had.
//
// Type indices are handed out in first-seen order, which is the order batches were created
// in — so the catalog's numbering mirrors the layout it replaces. That is not required by
// anything here; it is recorded because it makes the two comparable while both exist.
class UnitCatalog {
public:
    /// What a type contributes per TICK, derived once when the type is registered.
    ///
    /// WHY DERIVED HERE (PLAN2.md §5.1). A blueprint authors rates per SECOND, which is a fact
    /// about the unit; how much that is per tick depends on the clock, which is a fact about
    /// the sim. The conversion must happen exactly once, and "when the catalog learns about
    /// the type" is the only moment that is both after the rate is known and before any tick
    /// runs. Doing it inside the income pass instead would put a float divide in a loop that
    /// runs over every unit every tick — and, worse, would leave the per-second value where a
    /// later reader could use it directly.
    ///
    /// `Mag` throughout: `BuildCostEnergy` reaches 10,008,000 in the corpus, and a rate summed
    /// over a hundred producers needs the same headroom as the total it feeds.
    struct Rates {
        Mag massPerTick{};
        Mag energyPerTick{};
        Mag upkeepEnergyPerTick{};
        Mag buildPerTick{};
    };
    /// Registers a definition and returns the index units of that type will carry.
    ///
    /// Null is allowed and gets an index like anything else: a decorative crowd has no
    /// definition, earns nothing and fires nothing, and "no def" has to be representable
    /// rather than a reason to reject the unit.
    /// The rate is defaulted so the many callers that want the ordinary clock need not say
    /// so, and is taken by value because a `TickRate` is two words.
    [[nodiscard]] UnitTypeIndex add(const unitdef::UnitDef* def, TickRate rate = TickRate{});

    /// What a type contributes per tick. Zeroes for an unregistered index or a type with no
    /// definition, which is what a decorative crowd should earn.
    [[nodiscard]] const Rates& rates(UnitTypeIndex type) const noexcept {
        static constexpr Rates kNone{};
        return type < rates_.size() ? rates_[type] : kNone;
    }

    /// The definition for a type, or null — for an unregistered index as well as for a type
    /// registered without one. A pass that reads this must handle null either way, so
    /// bounds-checking to the same answer costs nothing and removes a crash.
    [[nodiscard]] const unitdef::UnitDef* def(UnitTypeIndex type) const noexcept {
        return type < defs_.size() ? defs_[type] : nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return defs_.size(); }

private:
    std::vector<const unitdef::UnitDef*> defs_;

    /// Parallel to `defs_`, index-locked by construction: both only ever grow by one, in
    /// `add`.
    std::vector<Rates> rates_;
};

} // namespace rm::sim
