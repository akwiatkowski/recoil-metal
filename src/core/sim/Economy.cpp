#include "core/sim/Economy.hpp"

#include <algorithm>

namespace rm::sim {

Resources drainPerTick(const Construction& work) noexcept {
    if (work.totalBuildTime <= Mag{} || work.buildPerTick <= Mag{}) {
        return Resources{};
    }
    // TICKS the whole build will take at this rate, and the cost spread over them. The
    // per-second version of this divided by seconds; the arithmetic is the same shape, with
    // the unit of time already folded into `buildPerTick`.
    //
    // The division is `Mag / Mag -> Fx`, done explicitly: the share of the total cost that
    // falls in one tick is a ratio, which is what the geometric type holds.
    const Fx sharePerTick = Fx::fromRaw(saturate(
        (FxWide{work.buildPerTick.raw()} << kFxFractionalBits) / work.totalBuildTime.raw()));
    return work.cost * sharePerTick;
}

void tickEconomy(Economy& economy, std::span<Construction> building) {
    // Income first, so a tick's earnings are spendable in the same tick. NO MULTIPLY BY A
    // TICK LENGTH: the income is already per tick, converted once when the catalog learned
    // the type (§5.1). `kTickSeconds` used to appear four times in this function and now
    // appears nowhere, which is the rule being satisfied rather than described.
    economy.stored += economy.incomePerTick;

    // Storage is a cap and overflow is LOST, which is what the game does — an economy with
    // nothing to spend on is wasting, and that is the pressure to build something.
    economy.stored.mass = std::min(economy.stored.mass, economy.storage.mass);
    economy.stored.energy = std::min(economy.stored.energy, economy.storage.energy);

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
    // Still clamped at zero. Fixed point cannot leave "a hair below zero" the way float
    // could, but rounding in the funding ratio can still overshoot by a step or two, and a
    // negative store would make the next tick's ratio negative and run every build backwards.
    economy.stored.mass = std::max(Mag{}, economy.stored.mass);
    economy.stored.energy = std::max(Mag{}, economy.stored.energy);

    for (Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        work.buildTimeRemaining -= work.buildPerTick * funded;
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
