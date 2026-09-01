#pragma once

#include "core/sim/Army.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Pathfinding.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace rm::sim {

/// Retail's fixed work allowance for one army's path service on one beat (C-174).
inline constexpr std::size_t kPathArmyBudget = 1000;

/// One accepted move awaiting the owning army's path service.
struct PathRequest {
    UnitId unit{};
    CommandId command = kInvalidCommandId;
    int army = kNoArmy;
    Fx fromX{};
    Fx fromZ{};
    Fx targetX{};
    Fx targetZ{};
    /// Snapshot at intake: resolver-owned grids may be temporary, while this request spans ticks.
    std::shared_ptr<const PassabilityGrid> grid;
};

/// A completed request. An empty path means that the current solver found no route.
struct PathResult {
    UnitId unit{};
    CommandId command = kInvalidCommandId;
    std::vector<std::array<Fx, 2>> path;
};

/// Match-owned, per-army FIFO path work.
///
/// Requests capture their issue-time start point and grid. The grid is immutable match terrain;
/// its address is deliberately not part of the authoritative state, while the search state is.
class PathService {
public:
    void enqueue(PathRequest request);

    /// Resumes at most one search per army for its fixed expansion allowance.
    [[nodiscard]] std::vector<PathResult> service();

    /// Whether a command still owns queued or active path work.
    [[nodiscard]] bool contains(UnitId unit, CommandId command) const noexcept;

    /// Removes all outstanding path work for a unit whose current order was replaced or stopped.
    void cancel(UnitId unit);

    [[nodiscard]] const std::vector<std::deque<PathRequest>>& pending() const noexcept {
        return pending_;
    }
    [[nodiscard]] const std::vector<std::deque<PathRequest>>& admissions() const noexcept {
        return admissions_;
    }
    [[nodiscard]] const std::vector<std::optional<PathRequest>>& activeRequests() const noexcept {
        return activeRequests_;
    }
    [[nodiscard]] const std::vector<std::optional<PathSearch>>& activeSearches() const noexcept {
        return activeSearches_;
    }
    [[nodiscard]] const std::vector<std::size_t>& retryWaits() const noexcept {
        return retryWaits_;
    }
    [[nodiscard]] const std::vector<std::size_t>& failureCounts() const noexcept {
        return failureCounts_;
    }

private:
    void ensureArmy(int army);

    /// C-176: an unroutable move waits this many service beats before each re-path attempt.
    static constexpr std::size_t kRetryDelayBeats = 10;
    /// C-176: the third failed search publishes its empty route and retires the intent.
    static constexpr std::size_t kMaximumFailures = 3;

    // Commands accepted during this beat become eligible only after its service pass completes.
    std::vector<std::deque<PathRequest>> admissions_;
    std::vector<std::deque<PathRequest>> pending_;
    std::vector<std::optional<PathRequest>> activeRequests_;
    std::vector<std::optional<PathSearch>> activeSearches_;
    std::vector<std::size_t> retryWaits_;
    std::vector<std::size_t> failureCounts_;
};

} // namespace rm::sim
