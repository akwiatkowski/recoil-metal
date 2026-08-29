#pragma once

// The three ways this engine puts a scene on a screen, and the one thing each of them needs.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5 — the last and largest piece. §7 asks for
// `main.mm` to be "argv plus wiring, under 400 lines from 3,948"; it had reached 5,098, and 650
// of those were three run modes inlined into `main` and closing over two dozen locals.
//
// `Session` IS THOSE LOCALS, named. That is the honest description rather than a design: the
// three modes need the same scene, and before this they shared it by being in the same function.
// A struct of references says what the sharing is, and says it at a place a reader can find.
//
// THESE ARE `.mm`, unlike the rest of `app/`. `runWindowed` owns an `NSApplication` and
// `writePng` an ImageIO destination, so this is the one part of the app layer that genuinely
// needs Objective-C — which is also why it is the part no test can link, and why everything
// that could be `.cpp` was moved out first.

#include "app/View.hpp"

#include "core/ui/GameProfile.hpp"
#include "platform/Window.hpp"
#include "render/Renderer.hpp"

#include <string>
#include <vector>

namespace rm::app {

/// Everything the three run modes share.
///
/// REFERENCES, not values, and non-owning: `main` owns all of it and outlives the call. Same
/// shape and same reason as `sim::Match` — a run mode is built at one call site from storage
/// that is already there.
struct Session {
    /// THE COMMAND LINE, carried rather than pre-parsed into fields.
    ///
    /// Two flags are read inside a run mode rather than before it — `--select` in the capture
    /// path and `--look` — and both are read exactly where their meaning is local. Adding a
    /// `Session` field per flag would be a second copy of `Cli.hpp`'s job; handing over argv
    /// says what is actually happening.
    int argc = 0;
    const char** argv = nullptr;

    const LoadedMap& map;
    UnitScene& units;
    const PropScene& props;
    PassabilitySet& passability;
    const rm::vfs::Vfs& content;
    const rm::Settings& settings;
    const rm::TerrainMesh& mesh;
    std::span<const rm::mapinfo::StartPosition> starts;

    /// Dust raised by `--march`, which the capture paths upload and the windowed path grows.
    std::vector<rm::Particle>& marchDust;

    ShotOptions shot;
    BenchOptions bench;
    MarchOptions march;
    LookOptions look;
    float focus = 0.0f;

    /// `--time`: where in an animation clip a capture freezes. Zero means the clip's start.
    float animationTime = 0.0f;

    /// `--ui-scale`: the player's requested multiplier on top of automatic magnification.
    ///
    /// A PREFERENCE RATHER THAN A CORRECTION. `ui::hudScale` already grows the interface with
    /// the viewport, which is what a larger window should do; this requests room left beyond
    /// Compact, or less prominent chrome where available fit permits.
    float uiScale = 1.0f;

    /// `--ui`: the run-wide interface vocabulary and material policy.
    rm::ui::GameProfile uiProfile = rm::ui::GameProfile::Fa;

    /// How many prop instances were placed, for the quality line each mode prints.
    std::size_t propInstances = 0;
};

/// Writes a captured frame as a PNG. False on any failure, having said which.
[[nodiscard]] bool writePng(const std::string& path, const rm::Renderer::CapturedImage& image);

/// `--bench-offscreen`: no window, no display link, hence no vsync — the only mode whose CPU
/// numbers describe the renderer rather than the display.
[[nodiscard]] int runOffscreenBenchmark(const Session& session);

/// `--screenshot`: one frame, headless, to a file.
[[nodiscard]] int runScreenshot(const Session& session);

/// The interactive game.
[[nodiscard]] int runWindowed(const Session& session);

} // namespace rm::app
