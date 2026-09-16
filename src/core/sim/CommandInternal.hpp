#pragma once

// The command family's file-local furniture, shared by its five translation units.
//
// WHY THIS EXISTS. `Command.cpp` was 3,610 lines, and splitting it into the pipeline stages
// it already described — `CommandBuffer` (intake), `CommandApply` (authority), `CommandAdvance`
// (the queue's next step), `CommandLog` (the record) — left these helpers with no single home.
// They were in anonymous namespaces, which is exactly right for one file and unavailable
// across five. Each is DEFINED in `Command.cpp`, the vocabulary file: a predicate or a helper
// no stage owns.
//
// NOT PART OF THE PUBLIC HEADER, deliberately. `Command.hpp` is what the app and the sim's
// other systems compile against; everything here is plumbing between the four stage files.
// The `Internal` in the name is the contract: nothing outside `src/core/sim/` may include this.

#include "core/sim/Command.hpp"

namespace rm::sim {

struct MoveState;

/// Sorts a recipient set by the complete generational handle and drops duplicates — the one
/// canonical order every intake path (buffer, apply, log record) agrees on.
void canonicalizeUnits(std::vector<UnitId>& units);

/// Whether the cancellation link is coherent: `CancelFactoryBuild` iff a command id is named.
[[nodiscard]] bool validCancellation(const CommandIssue& issue) noexcept;

/// Assist/guard target predicates, shared because the same "is this a legal target" answer is
/// asked at apply time AND again when the queue head reaches the order (`startCommand`).
[[nodiscard]] bool validAssist(const Command& command, const UnitStore& store,
                               const UnitCatalog& catalog) noexcept;
[[nodiscard]] bool validGuard(const Command& command, const UnitStore& store,
                              const UnitCatalog& catalog) noexcept;

/// Whether a repair target is still an ally of the builder — checked when the order starts
/// and re-checked while the queue watches it, in different stages.
[[nodiscard]] bool repairStillAllied(UnitIndex builder, UnitId target, const UnitStore& store,
                                     std::span<const Army> armies) noexcept;

/// Guard-domain answers the assistance predicate and the advancing queue both ask.
[[nodiscard]] std::optional<std::array<Fx, 3>> guardReturnPosition(
    UnitIndex slot, UnitId guardee, const UnitStore& store, const UnitCatalog& catalog,
    std::span<const Construction> building);
[[nodiscard]] std::optional<UnitId> guardAttackTarget(
    UnitIndex slot, const UnitStore& store, const UnitCatalog& catalog,
    std::span<const Army> armies, const Intel* intel, const PlayableRect* playableRect,
    TickIndex tick = 0, TickRate rate = TickRate{});

/// The unfinished construction row a builder owns, if any — asked when a cancellation decides
/// whether work stops (`CommandApply`) and when the queue advances past the order
/// (`CommandAdvance`).
[[nodiscard]] Construction* activeConstruction(std::vector<Construction>& building,
                                               UnitId builder) noexcept;

/// Whether a refused Build was refused by an OCCUPIED SITE — an unfinished row of the same
/// blueprint stands where it targets and belongs to this builder's side. Such an order still
/// means "build there" (resume an abandoned scaffold, lend to a colleague's), so intake and
/// head dispatch must admit it rather than drop it; the dispatch stage then decides which of
/// lend or takeover applies.
[[nodiscard]] bool joinableConstructionAt(std::vector<Construction>& building,
                                          const Command& command, const UnitStore& store,
                                          const UnitCatalog& catalog,
                                          std::span<const Army> armies) noexcept;

/// Drops a unit to stillness: clears the motion flags and the stored path in one place, so no
/// stage that cancels or supersedes an order can forget half of it.
void teardownMovement(MoveState& motion);

/// Starts a queue's head order for a unit the sim has already found live.
///
/// Defined in `CommandAdvance.cpp` — it IS the advancement machinery — but `applyCommandMember`
/// calls it for unqueued orders that start the moment they are issued, which is why it cannot
/// stay file-local. By SLOT, not by handle: see the definition's comment.
[[nodiscard]] bool startCommand(const Command& command, UnitStore& store,
                                const UnitCatalog& catalog, const Terrain& terrain,
                                const PassabilityGrid& grid, TickRate rate,
                                std::vector<Construction>* building, EventQueue* events,
                                const FeatureStore* features,
                                const PassabilityGrid* approachGrid = nullptr);

} // namespace rm::sim
