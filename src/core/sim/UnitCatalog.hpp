#pragma once

#include "core/Types.hpp"
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
    /// Registers a definition and returns the index units of that type will carry.
    ///
    /// Null is allowed and gets an index like anything else: a decorative crowd has no
    /// definition, earns nothing and fires nothing, and "no def" has to be representable
    /// rather than a reason to reject the unit.
    [[nodiscard]] UnitTypeIndex add(const unitdef::UnitDef* def);

    /// The definition for a type, or null — for an unregistered index as well as for a type
    /// registered without one. A pass that reads this must handle null either way, so
    /// bounds-checking to the same answer costs nothing and removes a crash.
    [[nodiscard]] const unitdef::UnitDef* def(UnitTypeIndex type) const noexcept {
        return type < defs_.size() ? defs_[type] : nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return defs_.size(); }

private:
    std::vector<const unitdef::UnitDef*> defs_;
};

} // namespace rm::sim
