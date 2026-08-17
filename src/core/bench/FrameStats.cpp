#include "core/bench/FrameStats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace {

/// Formats a double with fixed precision, avoiding iostream's locale surprises
/// in CSV output (a comma decimal separator would corrupt the file outright).
[[nodiscard]] std::string fixed(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

} // namespace

namespace rm::bench {

double FrameStats::meanFps() const noexcept {
    return meanMs > 0.0 ? 1000.0 / meanMs : 0.0;
}

double percentileMs(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const auto count = static_cast<double>(values.size());

    // Nearest rank: ceil(p * n), 1-based, so p=1.0 is the maximum and any
    // p > 0 selects at least the first element.
    const auto rank = static_cast<std::size_t>(std::ceil(clamped * count));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, values.size() - 1);

    return values[index];
}

FrameStats summarise(const std::vector<double>& values) {
    FrameStats stats;
    if (values.empty()) {
        return stats;
    }

    stats.frameCount = values.size();
    stats.meanMs = std::accumulate(values.begin(), values.end(), 0.0)
                 / static_cast<double>(values.size());
    stats.medianMs = percentileMs(values, 0.50);
    stats.p95Ms = percentileMs(values, 0.95);
    stats.p99Ms = percentileMs(values, 0.99);

    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    stats.minMs = *lo;
    stats.maxMs = *hi;

    return stats;
}

FrameRecorder::FrameRecorder(std::size_t warmupFrames) noexcept
    : warmupFrames_{warmupFrames} {}

void FrameRecorder::add(FrameSample sample) {
    if (skipped_ < warmupFrames_) {
        ++skipped_;
        return;
    }
    samples_.push_back(sample);
}

FrameStats FrameRecorder::cpuStats() const {
    std::vector<double> values;
    values.reserve(samples_.size());
    for (const FrameSample& sample : samples_) {
        values.push_back(sample.cpuMs);
    }
    return summarise(values);
}

FrameStats FrameRecorder::gpuStats() const {
    std::vector<double> values;
    values.reserve(samples_.size());
    for (const FrameSample& sample : samples_) {
        values.push_back(sample.gpuMs);
    }
    return summarise(values);
}

std::string FrameRecorder::toCsv() const {
    std::string csv = "frame,cpu_ms,gpu_ms\n";
    csv.reserve(csv.size() + samples_.size() * 24);

    for (std::size_t i = 0; i < samples_.size(); ++i) {
        csv += std::to_string(i);
        csv += ',';
        csv += fixed(samples_[i].cpuMs, 4);
        csv += ',';
        csv += fixed(samples_[i].gpuMs, 4);
        csv += '\n';
    }

    return csv;
}

std::string FrameRecorder::summaryLine(const std::string& label) const {
    const FrameStats cpu = cpuStats();
    const FrameStats gpu = gpuStats();

    // Mean plus the tail: a mean-only report calls a stuttering renderer fine.
    return label + ": " + std::to_string(cpu.frameCount) + " frames"
         + " | cpu mean " + fixed(cpu.meanMs, 3) + " ms"
         + " p95 " + fixed(cpu.p95Ms, 3) + " p99 " + fixed(cpu.p99Ms, 3)
         + " | gpu mean " + fixed(gpu.meanMs, 3) + " ms"
         + " p95 " + fixed(gpu.p95Ms, 3) + " p99 " + fixed(gpu.p99Ms, 3)
         + " | " + fixed(cpu.meanFps(), 1) + " fps";
}

} // namespace rm::bench
