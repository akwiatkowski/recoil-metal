// The three levels of ownership, and the two properties §7 P2.4 asks for.
//
// A participant, a side, and a group that wins together are three different things, and the
// engine had only one of them. What is tested here is that the distinctions do work rather
// than merely existing: victory turns on the ALLIANCE, and a player is not the same thing as
// the army it commands.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/Army.hpp"

#include <vector>

using rm::sim::Army;
using rm::sim::Player;

namespace {

/// Two pairs: armies 0 and 1 allied against 2 and 3. A 2v2, which is the arrangement §7 P2.4
/// names and the smallest one where "an army died" and "a side lost" differ.
[[nodiscard]] std::vector<Army> twoVersusTwo() {
    std::vector<Army> armies = rm::sim::freeForAll(4);
    armies[0].alliance = 0;
    armies[1].alliance = 0;
    armies[2].alliance = 1;
    armies[3].alliance = 1;
    return armies;
}

} // namespace

TEST_CASE("victory fires on alliance elimination, not on losing an army") {
    // THE PROPERTY. A free-for-all hides the difference — one army IS one alliance there — so
    // it takes a 2v2 to tell whether the win condition is reading the right level.
    std::vector<Army> armies = twoVersusTwo();

    SECTION("one of a pair dying does not end it") {
        armies[0].defeated = true;
        CHECK_FALSE(rm::sim::winningAlliance(armies).has_value());

        // Its ally is still standing, so its alliance is still in the match — which is
        // exactly what a per-army win condition would get wrong.
        CHECK(rm::sim::survivorCount(armies) == 3);
    }

    SECTION("both of a pair dying ends it, and the other pair wins") {
        armies[0].defeated = true;
        armies[1].defeated = true;

        const auto winner = rm::sim::winningAlliance(armies);
        REQUIRE(winner.has_value());
        CHECK(*winner == 1);
    }

    SECTION("the same four armies split four ways end it on the first death") {
        // The control: with every army its own alliance, one death leaves three alliances,
        // and three deaths leave one. If the section above passed for the wrong reason — a
        // win condition counting armies rather than alliances — this one would disagree.
        std::vector<Army> free = rm::sim::freeForAll(4);
        free[0].defeated = true;
        free[1].defeated = true;
        CHECK_FALSE(rm::sim::winningAlliance(free).has_value());

        free[2].defeated = true;
        const auto winner = rm::sim::winningAlliance(free);
        REQUIRE(winner.has_value());
        CHECK(*winner == free[3].alliance);
    }

    SECTION("everyone dying is a draw, not a win for the last alliance listed") {
        for (Army& army : armies) {
            army.defeated = true;
        }
        CHECK_FALSE(rm::sim::winningAlliance(armies).has_value());
    }
}

TEST_CASE("allies do not shoot each other, and enemies do") {
    const std::vector<Army> armies = twoVersusTwo();

    CHECK(rm::sim::allied(armies[0], armies[1]));
    CHECK(rm::sim::allied(armies[0], armies[0]));  // an army is its own ally
    CHECK_FALSE(rm::sim::allied(armies[0], armies[2]));

    CHECK_FALSE(rm::sim::hostile(armies[0], armies[1]));
    CHECK(rm::sim::hostile(armies[0], armies[2]));

    // A defeated army is nobody's target — otherwise a winning force keeps shooting a side
    // that is already out.
    std::vector<Army> withLoser = armies;
    withLoser[2].defeated = true;
    CHECK_FALSE(rm::sim::hostile(withLoser[0], withLoser[2]));
}

TEST_CASE("two players sharing an army both command its units") {
    // THE OTHER PROPERTY §7 P2.4 names, and the reason the `Player` level exists at all: a
    // human and a helper script driving one side is not expressible without it.
    const std::vector<Player> players{
        Player{.index = 0, .army = 0, .human = true, .name = "human"},
        Player{.index = 1, .army = 0, .human = false, .name = "helper"},
        Player{.index = 2, .army = 1, .human = false, .name = "opponent"},
    };

    CHECK(rm::sim::commands(players[0], 0));
    CHECK(rm::sim::commands(players[1], 0));
    CHECK_FALSE(rm::sim::commands(players[2], 0));

    const std::vector<rm::PlayerIndex> ofZero = rm::sim::commandersOf(players, 0);
    REQUIRE(ofZero.size() == 2);
    // In the players' own order, because two players applying orders in a different sequence
    // would be a different match.
    CHECK(ofZero[0] == 0);
    CHECK(ofZero[1] == 1);

    const std::vector<rm::PlayerIndex> ofOne = rm::sim::commandersOf(players, 1);
    REQUIRE(ofOne.size() == 1);
    CHECK(ofOne[0] == 2);
}

TEST_CASE("nobody commands nobody") {
    // `kNoArmy` on either side is not a match. A player with no army assigned must not
    // command every decorative unit on the map, which is what a plain equality would give.
    const Player unassigned{.index = 0, .army = rm::sim::kNoArmy, .human = true, .name = "x"};
    CHECK_FALSE(rm::sim::commands(unassigned, rm::sim::kNoArmy));
    CHECK_FALSE(rm::sim::commands(unassigned, 0));

    const Player assigned{.index = 1, .army = 0, .human = true, .name = "y"};
    CHECK_FALSE(rm::sim::commands(assigned, rm::sim::kNoArmy));
}

TEST_CASE("the default line-up is one player per army, with at most one human") {
    const std::vector<Player> players = rm::sim::onePlayerPerArmy(4, /*humanArmy=*/1);

    REQUIRE(players.size() == 4);
    for (std::size_t i = 0; i < players.size(); ++i) {
        CHECK(players[i].index == static_cast<rm::PlayerIndex>(i));
        CHECK(players[i].army == static_cast<int>(i));
        CHECK(players[i].human == (i == 1));
        CHECK_FALSE(players[i].name.empty());
    }

    // And with no human at all, which is what a headless `--play` builds.
    const std::vector<Player> scripted = rm::sim::onePlayerPerArmy(2);
    REQUIRE(scripted.size() == 2);
    CHECK_FALSE(scripted[0].human);
    CHECK_FALSE(scripted[1].human);
}

TEST_CASE("a player is not its army, and the levels do not collapse") {
    // The distinction stated as a test, because it is the one a future session is most
    // likely to optimise away: two players with the same army are still two players, and one
    // player's index says nothing about which army it drives.
    const std::vector<Player> players{
        Player{.index = 0, .army = 3, .human = true, .name = "a"},
        Player{.index = 1, .army = 3, .human = false, .name = "b"},
    };

    CHECK(players[0].index != players[1].index);  // two participants
    CHECK(players[0].army == players[1].army);    // one side
    CHECK(rm::sim::commandersOf(players, 3).size() == 2);

    // And an army nobody drives is legitimate: a decorative crowd, or a side whose player
    // left. It simply has no commanders.
    CHECK(rm::sim::commandersOf(players, 0).empty());
}
