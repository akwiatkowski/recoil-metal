#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h> // macOS 14+; replaces CVDisplayLink

#include "platform/Window.hpp"

#include "render/Renderer.hpp"

#include <QuartzCore/QuartzCore.hpp> // metal-cpp decl of CA::MetalLayer (no impl defines here!)

#include <cmath>

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
@end

@implementation RMDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink*)link
            needsUpdate:(CAMetalDisplayLinkUpdate*)update {
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
@end

@implementation RMTerrainView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)mouseDragged:(NSEvent*)event {
    if (self.renderer == nullptr) {
        return;
    }
    // Tuned so a drag across the window is a little under a half-turn. Dragging
    // right swings the camera right (the world appears to move left), which is
    // the convention Recoil and most RTS cameras use.
    constexpr float kRadiansPerPoint = 0.008f;
    self.renderer->camera().orbit(static_cast<float>(-event.deltaX) * kRadiansPerPoint,
                                  static_cast<float>(event.deltaY) * kRadiansPerPoint);
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

        delegate = [[RMDisplayLinkDelegate alloc] init];
        delegate.renderer = renderer.get();

        displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:metalLayer];
        displayLink.delegate = delegate;
        [displayLink addToRunLoop:[NSRunLoop currentRunLoop]
                          forMode:NSRunLoopCommonModes];
    }

    ~Impl() {
        // Stop callbacks BEFORE the renderer dies — both the delegate and the
        // view hold raw pointers to it.
        [displayLink invalidate];
        view.renderer = nullptr;
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

void Window::setUnits(std::span<const dds::Texture> textures,
                      std::span<const UnitBatch> batches) {
    impl_->renderer->setUnits(textures, batches);
}

void Window::focusOn(std::array<float, 3> target, float distance) {
    impl_->renderer->focusOn(target, distance);
}

void Window::show() {
    [impl_->window makeKeyAndOrderFront:nil];
}

} // namespace rm
