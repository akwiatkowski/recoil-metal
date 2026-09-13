#include "core/sim/Transport.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/CommandQueue.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Terrain.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/UnitStore.hpp"
#include "core/unit/UnitDef.hpp"

#include <algorithm>

namespace rm::sim {

namespace {

using AirState = MoveState::AirState;

/// Centre-to-centre reach for the attach: both radii plus a small pad, so a
/// transport need not be parked exactly on its cargo for the sling to take.
[[nodiscard]] Fx loadReach(const MoveState& carrier, const MoveState& cargo) noexcept {
    return carrier.radiusElmos + cargo.radiusElmos + Fx::fromInt(2);
}

/// Whether the unit is down where cargo can reach it — on the deck for a
/// flyer, trivially true for anything else (no ground carrier ships yet, but
/// the flag is the question being asked).
[[nodiscard]] bool grounded(const MoveState& motion) noexcept {
    return !motion.canFly || motion.airState == AirState::Bottom;
}

/// Slots already spoken for: the attach cost of every child aboard.
[[nodiscard]] int slotsUsed(const UnitStore& store, const UnitCatalog& catalog,
                            const unitdef::UnitDef& carrier, UnitId carrierId) noexcept {
    int used = 0;
    for (const UnitId child : store.childrenOf(carrierId)) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(child.index));
        used += def != nullptr ? carrier.transportAttachCost(def->transportCargoClass()) : 0;
    }
    return used;
}

/// Sends the unit toward a point on its own movement domain: direct steering
/// for a flyer, the type's grid otherwise. False when no route exists.
[[nodiscard]] bool routeToward(UnitIndex slot, Fx x, Fx z, UnitStore& store,
                               const Terrain& terrain,
                               std::span<const PassabilityGrid* const> gridForType) {
    MoveState& motion = store.motion()[slot];
    if (motion.canFly) {
        orderTo(motion, terrain, x, z);
        return true;
    }
    const auto type = static_cast<std::size_t>(store.typeAt(slot));
    if (type >= gridForType.size() || gridForType[type] == nullptr) {
        return false;
    }
    const Transform& at = store.transforms()[slot];
    const std::vector<std::array<Fx, 2>> path =
        findPath(*gridForType[type], at.x, at.z, x, z);
    if (path.empty()) {
        return false;
    }
    orderAlongPath(motion, path);
    return true;
}

/// Starts the next leg if the unit is in a state that takes a fresh commit:
/// standing on the deck or loitering at cruise. Anything mid-transition —
/// climbing, descending, already routed — is left to finish it first.
[[nodiscard]] bool commitLeg(UnitIndex slot, Fx x, Fx z, UnitStore& store,
                             const Terrain& terrain,
                             std::span<const PassabilityGrid* const> gridForType) {
    const MoveState& motion = store.motion()[slot];
    if (motion.moving || motion.airState == AirState::Up
        || motion.airState == AirState::Down) {
        return true;  // en route or mid-transition: the leg is already committed
    }
    return routeToward(slot, x, z, store, terrain, gridForType);
}

/// Whether a unit is still on its way to a ferry beacon: transportable, not
/// already aboard, and holding an order whose destination is inside the pickup
/// ring. An order DESTINATION, not the unit's position — a unit that already
/// arrived is standing in the ring and gets picked up on the position rule.
[[nodiscard]] bool inboundToBeacon(const UnitStore& store, const UnitCatalog& catalog,
                                   UnitIndex slot, std::array<Fx, 2> beacon) noexcept {
    if (!store.slotAlive(slot) || store.motion()[slot].attached) {
        return false;
    }
    const unitdef::UnitDef* def = catalog.def(store.typeAt(slot));
    if (def == nullptr || !def->transportable()) {
        return false;
    }
    const CommandQueue& queue = store.orders()[slot];
    const QueuedCommand* head = queue.current();
    if (head == nullptr) {
        return false;
    }
    const Fx dx = head->targetX() - beacon[0];
    const Fx dz = head->targetZ() - beacon[1];
    return dx * dx + dz * dz <= kFerryPickupRadius * kFerryPickupRadius;
}

} // namespace

bool canEverCarry(const unitdef::UnitDef& carrier, const unitdef::UnitDef& cargo) noexcept {
    const int cost = carrier.transportAttachCost(cargo.transportCargoClass());
    return cost > 0 && cost <= carrier.transportCapacity();
}

bool hasRoomFor(const UnitStore& store, const UnitCatalog& catalog, UnitId carrier,
                const unitdef::UnitDef& cargo) noexcept {
    const unitdef::UnitDef* carrierDef = catalog.def(store.typeAt(carrier.index));
    if (carrierDef == nullptr || !canEverCarry(*carrierDef, cargo)) {
        return false;
    }
    const int used = slotsUsed(store, catalog, *carrierDef, carrier);
    return used + carrierDef->transportAttachCost(cargo.transportCargoClass())
           <= carrierDef->transportCapacity();
}

bool attachCargo(UnitStore& store, const unitdef::UnitDef& /*carrier*/, UnitId carrierId,
                 UnitId cargo) noexcept {
    const std::span<Transform> transforms = store.transforms();
    const std::span<const MoveState> motion = store.motion();
    if (!store.alive(carrierId) || !store.alive(cargo) || store.parentOf(cargo)) {
        return false;
    }
    if (!grounded(motion[carrierId.index])) {
        return false;  // the deck is where a unit steps aboard
    }
    const Fx dx = transforms[cargo.index].x - transforms[carrierId.index].x;
    const Fx dz = transforms[cargo.index].z - transforms[carrierId.index].z;
    if (dx * dx + dz * dz
        > loadReach(motion[carrierId.index], motion[cargo.index])
              * loadReach(motion[carrierId.index], motion[cargo.index])) {
        return false;
    }

    // The slot pattern: cargo slings in a row under the hull, 3 elmos apart,
    // first outboard then filling toward the keel. The offset is captured by
    // `attach` from where the child stands, so the child is PLACED first.
    const std::size_t slot = store.childrenOf(carrierId).size();
    constexpr int kStep = 3;
    Transform& at = transforms[cargo.index];
    at.x = transforms[carrierId.index].x + Fx::fromInt(static_cast<int>(slot % 3) - 1) * kStep;
    at.z = transforms[carrierId.index].z + Fx::fromInt(static_cast<int>(slot / 3)) * kStep;
    // Height is captured as the carrier-relative offset: whatever the cargo
    // stood on, it keeps that clearance under the hull — a ground unit slung
    // aboard an airborne carrier hangs at its elevation below the keel.
    at.y = transforms[carrierId.index].y + Fx::fromInt(-2);
    return store.attach(carrierId, cargo);
}

void detachCargo(UnitStore& store, const Terrain& terrain, UnitId carrier) noexcept {
    const std::vector<UnitId> children = store.childrenOf(carrier);
    if (children.empty()) {
        return;
    }
    const Transform& at = store.transforms()[carrier.index];
    const Fx spread = store.motion()[carrier.index].radiusElmos + Fx::fromInt(2);
    // A deterministic ring: evenly spaced headings, each child the same reach
    // out from the keel. Fixed-point trig is not needed — the pattern is a
    // rotation through six directions chosen to spread a hold of any size.
    std::size_t i = 0;
    for (const UnitId child : children) {
        if (!store.detach(child)) {
            continue;
        }
        Transform& place = store.transforms()[child.index];
        const int ring = static_cast<int>(i / 6);
        const int step = static_cast<int>(i % 6);
        // Six directions on the hex lattice, one ring further out each wrap.
        static constexpr int kDx[6] = {1, 0, -1, -1, 0, 1};
        static constexpr int kDz[6] = {0, 1, 1, 0, -1, -1};
        place.x = at.x + spread * Fx::fromInt(kDx[step] * (ring + 1));
        place.z = at.z + spread * Fx::fromInt(kDz[step] * (ring + 1));
        place.y = terrain.heightAt(place.x, place.z);
        place.pitch = 0;
        place.roll = 0;
        MoveState& dropped = store.motion()[child.index];
        dropped.moving = false;
        dropped.path.clear();
        dropped.pathIndex = 0;
        ++i;
    }
}

namespace {

/// The cargo-side drive: a live `LoadTransport` walks its unit to the carrier
/// and, while the unit waits, calls an idle airborne carrier down to it.
/// Finishes the order on attach, on a dead or unfit carrier, and on an
/// unreachable route; leaves it waiting on a full one.
void advanceLoading(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                    std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                    QueuedCommand& order) {
    CommandQueue& queue = store.orders()[slot];
    const UnitId self = store.idAt(slot);
    const UnitId carrier = order.target();
    const auto finish = [&] { (void)queue.finish(); };

    if (!store.alive(carrier)) {
        finish();
        return;
    }
    if (store.parentOf(self) == carrier) {
        finish();  // aboard — the attach may have landed on an earlier beat
        return;
    }
    const unitdef::UnitDef* carrierDef = catalog.def(store.typeAt(carrier.index));
    const unitdef::UnitDef* cargoDef = catalog.def(store.typeAt(slot));
    if (carrierDef == nullptr || cargoDef == nullptr || !carrierDef->isTransport()
        || !canEverCarry(*carrierDef, *cargoDef)) {
        finish();
        return;
    }

    MoveState& carrierMotion = store.motion()[carrier.index];
    const Transform& carrierAt = store.transforms()[carrier.index];
    const Transform& cargoAt = store.transforms()[slot];

    // The carrier comes to the cargo when it can: an idle one has no better
    // claim on its route, so the pickup steers it overhead — even off the deck,
    // where the order is its takeoff commit. `Down` is the one state left
    // alone: it is mid-landing, and a fresh order would cancel the descent it
    // just committed to.
    if (carrierMotion.canFly && store.orders()[carrier.index].empty()
        && !carrierMotion.moving && carrierMotion.airState != AirState::Down) {
        orderTo(carrierMotion, terrain, cargoAt.x, cargoAt.z);
    }

    if (!grounded(carrierMotion)) {
        // Wait under it: stop walking once inside the shadow so the pair
        // converges rather than each chasing the other's last position.
        MoveState& cargoMotion = store.motion()[slot];
        const Fx gap = groundDistanceElmos(positionOf(cargoAt), positionOf(carrierAt));
        if (gap <= kFerryPickupRadius) {
            cargoMotion.moving = false;
            cargoMotion.path.clear();
            cargoMotion.pathIndex = 0;
        } else if (!cargoMotion.moving) {
            (void)routeToward(slot, carrierAt.x, carrierAt.z, store, terrain, gridForType);
        }
        return;
    }

    // The deck: walk into load reach and sling aboard.
    const Fx gap = groundDistanceElmos(positionOf(cargoAt), positionOf(carrierAt));
    if (gap <= loadReach(carrierMotion, store.motion()[slot])) {
        if (hasRoomFor(store, catalog, carrier, *cargoDef)
            && attachCargo(store, *carrierDef, carrier, self)) {
            finish();
        }
        return;  // full: stand here until a slot frees or the order is cleared
    }
    if (!store.motion()[slot].moving
        && !routeToward(slot, carrierAt.x, carrierAt.z, store, terrain, gridForType)) {
        finish();  // no route to where it waits — the click was legal, the walk is not
    }
}

/// The carrier-side drive for a `Ferry`: beacon → drop → beacon, forever.
/// The anchor is captured where the order started; the phase survives saves.
void advanceFerry(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                  std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                  QueuedCommand& order) {
    const UnitId self = store.idAt(slot);
    const Transform& at = store.transforms()[slot];
    MoveState& motion = store.motion()[slot];
    if (!order.transportAnchor()) {
        order.beginFerry({at.x, at.z});
    }
    const std::array<Fx, 2> beacon = *order.transportAnchor();
    const std::array<Fx, 2> drop = {order.targetX(), order.targetZ()};

    const auto nearPoint = [&](std::array<Fx, 2> point, Fx radius) {
        const Fx dx = at.x - point[0];
        const Fx dz = at.z - point[1];
        return dx * dx + dz * dz <= radius * radius;
    };

    switch (order.transportPhase()) {
    case TransportPhase::ToBeacon: {
        if (grounded(motion) && !motion.moving
            && nearPoint(beacon, kFerryPickupRadius)) {
            order.setTransportPhase(TransportPhase::Loading);
            return;
        }
        (void)commitLeg(slot, beacon[0], beacon[1], store, terrain, gridForType);
        return;
    }
    case TransportPhase::Loading: {
        const unitdef::UnitDef* carrierDef = catalog.def(store.typeAt(slot));
        // Take aboard whatever is standing under the beacon and fits: the
        // pickup is a zone, not an order match — a unit parked in it goes.
        for (UnitIndex other = 0; other < store.slotCount(); ++other) {
            if (other == slot || !store.slotAlive(other)
                || store.motion()[other].attached
                || store.motion()[other].armyIndex != motion.armyIndex) {
                continue;
            }
            const unitdef::UnitDef* def = catalog.def(store.typeAt(other));
            if (def == nullptr || !def->transportable()
                || carrierDef == nullptr || !hasRoomFor(store, catalog, self, *def)) {
                continue;
            }
            if (attachCargo(store, *carrierDef, self, store.idAt(other))) {
                // The order that brought it here — a Move to the beacon — is
                // fulfilled by the pickup, so its queue head retires.
                CommandQueue& queue = store.orders()[other];
                if (const QueuedCommand* head = queue.current(); head != nullptr) {
                    const Fx hx = head->targetX() - beacon[0];
                    const Fx hz = head->targetZ() - beacon[1];
                    if (hx * hx + hz * hz <= kFerryPickupRadius * kFerryPickupRadius) {
                        (void)queue.finish();
                    }
                }
            }
        }
        if (store.childrenOf(self).empty()) {
            return;  // nothing aboard yet: the beacon holds
        }
        // Leave when full, or when nobody transportable still heads for the
        // beacon — a squad strung out along the route is waited on.
        if (carrierDef != nullptr
            && slotsUsed(store, catalog, *carrierDef, self) >= carrierDef->transportCapacity()) {
            orderTo(motion, terrain, drop[0], drop[1]);
            order.setTransportPhase(TransportPhase::ToDrop);
            return;
        }
        for (UnitIndex other = 0; other < store.slotCount(); ++other) {
            if (inboundToBeacon(store, catalog, other, beacon)) {
                return;  // someone is still coming
            }
        }
        orderTo(motion, terrain, drop[0], drop[1]);
        order.setTransportPhase(TransportPhase::ToDrop);
        return;
    }
    case TransportPhase::ToDrop: {
        if (grounded(motion) && !motion.moving
            && nearPoint(drop, kFerryPickupRadius + motion.radiusElmos)) {
            detachCargo(store, terrain, self);
            order.setTransportPhase(TransportPhase::ToBeacon);
            return;
        }
        (void)commitLeg(slot, drop[0], drop[1], store, terrain, gridForType);
        return;
    }
    case TransportPhase::None:
        order.setTransportPhase(TransportPhase::ToBeacon);
        return;
    }
}

/// The carrier-side drive for `UnloadTransport`: get there, come down, and
/// set the hold on the ground. Completes when the last child steps off.
void advanceUnload(UnitStore& store, const UnitCatalog& /*catalog*/, const Terrain& terrain,
                   std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                   QueuedCommand& order) {
    const UnitId self = store.idAt(slot);
    MoveState& motion = store.motion()[slot];
    CommandQueue& queue = store.orders()[slot];
    const Transform& at = store.transforms()[slot];

    const Fx dx = at.x - order.targetX();
    const Fx dz = at.z - order.targetZ();
    const Fx arrive = motion.radiusElmos + Fx::fromInt(4);
    if (grounded(motion) && !motion.moving && dx * dx + dz * dz <= arrive * arrive) {
        detachCargo(store, terrain, self);
        (void)queue.finish();
        return;
    }
    if (!commitLeg(slot, order.targetX(), order.targetZ(), store, terrain, gridForType)) {
        (void)queue.finish();  // no route — the hold keeps its cargo
    }
}

} // namespace

void updateTransports(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                      std::span<const PassabilityGrid* const> gridForType) {
    const std::span<CommandQueue> orders = store.orders();
    for (UnitIndex slot = 0; slot < orders.size(); ++slot) {
        if (!store.slotAlive(slot) || store.motion()[slot].attached) {
            continue;  // cargo rides; it does not act
        }
        QueuedCommand* order = orders[slot].activeMutable();
        if (order == nullptr) {
            continue;
        }
        switch (order->kind()) {
        case CommandKind::LoadTransport:
            advanceLoading(store, catalog, terrain, gridForType, slot, *order);
            break;
        case CommandKind::UnloadTransport:
            advanceUnload(store, catalog, terrain, gridForType, slot, *order);
            break;
        case CommandKind::Ferry:
            advanceFerry(store, catalog, terrain, gridForType, slot, *order);
            break;
        default:
            break;
        }
    }
}

} // namespace rm::sim
