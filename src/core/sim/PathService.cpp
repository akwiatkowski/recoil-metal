#include "core/sim/PathService.hpp"

#include <algorithm>
#include <iterator>

namespace rm::sim {

void PathService::ensureArmy(int army) {
    if (army < 0) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(army) + 1;
    admissions_.resize(std::max(admissions_.size(), count));
    pending_.resize(std::max(pending_.size(), count));
    activeRequests_.resize(std::max(activeRequests_.size(), count));
    activeSearches_.resize(std::max(activeSearches_.size(), count));
    retryWaits_.resize(std::max(retryWaits_.size(), count));
    failureCounts_.resize(std::max(failureCounts_.size(), count));
}

void PathService::enqueue(PathRequest request) {
    if (request.army < 0 || request.grid == nullptr) {
        return;
    }
    ensureArmy(request.army);
    admissions_[static_cast<std::size_t>(request.army)].push_back(std::move(request));
}

std::vector<PathResult> PathService::service() {
    std::vector<PathResult> completed;
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        if (activeRequests_[army] && !activeSearches_[army] && retryWaits_[army] > 0) {
            --retryWaits_[army];
            // C-176's ten beats are all wait beats; the next service pass starts the re-path.
            continue;
        }
        if (!activeRequests_[army] && !pending_[army].empty()) {
            activeRequests_[army] = std::move(pending_[army].front());
            pending_[army].pop_front();
            retryWaits_[army] = 0;
            failureCounts_[army] = 0;
        }
        if (activeRequests_[army] && !activeSearches_[army]) {
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
        if (activeSearches_[army]->path().empty()
            && failureCounts_[army] + 1 < kMaximumFailures) {
            ++failureCounts_[army];
            retryWaits_[army] = kRetryDelayBeats;
            activeSearches_[army].reset();
            continue;
        }
        completed.push_back(PathResult{.unit = request.unit,
                                        .command = request.command,
                                        .path = activeSearches_[army]->path()});
        activeRequests_[army].reset();
        activeSearches_[army].reset();
        retryWaits_[army] = 0;
        failureCounts_[army] = 0;
    }

    // Admission is deliberately after every army has spent this beat's allowance. A request
    // accepted during the beat cannot consume its issuing beat's path work budget.
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        std::deque<PathRequest>& admissions = admissions_[army];
        pending_[army].insert(pending_[army].end(), std::make_move_iterator(admissions.begin()),
                               std::make_move_iterator(admissions.end()));
        admissions.clear();
    }
    ++serviceBeats_;
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
        for (const PathRequest& request : admissions_[army]) {
            if (request.unit == unit && request.command == command) {
                return true;
            }
        }
    }
    return false;
}

void PathService::cancel(UnitId unit) {
    const auto belongsTo = [unit](const PathRequest& request) { return request.unit == unit; };
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        std::erase_if(admissions_[army], belongsTo);
        std::erase_if(pending_[army], belongsTo);
        if (activeRequests_[army] && belongsTo(*activeRequests_[army])) {
            activeRequests_[army].reset();
            activeSearches_[army].reset();
            retryWaits_[army] = 0;
            failureCounts_[army] = 0;
        }
    }
}

} // namespace rm::sim
