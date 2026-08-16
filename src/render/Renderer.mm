// The PRIVATE_IMPLEMENTATION defines instantiate metal-cpp's inline
// implementations. They must appear in EXACTLY ONE translation unit in the
// whole program — this one (AGENT.md gotchas).
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "render/Renderer.hpp"

#include "core/ClearColor.hpp"
#include "core/Error.hpp"

#include <string>

namespace {

// Milestone-1 smoke shader. It draws nothing — its entire job is to prove
// that RUNTIME shader compilation works on this machine. There is no Xcode
// installed, hence no offline `metal` compiler; Metal's runtime compiler
// ships with macOS itself and compiles from source on demand (which will
// also give us shader hot-reload for free later, no build step).
constexpr const char* kSmokeShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

vertex float4 smokeVertex() {
    return float4(0.0, 0.0, 0.0, 1.0);
}

fragment float4 smokeFragment() {
    return float4(1.0, 0.0, 1.0, 1.0);
}
)MSL";

// Wraps NS::Error text into our exception type. [[noreturn]] so call sites
// read as plain control flow: `if (!x) throwMetalError(...);`.
[[noreturn]] void throwMetalError(const char* what, NS::Error* err) {
    std::string message{what};
    if (err != nullptr && err->localizedDescription() != nullptr) {
        message += ": ";
        message += err->localizedDescription()->utf8String();
    }
    throw rm::RendererError{message};
}

} // namespace

namespace rm {

Renderer::Renderer(CA::MetalLayer* layer)
    : layer_{layer}
{
    // MTL::CreateSystemDefaultDevice wraps the C function
    // MTLCreateSystemDefaultDevice, which by Cocoa convention returns a
    // +1 (retained) object → released in ~Renderer.
    device_ = MTL::CreateSystemDefaultDevice();
    if (device_ == nullptr) {
        throw RendererError{"no Metal-capable GPU found"};
    }

    layer_->setDevice(device_);
    // BGRA8 is the layer default on macOS and what Recoil-era content
    // assumes; stated explicitly because we match pipeline formats to it.
    layer_->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm);

    commandQueue_ = device_->newCommandQueue();
    if (commandQueue_ == nullptr) {
        throw RendererError{"failed to create Metal command queue"};
    }

    // --- Smoke pipeline: the no-Xcode toolchain proof ---------------------
    NS::Error* error = nullptr;
    MTL::Library* library = device_->newLibrary(
        NS::String::string(kSmokeShaderSource, NS::UTF8StringEncoding),
        nullptr, // default compile options
        &error);
    if (library == nullptr) {
        throwMetalError("runtime shader compilation failed", error);
    }

    MTL::Function* vertexFn = library->newFunction(
        NS::String::string("smokeVertex", NS::UTF8StringEncoding));
    MTL::Function* fragmentFn = library->newFunction(
        NS::String::string("smokeFragment", NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    descriptor->setFragmentFunction(fragmentFn);
    descriptor->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormat::PixelFormatBGRA8Unorm);

    smokePipeline_ = device_->newRenderPipelineState(descriptor, &error);

    // The pipeline retains the functions, so every temporary is released now.
    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (fragmentFn != nullptr) fragmentFn->release();
    library->release();

    if (smokePipeline_ == nullptr) {
        throwMetalError("failed to create smoke render pipeline", error);
    }
}

Renderer::~Renderer() {
    // Reverse acquisition order; all are +1 objects from newXxx()/CreateXxx.
    smokePipeline_->release();
    commandQueue_->release();
    device_->release();
}

void Renderer::drawFrame(double t, CA::MetalDrawable* drawable) noexcept {
    // The drawable is autoreleased by the display link, so every frame needs
    // its own pool or frame objects accumulate until the app exits.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    if (drawable != nullptr) {
        const Rgba clear = animatedClearColor(t);

        MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
        MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
        color0->setTexture(drawable->texture());
        color0->setLoadAction(MTL::LoadAction::LoadActionClear);
        color0->setStoreAction(MTL::StoreAction::StoreActionStore);
        color0->setClearColor(MTL::ClearColor::Make(clear.r, clear.g, clear.b, clear.a));

        // The encoder exists (and must end) even with zero draw calls: a
        // render pass with LoadActionClear is what performs the clear.
        MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
        encoder->endEncoding();

        commandBuffer->presentDrawable(drawable);
        commandBuffer->commit();

        pass->release();
    }

    pool->release();
}

} // namespace rm
