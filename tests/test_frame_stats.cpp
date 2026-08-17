// Benchmark statistics tests.
//
// These matter more than they look: milestone 4's whole output is a number
// comparing two renderers, and a percentile computed one way here and another
// way in the thing we compare against makes that number meaningless. So the
// definition is pinned, not assumed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/bench/FrameStats.hpp"

#include <string>
#include <vector>

using Catch::Approx;
using rm::bench::FrameRecorder;
using rm::bench::FrameSample;

TEST_CASE("percentiles use nearest rank, stated and pinned") {
    // 1..100. Nearest rank: index ceil(p*n)-1, no interpolation.
    std::vector<double> values;
    for (int i = 1; i <= 100; ++i) {
        values.push_back(static_cast<double>(i));
    }

    REQUIRE(rm::bench::percentileMs(values, 0.50) == Approx(50.0));
    REQUIRE(rm::bench::percentileMs(values, 0.95) == Approx(95.0));
    REQUIRE(rm::bench::percentileMs(values, 0.99) == Approx(99.0));
    REQUIRE(rm::bench::percentileMs(values, 1.00) == Approx(100.0));

    // p=0 still selects the smallest element rather than reading index -1.
    REQUIRE(rm::bench::percentileMs(values, 0.0) == Approx(1.0));
}

TEST_CASE("percentiles handle degenerate input without UB") {
    REQUIRE(rm::bench::percentileMs({}, 0.95) == Approx(0.0));
    REQUIRE(rm::bench::percentileMs({7.0}, 0.95) == Approx(7.0));

    // Out-of-range fractions clamp rather than index out of bounds.
    REQUIRE(rm::bench::percentileMs({1.0, 2.0, 3.0}, 5.0) == Approx(3.0));
    REQUIRE(rm::bench::percentileMs({1.0, 2.0, 3.0}, -1.0) == Approx(1.0));
}

TEST_CASE("the summary reports mean, median, tail and extremes") {
    const auto stats = rm::bench::summarise({4.0, 4.0, 4.0, 4.0, 40.0});

    REQUIRE(stats.frameCount == 5);
    REQUIRE(stats.meanMs == Approx(11.2));
    REQUIRE(stats.medianMs == Approx(4.0));
    REQUIRE(stats.minMs == Approx(4.0));
    REQUIRE(stats.maxMs == Approx(40.0));

    // The point of carrying the tail: this run's mean says 11 ms, but one frame
    // in five took 40. A mean-only report would call that smooth.
    REQUIRE(stats.p99Ms == Approx(40.0));
}

TEST_CASE("meanFps inverts the mean frame time") {
    const auto stats = rm::bench::summarise({16.0, 16.0, 16.0, 16.0});
    REQUIRE(stats.meanFps() == Approx(62.5));

    REQUIRE(rm::bench::summarise({}).meanFps() == Approx(0.0));
}

TEST_CASE("warmup frames are discarded, not merely flagged") {
    // Shader compilation and texture residency land in the first frames; left
    // in, they own the maximum and drag the tail percentiles with them.
    FrameRecorder recorder{3};

    recorder.add(FrameSample{100.0, 90.0});  // warmup
    recorder.add(FrameSample{80.0, 70.0});   // warmup
    recorder.add(FrameSample{60.0, 50.0});   // warmup
    recorder.add(FrameSample{4.0, 3.0});
    recorder.add(FrameSample{5.0, 3.5});

    REQUIRE(recorder.skipped() == 3);
    REQUIRE(recorder.recorded() == 2);
    REQUIRE(recorder.cpuStats().maxMs == Approx(5.0));
    REQUIRE(recorder.gpuStats().meanMs == Approx(3.25));
}

TEST_CASE("a recorder with no warmup keeps every sample") {
    FrameRecorder recorder;
    recorder.add(FrameSample{4.0, 3.0});
    recorder.add(FrameSample{6.0, 5.0});

    REQUIRE(recorder.skipped() == 0);
    REQUIRE(recorder.recorded() == 2);
    REQUIRE(recorder.cpuStats().meanMs == Approx(5.0));
}

TEST_CASE("CPU and GPU are summarised separately") {
    // The distinction is the deliverable: a renderer can be GPU-bound or
    // CPU-bound and wall-clock alone cannot tell you which.
    FrameRecorder recorder;
    recorder.add(FrameSample{16.0, 2.0});
    recorder.add(FrameSample{16.0, 2.0});

    REQUIRE(recorder.cpuStats().meanMs == Approx(16.0));
    REQUIRE(recorder.gpuStats().meanMs == Approx(2.0));
}

TEST_CASE("CSV carries one row per frame so the raw distribution survives") {
    FrameRecorder recorder;
    recorder.add(FrameSample{4.0, 3.0});
    recorder.add(FrameSample{5.5, 3.25});

    const std::string csv = recorder.toCsv();

    REQUIRE(csv.find("frame,cpu_ms,gpu_ms\n") == 0);
    REQUIRE(csv.find("0,4.0000,3.0000\n") != std::string::npos);
    REQUIRE(csv.find("1,5.5000,3.2500\n") != std::string::npos);

    // A decimal comma would corrupt the file; the formatter must be
    // locale-independent.
    REQUIRE(csv.find(",4,0000") == std::string::npos);
}

TEST_CASE("an empty recorder produces a header-only CSV, not a malformed one") {
    const FrameRecorder recorder;
    REQUIRE(recorder.toCsv() == "frame,cpu_ms,gpu_ms\n");
    REQUIRE(recorder.cpuStats().frameCount == 0);
}

TEST_CASE("the summary line names both axes and the tail") {
    FrameRecorder recorder;
    recorder.add(FrameSample{4.0, 2.0});

    const std::string line = recorder.summaryLine("recoil-metal");

    REQUIRE(line.find("recoil-metal") != std::string::npos);
    REQUIRE(line.find("cpu mean") != std::string::npos);
    REQUIRE(line.find("gpu mean") != std::string::npos);
    REQUIRE(line.find("p99") != std::string::npos);
    REQUIRE(line.find("fps") != std::string::npos);
}
