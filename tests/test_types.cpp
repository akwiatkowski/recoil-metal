#include "core/Types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

// The widths are a contract, not a detail.
//
// Two reasons to pin them rather than trust the header:
//
//   1. `core/Types.hpp` has no other consumer yet — the code that uses these lands with
//      P1 and P2 (PLAN2.md). A header nothing includes is a header nothing compiles, and
//      an uncompiled header rots quietly. This file is what compiles it.
//
//   2. Changing any width changes the replay hash, which is the project's whole
//      determinism claim. That should be a deliberate act with a failing test in front of
//      it, not something noticed when two platforms stop agreeing. If you are here because
//      one of these failed: the widening is probably right — bump the replay format version
//      with it, so old logs are rejected loudly rather than compared meaninglessly.

TEST_CASE("the sim's index widths are what the replay hash was computed against") {
    STATIC_REQUIRE(sizeof(rm::UnitIndex) == 4);
    STATIC_REQUIRE(sizeof(rm::ObjectIndex) == 4);
    STATIC_REQUIRE(sizeof(rm::Generation) == 4);
    STATIC_REQUIRE(sizeof(rm::TeamIndex) == 2);
    STATIC_REQUIRE(sizeof(rm::PlayerIndex) == 2);
    STATIC_REQUIRE(sizeof(rm::AllianceIndex) == 2);
    STATIC_REQUIRE(sizeof(rm::UnitTypeIndex) == 2);
    STATIC_REQUIRE(sizeof(rm::TickCount) == 4);
}

TEST_CASE("tick index and state hash are 64-bit, because both outlive a single match") {
    // A replay is concatenated onto others, and a 32-bit tick counter wraps after about
    // two and a half years of match time at 50 Hz. A 64-bit state hash makes "the hashes
    // collided" an explanation nobody has to consider.
    STATIC_REQUIRE(sizeof(rm::TickIndex) == 8);
    STATIC_REQUIRE(sizeof(rm::StateHash) == 8);
}

TEST_CASE("every index width is unsigned, so a negative one cannot be constructed") {
    // Signedness is load-bearing rather than stylistic: these are slots and counters, a
    // negative one is meaningless, and the project builds with -Wsign-conversion — an
    // accidentally signed alias would turn every comparison against a container size into
    // a warning, and the build treats warnings as errors.
    STATIC_REQUIRE(std::is_unsigned_v<rm::UnitIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::ObjectIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::Generation>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::TeamIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::PlayerIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::AllianceIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::UnitTypeIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::TickIndex>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::TickCount>);
    STATIC_REQUIRE(std::is_unsigned_v<rm::StateHash>);
}

TEST_CASE("a handle is index plus generation, and fits in eight bytes") {
    // The shape P1.1 builds on: an id is copyable, hashable, and carries no pointer. Worth
    // asserting now because it is what lets a dead unit's handle fail to resolve instead of
    // naming whoever inherited its slot — and because if it ever exceeds a machine word,
    // passing ids by value everywhere stops being free.
    STATIC_REQUIRE(sizeof(rm::UnitIndex) + sizeof(rm::Generation) == 8);
}
