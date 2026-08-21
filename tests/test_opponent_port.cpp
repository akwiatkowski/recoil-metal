// The opponent port's own contract (ADR-038).
//
// WHAT THIS DOES AND DOES NOT COVER. That the scripted opponent still plays the same match is
// the golden log's job — `make verify` compares 7000 ticks of hashes and is a far stronger
// statement than anything assertable here. What is NOT covered by that log is the port itself:
// the promises an implementation makes to its driver, which a second implementation (ADR-039's
// FAF adapter) has to keep and which no existing test would notice being broken.
//
// So these are the invariants the DRIVER relies on, tested against the one implementation that
// exists, in the state a fresh opponent is in before a match starts.
#include <catch2/catch_test_macros.hpp>

#include "app/Opponent.hpp"

#include <memory>

using rm::ai::Decision;
using rm::ai::Opponent;
using rm::ai::ScriptedOpponent;

TEST_CASE("a fresh opponent has decided nothing", "[opponent][port]") {
    ScriptedOpponent opponent;

    // `runOpponents` applies `drain()` unconditionally after `advance`, so an opponent that has
    // never thought must hand back an empty span rather than anything the caller has to test
    // for. The empty case being ordinary is what lets the driver hold no opinion.
    CHECK(opponent.drain().empty());
}

TEST_CASE("advancing without a world decides nothing rather than crashing", "[opponent][port]") {
    ScriptedOpponent opponent;
    opponent.playFor(0);

    // The driver always calls `observe` first, so this is not a sequence the game produces. It
    // is tested because the port cannot enforce ordering through its signatures, which makes
    // "nothing observed yet" a state every implementation will meet during bring-up — and an
    // adapter under construction is exactly when a crash is most expensive to diagnose.
    opponent.advance(rm::TickIndex{0});
    CHECK(opponent.drain().empty());
}

TEST_CASE("an opponent has not attacked until it says so", "[opponent][port]") {
    ScriptedOpponent opponent;

    // `advanceMatch` reads this to decide where a unit rolling off the factory goes: to the
    // fight, or to the rally point. Before the wave it must be false, or the first tank built
    // walks at the enemy commander alone.
    CHECK_FALSE(opponent.attackLaunched());
}

TEST_CASE("the port answers through the base class", "[opponent][port]") {
    // The driver holds `unique_ptr<Opponent>` and never downcasts, so everything it needs has
    // to be reachable virtually. If this stops compiling, the port has grown a member only the
    // scripted opponent has — which is the moment to ask whether an adapter can answer it.
    const std::unique_ptr<Opponent> opponent = std::make_unique<ScriptedOpponent>();

    CHECK(opponent->drain().empty());
    CHECK_FALSE(opponent->attackLaunched());
}

TEST_CASE("a decision defaults to a move that orders nothing", "[opponent][port]") {
    const Decision decision;

    // The tagged struct's default matters because `applyDecisions` switches on `kind` with no
    // default arm: a value-initialised Decision must land in an arm that does nothing rather
    // than in one that builds something at the origin.
    CHECK(decision.kind == Decision::Kind::Move);
    CHECK(decision.blueprint.empty());
    CHECK(decision.buildRate == 0.0f);
}
