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

#include "core/log/Log.hpp"
#include "core/sim/Intel.hpp"
#include "core/ui/GameProfile.hpp"
#include "core/ui/Effects.hpp"

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
    /// `--backing <s>`: the display scale a headless capture or benchmark stands in for.
    /// Width and height stay LOGICAL; the image gets `s` times the pixels, the interface
    /// lays out in points and rasterises its fonts at `s`, as a Retina window would. 1 to 4.
    float backing = 1.0f;
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

    /// `--replay-commands <path>`: play a recorded command log back through the headless
    /// pre-run instead of letting opponents think — §1.3's criterion made runnable: the
    /// same log and the same setup must produce the same match, which `--check-hash-log`
    /// then proves. Opponents and the factory roll-off are disabled (their decisions are
    /// IN the log); the setup's own orders are skipped as already issued.
    std::string replayCommandsPath;

    /// `--ai-sanity`: profile the FAF opponent's sandbox during the headless pre-run, or boot
    /// a standalone sandbox when no live FAF-opponent sandbox exists, and close with the sanity
    /// report — what got built, which engine bindings the AI called, and which of the corpus's
    /// own functions ran. The measuring half of "is the AI integrated".
    bool aiSanity = false;
};

struct LoggingOptions {
    rm::log::Options sink;
    std::vector<std::string> problems;
};

struct WindowOptions {
    unsigned int width = 1280;
    unsigned int height = 720;
    bool fullscreen = false;
};

// --- The parsers ---------------------------------------------------------------------------

[[nodiscard]] ShotOptions parseShot(int argc, const char* argv[]);
[[nodiscard]] std::vector<UnitOptions> parseUnits(int argc, const char* argv[]);
[[nodiscard]] rm::vfs::AssetSearch parseAssetSearch(int argc, const char* argv[]);
[[nodiscard]] rm::vfs::Vfs parseContent(int argc, const char* argv[]);
[[nodiscard]] float parseAnimationTime(int argc, const char* argv[]);
[[nodiscard]] MarchOptions parseMarch(int argc, const char* argv[]);

/// `--log-level trace|debug|info|warn|error|off` and `--log-file <path>`.
/// Invalid or missing values retain safe defaults and are returned as reportable problems.
[[nodiscard]] LoggingOptions parseLogging(int argc, const char* argv[]);

/// `--window <w> <h>` sets the initial logical-point size; `--fullscreen` uses the display.
[[nodiscard]] WindowOptions parseWindow(int argc, const char* argv[]);

/// `--ui-scale N`: the requested multiplier relative to automatic HUD size.
///
/// One, the default, is the automatic figure alone — `ui::hudScale`, which already grows the
/// interface with the viewport. Above one uses spare room left after automatic magnification;
/// it cannot shrink the authored viewport below Compact and make controls overlap. Below one
/// requests less prominent chrome where available fit permits. Out of range clamps, and anything
/// unparseable falls back to automatic.
[[nodiscard]] float parseUiScale(int argc, const char* argv[]);

/// `--ui fa|bar|neutral|faf`: select one concrete game-interface profile.
///
/// FA is the default and `faf` preserves the existing classic nine-slice option. BAR and Neutral
/// are explicit until a run-wide content-family policy exists; individual model formats are not
/// enough evidence because one scene may contain both.
[[nodiscard]] rm::ui::GameProfile parseGameProfile(int argc, const char* argv[]);

/// `--ui-effects full|reduced|off`: override the shared HUD backdrop effect.
/// Absent means Full unless macOS Reduced Transparency resolves it to Off in the windowed path.
[[nodiscard]] rm::ui::EffectsPreference parseUiEffects(int argc, const char* argv[]);

/// `--vision-style fa|recoil`: whether terrain blocks sight (ADR-037).
///
/// DEFAULTS TO `fa` — plain circles, no terrain consulted and no trees — because that is what
/// this engine's content is. Supreme Commander's `effects/vision.fx:35` is
/// `radius * vertex.xz + position.xy`: a flat disc, with no height input and no heightmap
/// sample anywhere in the file. Running FA blueprints on FA maps and then blocking sight with
/// hills is a different game wearing this one's content.
///
/// It used to default to `recoil` on the argument that blocked sight is the more interesting
/// game. That is a fair opinion about game design and the wrong default for a transcription:
/// the simple model is the one the content was balanced against, and the raycast is the
/// ADVANCED option — which is now genuinely advanced, since it reads each unit's own sensor
/// height (`UnitCatalog::IntelRadii::eyeHeight`) rather than looking out from the ground.
///
/// An unrecognised value is the default with a complaint, not an exit: a typo in a
/// rendering-adjacent flag should not stop a match starting.
[[nodiscard]] rm::sim::VisionStyle parseVisionStyle(int argc, const char* argv[]);

/// `--factions uef,seraphim,...`: which faction each army plays, in seat order, cycled over
/// more armies than names. Empty — flag absent, or nothing parsed — keeps the round-robin
/// default. Unknown names are reported and skipped: seating the wrong faction is a worse
/// answer than seating one fewer.
[[nodiscard]] std::vector<rm::sim::Faction> parseFactions(int argc, const char* argv[]);

/// `--dump-weapon <ID>`: print one unit's weapon timings, authored beside corrected.
[[nodiscard]] bool dumpWeapons(const rm::vfs::Vfs& content, const std::string& id);

/// The value after `flag`, as a count, or zero when the flag is absent.
[[nodiscard]] std::size_t parseCount(int argc, const char* argv[], std::string_view flag);

/// `--select-type <blueprint id>`: select the first matching unit in a headless capture.
[[nodiscard]] std::string_view parseSelectType(int argc, const char* argv[]);

/// Whether `flag` appears at all.
[[nodiscard]] bool hasFlag(int argc, const char* argv[], std::string_view flag);

} // namespace rm::app
