#pragma once

#include "core/Types.hpp"
#include "core/sim/Command.hpp"

#include <span>

namespace rm {
class Terrain;
namespace sim {

class UnitCatalog;
class UnitStore;
struct PassabilityGrid;

/// The per-tick transport pass: load, unload, and ferry execution.
///
/// THREE ORDER KINDS live here, because none of them completes by arrival —
/// they complete on attachment or touchdown, which only this pass can see:
///
///   - `LoadTransport` rides the CARGO's queue: the unit walks to its carrier,
///     and while it waits an idle airborne carrier flies to the unit and comes
///     down. The order retires when `attach` lands or when the carrier can
///     never take the class; a full carrier leaves the order waiting.
///   - `UnloadTransport` rides the TRANSPORT's queue: fly to the point, come
///     down, set every child on the ground. It retires when the hold empties.
///   - `Ferry` rides the transport's queue as a standing loop: beacon (where
///     the order started) → load whatever was ordered to the beacon → drop
///     point → back. It never retires; `Stop` ends it.
///
/// Cargo rides on the generic attachment machinery (`C-195`/`C-196`): attached
/// units do not move, do not collide, do not shoot, and are not shot at —
/// retail's `MarkWeaponsOnTransport`/`SetCanTakeDamage(false)` pair, structurally.
/// Carrier death detaches children where it dies (`UnitStore::kill`); fall
/// damage is deferred.
void updateTransports(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                      std::span<const PassabilityGrid* const> gridForType);

/// A `Move` the grid refused: offer the unit a lift instead (#15800).
///
/// Picks the nearest IDLE same-army carrier with room for the class, then
/// rewrites both queues with ordinary orders: the unit's becomes
/// `[LoadTransport, the original move]` — the click survives so a dead carrier
/// still leaves the intent standing — and the carrier's becomes
/// `[UnloadTransport at the click, Move back to where it waited]`. The pickup
/// itself rides the existing loading/unload drives; while the hold is empty
/// and a loader is inbound, `advanceUnload` holds the carrier at the pickup.
///
/// False when no carrier is free — the caller refuses or retires the move
/// exactly as before.
[[nodiscard]] bool offerAutoEmbark(UnitStore& store, const UnitCatalog& catalog,
                                   UnitIndex slot, const Command& move) noexcept;

/// How far a unit counts as "sent to the ferry" from the beacon, in elmos —
/// an order destination inside the ring marks the unit as waiting to board.
inline constexpr Fx kFerryPickupRadius = Fx::fromInt(10);

/// Whether `carrier` could ever take `cargo`: the class fits the capacity and
/// the attach table at all — regardless of what is already aboard.
[[nodiscard]] bool canEverCarry(const unitdef::UnitDef& carrier,
                                const unitdef::UnitDef& cargo) noexcept;

/// Whether `carrier` has room for `cargo` NOW: the class fits and the children
/// already aboard leave enough slots. Iterates `childrenOf` in slot order.
[[nodiscard]] bool hasRoomFor(const UnitStore& store, const UnitCatalog& catalog,
                              UnitId carrier, const unitdef::UnitDef& cargo) noexcept;

/// Slings `cargo` under `carrier` and attaches it. When the catalog carries the
/// carrier's `Attachpoint*` bones (`C-198`), the cargo is placed at the nearest
/// free bone of its own class — class-1 bones when its class has none free —
/// and rides it through `UnitStore::AttachBones`, so it swings with the hull.
/// A carrier with no resolved bones keeps the deterministic sling row.
/// Caller checks room first (`hasRoomFor`).
[[nodiscard]] bool attachCargo(UnitStore& store, const UnitCatalog& catalog,
                               const unitdef::UnitDef& carrier,
                               UnitId carrierId, UnitId cargo) noexcept;

/// Sets every child down on the deck around the carrier, in deterministic
/// ring order. Their queues keep whatever follows the boarding order.
void detachCargo(UnitStore& store, const Terrain& terrain, UnitId carrier) noexcept;

} // namespace sim
} // namespace rm
