// Text: the font atlases and the HUD's vertex buffers.
//
// The one domain with no world geometry in it at all — it draws in screen space, which is why
// it is the cleanest of the four to separate.
//
// ONE CLASS, SEVERAL TRANSLATION UNITS (PLAN2.md §7 P7.3). This file defines `Renderer`'s
// text and HUD members; it is not a separate object. That distinction is the honest state of the
// split: §7 asks for `MapRenderer`, `UnitRenderer`, `FxRenderer` and `UiRenderer` as Recoil has
// them (`CBaseGroundDrawer`, `CUnitDrawer`, `CProjectileDrawer`, `CMiniMap`), and four objects
// would mean deciding who owns the device, the command queue, the camera, the depth textures
// and the shadow map — every one of which all four need. Splitting the code first and the
// ownership second is the order that keeps every step verifiable: this one is provably
// behaviour-preserving, because it moves definitions between files and changes nothing else.
//
// The metal-cpp `*_PRIVATE_IMPLEMENTATION` defines stay in `Renderer.mm`, which must remain the
// one translation unit that instantiates them (AGENT.md gotchas).

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#import <CoreText/CoreText.h>

#include "render/RendererInternal.hpp"

#include "core/Error.hpp"
#include "core/camera/Frustum.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace rm {

// The uniform layouts, the buffer indices and the small helpers live in
// `RendererInternal.hpp` — see the note there on why they are not in `Renderer.hpp`.
using namespace render_detail;  // NOLINT(google-build-using-namespace)

void Renderer::buildFontAtlas(FontSlot& slot, const char* familyName, float points) {
    slot.glyphs.clear();
    slot.lineHeight = 0.0f;

    CFStringRef name =
        CFStringCreateWithCString(nullptr, familyName, kCFStringEncodingUTF8);
    CTFontRef font = name != nullptr ? CTFontCreateWithName(name, points, nullptr) : nullptr;
    if (name != nullptr) {
        CFRelease(name);
    }
    if (font == nullptr) {
        // Not fatal: `ui::build` degrades whatever this face was for. Reported, because a
        // missing face is otherwise indistinguishable from an interface with nothing to say.
        std::fprintf(stderr, "no HUD font \"%s\"; that part of the interface will not draw\n",
                     familyName);
        return;
    }

    const float ascent = static_cast<float>(CTFontGetAscent(font));
    const float descent = static_cast<float>(CTFontGetDescent(font));
    slot.lineHeight = ascent + descent + static_cast<float>(CTFontGetLeading(font));

    // Measure first, pack second. Every glyph in one row: 95 of them at ~11 pixels is about
    // 1100 wide, which is one modest texture and keeps the packing arithmetic to a running
    // sum rather than a bin-packer.
    std::array<CGGlyph, text::kGlyphCount> cgGlyphs{};
    std::array<UniChar, text::kGlyphCount> characters{};
    for (std::size_t i = 0; i < text::kGlyphCount; ++i) {
        characters[i] = static_cast<UniChar>(text::kFirstGlyph + static_cast<char>(i));
    }
    if (!CTFontGetGlyphsForCharacters(font, characters.data(), cgGlyphs.data(),
                                      static_cast<CFIndex>(text::kGlyphCount))) {
        // Partial coverage is still usable — appendText drops what it has no glyph for — so
        // this is a warning rather than a bail.
        std::fprintf(stderr, "the HUD font does not cover all of printable ASCII\n");
    }

    std::array<CGRect, text::kGlyphCount> bounds{};
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, cgGlyphs.data(),
                                    bounds.data(), static_cast<CFIndex>(text::kGlyphCount));
    std::array<CGSize, text::kGlyphCount> advances{};
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, cgGlyphs.data(),
                              advances.data(), static_cast<CFIndex>(text::kGlyphCount));

    // A SOLID BLOCK, reserved before the glyphs. Every panel, bevel, bar and bracket in the
    // interface is a quad whose uvs point at the middle of this — so the chrome and the letters
    // share one atlas, one bind and one draw, and there is no second pipeline to keep in step.
    // Four pixels rather than one so that sampling its centre is nowhere near an edge.
    constexpr int kSolidBlock = 4;

    int atlasWidth = kSolidBlock + 2 * kGlyphPadding;
    int atlasHeight = kSolidBlock + 2 * kGlyphPadding;
    for (const CGRect& box : bounds) {
        atlasWidth += static_cast<int>(std::ceil(box.size.width)) + 2 * kGlyphPadding;
        atlasHeight = std::max(atlasHeight,
                               static_cast<int>(std::ceil(box.size.height)) + 2 * kGlyphPadding);
    }
    atlasWidth = std::max(atlasWidth, 1);

    // An 8-bit COVERAGE bitmap, not colour: the glyph says how much of the vertex's colour
    // lands, which is what lets one atlas draw white text, red text and a shadow.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(atlasWidth * atlasHeight), 0);

    // The solid block, filled before CoreGraphics touches the bitmap: it is opaque coverage
    // rather than a glyph, so it is written rather than drawn.
    for (int row = kGlyphPadding; row < kGlyphPadding + kSolidBlock; ++row) {
        for (int col = kGlyphPadding; col < kGlyphPadding + kSolidBlock; ++col) {
            pixels[static_cast<std::size_t>(row * atlasWidth + col)] = 0xFF;
        }
    }
    slot.solidUv = {static_cast<float>(kGlyphPadding) / static_cast<float>(atlasWidth),
                    static_cast<float>(kGlyphPadding) / static_cast<float>(atlasHeight),
                    static_cast<float>(kGlyphPadding + kSolidBlock)
                        / static_cast<float>(atlasWidth),
                    static_cast<float>(kGlyphPadding + kSolidBlock)
                        / static_cast<float>(atlasHeight)};
    CGColorSpaceRef grey = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), static_cast<std::size_t>(atlasWidth), static_cast<std::size_t>(atlasHeight),
        8, static_cast<std::size_t>(atlasWidth), grey, kCGImageAlphaNone);
    CGColorSpaceRelease(grey);
    if (ctx == nullptr) {
        CFRelease(font);
        std::fprintf(stderr, "could not rasterise the HUD font\n");
        return;
    }
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, false);  // no subpixel: this is a single channel

    slot.glyphs.resize(text::kGlyphCount);

    // The glyphs start AFTER the solid block.
    int penX = kSolidBlock + 2 * kGlyphPadding;
    for (std::size_t i = 0; i < text::kGlyphCount; ++i) {
        const CGRect& box = bounds[i];
        const int w = static_cast<int>(std::ceil(box.size.width));
        const int h = static_cast<int>(std::ceil(box.size.height));

        if (w > 0 && h > 0) {
            // CoreGraphics draws from the BASELINE upward with y increasing up, while the
            // bitmap's rows run down. Placing the baseline at the glyph's own descent puts
            // the whole ink inside the row whatever the glyph is.
            const CGPoint at{static_cast<CGFloat>(penX) - box.origin.x,
                             static_cast<CGFloat>(kGlyphPadding) - box.origin.y};
            CTFontDrawGlyphs(font, &cgGlyphs[i], &at, 1, ctx);
        }

        // V IS FLIPPED. CoreGraphics draws bottom-up — y increases up from the bitmap's
        // last row — while `replaceRegion` uploads row 0 first and the sampler's v grows
        // downward. Every glyph's ink sits `kGlyphPadding` above the bitmap's BOTTOM, so
        // measured from the top it runs from `atlasHeight - padding - h` to
        // `atlasHeight - padding`.
        //
        // Getting this wrong does not look like a flip: the tallest glyph is nearly right
        // and every shorter one reads from a band it does not occupy, so the text comes out
        // legible and clipped, which reads as a font-size bug.
        const float vTop =
            static_cast<float>(atlasHeight - kGlyphPadding - h) / static_cast<float>(atlasHeight);
        const float vBottom =
            static_cast<float>(atlasHeight - kGlyphPadding) / static_cast<float>(atlasHeight);

        slot.glyphs[i] = text::Glyph{
            .uv = {static_cast<float>(penX) / static_cast<float>(atlasWidth), vTop,
                   static_cast<float>(penX + w) / static_cast<float>(atlasWidth), vBottom},
            .width = static_cast<float>(w),
            .height = static_cast<float>(h),
            // The ink's left edge relative to the pen, and its TOP relative to the baseline.
            // The second is negative for anything that rises above the baseline, which is
            // nearly everything — see Glyph.
            .bearingX = static_cast<float>(box.origin.x),
            .bearingY = -static_cast<float>(box.origin.y + box.size.height),
            .advance = static_cast<float>(advances[i].width),
        };

        penX += w + 2 * kGlyphPadding;
    }

    CGContextRelease(ctx);
    CFRelease(font);

    auto* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatR8Unorm, static_cast<NS::UInteger>(atlasWidth),
        static_cast<NS::UInteger>(atlasHeight), false);
    // A CLASS FACTORY, so its result is autoreleased and must NOT be released here — see
    // AGENT.md, which records the segfault that taught this.
    slot.atlas = device_->newTexture(descriptor);
    if (slot.atlas == nullptr) {
        slot.glyphs.clear();
        std::fprintf(stderr, "could not upload the atlas for \"%s\"\n", familyName);
        return;
    }

    const MTL::Region region =
        MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(atlasWidth),
                            static_cast<NS::UInteger>(atlasHeight));
    slot.atlas->replaceRegion(region, 0, pixels.data(),
                              static_cast<NS::UInteger>(atlasWidth));

    std::printf("hud font: %s %.0fpt, %zu glyphs in a %dx%d atlas, %.1fpx line\n", familyName,
                static_cast<double>(points), slot.glyphs.size(), atlasWidth, atlasHeight,
                static_cast<double>(slot.lineHeight));
}

text::Font Renderer::labelFont() const noexcept { return labelFont_.view(); }

text::Font Renderer::readoutFont() const noexcept { return readoutFont_.view(); }

void Renderer::setHud(std::span<const text::TextVertex> label,
                      std::span<const text::TextVertex> readout) noexcept {
    labelVertexCount_ = 0;
    readoutVertexCount_ = 0;
    if (textBuffer_ == nullptr) {
        return;
    }

    // The two faces share one buffer, the labels first and the readouts after. One allocation
    // and one upload; the draws differ only in which atlas they bind and where they start.
    auto* base = static_cast<text::TextVertex*>(textBuffer_->contents())
               + instanceSlot_ * text::kMaxTextVertices;

    // Truncated to whole TRIANGLES, so a dropped tail cannot leave half a quad — the rule the
    // decals follow. The chrome is in the label list, so if anything has to go it is a number
    // rather than the panel it sits on.
    const std::size_t labelFits =
        std::min(label.size(), text::kMaxTextVertices) / 3 * 3;
    if (labelFits > 0) {
        std::memcpy(base, label.data(), labelFits * sizeof(text::TextVertex));
        labelVertexCount_ = labelFits;
    }

    const std::size_t readoutRoom = text::kMaxTextVertices - labelFits;
    const std::size_t readoutFits = std::min(readout.size(), readoutRoom) / 3 * 3;
    if (readoutFits > 0) {
        std::memcpy(base + labelFits, readout.data(),
                    readoutFits * sizeof(text::TextVertex));
        readoutVertexCount_ = readoutFits;
    }
}

} // namespace rm
