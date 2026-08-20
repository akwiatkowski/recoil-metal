#pragma once

#include "core/sim/Movement.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::sim {

// What an army can spend, and what it is spending it on.
//
// Supreme Commander's economy is a FLOW rather than a bank: mass and energy arrive per
// second, are spent per second, and the store is a buffer between the two rather than the
// thing you save up. That is the whole reason a stall is a slowdown rather than a refusal
// — run out and everything under construction proceeds at the fraction of its cost you
// can actually afford, which is the mechanic the game is built around.
//
// Read as the specification: `lua/aibrain.lua` for the flow, and the blueprints for every
// number.

/// An army's mass and energy, in the units the blueprints state them.
struct Resources {
    float mass = 0.0f;
    float energy = 0.0f;
};

/// One army's economy for one tick.
struct Economy {
    Resources stored;
    Resources storage;  ///< the cap. Income beyond it is lost, as in the game

    /// What producers deliver per second. Summed from what is standing, so a destroyed
    /// extractor stops paying immediately.
    Resources incomePerSecond;

    /// What standing structures cost to RUN, per second. Energy only in the corpus.
    ///
    /// Charged BEFORE construction is funded, and that order is the mechanic: upkeep is not
    /// optional, so a base whose power fails stops building rather than stopping running.
    /// Funding builds first and letting upkeep take the remainder would invert that and
    /// make a brownout invisible.
    Resources upkeepPerSecond;

    /// The fraction of what was ASKED FOR that was actually paid last tick, 0..1.
    ///
    /// The stall ratio, and the number a player watches: 1 means everything is funded and
    /// anything less means every consumer is slowed by the same proportion. Kept because
    /// it is what construction multiplies its progress by — the alternative, funding the
    /// first builders in list order and starving the rest, makes progress depend on the
    /// order units happen to sit in an array.
    float fundedFraction = 1.0f;
};

/// The base rate of income every army gets, per second, whatever it has built.
///
/// The commander itself: a `.bp` marks it `NaturalProducer = true` and the game gives an
/// ACU a trickle of both so a match can start at all — without it nothing can afford the
/// first extractor and the game never begins. The values here are OURS, chosen small
/// enough to be a bootstrap rather than an economy: the first mass extractor produces 2
/// mass a second, so this is a quarter of one extractor.
inline constexpr Resources kCommanderTrickle{.mass = 0.5f, .energy = 5.0f};

/// What one thing under construction wants, per second, and what it has had.
struct Construction {
    /// Who is paying, and where it is being built.
    int armyIndex = kNoArmy;
    std::array<float, 3> position{};

    /// What the finished thing costs in total, from the blueprint.
    Resources cost;

    /// Build units still to do. `BuildTime` from the blueprint counts down at the
    /// builder's `BuildRate` per second — so a 60-BuildTime extractor takes six seconds
    /// for a rate-10 commander, which is what the game does with the same numbers.
    float buildTimeRemaining = 0.0f;
    float totalBuildTime = 0.0f;

    /// The builder's rate, in build units per second.
    float buildRate = 0.0f;

    /// Which unit this becomes. An index into whatever list the caller is building from —
    /// the sim does not know what a unit type is.
    std::size_t blueprintIndex = 0;

    [[nodiscard]] bool finished() const noexcept { return buildTimeRemaining <= 0.0f; }

    /// How far along, 0..1. What a progress bar wants, and what the game shows as a
    /// structure rising out of the ground.
    [[nodiscard]] float fraction() const noexcept {
        return totalBuildTime <= 0.0f ? 1.0f
                                      : 1.0f - buildTimeRemaining / totalBuildTime;
    }
};

/// Advances one army's economy and everything it is building by one tick.
///
/// TWO PASSES, and the order is the mechanic. First every construction states what it
/// wants this tick; then the army pays what it can and every construction advances by that
/// same fraction. Paying them in order instead would fund whoever came first and starve
/// the rest, which makes build progress depend on array order — the kind of wrong that is
/// invisible until two identical bases behave differently.
///
/// Upkeep is charged before construction is funded, so a base short of power stops
/// BUILDING rather than stopping running — see Economy::upkeepPerSecond.
///
/// Income is added BEFORE spending, so a tick's earnings are available in the same tick.
/// The game does this too, and it is the difference between a just-affordable build
/// proceeding and stuttering every other tick.
///
/// `building` must hold only THIS army's work. The army index on a Construction is there
/// so a caller can partition one list, not so this function can filter one — charging the
/// wrong army is a caller's mistake to avoid rather than something to paper over here,
/// because silently skipping a mismatched entry would leave it never built and never
/// reported.
void tickEconomy(Economy& economy, std::span<Construction> building);

/// Removes what is finished, returning it so the caller can put the units on the map.
[[nodiscard]] std::vector<Construction> takeFinished(std::vector<Construction>& building);

/// What a construction wants per second, given its cost and how fast it is being built.
///
/// The blueprint states a total cost and a total build time; the rate at which a builder
/// consumes is the cost spread over however long the build will actually take. So a faster
/// builder costs MORE per second and the same in total, which is what `BuildRate` means
/// and why two engineers on one structure drain twice as fast.
[[nodiscard]] Resources drainPerSecond(const Construction& work) noexcept;

} // namespace rm::sim
