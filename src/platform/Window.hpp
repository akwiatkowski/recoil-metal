#pragma once

#include "core/bench/FrameStats.hpp"
#include "core/map/TerrainType.hpp"
#include "core/map/GroundSplat.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/model/Model.hpp"
#include "core/scene/UnitBatch.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
#include "core/mesh/TerrainMesh.hpp"

#include "core/camera/OrbitCamera.hpp"
#include "core/scene/Picking.hpp"
#include "render/Renderer.hpp"

#include <memory>
#include <array>
#include <functional>
#include <span>

namespace rm {

// Which button a click came from. Named rather than a bool because the two mean
// different things to an RTS — select and order — and `click(ray, true)` at a
// call site says nothing about which is which.
enum class MouseButton { Left, Right };

// Modifier keys held during a click. macOS conventions: Shift and Command are
// the usual "add to selection" modifiers; Control is treated the same as
// Command for selection purposes.
struct MouseModifiers {
    bool shift = false;
    bool command = false;
    bool control = false;
};

// Owns the NSWindow, its CAMetalLayer, the vsync display link, and the
// Renderer. The pImpl idiom keeps every Objective-C type out of this header:
// the rest of the codebase (and the test target) never includes AppKit.
class Window {
public:
    // width/height are in points, not pixels — on a Retina display the Metal
    // drawable is 2x; the Impl matches contentsScale so we render at full
    // backing resolution (a classic silent half-resolution bug otherwise).
    Window(int width, int height, const char* title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;

    // Uploads terrain for the renderer to draw and frames the camera on it.
    void setTerrain(const TerrainMesh& mesh);

    // Uploads the map ground texture. Optional — see Renderer::setGroundTexture.
    void setGroundTexture(const TileAtlas& atlas);

    // The Supreme Commander ground path and the per-map water plane.
    // See Renderer::setGroundColourMap and Renderer::setWater.
    void setGroundColourMap(const ColourImage& image);
    void setWater(bool enabled, float levelElmos);

    /// The map's lighting and water settings. See Renderer::setEnvironment.
    void setEnvironment(const Renderer::Environment& environment);

    // The Supreme Commander ground splat. See Renderer::setSplat.
    void setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                  const dds::Texture& maskB);

    // Units to draw on the terrain. See Renderer::setUnits.
    void setUnits(std::span<const dds::Texture> textures, std::span<const UnitBatch> batches);

    // The map's scenery. See Renderer::setProps.
    void setProps(std::span<const dds::Texture> textures, std::span<const PropBatch> batches);

    /// Replaces one batch's instances for this frame. See Renderer::setInstances.
    /// Only meaningful from inside an onFrame callback, which is the only point
    /// at which a ring slot is open for writing.
    void setInstances(std::size_t batchIndex, std::span<const UnitInstance> instances);

    // Called once per displayed frame, before the scene is encoded, with the
    // seconds elapsed since the previous frame. This is the app's opportunity
    // to advance a simulation and push new instances.
    //
    // The display link runs on the main run loop, so this arrives on the main
    // thread — the same one the input handlers run on. There is no render
    // thread here and nothing to synchronise against.
    void onFrame(std::function<void(float seconds)> callback);

    // Called when the user clicks without dragging, with the world ray under
    // the cursor and the modifier keys held.
    //
    // A ray rather than a screen position, and a ray rather than a resolved
    // pick: building it needs the camera and the viewport, which live here,
    // while deciding what it hit needs the map and the units, which do not.
    // Drags are already spoken for by the camera (orbit and pan), so only a
    // press and release that stayed put is reported.
    void onClick(std::function<void(const Ray& ray, MouseButton button, MouseModifiers mods)> callback);

    // Called on a printable keypress, lowercased. Modifiers are not reported:
    // a binding that needed one would belong in a menu rather than here.
    //
    // A plain char rather than a key code, so that nothing downstream has to
    // include AppKit to ask "was that R".
    void onKey(std::function<void(char key)> callback);

    // Called on press AND release, for the keys that are HELD rather than tapped.
    //
    // Separate from `onKey` rather than replacing it, because the two answer different
    // questions and conflating them makes both worse: a toggle wants "was R pressed" and
    // fires once, while panning wants "is W down" and must survive the whole frame. A
    // toggle driven by this would fire twice per press.
    //
    // Space arrives as ' '. It is the one non-printable key here and it earns the
    // exception: holding it to swing the camera is the convention this app follows, and a
    // held key with no release event cannot express "let go".
    void onKeyState(std::function<void(char key, bool pressed)> callback);

    /// Whether a key is down right now. The form a per-frame update wants — asking is
    /// cheaper than tracking the same set again in the caller, and there is exactly one
    /// truth about what is held.
    [[nodiscard]] bool keyHeld(char key) const;

    /// The planar reflection quality setting. See Renderer::setReflections.
    void setReflections(bool enabled);
    [[nodiscard]] bool reflectionsEnabled() const;

    /// Whether the water refracts. See Renderer::setRefraction.
    void setRefraction(bool enabled);
    [[nodiscard]] bool refractionEnabled() const;

    /// Whether the map's scenery is drawn. See Renderer::setPropsVisible.
    void setPropsVisible(bool visible);
    [[nodiscard]] bool propsVisible() const;

    /// The stratum normal-map setting. See Renderer::setStratumNormals.
    void setStratumNormals(bool enabled);
    [[nodiscard]] bool stratumNormalsEnabled() const;

    /// This frame's selection rings. See Renderer::setGroundDecals. Like
    /// setInstances, only meaningful from inside an onFrame callback.
    void setGroundDecals(std::span<const DecalVertex> vertices);

    /// This frame's selection. See Renderer::setSelection.
    void setSelection(std::span<const SelectionEntry> selected);

    /// This frame's particles. See Renderer::setParticles. Like setInstances,
    /// only meaningful from inside an onFrame callback.
    void setParticles(std::span<const Particle> particles);

    /// Points the camera at a world position. See Renderer::focusOn.
    void focusOn(std::array<float, 3> target, float distance);

    /// The camera itself, for a caller that drives it per frame.
    ///
    /// Exposed because a WASD pan is a per-FRAME thing — a pan driven by key events moves
    /// in jerks the length of the auto-repeat interval — and because the view space returns
    /// to is the one the app opened with, which only the caller knows to remember.
    ///
    /// `OrbitCamera` is a plain core type over simd, so this leaks no AppKit and no Metal,
    /// which is the rule this header follows. Renderer::camera() already does the same.
    [[nodiscard]] OrbitCamera& camera();

    // Benchmark control. See Renderer::beginBenchmark.
    void beginBenchmark(std::size_t warmupFrames);
    [[nodiscard]] std::size_t recordedFrames() const;
    [[nodiscard]] bench::FrameRecorder benchmarkSnapshot() const;

    void show();

private:
    struct Impl;
    // unique_ptr to incomplete type: the destructor MUST be defined (even if
    // defaulted) in the .mm where Impl is complete, otherwise the inline
    // destructor here can't delete the pointee.
    std::unique_ptr<Impl> impl_;
};

} // namespace rm
