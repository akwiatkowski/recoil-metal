#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h> // macOS 14+; replaces CVDisplayLink

#include "platform/Window.hpp"

#include "render/Renderer.hpp"

#include <QuartzCore/QuartzCore.hpp> // metal-cpp decl of CA::MetalLayer (no impl defines here!)

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
    const std::function<void(const rm::Ray&, rm::MouseButton)>* clickCallback;
@end

@implementation RMTerrainView {
    // Points travelled since the current button went down. A press and release
    // that stayed under the slop is a click; anything more was a camera drag
    // and must not also fire an order.
    CGFloat _travelSincePress;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

/// How far the pointer may move between press and release and still count as a
/// click, in points. A few points absorbs the shake of an ordinary click
/// without swallowing a deliberate drag — the same order of magnitude as
/// AppKit's own drag thresholds.
static constexpr CGFloat kClickSlopPoints = 3.0;

/// Turns a mouse event into a world ray and hands it to the app.
- (void)reportClick:(NSEvent*)event button:(rm::MouseButton)button {
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
    (*self.clickCallback)(ray, button);
}

- (void)mouseDown:(NSEvent*)event {
    (void)event;
    _travelSincePress = 0.0;
}

- (void)mouseUp:(NSEvent*)event {
    if (_travelSincePress <= kClickSlopPoints) {
        [self reportClick:event button:rm::MouseButton::Left];
    }
}

- (void)rightMouseDown:(NSEvent*)event {
    (void)event;
    _travelSincePress = 0.0;
}

- (void)rightMouseUp:(NSEvent*)event {
    if (_travelSincePress <= kClickSlopPoints) {
        [self reportClick:event button:rm::MouseButton::Right];
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
    // Tuned so a drag across the window is a little under a half-turn. Dragging
    // right swings the camera right (the world appears to move left), which is
    // the convention Recoil and most RTS cameras use.
    constexpr float kRadiansPerPoint = 0.008f;
    self.renderer->camera().orbit(static_cast<float>(-event.deltaX) * kRadiansPerPoint,
                                  static_cast<float>(event.deltaY) * kRadiansPerPoint);
}

- (void)rightMouseDragged:(NSEvent*)event {
    _travelSincePress += std::abs(event.deltaX) + std::abs(event.deltaY);
    [self panBy:event];
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
    self.renderer->camera().zoom(factor);
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
    std::function<void(const rm::Ray&, rm::MouseButton)> clickCallback;

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

        // Order matters: wantsLayer first, then assign — the reverse creates
        // a layer-hosting view and your layer never displays.
        [view setWantsLayer:YES];
        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        // Match the backing scale or Metal renders at half resolution on
        // Retina and everything is silently blurry.
        [metalLayer setContentsScale:[[window screen] backingScaleFactor]];
        [view setLayer:metalLayer];

        // Resize the drawable with the view, otherwise the terrain stretches
        // whenever the window changes size.
        [view setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];

        // metal-cpp types are layout-compatible with their ObjC twins by
        // design (Apple's metal-cpp README). __bridge = pointer cast with no
        // ownership change — the layer stays owned by the content view.
        renderer = std::make_unique<rm::Renderer>(
            (__bridge CA::MetalLayer*)metalLayer);

        view.renderer = renderer.get();
        view.clickCallback = &clickCallback;

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

void Window::setWater(bool enabled, float levelElmos) {
    impl_->renderer->setWater(enabled, levelElmos);
}

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

void Window::onClick(std::function<void(const Ray& ray, MouseButton button)> callback) {
    impl_->clickCallback = std::move(callback);
}

void Window::focusOn(std::array<float, 3> target, float distance) {
    impl_->renderer->focusOn(target, distance);
}

void Window::show() {
    [impl_->window makeKeyAndOrderFront:nil];
}

} // namespace rm
