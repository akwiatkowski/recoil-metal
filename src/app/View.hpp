#pragma once

// Where the camera looks, and what a capture writes.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5 — the last of it. Two kinds of thing that both
// happen once, at startup or at the end: pointing the camera somewhere sensible, and turning a
// benchmark into a file.
//
// THE TWO TEMPLATES ARE TEMPLATES FOR A REASON WORTH KEEPING: `Window` and `Renderer` both offer
// `focusOn` and `camera()`, and neither shares a base class with the other — one owns an AppKit
// view and the other owns a Metal device. A template over "whatever has those two" is what lets
// the windowed path and the capture path point the camera the same way, which is why every
// screenshot frames the same thing the live view would.

#include "app/Interface.hpp"

#include "core/bench/FrameStats.hpp"

#include <string>

namespace rm::app {

/// Applies whichever ground the map brought, plus its water plane.
///
/// Templated on the target for the same reason focusOnFirstUnit is: Window and
/// Renderer both offer these and share no base class.
template <typename Target>
void applyGround(Target& target, const LoadedMap& map) {
    if (map.atlas) {
        target.setGroundTexture(*map.atlas);   // SMF: a baked BC1 atlas
    } else if (map.colours) {
        target.setGroundColourMap(*map.colours);  // .scmap: terrain-type bands
    }
    // Set after the colour map, not instead of it: the terrain-type bands stay
    // loaded as the fallback the shader uses whenever the splat is off, so a map
    // whose layer textures are missing still draws something map-shaped.
    if (!map.splat.empty()) {
        target.setSplat(map.splat, map.splatMaskA, map.splatMaskB);
    }
    // The map's own thumbnail. Uploaded with the ground because it IS ground — a picture of
    // it — and because this is the one place that runs once per map for every target.
    if (map.preview.width > 0 && map.preview.height > 0) {
        target.setMinimapImage(map.preview);
    }
    target.setWater(map.hasWater, map.waterLevel);
    if (map.environment) {
        target.setEnvironment(*map.environment);
    }
}

/// Points the camera at the first instance of the first model.
///
/// Framing the whole map makes a unit about two pixels across, which says
/// nothing about whether it is textured correctly. Templated on the target
/// because Window and Renderer both offer focusOn and neither shares a base
/// class — a one-method interface would be ceremony for two call sites.
template <typename Target>
void focusOnFirstUnit(Target& target, const UnitScene& scene, float radiiBack) {
    if (scene.batches.empty() || scene.batches.front().instances.empty()
        || scene.batches.front().model == nullptr) {
        return;
    }

    const rm::UnitBatch& batch = scene.batches.front();
    const rm::UnitInstance& first = batch.instances.front();
    target.focusOn(first.position,
                   std::max(batch.model->radius, 1.0f) * radiiBack * first.scale);
}

/// `--look X Z RADIUS`: aim a capture at a world point, framed to show RADIUS elmos
/// around it. A match's stages happen at one base or the other, and neither is the
/// first unit `--focus` knows how to find.
struct LookOptions {
    bool enabled = false;
    float x = 0.0f;
    float z = 0.0f;
    float radiusElmos = 300.0f;
};

struct BenchOptions {
    bool enabled = false;
    bool offscreen = false;  ///< no window, no vsync
    std::size_t frames = 600;
    std::size_t warmup = 60;
    unsigned int width = 1920;
    unsigned int height = 1080;
    std::string csvPath;
};

[[nodiscard]] float parseFocus(int argc, const char* argv[]);
[[nodiscard]] LookOptions parseLook(int argc, const char* argv[]);
[[nodiscard]] BenchOptions parseBench(int argc, const char* argv[]);

/// Writes a benchmark's per-frame samples as CSV.
void writeCsv(const std::string& path, const rm::bench::FrameRecorder& recorder);

} // namespace rm::app
