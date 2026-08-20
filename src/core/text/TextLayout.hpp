#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace rm::text {

// Turning a string into quads, and nothing else.
//
// The first interface this engine draws that is not part of the world. Everything so far
// has been in elmos on the ground — selection rings, order markers, wrecks — and a HUD is
// not: it is in PIXELS on the window, it does not move when the camera does, and it needs
// glyphs.
//
// THE SPLIT, which is what keeps this file testable. The glyphs themselves are rasterised
// by the platform (CoreText into a Metal texture — see Renderer), because that is where the
// fonts and the GPU are. Where each glyph GOES is arithmetic, so it is here, in core/, with
// tests. What that buys: a layout bug shows up as a failing assertion rather than as
// unreadable text in a screenshot, and the two are indistinguishable by eye at eleven
// pixels.

/// Where one glyph lives in the atlas, and how it sits on the line.
///
/// All in PIXELS at the size the atlas was baked, except the uv rectangle, which is
/// normalised because that is what a sampler wants.
struct Glyph {
    /// Normalised atlas rectangle: u0, v0, u1, v1.
    std::array<float, 4> uv{};

    /// The glyph's size on screen, in pixels.
    float width = 0.0f;
    float height = 0.0f;

    /// Offset from the pen position to the glyph's top-left corner. `bearingY` is usually
    /// NEGATIVE for a letter that sits on the baseline, since the pen is on the baseline and
    /// the glyph is above it.
    float bearingX = 0.0f;
    float bearingY = 0.0f;

    /// How far the pen moves after drawing this glyph. Not the same as `width`: a space has
    /// an advance and no width at all, which is exactly why the two are separate.
    float advance = 0.0f;
};

/// The characters the atlas covers: printable ASCII, space through tilde.
///
/// Ninety-five glyphs. Enough for every number, label and message this HUD will ever want,
/// and small enough that the atlas is one row of a small texture. Anything outside the range
/// is DROPPED rather than substituted — a box glyph would be inventing a character the font
/// does not have, and silently skipping is what a HUD wants over a line of tofu.
inline constexpr char kFirstGlyph = ' ';
inline constexpr char kLastGlyph = '~';
inline constexpr std::size_t kGlyphCount =
    static_cast<std::size_t>(kLastGlyph - kFirstGlyph) + 1;

/// One textured, coloured vertex of a glyph quad.
///
/// Position is in PIXELS from the window's top-left, which the shader turns into clip space
/// — so a caller places text where it means to rather than in a normalised space that
/// changes meaning when the window resizes.
struct TextVertex {
    std::array<float, 2> position{};  ///< pixels, top-left origin
    std::array<float, 2> uv{};
    std::array<float, 4> colour{};  ///< straight rgba
};

/// Appends the quads for `text`, with the pen starting at (`x`, `y`) — the BASELINE's left
/// end, not the top-left of the first letter.
///
/// Returns the pen's x after the last glyph, so a caller can chain runs of different colours
/// on one line without measuring twice.
///
/// `scale` multiplies the baked size. One is crisp because it is the size the atlas was
/// rasterised at; anything else resamples, so a HUD should prefer baking bigger to scaling
/// up.
float appendText(std::vector<TextVertex>& out, std::span<const Glyph> glyphs,
                 std::string_view text, float x, float y, std::array<float, 4> colour,
                 float scale = 1.0f);

/// How wide `text` would be, in pixels, without laying it out.
///
/// What right-aligning needs, and what centring needs twice. Measured from the ADVANCES
/// rather than the widths, so a trailing space counts — which is what makes measuring a
/// string and laying it out agree.
[[nodiscard]] float measureText(std::span<const Glyph> glyphs, std::string_view text,
                                float scale = 1.0f);

/// Vertices one glyph contributes: two triangles.
inline constexpr std::size_t kVerticesPerGlyph = 6;

/// The most text the renderer will draw in a frame, in vertices.
///
/// Six thousand is a thousand glyphs — far more HUD than this app will ever show, and a
/// third of a megabyte of buffer per frame in flight. Beyond it, text is DROPPED rather than
/// the buffer grown, on the same rule the particles and decals follow: a buffer cannot be
/// reallocated while the GPU may still be reading last frame's copy.
inline constexpr std::size_t kMaxTextVertices = kVerticesPerGlyph * 1000;

} // namespace rm::text
