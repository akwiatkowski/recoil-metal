#pragma once

// The command line, parsed.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5, and this is the part that most wanted moving:
// argv parsing is pure — a `const char*[]` in, a struct out — and it was the only code in the
// engine that could not be tested at all, because it lived in an executable-only translation
// unit. §7's stated test for the phase is "the suite covers what moved out", and this is where
// that has the most to say.
//
// EVERY FLAG IS A SEPARATE SCAN, deliberately, rather than one pass over argv filling a struct.
// A scan per flag means adding one touches nothing else and a flag's meaning is readable in one
// place; the cost is a handful of passes over a dozen strings, once, at startup.

#include "app/SceneBuild.hpp"

#include "core/vfs/AssetSearch.hpp"
#include "core/vfs/Vfs.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rm::app {

/// Parsed --bench / --bench-offscreen arguments.
/// Parsed --units arguments.
/// Parsed --screenshot arguments.
struct ShotOptions {
    bool enabled = false;
    std::string path;
    unsigned int width = 1920;
    unsigned int height = 1080;
};

/// `--march <x> <z> <seconds>`: order every unit to a world position and run the
/// sim that long before the first frame.
///
/// Click-to-move cannot be screenshotted — there is no click in a headless
/// capture, and no way to script one here (AppleScript has no Accessibility
/// permission on this machine). This gives the same path a reproducible
/// entry point: the same orders and the same number of fixed ticks produce the
/// same scene every run, which is what makes a screenshot or a benchmark of
/// moving units worth comparing. Same rationale as `--time` for animation.
struct MarchOptions {
    /// `--march` orders every unit to (x, z) before simulating; `--play` simulates the
    /// skirmish without the blanket order, which is what a MATCH wants — the scripted
    /// armies decide their own movement, and sending both commanders to one point is a
    /// demolition derby rather than a game.
    bool orderAll = true;
    bool enabled = false;
    float x = 0.0f;
    float z = 0.0f;
    float seconds = 0.0f;

    /// Where to write this run's per-tick state hashes, or empty for nowhere.
    ///
    /// The determinism artifact. Two runs of the same invocation must produce identical
    /// files, and — once P8's Linux build exists — so must two ARCHITECTURES, which is the
    /// project's stated success criterion (PLAN2.md §1.3).
    std::string hashLogPath;

    /// A hash log to compare this run against. The check that makes the artifact useful:
    /// it reports the FIRST tick that disagrees, which is a breakpoint rather than a mood.
    std::string checkHashLogPath;

    /// Where to write this run's COMMAND log, or empty for nowhere.
    ///
    /// The other half of the determinism artifact, and the more interesting half: a hash log
    /// says two runs agreed, and a command log says what they were asked to do. Together with
    /// the initial state they are §1.3's criterion — "the same command log produces the same
    /// match" — with both halves now naming things that exist.
    std::string commandLogPath;
};

// --- The parsers ---------------------------------------------------------------------------

[[nodiscard]] ShotOptions parseShot(int argc, const char* argv[]);
[[nodiscard]] std::vector<UnitOptions> parseUnits(int argc, const char* argv[]);
[[nodiscard]] rm::vfs::AssetSearch parseAssetSearch(int argc, const char* argv[]);
[[nodiscard]] rm::vfs::Vfs parseContent(int argc, const char* argv[]);
[[nodiscard]] float parseAnimationTime(int argc, const char* argv[]);
[[nodiscard]] MarchOptions parseMarch(int argc, const char* argv[]);

/// `--dump-weapon <ID>`: print one unit's weapon timings, authored beside corrected.
[[nodiscard]] bool dumpWeapons(const rm::vfs::Vfs& content, const std::string& id);

/// The value after `flag`, as a count, or zero when the flag is absent.
[[nodiscard]] std::size_t parseCount(int argc, const char* argv[], std::string_view flag);

/// Whether `flag` appears at all.
[[nodiscard]] bool hasFlag(int argc, const char* argv[], std::string_view flag);

} // namespace rm::app
