#pragma once

#include "core/Types.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace rm::sim {

// A record of what a match DID, tick by tick, so that two runs can be compared.
//
// WHY THIS EXISTS. The project's success criterion is bit-identical replay across two
// architectures (PLAN2.md §1.3), and a criterion you cannot measure is a wish. This is the
// measurement: a file of per-tick state hashes plus enough header to know the two runs were
// asking the same question.
//
// It is also the debugging tool. `16-new-engine-feasibility.md §4` notes that black-box
// parity projects have no test oracle, so every regression test degenerates into "run both
// and diff" against a binary you cannot instrument. Our sim is the only authority on its own
// behaviour, so a recorded hash log IS the oracle — and a divergence report names the tick,
// which turns "the match played out differently" into a breakpoint.
//
// WHAT IS NOT HERE YET, deliberately: player commands. A replay in the full sense is a
// command log plus a seed, and there are no commands to log — the human's clicks do not yet
// go through a queue, and the scripted opponent decides from state rather than from input
// (PLAN2.md P4). What a `--play` match does is already a pure function of its setup, so a
// hash log is a complete record of it today. When commands arrive they are appended to this
// format, and the header below is what tells an old log from a new one.

/// What the two runs must agree about before their hashes are worth comparing.
struct ReplayHeader {
    /// Bumped whenever the format or the hashed field set changes, so an old log is
    /// rejected rather than compared against a hash that was never computed the same way.
    static constexpr std::uint32_t kFormatVersion = 2;

    std::uint32_t formatVersion = kFormatVersion;

    /// The rate the match was simulated at. A log recorded at 10 Hz means nothing against a
    /// run at 20 — every duration lands on a different tick — so this is checked and the
    /// comparison refused, never rescaled (PLAN2.md §5.1).
    int ticksPerSecond = 0;

    /// A fingerprint of the integer widths in `core/Types.hpp`. Changing a width changes the
    /// hash, which `Types.hpp` promises will invalidate old logs loudly; this is the
    /// mechanism that keeps the promise.
    std::uint64_t widthsFingerprint = 0;

    /// How many ticks were recorded. Redundant against the hash count, and checked against
    /// it — a truncated file is a real thing that happens, and a short log silently
    /// comparing equal for as far as it goes is the worst possible failure here.
    TickIndex tickCount = 0;
};

/// The widths `core/Types.hpp` currently declares, as one number.
[[nodiscard]] std::uint64_t widthsFingerprint() noexcept;

/// Where two runs stopped agreeing.
struct Divergence {
    /// The first tick whose hashes differ. Only meaningful when `diverged`.
    TickIndex tick = 0;
    StateHash recorded = 0;
    StateHash replayed = 0;
    bool diverged = false;

    /// Set when the two runs are not comparable at all — a different tick rate, a different
    /// format version, different widths. Distinguished from a divergence because the answers
    /// differ: a divergence is a bug in the sim, an incomparable pair is a mistake in the
    /// invocation.
    bool incomparable = false;
    std::string why;
};

/// Compares two hash sequences recorded under the same header.
///
/// Reports the FIRST disagreement rather than a count, because that is the one with
/// diagnostic value: every later tick is downstream of it, so a divergence at tick 40 makes
/// ticks 41 onward noise rather than evidence.
///
/// A length mismatch is a divergence at the point one run ran out, not an error: a match
/// that ended early ended early, and saying so at the tick it happened is more useful than
/// refusing to answer.
[[nodiscard]] Divergence compareHashes(std::span<const StateHash> recorded,
                                       std::span<const StateHash> replayed);

/// Checks two headers describe comparable runs, before any hash is looked at.
[[nodiscard]] Divergence compareHeaders(const ReplayHeader& recorded,
                                        const ReplayHeader& replayed);

struct ReplayLog {
    ReplayHeader header;
    std::vector<StateHash> hashes;
};

struct ReplayError {
    enum class Code {
        CannotOpen,   ///< the path could not be opened for reading or writing
        NotAReplay,   ///< the magic is absent — this is some other file
        BadVersion,   ///< a format version this build does not know how to read
        Truncated,    ///< fewer hashes on disk than the header promised
    };
    Code code;
    std::string message;
};

/// Writes a hash log. Text, one hex hash per line after a header, on purpose: a determinism
/// artifact whose own format needs a tool to read is a poor artifact, and `diff` on two of
/// these tells you the tick immediately.
[[nodiscard]] std::expected<void, ReplayError> writeHashLog(const std::string& path,
                                                            const ReplayHeader& header,
                                                            std::span<const StateHash> hashes);

[[nodiscard]] std::expected<ReplayLog, ReplayError> readHashLog(const std::string& path);

} // namespace rm::sim
