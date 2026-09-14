#include "core/TaskPool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace rm {

/// One pool generation's shared job state. A generation hands out fixed chunks
/// by atomic index; every lane — workers and the caller alike — pulls from the
/// same counter, so a finished chunk is a finished chunk whichever lane took it.
struct TaskPool::Impl {
    explicit Impl(std::size_t lanes) {
        threads.reserve(lanes > 0 ? lanes - 1 : 0);
        for (std::size_t lane = 1; lane < lanes; ++lane) {
            threads.emplace_back([this] { workerLoop(); });
        }
    }

    ~Impl() {
        {
            const std::lock_guard lock{mutex};
            stopping = true;
            ++generation;  // wakes every waiter for the exit check
        }
        cv.notify_all();
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    /// Hands chunks of the live job to whichever lane calls — a worker on
    /// wake, or the submitting thread inside `run`. Returns when the job is
    /// drained; the job is COMPLETE only when `finished` reaches `chunks`,
    /// which `run` waits on.
    ///
    /// A lane enters here only while holding a claim on `inFlight` (see
    /// `run`), and a claim pins the generation: the fields it reads below —
    /// `job`, `chunks`, `count`, `chunkSize` — cannot be overwritten while any
    /// claim is held, so the job pointer it calls is always the live one.
    void drainJob() noexcept {
        for (;;) {
            const std::size_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) {
                break;
            }
            const std::size_t first = chunk * chunkSize;
            const std::size_t last = std::min(first + chunkSize, count);
            (*job)(first, last);
            finished.fetch_add(1, std::memory_order_release);
        }
    }

    void run(std::size_t n,
             const std::function<void(std::size_t, std::size_t)>& fn) noexcept {
        // BEFORE publishing: no lane may still hold a view of the previous
        // job's fields when they are overwritten. A worker that observed a
        // generation counts itself under the lock before leaving the wait —
        // and a worker that has NOT woken yet cannot pass the predicate until
        // the generation bump below — so a zero here means nobody anywhere is
        // between the wait and the drain with a stale job pointer. Without
        // this barrier a preempted worker could wake into fields mid-rewrite:
        // the old (by then dead) function called with the new range.
        while (inFlight.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        {
            const std::lock_guard lock{mutex};
            job = &fn;
            count = n;
            chunks = std::min(threads.size() + 1, n);
            chunkSize = (n + chunks - 1) / chunks;
            nextChunk.store(0, std::memory_order_relaxed);
            finished.store(0, std::memory_order_relaxed);
            ++generation;  // release: the job is fully described before this lands
        }
        cv.notify_all();
        inFlight.fetch_add(1, std::memory_order_acq_rel);
        drainJob();
        inFlight.fetch_sub(1, std::memory_order_acq_rel);
        // The caller's own drain returns when chunks run out, which can be
        // before the last one finishes on another lane — wait for it here, and
        // for every lane to have released its claim, so the next `run`'s
        // pre-publish barrier sees a drained job rather than a live one.
        while (finished.load(std::memory_order_acquire) != chunks
               || inFlight.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    void workerLoop() noexcept {
        std::size_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] { return generation != seen; });
                seen = generation;
                if (stopping) {
                    return;
                }
                // The claim is taken UNDER the lock, before the lane can read
                // a single job field — that ordering is what makes the
                // publisher's `inFlight == 0` barrier mean "no stale readers".
                inFlight.fetch_add(1, std::memory_order_acq_rel);
            }
            drainJob();
            inFlight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t generation = 0;
    bool stopping = false;

    // The live job — written under `mutex` before the generation bump, read by
    // every lane after it, so plain fields suffice for everything except the
    // three counters. `job` borrows the caller's function for the run's
    // lifetime only.
    const std::function<void(std::size_t, std::size_t)>* job = nullptr;
    std::size_t count = 0;
    std::size_t chunks = 0;
    std::size_t chunkSize = 0;
    std::atomic<std::size_t> nextChunk{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<int> inFlight{0};
};

TaskPool::TaskPool(std::size_t workers) : lanes_(std::max<std::size_t>(workers, 1)) {
    if (lanes_ > 1) {
        impl_ = new Impl(lanes_);
    }
}

TaskPool::~TaskPool() { delete impl_; }

void TaskPool::run(
    std::size_t count,
    const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0) {
        return;
    }
    if (impl_ == nullptr) {
        fn(0, count);
        return;
    }
    impl_->run(count, fn);
}

namespace {

/// The lane count a fresh pool gets: performance cores when the platform names
/// them (Apple Silicon splits P from E and the tick wants the fast ones), the
/// generic count otherwise — and `RM_SIM_WORKERS` overrides both, because the
/// determinism A/B and the benchmark need a serial mode that survives defaults.
[[nodiscard]] std::size_t defaultLanes() noexcept {
    if (const char* env = std::getenv("RM_SIM_WORKERS")) {
        const long parsed = std::strtol(env, nullptr, 10);
        if (parsed > 0) {
            return static_cast<std::size_t>(parsed);
        }
        return 1;
    }
#if defined(__APPLE__)
    int cores = 0;
    std::size_t size = sizeof(cores);
    if (sysctlbyname("hw.perflevel0.ncpu", &cores, &size, nullptr, 0) == 0
        && cores > 0) {
        return static_cast<std::size_t>(cores);
    }
#endif
    const unsigned int total = std::thread::hardware_concurrency();
    return total > 0 ? static_cast<std::size_t>(total) : 1;
}

TaskPool* gPool = nullptr;

} // namespace

TaskPool& simPool() {
    // First call builds the pool. `parallelFor` reaches this only from the
    // tick thread and the draw gather — never concurrently with itself, and
    // `setSimPoolSize` is documented setup-time — so a raw pointer is honest
    // about the actual threading guarantee rather than a lock pretending one.
    if (gPool == nullptr) {
        gPool = new TaskPool(defaultLanes());
    }
    return *gPool;
}

void setSimPoolSize(std::size_t workers) {
    delete gPool;
    gPool = workers == 0 ? new TaskPool(defaultLanes()) : new TaskPool(workers);
}

void parallelFor(
    std::size_t count,
    const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0) {
        return;
    }
    simPool().run(count, fn);
}

} // namespace rm
