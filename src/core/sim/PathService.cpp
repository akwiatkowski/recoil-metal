#include "core/sim/PathService.hpp"

#include <algorithm>

namespace rm::sim {

void PathService::ensureArmy(int army) {
    if (army < 0) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(army) + 1;
    pending_.resize(std::max(pending_.size(), count));
    activeRequests_.resize(std::max(activeRequests_.size(), count));
    activeSearches_.resize(std::max(activeSearches_.size(), count));
}

void PathService::enqueue(PathRequest request) {
    if (request.army < 0 || request.grid == nullptr) {
        return;
    }
    ensureArmy(request.army);
    pending_[static_cast<std::size_t>(request.army)].push_back(std::move(request));
}

std::vector<PathResult> PathService::service() {
    std::vector<PathResult> completed;
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        if (!activeRequests_[army] && !pending_[army].empty()) {
            activeRequests_[army] = std::move(pending_[army].front());
            pending_[army].pop_front();
            const PathRequest& request = *activeRequests_[army];
            activeSearches_[army].emplace(request.grid, request.fromX, request.fromZ,
                                          request.targetX, request.targetZ);
        }
        if (!activeSearches_[army]) {
            continue;
        }
        activeSearches_[army]->step(kPathArmyBudget);
        if (!activeSearches_[army]->finished()) {
            continue;
        }
        const PathRequest request = *activeRequests_[army];
        completed.push_back(PathResult{.unit = request.unit,
                                       .command = request.command,
                                       .path = activeSearches_[army]->path()});
        activeRequests_[army].reset();
        activeSearches_[army].reset();
    }
    return completed;
}

bool PathService::contains(UnitId unit, CommandId command) const noexcept {
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        if (activeRequests_[army] && activeRequests_[army]->unit == unit
            && activeRequests_[army]->command == command) {
            return true;
        }
        for (const PathRequest& request : pending_[army]) {
            if (request.unit == unit && request.command == command) {
                return true;
            }
        }
    }
    return false;
}

} // namespace rm::sim
