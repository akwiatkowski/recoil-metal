#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rm {

// The quality switches, and where they come from when nobody says.
//
// This exists because milestone 13 left a question it could not answer: the
// reflection pass and the stratum normal maps are both settings, both default to
// on, and there is no way to change a default — so the value compiled in is the
// only one most runs will ever see. Milestone 14 added a third switch that costs
// more than both together, which settled that a file was needed.
//
// DEFAULTS ARE LOOKS-BEST, DELIBERATELY. The cheap-by-default argument is the
// usual one and it is wrong for this project: the thing being demonstrated is
// what the content looks like when a Metal renderer draws it, the screenshots in
// the README are a deliverable, and a reader who runs it should see the same
// thing. The costs are known and stated rather than hidden —
//
//   planar reflection    +0.50 ms
//   stratum normals      +0.21 ms
//   props                +2.8 ms at 5182 of them, +6.2 ms at 46 971
//
// — and every one is a single keypress away. What that DOES oblige is that a
// benchmark states which switches were on, because otherwise two numbers from
// this renderer are not comparable with each other, let alone with Recoil's.
struct Settings {
    bool reflections = true;
    bool stratumNormals = true;
    bool props = true;
};

/// Where the settings file lives: the platform's own place for it.
///
/// `~/Library/Application Support/recoil-metal/settings.lua`. Not `~/.config`,
/// which is a Linux convention this project has no reason to import — "Mac-native
/// means Mac-native" is a settled decision, and the same one that rules out SDL.
[[nodiscard]] std::filesystem::path settingsPath();

/// Reads the settings file, falling back to the defaults for anything it does not
/// mention.
///
/// A MISSING FILE IS NOT AN ERROR — it is the ordinary case, and the whole point
/// of having defaults. A file that is present but unreadable IS worth reporting,
/// because silently falling back to defaults is how a setting someone edited
/// stops taking effect without saying why; the messages come back in `problems`
/// for the caller to print, so that core/ does no I/O to a terminal.
///
/// The file is a Lua table, read by the same data reader that reads mapinfo.lua:
///
///     -- ~/Library/Application Support/recoil-metal/settings.lua
///     {
///         reflections = true,
///         stratum_normals = true,
///         props = false,
///     }
///
/// Hand-edited, and read-only from this side. Persisting the live `r`/`n`/`p`
/// toggles was the alternative and is worse: a keypress that rewrites a config
/// file is a surprise, and the toggles exist to compare two frames rather than to
/// state a preference.
[[nodiscard]] Settings loadSettings(const std::filesystem::path& path,
                                    std::vector<std::string>& problems);

} // namespace rm
