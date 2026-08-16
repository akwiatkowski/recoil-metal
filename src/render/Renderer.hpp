#pragma once

// Forward declarations keep Metal types out of the header: anything including
// this file stays pure C++ and test-compilable without frameworks.
namespace CA { class MetalLayer; class MetalDrawable; }
namespace MTL { class Device; class CommandQueue; class RenderPipelineState; }

namespace rm {

// Owns every long-lived Metal object — device, command queue, pipelines —
// and renders frames into the window's CAMetalLayer.
//
// Ownership follows Cocoa rules, which metal-cpp exposes raw (no ARC for C++
// objects): newXxx() results are +1 and released in the destructor, in
// reverse acquisition order.
class Renderer {
public:
    // layer is NOT owned; it belongs to the window's content view and must
    // outlive the Renderer.
    explicit Renderer(CA::MetalLayer* layer);
    ~Renderer();

    // GPU resources are unique and the display link holds a raw pointer to
    // this object, so neither copying nor moving is safe.
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Renders one frame into drawable; t is seconds since window creation
    // (drives the milestone-1 colour cycle). The drawable comes FROM the
    // CAMetalDisplayLinkUpdate — with CAMetalDisplayLink, calling
    // nextDrawable() yourself throws CAMetalLayerInvalidOperation; the link
    // owns the drawable pool (and the frame pacing with it).
    // noexcept: a throw mid-frame would escape into the ObjC display-link
    // callback, which is undefined territory.
    void drawFrame(double t, CA::MetalDrawable* drawable) noexcept;

private:
    CA::MetalLayer* layer_;               // not owned
    MTL::Device* device_;                 // owned
    MTL::CommandQueue* commandQueue_;     // owned
    MTL::RenderPipelineState* smokePipeline_; // owned
};

} // namespace rm
