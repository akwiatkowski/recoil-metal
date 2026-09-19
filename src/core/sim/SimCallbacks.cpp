#include "core/sim/SimCallbacks.hpp"

#include <algorithm>

// The whitelisted handlers, in `SimCallbacks.lua`'s registration order where the
// bounded slice covers them. Every handler validates before it mutates — the module
// comment's "validate all arguments against cheats and exploits" is the reason the
// channel exists at all.
//
// `OkayToMessWithArmy` (`SimUtils.lua`) is retail's guard: the named army must be a
// real, living participant. Retail's version also refuses civilian armies and
// out-of-game brains; the bounded slice's equivalent is "in range and not defeated".

namespace rm::sim {
namespace {

/// `OkayToMessWithArmy`: a valid, undefeated army index.
[[nodiscard]] bool okayToMessWith(const SimCallbackContext& context, int army) noexcept {
    return army >= 0 && static_cast<std::size_t>(army) < context.armies.size()
           && !context.armies[static_cast<std::size_t>(army)].defeated;
}

/// `SetAlliance(a, b, 'Ally')` — retail merges TEAMS, not armies: everyone sharing
/// `to`'s alliance joins `from`'s. The surviving index is `from`'s, matching
/// `DiplomacyHandler`'s `SetAlliance(action.To, action.From, 'Ally')` where the
/// offerer's side absorbs the accepter's.
void mergeAlliance(std::vector<Army>& armies, int from, int to) noexcept {
    const int into = armies[static_cast<std::size_t>(from)].alliance;
    const int outOf = armies[static_cast<std::size_t>(to)].alliance;
    if (into == outOf) {
        return;
    }
    for (Army& army : armies) {
        if (army.alliance == outOf) {
            army.alliance = into;
        }
    }
}

/// `SetAlliance(a, b, 'Enemy')` — `a` leaves `b`'s alliance for a fresh one of its
/// own. The fresh index is one past the highest in use rather than `a`'s army index:
/// a re-broken army would otherwise collide with the alliance it already holds.
///
/// KNOWN EDGE (bounded slice): intel grids are sized per alliance at setup, so an
/// alliance created mid-match has no grid — `Intel::sees` answers false for it,
/// which is a fog-of-war match's "the splinter sees nothing" rather than a crash.
/// Reconfiguring intel on alliance change is retail's `OnAllianceChange` work and
/// is deliberately not in this slice.
void breakOff(std::vector<Army>& armies, int leaving) noexcept {
    int highest = 0;
    for (const Army& army : armies) {
        highest = std::max(highest, army.alliance);
    }
    armies[static_cast<std::size_t>(leaving)].alliance = highest + 1;
}

/// `SimUtils.GiveResourcesToPlayer` (`SimUtils.lua:147`): the sender gives a
/// FRACTION of what it has stored, per resource — `data.Mass * GetEconomyStored`
/// through `TakeResource`, which clamps at the stored amount, so a fraction past 1
/// simply empties the store. Both armies must be living; retail does NOT require
/// them allied (the UI gate is client-side), and neither do we.
bool giveResourcesToPlayer(const SimCallbackContext& context, const SimCallbackArgs& args) {
    if (!okayToMessWith(context, args.from) || !okayToMessWith(context, args.to)) {
        return false;
    }
    const auto from = static_cast<std::size_t>(args.from);
    const auto to = static_cast<std::size_t>(args.to);
    if (from >= context.economies.size() || to >= context.economies.size()) {
        return false;
    }
    Economy& sender = context.economies[from];
    Economy& receiver = context.economies[to];
    const Fx fraction = std::clamp(args.mass, Fx{}, kFxOne);
    const Mag massTaken = sender.stored.mass * fraction;
    const Fx energyFraction = std::clamp(args.energy, Fx{}, kFxOne);
    const Mag energyTaken = sender.stored.energy * energyFraction;
    sender.stored.mass -= massTaken;
    sender.stored.energy -= energyTaken;
    receiver.stored.mass += massTaken;
    receiver.stored.energy += energyTaken;
    return true;
}

/// `SimDiplomacy.DiplomacyHandler` (`SimDiplomacy.lua:5`): the offer/accept/reject/
/// break/never verb set over the `offers` and `ignoredList` tables.
bool diplomacyHandler(const SimCallbackContext& context, const SimCallbackArgs& args) {
    if (!okayToMessWith(context, args.from) || !okayToMessWith(context, args.to)) {
        return false;
    }
    const std::pair<int, int> offered{args.from, args.to};
    if (args.action == "offer") {
        // `table.find` guard: a repeated offer is a no-op, not a second entry.
        if (context.state.diplomacyIgnored.contains({args.to, args.from})) {
            return false;  // 'never' already answered for this pair
        }
        context.state.diplomacyOffers.insert(offered);
        return true;
    }
    if (args.action == "accept") {
        // `table.find(offers[action.To], action.From)`: the acceptance only lands
        // when the accepter had offered the sender first — mutual offers make an
        // alliance, a one-sided accept does not.
        if (!context.state.diplomacyOffers.contains({args.to, args.from})) {
            return false;
        }
        context.state.diplomacyOffers.erase({args.to, args.from});
        context.state.diplomacyOffers.erase(offered);
        mergeAlliance(context.armies, args.from, args.to);
        return true;
    }
    if (args.action == "reject") {
        return context.state.diplomacyOffers.erase({args.to, args.from}) > 0;
    }
    if (args.action == "break") {
        // `IsAlly(action.To, action.From)` then `SetAlliance(…, 'Enemy')`: the
        // army named `to` leaves the alliance it shared with `from`.
        Army& departing = context.armies[static_cast<std::size_t>(args.to)];
        if (departing.alliance != context.armies[static_cast<std::size_t>(args.from)].alliance) {
            return false;
        }
        breakOff(context.armies, args.to);
        return true;
    }
    if (args.action == "never") {
        context.state.diplomacyIgnored.insert(offered);
        return true;
    }
    return false;
}

/// `SimUtils.BreakAlliance`: `data.From` leaves `data.To`'s alliance — refused
/// outright under `ScenarioInfo.TeamGame` (locked teams).
bool breakAlliance(const SimCallbackContext& context, const SimCallbackArgs& args) {
    if (context.teamLock) {
        return false;
    }
    if (!okayToMessWith(context, args.from) || !okayToMessWith(context, args.to)) {
        return false;
    }
    breakOff(context.armies, args.from);
    return true;
}

/// `SimPing.SpawnPing` (`SimPing.lua:19`): a timed ping mesh, or a persistent
/// marker when `data.Marker` is set. The marker half is capped per owner and
/// validated against the stored table; both halves debounce on `PingLocked` —
/// retail's one-second `WaitSeconds` guard, here a tick deadline.
bool spawnPing(const SimCallbackContext& context, const SimCallbackArgs& args) {
    if (context.tick < context.state.pingLockedUntil) {
        return false;
    }
    if (args.owner < 0 || static_cast<std::size_t>(args.owner) >= context.armies.size()) {
        return false;
    }
    int markerId = -1;
    if (args.marker) {
        // `GetPingID`: the lowest free slot under `MaxPingMarkers`, or refusal.
        int freeId = 0;
        for (int id = 1; id <= kMaxPingMarkers; ++id) {
            if (!context.state.markers.contains({args.owner, id})) {
                freeId = id;
                break;
            }
        }
        if (freeId == 0) {
            return false;
        }
        markerId = freeId;
        context.state.markers[{args.owner, freeId}] = SimMarker{
            .owner = args.owner, .id = freeId,
            .name = args.name, .location = args.location};
    }
    context.state.pingLockedUntil = context.tick + context.rate.ticks(seconds(1.0f));
    emit(context.events, Event{.kind = EventKind::PingSpawned,
                               .army = args.owner,
                               .at = args.location,
                               .to = args.owner,
                               .markerId = markerId,
                               .text = args.text});
    return true;
}

/// `SimPing.UpdateMarker` (`SimPing.lua:106`): delete/move/rename/renew against the
/// stored marker table. A delete is guarded by `OkayToMessWithArmy`; the other
/// actions only need the marker to exist.
bool updateMarker(const SimCallbackContext& context, const SimCallbackArgs& args) {
    const auto found = context.state.markers.find({args.owner, args.id});
    MarkerAction action = MarkerAction::None;
    if (args.action == "delete") {
        if (found == context.state.markers.end() || !okayToMessWith(context, args.owner)) {
            return false;
        }
        context.state.markers.erase(found);
        action = MarkerAction::Delete;
    } else if (args.action == "move") {
        if (found == context.state.markers.end()) return false;
        found->second.location = args.location;
        action = MarkerAction::Move;
    } else if (args.action == "rename") {
        if (found == context.state.markers.end()) return false;
        found->second.name = args.name;
        action = MarkerAction::Rename;
    } else if (args.action == "renew") {
        // Retail's renew re-sends every allied marker; the bounded slice reports
        // the request and mutates nothing.
        action = MarkerAction::Renew;
    } else {
        return false;
    }
    emit(context.events, Event{.kind = EventKind::MarkerUpdated,
                               .army = args.owner,
                               .at = args.location,
                               .to = args.owner,
                               .markerId = args.id,
                               .markerAction = action,
                               .text = args.name});
    return true;
}

} // namespace

bool doSimCallback(const SimCallbackContext& context, const std::string_view name,
                   const SimCallbackArgs& args) {
    // The whitelist — `SimCallbacks.lua`'s `Callbacks` table, bounded to the names
    // this slice implements. An unlisted name is refused like retail's
    // `error('No callback named …')`: the caller is told, the sim is untouched.
    if (name == "GiveResourcesToPlayer") {
        return giveResourcesToPlayer(context, args);
    }
    if (name == "SetResourceSharing") {
        // `brain:SetResourceSharing(data.Value)` — the flag lives on the army.
        if (!okayToMessWith(context, args.army)) return false;
        context.armies[static_cast<std::size_t>(args.army)].resourceSharing = args.value;
        return true;
    }
    if (name == "RequestAlliedVictory") {
        // Refused in a locked team game (`SimUtils.lua:166`), else the army's
        // `RequestingAlliedVictory` flag — read by the victory pass.
        if (context.teamLock || !okayToMessWith(context, args.army)) return false;
        context.armies[static_cast<std::size_t>(args.army)].requestingAlliedVictory =
            args.value;
        return true;
    }
    if (name == "SetOfferDraw") {
        if (!okayToMessWith(context, args.army)) return false;
        context.armies[static_cast<std::size_t>(args.army)].offeringDraw = args.value;
        return true;
    }
    if (name == "BreakAlliance") {
        return breakAlliance(context, args);
    }
    if (name == "DiplomacyHandler") {
        return diplomacyHandler(context, args);
    }
    if (name == "SpawnPing") {
        return spawnPing(context, args);
    }
    if (name == "UpdateMarker") {
        return updateMarker(context, args);
    }
    return false;
}

bool sendChatMessage(const SimCallbackContext& context, const int from, const int to,
                     std::string text, const int tauntIndex, const bool templated) {
    // `IsValidPayload` (`shared/ChatPayload.lua`): a plain message needs non-empty
    // text; a recipient army must exist. 'notify' never reaches here — the binding
    // drops UI-internal traffic before it becomes a sim event.
    if (!templated && tauntIndex < 0 && text.empty()) {
        return false;
    }
    if (to >= 0 && static_cast<std::size_t>(to) >= context.armies.size()) {
        return false;
    }
    if (from >= static_cast<int>(context.armies.size())) {
        return false;
    }
    emit(context.events,
         Event{.kind = templated ? EventKind::TemplateShared : EventKind::ChatMessage,
               .army = from,
               .to = to,
               .tauntIndex = tauntIndex,
               .text = std::move(text)});
    return true;
}

} // namespace rm::sim
