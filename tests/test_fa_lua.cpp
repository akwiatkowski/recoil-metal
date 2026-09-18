// Player-perspective coverage for lua claims (see docs/fa-exe-analysis-plan.md):
// WP-07's scheduler contract (C-305, C-307, C-310) as the FAF sandbox implements it.
//
// Recoil Metal has no retail-style sim-Lua host (see build/re-fa/coverage/FA-LUA.md):
// the one real Lua VM is the FAF AI sandbox, whose coroutine pump reimplements the
// ForkThread/WaitTicks thread model. These cases pin the parts of the contract a
// player can observe — wake order, wait length, and error containment — through the
// sandbox's own pump, the same driver the AI runs on.
#include <catch2/catch_test_macros.hpp>

#include "app/FafAi.hpp"

#include <filesystem>

using rm::ai::FafAi;

namespace {

[[nodiscard]] std::filesystem::path corpusRoot() {
    // The tests run from the build directory; the corpus sits beside the source tree.
    for (const char* candidate : {"vendor/ai/faf", "../vendor/ai/faf", "../../vendor/ai/faf"}) {
        if (std::filesystem::is_directory(candidate)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return {};
}

} // namespace

TEST_CASE("C-305: threads due on the same tick resume in fork order",
          "[fa-lua][threads]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // C-305's observable half: a stage consumes its ready ring in strict FIFO. Three
    // workers fork in order and each sleeps a different number of ticks; every pass
    // must resume the due set oldest-first, or a manager that forks before another
    // would starve behind it.
    REQUIRE(ai.eval(R"(
        order = {}
        local function worker(name, sleep)
            while true do
                table.insert(order, name)
                WaitTicks(sleep)
            end
        end
        ForkThread(function() worker('a', 1) end)
        ForkThread(function() worker('b', 2) end)
        ForkThread(function() worker('c', 3) end)
    )"));

    // Tick 0: all three are due, in fork order.
    CHECK(ai.pump(0) == 3);
    REQUIRE(ai.eval("assert(order[1] == 'a' and order[2] == 'b' and order[3] == 'c')"));

    // Tick 1: only 'a' is due again. Tick 2: 'a' then 'b' — 'a' re-queued first
    // because it woke first, which is the splice-preserves-order half of the claim.
    CHECK(ai.pump(1) == 1);
    CHECK(ai.pump(2) == 2);
    REQUIRE(ai.eval(R"(
        assert(order[4] == 'a')
        assert(order[5] == 'a' and order[6] == 'b')
    )"));

    // Tick 3: 'a' and 'c' are due ('c' slept 3), still oldest-first.
    CHECK(ai.pump(3) == 2);
    REQUIRE(ai.eval("assert(order[7] == 'a' and order[8] == 'c')"));
    CHECK(ai.threadErrors().empty());
}

TEST_CASE("C-307: WaitTicks sleeps the documented n ticks, floored at one",
          "[fa-lua][threads]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // DOCUMENTED DIVERGENCE, pinned deliberately: retail's `WaitTicks(n)` yields the
    // status `n` verbatim and resumes after `n−1` beats (C-307), one shorter than
    // `ScriptTask.lua`'s own comment describes. This sandbox waits n FULL ticks —
    // `wake = tick + max(1, n)` (src/app/FafAi.cpp) — matching the documented
    // semantics rather than the measured off-by-one. The test asserts OUR contract;
    // the claim id marks where the behaviours differ.
    REQUIRE(ai.eval(R"(
        woke = {}
        ForkThread(function()
            WaitTicks(3)
            table.insert(woke, 'three')
        end)
        ForkThread(function()
            WaitTicks(0)   -- floored at one: a zero wait is still a yield
            table.insert(woke, 'zero')
        end)
        ForkThread(function()
            WaitSeconds(0.5)  -- 5 ticks at the sandbox's 10 Hz
            table.insert(woke, 'half')
        end)
    )"));

    CHECK(ai.pump(0) == 3);   // all three start and yield
    CHECK(ai.pump(1) == 1);   // only the floored zero-wait returns
    CHECK(ai.pump(2) == 0);   // the 3-tick wait is NOT back early — retail's n−1
    CHECK(ai.pump(3) == 1);   // would have resumed it here
    CHECK(ai.pump(4) == 0);
    CHECK(ai.pump(5) == 1);   // the half-second wait lands on its fifth tick
    REQUIRE(ai.eval(R"(
        assert(woke[1] == 'zero' and woke[2] == 'three' and woke[3] == 'half')
    )"));
    CHECK(ai.threadErrors().empty());
}

TEST_CASE("C-310: an erroring thread dies alone and the pass continues",
          "[fa-lua][threads]") {
    const std::filesystem::path root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    FafAi ai(root);
    REQUIRE(ai.ready());

    // C-310's containment rule, in the direction the existing mayfly case does not
    // cover: the broken thread is FIRST in the pass. If the error propagated instead
    // of being contained, the threads queued behind it would never run — which is
    // what "errors kill only the thread" means to every manager sharing the stage.
    REQUIRE(ai.eval(R"(
        ran = {}
        ForkThread(function() error('first thread blows up') end)
        ForkThread(function() table.insert(ran, 'second') end)
        ForkThread(function() table.insert(ran, 'third') end)
    )"));

    CHECK(ai.pump(0) == 3);   // all three were resumed; the error did not stop the pass
    CHECK(ai.threadsAlive() == 0);
    REQUIRE(ai.threadErrors().size() == 1);
    REQUIRE(ai.eval("assert(ran[1] == 'second' and ran[2] == 'third')"));

    // And the sandbox itself is unharmed: the next tick still pumps and evaluates.
    CHECK(ai.pump(1) == 0);
    REQUIRE(ai.eval("assert(true)"));
}
