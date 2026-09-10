#include "core/sim/Economy.hpp"
#include "core/sim/Capture.hpp"

#include <algorithm>

namespace rm::sim {

Resources drainPerTick(const EnhancementWork& work) noexcept {
    if (work.paused || work.finished() || work.totalBuildTime <= Mag{} || work.buildPerTick <= Mag{}) {
        return {};
    }
    const Fx fraction = Fx::fromRaw(saturate(
        (FxWide{work.buildPerTick.raw()} << kFxFractionalBits) / work.totalBuildTime.raw()));
    return work.cost * fraction;
}

void advanceEnhancement(EnhancementWork& work) noexcept {
    if (work.paused || work.finished()) return;
    // EnhanceTask.lua:56–60. Completion stops active consumption before the next economy pass.
    work.buildTimeRemaining = std::max(Mag{}, work.buildTimeRemaining
        - std::max(Mag{}, work.buildPerTick) * work.fundedLastTick);
}

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

/// Whether this beat's economy still has to pay for a construction.
///
/// Unfinished work always does. Work that finished THIS beat does too, on its stamp: retail
/// bills the builder for the full delta it offered and never reconciles that against the
/// smaller amount `Materialize` was able to apply (`C-100`, `C-187`), so the completing beat
/// is charged in full. Work that finished on an EARLIER beat is neither stamped nor unfinished
/// and drops out here, which is what stops a completed record billing forever.
[[nodiscard]] bool stillBilled(const Construction& work) noexcept {
    return !work.finished() || work.workedThisTick;
}

} // namespace

SiloAmmo makeSiloAmmo(UnitId owner, std::size_t weapon, bool nukeWeapon, int capacity,
                      Resources projectileCost, Mag buildTime, Mag buildPerTick) noexcept {
    const Mag pace = std::max(buildPerTick, Mag::fromRaw(1));
    const TickCount total = static_cast<TickCount>(buildTime.raw() / pace.raw());
    if (total == 0) return SiloAmmo{.owner = owner, .weapon = weapon,
                                    .slot = static_cast<std::uint8_t>(nukeWeapon), .capacity = capacity};
    return SiloAmmo{.owner = owner,
                    .weapon = weapon,
                    .slot = static_cast<std::uint8_t>(nukeWeapon),
                    .capacity = capacity,
                    .totalTicks = total,
                    .costPerTick = {.mass = Mag::fromRaw(projectileCost.mass.raw() / total),
                                    .energy = Mag::fromRaw(projectileCost.energy.raw() / total)}};
}

MissileRedirect makeMissileRedirect(UnitId owner, Fx radiusElmos,
                                     int cooldownTicks) noexcept {
    return MissileRedirect{.owner = owner, .radiusElmos = radiusElmos,
                           .cooldownTicks = cooldownTicks};
}

void advanceConstruction(Construction& work) noexcept {
    if (work.finished()) {
        return;
    }
    // Progress on LAST beat's funded fraction (`C-162`). Retail writes the ratio onto the
    // builder in the motion stage, which runs last, and reads it here in command dispatch,
    // which runs first (`C-142`) — so a builder always advances on the previous beat's
    // funding. Using the fraction the economy is about to compute would be a one-tick head
    // start the engine does not give, and it is the difference `C-100` measured.
    const Mag progress = work.effectiveBuildPerTick() * work.fundedLastTick;
    work.buildTimeRemaining -= progress;
    work.buildTimeRemaining = std::max(Mag{}, work.buildTimeRemaining);
    // Stamped whether or not any progress was possible — retail's `Materialize(0.0f)`
    // heartbeat writes `Entity+0x520` on a beat that moves nothing (`C-187`, `C-098`).
    work.workedThisTick = true;
    work.advancedLastTick = progress > Mag{};
}

void tickEconomy(Economy& economy, std::span<Construction> building,
                  std::span<RepairWork> repairs, std::span<SiloAmmo> siloAmmo, bool deferOverflow,
                  std::span<UnitResourceFlow> flows, int armyIndex,
                  std::span<EnhancementWork> enhancements, std::span<CaptureWork> captures) {
    // Clamp only what CARRIED IN. Reclaim currently credits `stored` directly before this
    // pass, so its over-cap excess is still lost rather than becoming a hidden reserve.
    economy.stored.mass = std::max(Mag{}, std::min(economy.stored.mass, economy.storage.mass));
    economy.stored.energy =
        std::max(Mag{}, std::min(economy.stored.energy, economy.storage.energy));

    // Income then becomes spendable before the final capacity clamp, as retail's allocator
    // does (`C-163`). A full bank can therefore pay one tick's bill from one tick's income
    // and remain full. NO MULTIPLY BY A TICK LENGTH: this is already a per-tick rate (§5.1).
    economy.stored += economy.incomePerTick;
    economy.generatedLifetime += economy.incomePerTick;
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
        if (!stillBilled(work)) {
            continue;
        }
        const Resources demand = drainPerTick(work);
        wanted += demand;
        bucket(outstanding(demand, work.allocated));
    }
    for (const RepairWork& repair : repairs) {
        wanted += repair.demand;
        bucket(repair.demand);
    }
    for (const CaptureWork& work : captures) {
        wanted += work.demand;
        bucket(work.demand);
    }
    for (const EnhancementWork& work : enhancements) {
        const Resources demand = drainPerTick(work);
        wanted += demand;
        bucket(outstanding(demand, work.allocated));
    }
    const auto autoBuilding = [&siloAmmo](const SiloAmmo& ammo) {
        if (!ammo.building()) return false;
        // C-241 starts tactical first; nuke is considered only when tactical could not queue.
        return ammo.slot == 0 || std::none_of(siloAmmo.begin(), siloAmmo.end(),
            [&ammo](const SiloAmmo& other) { return other.owner == ammo.owner && other.slot == 0
                                                      && other.building(); });
    };
    for (const SiloAmmo& ammo : siloAmmo) {
        if (autoBuilding(ammo)) {
            wanted += ammo.costPerTick;
            bucket(outstanding(ammo.costPerTick, ammo.delivered));
        }
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
    const auto recordCharge = [flows, armyIndex](UnitId unit, Resources charge) {
        if (unit.index < flows.size() && flows[unit.index].unit == unit
            && flows[unit.index].armyIndex == armyIndex) {
            flows[unit.index].usageLastTick += charge;
        }
    };
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
    // The allocator has one aggregate upkeep request. Attribute its actual charge
    // proportionally to the units that submitted it, preserving the final residue.
    Resources remainingCharge = granted;
    Resources remainingDemand = economy.upkeepPerTick;
    for (auto& flow : flows) {
        if (flow.armyIndex != armyIndex) continue;
        const Resources share{
            .mass = flow.upkeepPerTick.mass == remainingDemand.mass ? remainingCharge.mass
                : remainingCharge.mass * fundingRatio(flow.upkeepPerTick.mass, remainingDemand.mass),
            .energy = flow.upkeepPerTick.energy == remainingDemand.energy ? remainingCharge.energy
                : remainingCharge.energy * fundingRatio(flow.upkeepPerTick.energy, remainingDemand.energy),
        };
        flow.usageLastTick += share;
        remainingCharge.mass -= share.mass;
        remainingCharge.energy -= share.energy;
        remainingDemand.mass -= flow.upkeepPerTick.mass;
        remainingDemand.energy -= flow.upkeepPerTick.energy;
    }

    for (Construction& work : building) {
        if (!stillBilled(work)) {
            continue;
        }
        // The request stays deliberately uncapped even on the completing tick: retail does not
        // reconcile it with what `Materialize` applies (`C-100`, `C-187`).
        //
        // THE PROGRESS ITSELF ALREADY HAPPENED, at the start of the beat in the command-dispatch
        // stage (`advanceConstruction`). All that is left here is the bill and the ratio the
        // NEXT beat's progress will be multiplied by — retail's split across two stages exactly.
        const Resources before = granted;
        work.fundedLastTick = grantAndConsume(drainPerTick(work), work.allocated);
        recordCharge(work.builder, {.mass = granted.mass - before.mass,
                                   .energy = granted.energy - before.energy});
        work.workedThisTick = false;
    }
    for (EnhancementWork& work : enhancements) {
        if (work.paused || work.finished()) {
            work.fundedLastTick = Fx{};
            continue;
        }
        const Resources before = granted;
        work.fundedLastTick = grantAndConsume(drainPerTick(work), work.allocated);
        recordCharge(work.owner, {.mass = granted.mass - before.mass,
                                 .energy = granted.energy - before.energy});
    }
    for (RepairWork& repair : repairs) {
        // Repairs have no carry-forward allocation: their live target can be healed, filled,
        // or destroyed before the next tick, so this request is consumed in the award beat.
        Resources allocated;
        const Resources before = granted;
        repair.funded = grantAndConsume(repair.demand, allocated);
        if (repair.builder < flows.size()) {
            recordCharge(flows[repair.builder].unit, {.mass = granted.mass - before.mass,
                                                     .energy = granted.energy - before.energy});
        }
    }
    for (CaptureWork& work : captures) {
        // Like repairs, captures carry no allocation forward: the award beat funds
        // this beat's demand or nothing, and the apply pass advances only fully
        // funded beats. `funded` is the ratio the next progress step reads.
        Resources allocated;
        const Resources before = granted;
        work.funded = grantAndConsume(work.demand, allocated);
        if (work.captor < flows.size()) {
            recordCharge(flows[work.captor].unit, {.mass = granted.mass - before.mass,
                                                  .energy = granted.energy - before.energy});
        }
    }
    for (SiloAmmo& ammo : siloAmmo) {
        if (!autoBuilding(ammo)) {
            continue;
        }
        // C-084: this is an event delivery accumulator, not Construction's lagged ratio.
        // A partial award remains here until an entire production beat is affordable.
        const Resources out = outstanding(ammo.costPerTick, ammo.delivered);
        const Fx ratio = grantFor(out);
        const Resources share = out * ratio;
        ammo.delivered += share;
        recordCharge(ammo.owner, share);
        granted += share;
        if (ammo.delivered.mass >= ammo.costPerTick.mass
            && ammo.delivered.energy >= ammo.costPerTick.energy) {
            ammo.delivered = {};
            ++ammo.elapsedTicks;
            if (ammo.elapsedTicks >= ammo.totalTicks) {
                ++ammo.stored;
                ammo.elapsedTicks = 0;
            }
        }
    }

    economy.usageLastTick = granted;
    economy.stored.mass = std::max(Mag{}, economy.stored.mass - granted.mass);
    economy.stored.energy = std::max(Mag{}, economy.stored.energy - granted.energy);
    // A whole match offers this beat's excess to allies before applying capacity (`C-163`).
    // Standalone economy callers retain the final clamp here.
    if (deferOverflow) return;
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

    }
    // Non-sharing armies must discard excess too; the tick deferred capacity for everyone.
    for (auto& economy : economies) {
        economy.stored.mass = std::max(Mag{}, std::min(economy.stored.mass, economy.storage.mass));
        economy.stored.energy = std::max(Mag{}, std::min(economy.stored.energy, economy.storage.energy));
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
