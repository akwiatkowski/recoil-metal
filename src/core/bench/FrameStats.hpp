#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rm::bench {

// One frame's timing, in milliseconds.
//
// CPU and GPU are recorded separately because they answer different questions.
// A renderer can be GPU-bound (the frame's shading work dominates) or CPU-bound
// (encoding, culling, driver overhead dominates), and the whole point of
// milestone 4 is to say which — reporting only wall-clock would hide it.
struct FrameSample {
    double cpuMs = 0.0;  ///< wall time between successive frame starts
    double gpuMs = 0.0;  ///< MTLCommandBuffer GPUEndTime - GPUStartTime
};

// Summary of a run. Every field is milliseconds except frameCount.
//
// Percentiles matter more than the mean for frame timing: a renderer averaging
// 4 ms with a 30 ms p99 stutters visibly, and the mean alone calls that fine.
struct FrameStats {
    std::size_t frameCount = 0;
    double meanMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;

    /// Frames per second implied by the mean. Zero when there are no samples.
    [[nodiscard]] double meanFps() const noexcept;
};

// Nearest-rank percentile over a copy of the samples.
//
// "Percentile" has several incompatible definitions; this one is the simplest
// and is stated explicitly so results stay comparable across runs and against
// whatever Recoil's own numbers are computed with: sort ascending, then take
// the element at index ceil(p * n) - 1, clamped into range. No interpolation.
[[nodiscard]] double percentileMs(std::vector<double> values, double fraction);

/// Summarises a set of samples along one axis (CPU or GPU).
[[nodiscard]] FrameStats summarise(const std::vector<double>& values);

// Collects per-frame timings for a benchmark run.
//
// Deliberately dumb: it stores every sample rather than streaming statistics.
// A 10 000-frame run is 160 KB, and keeping the raw samples means the CSV can
// be re-analysed later without re-running the benchmark — which matters when
// the run takes a minute and the question changes afterwards.
class FrameRecorder {
public:
    /// Discards the first `warmupFrames` samples. Shader compilation, texture
    /// residency and clock ramp-up all land in the first frames and would
    /// otherwise dominate the maximum and skew the tail percentiles.
    explicit FrameRecorder(std::size_t warmupFrames = 0) noexcept;

    void add(FrameSample sample);

    [[nodiscard]] std::size_t recorded() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t skipped() const noexcept { return skipped_; }
    [[nodiscard]] const std::vector<FrameSample>& samples() const noexcept { return samples_; }

    [[nodiscard]] FrameStats cpuStats() const;
    [[nodiscard]] FrameStats gpuStats() const;

    /// Per-frame CSV: frame,cpu_ms,gpu_ms. One row per recorded frame, so the
    /// raw distribution survives for later analysis.
    [[nodiscard]] std::string toCsv() const;

    /// One-line human summary for the terminal.
    [[nodiscard]] std::string summaryLine(const std::string& label) const;

private:
    std::vector<FrameSample> samples_;
    std::size_t warmupFrames_ = 0;
    std::size_t skipped_ = 0;
};

} // namespace rm::bench
