// The UI→sim callback channel: C-319's whitelisted `DoCallback` dispatch, C-346's
// diplomacy/resource/ping handlers, and C-344's `SessionSendChatMessage` events.
// Each TEST_CASE cites its claim IDs; the retail semantics are quoted from
// build/re-fa/corpus/lua/lua/{SimCallbacks,SimUtils,SimPing,SimDiplomacy}.lua.
#include <catch2/catch_test_macros.hpp>

#include "core/map/HeightField.hpp"
#include "core/sim/SimCallbacks.hpp"
#include "core/sim/Skirmish.hpp"

#include "support/TestRoster.hpp"

#include <vector>

using rm::sim::Army;
using rm::sim::EventKind;
using rm::sim::SimCallbackArgs;
using rm::sim::SimCallbackContext;
using rm::sim::SimCallbackState;

namespace {

/// A context over four free-for-all armies with economies and a live event
/// queue — the pieces every handler may touch.
struct Channel {
    std::vector<Army> armies = rm::sim::freeForAll(4);
    std::vector<rm::sim::Economy> economies{4};
    rm::sim::EventQueue events;
    SimCallbackState state;
    rm::sim::TickRate rate{};
    rm::TickIndex tick = 0;

    [[nodiscard]] SimCallbackContext context() {
        return SimCallbackContext{.armies = armies,
                                  .economies = economies,
                                  .events = &events,
                                  .state = state,
                                  .tick = tick,
                                  .rate = rate};
    }

    [[nodiscard]] bool call(std::string_view name, const SimCallbackArgs& args = {}) {
        auto context = this->context();
        return rm::sim::doSimCallback(context, name, args);
    }
};

[[nodiscard]] rm::HeightField flatField() {
    rm::HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

[[nodiscard]] rm::unitdef::UnitDef commanderDef() {
    rm::unitdef::UnitDef def;
    def.name = "UEL0001";
    return def;
}

} // namespace

TEST_CASE("C-319: the callback dispatch is a whitelist — unknown names are refused",
          "[sim]") {
    // `SimCallbacks.lua:13`: `DoCallback` looks the name up in the `Callbacks`
    // table and `error('No callback named …')`s on anything else. A refused
    // name must leave the sim untouched — the channel exists precisely so the
    // UI cannot reach arbitrary sim state.
    Channel channel;

    CHECK_FALSE(channel.call("ExecuteArbitraryLua"));
    CHECK_FALSE(channel.call("GiveResourcesToPlayer2"));
    CHECK_FALSE(channel.call(""));  // even the empty name is just not in the table

    // A whitelisted name answers true — `SetOfferDraw` is the cheapest one.
    CHECK(channel.call("SetOfferDraw", {.army = 0, .value = true}));
    CHECK(channel.armies[0].offeringDraw);
}

TEST_CASE("C-346: GiveResourcesToPlayer moves a fraction of the sender's stores",
          "[sim]") {
    // `SimUtils.lua:147`: `data.Mass`/`data.Energy` are FRACTIONS of what the
    // sender has stored — `TakeResource('Mass', data.Mass * GetEconomyStored)`
    // — not absolute amounts. Both armies must be living; retail does not
    // require them allied.
    Channel channel;
    channel.economies[0].stored = {.mass = rm::sim::magFromFloat(1000.0f),
                                   .energy = rm::sim::magFromFloat(2000.0f)};

    CHECK(channel.call("GiveResourcesToPlayer",
                       {.from = 0, .to = 1,
                        .mass = rm::sim::fxFromFloat(0.5f),
                        .energy = rm::sim::fxFromFloat(0.25f)}));
    CHECK(rm::sim::magToFloat(channel.economies[0].stored.mass) == 500.0f);
    CHECK(rm::sim::magToFloat(channel.economies[0].stored.energy) == 1500.0f);
    CHECK(rm::sim::magToFloat(channel.economies[1].stored.mass) == 500.0f);
    CHECK(rm::sim::magToFloat(channel.economies[1].stored.energy) == 500.0f);

    // A fraction past 1 empties the store — TakeResource clamps at what is
    // there, it does not mint resources.
    CHECK(channel.call("GiveResourcesToPlayer",
                       {.from = 0, .to = 1,
                        .mass = rm::sim::fxFromFloat(2.0f),
                        .energy = rm::sim::fxFromFloat(2.0f)}));
    CHECK(rm::sim::magToFloat(channel.economies[0].stored.mass) == 0.0f);
    CHECK(rm::sim::magToFloat(channel.economies[1].stored.mass) == 1000.0f);

    // `OkayToMessWithArmy`: a defeated army can neither give nor receive.
    channel.armies[2].defeated = true;
    CHECK_FALSE(channel.call("GiveResourcesToPlayer",
                             {.from = 2, .to = 1, .mass = rm::sim::kFxOne}));
    CHECK_FALSE(channel.call("GiveResourcesToPlayer",
                             {.from = 1, .to = 2, .mass = rm::sim::kFxOne}));
    CHECK(rm::sim::magToFloat(channel.economies[1].stored.mass) == 1000.0f);
}

TEST_CASE("C-346: SetResourceSharing and SetOfferDraw write the army flags",
          "[sim]") {
    // `SimUtils.lua:159`/`177`: both are single-flag writes guarded by
    // `OkayToMessWithArmy`.
    Channel channel;
    CHECK(channel.call("SetResourceSharing", {.army = 1, .value = true}));
    CHECK(channel.armies[1].resourceSharing);
    CHECK(channel.call("SetOfferDraw", {.army = 1, .value = true}));
    CHECK(channel.armies[1].offeringDraw);

    channel.armies[1].defeated = true;
    CHECK_FALSE(channel.call("SetResourceSharing", {.army = 1, .value = false}));
    CHECK(channel.armies[1].resourceSharing);  // unchanged
}


TEST_CASE("C-346: DiplomacyHandler accepts only an offer that was made",
          "[sim]") {
    // `SimDiplomacy.lua:5`: 'offer' appends `action.To` to `offers[action.From]`;
    // 'accept' fires `SetAlliance(action.To, action.From, 'Ally')` only when
    // `offers[action.To]` names `action.From` — the acceptor accepts the
    // OFFERER's offer, so an accept with no standing offer is a no-op, which
    // is what stops an army annexing an unwilling ally.
    Channel channel;

    // No offer on the table: the accept is refused, alliances unmoved.
    CHECK_FALSE(channel.call("DiplomacyHandler",
                             {.from = 1, .to = 0, .action = "accept"}));
    CHECK(channel.armies[0].alliance != channel.armies[1].alliance);

    // Army 0 offers army 1; army 1 accepts — the alliances merge.
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 0, .to = 1, .action = "offer"}));
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 1, .to = 0, .action = "accept"}));
    CHECK(channel.armies[0].alliance == channel.armies[1].alliance);
    CHECK(channel.state.diplomacyOffers.empty());
}

TEST_CASE("C-346: reject and never close an offer; break splits the alliance",
          "[sim]") {
    Channel channel;

    // 'reject' removes the pending offer — a second reject finds nothing.
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 0, .to = 1, .action = "offer"}));
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 1, .to = 0, .action = "reject"}));
    CHECK(channel.state.diplomacyOffers.empty());
    CHECK_FALSE(channel.call("DiplomacyHandler",
                             {.from = 1, .to = 0, .action = "reject"}));

    // 'never' records `ignoredList[action.From] += action.To` — army 2 swearing
    // off army 1 blocks army 1's offers TO army 2, not the other way round.
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 2, .to = 1, .action = "never"}));
    CHECK_FALSE(channel.call("DiplomacyHandler",
                             {.from = 1, .to = 2, .action = "offer"}));

    // Form an alliance, then 'break': `IsAlly` gates it and `SetAlliance(…,
    // 'Enemy')` gives the departing army a fresh alliance of its own.
    channel.armies[0].alliance = 0;
    channel.armies[1].alliance = 0;
    CHECK(channel.call("DiplomacyHandler",
                       {.from = 0, .to = 1, .action = "break"}));
    CHECK(channel.armies[1].alliance != channel.armies[0].alliance);
    // Breaking a non-alliance is refused, like retail's `IsAlly` guard.
    CHECK_FALSE(channel.call("DiplomacyHandler",
                             {.from = 0, .to = 1, .action = "break"}));
}

TEST_CASE("C-346: BreakAlliance is refused under locked teams",
          "[sim]") {
    // `SimUtils.lua:9`: `ScenarioInfo.TeamGame` — `Options.TeamLock == 'locked'`
    // in `simInit.lua:162` — refuses the whole callback.
    Channel channel;
    channel.armies[0].alliance = 0;
    channel.armies[1].alliance = 0;

    channel.state = SimCallbackState{};
    auto locked = channel.context();
    locked.teamLock = true;
    CHECK_FALSE(rm::sim::doSimCallback(locked, "BreakAlliance",
                                       {.from = 1, .to = 0}));
    CHECK(channel.armies[1].alliance == channel.armies[0].alliance);

    // Unlocked: `data.From` leaves `data.To`'s alliance for a fresh index.
    CHECK(channel.call("BreakAlliance", {.from = 1, .to = 0}));
    CHECK(channel.armies[1].alliance != channel.armies[0].alliance);
    CHECK(channel.armies[1].alliance > channel.armies[0].alliance);
}

TEST_CASE("C-346: a sole surviving alliance needs RequestingAlliedVictory to win",
          "[sim]") {
    // `victory.lua:50`: the win check is pairwise `IsAlly` AND
    // `RequestingAlliedVictory` on every surviving brain — a multi-member
    // surviving alliance whose members never asked for the shared win does not
    // end the match. `simInit.lua:200` sets the flag for armies teamed at
    // setup; mid-match alliances opt in through the callback.
    const rm::HeightField field = flatField();
    const rm::sim::Terrain terrain{field};

    rm::test::Roster roster;
    const rm::UnitTypeIndex commander = roster.addType(commanderDef());
    (void)roster.add(commander, 0.0f, 0.0f, 0, 12000.0f);
    (void)roster.add(commander, 50.0f, 0.0f, 1, 12000.0f);
    const rm::sim::UnitId enemyA = roster.add(commander, 400.0f, 0.0f, 2, 12000.0f);
    const rm::sim::UnitId enemyB = roster.add(commander, 500.0f, 0.0f, 3, 12000.0f);

    std::vector<Army> armies = rm::sim::freeForAll(4);
    armies[0].alliance = 0;
    armies[1].alliance = 0;
    armies[2].alliance = 1;
    armies[3].alliance = 1;
    std::vector<rm::sim::Economy> economies(4);
    std::vector<rm::sim::Projectile> projectiles;
    const std::vector<int> commandersEver(4, 1);
    rm::sim::Match match{.armies = armies,
                         .economies = economies,
                         .projectiles = &projectiles,
                         .commandersEver = commandersEver};
    const rm::sim::TickRate rate{};
    const rm::TickCount pollTicks = rate.ticks(rm::sim::seconds(3.0f));

    // Alliance 1 falls — but nobody requested the allied victory, so alliance 0
    // is NOT a terminal winner: no pending verdict, no confirmation clock.
    roster.health(enemyA).current = rm::sim::Mag{};
    roster.health(enemyB).current = rm::sim::Mag{};
    for (rm::TickCount tick = 0; tick < pollTicks * 2; ++tick) {
        (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    }
    REQUIRE(armies[2].defeated);
    REQUIRE(armies[3].defeated);
    CHECK_FALSE(match.pendingWinner.has_value());
    CHECK_FALSE(match.over);

    // Both members opt in through the callback — the pending verdict appears.
    SimCallbackState state;
    SimCallbackContext context{.armies = armies,
                               .economies = economies,
                               .events = nullptr,
                               .state = state,
                               .rate = rate};
    CHECK(rm::sim::doSimCallback(context, "RequestAlliedVictory",
                                 {.army = 0, .value = true}));
    CHECK(rm::sim::doSimCallback(context, "RequestAlliedVictory",
                                 {.army = 1, .value = true}));
    (void)rm::sim::tickSkirmish(roster.store, roster.catalog, match, terrain, rate);
    REQUIRE(match.pendingWinner.has_value());
    CHECK(*match.pendingWinner == 0);
}

TEST_CASE("C-346: SpawnPing emits the ping event and markers cap at fifteen",
          "[sim]") {
    // `SimPing.lua:19`: a plain ping is a timed mesh; `data.Marker` files a
    // persistent marker under the lowest free id, capped at `MaxPingMarkers`
    // per owner. `PingLocked` debounces the whole callback for a second.
    Channel channel;

    CHECK(channel.call("SpawnPing",
                       {.owner = 0,
                        .location = {rm::sim::fxFromFloat(10), rm::sim::Fx{},
                                     rm::sim::fxFromFloat(20)},
                        .text = "alert"}));
    REQUIRE(channel.events.count(EventKind::PingSpawned) == 1);
    const rm::sim::Event& ping = channel.events.all().back();
    CHECK(ping.army == 0);
    CHECK(ping.text == "alert");
    CHECK(ping.markerId == -1);  // a timed ping, not a marker
    CHECK(rm::sim::fxToFloat(ping.at[0]) == 10.0f);
    CHECK(rm::sim::fxToFloat(ping.at[2]) == 20.0f);

    // The one-second debounce: a second ping inside the window is refused.
    CHECK_FALSE(channel.call("SpawnPing", {.owner = 0, .text = "move"}));
    channel.tick += channel.rate.ticks(rm::sim::seconds(1.0f));

    // Markers take the lowest free id and are stored for UpdateMarker.
    CHECK(channel.call("SpawnPing",
                       {.owner = 0, .marker = true,
                        .location = {rm::sim::fxFromFloat(5), rm::sim::Fx{},
                                     rm::sim::fxFromFloat(5)},
                        .name = "rally"}));
    CHECK(channel.state.markers.at({0, 1}).name == "rally");
    CHECK(channel.events.all().back().markerId == 1);

    // The cap: fifteen markers per owner, then refusal — `GetPingID` finds no
    // free slot.
    channel.tick += channel.rate.ticks(rm::sim::seconds(1.0f));
    for (int id = 2; id <= rm::sim::kMaxPingMarkers; ++id) {
        REQUIRE(channel.call("SpawnPing", {.owner = 0, .marker = true}));
        channel.tick += channel.rate.ticks(rm::sim::seconds(1.0f));
    }
    CHECK_FALSE(channel.call("SpawnPing", {.owner = 0, .marker = true}));
    // A different owner still has its own fifteen.
    CHECK(channel.call("SpawnPing", {.owner = 1, .marker = true}));
}

TEST_CASE("C-346: UpdateMarker moves, renames and deletes stored markers",
          "[sim]") {
    // `SimPing.lua:106`: every action validates against `PingMarkers[owner][id]`
    // — mutating a marker that does not exist is refused — and a delete is
    // additionally guarded by `OkayToMessWithArmy`.
    Channel channel;
    channel.state.markers[{0, 1}] = rm::sim::SimMarker{
        .owner = 0, .id = 1, .name = "rally",
        .location = {rm::sim::fxFromFloat(5), rm::sim::Fx{}, rm::sim::fxFromFloat(5)}};

    CHECK(channel.call("UpdateMarker",
                       {.owner = 0, .id = 1, .action = "rename", .name = "fall back"}));
    CHECK(channel.state.markers.at({0, 1}).name == "fall back");
    CHECK(channel.events.all().back().markerAction == rm::sim::MarkerAction::Rename);

    CHECK(channel.call("UpdateMarker",
                       {.owner = 0, .id = 1,
                        .location = {rm::sim::fxFromFloat(9), rm::sim::Fx{},
                                     rm::sim::fxFromFloat(9)},
                        .action = "move"}));
    CHECK(rm::sim::fxToFloat(channel.state.markers.at({0, 1}).location[0]) == 9.0f);
    CHECK(channel.events.all().back().markerAction == rm::sim::MarkerAction::Move);

    // A marker that does not exist: refused, whatever the action.
    CHECK_FALSE(channel.call("UpdateMarker",
                             {.owner = 0, .id = 7, .action = "rename", .name = "x"}));
    CHECK_FALSE(channel.call("UpdateMarker",
                             {.owner = 0, .id = 7, .action = "delete"}));
    CHECK_FALSE(channel.call("UpdateMarker",
                             {.owner = 0, .id = 1, .action = "explode"}));

    CHECK(channel.call("UpdateMarker", {.owner = 0, .id = 1, .action = "delete"}));
    CHECK(channel.state.markers.empty());
    CHECK(channel.events.all().back().markerAction == rm::sim::MarkerAction::Delete);
}

TEST_CASE("C-344: SessionSendChatMessage emits chat, taunt and template events",
          "[sim]") {
    // `C-344`: taunts (`taunt.lua:99`'s `{Taunt=true, data=index}`) and shared
    // build templates (`build_templates.lua:89`'s `{Template=true, data=…}`)
    // ride the chat channel, not the command sink — so both arrive as sim
    // events the recipient armies' brains can observe.
    Channel channel;

    CHECK(rm::sim::sendChatMessage(channel.context(), 0, rm::sim::kChatAllies,
                                   "push now"));
    REQUIRE(channel.events.count(EventKind::ChatMessage) == 1);
    const rm::sim::Event& chat = channel.events.all().back();
    CHECK(chat.army == 0);
    CHECK(chat.to == rm::sim::kChatAllies);
    CHECK(chat.text == "push now");
    CHECK(chat.tauntIndex == -1);

    // A taunt carries the taunt-table row, not text.
    CHECK(rm::sim::sendChatMessage(channel.context(), 0, rm::sim::kChatAll,
                                   "", /*tauntIndex=*/7));
    CHECK(channel.events.all().back().tauntIndex == 7);

    // A shared template is its own kind — the payload is the serialized table.
    CHECK(rm::sim::sendChatMessage(channel.context(), 0, /*to=*/1,
                                   "{name='firebase', units={'UEL0103'}}",
                                   /*tauntIndex=*/-1, /*templated=*/true));
    const rm::sim::Event& shared = channel.events.all().back();
    CHECK(shared.kind == EventKind::TemplateShared);
    CHECK(shared.to == 1);

    // `IsValidPayload` (`shared/ChatPayload.lua`): a plain message needs text,
    // and a recipient army must exist.
    CHECK_FALSE(rm::sim::sendChatMessage(channel.context(), 0, rm::sim::kChatAll, ""));
    CHECK_FALSE(rm::sim::sendChatMessage(channel.context(), 0, /*to=*/9, "hello"));
}
