#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h> // macOS 14+; replaces CVDisplayLink

#include "platform/Window.hpp"

#include <set>

#include "core/ui/Viewport.hpp"
#include "render/Renderer.hpp"

#include <QuartzCore/QuartzCore.hpp> // metal-cpp decl of CA::MetalLayer (no impl defines here!)

#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <utility>

// Thin ObjC delegate that forwards vsync callbacks into the C++ Renderer.
// CAMetalDisplayLink (macOS 14+) is the modern replacement for the
// deprecated CVDisplayLink callback dance.
// NOTE: Objective-C declarations are only allowed at global scope, so this
// deliberately cannot live in an anonymous namespace — the RM prefix is the
// namespacing mechanism here, per Cocoa convention.
@interface RMDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
// Not owned — the Renderer lives in Window::Impl, which outlives the link
// because Impl invalidates the link before destroying the renderer.
@property(nonatomic, assign) rm::Renderer* renderer;
// Points at Window::Impl's own callback object, so installing one later is
// visible here without re-plumbing. A pointer rather than a copy because
// Window::Impl is private and cannot be named from this global scope.
@property(nonatomic, assign) const std::function<void(float)>* frameCallback;
@end

@implementation RMDisplayLinkDelegate {
    // Seconds, from the same monotonic clock the renderer uses. Zero until the
    // first frame, whose delta is meaningless.
    double _lastFrameSeconds;
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)link
            needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const double elapsed = _lastFrameSeconds > 0.0 ? now - _lastFrameSeconds : 0.0;
    _lastFrameSeconds = now;

    // Opened before the callback and closed by drawFrame below: the callback is
    // where setInstances runs, and that needs a ring slot the GPU has finished
    // with. Always paired, whether or not anyone installed a callback.
    self.renderer->beginFrame();

    if (self.frameCallback != nullptr && *self.frameCallback) {
        (*self.frameCallback)(static_cast<float>(elapsed));
    }

    // The drawable comes from the update — with CAMetalDisplayLink, calling
    // -nextDrawable yourself throws CAMetalLayerInvalidOperation.
    self.renderer->drawFrame((__bridge CA::MetalDrawable*)update.drawable);
}
@end

// Content view that hosts the CAMetalLayer and turns mouse input into camera
// motion. A plain NSView would work for display, but only a first responder
// receives -mouseDragged: and -scrollWheel:.
//
// Both handlers run on the main thread, and the display link is attached to the
// main run loop, so drawFrame observes the camera on that same thread — no
// synchronisation is needed. That invariant is documented on Renderer::camera().
@interface RMTerrainView : NSView
@property(nonatomic, assign) rm::Renderer* renderer;  // not owned
// Window::Impl's click callback, by pointer — see RMDisplayLinkDelegate.
@property(nonatomic, assign)
    const std::function<void(const rm::Ray&, rm::MouseButton, rm::MouseModifiers)>* clickCallback;
@property(nonatomic, assign) const std::function<void(char)>* keyCallback;
@property(nonatomic, assign) const std::function<void(char, bool)>* keyStateCallback;
@property(nonatomic, assign) std::set<char>* heldKeys;
/// The player's requested multiplier on automatic size — `--ui-scale`. 1 is automatic.
@property(nonatomic, assign) float userHudScale;

/// Re-derives the layer's drawableSize from bounds x contentsScale.
- (void)rmSyncDrawableSize;

/// The left button's live state, for Window's per-frame polling. See the ivars.
- (BOOL)leftDown;
- (std::array<float, 2>)leftDownAtPoint;

/// One value shared by layout, input, projection, font rasterization, and safe-area anchoring.
- (rm::ui::UiViewport)rmUiViewport;
- (void)rmSyncUiViewport;
@end

@implementation RMTerrainView {
    // Points travelled since the current button went down. A press and release
    // that stayed under the slop is a click; anything more was a camera drag
    // and must not also fire an order.
    CGFloat _travelSincePress;

    // The left button's live state, POLLED by the app per frame (Window::leftMouseHeld)
    // rather than delivered as drag events — the interface is rebuilt per frame anyway, so
    // a drag is two facts: is the button down, and where did the press begin. The origin is
    // kept in logical points. The frame's UiViewport converts it beside the hit tests.
    BOOL _leftDown;
    std::array<float, 2> _leftDownAtPoint;

    // An ordinary RTS order is reported on press for minimum latency. Shift-right stays
    // release-gated because that same gesture may become either a queued order or a pan.
    BOOL _rightReportedOnDown;
    NSEventModifierFlags _rightModifiersAtPress;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

/// Keeps the CAMetalLayer's drawableSize equal to bounds x contentsScale.
///
/// CAMetalLayer does NOT do this itself: drawableSize is computed once, at the first
/// -nextDrawable, and then LATCHES — a later frame change leaves it at the old value
/// (measured: shrink the layer from 1600x900 to 900x500 and the next drawable is still
/// 3200x1800). MTKView resyncs it on every resize for exactly this reason, and a raw
/// layer-hosting view has to do the same by hand.
///
/// Skipping the sync was the resize bug: the scene kept rendering into the startup-sized
/// drawable while the HUD was laid out for the window's new size, so everything anchored to
/// the bottom or right edge — minimap, build tray, roster, clock — slid off the visible area.
- (void)rmSyncDrawableSize {
    CAMetalLayer* metalLayer = static_cast<CAMetalLayer*>(self.layer);
    if (metalLayer == nil) {
        return;
    }
    const CGFloat scale = metalLayer.contentsScale > 0.0 ? metalLayer.contentsScale : 1.0;
    const CGSize want = CGSizeMake(self.bounds.size.width * scale,
                                   self.bounds.size.height * scale);
    if (want.width >= 1.0 && want.height >= 1.0
        && !CGSizeEqualToSize(metalLayer.drawableSize, want)) {
        metalLayer.drawableSize = want;
    }
}

- (rm::ui::UiViewport)rmUiViewport {
    const float width = static_cast<float>(std::max(self.bounds.size.width, 1.0));
    const float height = static_cast<float>(std::max(self.bounds.size.height, 1.0));
    const CGFloat backing =
        self.window != nil && self.window.backingScaleFactor > 0.0
            ? self.window.backingScaleFactor
            : 1.0;
    const NSEdgeInsets insets = self.safeAreaInsets;
    const float left = static_cast<float>(std::max(insets.left, 0.0));
    const float top = static_cast<float>(std::max(insets.top, 0.0));
    const float right = static_cast<float>(std::max(insets.right, 0.0));
    const float bottom = static_cast<float>(std::max(insets.bottom, 0.0));
    return rm::ui::UiViewport::withSafeContent(
        width, height, static_cast<float>(backing),
        {left, top, std::max(0.0f, width - left - right),
         std::max(0.0f, height - top - bottom)},
        self.userHudScale > 0.0f ? self.userHudScale : 1.0f);
}

- (void)rmSyncUiViewport {
    if (self.renderer == nullptr) {
        return;
    }
    self.renderer->setUiViewport([self rmUiViewport]);
}

/// Every resize comes through here, live-drag steps included.
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self rmSyncDrawableSize];
    // The magnification is a function of the bounds, so a resize changes it — and the faces
    // are rasterised for it. Without this a window dragged larger keeps its old atlas and the
    // text alone stays the size it was while every rectangle around it grows.
    [self rmSyncUiViewport];
}

/// The window changed screens (or the screen changed scale): re-match the backing scale, or
/// the render is silently half resolution on the new display — and then resync the drawable,
/// because contentsScale is the other half of its size.
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    CAMetalLayer* metalLayer = static_cast<CAMetalLayer*>(self.layer);
    if (metalLayer != nil && self.window != nil && self.window.backingScaleFactor > 0.0) {
        metalLayer.contentsScale = self.window.backingScaleFactor;
        [self rmSyncUiViewport];
    }
    [self rmSyncDrawableSize];
}

/// How far the pointer may move between press and release and still count as a
/// click, in points. A few points absorbs the shake of an ordinary click
/// without swallowing a deliberate drag — the same order of magnitude as
/// AppKit's own drag thresholds.
static constexpr CGFloat kClickSlopPoints = 3.0;

/// A point in WINDOW coordinates, in AppKit logical points with a top-left origin. See
/// `MouseModifiers::pointX` and ADR-057.
///
/// ONE FUNCTION, called by both the click path and `Window::cursor`, because the two must agree
/// exactly: a cell that lights under the cursor and a cell that a click acts on have to be the
/// same cell, and two copies of a conversion that must agree are two copies that will not.
static std::array<float, 2> viewPointIn(NSView* view, NSPoint windowPoint) {
    const NSPoint local = [view convertPoint:windowPoint fromView:nil];
    // Flipped because the view is bottom-left and the HUD lays out from the top. No backing
    // scale here; UiViewport performs the separate authored-HUD conversion later.
    return {{static_cast<float>(local.x),
             static_cast<float>(view.bounds.size.height - local.y)}};
}

/// Turns a mouse event into a world ray and hands it to the app.
- (void)reportClick:(NSEvent*)event button:(rm::MouseButton)button {
    [self reportClick:event button:button modifierFlags:event.modifierFlags];
}

/// The deferred Shift-right path keeps the press-time modifiers: changing a key halfway through
/// a click must not turn a queued order into a plain one, or an order into a camera pan.
- (void)reportClick:(NSEvent*)event
              button:(rm::MouseButton)button
       modifierFlags:(NSEventModifierFlags)modifierFlags {
    if (self.renderer == nullptr || self.clickCallback == nullptr || !*self.clickCallback) {
        return;
    }

    // Window coordinates to this view's, which are bottom-left origin and in
    // POINTS — the same units screenRay wants. Handing it backing pixels here
    // is the classic Retina bug: picking would work only in the lower-left
    // quarter of the window.
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];

    const rm::Ray ray = rm::screenRay(self.renderer->camera(), static_cast<float>(local.x),
                                      static_cast<float>(local.y),
                                      static_cast<float>(self.bounds.size.width),
                                      static_cast<float>(self.bounds.size.height));
    const std::array<float, 2> point = viewPointIn(self, event.locationInWindow);
    const rm::MouseModifiers mods{
        .pointX = point[0],
        .pointY = point[1],
        .shift = (modifierFlags & NSEventModifierFlagShift) != 0,
        .command = (modifierFlags & NSEventModifierFlagCommand) != 0,
        .control = (modifierFlags & NSEventModifierFlagControl) != 0,
        .clicks = static_cast<int>(event.clickCount),
    };
    (*self.clickCallback)(ray, button, mods);
}

/// Reports a printable keypress, lowercased, to the app.
///
/// Deliberately a plain char rather than an NSEvent or a key code: the app is
/// C++ that includes no AppKit, and every use so far is "did the user press r".
/// Modifiers are not forwarded — a toggle that needed one would be a menu item,
/// not a keypress.
///
/// Nothing is passed to super, so AppKit does not beep at an unhandled key.
- (char)charFor:(NSEvent*)event {
    NSString* characters = event.charactersIgnoringModifiers;
    if (characters.length == 0) {
        return 0;
    }
    const unichar first = [characters characterAtIndex:0];
    if (first > 127) {
        return 0;  // not something a `char` can carry
    }
    return static_cast<char>(std::tolower(static_cast<int>(first)));
}

- (void)keyDown:(NSEvent*)event {
    const char key = [self charFor:event];
    if (key == 0) {
        return;
    }

    // The HELD set first, and it is updated even when nothing is listening: the state has
    // to be right whether or not a callback happens to be installed, or a key held while
    // a mode changes is stuck down forever.
    //
    // AppKit repeats keyDown while a key is held, so a repeat must not be reported as a
    // fresh press — a pan that re-triggered on every repeat would accelerate the longer
    // it ran.
    const bool repeat = self.heldKeys != nullptr && self.heldKeys->contains(key);
    if (self.heldKeys != nullptr) {
        self.heldKeys->insert(key);
    }
    if (!repeat && self.keyStateCallback != nullptr && *self.keyStateCallback) {
        (*self.keyStateCallback)(key, true);
    }

    // The tap callback still fires on a repeat, because that is what a key-repeat is for
    // in a text-like binding, and every current use is a toggle where a repeat is
    // harmless.
    if (self.keyCallback != nullptr && *self.keyCallback) {
        (*self.keyCallback)(key);
    }
}

- (void)keyUp:(NSEvent*)event {
    const char key = [self charFor:event];
    if (key == 0) {
        return;
    }
    if (self.heldKeys != nullptr) {
        self.heldKeys->erase(key);
    }
    if (self.keyStateCallback != nullptr && *self.keyStateCallback) {
        (*self.keyStateCallback)(key, false);
    }
}

- (void)mouseDown:(NSEvent*)event {
    _travelSincePress = 0.0;
    _leftDown = YES;
    // LOGICAL POINTS, matching `cursor()`: UiViewport converts both the press and current point
    // before the band-select and panel hit tests compare them with projected world positions.
    _leftDownAtPoint = viewPointIn(self, event.locationInWindow);
}

- (void)mouseUp:(NSEvent*)event {
    _leftDown = NO;
    if (_travelSincePress <= kClickSlopPoints) {
        [self reportClick:event button:rm::MouseButton::Left];
    }
}

- (BOOL)leftDown {
    return _leftDown;
}

- (std::array<float, 2>)leftDownAtPoint {
    return _leftDownAtPoint;
}

- (void)rightMouseDown:(NSEvent*)event {
    _travelSincePress = 0.0;
    _rightModifiersAtPress = event.modifierFlags;
    _rightReportedOnDown =
        (_rightModifiersAtPress & NSEventModifierFlagShift) == 0;
    if (_rightReportedOnDown) {
        [self reportClick:event button:rm::MouseButton::Right];
    }
}

- (void)rightMouseUp:(NSEvent*)event {
    if (!_rightReportedOnDown && _travelSincePress <= kClickSlopPoints) {
        [self reportClick:event
                    button:rm::MouseButton::Right
             modifierFlags:_rightModifiersAtPress];
    }
}

- (void)mouseDragged:(NSEvent*)event {
    _travelSincePress += std::abs(event.deltaX) + std::abs(event.deltaY);
    if (self.renderer == nullptr) {
        return;
    }
    // Shift is the trackpad's way in: a right-drag needs a second button, and a
    // two-finger click-drag on a trackpad is awkward enough that binding pan to
    // it alone would leave laptop use without a pan at all.
    if ((event.modifierFlags & NSEventModifierFlagShift) != 0) {
        [self panBy:event];
        return;
    }

    // ROTATION IS HELD BEHIND SPACE, because left-drag belongs to selection in an RTS and
    // a camera that swings when you meant to select is the single most disorienting thing
    // an RTS camera can do. Space is the modal key: hold it and the mouse turns the view,
    // let go and the view returns to overhead (see the space handler in main).
    if (self.heldKeys == nullptr || !self.heldKeys->contains(' ')) {
        return;
    }

    // Tuned so a drag across the window is a little under a half-turn. Dragging
    // right swings the camera right (the world appears to move left), which is
    // the convention Recoil and most RTS cameras use.
    constexpr float kRadiansPerPoint = 0.008f;
    self.renderer->camera().orbit(static_cast<float>(-event.deltaX) * kRadiansPerPoint,
                                  static_cast<float>(event.deltaY) * kRadiansPerPoint);
}

// Right-drag pans only with a modifier. Bare right-drag is left alone because the press has
// already reported its order; waiting to distinguish it from a drag was avoidable input latency.
- (void)rightMouseDragged:(NSEvent*)event {
    _travelSincePress += std::abs(event.deltaX) + std::abs(event.deltaY);
    if ((_rightModifiersAtPress & NSEventModifierFlagShift) != 0) {
        [self panBy:event];
    }
}

// Drags the ground under the cursor, rather than nudging the camera by a tuned
// constant. Because the step is derived from the frustum's width at the target
// (OrbitCamera::elmosPerPoint), terrain keeps pace with the pointer at every
// zoom level — there is no sensitivity constant here to get wrong.
- (void)panBy:(NSEvent*)event {
    if (self.renderer == nullptr) {
        return;
    }
    rm::OrbitCamera& camera = self.renderer->camera();
    const float scale = camera.elmosPerPoint(static_cast<float>(self.bounds.size.height));

    // Both signs are negated because the camera moves opposite to the ground:
    // pulling the terrain right means walking the target left. AppKit's deltaY
    // is positive downwards, which is the same sense `orbit` above relies on.
    camera.pan(static_cast<float>(-event.deltaX) * scale,
               static_cast<float>(event.deltaY) * scale);
}

- (void)scrollWheel:(NSEvent*)event {
    if (self.renderer == nullptr) {
        return;
    }
    // Exponential zoom: each notch multiplies the distance, so the step feels
    // the same whether you are 100 or 10 000 elmos out. A linear step would
    // crawl when far away and overshoot when close.
    constexpr float kZoomPerPoint = 0.04f;
    const float factor = std::exp(static_cast<float>(-event.scrollingDeltaY) * kZoomPerPoint);

    // ANCHORED TO THE CURSOR: the point under the pointer stays under the pointer, which is
    // what lets a player dive at the thing they are looking at instead of at the middle of
    // the screen and then hunting for it. The anchor is the cursor ray's intersection with
    // the TARGET'S OWN HEIGHT PLANE rather than the terrain: this view has no heightfield,
    // and at RTS pitches the plane differs from the ground by a correction the next wheel
    // notch re-applies anyway — while a terrain pick would make zoom feel different over a
    // hill than beside it.
    //
    // The arithmetic is the standard anchored-zoom identity: after scaling the distance by
    // `factor`, moving the target to `anchor + (target - anchor) * factor` keeps the
    // anchor's screen position fixed. Zooming OUT (factor > 1) walks the target away from
    // the cursor by the same identity, which is what makes a dive reversible.
    rm::OrbitCamera& camera = self.renderer->camera();
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    const rm::Ray ray = rm::screenRay(camera, static_cast<float>(local.x),
                                      static_cast<float>(local.y),
                                      static_cast<float>(self.bounds.size.width),
                                      static_cast<float>(self.bounds.size.height));

    // The APPLIED factor, not the requested one: `zoom` clamps the distance at both ends,
    // and shifting the target by a factor the distance did not actually move by would make
    // the view slide sideways at the zoom limits.
    const float before = camera.distance;
    camera.zoom(factor);
    const float applied = before > 0.0f ? camera.distance / before : 1.0f;

    if (std::abs(ray.direction.y) > 1e-4f) {
        const float t = (camera.target.y - ray.origin.y) / ray.direction.y;
        if (t > 0.0f) {
            const simd_float3 anchor = ray.origin + ray.direction * t;
            camera.target = anchor + (camera.target - anchor) * applied;
        }
    }
}

@end

struct rm::Window::Impl {
    NSWindow* window;                 // owned (ARC)
    RMTerrainView* view;              // owned (ARC), also the window's content view
    CAMetalDisplayLink* displayLink;  // owned (ARC)
    RMDisplayLinkDelegate* delegate;  // owned (ARC)
    std::unique_ptr<rm::Renderer> renderer;

    // The app's hooks. Held here — rather than copied into the ObjC objects —
    // so that installing one after construction takes effect immediately: the
    // view and the delegate hold pointers to these very objects.
    std::function<void(float)> frameCallback;
    std::function<void(const rm::Ray&, rm::MouseButton, rm::MouseModifiers)> clickCallback;
    std::function<void(char)> keyCallback;
    std::function<void(char, bool)> keyStateCallback;
    std::set<char> heldKeys;

    Impl(int width, int height, const char* title) {
        constexpr NSUInteger style = NSWindowStyleMaskTitled
                                   | NSWindowStyleMaskClosable
                                   | NSWindowStyleMaskMiniaturizable
                                   | NSWindowStyleMaskResizable;
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        [window setTitle:[NSString stringWithUTF8String:title]];
        [window center];

        view = [[RMTerrainView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        [window setContentView:view];
        [window makeFirstResponder:view];
        [window setContentMinSize:NSMakeSize(1280, 720)];

        // Order matters: wantsLayer first, then assign — the reverse creates
        // a layer-hosting view and your layer never displays.
        [view setWantsLayer:YES];
        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        // Match the backing scale or Metal renders at half resolution on
        // Retina and everything is silently blurry.
        [metalLayer setContentsScale:[[window screen] backingScaleFactor]];
        [view setLayer:metalLayer];

        // Redraw during live resize rather than stretching the last frame.
        [view setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];

        // The first drawableSize. Without this it stays 0x0 until the first -nextDrawable and
        // the first world frame would render into a stale or empty target.
        [view rmSyncDrawableSize];

        // metal-cpp types are layout-compatible with their ObjC twins by
        // design (Apple's metal-cpp README). __bridge = pointer cast with no
        // ownership change — the layer stays owned by the content view.
        renderer = std::make_unique<rm::Renderer>(
            (__bridge CA::MetalLayer*)metalLayer);
        view.renderer = renderer.get();
        [view rmSyncUiViewport];
        view.clickCallback = &clickCallback;
        view.keyCallback = &keyCallback;
        view.keyStateCallback = &keyStateCallback;
        view.heldKeys = &heldKeys;

        delegate = [[RMDisplayLinkDelegate alloc] init];
        delegate.renderer = renderer.get();
        delegate.frameCallback = &frameCallback;

        displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:metalLayer];
        displayLink.delegate = delegate;
        [displayLink addToRunLoop:[NSRunLoop currentRunLoop]
                          forMode:NSRunLoopCommonModes];
    }

    ~Impl() {
        // Stop callbacks BEFORE the renderer dies — both the delegate and the
        // view hold raw pointers to it, and to the callbacks below, which are
        // destroyed with this object.
        [displayLink invalidate];
        view.renderer = nullptr;
        view.clickCallback = nullptr;
        view.keyCallback = nullptr;
        view.keyStateCallback = nullptr;
        view.heldKeys = nullptr;
        delegate.frameCallback = nullptr;
        renderer.reset();
    }
};

namespace rm {

Window::Window(int width, int height, const char* title)
    : impl_{std::make_unique<Impl>(width, height, title)}
{}

Window::~Window() = default;
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;

void Window::setTerrain(const TerrainMesh& mesh) {
    impl_->renderer->setTerrain(mesh);
}

void Window::setGroundTexture(const TileAtlas& atlas) {
    impl_->renderer->setGroundTexture(atlas);
}

void Window::beginBenchmark(std::size_t warmupFrames) {
    impl_->renderer->beginBenchmark(warmupFrames);
}

std::size_t Window::recordedFrames() const {
    return impl_->renderer->recordedFrames();
}

bench::FrameRecorder Window::benchmarkSnapshot() const {
    return impl_->renderer->benchmarkSnapshot();
}

void Window::setGroundColourMap(const ColourImage& image) {
    impl_->renderer->setGroundColourMap(image);
}

void Window::setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                      const dds::Texture& maskB) {
    impl_->renderer->setSplat(layers, maskA, maskB);
}

void Window::setEnvironment(const Renderer::Environment& environment) {
    impl_->renderer->setEnvironment(environment);
}

void Window::setWater(bool enabled, float levelElmos) {
    impl_->renderer->setWater(enabled, levelElmos);
}

void Window::setProps(std::span<const dds::Texture> textures,
                      std::span<const PropBatch> batches) {
    impl_->renderer->setProps(textures, batches);
}

void Window::setFog(std::span<const std::uint16_t> counts, int squaresX, int squaresZ,
                    float widthElmos, float depthElmos) {
    impl_->renderer->setFog(counts, squaresX, squaresZ, widthElmos, depthElmos);
}

void Window::clearFog() noexcept { impl_->renderer->clearFog(); }

void Window::setUnits(std::span<const dds::Texture> textures,
                      std::span<const UnitBatch> batches) {
    impl_->renderer->setUnits(textures, batches);
}

void Window::setInstances(std::size_t batchIndex, std::span<const UnitInstance> instances) {
    impl_->renderer->setInstances(batchIndex, instances);
}

void Window::onFrame(std::function<void(float seconds)> callback) {
    impl_->frameCallback = std::move(callback);
}

void Window::onClick(
    std::function<void(const Ray& ray, MouseButton button, MouseModifiers mods)> callback) {
    impl_->clickCallback = std::move(callback);
}

void Window::onKey(std::function<void(char key)> callback) {
    impl_->keyCallback = std::move(callback);
}

void Window::onKeyState(std::function<void(char key, bool pressed)> callback) {
    impl_->keyStateCallback = std::move(callback);
}

bool Window::keyHeld(char key) const { return impl_->heldKeys.contains(key); }

std::array<float, 2> Window::cursor() const {
    NSWindow* window = impl_->view.window;
    if (window == nil) {
        return {{-1.0f, -1.0f}};  // no window, no cursor: a point that misses every hit test
    }

    // `mouseLocationOutsideOfEventStream` is the current position without an event and without
    // a tracking area — which is the whole reason this can be a poll. It is in WINDOW
    // coordinates, so it goes through the very same conversion a click does.
    //
    // LOGICAL POINTS, not authored HUD space: screenRay consumes these directly and UiViewport
    // converts the same point for hit tests.
    return viewPointIn(impl_->view, window.mouseLocationOutsideOfEventStream);
}

void Window::setReflections(bool enabled) {
    impl_->renderer->setReflections(enabled);
}

bool Window::reflectionsEnabled() const {
    return impl_->renderer->reflectionsEnabled();
}

void Window::setRefraction(bool enabled) {
    impl_->renderer->setRefraction(enabled);
}

bool Window::refractionEnabled() const {
    return impl_->renderer->refractionEnabled();
}

void Window::setPropsVisible(bool visible) {
    impl_->renderer->setPropsVisible(visible);
}

bool Window::propsVisible() const {
    return impl_->renderer->propsVisible();
}

void Window::setStratumNormals(bool enabled) {
    impl_->renderer->setStratumNormals(enabled);
}

bool Window::stratumNormalsEnabled() const {
    return impl_->renderer->stratumNormalsEnabled();
}

void Window::setGroundDecals(std::span<const DecalVertex> vertices) {
    impl_->renderer->setGroundDecals(vertices);
}

void Window::setConstructions(std::span<const Renderer::ConstructionDraw> sites) noexcept {
    impl_->renderer->setConstructions(sites);
}

void Window::setConstructionTime(float seconds) noexcept {
    impl_->renderer->setConstructionTime(seconds);
}

bool Window::leftMouseHeld() const { return [impl_->view leftDown]; }

std::array<float, 2> Window::dragOrigin() const { return [impl_->view leftDownAtPoint]; }

bool Window::controlHeldNow() const {
    return ([NSEvent modifierFlags] & NSEventModifierFlagControl) != 0;
}

bool Window::shiftHeldNow() const {
    return ([NSEvent modifierFlags] & NSEventModifierFlagShift) != 0;
}

void Window::setGhost(std::size_t batch, const UnitInstance& instance,
                      std::array<float, 4> tint) noexcept {
    impl_->renderer->setGhost(batch, instance, tint);
}

void Window::clearGhost() noexcept { impl_->renderer->clearGhost(); }

void Window::setSelection(std::span<const SelectionEntry> selected) {
    impl_->renderer->setSelection(selected);
}

void Window::setParticles(std::span<const Particle> particles) {
    impl_->renderer->setParticles(particles);
}

void Window::focusOn(std::array<float, 3> target, float distance) {
    impl_->renderer->focusOn(target, distance);
}

OrbitCamera& Window::camera() { return impl_->renderer->camera(); }

unsigned int Window::width() const {
    return static_cast<unsigned int>(std::max(1.0, impl_->view.bounds.size.width));
}

unsigned int Window::height() const {
    return static_cast<unsigned int>(std::max(1.0, impl_->view.bounds.size.height));
}

void Window::setUserHudScale(float scale) {
    impl_->view.userHudScale =
        std::clamp(scale, rm::ui::kMinUserHudScale, rm::ui::kMaxUserHudScale);
    [impl_->view rmSyncUiViewport];
}

float Window::userHudScale() const noexcept { return impl_->view.userHudScale; }

ui::UiViewport Window::uiViewport() const { return [impl_->view rmUiViewport]; }

text::Font Window::labelFont() const { return impl_->renderer->labelFont(); }

text::Font Window::readoutFont() const { return impl_->renderer->readoutFont(); }

void Window::setWaterWaveTexture(const dds::Texture& normal) {
    impl_->renderer->setWaterWaveTexture(normal);
}

void Window::setMinimapImage(const dds::Texture& image) {
    impl_->renderer->setMinimapImage(image);
}

void Window::setMinimapRect(float x, float y, float width, float height) noexcept {
    impl_->renderer->setMinimapRect(x, y, width, height);
}

void Window::setIconAtlas(const dds::Texture& atlas) {
    impl_->renderer->setIconAtlas(atlas);
}

void Window::setHud(const ui::Geometry& geometry) {
    impl_->renderer->setHud(geometry);
}

void Window::show() {
    [impl_->window makeKeyAndOrderFront:nil];
}

} // namespace rm
