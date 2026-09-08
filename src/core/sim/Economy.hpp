#pragma once

#include "core/sim/Fx.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/IdPool.hpp"

#include "core/sim/Movement.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
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

/// Derived display/debug readings, rebuilt each tick and never used to drive simulation.
/// Construction charges belong to the founding builder, including its assisted work.
struct UnitResourceFlow {
    UnitId unit{};
    int armyIndex = kNoArmy;
    Resources incomePerTick;
    Resources upkeepPerTick;
    Resources usageLastTick;
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

    /// What standing structures cost to RUN, per tick. Energy only in the corpus.
    ///
    /// A REQUEST, competing with construction rather than preceding it (`C-161`). This
    /// comment twice said the opposite — first that charging upkeep first "is the mechanic",
    /// then that it was ours and wanted fixing — and it is now fixed: upkeep and construction
    /// are peers, and when energy binds they receive the same fraction.
    ///
    /// Retail cannot prioritise between them even in principle. `Unit.lua` sums maintenance
    /// and build cost into ONE consumption figure before the native setter, so the engine
    /// holds a single number per consumer and cannot tell the halves apart (`C-103`, `C-161`).
    ///
    /// One divergence survives and is deliberate: retail's ratio is per unit, ours is per
    /// bucket. Those are the same number for every consumer in this corpus, because upkeep is
    /// energy-only and therefore always lands in the single-resource bucket — see
    /// `singleResourceFunded`. A unit with *mass* upkeep would break the equivalence and need
    /// a real per-unit request.
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

    /// Whether this army offers its over-cap excess to allies (`C-163`).
    ///
    /// Retail gates sharing on a per-army flag and defaults it on for team members; a
    /// free-for-all army has no allies to give to, so the flag never matters there.
    bool sharesOverflow = true;

    /// Excess handed over by allies, waiting to arrive.
    ///
    /// **Credited to INCOME, not to storage** — retail adds a share to the recipient's income
    /// accumulator, which the allocator consumes at the start of the next beat (`C-163`). So a
    /// gift arrives a beat late and is spendable rather than instantly banked, and an ally
    /// already at cap gains nothing from it. Held separately because `incomePerTick` is
    /// rebuilt from standing units every tick and anything written there would be erased.
    Resources sharedIn;

    /// The ratio a consumer with THIS demand shape was granted at — retail's `Unit+0x53c`.
    ///
    /// Retail stores this per unit, on the unit's own `CEconRequest`, and shields and intel
    /// read it as a rate multiplier (`C-161`, `C-071`). We hold bucket sums rather than
    /// per-unit requests because the allocator is linear in them (see `multiResourceFunded`),
    /// and this recovers the per-unit answer from the shape of what a consumer wants.
    ///
    /// **Equivalent for this corpus, and not in general.** Every upkeep payer in Forged
    /// Alliance draws energy only, so every one of them lands in the single-resource bucket
    /// and shares one ratio. A unit with *mass* upkeep would break that and need a real
    /// per-unit request — which is why this takes the demand rather than assuming.
    ///
    /// Returns 1 for a consumer that wants nothing, which keeps it neutral as a multiplier.
    [[nodiscard]] Fx consumedRatio(Resources demand) const noexcept {
        const bool wantsMass = demand.mass > Mag{};
        const bool wantsEnergy = demand.energy > Mag{};
        if (!wantsMass && !wantsEnergy) {
            return kFxOne;
        }
        const bool outstandingOnBinding = massIsBinding ? wantsMass : wantsEnergy;
        return outstandingOnBinding ? multiResourceFunded : singleResourceFunded;
    }

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

    /// A factory's own Build retained behind Guard. A mirrored guardee build has no retained
    /// entry; remembering identity prevents later same-type requests from stealing its completion.
    CommandId retainedCommandId = kInvalidCommandId;

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

    /// Whether the command-dispatch stage advanced this work THIS tick.
    ///
    /// Retail's `Entity+0x520` (`C-187`): a stamp the build helper writes on the target every
    /// beat it works on it, including the `Materialize(0.0f)` heartbeat when no progress is
    /// possible. It exists here for the same reason it exists there — the beat's work and the
    /// beat's bill are computed in different stages, and the bill has to know that the work
    /// happened. Without it the beat that COMPLETES a structure would be free, because
    /// `tickEconomy` runs after `advanceOrders` and would see nothing left to build.
    ///
    /// Set by `advanceConstruction`, cleared by `tickEconomy` once it has charged for it.
    bool workedThisTick = false;

    /// Presentation copy of the work stamp, retained through the completed tick so a renderer
    /// can distinguish an active beam from a stalled site. Cleared at the next `tickSkirmish`
    /// boundary; transient like `workedThisTick`, so it is neither saved nor hashed.
    bool advancedLastTick = false;

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

/// One explicit repair's economy request for this tick.
///
/// Unlike construction, a repair has no persistent partial object: it heals an already-live
/// unit in the economy stage that awarded its request. Keeping the request as data lets repair
/// compete with upkeep and construction in that one allocation pass rather than spending the
/// store ahead of them.
struct RepairWork {
    int armyIndex = kNoArmy;
    UnitIndex builder = 0;
    UnitIndex target = 0;
    Resources demand;
    Fx funded{};
};

/// Continuous enhancement work, funded by the same allocator as construction.
/// EnhanceTask consumes the previous beat's resource fraction at command dispatch;
/// unlike a silo economy event, a partially funded beat makes partial progress.
struct EnhancementWork {
    UnitId owner{};
    std::string name;
    Resources cost;
    Mag totalBuildTime{};
    Mag buildTimeRemaining{};
    Mag buildPerTick{};
    Resources allocated;
    Fx fundedLastTick{};
    bool paused = false;

    [[nodiscard]] bool finished() const noexcept { return buildTimeRemaining <= Mag{}; }
};

[[nodiscard]] Resources drainPerTick(const EnhancementWork& work) noexcept;
void advanceEnhancement(EnhancementWork& work) noexcept;

/// One CAiSiloBuildImpl-shaped counted-projectile record.  It is match-owned rather than a
/// `UnitStore` field: C-081 locates retail's state behind Unit+0x558, not on Unit itself.
struct SiloAmmo {
    UnitId owner{};
    std::size_t weapon = 0;
    /// CAiSiloBuildImpl slot: tactical 0, nuke 1 (`C-081`, `C-082`).
    std::uint8_t slot = 0;
    int stored = 0;
    int capacity = 0;
    TickCount totalTicks = 0;
    TickCount elapsedTicks = 0;
    Resources costPerTick;
    Resources delivered;

    [[nodiscard]] bool building() const noexcept {
        return stored < capacity && totalTicks > 0 && costPerTick.mass >= Mag{}
               && costPerTick.energy >= Mag{};
    }
};

/// One Cybran Loyalist-style missile redirector (`C-088`, ART-S007
/// `lua/sim/defaultantiprojectile.lua:MissileRedirect`). Match-owned like `SiloAmmo`:
/// the redirector is an entity attached to its owner in retail, not unit state.
/// Each redirect costs one full rate cycle (`RedirectRateOfFire`, URL0303: 1/sec);
/// while `remaining` is nonzero the unit watches but does not touch.
struct MissileRedirect {
    UnitId owner{};
    Fx radiusElmos{};
    int cooldownTicks = 0;
    int remaining = 0;
};

/// Creates one silo component from already-bound fixed-point blueprint values (`C-083`, C-241).
[[nodiscard]] SiloAmmo makeSiloAmmo(UnitId owner, std::size_t weapon, bool nukeWeapon,
                                    int capacity, Resources projectileCost, Mag buildTime,
                                    Mag buildPerTick) noexcept;

/// Creates one redirector component from already-bound blueprint values (`C-088`).
/// The cooldown arrives in ticks: the rate-to-ticks conversion is content-side work
/// done once at load, and the sim never sees the float again (PLAN2.md §5.1).
[[nodiscard]] MissileRedirect makeMissileRedirect(UnitId owner, Fx radiusElmos,
                                                  int cooldownTicks) noexcept;

/// Puts one beat's work into one construction — retail's `Unit::Materialize` step.
///
/// WHY IT IS NOT PART OF `tickEconomy` ANY MORE. Retail advances a build inside the builder's
/// own task, which runs in the **command-dispatch stage** — the FIRST stage of a beat
/// (`C-142`, `C-188`) — and the same task then retires the build order when the work is done.
/// The economy ratio it multiplies by is written in the motion stage, which runs LAST, so the
/// figure a build advances on is always the previous beat's (`C-162`, `fundedLastTick`).
/// Computing progress at the end of our beat instead put the work and the queue mutation that
/// follows from it in the wrong stage, which is the residue `C-112` had left open.
///
/// THE CLAMP IS ON THE SUM, where retail clamps each builder's call separately inside
/// `Materialize` (`C-187`). That is not an approximation: the clamp only saturates at
/// completion and every contribution is non-negative, so clamping the sum and clamping each
/// term in turn reach the same fraction whatever order the builders tick in. A test holds it.
/// What genuinely cannot be reproduced is retail's health-during-construction — a late
/// assister on the completing beat contributes no progress but still adds the full
/// `maxHealth × delta` — because a construction here is a record and not a partially built
/// entity with health of its own.
///
/// Stamps `workedThisTick` for billing and `advancedLastTick` for presentation, including a
/// build that finished at the start of the beat.
void advanceConstruction(Construction& work) noexcept;

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
/// A whole-match caller defers the final capacity clamp until `shareOverflow` has run.
void tickEconomy(Economy& economy, std::span<Construction> building,
                  std::span<RepairWork> repairs = {}, std::span<SiloAmmo> siloAmmo = {},
                  bool deferOverflow = false, std::span<UnitResourceFlow> flows = {},
                  int armyIndex = kNoArmy, std::span<EnhancementWork> enhancements = {});

/// Hand each army's over-cap excess to its allies, retail's `C-163` progressive split.
///
/// Run AFTER every army has ticked, because an army's spare capacity is only known once it
/// has spent. Not a flat `1/n`: retail walks the recipients dividing the *remaining* excess
/// by the *remaining* recipient count, capping each by that ally's headroom and subtracting
/// what it took — so an ally with no room passes its share along to the next rather than
/// wasting it, and the last recipient can receive far more than `1/n`.
///
/// Whatever no ally can hold is destroyed, which is retail's behaviour and ours.
void shareOverflow(std::span<Economy> economies, std::span<const Army> armies);

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
