#include "core/sim/Economy.hpp"

#include <algorithm>

namespace rm::sim {

Resources drainPerSecond(const Construction& work) noexcept {
    if (work.totalBuildTime <= 0.0f || work.buildRate <= 0.0f) {
        return Resources{};
    }
    // Seconds the whole build will take at this rate, and the cost spread over them.
    const float seconds = work.totalBuildTime / work.buildRate;
    if (seconds <= 0.0f) {
        return Resources{};
    }
    return Resources{.mass = work.cost.mass / seconds, .energy = work.cost.energy / seconds};
}

void tickEconomy(Economy& economy, std::span<Construction> building) {
    // Income first, so a tick's earnings are spendable in the same tick.
    economy.stored.mass += economy.incomePerSecond.mass * kTickSeconds;
    economy.stored.energy += economy.incomePerSecond.energy * kTickSeconds;

    // Storage is a cap and overflow is LOST, which is what the game does — an economy with
    // nothing to spend on is wasting, and that is the pressure to build something.
    economy.stored.mass = std::min(economy.stored.mass, economy.storage.mass);
    economy.stored.energy = std::min(economy.stored.energy, economy.storage.energy);

    // UPKEEP FIRST, and unconditionally: what is standing costs what it costs whether or
    // not it can be paid for. An economy that cannot meet it simply has nothing left, which
    // is what a brownout is — and construction, funded from the remainder below, is what
    // visibly stops.
    economy.stored.mass =
        std::max(0.0f, economy.stored.mass - economy.upkeepPerSecond.mass * kTickSeconds);
    economy.stored.energy =
        std::max(0.0f, economy.stored.energy - economy.upkeepPerSecond.energy * kTickSeconds);

    // Pass one: what does everything want this tick?
    Resources wanted;
    for (const Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        const Resources rate = drainPerSecond(work);
        wanted.mass += rate.mass * kTickSeconds;
        wanted.energy += rate.energy * kTickSeconds;
    }

    // Pass two: pay what can be paid, and let the shortfall slow EVERYTHING equally.
    float funded = 1.0f;
    if (wanted.mass > 0.0f) {
        funded = std::min(funded, economy.stored.mass / wanted.mass);
    }
    if (wanted.energy > 0.0f) {
        funded = std::min(funded, economy.stored.energy / wanted.energy);
    }
    funded = std::clamp(funded, 0.0f, 1.0f);
    economy.fundedFraction = funded;

    economy.stored.mass -= wanted.mass * funded;
    economy.stored.energy -= wanted.energy * funded;
    // Floating point can leave a hair below zero after the subtraction, and a negative
    // store would make the next tick's ratio negative and run every build backwards.
    economy.stored.mass = std::max(0.0f, economy.stored.mass);
    economy.stored.energy = std::max(0.0f, economy.stored.energy);

    for (Construction& work : building) {
        if (work.finished()) {
            continue;
        }
        work.buildTimeRemaining -= work.buildRate * kTickSeconds * funded;
        work.buildTimeRemaining = std::max(0.0f, work.buildTimeRemaining);
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
