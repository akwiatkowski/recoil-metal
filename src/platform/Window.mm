#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h> // macOS 14+; replaces CVDisplayLink

#include "platform/Window.hpp"

#include "render/Renderer.hpp"

#include <QuartzCore/QuartzCore.hpp> // metal-cpp decl of CA::MetalLayer (no impl defines here!)

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
@property(nonatomic, assign) CFTimeInterval startTime;
@end

@implementation RMDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink*)link
            needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    // The drawable comes from the update — with CAMetalDisplayLink, calling
    // -nextDrawable yourself throws CAMetalLayerInvalidOperation.
    self.renderer->drawFrame(CACurrentMediaTime() - self.startTime,
                             (__bridge CA::MetalDrawable*)update.drawable);
}
@end

struct rm::Window::Impl {
    NSWindow* window;                 // owned (ARC)
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

        // Order matters: wantsLayer first, then assign — the reverse creates
        // a layer-hosting view and your layer never displays.
        [[window contentView] setWantsLayer:YES];
        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        // Match the backing scale or Metal renders at half resolution on
        // Retina and everything is silently blurry.
        [metalLayer setContentsScale:[[window screen] backingScaleFactor]];
        [[window contentView] setLayer:metalLayer];

        // metal-cpp types are layout-compatible with their ObjC twins by
        // design (Apple's metal-cpp README). __bridge = pointer cast with no
        // ownership change — the layer stays owned by the content view.
        renderer = std::make_unique<rm::Renderer>(
            (__bridge CA::MetalLayer*)metalLayer);

        delegate = [[RMDisplayLinkDelegate alloc] init];
        delegate.renderer = renderer.get();
        delegate.startTime = CACurrentMediaTime();

        displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:metalLayer];
        displayLink.delegate = delegate;
        [displayLink addToRunLoop:[NSRunLoop currentRunLoop]
                          forMode:NSRunLoopCommonModes];
    }

    ~Impl() {
        // Stop callbacks BEFORE the renderer dies — the delegate holds a raw
        // pointer to it.
        [displayLink invalidate];
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

void Window::show() {
    [impl_->window makeKeyAndOrderFront:nil];
}

} // namespace rm
