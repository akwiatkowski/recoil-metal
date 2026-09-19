#pragma once

// The UI→sim callback channel (`C-319`, `C-346`, `C-344`).
//
// RETAIL'S SHAPE. `Sim::LuaSimCallback` (`0x00750f20`) imports `/lua/SimCallbacks.lua`
// and calls `DoCallback(name, data, units)`; the user-side `SimCallback` binding
// (`0x008c0520`) is the only way the UI layer reaches INTO the sim outside the command
// stream. `SimCallbacks.lua` is a WHITELIST: `DoCallback` looks the name up in a table
// of named handlers and `error()`s on anything else — the module's own header says the
// handlers "need to validate all arguments against cheats and exploits", which is why
// the channel is a name table and not `ExecuteLuaInSim`.
//
// OUR SHAPE. `doSimCallback` is that dispatch: a fixed set of names, each a function
// over `SimCallbackContext`, and an unknown name is refused exactly like retail's
// `error('No callback named …')` — the caller learns it was refused rather than the
// sim silently doing nothing. The `units` argument retail passes (the sender's
// selection, for `GiveUnitsToPlayer`/`OnControlGroup*`) is not in the bounded slice:
// no whitelisted name here consumes it.
//
// WHAT RIDES IT (`C-346`): diplomacy (`DiplomacyHandler`, `BreakAlliance`,
// `RequestAlliedVictory`, `SetOfferDraw`), resource sharing (`GiveResourcesToPlayer`,
// `SetResourceSharing`) and pings/markers (`SpawnPing`, `UpdateMarker`). What does
// NOT: chat — `SessionSendChatMessage` is a sibling channel (`C-344`), so
// `sendChatMessage` is a separate entry point beside the dispatch rather than a name
// inside it.

#include "core/Types.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Events.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/TickRate.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rm::sim {

/// One map marker — `SimPing.lua`'s `PingMarkers[owner][id]` entry. The sim keeps it
/// because `UpdateMarker` validates against it (a delete/move/rename of a marker that
/// does not exist is refused, like retail's `PingMarkers[data.Owner][data.ID]` check)
/// and because a defeat flushes the owner's markers (`OnArmyDefeat`).
struct SimMarker {
    int owner = kNoArmy;
    int id = 0;
    std::string name;
    std::array<Fx, 3> location{};
};

/// The persistent half of `SimCallbacks.lua` — the module-local tables retail keeps in
/// Lua upvalues: `SimDiplomacy.lua`'s `offers`/`ignoredList` and `SimPing.lua`'s
/// `PingMarkers`/`PingLocked`.
///
/// MATCH STATE, caller-owned like the event queue: it lives on the scene beside the
/// armies it negotiates over, and a saved match that drops it loses in-flight offers
/// the way a retail save loses Lua-side tables it did not serialize.
struct SimCallbackState {
    /// `SimDiplomacy.lua`'s `offers`: (offering army, offered army) pairs. An 'accept'
    /// only lands when the reverse pair is present — retail's
    /// `table.find(offers[action.To], action.From)`.
    std::set<std::pair<int, int>> diplomacyOffers;

    /// `SimDiplomacy.lua`'s `ignoredList`: (ignoring army, ignored army) — a 'never'
    /// answer that suppresses further offers from that army.
    std::set<std::pair<int, int>> diplomacyIgnored;

    /// `SimPing.lua`'s `PingMarkers`, keyed (owner, id) — the cap is per owner.
    std::map<std::pair<int, int>, SimMarker> markers;

    /// `SimPing.lua`'s `PingLocked`, as a tick instead of a `WaitSeconds(1)` thread:
    /// pings are refused while `tick < pingLockedUntil`. Zero means unlocked.
    TickIndex pingLockedUntil = 0;
};

/// `SimPing.lua`'s `MaxPingMarkers` — fifteen live markers per owner, after which
/// `SpawnPing` with `Marker` set is refused.
inline constexpr int kMaxPingMarkers = 15;

/// `SessionSendChatMessage`'s recipient (`C-344`). Retail addresses CLIENTS; the
/// bounded slice addresses armies: a non-negative `to` is the recipient army index,
/// `kChatAll` broadcasts to every army, `kChatAllies` to the sender's alliance
/// (sender included — retail's own UI echoes its sender's line). FAF's 'notify'
/// channel is UI-internal traffic and is refused at the binding, never reaching here.
inline constexpr int kChatAll = -2;
inline constexpr int kChatAllies = -3;

/// The arguments one callback carries — `SimCallback{Func=…, Args={…}}`'s `Args`
/// table, flattened. Retail's handlers read named fields off `data`; the fields below
/// are the union the whitelisted names use, with Lua's 1-based army indices already
/// converted to this engine's 0-based ones by whoever translated the table.
struct SimCallbackArgs {
    /// `data.From`/`data.To` — the armies a diplomacy or transfer action names.
    int from = kNoArmy;
    int to = kNoArmy;
    /// `data.Army` — the single army `SetResourceSharing`/`RequestAlliedVictory`/
    /// `SetOfferDraw` act on.
    int army = kNoArmy;
    /// `data.Owner` — the ping or marker's owner.
    int owner = kNoArmy;
    /// `data.ID` — the marker id `SpawnPing` assigns and `UpdateMarker` names.
    int id = 0;
    /// `data.Value` — the flag `SetResourceSharing`/`RequestAlliedVictory`/
    /// `SetOfferDraw` write.
    bool value = false;
    /// `data.Marker` — whether the ping is a persistent marker rather than a
    /// timed mesh.
    bool marker = false;
    /// `data.Mass`/`data.Energy` — `GiveResourcesToPlayer`'s FRACTIONS of the
    /// sender's stored amount (`SimUtils.lua:147` multiplies by
    /// `GetEconomyStored`, it does not send absolute amounts).
    Fx mass{};
    Fx energy{};
    /// `data.Location` — the ping's world position.
    std::array<Fx, 3> location{};
    /// `data.Action` — `DiplomacyHandler`'s verb ('offer'/'accept'/'reject'/
    /// 'break'/'never') and `UpdateMarker`'s ('delete'/'move'/'rename'/'renew').
    std::string action;
    /// `data.Mesh`/`data.Type` — the ping kind, and `UpdateMarker`'s `Name` on a
    /// rename. Carried as text because the sim does not own the ping-mesh
    /// catalogue; the event is the contract.
    std::string text;
    /// `data.Name` — a marker's label, set at spawn and rewritten by 'rename'.
    std::string name;
};

/// Everything a handler may touch. References rather than values, the same shape and
/// for the same reason as `Match`: a dispatch mutates all of it, and the context is
/// built at each call site from storage that outlives it.
struct SimCallbackContext {
    std::vector<Army>& armies;
    /// One economy per army, indexed like `armies` — `GiveResourcesToPlayer` moves
    /// stored mass/energy between two of them. May be empty (a scene with no
    /// economies refuses transfers rather than indexing nothing).
    std::vector<Economy>& economies;
    /// Where ping/marker/chat notifications go. May be null — the mutation still
    /// happens, only the notification is dropped, like every other emit.
    EventQueue* events = nullptr;
    /// The module-local tables — see `SimCallbackState`.
    SimCallbackState& state;
    /// `ScenarioInfo.TeamGame` (`simInit.lua:162`, `Options.TeamLock == 'locked'`):
    /// locked teams refuse `BreakAlliance` and `RequestAlliedVictory` outright.
    bool teamLock = false;
    /// The tick the dispatch runs on — `PingLocked`'s debounce is measured in ticks
    /// rather than a `WaitSeconds` thread.
    TickIndex tick = 0;
    /// The sim clock, for the one-second ping debounce.
    TickRate rate{};
};

/// `DoCallback(name, data, units)` — the whitelist dispatch (`C-319`).
///
/// Returns whether the name was whitelisted AND its handler accepted the arguments.
/// False covers both retail outcomes: `error('No callback named …')` for an unknown
/// name, and a handler's own early `return` for arguments it refuses (a defeated
/// army, a locked team, a bad marker id) — the channel does not distinguish them to
/// its caller, and neither do we.
[[nodiscard]] bool doSimCallback(const SimCallbackContext& context, std::string_view name,
                                 const SimCallbackArgs& args);

/// `SessionSendChatMessage` (`C-344`) — the sibling channel, not a SimCallback.
///
/// `tauntIndex >= 0` sends a taunt (`taunt.lua:99`'s `{Taunt=true, data=…}` — the
/// index names a line in the taunt table, not text). `templated` sends a shared
/// build template (`build_templates.lua:89`'s `{Template=true, data=…}`) with
/// `text` carrying the serialized template table. Otherwise `text` is the message.
/// Emits `ChatMessage` or `TemplateShared` on the context's queue; returns false for
/// a payload retail's own validation would drop (empty text on a plain message, a
/// recipient army that does not exist).
[[nodiscard]] bool sendChatMessage(const SimCallbackContext& context, int from, int to,
                                   std::string text, int tauntIndex = -1,
                                   bool templated = false);

} // namespace rm::sim
