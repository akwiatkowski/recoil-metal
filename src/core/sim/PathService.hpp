#pragma once

#include "core/sim/Army.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/Pathfinding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
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

/// Match-owned, per-army FIFO path work over shared flow fields (ADR-035 layers 2-3).
///
/// Requests capture their issue-time start point and grid. The grid is immutable match terrain;
/// its address is deliberately not part of the authoritative state, while the search state is.
///
/// The unit of work is a FIELD, not a search: every request to a goal cell shares one
/// reverse-Dijkstra `FlowField` (keyed on the goal cell and the grid's content fingerprint,
/// since requests carry COPIES of the grid). A requester whose start cell the frontier has
/// already settled publishes the same beat it attaches — fifty shift-clicked movers to one
/// point pay one expansion instead of fifty searches.
class PathService {
public:
    /// One army's in-flight work: the request, the shared field serving it, and
    /// the start cell that field must settle before the route extracts.
    struct ActiveField {
        std::shared_ptr<FlowField> field;
        std::uint64_t gridFingerprint = 0;
        int goalX = 0;
        int goalZ = 0;
        int startCell = -1;
    };

    void enqueue(PathRequest request);

    /// Resumes the fields each army's head request is waiting on, within the
    /// fixed per-army expansion allowance, and publishes every route a field
    /// can now answer — one beat may complete several requests that share a goal.
    [[nodiscard]] std::vector<PathResult> service();

    /// Records which cells a grid's structures now stand on (ADR-035 layer 3).
    ///
    /// Called once per grid per tick, BEFORE `service`. A layer that differs
    /// from the last one retires exactly the cached fields whose frontier ever
    /// touched a changed cell; the rest keep serving.
    void setBlocking(const PassabilityGrid& grid, std::vector<std::uint8_t> blocked);

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
    [[nodiscard]] const std::vector<std::optional<ActiveField>>& activeFields() const noexcept {
        return activeFields_;
    }
    [[nodiscard]] const std::vector<std::size_t>& retryWaits() const noexcept {
        return retryWaits_;
    }
    [[nodiscard]] const std::vector<std::size_t>& failureCounts() const noexcept {
        return failureCounts_;
    }
    /// The tick whose service pass most recently completed. `advanceOrders` runs immediately
    /// afterwards, so this is the match tick for C-177's phase gate.
    [[nodiscard]] std::uint64_t lastServiceBeat() const noexcept {
        return serviceBeats_ == 0 ? 0 : serviceBeats_ - 1;
    }
    [[nodiscard]] std::uint64_t serviceBeats() const noexcept { return serviceBeats_; }
    /// Restores the match-owned beat clock before service resumes from a save state.
    void restoreServiceBeats(std::uint64_t beats) noexcept { serviceBeats_ = beats; }

    /// The service's authoritative state in serializable form (SaveState v37).
    /// Requests drop their grid pointer — the caller rebinds it on restore —
    /// and each cached field reduces to (grid fingerprint, goal, overlay,
    /// expansion count, stale flag): everything `FlowField` needs to replay
    /// its deterministic frontier back to the pre-save state.
    struct Snapshot {
        struct SavedRequest {
            UnitId unit{};
            CommandId command = kInvalidCommandId;
            int army = kNoArmy;
            Fx fromX{};
            Fx fromZ{};
            Fx targetX{};
            Fx targetZ{};
        };
        struct SavedField {
            std::uint64_t grid = 0;
            int goalX = 0;
            int goalZ = 0;
            std::uint64_t used = 0;
            std::size_t closed = 0;
            bool stale = false;
            std::vector<std::uint8_t> blocked;
        };
        struct SavedActiveField {
            int army = -1;
            std::uint64_t gridFingerprint = 0;
            int goalX = 0;
            int goalZ = 0;
            int startCell = -1;
        };
        std::vector<std::deque<SavedRequest>> admissions;
        std::vector<std::deque<SavedRequest>> pending;
        std::vector<std::optional<SavedRequest>> activeRequests;
        std::vector<SavedActiveField> activeFields;
        std::vector<std::size_t> retryWaits;
        std::vector<std::size_t> failureCounts;
        std::uint64_t serviceBeats = 0;
        std::vector<SavedField> fields;
        std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> blocked;
        std::uint64_t fieldClock = 0;
    };

    /// Captures every queue, counter, cache and in-flight field — the state
    /// `feedPathService` hashes, minus the grid pointers the caller rebinds.
    [[nodiscard]] Snapshot snapshot() const;

    /// Rebuilds the service from a snapshot. `gridFor` supplies each request's
    /// passability grid by unit — the same content the request snapshotted, so
    /// fingerprints match and restored fields keep sharing. Fields replay their
    /// recorded expansion count before any request attaches, reproducing the
    /// exact frontier the save interrupted.
    void restore(const Snapshot& state,
                 const std::function<std::shared_ptr<const PassabilityGrid>(UnitId)>& gridFor);

    /// How many fields the cache has ever built — the test hook for "N movers,
    /// one field".
    [[nodiscard]] std::size_t fieldsCreated() const noexcept { return fieldsCreated_; }

private:
    struct FieldKey {
        std::uint64_t grid = 0;
        int goalX = 0;
        int goalZ = 0;
        [[nodiscard]] constexpr auto operator<=>(const FieldKey&) const noexcept = default;
    };
    struct CachedField {
        std::shared_ptr<FlowField> field;
        std::uint64_t used = 0;  // LRU clock
    };

    void ensureArmy(int army);
    /// Promotes the head of an army's queue into `activeRequests_` and binds it
    /// to a cached or freshly-built field for its goal.
    void attach(std::size_t army);

    /// C-176: an unroutable move waits this many service beats before each re-path attempt.
    static constexpr std::size_t kRetryDelayBeats = 10;
    /// C-176: the third failed search publishes its empty route and retires the intent.
    static constexpr std::size_t kMaximumFailures = 3;
    /// Fields kept per service across goals and grids. Each is bounded by its
    /// grid's cell count; eight live goals per movement domain is generous for
    /// a match tick.
    static constexpr std::size_t kFieldCacheLimit = 8;

    // Commands accepted during this beat become eligible only after its service pass completes.
    std::vector<std::deque<PathRequest>> admissions_;
    std::vector<std::deque<PathRequest>> pending_;
    std::vector<std::optional<PathRequest>> activeRequests_;
    std::vector<std::optional<ActiveField>> activeFields_;
    std::vector<std::size_t> retryWaits_;
    std::vector<std::size_t> failureCounts_;
    std::uint64_t serviceBeats_ = 0;

    /// The layer-2 cache and the layer-3 overlay. Both keyed on grid content
    /// fingerprints — the requests' grids are private copies, so pointer
    /// identity would never share.
    std::map<FieldKey, CachedField> fields_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> blocked_;
    std::uint64_t fieldClock_ = 0;
    std::size_t fieldsCreated_ = 0;
};

} // namespace rm::sim
