#include "core/sim/PathService.hpp"

#include "core/TaskPool.hpp"

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
    activeFields_.resize(std::max(activeFields_.size(), count));
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

void PathService::setBlocking(const PassabilityGrid& grid,
                              std::vector<std::uint8_t> blocked) {
    const std::uint64_t fingerprint = fingerprintOf(grid);
    std::vector<std::uint8_t>& previous = blocked_[fingerprint];
    if (previous == blocked) {
        return;
    }
    // The changed cells dirty a field only if its frontier ever reached them —
    // a placement far outside an explored region leaves that field's answers
    // untouched (ADR-035 layer 3's "region it touches"). The layer's length can
    // differ between calls — the first call has no predecessor — so a missing
    // slot reads as unblocked.
    const auto at = [](const std::vector<std::uint8_t>& layer, std::size_t cell) {
        return cell < layer.size() ? layer[cell] : std::uint8_t{0};
    };
    const std::size_t cells = std::max(previous.size(), blocked.size());
    for (auto it = fields_.begin(); it != fields_.end();) {
        if (it->first.grid != fingerprint) {
            ++it;
            continue;
        }
        bool dirty = false;
        for (std::size_t cell = 0; cell < cells && !dirty; ++cell) {
            dirty = at(previous, cell) != at(blocked, cell)
                    && it->second.field->touched(static_cast<int>(cell));
        }
        if (dirty) {
            it->second.field->markStale();
            it = fields_.erase(it);
        } else {
            ++it;
        }
    }
    previous = std::move(blocked);
}

void PathService::attach(std::size_t army) {
    if (!activeRequests_[army] && !pending_[army].empty()) {
        activeRequests_[army] = std::move(pending_[army].front());
        pending_[army].pop_front();
        retryWaits_[army] = 0;
        failureCounts_[army] = 0;
    }
    if (!activeRequests_[army]) {
        return;
    }
    const PathRequest& request = *activeRequests_[army];
    const PassabilityGrid& grid = *request.grid;
    const std::uint64_t fingerprint = fingerprintOf(grid);
    const Fx targetX = std::clamp(request.targetX, Fx{},
                                  Fx::fromInt(grid.cellsX) * grid.elmosPerCell);
    const Fx targetZ = std::clamp(request.targetZ, Fx{},
                                  Fx::fromInt(grid.cellsZ) * grid.elmosPerCell);
    const FieldKey key{.grid = fingerprint,
                       .goalX = grid.cellAtWorld(targetX),
                       .goalZ = grid.cellAtWorld(targetZ)};

    auto it = fields_.find(key);
    if (it == fields_.end()) {
        const auto blocked = blocked_.find(fingerprint);
        auto field = std::make_shared<FlowField>(
            request.grid, key.goalX, key.goalZ,
            blocked != blocked_.end() ? blocked->second : std::vector<std::uint8_t>{});
        ++fieldsCreated_;
        if (fields_.size() >= kFieldCacheLimit) {
            const auto oldest = std::min_element(
                fields_.begin(), fields_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            oldest->second.field->markStale();
            fields_.erase(oldest);
        }
        it = fields_.emplace(key, CachedField{.field = std::move(field), .used = 0}).first;
    }
    it->second.used = ++fieldClock_;
    activeFields_[army] = ActiveField{
        .field = it->second.field,
        .gridFingerprint = fingerprint,
        .goalX = key.goalX,
        .goalZ = key.goalZ,
        .startCell = grid.cellAtWorld(request.fromZ) * grid.cellsX
                   + grid.cellAtWorld(request.fromX),
    };
}

std::vector<PathResult> PathService::service() {
    std::vector<PathResult> completed;

    // BOOKKEEPING, serial and per army: promotion, retry waits and field
    // attachment are cheap state moves whose ORDER is the contract. A session
    // bound to a field a blocking change has since retired lets go of it and
    // re-attaches on a fresh build.
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        if (activeFields_[army] && activeFields_[army]->field->stale()) {
            activeFields_[army].reset();
        }
        if (activeRequests_[army] && !activeFields_[army] && retryWaits_[army] > 0) {
            --retryWaits_[army];
            // C-176's ten beats are all wait beats; the next service pass re-attaches.
            continue;
        }
        if (!activeFields_[army]
            && (!activeRequests_[army] || retryWaits_[army] == 0)) {
            attach(army);
        }
    }

    // THE EXPENSIVE PART, fork-joined (ADR-036/D15): each army's owed field
    // spends its beat allowance independently. Two armies may share a field, so
    // the work is deduplicated by POINTER — one step per field per beat — and a
    // `FlowField` is a private object, so per-field steps are per-object writes
    // the pool can reorder without ever reordering a result.
    std::vector<FlowField*> expanding;
    std::vector<std::vector<int>> targets;  // every start each field owes this beat
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        if (!activeFields_[army]) {
            continue;
        }
        ActiveField& active = *activeFields_[army];
        if (active.field->reach(active.startCell) != FlowField::Reach::Pending) {
            continue;
        }
        const auto found = std::find(expanding.begin(), expanding.end(), active.field.get());
        if (found == expanding.end()) {
            expanding.push_back(active.field.get());
            targets.push_back({active.startCell});
        } else {
            // A shared field owes every waiting start — the step stops only when
            // they are ALL settled.
            targets[static_cast<std::size_t>(found - expanding.begin())].push_back(
                active.startCell);
        }
    }
    rm::parallelFor(expanding.size(), [&expanding, &targets](std::size_t first,
                                                           std::size_t last) {
        for (std::size_t i = first; i < last; ++i) {
            expanding[i]->stepUntil(targets[i], kPathArmyBudget);
        }
    });

    // COLLECTION, serial and in army order: a beat may publish SEVERAL routes
    // when a shared field has already settled the next requester's start — the
    // drain loop, not the search, is what makes a formation click cheap.
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        while (activeRequests_[army] && activeFields_[army]) {
            ActiveField& active = *activeFields_[army];
            const FlowField::Reach reach = active.field->reach(active.startCell);
            if (reach == FlowField::Reach::Pending) {
                break;
            }
            const PathRequest request = *activeRequests_[army];
            if (reach == FlowField::Reach::Unreachable
                && failureCounts_[army] + 1 < kMaximumFailures) {
                ++failureCounts_[army];
                retryWaits_[army] = kRetryDelayBeats;
                activeFields_[army].reset();
                break;
            }
            completed.push_back(PathResult{
                .unit = request.unit,
                .command = request.command,
                .path = reach == FlowField::Reach::Reached
                            ? active.field->route(request.fromX, request.fromZ,
                                                  request.targetX, request.targetZ)
                            : std::vector<std::array<Fx, 2>>{},
            });
            activeRequests_[army].reset();
            activeFields_[army].reset();
            retryWaits_[army] = 0;
            failureCounts_[army] = 0;
            // The queue's next request may already be answered by this field or
            // another cached one — attach and let the loop decide.
            attach(army);
        }
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
            activeFields_[army].reset();
            retryWaits_[army] = 0;
            failureCounts_[army] = 0;
        }
    }
}

} // namespace rm::sim
