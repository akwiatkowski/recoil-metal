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
#include "platform/KeyInput.hpp"
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
    /// WHERE the click landed, in AppKit logical points with a top-left origin — the space
    /// `width()` and `height()` report. `UiViewport::toHud` performs the explicit layout-space
    /// conversion beside the hit test that needs it.
    ///
    /// HERE RATHER THAN AS A THIRD `onClick` PARAMETER because it belongs to the same question:
    /// a callback is handed a ray for the world and this for the screen, and a caller that wants
    /// neither ignores both. Added for the minimap (§7 P7.4), which has to know whether a click
    /// was on the panel before it decides what the click meant — a ray alone cannot say, since
    /// the panel is in front of the world rather than in it.
    ///
    /// Backing pixels never enter input. The renderer derives drawable size and font atlas
    /// raster scale separately.
    ///
    /// The accompanying ray was derived from this same logical point and the view's logical
    /// bounds; after that conversion it contains a world-space origin and direction.
    float pointX = 0.0f;
    float pointY = 0.0f;

    bool shift = false;
    bool command = false;
    bool control = false;

    /// AppKit's clickCount: 2 on the second click of a double-click. The first click of the
    /// pair still arrives as 1 and is handled normally — a double-click refines what the
    /// single click did, which is exactly how select-then-select-all-of-type should feel.
    int clicks = 1;
};

// Owns the NSWindow, its CAMetalLayer, the vsync display link, and the
// Renderer. The pImpl idiom keeps every Objective-C type out of this header:
// the rest of the codebase (and the test target) never includes AppKit.
class Window {
public:
    // width/height are in points, not pixels — on a Retina display the Metal
    // drawable is 2x; the Impl matches contentsScale so we render at full
    // backing resolution (a classic silent half-resolution bug otherwise).
    Window(int width, int height, const char* title, bool fullscreen = false);
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

    /// The fog of war mask for the frame about to be drawn. See Renderer::setFog.
    void setFog(std::span<const std::uint16_t> counts, int squaresX, int squaresZ,
                float widthElmos, float depthElmos);
    void clearFog() noexcept;

    // The Supreme Commander ground splat. See Renderer::setSplat.
    void setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                  const dds::Texture& maskB);

    // Units to draw on the terrain. See Renderer::setUnits.
    void setUnits(std::span<const dds::Texture> textures, std::span<const char> srgb,
                  std::span<const UnitBatch> batches);

    // The map's scenery. See Renderer::setProps.
    void setProps(std::span<const dds::Texture> textures, std::span<const char> srgb,
                  std::span<const PropBatch> batches);

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

    // Called with the world ray under the cursor and the modifier keys held. Left clicks and
    // Shift-right clicks arrive on release after the drag-slop check. An unshifted right click
    // arrives on press so an ordinary RTS order does not wait for the button to come back up.
    //
    // A ray rather than a screen position, and a ray rather than a resolved
    // pick: building it needs the camera and the viewport, which live here,
    // while deciding what it hit needs the map and the units, which do not.
    // Drags are already spoken for by the camera (orbit and pan); Shift-right therefore remains
    // release-gated so a pan cannot also queue an order.
    void onClick(std::function<void(const Ray& ray, MouseButton button, MouseModifiers mods)> callback);

    /// Called for every bound logical key press and release. Repeat and modifiers are facts on the
    /// same event rather than separate platform polls, so consumers cannot observe mismatched
    /// input moments. Characters remain layout-aware; this is not a physical-key mapping.
    void onKey(std::function<void(KeyEvent event)> callback);

    /// Called for every wheel notch with its scrolling deltas, before the default zoom.
    /// Returning true consumes the notch (an array drag spends it on spacing); anything
    /// else, including no callback at all, keeps today's anchored zoom.
    void onScroll(std::function<bool(float scrollingDeltaY)> callback);

    /// Whether the left button is down right now, and where its press began, in AppKit logical
    /// points. POLLED, like the cursor and for the same reason: a drag is a per-frame
    /// fact, and the interface is rebuilt per frame. The origin is only meaningful while
    /// the button is held; the band-select rectangle and the minimap's drag-to-pan are both
    /// derived from these two answers and the cursor, with no drag events plumbed at all.
    [[nodiscard]] bool leftMouseHeld() const;
    [[nodiscard]] std::array<float, 2> dragOrigin() const;

    /// Shift's live state for a band selection completed by per-frame mouse polling rather than
    /// by a key event. Keyboard bindings consume `KeyEvent::modifiers` instead.
    [[nodiscard]] bool shiftHeldNow() const;

    /// Whether a key is down right now. The form a per-frame update wants — asking is
    /// cheaper than tracking the same set again in the caller, and there is exactly one
    /// truth about what is held.
    [[nodiscard]] bool keyHeld(Key key) const;

    /// Where the cursor is right now, in `MouseModifiers::pointX`'s space — logical points,
    /// top-left origin. `UiViewport::toHud` converts it before a HUD hit test.
    ///
    /// A POLL RATHER THAN AN `onMouseMove` CALLBACK, for `keyHeld`'s reason and one more of its
    /// own. A callback would need a tracking area and would then cache a value that only the
    /// frame loop ever reads — the interface is rebuilt every frame, so a hover computed at
    /// mouse-event rate is either stale or recomputed anyway. Asking once per frame is exactly
    /// the cadence the answer is used at.
    ///
    /// The cursor may be OUTSIDE the window, in which case the coordinates fall outside the
    /// AppKit content view and every hit test misses — which is the correct answer, and the
    /// reason this does not need to be an optional.
    [[nodiscard]] std::array<float, 2> cursor() const;

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

    /// The content view's size in logical points. World rendering obtains drawable pixels from
    /// its Metal view; HUD layout and input must not inherit that scale.
    [[nodiscard]] unsigned int width() const;
    [[nodiscard]] unsigned int height() const;

    /// What the player asked for, on top of the automatic figure. 1 is automatic alone.
    void setUserHudScale(float scale);
    [[nodiscard]] float userHudScale() const noexcept;

    /// AppKit points, display backing scale, safe content, and every HUD-space conversion.
    /// The renderer is synchronised before this value reaches frame code, so obtaining fonts
    /// after this call cannot observe an atlas for another display scale.
    [[nodiscard]] ui::UiViewport uiViewport() const;

    /// The two faces the interface is set in. See Renderer::labelFont.
    [[nodiscard]] text::Font labelFont() const;
    [[nodiscard]] text::Font readoutFont() const;

    /// The map's own preview thumbnail, under the minimap. See Renderer::setMinimapImage.
    void setMinimapImage(const dds::Texture& image);

    /// The map's wave-normal texture. See Renderer::setWaterWaveTexture.
    void setWaterWaveTexture(const dds::Texture& normal);

    /// Where to draw it this frame, in authored HUD points. See Renderer::setMinimapRect.
    void setMinimapRect(float x, float y, float width, float height) noexcept;

    /// This frame's semantic UI layers. See Renderer::setHud.
    void setHud(const ui::Geometry& geometry);

    /// Shared HUD backdrop quality. See Renderer::setUiEffects.
    void setUiEffects(ui::EffectsLevel level);

    /// The build tray's packed unit icons. See Renderer::setIconAtlas.
    void setIconAtlas(const dds::Texture& atlas);
    void setWeaponMaterials(std::span<const WeaponMaterial> materials);

    /// This frame's selection rings. See Renderer::setGroundDecals. Like
    /// setInstances, only meaningful from inside an onFrame callback.
    void setGroundDecals(std::span<const DecalVertex> vertices);

    /// The build ghost — the armed blueprint's silhouette at the cursor. See
    /// Renderer::setGhost; sticky until cleared, unlike the per-frame lists.
    void setGhost(std::size_t batch, const UnitInstance& instance,
                  std::array<float, 4> tint);
    void clearGhost() noexcept;
    void setGhosts(std::span<const Renderer::GhostDraw> ghosts);
    void setBuildGrid(bool enabled) noexcept;
    /// Native events for held-preview acceptance, with a deterministic polled cursor.
    void sendMouseDrag(float pointX, float pointY, bool release = false);
    void sendScroll(float points);
    void sendEscape();

    /// This frame's construction sites, and the clock their effects run on.
    /// See Renderer::setConstructions.
    void setConstructions(std::span<const Renderer::ConstructionDraw> sites) noexcept;
    void setConstructionTime(float seconds) noexcept;

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

    /// Input acceptance keeps the window visible without activating the app, so macOS
    /// cannot suspend display callbacks when another ordinary window would cover it.
    /// It ignores WindowServer mouse input; explicit test NSEvents still use sendEvent.
    void show(bool inputAcceptance = false);

    /// Native-event acceptance only: logical top-left points go through NSWindow's ordinary
    /// responder dispatch, including press/release handling. Requires no Accessibility access.
    /// Injected pairs accept first mouse while inactive; physical clicks keep normal behavior.
    void sendMouseClick(float pointX, float pointY, MouseButton button, bool shift = false);
    /// Simulated display backing, explicitly distinct from testing a physical Retina screen.
    void setSimulatedBacking(float scale);
    /// Call between frames, never from onFrame (which owns an active renderer write slot).
    [[nodiscard]] Renderer::CapturedImage capture();
    void stop();

private:
    struct Impl;
    // unique_ptr to incomplete type: the destructor MUST be defined (even if
    // defaulted) in the .mm where Impl is complete, otherwise the inline
    // destructor here can't delete the pointee.
    std::unique_ptr<Impl> impl_;
};

} // namespace rm
