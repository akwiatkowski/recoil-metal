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
///     the order started) → load whatever was ORDERED to the beacon → drop
///     point → back. The pickup is an assignment, not a zone (`C-199`): a
///     unit merely parked in the ring is left alone. It never retires;
///     `Stop` ends it.
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
/// `kFerryPickupRadius`: 10 elmos, the sim's long-standing pickup-ring radius
/// (predates this slice; no authored source — `C-199` names no distance).
inline constexpr Fx kFerryPickupRadius = Fx::fromInt(10);

/// The ferry waiting ring (`C-199`): `TransportGenerateWaitingFormationForUnits`
/// (`0x005eca10`) builds a formation anchored at the transport, using the
/// carrier blueprint's `GuardFormationName` as the script. No shipped
/// blueprint states the key, so the engine default — retail's Guard ring
/// (`lua/formations.lua: GuardFormation`, `sizeMult = 3` elmos, first five
/// slots per ring, naval rings at 8) — is the layout: staging spot `i`
/// (zero-based among the carrier's current waiters) sits at radius `3 + 3 ×
/// (i / 5)` elmos around the CARRIER, at bearing `i / current-ring-size`
/// of a full turn. A waiter is a live, unattached, transportable same-army
/// unit holding an order into the beacon's ring, in slot order — the same
/// scan the Loading phase boards from.
[[nodiscard]] std::array<Fx, 2> ferryStagingSpot(const UnitStore& store,
                                                 const UnitCatalog& catalog,
                                                 UnitId carrier,
                                                 UnitIndex waiter,
                                                 std::array<Fx, 2> beacon) noexcept;

/// Whether `carrier` could ever take `cargo`: the class fits the capacity and
/// the attach table at all — regardless of what is already aboard.
[[nodiscard]] bool canEverCarry(const unitdef::UnitDef& carrier,
                                const unitdef::UnitDef& cargo) noexcept;

/// Whether `carrier` has room for `cargo` NOW: the class fits and the children
/// already aboard leave enough slots. Iterates `childrenOf` in slot order.
[[nodiscard]] bool hasRoomFor(const UnitStore& store, const UnitCatalog& catalog,
                              UnitId carrier, const unitdef::UnitDef& cargo) noexcept;

/// Slings `cargo` under `carrier` and attaches it. When the catalog carries
/// the carrier's `Attachpoint*` bones (`C-198`), every class prices against
/// the class-1 list — `ClassNAttachSize` class-1 points — and the cargo hangs
/// from the first free point in origin-distance order, so the fill is a
/// deterministic prefix of the list. A carrier with no resolved bones keeps
/// the deterministic sling row.
/// Caller checks room first (`hasRoomFor`).
[[nodiscard]] bool attachCargo(UnitStore& store, const UnitCatalog& catalog,
                               const unitdef::UnitDef& carrier,
                               UnitId carrierId, UnitId cargo) noexcept;
/// Parks `aircraft` ON the staging pad's nearest free `Attachpoint` bone
/// (`C-456`): the bone's rest offset from the pad origin is the parking spot —
/// its X/Z the deck position, its Y the landing-site height — and the real
/// bone index rides the attachment record. False when the pad resolves no
/// bones (caller keeps the berth-grid fallback) or none is free.
[[nodiscard]] bool dockOnStagingBone(UnitStore& store, const UnitCatalog& catalog,
                                    UnitId pad, UnitId aircraft) noexcept;

/// Sets every child down around the carrier — in deterministic ring order on
/// the deck for transports, or airborne at the carrier's position for a
/// `CARRIER`'s internal storage launch (`C-225`). Their queues keep whatever
/// follows the boarding order.
void detachCargo(UnitStore& store, const UnitCatalog& catalog,
                 const Terrain& terrain, UnitId carrier) noexcept;

} // namespace sim
} // namespace rm
