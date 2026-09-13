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

/// Slings `cargo` under `carrier` at the next deterministic slot offset and
/// attaches it. Caller checks room first (`hasRoomFor`). The attach offset is
/// world-axis — a boneless child keeps its station under the hull rather than
/// swinging with the carrier's heading.
[[nodiscard]] bool attachCargo(UnitStore& store, const unitdef::UnitDef& carrier,
                               UnitId carrierId, UnitId cargo) noexcept;

/// Sets every child down on the deck around the carrier, in deterministic
/// ring order. Their queues keep whatever follows the boarding order.
void detachCargo(UnitStore& store, const Terrain& terrain, UnitId carrier) noexcept;

} // namespace sim
} // namespace rm
