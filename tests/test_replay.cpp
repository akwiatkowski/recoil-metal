#include "core/sim/Replay.hpp"

#include "core/sim/Movement.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using rm::sim::compareHashes;
using rm::sim::compareHeaders;
using rm::sim::ReplayHeader;

namespace {

[[nodiscard]] ReplayHeader header(int rate = rm::sim::kTicksPerSecond) {
    ReplayHeader h;
    h.ticksPerSecond = rate;
    h.widthsFingerprint = rm::sim::widthsFingerprint();
    return h;
}

/// A path in the system temp directory, removed when the test ends however it ends.
class TempPath {
public:
    explicit TempPath(const char* name)
        : path_{(std::filesystem::temp_directory_path() / name).string()} {}
    ~TempPath() { std::filesystem::remove(path_); }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;

    [[nodiscard]] const std::string& get() const noexcept { return path_; }

private:
    std::string path_;
};

} // namespace

TEST_CASE("two runs that agree report no divergence") {
    const std::vector<rm::StateHash> a{1, 2, 3, 4, 5};
    const std::vector<rm::StateHash> b{1, 2, 3, 4, 5};
    const rm::sim::Divergence d = compareHashes(a, b);
    REQUIRE_FALSE(d.diverged);
    REQUIRE_FALSE(d.incomparable);
}

TEST_CASE("the FIRST divergent tick is reported, not the last and not a count") {
    // Everything after the first disagreement is downstream of it, so a later tick is noise
    // rather than evidence. This is the property that makes the report a breakpoint.
    const std::vector<rm::StateHash> a{1, 2, 3, 4, 5};
    const std::vector<rm::StateHash> b{1, 2, 99, 4, 98};

    const rm::sim::Divergence d = compareHashes(a, b);
    REQUIRE(d.diverged);
    REQUIRE(d.tick == 2);
    REQUIRE(d.recorded == 3);
    REQUIRE(d.replayed == 99);
}

TEST_CASE("a one-bit difference on the last tick is still caught") {
    std::vector<rm::StateHash> a{1, 2, 3, 4, 5};
    std::vector<rm::StateHash> b = a;
    b.back() ^= 1ULL;  // the smallest possible disagreement

    const rm::sim::Divergence d = compareHashes(a, b);
    REQUIRE(d.diverged);
    REQUIRE(d.tick == 4);
}

TEST_CASE("a run that stopped early diverges at the tick it ran out") {
    const std::vector<rm::StateHash> full{1, 2, 3, 4, 5};
    const std::vector<rm::StateHash> shortRun{1, 2, 3};

    const rm::sim::Divergence d = compareHashes(full, shortRun);
    REQUIRE(d.diverged);
    REQUIRE(d.tick == 3);
    REQUIRE_FALSE(d.why.empty());
}

TEST_CASE("a log recorded at another tick rate is refused, not rescaled") {
    // PLAN2.md §5.1. Every authored duration lands on a different tick at a different rate,
    // so the two runs did not play the same match — comparing them would report a divergence
    // that is really a configuration difference, and the user would go hunting a sim bug.
    const rm::sim::Divergence d = compareHeaders(header(10), header(20));
    REQUIRE(d.incomparable);
    REQUIRE_FALSE(d.diverged);
    REQUIRE(d.why.find("tick rate") != std::string::npos);
}

TEST_CASE("a log recorded under different integer widths is refused") {
    // core/Types.hpp promises that changing a width invalidates old logs loudly. This is
    // the mechanism that keeps the promise.
    ReplayHeader recorded = header();
    recorded.widthsFingerprint ^= 0xFULL;

    const rm::sim::Divergence d = compareHeaders(recorded, header());
    REQUIRE(d.incomparable);
    REQUIRE(d.why.find("widths") != std::string::npos);
}

TEST_CASE("a log from a different format version is refused") {
    ReplayHeader recorded = header();
    recorded.formatVersion = ReplayHeader::kFormatVersion + 1;

    const rm::sim::Divergence d = compareHeaders(recorded, header());
    REQUIRE(d.incomparable);
}

TEST_CASE("comparable headers compare clean") {
    const rm::sim::Divergence d = compareHeaders(header(), header());
    REQUIRE_FALSE(d.incomparable);
    REQUIRE_FALSE(d.diverged);
}

TEST_CASE("a hash log round-trips through a file") {
    const TempPath path{"rm_replay_roundtrip.log"};
    const std::vector<rm::StateHash> hashes{0xDEADBEEFULL, 0x1234ULL, 0xFFFFFFFFFFFFFFFFULL};

    REQUIRE(rm::sim::writeHashLog(path.get(), header(), hashes).has_value());

    const auto read = rm::sim::readHashLog(path.get());
    REQUIRE(read.has_value());
    REQUIRE(read->header.formatVersion == ReplayHeader::kFormatVersion);
    REQUIRE(read->header.ticksPerSecond == rm::sim::kTicksPerSecond);
    REQUIRE(read->header.widthsFingerprint == rm::sim::widthsFingerprint());
    REQUIRE(read->hashes == hashes);

    // And the round-trip is exact, including the largest representable hash — a log written
    // in hex and read back in decimal would pass every other assertion here and fail this.
    REQUIRE(read->hashes.back() == 0xFFFFFFFFFFFFFFFFULL);
}

TEST_CASE("pointing the reader at some other file says so, rather than diverging") {
    const TempPath path{"rm_replay_notalog.txt"};
    {
        std::FILE* f = std::fopen(path.get().c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("this is not a hash log\n", f);
        std::fclose(f);
    }

    const auto read = rm::sim::readHashLog(path.get());
    REQUIRE_FALSE(read.has_value());
    REQUIRE(read.error().code == rm::sim::ReplayError::Code::NotAReplay);
}

TEST_CASE("a truncated log is an error, not silent agreement") {
    // The worst failure this format could have: a short file comparing equal for as far as
    // it goes and being reported as a match.
    const TempPath path{"rm_replay_truncated.log"};
    const std::vector<rm::StateHash> hashes{1, 2, 3, 4, 5};
    REQUIRE(rm::sim::writeHashLog(path.get(), header(), hashes).has_value());

    // Chop the last two hash lines, leaving the header's promise of five intact.
    {
        const auto whole = rm::sim::readHashLog(path.get());
        REQUIRE(whole.has_value());
        std::FILE* f = std::fopen(path.get().c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("recoil-metal hash log\nversion ", f);
        std::fputs(std::to_string(ReplayHeader::kFormatVersion).c_str(), f);
        std::fputs("\nticks-per-second ", f);
        std::fputs(std::to_string(rm::sim::kTicksPerSecond).c_str(), f);
        std::fputs("\nwidths ", f);
        std::fprintf(f, "%llx", static_cast<unsigned long long>(rm::sim::widthsFingerprint()));
        std::fputs("\ntick-count 5\n1\n2\n3\n", f);
        std::fclose(f);
    }

    const auto read = rm::sim::readHashLog(path.get());
    REQUIRE_FALSE(read.has_value());
    REQUIRE(read.error().code == rm::sim::ReplayError::Code::Truncated);
}

TEST_CASE("a missing file is an error rather than an empty log") {
    const auto read = rm::sim::readHashLog("/nonexistent/dir/rm_replay_missing.log");
    REQUIRE_FALSE(read.has_value());
    REQUIRE(read.error().code == rm::sim::ReplayError::Code::CannotOpen);
}

TEST_CASE("the widths fingerprint changes when a width does") {
    // Not testable directly without editing Types.hpp, so this pins the current value's
    // shape instead: every alias contributes, so the packed field is non-zero in each of
    // the slots the fingerprint claims to cover.
    const std::uint64_t f = rm::sim::widthsFingerprint();
    for (int slot = 0; slot < 13; ++slot) {
        const std::uint64_t nibble = (f >> (slot * 4)) & 0xFULL;
        REQUIRE(nibble != 0);
    }
}
