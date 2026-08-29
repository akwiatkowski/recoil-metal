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
#include "core/ui/Hud.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace rm {

// The uniform layouts, the buffer indices and the small helpers live in
// `RendererInternal.hpp` — see the note there on why they are not in `Renderer.hpp`.
using namespace render_detail;  // NOLINT(google-build-using-namespace)

void Renderer::buildFontAtlas(FontSlot& slot, const char* familyName, float points) {
    if (slot.atlas != nullptr) {
        slot.atlas->release();
        slot.atlas = nullptr;
    }
    slot.glyphs.clear();
    slot.lineHeight = 0.0f;

    const float scale = std::max(fontRasterScale_, 1.0f);
    const float rasterPoints = points * scale;

    CFStringRef name =
        CFStringCreateWithCString(nullptr, familyName, kCFStringEncodingUTF8);
    CTFontRef font =
        name != nullptr ? CTFontCreateWithName(name, rasterPoints, nullptr) : nullptr;
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
    slot.lineHeight =
        (ascent + descent + static_cast<float>(CTFontGetLeading(font))) / scale;

    // Measure first, pack second. All 95 glyphs stay in one row; its pixel width grows with the
    // raster scale, and a running sum remains enough without a bin-packer.
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

    // A SOLID BLOCK, reserved before the glyphs. Solid quads keep these coordinates in the common
    // UI vertex format even though their semantic pipeline no longer samples the atlas. Four
    // pixels rather than one keeps the Font contract valid for any coverage-sampled caller.
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
            .width = static_cast<float>(w) / scale,
            .height = static_cast<float>(h) / scale,
            // The ink's left edge relative to the pen, and its TOP relative to the baseline.
            // The second is negative for anything that rises above the baseline, which is
            // nearly everything — see Glyph.
            .bearingX = static_cast<float>(box.origin.x) / scale,
            .bearingY = -static_cast<float>(box.origin.y + box.size.height) / scale,
            .advance = static_cast<float>(advances[i].width) / scale,
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

    std::printf("hud font: %s %.0fpt @ %.0fx, %zu glyphs in a %dx%d atlas, %.1fpt line\n",
                familyName, static_cast<double>(points), static_cast<double>(scale),
                slot.glyphs.size(), atlasWidth, atlasHeight,
                static_cast<double>(slot.lineHeight));
}

text::Font Renderer::labelFont() const noexcept { return labelFont_.view(); }

text::Font Renderer::readoutFont() const noexcept { return readoutFont_.view(); }

void Renderer::setUiViewport(const ui::UiViewport& viewport) {
    const float rasterScale = viewport.fontRasterScale();
    uiViewport_ = viewport;
    if (std::abs(rasterScale - fontRasterScale_) < 0.01f) {
        return;
    }
    fontRasterScale_ = rasterScale;
    buildFontAtlas(labelFont_, kLabelFontName, kLabelPointSize);
    buildFontAtlas(readoutFont_, kReadoutFontName, kReadoutPointSize);
}

void Renderer::setHud(const ui::Geometry& geometry) noexcept {
    uiLayerVertexCounts_ = {};
    const bool hasMinimap = minimapTexture_ != nullptr && minimapRect_[2] > 0.0f
                         && minimapRect_[3] > 0.0f;
    std::array<std::size_t, ui::kUiLayerCount> submitted = geometry.submittedVertices();
    if (hasMinimap) {
        submitted[ui::uiLayerIndex(ui::UiLayer::PanelSurface)] += text::kVerticesPerGlyph;
    }
    uiCapacityReport_ = ui::uiCapacityReport(submitted);
    if (uiBuffer_ == nullptr) {
        return;
    }

    auto* frameBase = static_cast<text::TextVertex*>(uiBuffer_->contents())
                    + instanceSlot_ * ui::kUiVerticesPerFrame;
    const std::span<const text::TextVertex> none;

    // Mixed-material layers still own one semantic capacity. The first stream has draw-order
    // priority within that layer; both are copied only as complete quads.
    const auto uploadLayer = [&](ui::UiLayer layer,
                                 std::span<const text::TextVertex> first,
                                 std::span<const text::TextVertex> second) {
        assert(first.size() % text::kVerticesPerGlyph == 0);
        assert(second.size() % text::kVerticesPerGlyph == 0);

        const std::size_t index = ui::uiLayerIndex(layer);
        // The minimap preview uses a six-vertex inline draw in this same semantic layer. Reserve
        // its quad even though it does not occupy this buffer, so the capacity report and limit
        // describe all panel-surface geometry rather than only the uploaded material streams.
        const std::size_t reserved = layer == ui::UiLayer::PanelSurface && hasMinimap
                                       ? text::kVerticesPerGlyph
                                       : 0;
        const ui::UiLayerUpload upload = ui::uiLayerUpload(
            first.size(), second.size(), ui::kUiLayerVertexCapacity - reserved);
        text::TextVertex* destination = frameBase + index * ui::kUiLayerVertexCapacity;
        if (upload.first > 0) {
            std::memcpy(destination, first.data(), upload.first * sizeof(text::TextVertex));
        }
        if (upload.second > 0) {
            std::memcpy(destination + upload.first, second.data(),
                        upload.second * sizeof(text::TextVertex));
        }
        uiLayerVertexCounts_[index] = {upload.first, upload.second};
        uiCapacityReport_.layers[index].uploaded = upload.total() + reserved;
    };

    uploadLayer(ui::UiLayer::WorldOverlay, geometry.worldOverlay.image,
                geometry.worldOverlay.solid);
    uploadLayer(ui::UiLayer::PanelSurface, geometry.panelSurface.solid,
                geometry.panelSurface.image);
    uploadLayer(ui::UiLayer::Chrome, geometry.chrome, none);
    uploadLayer(ui::UiLayer::Icon, geometry.icon, none);
    uploadLayer(ui::UiLayer::Label, geometry.label, none);
    uploadLayer(ui::UiLayer::ForegroundReadout, geometry.foregroundReadout, none);

#ifndef NDEBUG
    assert(!uiCapacityReport_.droppedAny() && "a semantic UI layer exceeded its fixed capacity");
#else
    if (uiCapacityReport_.droppedAny() && !uiOverflowWarned_) {
        for (std::size_t index = 0; index < ui::kUiLayerCount; ++index) {
            const ui::UiLayerUsage& usage = uiCapacityReport_.layers[index];
            if (usage.dropped() > 0) {
                std::fprintf(stderr, "HUD %.*s layer dropped %zu of %zu vertices\n",
                             static_cast<int>(ui::uiLayerName(static_cast<ui::UiLayer>(index)).size()),
                             ui::uiLayerName(static_cast<ui::UiLayer>(index)).data(),
                             usage.dropped(), usage.submitted);
            }
        }
        uiOverflowWarned_ = true;
    }
#endif
}

void Renderer::setIconAtlas(const dds::Texture& atlas) {
    if (iconAtlas_ != nullptr) {
        iconAtlas_->release();
        iconAtlas_ = nullptr;
    }
    if (atlas.width <= 0 || atlas.height <= 0 || atlas.data.empty()) {
        return;  // no icons: the cells keep their reserved squares and their ids
    }
    iconAtlas_ = uploadTexture(atlas, "build icons");
}


void Renderer::setMinimapImage(const dds::Texture& image) {
    if (minimapTexture_ != nullptr) {
        minimapTexture_->release();
        minimapTexture_ = nullptr;
    }
    if (image.width <= 0 || image.height <= 0 || image.data.empty()) {
        return;  // a map with no preview: the minimap draws its own panel and says nothing
    }
    minimapTexture_ = uploadTexture(image, "minimap preview");
}

void Renderer::setMinimapRect(float x, float y, float width, float height) noexcept {
    minimapRect_ = {x, y, width, height};
}

} // namespace rm
