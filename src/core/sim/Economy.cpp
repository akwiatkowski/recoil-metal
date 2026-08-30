#include "core/sim/Economy.hpp"

#include <algorithm>

namespace rm::sim {

Resources drainPerTick(const Construction& work) noexcept {
    // The EFFECTIVE rate — founder plus assisters — throughout: help makes the work drain
    // faster as well as finish sooner, which is what `BuildRate` means and why piling
    // engineers onto one build is a decision about the bank, not just the clock.
    // Retail does NOT reconcile the completing request with the amount Materialize applies:
    // every builder offers its full delta and the target alone clamps progress (`C-100`,
    // `C-187`). Our aggregate therefore prices the full combined rate even on the last tick.
    const Mag rate = work.effectiveBuildPerTick();
    if (work.totalBuildTime <= Mag{} || rate <= Mag{}) {
        return Resources{};
    }
    // TICKS the whole build will take at this rate, and the cost spread over them. The
    // per-second version of this divided by seconds; the arithmetic is the same shape, with
    // the unit of time already folded into the per-tick rate.
    //
    // The division is `Mag / Mag -> Fx`, done explicitly: the share of the total cost that
    // falls in one tick is a ratio, which is what the geometric type holds.
    const Fx sharePerTick = Fx::fromRaw(saturate(
        (FxWide{rate.raw()} << kFxFractionalBits) / work.totalBuildTime.raw()));
    return work.cost * sharePerTick;
}

namespace {

/// `numerator / denominator` as a ratio, clamped to 1. Retail's `divss` then `min`.
[[nodiscard]] Fx fundingRatio(Mag numerator, Mag denominator) noexcept {
    if (denominator <= Mag{}) {
        return kFxOne;  // nothing wanted is fully funded, which keeps the `min` neutral
    }
    const Fx raw = Fx::fromRaw(saturate((FxWide{numerator.raw()} << kFxFractionalBits)
                                        / denominator.raw()));
    return std::min(raw, kFxOne);
}

} // namespace

void tickEconomy(Economy& economy, std::span<Construction> building) {
    // Clamp only what CARRIED IN. Reclaim currently credits `stored` directly before this
    // pass, so its over-cap excess is still lost rather than becoming a hidden reserve.
    economy.stored.mass = std::max(Mag{}, std::min(economy.stored.mass, economy.storage.mass));
    economy.stored.energy =
        std::max(Mag{}, std::min(economy.stored.energy, economy.storage.energy));

    // Income then becomes spendable before the final capacity clamp, as retail's allocator
    // does (`C-163`). A full bank can therefore pay one tick's bill from one tick's income
    // and remain full. NO MULTIPLY BY A TICK LENGTH: this is already a per-tick rate (§5.1).
    economy.stored += economy.incomePerTick;
    // What allies handed over last tick arrives with income, not as a deposit (`C-163`), so it
    // is spendable this tick and an army already at cap gains nothing from it.
    economy.stored += economy.sharedIn;
    economy.sharedIn = Resources{};

    // UPKEEP IS A REQUEST, not a lump taken off the top (`C-161`). Retail never sees upkeep
    // and construction as two things: `Unit.lua` sums maintenance and build cost into ONE
    // consumption figure before the native setter, so the engine cannot prioritise between
    // them even in principle. Where we used to starve a build to keep the lights on, retail
    // runs both at whatever fraction the allocator hands out.
    //
    // Pass one: what is still OUTSTANDING, per consumer. Retail computes
    // `outstanding = max(0, demand - allocated)`, so a consumer holding an unspent residue
    // from last tick asks for correspondingly less (`C-162`).
    const auto outstanding = [](Resources demand, Resources allocated) {
        return Resources{
            .mass = std::max(Mag{}, demand.mass - allocated.mass),
            .energy = std::max(Mag{}, demand.energy - allocated.energy),
        };
    };

    // Bucketed by HOW MANY RESOURCES ARE STILL OUTSTANDING — not by nominal demand
    // (`C-159`). A two-resource consumer whose mass is already covered drops into the
    // single-resource bucket and is funded at `r2`, which is the case that made the engine's
    // scheme look wrong until the immediates were read.
    Resources multi;   // both resources outstanding
    Resources single;  // one or neither

    const auto bucket = [&multi, &single](Resources out) {
        const int resources = (out.mass > Mag{} ? 1 : 0) + (out.energy > Mag{} ? 1 : 0);
        (resources == 2 ? multi : single) += out;
    };

    const Resources upkeepOutstanding = outstanding(economy.upkeepPerTick,
                                                    economy.upkeepAllocated);
    bucket(upkeepOutstanding);

    Resources wanted = economy.upkeepPerTick;
    for (Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        const Resources demand = drainPerTick(work);
        wanted += demand;
        bucket(outstanding(demand, work.allocated));
    }

    // The DEMAND, remembered for whoever asks how loaded this economy is. Construction
    // plus upkeep — everything the tick tried to pay — which is what Moho's
    // GetEconomyRequested reports and what the FAF brain's efficiency conditions divide
    // income by. Derived from state the hash already covers, so it is deterministic
    // without being fed to the hash itself.
    economy.requestedLastTick = wanted;

    // Pass two: the two ratios. `supply` is what is on hand — retail's `stored + income`,
    // which income has already been folded into above.
    const Resources supply = economy.stored;
    const Resources total{.mass = multi.mass + single.mass,
                          .energy = multi.energy + single.energy};

    // r1 over the COMBINED total, and the resource that bound it. Energy wins an exact tie
    // because the engine's comparison updates only on a strict improvement and its loop
    // starts at energy (`C-159`).
    const Fx massRatio = fundingRatio(supply.mass, total.mass);
    const Fx energyRatio = fundingRatio(supply.energy, total.energy);
    const bool massBinds = massRatio < energyRatio;
    const Fx r1 = massBinds ? massRatio : energyRatio;

    // r2 covers the single-resource bucket out of whatever the multi-resource bucket left,
    // and it ignores the binding resource — that one is already spent to the last unit.
    const Resources remaining{
        .mass = std::max(Mag{}, supply.mass - multi.mass * r1),
        .energy = std::max(Mag{}, supply.energy - multi.energy * r1),
    };
    const Fx r2 = massBinds ? fundingRatio(remaining.energy, single.energy)
                            : fundingRatio(remaining.mass, single.mass);

    economy.multiResourceFunded = r1;
    economy.singleResourceFunded = r2;
    economy.massIsBinding = massBinds;
    economy.fundedFraction = std::min(r1, r2);

    // Grant, and spend. A consumer takes `r1` when it is outstanding on the binding resource,
    // else `r2` — which is the bucket it was counted in.
    const auto grantFor = [massBinds, r1, r2](Resources out) {
        const Mag binding = massBinds ? out.mass : out.energy;
        return binding > Mag{} ? r1 : r2;
    };

    // Grant, then CONSUME. Retail keeps these separate and the separation is the whole
    // mechanism: the allocator moves resources out of the economy into a request's
    // `allocated`, and `Consume` later moves them from there into actual work. The binding
    // resource lands exactly at zero, but the non-binding one keeps a residue that carries
    // into the next tick as a pre-credit (`C-162`). Zeroing `allocated` here instead would
    // look tidier and would delete that behaviour entirely.
    Resources granted;
    const auto grantAndConsume = [&granted, &outstanding, &grantFor](Resources demand,
                                                                     Resources& allocated) {
        const Resources out = outstanding(demand, allocated);
        const Fx ratio = grantFor(out);
        const Resources share = out * ratio;
        allocated += share;
        granted += share;

        // What the consumer actually spends this tick, taken out of its allocation.
        const Resources consumed = demand * ratio;
        allocated.mass = std::max(Mag{}, allocated.mass - consumed.mass);
        allocated.energy = std::max(Mag{}, allocated.energy - consumed.energy);
        return ratio;
    };

    grantAndConsume(economy.upkeepPerTick, economy.upkeepAllocated);

    for (Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        // Progress FIRST, on last tick's fraction (`C-162`). Retail writes the ratio onto the
        // builder in the motion stage, which runs last, and reads it in command dispatch,
        // which runs first (`C-142`) — so a builder always advances on the previous beat's
        // funding. Using the fraction computed just above would be a one-tick head start the
        // engine does not give, and it is the difference `C-100` measured.
        work.buildTimeRemaining -= work.effectiveBuildPerTick() * work.fundedLastTick;
        // The request stays deliberately uncapped even on the completing tick: retail does not
        // reconcile it with what `Materialize` applies (`C-100`, `C-187`).
        work.buildTimeRemaining = std::max(Mag{}, work.buildTimeRemaining);

        work.fundedLastTick = grantAndConsume(drainPerTick(work), work.allocated);
    }

    economy.usageLastTick = granted;
    economy.stored.mass = std::max(Mag{}, economy.stored.mass - granted.mass);
    economy.stored.energy = std::max(Mag{}, economy.stored.energy - granted.energy);
    // Capacity is applied AFTER spending. Excess is still lost here because allied sharing is
    // not implemented; retail offers it to allies before this same final clamp (`C-163`).
    // The zero floor is OUTSIDE the cap and not redundant: a negative capacity would otherwise
    // pull the store below zero, and the next tick's ratios would then run every build
    // backwards. A test holds this.
    economy.stored.mass =
        std::max(Mag{}, std::min(economy.stored.mass, economy.storage.mass));
    economy.stored.energy =
        std::max(Mag{}, std::min(economy.stored.energy, economy.storage.energy));
}

void shareOverflow(std::span<Economy> economies, std::span<const Army> armies) {
    // Retail's split is PROGRESSIVE, not flat (`C-163`): it divides the *remaining* excess by
    // the *remaining* recipient count, caps each share by that ally's headroom, and subtracts
    // what was actually taken. So an ally with no room passes its share on rather than wasting
    // it, and the last recipient can receive far more than an equal split would give.
    //
    // A flat `1/n` would be the obvious reading and is wrong wherever any ally is near cap,
    // which in a stalling team is most of the time.
    for (std::size_t giver = 0; giver < economies.size(); ++giver) {
        Economy& from = economies[giver];
        if (!from.sharesOverflow || giver >= armies.size()) {
            continue;
        }

        Resources excess{
            .mass = std::max(Mag{}, from.stored.mass - from.storage.mass),
            .energy = std::max(Mag{}, from.stored.energy - from.storage.energy),
        };
        if (excess.mass <= Mag{} && excess.energy <= Mag{}) {
            continue;
        }

        // Recipients, in army order — the traversal order is part of the result, because the
        // progressive split gives later allies whatever earlier ones could not hold.
        std::vector<std::size_t> allies;
        for (std::size_t other = 0; other < economies.size(); ++other) {
            if (other != giver && other < armies.size()
                && armies[other].alliance == armies[giver].alliance) {
                allies.push_back(other);
            }
        }

        std::size_t remaining = allies.size();
        for (const std::size_t index : allies) {
            Economy& to = economies[index];
            const Mag divisor = Mag::fromInt(static_cast<std::int64_t>(remaining));
            const auto offer = [divisor](Mag pool, Mag headroom) {
                if (pool <= Mag{} || headroom <= Mag{}) {
                    return Mag{};
                }
                const Mag share = Mag::fromRaw((pool.raw() << kFxFractionalBits)
                                               / divisor.raw());
                return std::min(share, headroom);
            };
            const Mag mass = offer(excess.mass,
                                   std::max(Mag{}, to.storage.mass - to.stored.mass));
            const Mag energy = offer(excess.energy,
                                     std::max(Mag{}, to.storage.energy - to.stored.energy));

            // Credited to INCOME, so it arrives next tick and is spendable rather than banked.
            to.sharedIn.mass += mass;
            to.sharedIn.energy += energy;
            excess.mass -= mass;
            excess.energy -= energy;
            --remaining;
        }

        // The giver keeps only its cap. Whatever no ally could hold is destroyed, which is
        // retail's outcome too — sharing reduces the waste, it does not remove it.
        from.stored.mass = std::min(from.stored.mass, from.storage.mass);
        from.stored.energy = std::min(from.stored.energy, from.storage.energy);
    }
}

std::vector<Construction> takeFinished(std::vector<Construction>& building) {
    std::vector<Construction> done;
    for (const Construction& work : building) {
        if (work.finished()) {
            done.push_back(work);
        }
    }
    std::erase_if(building, [](const Construction& work) { return work.finished(); });
    return done;
}

} // namespace rm::sim
