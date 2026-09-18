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

namespace {

[[nodiscard]] PathService::Snapshot::SavedRequest saveRequest(
    const PathRequest& request) {
    return {.unit = request.unit,
            .command = request.command,
            .army = request.army,
            .fromX = request.fromX,
            .fromZ = request.fromZ,
            .targetX = request.targetX,
            .targetZ = request.targetZ};
}

[[nodiscard]] PathRequest loadRequest(
    const PathService::Snapshot::SavedRequest& saved,
    const std::function<std::shared_ptr<const PassabilityGrid>(UnitId)>& gridFor) {
    return {.unit = saved.unit,
            .command = saved.command,
            .army = saved.army,
            .fromX = saved.fromX,
            .fromZ = saved.fromZ,
            .targetX = saved.targetX,
            .targetZ = saved.targetZ,
            .grid = gridFor(saved.unit)};
}

} // namespace

PathService::Snapshot PathService::snapshot() const {
    Snapshot state;
    state.admissions.resize(admissions_.size());
    for (std::size_t army = 0; army < admissions_.size(); ++army) {
        for (const PathRequest& request : admissions_[army]) {
            state.admissions[army].push_back(saveRequest(request));
        }
    }
    state.pending.resize(pending_.size());
    for (std::size_t army = 0; army < pending_.size(); ++army) {
        for (const PathRequest& request : pending_[army]) {
            state.pending[army].push_back(saveRequest(request));
        }
    }
    state.activeRequests.resize(activeRequests_.size());
    for (std::size_t army = 0; army < activeRequests_.size(); ++army) {
        if (activeRequests_[army]) {
            state.activeRequests[army] = saveRequest(*activeRequests_[army]);
        }
    }
    for (std::size_t army = 0; army < activeFields_.size(); ++army) {
        if (activeFields_[army]) {
            const ActiveField& active = *activeFields_[army];
            state.activeFields.push_back({.army = static_cast<int>(army),
                                          .gridFingerprint = active.gridFingerprint,
                                          .goalX = active.goalX,
                                          .goalZ = active.goalZ,
                                          .startCell = active.startCell});
        }
    }
    state.retryWaits = retryWaits_;
    state.failureCounts = failureCounts_;
    state.serviceBeats = serviceBeats_;
    for (const auto& [key, cached] : fields_) {
        state.fields.push_back({.grid = key.grid,
                                .goalX = key.goalX,
                                .goalZ = key.goalZ,
                                .used = cached.used,
                                .closed = cached.field->closedCount(),
                                .stale = cached.field->stale(),
                                .blocked = cached.field->blocked()});
    }
    state.blocked.assign(blocked_.begin(), blocked_.end());
    state.fieldClock = fieldClock_;
    return state;
}

void PathService::restore(
    const Snapshot& state,
    const std::function<std::shared_ptr<const PassabilityGrid>(UnitId)>& gridFor) {
    admissions_.clear();
    pending_.clear();
    activeRequests_.clear();
    activeFields_.clear();
    retryWaits_.clear();
    failureCounts_.clear();
    fields_.clear();
    blocked_.clear();

    admissions_.resize(state.admissions.size());
    for (std::size_t army = 0; army < state.admissions.size(); ++army) {
        for (const auto& saved : state.admissions[army]) {
            admissions_[army].push_back(loadRequest(saved, gridFor));
        }
    }
    pending_.resize(state.pending.size());
    for (std::size_t army = 0; army < state.pending.size(); ++army) {
        for (const auto& saved : state.pending[army]) {
            pending_[army].push_back(loadRequest(saved, gridFor));
        }
    }
    activeRequests_.resize(state.activeRequests.size());
    for (std::size_t army = 0; army < state.activeRequests.size(); ++army) {
        if (state.activeRequests[army]) {
            activeRequests_[army] = loadRequest(*state.activeRequests[army], gridFor);
        }
    }
    retryWaits_ = state.retryWaits;
    failureCounts_ = state.failureCounts;
    serviceBeats_ = state.serviceBeats;
    fieldClock_ = state.fieldClock;
    blocked_.insert(state.blocked.begin(), state.blocked.end());
    // `ensureArmy` keeps all six vectors at max-army+1; a snapshot taken after
    // a high-numbered army drained still carries that width, so size from the
    // widest restored queue rather than any single one.
    const std::size_t armies =
        std::max({admissions_.size(), pending_.size(), activeRequests_.size(),
                  retryWaits_.size(), failureCounts_.size()});
    activeFields_.resize(armies);
    retryWaits_.resize(std::max(retryWaits_.size(), armies));
    failureCounts_.resize(std::max(failureCounts_.size(), armies));

    // Rebuild every cached field first: each replays its recorded expansion
    // count, which reproduces the exact frontier — the pop order is a pure
    // function of (grid, goal, overlay). Active sessions then re-point at the
    // rebuilt field by the same (fingerprint, goal) key `attach` computes.
    std::map<FieldKey, std::shared_ptr<FlowField>> rebuilt;
    for (const Snapshot::SavedField& saved : state.fields) {
        // The grid any request on this field carried: fields only exist for
        // goals a request asked for, so one is always findable — fall back to
        // the first request's grid when the field outlived its requesters.
        std::shared_ptr<const PassabilityGrid> grid;
        for (const auto& queue : pending_) {
            for (const PathRequest& request : queue) {
                if (request.grid && fingerprintOf(*request.grid) == saved.grid) {
                    grid = request.grid;
                }
            }
        }
        for (const auto& request : activeRequests_) {
            if (!grid && request && request->grid
                && fingerprintOf(*request->grid) == saved.grid) {
                grid = request->grid;
            }
        }
        for (const auto& queue : admissions_) {
            for (const PathRequest& request : queue) {
                if (!grid && request.grid && fingerprintOf(*request.grid) == saved.grid) {
                    grid = request.grid;
                }
            }
        }
        if (!grid) {
            continue;  // no requester left to rebind — the field is dead weight
        }
        auto field = std::make_shared<FlowField>(grid, saved.goalX, saved.goalZ,
                                                 saved.blocked);
        field->stepToClosedCount(saved.closed);
        if (saved.stale) {
            field->markStale();
        }
        const FieldKey key{.grid = saved.grid, .goalX = saved.goalX, .goalZ = saved.goalZ};
        fields_.emplace(key, CachedField{.field = field, .used = saved.used});
        rebuilt.emplace(key, std::move(field));
    }

    activeFields_.resize(pending_.size());
    for (const Snapshot::SavedActiveField& saved : state.activeFields) {
        if (saved.army < 0 || static_cast<std::size_t>(saved.army) >= activeFields_.size()) {
            continue;
        }
        const FieldKey key{.grid = saved.gridFingerprint,
                           .goalX = saved.goalX,
                           .goalZ = saved.goalZ};
        const auto found = rebuilt.find(key);
        if (found == rebuilt.end()) {
            continue;  // its field was dropped above — the session re-attaches
        }
        activeFields_[static_cast<std::size_t>(saved.army)] = ActiveField{
            .field = found->second,
            .gridFingerprint = saved.gridFingerprint,
            .goalX = saved.goalX,
            .goalZ = saved.goalZ,
            .startCell = saved.startCell};
    }
}

} // namespace rm::sim
