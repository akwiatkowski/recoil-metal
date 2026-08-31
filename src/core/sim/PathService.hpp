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

    [[nodiscard]] const std::vector<std::deque<PathRequest>>& pending() const noexcept {
        return pending_;
    }
    [[nodiscard]] const std::vector<std::optional<PathRequest>>& activeRequests() const noexcept {
        return activeRequests_;
    }
    [[nodiscard]] const std::vector<std::optional<PathSearch>>& activeSearches() const noexcept {
        return activeSearches_;
    }

private:
    void ensureArmy(int army);

    std::vector<std::deque<PathRequest>> pending_;
    std::vector<std::optional<PathRequest>> activeRequests_;
    std::vector<std::optional<PathSearch>> activeSearches_;
};

} // namespace rm::sim
