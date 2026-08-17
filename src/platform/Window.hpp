#pragma once

#include "core/mesh/TerrainMesh.hpp"

#include <memory>

namespace rm {

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

    void show();

private:
    struct Impl;
    // unique_ptr to incomplete type: the destructor MUST be defined (even if
    // defaulted) in the .mm where Impl is complete, otherwise the inline
    // destructor here can't delete the pointee.
    std::unique_ptr<Impl> impl_;
};

} // namespace rm
