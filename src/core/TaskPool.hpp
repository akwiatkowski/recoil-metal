#pragma once

#include <cstddef>
#include <functional>

namespace rm {

// The fork-join pool the tick's parallel passes run on (ADR-036, decision D15):
//
//     Parallelism may reorder WORK, never RESULTS.
//
// The pool decides WHEN work happens; it can never decide WHAT a result is,
// because the API has no way to return one — `parallelFor` hands a caller's
// function a half-open index range and nothing else. Each pass writes its own
// pre-sized slots and the caller combines them in slot order after the join,
// so the answer is identical at every pool size — and the determinism tests
// (`test_taskpool`, plus the hash-equality cases over `tickSkirmish`) are what
// would notice if it were not.
//
// Where the pool lives: `core`, not `core/sim`, because the same fork-join
// shape serves the scene-side draw gather, which is unsynced presentation and
// needs the discipline even less (ADR-036 ranks it third).

/// A fixed-size worker pool whose only verb is `run`.
///
/// `run` never returns while a chunk of its job is still executing — the join
/// is the function's exit. The CALLING thread works too, so a job pays for at
/// most `lanes - 1` wakeups and a lone-lane pool is a plain loop.
///
/// The function a job runs must not throw: an exception escaping a worker
/// would leave the completion count short and the join hanging, and the tick
/// code this serves does not throw anyway.
class TaskPool {
public:
    /// `workers` counts TOTAL lanes including the caller's; 0 and 1 both mean
    /// "run inline" — no threads are created, so a serial build pays nothing.
    explicit TaskPool(std::size_t workers);
    ~TaskPool();

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    /// Calls `fn(first, last)` once per contiguous chunk covering `[0, count)`.
    /// Chunk count never exceeds the lane count, so chunks stay coarse.
    void run(std::size_t count,
             const std::function<void(std::size_t first, std::size_t last)>& fn);

    [[nodiscard]] std::size_t workers() const noexcept { return lanes_; }

private:
    struct Impl;          // pImpl: threads and synchronisation stay out of the header
    Impl* impl_ = nullptr; // owned; null on a single-lane pool
    std::size_t lanes_ = 1;
};

/// The pool the sim's tick passes share.
///
/// Sized once at first use: the performance-core count (`hw.perflevel0.ncpu`),
/// falling back to `std::thread::hardware_concurrency`, or `RM_SIM_WORKERS`
/// when the environment sets it — `RM_SIM_WORKERS=1` is the serial A/B handle
/// the determinism harness and benchmarks both want.
[[nodiscard]] TaskPool& simPool();

/// Rebuilds the shared pool at `workers` lanes; 0 asks for the machine
/// default. A SETUP-TIME call, not a mid-tick one: resizing while a `run` is
/// in flight is a bug the caller owns, the same class as mutating match state
/// mid-tick. Tests use it to prove results are pool-size independent.
void setSimPoolSize(std::size_t workers);

/// Fork-join over `[0, count)`: `fn(first, last)` runs once per contiguous
/// chunk, on the shared pool's lanes plus the caller's, and returns only when
/// every index has been handed out and finished.
///
/// Inline — no pool involvement — when the range cannot split or the pool is
/// a single lane, so the serial answer is the default, not a special case.
void parallelFor(std::size_t count,
                 const std::function<void(std::size_t first, std::size_t last)>& fn);

} // namespace rm
