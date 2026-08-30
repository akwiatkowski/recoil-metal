#pragma once

#include "core/sim/Fx.hpp"
#include "core/sim/IdPool.hpp"

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
///
/// FIXED POINT (`Mag`), because these are magnitudes: `BuildCostEnergy` reaches 10,008,000 in
/// the corpus (XSB2401) and a T3 energy farm's storage is larger still, both far outside
/// `Fx`'s ±131,072. See `core/Types.hpp` for the measurement.
struct Resources {
    Mag mass{};
    Mag energy{};

    [[nodiscard]] friend constexpr Resources operator+(Resources a, Resources b) noexcept {
        return Resources{.mass = a.mass + b.mass, .energy = a.energy + b.energy};
    }
    constexpr Resources& operator+=(Resources other) noexcept {
        mass += other.mass;
        energy += other.energy;
        return *this;
    }
    /// Scaling by a fraction — the stall ratio, applied to what everything asked for.
    [[nodiscard]] friend constexpr Resources operator*(Resources a, Fx scale) noexcept {
        return Resources{.mass = a.mass * scale, .energy = a.energy * scale};
    }
};

/// One army's economy for one tick.
struct Economy {
    Resources stored;
    Resources storage;  ///< the cap. Income beyond it is lost, as in the game

    /// What producers deliver PER TICK. Summed from what is standing, so a destroyed
    /// extractor stops paying immediately.
    ///
    /// Per tick rather than per second (§5.1): the per-second figure is a fact about the
    /// blueprint and is converted once, when `UnitCatalog` registers the type. Storing it per
    /// second here would mean a divide in the tick and would leave the unconverted value
    /// where a reader could use it directly.
    Resources incomePerTick;

    /// What standing structures cost to RUN, per second. Energy only in the corpus.
    ///
    /// Charged BEFORE construction is funded. **This is ours, not Forged Alliance's**, and
    /// the comment here used to claim the opposite — that the ordering "is the mechanic".
    /// It is not. Retail has no priority between them: upkeep and construction are peers in
    /// one request list, and when energy is the binding resource both receive the *same*
    /// fraction. Where we starve a build to keep the lights on, retail runs both at half
    /// rate (`C-103`, `C-067`).
    ///
    /// The deeper divergence is that retail models upkeep as demand at all, which is what
    /// lets an unpayable bill throttle the consumers that incurred it. Here it is a lump
    /// subtracted before demand is measured, with the shortfall absorbed — so a brownout is
    /// invisible to shields and intel rather than slowing them. Fixing the ordering without
    /// first making upkeep a request would not move us closer to retail.
    Resources upkeepPerTick;

    /// What last tick TRIED to pay: construction drain plus upkeep. The load figure an AI
    /// wants (income/requested is FA's efficiency); recomputed every tick from hashed
    /// state, so it carries no hash entry of its own.
    Resources requestedLastTick;

    /// What last tick ACTUALLY paid: granted construction plus the affordable part of upkeep.
    /// Distinct from requested demand so a stalled army can report both over-commitment and
    /// real consumption (`C-163`). Recomputed from hashed state alongside requestedLastTick.
    Resources usageLastTick;

    /// The fraction of what was ASKED FOR that was actually paid last tick, 0..1.
    ///
    /// The stall ratio, and the number a player watches. **This is now the WORSE of the two
    /// bucket ratios** (`multiResourceFunded`, `singleResourceFunded`) rather than a single
    /// global throttle — kept because it is the figure the HUD and the AI read, and because
    /// "how stalled am I" wants one number. Consumers no longer multiply by it; they multiply
    /// by whichever bucket ratio applies to them.
    /// `Fx`, not `Mag`: this is a ratio in 0..1, which is what the geometric type is for, and
    /// it is multiplied INTO magnitudes rather than added to them.
    Fx fundedFraction = kFxOne;

    /// Retail's two allocation ratios (`C-159`), read from `0x007790e0`.
    ///
    /// A request is bucketed by **how many resources it still has outstanding**, not by what
    /// it nominally wants: two → the multi-resource bucket, one or zero → the single-resource
    /// bucket. Then
    ///
    ///     r1 = min(1, min_k supply[k] / (multi[k] + single[k]))   // k* = the argmin
    ///     rem[k] = max(0, supply[k] - multi[k] * r1)
    ///     r2 = min(1, min_{k != k*} rem[k] / single[k])
    ///
    /// and a request is granted at `r1` when it is outstanding on the binding resource, else
    /// at `r2`. **The r1 divisor is `multi + single`, not `multi`** — that is the engine's, and
    /// it is what `C-067` had recorded wrongly before `C-159` read the instructions.
    ///
    /// **Aggregating same-bucket consumers is lossless**, which is why we hold two sums rather
    /// than a per-unit request list: both ratios depend only on the bucket totals, so ten
    /// energy-only radar and one radar drawing ten times as much produce identical ratios.
    /// Per-request identity only matters for the carry-forward below.
    Fx multiResourceFunded = kFxOne;
    Fx singleResourceFunded = kFxOne;

    /// Which resource bound the allocation last tick — retail's `k*`.
    ///
    /// **Energy wins an exact tie**, because the engine's `comiss`/`jbe` updates only on a
    /// strict improvement and its loop starts at the energy index (`C-159`). A tie is not
    /// hypothetical: it is what a perfectly balanced economy produces every tick.
    bool massIsBinding = false;

    /// Upkeep granted and not yet spent, carried across ticks (`C-162`).
    ///
    /// Retail's `Consume` drains a request's allocation immediately after the ratio is read,
    /// and the binding resource lands exactly at zero — but the NON-binding one keeps a
    /// residue, so next tick's outstanding is smaller and the request asks for less. Without
    /// this, a two-resource consumer re-asks for its full demand every tick and the bucket
    /// totals never settle.
    Resources upkeepAllocated;
};

/// The base rate of income every army gets, per second, whatever it has built.
///
/// The commander itself: a `.bp` marks it `NaturalProducer = true` and the game gives an
/// ACU a trickle of both so a match can start at all — without it nothing can afford the
/// first extractor and the game never begins. The values here are OURS, chosen small
/// enough to be a bootstrap rather than an economy: the first mass extractor produces 2
/// mass a second, so this is a quarter of one extractor.
/// Per SECOND, because it is an authored value like any blueprint's — converted through the
/// tick rate at the point of use, the same way a producer's output is.
inline constexpr float kCommanderTrickleMassPerSecond = 0.5f;
inline constexpr float kCommanderTrickleEnergyPerSecond = 5.0f;

/// What one thing under construction wants, per second, and what it has had.
struct Construction {
    /// Who is paying, and where it is being built.
    int armyIndex = kNoArmy;
    /// FIXED POINT, like every other position the sim holds (PLAN2.md §5.2, §7 P10.0).
    ///
    /// **THIS WAS THE LAST FLOAT IN SIM STATE**, and it survived because the check that bans
    /// them could not see it. `check_no_sim_floats.sh` finds floats by grepping for the tokens
    /// `float` and `double` in declaring position, so a *conversion call* is invisible to it —
    /// and `Command.cpp` was filling this field with `fxToFloat(command.targetX)`, taking an
    /// `Fx` the caller already had and rounding it into a float, inside the sim, on a value the
    /// state hash then read as raw IEEE bits.
    ///
    /// It was never a live desync: an `int32`-to-`float` conversion is IEEE-defined and
    /// reproducible. It was worse than that in a quieter way — §2 claimed the sim was "fixed
    /// point END TO END (enforced)" and it was not, and the enforcement was structurally unable
    /// to notice. Two round trips existed to prove it: `Skirmish.cpp` converted this field back
    /// to `Fx` to raise an event, and `app/Match.cpp` converted it back again to measure a
    /// distance. Both are now the identity.
    ///
    /// **THE `y` IS ALWAYS ZERO, and that is a decision rather than an omission.** A build order
    /// names a place on the MAP; the ground decides the height, and `spawnUnit` proves it by
    /// overwriting whatever it is handed with `terrain.heightAt(x, z)`. So a height stored here
    /// is never read — it is a derived value that only ever reached the state hash, where it
    /// made two runs of one match look different for a reason no player could observe.
    ///
    /// It was not always zero. The app's build path carried the site's `y` through (a mass
    /// marker's, which the map states), and routing builds through `applyCommand` zeroed it —
    /// which is why that change moved the golden at the first EXTRACTOR and at nothing else.
    /// The screenshot hash was identical across it, which is what said the match was unchanged
    /// while the fingerprint was not.
    std::array<Fx, 3> position{};

    /// What the finished thing costs in total, from the blueprint.
    Resources cost;

    /// Build units still to do. `BuildTime` from the blueprint counts down at the
    /// builder's `BuildRate` per second — so a 60-BuildTime extractor takes six seconds
    /// for a rate-10 commander, which is what the game does with the same numbers.
    Mag buildTimeRemaining{};
    Mag totalBuildTime{};

    /// The builder's rate, in build units PER TICK — derived, like every other rate (§5.1).
    Mag buildPerTick{};

    /// Which unit this becomes. An index into whatever list the caller is building from —
    /// the sim does not know what a unit type is.
    std::size_t blueprintIndex = 0;

    /// When live, this construction UPGRADES that unit in place — Moho's tech path, where a
    /// T2 factory is not a new building but the T1 factory becoming one. Completion replaces
    /// the unit instead of standing a second one on top of it, and the unit dying first
    /// cancels the work (`pruneOrphanUpgrades`). A default UnitId is never live
    /// (generations start at 1), so no flag is needed to mean "an ordinary build".
    UnitId upgradeOf{};

    /// WHO is building this — the unit whose order created it. What an `Assist` resolves
    /// against: "help that engineer" means "add my rate to ITS construction", and without
    /// this field the only link was a position match too fragile to trust. Stale once the
    /// builder dies, which is fine — assistance stops, the work itself continues, exactly
    /// as the game has it.
    UnitId builder{};

    /// What ASSISTERS add this tick, in build units per tick — recomputed every tick by
    /// `applyAssistance` from who is standing in reach with an Assist order, so a helper
    /// that walks away or dies stops helping the same tick. Derived state in a hashed
    /// struct: deterministic by construction, so hashing it costs nothing and catches a
    /// divergence in the assist scan itself.
    Mag assistPerTick{};

    /// Granted and not yet spent, carried across ticks — this construction's half of
    /// `C-162`'s residue. See `Economy::upkeepAllocated` for why it exists.
    Resources allocated;

    /// The funding ratio this work advanced at LAST tick.
    ///
    /// Retail caches the ratio on the builder and reads it a beat later: it is written in the
    /// motion stage, which runs last, and read in the command-dispatch stage, which runs first
    /// (`C-142`, `C-162`). So a builder always spends the previous beat's fraction. The cache
    /// exists because `Consume` drains the allocation the moment the ratio is taken, leaving a
    /// later in-beat read to return roughly zero — it is not an optimisation.
    Fx fundedLastTick = kFxOne;

    /// The rate the work actually advances at: the founder's plus everyone helping.
    [[nodiscard]] Mag effectiveBuildPerTick() const noexcept {
        return buildPerTick + assistPerTick;
    }

    [[nodiscard]] bool isUpgrade() const noexcept { return upgradeOf.generation != 0; }

    [[nodiscard]] bool finished() const noexcept { return buildTimeRemaining <= Mag{}; }

    /// How far along, 0..1. What a progress bar wants, and what the game shows as a
    /// structure rising out of the ground.
    /// An `Fx`, because a progress ratio is geometry rather than a magnitude. Computed by
    /// widening to `FxWide` and shifting, which is the same arithmetic `Fx::operator/` does —
    /// spelled out here because the operands are `Mag` and the result is `Fx`, and there is no
    /// operator for that mix by design: mixing the two types should be visible.
    [[nodiscard]] Fx fraction() const noexcept {
        if (totalBuildTime <= Mag{}) {
            return kFxOne;
        }
        const FxWide done = (totalBuildTime - buildTimeRemaining).raw();
        return Fx::fromRaw(saturate((done << kFxFractionalBits) / totalBuildTime.raw()));
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
[[nodiscard]] Resources drainPerTick(const Construction& work) noexcept;

} // namespace rm::sim
