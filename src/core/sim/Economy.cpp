#include "core/sim/Economy.hpp"

#include <algorithm>

namespace rm::sim {

Resources drainPerTick(const Construction& work) noexcept {
    // The EFFECTIVE rate — founder plus assisters — throughout: help makes the work drain
    // faster as well as finish sooner, which is what `BuildRate` means and why piling
    // engineers onto one build is a decision about the bank, not just the clock.
    // The last tick can need less work than the builders offer. Charging the full rate
    // there would bill for work that cannot happen after the remaining amount clamps to
    // zero, so demand and progress share the same capped rate.
    const Mag rate = std::min(work.effectiveBuildPerTick(), work.buildTimeRemaining);
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

    // UPKEEP FIRST, and unconditionally: what is standing costs what it costs whether or
    // not it can be paid for. An economy that cannot meet it simply has nothing left, which
    // is what a brownout is — and construction, funded from the remainder below, is what
    // visibly stops.
    economy.stored.mass = std::max(Mag{}, economy.stored.mass - economy.upkeepPerTick.mass);
    economy.stored.energy =
        std::max(Mag{}, economy.stored.energy - economy.upkeepPerTick.energy);

    // Pass one: what does everything want this tick?
    Resources wanted;
    for (const Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        wanted += drainPerTick(work);
    }

    // The DEMAND, remembered for whoever asks how loaded this economy is. Construction
    // plus upkeep — everything the tick tried to pay — which is what Moho's
    // GetEconomyRequested reports and what the FAF brain's efficiency conditions divide
    // income by. Derived from state the hash already covers, so it is deterministic
    // without being fed to the hash itself.
    economy.requestedLastTick = wanted + economy.upkeepPerTick;

    // Pass two: pay what can be paid, and let the shortfall slow EVERYTHING equally.
    // The ratio of what is banked to what was asked for, per resource, and the worse of the
    // two. `Mag / Mag -> Fx` again: a funding ratio is not a magnitude.
    Fx funded = kFxOne;
    if (wanted.mass > Mag{}) {
        funded = std::min(funded, Fx::fromRaw(saturate(
                                      (FxWide{economy.stored.mass.raw()} << kFxFractionalBits)
                                      / wanted.mass.raw())));
    }
    if (wanted.energy > Mag{}) {
        funded = std::min(funded, Fx::fromRaw(saturate(
                                      (FxWide{economy.stored.energy.raw()} << kFxFractionalBits)
                                      / wanted.energy.raw())));
    }
    funded = std::clamp(funded, Fx{}, kFxOne);
    economy.fundedFraction = funded;

    economy.stored.mass -= wanted.mass * funded;
    economy.stored.energy -= wanted.energy * funded;
    // Capacity is applied AFTER spending. Excess is still lost here because allied sharing is
    // not implemented; retail offers it to allies before this same final clamp (`C-163`).
    // The zero floor also catches fixed-point ratio rounding that overshot by a raw step.
    economy.stored.mass = std::max(Mag{}, std::min(economy.stored.mass, economy.storage.mass));
    economy.stored.energy =
        std::max(Mag{}, std::min(economy.stored.energy, economy.storage.energy));

    for (Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        const Mag rate = std::min(work.effectiveBuildPerTick(), work.buildTimeRemaining);
        work.buildTimeRemaining -= rate * funded;
        work.buildTimeRemaining = std::max(Mag{}, work.buildTimeRemaining);
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
