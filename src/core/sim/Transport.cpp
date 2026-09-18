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

bool attachCargo(UnitStore& store, const UnitCatalog& catalog,
                 const unitdef::UnitDef& /*carrier*/, UnitId carrierId,
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

    // `C-198`: cargo hangs from a named `Attachpoint*` bone, not a slot number.
    // The bone's class list is the cargo's own; a class with no free bone falls
    // back to the class-1 list — the one `TransportHasSpaceFor` always prices
    // against. Nearest free bone to where the cargo stands wins, so a squad
    // boarding from one side fills that side first.
    const unitdef::UnitDef* cargoDef = catalog.def(store.typeAt(cargo.index));
    const int cargoClass =
        cargoDef != nullptr ? cargoDef->transportCargoClass() : 1;
    const std::span<const UnitCatalog::AttachBone> bones =
        catalog.attachBones(store.typeAt(carrierId.index));
    const auto boneFree = [&](const UnitCatalog::AttachBone& bone) {
        for (const UnitId child : store.childrenOf(carrierId)) {
            if (store.attachmentBonesOf(child).parent == bone.bone) {
                return false;
            }
        }
        return true;
    };
    const UnitCatalog::AttachBone* best = nullptr;
    Fx bestDist{};
    for (const int klass : {cargoClass, 1}) {
        for (const UnitCatalog::AttachBone& bone : bones) {
            if (bone.cargoClass != klass || !boneFree(bone)) {
                continue;
            }
            // Distance in the carrier's rest frame: rotate the bone into world
            // axes by the hull's heading, then measure to the cargo.
            const std::array<Fx, 2> world = rotateByHeading(
                transforms[carrierId.index].heading, {bone.rest[0], bone.rest[2]});
            const Fx bx = transforms[carrierId.index].x + world[0];
            const Fx bz = transforms[carrierId.index].z + world[1];
            const Fx ddx = transforms[cargo.index].x - bx;
            const Fx ddz = transforms[cargo.index].z - bz;
            const Fx dist = ddx * ddx + ddz * ddz;
            if (best == nullptr || dist < bestDist) {
                best = &bone;
                bestDist = dist;
            }
        }
        if (best != nullptr) {
            break;
        }
    }

    if (best != nullptr) {
        // Pre-place the cargo exactly on the bone so `attach` captures a zero
        // residual — the child rides the bone, not the spot it walked up from.
        const std::array<Fx, 2> world = rotateByHeading(
            transforms[carrierId.index].heading, {best->rest[0], best->rest[2]});
        Transform& at = transforms[cargo.index];
        at.x = transforms[carrierId.index].x + world[0];
        at.z = transforms[carrierId.index].z + world[1];
        at.y = transforms[carrierId.index].y + best->rest[1];
        return store.attach(carrierId, cargo,
                            UnitStore::AttachBones{.parent = best->bone,
                                                   .parentRest = {best->rest[0], best->rest[2]},
                                                   .parentRestHeight = best->rest[1]});
    }

    // No bones resolved (a test catalog, or a carrier whose mesh was never
    // loaded): the slot pattern — cargo slings in a row under the hull, 3 elmos
    // apart, first outboard then filling toward the keel.
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
            && attachCargo(store, catalog, *carrierDef, carrier, self)) {
            finish();
        }
        return;  // full: stand here until a slot frees or the order is cleared
    }
    if (!store.motion()[slot].moving
        && !routeToward(slot, carrierAt.x, carrierAt.z, store, terrain, gridForType)) {
        finish();  // no route to where it waits — the click was legal, the walk is not
    }
}

/// The carrier-side drive for a ferry route: beacon → drop → beacon, forever.
/// `order` supplies the phase state; `beacon`/`drop` are the route's two ends.
/// A `drop` equal to the beacon means "no destination yet" — the carrier
/// loads whatever waits at the beacon and holds, rather than flying a
/// zero-length leg that would detach and re-attach the same cargo each pass.
void advanceFerryRoute(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                       std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                       QueuedCommand& order, std::array<Fx, 2> beacon,
                       std::array<Fx, 2> drop) {
    const UnitId self = store.idAt(slot);
    const Transform& at = store.transforms()[slot];
    MoveState& motion = store.motion()[slot];
    const bool hasDrop = drop[0] != beacon[0] || drop[1] != beacon[1];

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
            if (attachCargo(store, catalog, *carrierDef, self, store.idAt(other))) {
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
        if (!hasDrop) {
            return;  // no destination: hold the loaded cargo at the beacon
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
/// The `Ferry` order's own route: beacon where the order started, drop at the
/// order's target. The anchor is captured where the order started; the phase
/// survives saves.
void advanceFerry(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                  std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                  QueuedCommand& order) {
    const Transform& at = store.transforms()[slot];
    if (!order.transportAnchor()) {
        order.beginFerry({at.x, at.z});
    }
    advanceFerryRoute(store, catalog, terrain, gridForType, slot, order,
                      *order.transportAnchor(), {order.targetX(), order.targetZ()});
}

/// `C-183`'s ferry rung: a transport guarding a `FERRYBEACON` flies the
/// beacon's route — pickup at the beacon, drop where the beacon's own active
/// command points. The guard command's `transportAnchor`/`transportPhase`
/// carry the route state, the same fields a `Ferry` order uses; a beacon with
/// no destination command holds the carrier at the pickup.
void advanceGuardFerry(UnitStore& store, const UnitCatalog& catalog, const Terrain& terrain,
                       std::span<const PassabilityGrid* const> gridForType, UnitIndex slot,
                       QueuedCommand& order) {
    const UnitId beacon = order.target();
    if (!store.alive(beacon)) {
        return;  // dead beacon: the ordinary guard ladder owns the unit again
    }
    const unitdef::UnitDef* guardDef = catalog.def(store.typeAt(slot));
    const unitdef::UnitDef* beaconDef = catalog.def(store.typeAt(beacon.index));
    if (guardDef == nullptr || !guardDef->isTransport()
        || beaconDef == nullptr || !beaconDef->isFerryBeacon()) {
        return;
    }
    const Transform& beaconAt = store.transforms()[beacon.index];
    if (!order.transportAnchor()) {
        order.beginFerry({beaconAt.x, beaconAt.z});
    }
    // The drop is the beacon's own active command target — retail's beacon
    // unit carries the route's destination on its command. No command, or a
    // command with no position, means the route has no far end yet.
    std::array<Fx, 2> drop = {beaconAt.x, beaconAt.z};
    if (const QueuedCommand* head = store.orders()[beacon.index].active();
        head != nullptr) {
        drop = {head->targetX(), head->targetZ()};
    }
    advanceFerryRoute(store, catalog, terrain, gridForType, slot, order,
                      *order.transportAnchor(), drop);
}

/// Whether `slot` has a live cargo inbound on an active `LoadTransport` —
/// the embark claim that keeps a waiting carrier from flying off empty.
[[nodiscard]] UnitId inboundLoader(const UnitStore& store, UnitId self) noexcept {
    for (UnitIndex other = 0; other < store.slotCount(); ++other) {
        if (!store.slotAlive(other)) {
            continue;
        }
        const QueuedCommand* head = store.orders()[other].active();
        if (head != nullptr && head->kind() == CommandKind::LoadTransport
            && head->target() == self) {
            return store.idAt(other);
        }
    }
    return {};
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

    // THE EMBARK PICKUP. An empty hold with a live loader inbound means this
    // unload began as an auto-embark — the click is the drop, but the pickup
    // comes first. A grounded carrier simply waits: the loader's own drive
    // walks the cargo to it. An airborne one flies to meet the cargo and then
    // holds station, because re-issuing the leg every tick would keep
    // resetting the idle-descent timer and the deck would never come down.
    if (store.childrenOf(self).empty()) {
        if (const UnitId loader = inboundLoader(store, self); loader.generation != 0) {
            if (!grounded(motion)) {
                const Transform& theirs = store.transforms()[loader.index];
                const Fx gap = groundDistanceElmos(positionOf(at), positionOf(theirs));
                if (gap > kFerryPickupRadius) {
                    (void)commitLeg(slot, theirs.x, theirs.z, store, terrain, gridForType);
                }
            }
            return;
        }
    }

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
        case CommandKind::Guard:
        case CommandKind::Assist:
            // `C-183`'s ferry rung: a transport guarding a FERRYBEACON flies
            // its route. `advanceGuardFerry` no-ops for every other guard, so
            // the ordinary ladder in `advanceOrders` keeps ownership.
            advanceGuardFerry(store, catalog, terrain, gridForType, slot, *order);
            break;
        default:
            break;
        }
    }
}

bool offerAutoEmbark(UnitStore& store, const UnitCatalog& catalog, UnitIndex slot,
                     const Command& move) noexcept {
    const unitdef::UnitDef* cargoDef = catalog.def(store.typeAt(slot));
    const MoveState& cargoMotion = store.motion()[slot];
    if (cargoDef == nullptr || !cargoDef->transportable() || cargoMotion.attached) {
        return false;
    }

    // The nearest idle same-army carrier that can take the class. An empty
    // queue is the claim — a committed transport is not poached, and a foreign
    // one is not borrowed (cargo rides its own army's decks only).
    const Transform& at = store.transforms()[slot];
    UnitId carrier{};
    Fx nearest{};
    for (UnitIndex other = 0; other < store.slotCount(); ++other) {
        if (other == slot || !store.slotAlive(other)
            || store.motion()[other].attached
            || store.motion()[other].armyIndex != cargoMotion.armyIndex
            || !store.orders()[other].empty()) {
            continue;
        }
        const UnitId id = store.idAt(other);
        const unitdef::UnitDef* def = catalog.def(store.typeAt(other));
        if (def == nullptr || !hasRoomFor(store, catalog, id, *cargoDef)) {
            continue;
        }
        const Fx distance =
            groundDistanceElmos(positionOf(at), positionOf(store.transforms()[other]));
        if (carrier.generation == 0 || distance < nearest) {
            carrier = id;
            nearest = distance;
        }
    }
    if (carrier.generation == 0) {
        return false;
    }

    // Board first, then still owe the click: if the carrier dies the intent
    // the player issued is still on the queue to be tried again or refused.
    CommandQueue& queue = store.orders()[slot];
    Command load{};
    load.kind = CommandKind::LoadTransport;
    load.unit = store.idAt(slot);
    load.target = carrier;
    (void)queue.give(load, false);
    queue.append(move);

    // The carrier owes the click an unload and itself a ride home. While the
    // hold is empty, `advanceUnload`'s pickup hold keeps it at the cargo.
    CommandQueue& carrierQueue = store.orders()[carrier.index];
    Command unload{};
    unload.kind = CommandKind::UnloadTransport;
    unload.unit = carrier;
    unload.targetX = move.targetX;
    unload.targetZ = move.targetZ;
    (void)carrierQueue.give(unload, false);
    const Transform& carrierAt = store.transforms()[carrier.index];
    Command home{};
    home.kind = CommandKind::Move;
    home.unit = carrier;
    home.targetX = carrierAt.x;
    home.targetZ = carrierAt.z;
    carrierQueue.append(home);
    return true;
}

} // namespace rm::sim
