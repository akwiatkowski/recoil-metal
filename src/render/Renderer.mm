// The PRIVATE_IMPLEMENTATION defines instantiate metal-cpp's inline
// implementations. They must appear in EXACTLY ONE translation unit in the
// whole program — this one (AGENT.md gotchas).
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#import <CoreText/CoreText.h>
#include "render/Renderer.hpp"

#include "core/Error.hpp"
#include "core/camera/Frustum.hpp"
#include "render/shaders/Common.hpp"
#include "render/shaders/Fx.hpp"
#include "render/shaders/Map.hpp"
#include "render/shaders/Outline.hpp"
#include "render/shaders/Ui.hpp"
#include "render/shaders/Units.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <string>

#include "render/RendererInternal.hpp"
#include "render/MpsBlur.hpp"

namespace rm {

// The furniture moved to `RendererInternal.hpp` when this file was split four ways (§7 P7.3):
// an anonymous namespace is right for one translation unit and unavailable across five.
// Brought in wholesale rather than qualified at 400 call sites — the names were file-local a
// commit ago and their meaning has not changed.
using namespace render_detail;  // NOLINT(google-build-using-namespace)

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

    // BC1 is what .smt tiles already are. Apple Silicon supports it natively,
    // so the tiles reach the GPU untouched; checking here means a machine
    // without it fails loudly at startup instead of rendering garbage.
    if (!device_->supportsBCTextureCompression()) {
        throw RendererError{
            "this GPU cannot sample BC1 textures, which is the format Recoil's "
            ".smt tiles are stored in"};
    }

    // Null layer = headless (offscreen benchmarking). Everything else below is
    // layer-independent.
    if (layer_ != nullptr) {
        layer_->setDevice(device_);
        layer_->setPixelFormat(kColorFormat);
    }

    commandQueue_ = device_->newCommandQueue();
    if (commandQueue_ == nullptr) {
        throw RendererError{"failed to create Metal command queue"};
    }

    // --- Pipelines, compiled from source at runtime -------------------------
    NS::Error* error = nullptr;
    const std::string shaderSource = assembleShaderSource();
    MTL::Library* library = device_->newLibrary(
        NS::String::string(shaderSource.c_str(), NS::UTF8StringEncoding),
        nullptr, // default compile options
        &error);
    if (library == nullptr) {
        throwMetalError("runtime shader compilation failed", error);
    }

    terrainShadowPipeline_ = makeDepthOnlyPipeline(device_, library, "terrainShadowVertex");
    unitShadowPipeline_ = makeDepthOnlyPipeline(device_, library, "unitShadowVertex");
    propShadowPipeline_ =
        makeDepthOnlyPipeline(device_, library, "propShadowVertex", "propShadowFragment");
    terrainPipeline_ = makePipeline(device_, library, "terrainVertex", "terrainFragment",
                                    BlendMode::Opaque);
    skyPipeline_ =
        makePipeline(device_, library, "skyVertex", "skyFragment", BlendMode::Opaque);
    // No hardware blending: the water shader reads the framebuffer itself and
    // composites, which is what lets it absorb by depth rather than by a single
    // alpha. Leaving blending on would mix the result a second time.
    waterPipeline_ = makePipeline(device_, library, "waterVertex", "waterFragment",
                                  BlendMode::Opaque);
    unitPipeline_ = makePipeline(device_, library, "unitVertex", "unitFragment",
                                 BlendMode::Opaque);
    // The build ghost: the same vertex stage — it IS a unit, geometrically — with a flat
    // luminous fragment and blending, because a silhouette that occluded the ground it is
    // about to claim would hide the one thing the player is judging.
    ghostPipeline_ = makePipeline(device_, library, "unitVertex", "unitGhostFragment",
                                  BlendMode::StraightAlpha);
    // A construction site: the same vertex stage again, and blended because Aeon's unbuilt
    // half is translucent light. The other three factions discard rather than blend, so the
    // blend state costs them nothing.
    constructionPipeline_ = makePipeline(device_, library, "unitVertex", "unitBuildFragment",
                                         BlendMode::StraightAlpha);
    // Blended, and drawn last of all: the HUD sits over the world rather than in it.
    textPipeline_ = makePipeline(device_, library, "textVertex", "textFragment",
                                  BlendMode::PremultipliedAlpha);
    solidPipeline_ = makePipeline(device_, library, "textVertex", "solidFragment",
                                  BlendMode::PremultipliedAlpha);
    // The same vertex function and the same blend — only the fragment differs, sampling a
    // full-colour image instead of a coverage mask. See `imageFragment`.
    imagePipeline_ = makePipeline(device_, library, "textVertex", "imageFragment",
                                  BlendMode::PremultipliedAlpha);
    minimapFogPipeline_ = makePipeline(device_, library, "textVertex", "minimapFogFragment",
                                       BlendMode::PremultipliedAlpha);
    downsamplePipeline_ = makePipeline(device_, library, "screenVertex", "screenFragment",
                                       BlendMode::Opaque, MTL::PixelFormatInvalid);
    composePipeline_ = makePipeline(device_, library, "screenVertex", "screenFragment",
                                    BlendMode::Opaque);
    glassPipeline_ = makePipeline(device_, library, "textVertex", "glassFragment",
                                  BlendMode::PremultipliedAlpha);

    // A selection ring is interface laid over the ground, and a solid band would hide the
    // terrain it marks.
    decalPipeline_ = makePipeline(device_, library, "decalVertex", "decalFragment",
                                  BlendMode::StraightAlpha);
    // The selection outline. Front faces culled and no depth write: what shows is the
    // shell's far side, and only where it survives the depth test against the unit
    // that has already been drawn — which is exactly the silhouette.
    {
        outlinePipeline_ = makePipeline(device_, library, "outlineVertex", "outlineFragment",
                                        BlendMode::Opaque);

        const std::size_t bytes = kMaxOutlinedUnits * sizeof(UnitInstance) * kMaxFramesInFlight;
        outlineBuffer_ = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        if (outlineBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the selection outline buffer"};
        }
    }

    // Particle colours are authored premultiplied so the same pipeline can draw translucent
    // dust and additive sparks. The explicit mode keeps that contract beside every other one.
    particlePipeline_ = makePipeline(device_, library, "particleVertex", "particleFragment",
                                     BlendMode::PremultipliedAlpha);
    modulatedParticlePipelines_[0] = makePipeline(device_, library, "particleVertex", "particleFragment",
                                                 BlendMode::ModulateInverse);
    modulatedParticlePipelines_[1] = makePipeline(device_, library, "particleVertex", "particleFragment",
                                                 BlendMode::Modulate2xInverse);
    library->release();

    // Two immutable kernels, one selected per frame. Only one is ever encoded, and both work
    // on the same quarter-resolution pair. At quarter scale these are roughly 14 px and 7 px
    // radii in the drawable, enough to separate the panel from motion without frosting it.
    fullBlur_ = mps::createGaussianBlur(device_, 3.5f);
    reducedBlur_ = mps::createGaussianBlur(device_, 1.75f);

    // --- Depth state -------------------------------------------------------
    auto* depthDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
    depthDescriptor->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLess);
    depthDescriptor->setDepthWriteEnabled(true);
    depthState_ = device_->newDepthStencilState(depthDescriptor);

    // The sky writes no depth and passes only where nothing has been drawn:
    // it sits at the far plane, so everything else beats it on depth and it
    // fills whatever is left. That lets it be drawn FIRST, which keeps it out
    // of the blended water's way.
    {
        auto* skyDepth = MTL::DepthStencilDescriptor::alloc()->init();
        skyDepth->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
        skyDepth->setDepthWriteEnabled(false);
        skyDepthState_ = device_->newDepthStencilState(skyDepth);
        skyDepth->release();
    }

    // --- Shadow map --------------------------------------------------------
    {
        auto* shadowDescriptor = MTL::TextureDescriptor::alloc()->init();
        shadowDescriptor->setTextureType(MTL::TextureType::TextureType2D);
        shadowDescriptor->setPixelFormat(kShadowFormat);
        shadowDescriptor->setWidth(kShadowResolution);
        shadowDescriptor->setHeight(kShadowResolution);
        // Written by the shadow pass, read by both fragment shaders.
        shadowDescriptor->setUsage(MTL::TextureUsageRenderTarget
                                   | MTL::TextureUsageShaderRead);
        shadowDescriptor->setStorageMode(MTL::StorageModePrivate);
        shadowMap_ = device_->newTexture(shadowDescriptor);
        shadowDescriptor->release();

        // A COMPARISON sampler: the hardware does the depth test per tap and
        // filters the results, which is what makes four taps a soft edge rather
        // than four hard ones. Clamp-to-edge would smear the border, so anything
        // outside is treated as lit by the shader instead.
        auto* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
        samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        samplerDescriptor->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        samplerDescriptor->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        samplerDescriptor->setCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
        shadowSampler_ = device_->newSamplerState(samplerDescriptor);
        samplerDescriptor->release();
    }

    // --- Reflection target -------------------------------------------------
    // Half resolution in each axis. A reflection seen through a rippling
    // surface is the one thing in a frame nobody can examine closely, and this
    // quarters what the extra scene pass costs.
    {
        auto* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType::TextureType2D);
        descriptor->setPixelFormat(kColorFormat);
        descriptor->setWidth(kReflectionWidth);
        descriptor->setHeight(kReflectionHeight);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        descriptor->setStorageMode(MTL::StorageModePrivate);
        reflectionColour_ = device_->newTexture(descriptor);

        descriptor->setPixelFormat(kDepthFormat);
        descriptor->setUsage(MTL::TextureUsageRenderTarget);
        reflectionDepth_ = device_->newTexture(descriptor);
        descriptor->release();

        auto* sampler = MTL::SamplerDescriptor::alloc()->init();
        sampler->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sampler->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sampler->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        sampler->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        reflectionSampler_ = device_->newSamplerState(sampler);
        sampler->release();
    }

    // Water tests against terrain but does not write depth: it is translucent,
    // so writing would occlude anything drawn behind it later.
    depthDescriptor->setDepthWriteEnabled(false);
    waterDepthState_ = device_->newDepthStencilState(depthDescriptor);

    // Rings test LESS-EQUAL rather than LESS, and write no depth.
    //
    // Less-equal because the band is lifted a hair above ground that the
    // terrain has already written at very nearly the same depth, and a strict
    // test loses that fight wherever the lift rounds away at distance. No depth
    // write because a ring must not occlude the unit standing inside it.
    depthDescriptor->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
    decalDepthState_ = device_->newDepthStencilState(depthDescriptor);
    depthDescriptor->release();

    if (depthState_ == nullptr || waterDepthState_ == nullptr || decalDepthState_ == nullptr) {
        throw RendererError{"failed to create depth-stencil state"};
    }

    // --- Particles ---------------------------------------------------------
    // Depth-TESTED so dust behind a hill is hidden, but no depth WRITE: a puff
    // must not occlude the unit that raised it, and two overlapping puffs must
    // both show.
    {
        auto* particleDepth = MTL::DepthStencilDescriptor::alloc()->init();
        // LessEqual, not Less. Dust is raised AT the ground and the terrain has
        // already written very nearly the same depth there, so a strict test loses
        // that fight wherever the lift rounds away at distance — and loses it
        // silently: the draw is issued, the count is right, and nothing appears.
        particleDepth->setDepthCompareFunction(MTL::CompareFunction::CompareFunctionLessEqual);
        particleDepth->setDepthWriteEnabled(false);
        particleDepthState_ = device_->newDepthStencilState(particleDepth);
        particleDepth->release();
        if (particleDepthState_ == nullptr) {
            throw RendererError{"failed to create the particle depth state"};
        }

        const std::size_t bytes = kMaxParticles * sizeof(Particle) * kMaxFramesInFlight;
        particleBuffer_ = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        if (particleBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the particle buffer"};
        }
    }

    // --- The build ghost -----------------------------------------------------
    {
        // One UnitInstance per frame in flight: the ghost is a single model, but it moves
        // with the cursor, so each frame writes its own slot like every per-frame upload.
        ghostInstanceBuffer_ = device_->newBuffer(sizeof(UnitInstance) * kMaxFramesInFlight,
                                                  MTL::ResourceStorageModeShared);
        if (ghostInstanceBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the ghost instance buffer"};
        }
    }

    // --- Construction sites --------------------------------------------------
    {
        // A ring of `kMaxConstructions` instances per frame in flight. Every site is drawn
        // with its own uniform block, so they cannot share one instanced draw; what they share
        // is this buffer, one slot each, written where the site's index puts it.
        constructionInstanceBuffer_ =
            device_->newBuffer(sizeof(UnitInstance) * kMaxConstructions * kMaxFramesInFlight,
                               MTL::ResourceStorageModeShared);
        if (constructionInstanceBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the construction instance buffer"};
        }
    }

    // --- UI -----------------------------------------------------------------
    {
        // A ring like everything else written per frame: the GPU may still be reading last
        // frame's copy, and this buffer cannot be reallocated while it is.
        const std::size_t bytes =
            ui::kUiVerticesPerFrame * sizeof(text::TextVertex) * kMaxFramesInFlight;
        uiBuffer_ = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        if (uiBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the UI buffer"};
        }

        auto* sampler = MTL::SamplerDescriptor::alloc()->init();
        // LINEAR, because a glyph is coverage and nearest-neighbour on coverage is a
        // staircase along every diagonal stroke. CLAMP because a glyph's rectangle is exact
        // and a repeat would fetch its neighbour's ink at the seam.
        sampler->setMinFilter(MTL::SamplerMinMagFilterLinear);
        sampler->setMagFilter(MTL::SamplerMinMagFilterLinear);
        sampler->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
        sampler->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
        fontSampler_ = device_->newSamplerState(sampler);
        sampler->release();
        if (fontSampler_ == nullptr) {
            throw RendererError{"failed to create the font sampler"};
        }

        buildFontAtlas(labelFont_, kLabelFontName, kLabelPointSize);
        buildFontAtlas(readoutFont_, kReadoutFontName, kReadoutPointSize);
    }

    // --- Selection ring buffer ---------------------------------------------
    // One slot per frame in flight, allocated once. Shared storage because the
    // CPU rewrites it every frame; the frames-in-flight wait in beginFrame is
    // what makes that safe, exactly as for unit instances.
    {
        const std::size_t bytes =
            kMaxDecalVertices * sizeof(DecalVertex) * kMaxFramesInFlight;
        decalBuffer_ = device_->newBuffer(bytes, MTL::ResourceStorageModeShared);
        if (decalBuffer_ == nullptr) {
            throw RendererError{"failed to allocate the selection ring buffer"};
        }
    }

    // --- Ground sampler ----------------------------------------------------
    auto* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
    samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    samplerDescriptor->setMipFilter(MTL::SamplerMipFilter::SamplerMipFilterLinear);
    // Clamp, not repeat: the atlas covers the map exactly, and repeating would
    // wrap the far edge back onto the near one.
    samplerDescriptor->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    samplerDescriptor->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    samplerDescriptor->setMaxAnisotropy(8);  // terrain is viewed at grazing angles
    groundSampler_ = device_->newSamplerState(samplerDescriptor);

    // A second sampler that REPEATS, for the splat layers alone.
    //
    // The two addressing modes are not a preference, they are a consequence of
    // what each texture is. The atlas and the two masks cover the map exactly,
    // so their uv never leaves 0..1 and clamping is right. A splat layer covers
    // a few dozen elmos and is meant to tile across the whole map, so its uv
    // reaches into the hundreds — under clamping every texel past the first
    // repeat samples the texture's edge, which renders as a smooth smear that
    // looks like a missing texture rather than a wrong sampler.
    samplerDescriptor->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeRepeat);
    samplerDescriptor->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeRepeat);
    splatSampler_ = device_->newSamplerState(samplerDescriptor);

    samplerDescriptor->release();

    if (groundSampler_ == nullptr) {
        throw RendererError{"failed to create ground sampler state"};
    }

    // --- Fallback ground texture -------------------------------------------
    // Always bound, so the shader never names an unbound texture even on the
    // branch that does not sample it.
    MTL::TextureDescriptor* fallback =
        MTL::TextureDescriptor::texture2DDescriptor(kGroundFormat, 4, 4, /*mipmapped=*/false);
    fallback->setUsage(MTL::TextureUsageShaderRead);
    groundTexture_ = device_->newTexture(fallback);
    if (groundTexture_ == nullptr) {
        throw RendererError{"failed to create fallback ground texture"};
    }
    groundTexture_->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, kFallbackBlock,
                                  sizeof(kFallbackBlock));
}

Renderer::~Renderer() {
    // Drain before releasing anything. Every committed frame's completion
    // handler captures `this`, so Metal must be done CALLING them all before the
    // members they touch go away. Each outstanding handler holds one ring slot
    // and hands it back as its last action, so acquiring kMaxFramesInFlight of
    // them blocks until the last handler has returned — and cannot deadlock,
    // because no new frame can open during destruction. The slots are handed
    // straight back so the member destructs full, as it was constructed.
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        framesInFlight_.acquire();
    }
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        framesInFlight_.release();
    }

    // Reverse acquisition order; all are +1 objects from newXxx()/CreateXxx.
    releaseWaterBuffers();
    if (reflectionSampler_ != nullptr) reflectionSampler_->release();
    if (reflectionDepth_ != nullptr) reflectionDepth_->release();
    if (reflectionColour_ != nullptr) reflectionColour_->release();
    if (skyDepthState_ != nullptr) skyDepthState_->release();
    if (skyPipeline_ != nullptr) skyPipeline_->release();
    if (shadowSampler_ != nullptr) shadowSampler_->release();
    if (shadowMap_ != nullptr) shadowMap_->release();
    if (propShadowPipeline_ != nullptr) propShadowPipeline_->release();
    if (unitShadowPipeline_ != nullptr) unitShadowPipeline_->release();
    if (terrainShadowPipeline_ != nullptr) terrainShadowPipeline_->release();
    if (outlineBuffer_ != nullptr) outlineBuffer_->release();
    if (outlinePipeline_ != nullptr) outlinePipeline_->release();
    for (auto& entry : weaponTextures_) {
        entry.texture->release();
        if (entry.ramp) entry.ramp->release();
    }
    if (particleBuffer_ != nullptr) particleBuffer_->release();
    if (particleDepthState_ != nullptr) particleDepthState_->release();
    if (particlePipeline_ != nullptr) particlePipeline_->release();
    for (auto* pipeline : modulatedParticlePipelines_) if (pipeline) pipeline->release();
    if (decalBuffer_ != nullptr) decalBuffer_->release();
    if (decalDepthState_ != nullptr) decalDepthState_->release();
    if (textPipeline_ != nullptr) textPipeline_->release();
    if (solidPipeline_ != nullptr) solidPipeline_->release();
    if (imagePipeline_ != nullptr) imagePipeline_->release();
    if (minimapFogPipeline_ != nullptr) minimapFogPipeline_->release();
    if (glassPipeline_ != nullptr) glassPipeline_->release();
    if (composePipeline_ != nullptr) composePipeline_->release();
    if (downsamplePipeline_ != nullptr) downsamplePipeline_->release();
    if (minimapTexture_ != nullptr) minimapTexture_->release();
    if (iconAtlas_ != nullptr) iconAtlas_->release();
    if (labelFont_.atlas != nullptr) labelFont_.atlas->release();
    if (readoutFont_.atlas != nullptr) readoutFont_.atlas->release();
    if (fontSampler_ != nullptr) fontSampler_->release();
    if (uiBuffer_ != nullptr) uiBuffer_->release();
    if (decalPipeline_ != nullptr) decalPipeline_->release();
    if (sceneColour_ != nullptr) sceneColour_->release();
    if (blurB_ != nullptr) blurB_->release();
    if (blurA_ != nullptr) blurA_->release();
    if (worldColour_ != nullptr) worldColour_->release();
    mps::destroyGaussianBlur(reducedBlur_);
    mps::destroyGaussianBlur(fullBlur_);
    releaseTerrainBuffers();
    releasePropBuffers();  // before the units: acquired after them
    releaseUnitBuffers();  // frees the unit textures too
    // Whatever `releaseUnitBuffers` was still holding back for a frame in flight. The
    // semaphore drain at the top of this destructor proved every in-flight frame's handler
    // has finished, so "everything" is freeable now rather than aspirationally later.
    collectRetiredBuffers(/*everything=*/true);
    unitPipeline_->release();
    if (ghostPipeline_ != nullptr) ghostPipeline_->release();
    if (ghostInstanceBuffer_ != nullptr) ghostInstanceBuffer_->release();
    if (constructionPipeline_ != nullptr) constructionPipeline_->release();
    if (constructionInstanceBuffer_ != nullptr) constructionInstanceBuffer_->release();
    releaseSplat();
    if (groundTexture_ != nullptr) groundTexture_->release();
    if (fogTexture_ != nullptr) fogTexture_->release();
    groundSampler_->release();
    if (splatSampler_ != nullptr) splatSampler_->release();
    if (depthTexture_ != nullptr) depthTexture_->release();
    waterDepthState_->release();
    depthState_->release();
    waterPipeline_->release();
    terrainPipeline_->release();
    commandQueue_->release();
    device_->release();
}











void Renderer::beginFrame() noexcept {
    // Blocks until at most kMaxFramesInFlight - 1 frames are still outstanding,
    // which is precisely the condition for the slot chosen next to be free: it
    // was last used kMaxFramesInFlight frames ago, and that frame's completion
    // handler has now run.
    framesInFlight_.acquire();
    instanceSlot_ = (instanceSlot_ + 1) % kMaxFramesInFlight;
    frameOpen_ = true;
    ++frameCounter_;
    collectRetiredBuffers();

    // Particles too, and for the same reason. Selection outlines likewise: a stale
    // list would outline whatever now occupies those slots.
    particleCount_ = 0;
    uiLayerVertexCounts_ = {};
    uiCapacityReport_ = {};
    outlineRuns_.clear();

    // Rings are forgotten at the start of every frame, so a frame that pushes
    // none draws none.
    //
    // Unlike instances, which persist deliberately — a batch nobody writes
    // keeps what it was uploaded with, because setUnits seeded every slot. The
    // ring buffer has no such seeding, so a count that outlived its frame would
    // point the draw at whatever a different slot happens to hold. Clearing
    // here means the only way to get that wrong is to push rings before
    // beginFrame, which the header forbids.
    decalVertexCount_ = 0;
}

MTL::Texture* Renderer::uploadTexture(const dds::Texture& texture, const char* what) {
    // DXT1/3/5 map one-to-one onto BC1/BC2/BC3, and BGRA8 onto BGRA8Unorm — all
    // natively sampleable here, so the payload goes up untouched exactly like
    // the terrain atlas.
    MTL::PixelFormat format = MTL::PixelFormat::PixelFormatBC1_RGBA;
    switch (texture.format) {
        case dds::Format::Bc1: format = MTL::PixelFormat::PixelFormatBC1_RGBA; break;
        case dds::Format::Bc2: format = MTL::PixelFormat::PixelFormatBC2_RGBA; break;
        case dds::Format::Bc3: format = MTL::PixelFormat::PixelFormatBC3_RGBA; break;
        case dds::Format::Bgra8: format = MTL::PixelFormat::PixelFormatBGRA8Unorm; break;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        format, static_cast<NS::UInteger>(texture.width),
        static_cast<NS::UInteger>(texture.height), /*mipmapped=*/false);
    descriptor->setMipmapLevelCount(static_cast<NS::UInteger>(texture.mipLevels));
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* created = device_->newTexture(descriptor);
    if (created == nullptr) {
        throw RendererError{std::string{"failed to allocate the "} + what + " texture"};
    }

    for (int level = 0; level < texture.mipLevels; ++level) {
        const std::span<const std::byte> mip = texture.mip(level);
        if (mip.empty()) {
            continue;
        }
        created->replaceRegion(
            MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(texture.mipWidth(level)),
                                static_cast<NS::UInteger>(texture.mipHeight(level))),
            static_cast<NS::UInteger>(level), mip.data(),
            static_cast<NS::UInteger>(texture.mipBytesPerRow(level)));
    }

    return created;
}







Renderer::CapturedImage Renderer::renderToImage(unsigned int width, unsigned int height) {
    CapturedImage image;
    if (width == 0 || height == 0) {
        return image;
    }

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    // Shared storage, unlike the benchmark's private target: this one is read
    // back on the CPU.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kColorFormat, width, height, /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* target = device_->newTexture(descriptor);
    ensureDepthTexture(width, height);

    if (target == nullptr || depthTexture_ == nullptr) {
        if (target != nullptr) target->release();
        pool->release();
        throw RendererError{"failed to allocate an offscreen capture target"};
    }

    MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

    // Which props are close enough to be worth drawing, decided once for the whole
    // frame — before the shadow and reflection passes, so every pass agrees about
    // what the scenery is.
    cullProps();

    encodeShadowPass(commandBuffer);
    encodeReflectionPass(commandBuffer);

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
    color0->setTexture(target);
    color0->setLoadAction(MTL::LoadAction::LoadActionClear);
    color0->setStoreAction(MTL::StoreAction::StoreActionStore);  // kept, unlike the benchmark
    color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(depthTexture_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
    depth->setClearDepth(1.0);

    encodeFrame(commandBuffer, pass, width, height);

    commandBuffer->commit();
    commandBuffer->waitUntilCompleted();  // a capture wants the result, not throughput

    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    target->getBytes(image.bgra.data(), static_cast<NS::UInteger>(width) * 4,
                     MTL::Region::Make2D(0, 0, width, height), 0);

    pass->release();
    target->release();
    pool->release();

    return image;
}

void Renderer::focusOn(std::array<float, 3> target, float distance) noexcept {
    camera_.target = simd_make_float3(target[0], target[1], target[2]);
    camera_.distance = std::clamp(distance, OrbitCamera::kMinDistance,
                                  OrbitCamera::kMaxDistance);
    // Keep the far plane clear of the map behind the new target.
    camera_.farZ = camera_.distance * 4.0f + mapWidth_ * 2.0f;
}

void Renderer::beginBenchmark(std::size_t warmupFrames) {
    const std::lock_guard lock{benchMutex_};
    recorder_ = bench::FrameRecorder{warmupFrames};
    recording_ = true;
    lastFrameStart_ = 0.0;
}

bench::FrameRecorder Renderer::benchmarkSnapshot() const {
    const std::lock_guard lock{benchMutex_};
    return recorder_;
}

std::size_t Renderer::recordedFrames() const {
    const std::lock_guard lock{benchMutex_};
    return recorder_.recorded();
}

void Renderer::ensureDepthTexture(unsigned int width, unsigned int height) noexcept {
    if (depthTexture_ != nullptr && depthWidth_ == width && depthHeight_ == height) {
        return;
    }

    if (depthTexture_ != nullptr) {
        depthTexture_->release();
        depthTexture_ = nullptr;
    }

    // texture2DDescriptor returns an autoreleased object — the caller's pool
    // (drawFrame's) owns it.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kDepthFormat, width, height, /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    // Private: the depth buffer is never read back by the CPU, so it does not
    // need to live in shared memory.
    descriptor->setStorageMode(MTL::StorageModePrivate);

    depthTexture_ = device_->newTexture(descriptor);
    depthWidth_ = width;
    depthHeight_ = height;
}

bench::FrameRecorder Renderer::runOffscreenBenchmark(unsigned int width, unsigned int height,
                                                     std::size_t frames,
                                                     std::size_t warmupFrames) {
    bench::FrameRecorder recorder{warmupFrames};
    if (indexCount_ == 0 || width == 0 || height == 0 || frames == 0) {
        return recorder;
    }

    NS::AutoreleasePool* setupPool = NS::AutoreleasePool::alloc()->init();

    // Offscreen colour target. Private storage: nothing reads it back, the
    // point is to make the GPU do the work, not to keep the image.
    MTL::TextureDescriptor* colorDescriptor =
        MTL::TextureDescriptor::texture2DDescriptor(kColorFormat, width, height,
                                                    /*mipmapped=*/false);
    colorDescriptor->setUsage(MTL::TextureUsageRenderTarget);
    colorDescriptor->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* colorTarget = device_->newTexture(colorDescriptor);

    ensureDepthTexture(width, height);

    if (colorTarget == nullptr || depthTexture_ == nullptr) {
        if (colorTarget != nullptr) colorTarget->release();
        setupPool->release();
        throw RendererError{"failed to allocate offscreen benchmark targets"};
    }
    setupPool->release();

    // Bound in-flight frames. Without this the loop would queue every frame at
    // once, measuring how fast the CPU can encode rather than how fast the GPU
    // can draw.
    std::counting_semaphore<static_cast<std::ptrdiff_t>(kMaxFramesInFlight)> inFlight{
        static_cast<std::ptrdiff_t>(kMaxFramesInFlight)};

    std::mutex recordMutex;
    double previousStart = 0.0;

    const auto nowSeconds = [] {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    for (std::size_t frame = 0; frame < frames; ++frame) {
        inFlight.acquire();

        NS::AutoreleasePool* framePool = NS::AutoreleasePool::alloc()->init();

        const double start = nowSeconds();
        double cpuMs = 0.0;
        if (previousStart > 0.0) {
            cpuMs = (start - previousStart) * 1000.0;
        }
        previousStart = start;

        MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

        // Which props are worth drawing from where the camera is, decided once for
        // the whole frame so every pass agrees about what the scenery is.
        cullProps();

        encodeShadowPass(commandBuffer);
        encodeReflectionPass(commandBuffer);

        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
        MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
        color0->setTexture(colorTarget);
        color0->setLoadAction(MTL::LoadAction::LoadActionClear);
        // DontCare: the image is never presented or read, so on a tile-based GPU
        // this avoids a full-framebuffer writeback that would not happen in the
        // windowed path either.
        color0->setStoreAction(MTL::StoreAction::StoreActionDontCare);
        color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

        MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
        depth->setTexture(depthTexture_);
        depth->setLoadAction(MTL::LoadAction::LoadActionClear);
        depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
        depth->setClearDepth(1.0);

        encodeFrame(commandBuffer, pass, width, height);

        commandBuffer->addCompletedHandler(MTL::HandlerFunction{
            [&recorder, &recordMutex, &inFlight, cpuMs](MTL::CommandBuffer* completed) {
                const double gpuMs =
                    (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                {
                    const std::lock_guard lock{recordMutex};
                    recorder.add(bench::FrameSample{cpuMs, gpuMs});
                }
                inFlight.release();
            }});

        commandBuffer->commit();

        pass->release();
        framePool->release();
    }

    // Drain: reacquire every slot so all completion handlers have run before the
    // recorder is read or the targets are freed.
    for (std::size_t i = 0; i < kMaxFramesInFlight; ++i) {
        inFlight.acquire();
    }

    colorTarget->release();

    const std::lock_guard lock{recordMutex};
    return recorder;
}

void Renderer::ensureSceneColour(unsigned int width, unsigned int height) noexcept {
    if (sceneColour_ != nullptr && sceneColour_->width() == width
        && sceneColour_->height() == height) {
        return;
    }
    if (sceneColour_ != nullptr) {
        sceneColour_->release();
        sceneColour_ = nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kColorFormat, width, height, /*mipmapped=*/false);
    // ShaderRead to sample it, and RenderTarget because a blit destination that
    // shares the drawable's format wants the same usage set — Metal validates the
    // pair rather than inferring it.
    descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    // NOT released: texture2DDescriptor is a class factory method, so what it
    // returns is autoreleased (+0). Releasing it here is an over-release, and it
    // segfaults on the next frame rather than at the mistake — see AGENT.md on
    // metal-cpp not being ARC.
    sceneColour_ = device_->newTexture(descriptor);
}

void Renderer::ensureBackdropTextures(unsigned int width, unsigned int height) noexcept {
    const unsigned int blurWidth = std::max(1u, (width + 3u) / 4u);
    const unsigned int blurHeight = std::max(1u, (height + 3u) / 4u);
    if (worldColour_ != nullptr && worldColour_->width() == width
        && worldColour_->height() == height && blurA_ != nullptr
        && blurA_->width() == blurWidth && blurA_->height() == blurHeight
        && blurB_ != nullptr) {
        return;
    }

    if (blurB_ != nullptr) blurB_->release();
    if (blurA_ != nullptr) blurA_->release();
    if (worldColour_ != nullptr) worldColour_->release();
    blurB_ = nullptr;
    blurA_ = nullptr;
    worldColour_ = nullptr;

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kColorFormat, width, height, /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    worldColour_ = device_->newTexture(descriptor);

    descriptor->setWidth(blurWidth);
    descriptor->setHeight(blurHeight);
    descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead
                         | MTL::TextureUsageShaderWrite);
    blurA_ = device_->newTexture(descriptor);
    blurB_ = device_->newTexture(descriptor);
}

void Renderer::encodeFrame(MTL::CommandBuffer* commandBuffer, MTL::RenderPassDescriptor* pass,
                           unsigned int width, unsigned int height) noexcept {
    if (uiEffects_ == ui::EffectsLevel::Off || !uiHasGlassPanels_) {
        encodeScene(commandBuffer, pass, width, height);
        return;
    }

    ensureBackdropTextures(width, height);
    if (worldColour_ == nullptr || blurA_ == nullptr || blurB_ == nullptr
        || fullBlur_ == nullptr || reducedBlur_ == nullptr) {
        encodeScene(commandBuffer, pass, width, height);
        return;
    }

    // World first, into a shader-readable target. The water's pre-water refraction copy stays
    // separate (`sceneColour_`); this texture is the complete post-water world the HUD sees.
    MTL::RenderPassDescriptor* worldPass = MTL::RenderPassDescriptor::alloc()->init();
    auto* worldColor = worldPass->colorAttachments()->object(0);
    worldColor->setTexture(worldColour_);
    worldColor->setLoadAction(MTL::LoadAction::LoadActionClear);
    worldColor->setStoreAction(MTL::StoreAction::StoreActionStore);
    worldColor->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));
    auto* worldDepth = worldPass->depthAttachment();
    worldDepth->setTexture(depthTexture_);
    worldDepth->setLoadAction(MTL::LoadAction::LoadActionClear);
    worldDepth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
    worldDepth->setClearDepth(1.0);
    encodeScene(commandBuffer, worldPass, width, height, nullptr, false);

    // One cheap raster downsample followed by exactly one MPS blur over the quarter-size pair.
    MTL::RenderPassDescriptor* downsamplePass = MTL::RenderPassDescriptor::alloc()->init();
    auto* downsampleColor = downsamplePass->colorAttachments()->object(0);
    downsampleColor->setTexture(blurA_);
    downsampleColor->setLoadAction(MTL::LoadAction::LoadActionDontCare);
    downsampleColor->setStoreAction(MTL::StoreAction::StoreActionStore);
    MTL::RenderCommandEncoder* downsample = commandBuffer->renderCommandEncoder(downsamplePass);
    downsample->setRenderPipelineState(downsamplePipeline_);
    downsample->setFragmentTexture(worldColour_, NS::UInteger{0});
    downsample->setFragmentSamplerState(fontSampler_, NS::UInteger{0});
    downsample->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                               NS::UInteger{3});
    downsample->endEncoding();

    mps::encodeGaussianBlur(uiEffects_ == ui::EffectsLevel::Reduced ? reducedBlur_ : fullBlur_,
                            commandBuffer, blurA_, blurB_);

    // Restore the sharp world, then put semantic HUD layers over it. Only PanelSurface samples
    // the shared blur; every icon, label, edge and readout remains crisp.
    MTL::RenderCommandEncoder* composite = commandBuffer->renderCommandEncoder(pass);
    composite->setRenderPipelineState(composePipeline_);
    composite->setFragmentTexture(worldColour_, NS::UInteger{0});
    composite->setFragmentSamplerState(fontSampler_, NS::UInteger{0});
    composite->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                              NS::UInteger{3});
    encodeUi(composite, width, height, true);
    composite->endEncoding();

    downsamplePass->release();
    worldPass->release();
}

void Renderer::encodeScene(MTL::CommandBuffer* commandBuffer, MTL::RenderPassDescriptor* pass,
                           unsigned int width, unsigned int height,
                           const SceneOverride* override, bool includeUi) noexcept {
    if (height == 0) {
        return;
    }

    // Whether the water is going to want a copy of the colour target, decided
    // before the pass is created because it changes the pass's store actions.
    //
    // Not in the reflection pass: that renders a mirrored world for the water to
    // sample, and water inside it is skipped entirely, so there is nothing there
    // to refract.
    const bool wantsWater = waterIndexCount_ > 0 && hasWater_ && terrainMinY_ < waterLevel_
                            && (override == nullptr || !override->skipWater);
    const auto* uploadedParticles = particleBuffer_ ? static_cast<const Particle*>(particleBuffer_->contents())
        + instanceSlot_ * kMaxParticles : nullptr;
    bool refractingParticles = false;
    if (override == nullptr && uploadedParticles) {
        for (std::size_t i=0; i<particleCount_; ++i) {
            const auto id = uploadedParticles[i].material;
            if (id < weaponTextures_.size() && weaponTextures_[id].uniforms[12] == 5) {
                refractingParticles = true; break;
            }
        }
    }
    const bool grabScene = (wantsWater && refractionEnabled_ && override == nullptr) || refractingParticles;
    if (grabScene) {
        ensureSceneColour(width, height);
    }
    const bool grabbing = grabScene && sceneColour_ != nullptr;

    if (grabbing) {
        // The split costs exactly this: the colour attachment has to be written
        // out so it can be copied, and the depth attachment has to be written out
        // so the water can still depth-test against the terrain when the pass
        // resumes. On a tile-based GPU both are writebacks that the single-pass
        // version never performs, which is the whole price of the offset.
        pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);
        if (pass->depthAttachment()->texture() != nullptr) {
            pass->depthAttachment()->setStoreAction(MTL::StoreAction::StoreActionStore);
        }
    }

    MTL::Texture* colourTarget = pass->colorAttachments()->object(0)->texture();
    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    TerrainUniforms uniforms{
        .viewProjection = override != nullptr ? override->viewProjection
                                              : camera_.viewProjection(aspect),
        .sunDirection = kSunDirection,
        .minHeight = terrainMinY_,
        .maxHeight = terrainMaxY_,
        .mapWidth = mapWidth_,
        .mapDepth = mapDepth_,
        .hasTexture = hasGroundTexture_ ? 1.0f : 0.0f,
        .cameraPosition = camera_.eye(),
        .hasTexture2 = 0.0f,
        .waterLevel = waterLevel_,
        .supremeCommanderShading = 0.0f,
        .lightViewProjection = lightViewProjection_,
        .hasShadows = hasShadows_ ? 1.0f : 0.0f,
        .animationTime = animationTime_,
        .inverseViewProjection = simd_inverse(camera_.viewProjection(aspect)),
        .fogColour = simd_make_float3(environment_.fogColour[0], environment_.fogColour[1],
                                      environment_.fogColour[2]),
        .waterFresnelBias = environment_.waterFresnelBias,
        .waterSurfaceColour = simd_make_float3(environment_.waterSurfaceColour[0],
                                               environment_.waterSurfaceColour[1],
                                               environment_.waterSurfaceColour[2]),
        .waterFresnelPower = environment_.waterFresnelPower,
        .waterSunColour = simd_make_float3(environment_.waterSunColour[0],
                                           environment_.waterSunColour[1],
                                           environment_.waterSunColour[2]),
        .waterColourLerp = environment_.waterColourLerp,
        .waterSkyReflection = environment_.waterSkyReflection,
        .waterSunShininess = environment_.waterSunShininess,
        // Far below any map by default, so nothing is clipped unless a pass
        // asks for it.
        .clipBelowY = override != nullptr ? override->clipBelowY : -1.0e9f,
        .viewportSize = simd_make_float2(static_cast<float>(width), static_cast<float>(height)),
        // Must agree with encodeReflectionPass's own guard: the water reads
        // this to decide whether the reflection texture holds this frame's
        // mirror or last frame's leftovers.
        .hasReflection = reflectionsEnabled_ ? 1.0f : 0.0f,
        .waterRefractionScale = environment_.waterRefractionScale,
        .skyZenithTint = simd_make_float3(environment_.skyZenithTint[0],
                                          environment_.skyZenithTint[1],
                                          environment_.skyZenithTint[2]),
        .sunColour = simd_make_float3(environment_.sunColour[0], environment_.sunColour[1],
                                      environment_.sunColour[2]),
        .sunAmbience = simd_make_float3(environment_.sunAmbience[0], environment_.sunAmbience[1],
                                        environment_.sunAmbience[2]),
        .shadowFill = simd_make_float3(environment_.shadowFill[0], environment_.shadowFill[1],
                                       environment_.shadowFill[2]),
        .lightingMultiplier = environment_.lightingMultiplier,
        .hasFog = hasFog_ ? 1.0f : 0.0f,
        .fogWidthElmos = fogWidthElmos_,
        .fogDepthElmos = fogDepthElmos_,
        .waveRepeats = simd_make_float4(environment_.waveRepeats[0],
                                        environment_.waveRepeats[1], 0.0f, 0.0f),
        .waveMovements = simd_make_float4(
            environment_.waveMovements[0], environment_.waveMovements[1],
            environment_.waveMovements[2], environment_.waveMovements[3]),
        .hasWaterWaves = waterWaves_ != nullptr ? 1.0f : 0.0f,
    };

    // --- Sky ---------------------------------------------------------------
    // First, at the far plane, writing no depth. Everything else then draws
    // over it by winning the depth test rather than by being drawn later.
    encoder->setRenderPipelineState(skyPipeline_);
    encoder->setDepthStencilState(skyDepthState_);
    encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
    encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
    encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                            NS::UInteger{3});

    // Culling stays off: the camera is free to dip below the terrain, and a
    // heightfield seen from underneath should still be visible rather than
    // vanishing. The mesh winding is nonetheless consistent (pinned by tests)
    // so enabling culling later is a one-line change.
    encoder->setCullMode(MTL::CullMode::CullModeNone);
    encoder->setFrontFacingWinding(MTL::Winding::WindingCounterClockwise);

    // --- Terrain -----------------------------------------------------------
    if (indexCount_ > 0) {
        encoder->setRenderPipelineState(terrainPipeline_);
        encoder->setDepthStencilState(depthState_);

        encoder->setVertexBuffer(vertexBuffer_, 0, kVertexBufferIndex);
        // setVertexBytes/setFragmentBytes for a small, per-frame struct: under
        // 4 KB Metal copies it into the command buffer directly, so there is no
        // uniform buffer to allocate or synchronise.
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentTexture(groundTexture_, kGroundTextureIndex);
        // The fallback keeps the slot bound when there is no fog: an unbound texture read
        // is undefined, and `hasFog` already gates whether the sample is used.
        encoder->setFragmentTexture(fogTexture_ != nullptr ? fogTexture_ : groundTexture_,
                                    kFogTextureIndex);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);
        encoder->setFragmentSamplerState(splatSampler_, kSplatSamplerIndex);
        encoder->setFragmentTexture(shadowMap_, kShadowTextureIndex);
        encoder->setFragmentSamplerState(shadowSampler_, kShadowSamplerIndex);

        // The splat. Every slot is bound whether or not the map uses one,
        // because the fragment shader names all nine layers and both masks
        // unconditionally, and sampling an unbound texture is undefined even
        // when the result is multiplied away.
        SplatUniforms splat{};
        splat.enabled = splatEnabled_ ? 1.0f : 0.0f;
        splat.normalStrength = stratumNormalsEnabled_ ? kStratumNormalStrength : 0.0f;
        for (std::size_t i = 0; i < kSplatLayers; ++i) {
            splat.tileElmos[i] = splatTileElmos_[i];
            splat.present[i] = splatPresent_[i];
            encoder->setFragmentTexture(
                splatLayers_[i] != nullptr ? splatLayers_[i] : groundTexture_,
                kSplatLayerBaseIndex + static_cast<NS::UInteger>(i));
        }
        for (std::size_t i = 0; i < kSplatNormalLayers; ++i) {
            splat.normalTileElmos[i] = splatNormalTileElmos_[i];
            splat.normalPresent[i] = splatNormalPresent_[i];
            encoder->setFragmentTexture(
                splatNormals_[i] != nullptr ? splatNormals_[i] : groundTexture_,
                kSplatNormalBaseIndex + static_cast<NS::UInteger>(i));
        }
        encoder->setFragmentTexture(splatMaskA_ != nullptr ? splatMaskA_ : groundTexture_,
                                    kSplatMaskAIndex);
        encoder->setFragmentTexture(splatMaskB_ != nullptr ? splatMaskB_ : groundTexture_,
                                    kSplatMaskBIndex);
        encoder->setFragmentBytes(&splat, sizeof(splat), kSplatUniformBufferIndex);

        // The same cull-and-merge as the shadow pass, against the view frustum
        // instead of the light's box. Conservative: a chunk that merely
        // straddles a plane is kept, because culling anything not wholly
        // inside clips terrain at the edge of the screen.
        const Frustum frustum = frustumOf(uniforms.viewProjection);
        drawTerrainChunks(encoder, [&frustum](const TerrainChunk& chunk) {
            return frustum.intersectsBox(simd_make_float3(chunk.minX, chunk.minY, chunk.minZ),
                                         simd_make_float3(chunk.maxX, chunk.maxY, chunk.maxZ));
        }, camera_.eye());
    }

    // --- Selection outlines ------------------------------------------------
    // BEFORE the units, which is the whole trick. The shell is drawn with depth
    // write, then the unit is drawn over it, so all that survives is the few pixels
    // of shell that stick out past the silhouette.
    //
    // Drawn AFTER the units instead, it shows its own internal creases: a hard-edged
    // model has split normals wherever it has a crease, the shell tears open along
    // every one of them, and its far side shows through the unit's own surface as a
    // green line across the hull. Which is what this looked like at first.
    //
    // Skipped in the reflection pass: a mirror showing the interface would be the
    // interface appearing twice.
    if (!outlineRuns_.empty() && outlinePipeline_ != nullptr && override == nullptr) {
        encoder->setRenderPipelineState(outlinePipeline_);
        // Front faces culled: what is wanted is the far side of the shell. Depth
        // TESTED, so a shell hides behind terrain and behind other units — and depth
        // NOT WRITTEN, which is the part that took three attempts.
        //
        // With depth write, the shell wins at every crease: a hard-edged model has
        // split normals there, the extruded shell pushes the two sides apart, and the
        // gap between them lands NEARER than the model's own surface. So the unit
        // drawn next fails its depth test along every crease and the shell shows
        // through as green lines across the hull. Writing nothing leaves the depth
        // buffer holding the terrain, the unit passes everywhere it should, and its
        // colour covers all of the shell but the rim.
        encoder->setCullMode(MTL::CullMode::CullModeFront);
        encoder->setDepthStencilState(decalDepthState_);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

        for (const OutlineRun& run : outlineRuns_) {
            const GpuUnitBatch& batch = unitBatches_[run.batch];

            encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
            encoder->setVertexBuffer(
                outlineBuffer_,
                static_cast<NS::UInteger>((instanceSlot_ * kMaxOutlinedUnits
                                           + run.firstInstance)
                                          * sizeof(UnitInstance)),
                kInstanceBufferIndex);
            encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

            PoseUniforms pose;
            pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
            pose.boneCount =
                static_cast<std::uint32_t>(batch.boneStrideBytes / sizeof(BoneTransform));
            pose.duration = batch.duration;
            pose.time = batch.animationDrivenByInstance ? 0.0f : animationTime_;
            setBuilderAimUniforms(pose, batch.builderAim);
            encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

            encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                           static_cast<NS::UInteger>(batch.indexCount),
                                           MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                           /*indexBufferOffset=*/0,
                                           static_cast<NS::UInteger>(run.instanceCount));
        }

        // Back to the default, since everything after this expects it.
        encoder->setCullMode(MTL::CullMode::CullModeNone);
    }

    // --- Units -------------------------------------------------------------
    // Deliberately NOT nested inside the terrain branch: a model should be
    // viewable with no map loaded, and the first version of this got that wrong
    // in a way that silently drew nothing at all.
    if (!unitBatches_.empty()) {
        encoder->setRenderPipelineState(unitPipeline_);
        encoder->setDepthStencilState(depthState_);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);

        // The vertex stage reads only the view-projection out of the uniforms,
        // which is the same for every batch, so it is set once here. The
        // fragment stage's copy carries the per-pair hasTexture flags and is
        // re-set whenever the pair changes.
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

        // Impossible pair, so the first batch always binds. The batches arrive
        // grouped by pair (setUnits ordered them), so this rebinds once per
        // distinct pair rather than once per model.
        TexturePair bound{-2, -2};

        const auto textureAt = [this](int index) -> MTL::Texture* {
            if (index < 0 || static_cast<std::size_t>(index) >= unitTextures_.size()) {
                return nullptr;
            }
            return unitTextures_[static_cast<std::size_t>(index)];
        };

        for (const GpuUnitBatch& batch : unitBatches_) {
            if (batch.instanceCount == 0) {
                continue;  // registered but not yet built: only the ghost may draw it
            }
            MTL::Texture* diffuse = textureAt(batch.textures.diffuse);
            MTL::Texture* shading = textureAt(batch.textures.shading);
            MTL::Texture* normals = textureAt(batch.normals);

            if (!(batch.textures == bound)) {
                bound = batch.textures;

                // Both slots are always bound — referencing an unbound texture
                // is undefined even on a branch that does not sample it — so the
                // fallback block stands in for whichever the model lacks.
                encoder->setFragmentTexture(diffuse != nullptr ? diffuse : groundTexture_,
                                            kGroundTextureIndex);
                encoder->setFragmentTexture(shading != nullptr ? shading : groundTexture_,
                                            kShadingTextureIndex);
            }
            // Normal maps are not part of Recoil's authored two-texture batching key, so
            // bind this optional Supreme Commander slot per batch.
            encoder->setFragmentTexture(normals != nullptr ? normals : groundTexture_,
                                        kUnitNormalsTextureIndex);

            // Per batch, not per pair: which family's channel layout to read is
            // a property of the MODEL, and two models could share a texture pair
            // without sharing a family. Under 4 KB, so Metal copies this into
            // the command buffer inline — there is no buffer to rebind.
            TerrainUniforms unitUniforms = uniforms;
            unitUniforms.hasTexture = diffuse != nullptr ? 1.0f : 0.0f;
            unitUniforms.hasTexture2 = shading != nullptr ? 1.0f : 0.0f;
            unitUniforms.hasUnitNormals = normals != nullptr ? 1.0f : 0.0f;
            unitUniforms.supremeCommanderShading = batch.supremeCommanderShading ? 1.0f : 0.0f;
            unitUniforms.alphaIsOpacity = batch.alphaIsOpacity ? 1.0f : 0.0f;
            encoder->setFragmentBytes(&unitUniforms, sizeof(unitUniforms), kUniformBufferIndex);
            encoder->setFragmentTexture(shadowMap_, kShadowTextureIndex);
            encoder->setFragmentSamplerState(shadowSampler_, kShadowSamplerIndex);

            encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
            // This frame's ring slot: an offset, not a rebind. UnitInstance is
            // tightly packed on a 4-byte boundary, so a slot's stride satisfies
            // Apple silicon's buffer-offset alignment.
            encoder->setVertexBuffer(
                batch.instanceBuffer,
                static_cast<NS::UInteger>(instanceSlot_ * batch.instanceCapacity
                                          * sizeof(UnitInstance)),
                kInstanceBufferIndex);

            // The WHOLE keyframe buffer, not one pose's worth. Which pose to
            // read is now decided per instance in the vertex shader, so that a
            // batch is a squad rather than one unit drawn many times; binding
            // at a pose offset here is what used to force them into lockstep.
            encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

            PoseUniforms pose;
            pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
            pose.boneCount = static_cast<std::uint32_t>(
                batch.boneStrideBytes / sizeof(BoneTransform));
            pose.duration = batch.duration;
            // Zero hands the whole decision to the instances — see UnitBatch.
            pose.time = batch.animationDrivenByInstance ? 0.0f : animationTime_;
            setBuilderAimUniforms(pose, batch.builderAim);
            encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

            // One call for every instance — the whole point of the instance
            // buffer.
            encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                           static_cast<NS::UInteger>(batch.indexCount),
                                           MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                           /*indexBufferOffset=*/0,
                                           static_cast<NS::UInteger>(batch.instanceCount));
        }
    }

    // --- Props -------------------------------------------------------------
    // The map's own scenery. Same pipeline and same shaders as the units — a
    // prop is a static model with one texture — so this differs only in what it
    // binds and in the alpha flag that turns the team-colour mask into a cutout.
    //
    // AFTER the units rather than before, which costs nothing and is worth
    // stating: every prop fragment that fails its cutout is discarded, so the
    // pipeline cannot reject them by depth before shading. Drawing the opaque
    // units first means the depth buffer is already dense where they stand, and
    // a tree behind a building is rejected on depth before its alpha is sampled.
    if (!propGroups_.empty() && propsVisible_) {
        encoder->setRenderPipelineState(unitPipeline_);
        encoder->setDepthStencilState(depthState_);
        encoder->setFragmentSamplerState(groundSampler_, kGroundSamplerIndex);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentTexture(shadowMap_, kShadowTextureIndex);
        encoder->setFragmentSamplerState(shadowSampler_, kShadowSamplerIndex);

        for (const GpuPropGroup& group : propGroups_) {
            for (const GpuPropLevel& level : group.levels) {
                if (level.instanceCount == 0) {
                    continue;  // no prop of this blueprint is at this level right now
                }

                const auto propTextureAt = [this](int index) -> MTL::Texture* {
                    if (index < 0 || static_cast<std::size_t>(index) >= propTextures_.size()) {
                        return nullptr;
                    }
                    return propTextures_[static_cast<std::size_t>(index)];
                };
                MTL::Texture* albedo = propTextureAt(level.albedo);
                MTL::Texture* normals = propTextureAt(level.normals);

                // Both slots bound whatever happens: sampling an unbound texture is
                // undefined even on a branch that does not run. The shading slot
                // carries the NORMAL MAP on this path — a prop blueprint never names
                // the channel set a unit's shading texture holds.
                encoder->setFragmentTexture(albedo != nullptr ? albedo : groundTexture_,
                                            kGroundTextureIndex);
                encoder->setFragmentTexture(normals != nullptr ? normals : groundTexture_,
                                            kShadingTextureIndex);

                TerrainUniforms propUniforms = uniforms;
                propUniforms.hasTexture = albedo != nullptr ? 1.0f : 0.0f;
                // On this path hasTexture2 means "the shading slot holds a normal
                // map", which alphaIsOpacity below is what distinguishes.
                propUniforms.hasTexture2 = normals != nullptr ? 1.0f : 0.0f;
                propUniforms.supremeCommanderShading =
                    level.supremeCommanderShading ? 1.0f : 0.0f;
                propUniforms.alphaIsOpacity = 1.0f;
                encoder->setFragmentBytes(&propUniforms, sizeof(propUniforms),
                                          kUniformBufferIndex);

                encoder->setVertexBuffer(level.vertexBuffer, 0, kVertexBufferIndex);
                // This frame's slot, then this LEVEL's run inside it. The runs are
                // contiguous and finest first (cullPropsByLevel), which is what lets
                // every level of a blueprint share one buffer — a prop is drawn at
                // exactly one level, so three buffers would hold the same instances
                // three times over.
                encoder->setVertexBuffer(
                    group.instanceBuffer,
                    static_cast<NS::UInteger>((instanceSlot_ * group.instanceCapacity
                                               + level.firstInstance)
                                              * sizeof(UnitInstance)),
                    kInstanceBufferIndex);
                encoder->setVertexBuffer(level.boneBuffer, 0, kBoneBufferIndex);

                PoseUniforms pose;
                pose.poseCount = 1;
                pose.boneCount =
                    static_cast<std::uint32_t>(level.boneStrideBytes / sizeof(BoneTransform));
                pose.duration = 0.0f;
                pose.time = 0.0f;
                encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

                encoder->drawIndexedPrimitives(
                    MTL::PrimitiveType::PrimitiveTypeTriangle,
                    static_cast<NS::UInteger>(level.indexCount), MTL::IndexType::IndexTypeUInt32,
                    level.indexBuffer, /*indexBufferOffset=*/0,
                    static_cast<NS::UInteger>(level.instanceCount));
            }
        }
    }

    // --- Selection rings ---------------------------------------------------
    // After the units, so a ring is not hidden by the unit it belongs to, and
    // BEFORE the water, so a ring on a submerged shelf is tinted by the sea
    // above it like everything else down there.
    //
    // Skipped in the reflection pass: a mirror showing the interface would be
    // the interface appearing twice.
    if (decalVertexCount_ > 0 && decalPipeline_ != nullptr && override == nullptr) {
        encoder->setRenderPipelineState(decalPipeline_);
        encoder->setDepthStencilState(decalDepthState_);
        encoder->setVertexBuffer(
            decalBuffer_,
            static_cast<NS::UInteger>(instanceSlot_ * kMaxDecalVertices * sizeof(DecalVertex)),
            kVertexBufferIndex);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                                static_cast<NS::UInteger>(decalVertexCount_));
    }

    // --- The build ghost ----------------------------------------------------
    // The armed blueprint's model at the cursor, as a translucent silhouette in the
    // interface's own light. AFTER the decals so it draws over its own ground ring, and
    // depth-tested without writing (the rings' state): terrain hides the ghost's far side,
    // and the ghost hides nothing — a preview that occluded the ground being judged would
    // cost the player the one thing they are looking at.
    //
    // Skipped in the reflection pass like the rest of the interface: a mirrored ghost
    // would be the interface appearing in the sea.
    // --- Construction sites ------------------------------------------------
    //
    // Before the ghost, because a player placing a second building wants the silhouette on
    // top of the site already going up rather than behind it. Depth-tested and WRITING, unlike
    // the ghost: a construction is a real object standing on real ground, and a wall going up
    // should hide what is behind it.
    //
    // Skipped in the reflection pass with everything else that is not plain world geometry —
    // the pass renders at a quarter resolution through a wave normal, and what would arrive is
    // a smear of the scan line rather than a building.
    if (!constructions_.empty() && constructionPipeline_ != nullptr
        && constructionInstanceBuffer_ != nullptr && override == nullptr) {
        encoder->setRenderPipelineState(constructionPipeline_);
        encoder->setDepthStencilState(depthState_);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

        // The fragment side reads the frame uniforms and the shadow map too, so a
        // site is lit by the map's sun exactly as the finished building will be.
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentTexture(shadowMap_, kShadowTextureIndex);
        encoder->setFragmentSamplerState(shadowSampler_, kShadowSamplerIndex);

        auto* slots = static_cast<UnitInstance*>(constructionInstanceBuffer_->contents())
                    + instanceSlot_ * kMaxConstructions;

        std::size_t written = 0;
        for (const ConstructionDraw& site : constructions_) {
            if (written >= kMaxConstructions || site.batch >= batchForSourceIndex_.size()) {
                continue;
            }
            const std::size_t target = batchForSourceIndex_[site.batch];
            if (target == kNoBatch || target >= unitBatches_.size()) {
                continue;  // the model has not been uploaded yet; next frame it will be
            }
            const GpuUnitBatch& batch = unitBatches_[target];
            if (batch.vertexBuffer == nullptr || batch.indexBuffer == nullptr
                || batch.boneBuffer == nullptr) {
                continue;
            }

            const std::size_t slot = written++;
            slots[slot] = site.instance;

            encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
            encoder->setVertexBuffer(
                constructionInstanceBuffer_,
                static_cast<NS::UInteger>(
                    (instanceSlot_ * kMaxConstructions + slot) * sizeof(UnitInstance)),
                kInstanceBufferIndex);
            encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

            PoseUniforms pose;
            pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
            pose.boneCount =
                static_cast<std::uint32_t>(batch.boneStrideBytes / sizeof(BoneTransform));
            pose.duration = batch.duration;
            pose.time = 0.0f;  // a building goes up at rest; nothing is built mid-stride
            encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

            // The uniform block the four faction effects read. Laid out to match
            // `BuildUniforms` in the shader; the padding is there because Metal wants the
            // struct's size rounded up and a mismatch would shear every field after it.
            struct BuildUniforms {
                std::array<float, 4> tint;
                float progress;
                float baseY;
                float height;
                float seconds;
                float centreX;
                float centreZ;
                std::uint32_t style;
                std::uint32_t pad0;
            } build{
                .tint = site.tint,
                .progress = std::clamp(site.progress, 0.0f, 1.0f),
                .baseY = site.baseY,
                .height = site.heightElmos,
                .seconds = constructionSeconds_,
                .centreX = site.instance.position[0],
                .centreZ = site.instance.position[2],
                .style = static_cast<std::uint32_t>(site.style),
                .pad0 = 0,
            };
            encoder->setFragmentBytes(&build, sizeof(build), kBuildUniformBufferIndex);
            encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                           static_cast<NS::UInteger>(batch.indexCount),
                                           MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                           /*indexBufferOffset=*/0, NS::UInteger{1});
        }
    }

    if (ghost_ && ghostPipeline_ != nullptr && ghostInstanceBuffer_ != nullptr
        && override == nullptr && ghost_->batch < batchForSourceIndex_.size()) {
        const std::size_t target = batchForSourceIndex_[ghost_->batch];
        if (target != kNoBatch && target < unitBatches_.size()) {
            const GpuUnitBatch& batch = unitBatches_[target];
            if (batch.vertexBuffer != nullptr && batch.indexBuffer != nullptr
                && batch.boneBuffer != nullptr) {
                auto* slot = static_cast<UnitInstance*>(ghostInstanceBuffer_->contents())
                             + instanceSlot_;
                *slot = ghost_->instance;

                encoder->setRenderPipelineState(ghostPipeline_);
                encoder->setDepthStencilState(decalDepthState_);
                encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
                encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
                encoder->setVertexBuffer(
                    ghostInstanceBuffer_,
                    static_cast<NS::UInteger>(instanceSlot_ * sizeof(UnitInstance)),
                    kInstanceBufferIndex);
                encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

                PoseUniforms pose;
                pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
                pose.boneCount =
                    static_cast<std::uint32_t>(batch.boneStrideBytes / sizeof(BoneTransform));
                pose.duration = batch.duration;
                pose.time = 0.0f;  // a ghost stands at rest; nothing is built mid-stride
                encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

                encoder->setFragmentBytes(&ghost_->tint, sizeof(ghost_->tint),
                                          kUniformBufferIndex);
                encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                               static_cast<NS::UInteger>(batch.indexCount),
                                               MTL::IndexType::IndexTypeUInt32,
                                               batch.indexBuffer,
                                               /*indexBufferOffset=*/0, NS::UInteger{1});
            }
        }
    }

    // --- Particles ---------------------------------------------------------
    // After the decals, so dust drifts over a selection ring rather than under it,
    // and before the water for the same reason the decals are: a puff on a
    // submerged shelf is tinted by the sea above it like everything else there.
    //
    // Skipped in the reflection pass. A mirrored puff would be defensible, but the
    // pass renders at a quarter resolution into a texture the water then samples
    // through a wave normal, so what arrives is a smear — and dust is the one thing
    // in the scene whose whole appearance is a soft gradient.
    const auto drawParticles = [&](bool refracting) {
      if (particleCount_ > 0 && particlePipeline_ != nullptr && override == nullptr) {
        encoder->setCullMode(MTL::CullModeNone); // Retail particle techniques are double-sided.
        encoder->setRenderPipelineState(particlePipeline_);
        encoder->setDepthStencilState(particleDepthState_);
        encoder->setVertexBuffer(
            particleBuffer_,
            static_cast<NS::UInteger>(instanceSlot_ * kMaxParticles * sizeof(Particle)),
            kVertexBufferIndex);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

        const auto* particles = static_cast<const Particle*>(particleBuffer_->contents())
                              + instanceSlot_ * kMaxParticles;
        for (std::size_t first = 0; first < particleCount_;) {
            std::size_t end = first + 1;
            while (end < particleCount_ && particles[end].material == particles[first].material) ++end;
            const auto id = particles[first].material;
            const bool textured = id < weaponTextures_.size();
            const bool isRefraction = textured && weaponTextures_[id].uniforms[12] == 5;
            if (isRefraction != refracting) { first=end; continue; }
            const float hasRamp = textured && weaponTextures_[id].ramp ? 1.0f : 0.0f;
            encoder->setFragmentTexture(textured ? weaponTextures_[id].texture : groundTexture_, 0);
            encoder->setFragmentTexture(hasRamp > 0 ? weaponTextures_[id].ramp : groundTexture_, 1);
            encoder->setFragmentTexture(isRefraction ? sceneColour_ : groundTexture_, 2);
            const std::array<float,16> procedural{};
            const auto& material = textured ? weaponTextures_[id].uniforms : procedural;
            const int blend = static_cast<int>(material[12]);
            encoder->setRenderPipelineState(blend == 1 || blend == 2
                ? modulatedParticlePipelines_[static_cast<std::size_t>(blend-1)] : particlePipeline_);
            encoder->setFragmentBytes(material.data(), sizeof(material), 0);
            encoder->setVertexBuffer(particleBuffer_,
                (instanceSlot_ * kMaxParticles + first) * sizeof(Particle), kVertexBufferIndex);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger{0},
                NS::UInteger{4}, static_cast<NS::UInteger>(end-first));
            first = end;
        }
      }
    };
    drawParticles(false);

    // --- The grab ----------------------------------------------------------
    // A copy of everything drawn so far, for the water to refract. This is the
    // pass split the offset needs, and the reason this function owns its encoder:
    // end the pass, copy the colour target, resume with the attachments loaded
    // back, then draw the water.
    //
    // A blit rather than rendering the world a second time: one copy of one
    // texture against a second pass over all the geometry.
    const auto captureScene = [&]() {
        encoder->endEncoding();

        MTL::BlitCommandEncoder* blit = commandBuffer->blitCommandEncoder();
        blit->copyFromTexture(colourTarget, 0, 0, MTL::Origin{0, 0, 0},
                              MTL::Size{width, height, 1}, sceneColour_, 0, 0,
                              MTL::Origin{0, 0, 0});
        blit->endEncoding();

        // Load, not clear: this pass continues the last one. Getting either
        // attachment wrong here is loud — a cleared colour draws the water on an
        // empty sky, and a cleared depth lets it paint over the terrain standing
        // in front of it.
        pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionLoad);
        pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);
        if (pass->depthAttachment()->texture() != nullptr) {
            pass->depthAttachment()->setLoadAction(MTL::LoadAction::LoadActionLoad);
            pass->depthAttachment()->setStoreAction(refractingParticles ? MTL::StoreActionStore
                : MTL::StoreActionDontCare);
        }
        encoder = commandBuffer->renderCommandEncoder(pass);
    };
    if (grabbing) captureScene();

    // --- Water -------------------------------------------------------------
    // Last, so it blends over whatever terrain and units sit below y = 0. Only
    // worth drawing when something actually is below it.
    if (wantsWater) {
        uniforms.hasSceneColour = grabbing && refractionEnabled_ ? 1.0f : 0.0f;

        encoder->setRenderPipelineState(waterPipeline_);
        encoder->setDepthStencilState(waterDepthState_);
        encoder->setFragmentTexture(reflectionColour_, kReflectionTextureIndex);
        // Always bound, even when it will not be read: sampling an unbound
        // texture is undefined even on a branch that does not run. The reflection
        // target stands in when there is no copy — same format, and certainly
        // allocated.
        encoder->setFragmentTexture(sceneColour_ != nullptr ? sceneColour_ : reflectionColour_,
                                    kSceneColourTextureIndex);
        // The wave normals, or the reflection target standing in — same undefined-sampling
        // rule as above; hasWaterWaves is what decides whether the shader reads it.
        encoder->setFragmentTexture(waterWaves_ != nullptr ? waterWaves_ : reflectionColour_,
                                    16);
        encoder->setFragmentSamplerState(reflectionSampler_, kReflectionSamplerIndex);
        encoder->setVertexBuffer(waterVertexBuffer_, 0, kVertexBufferIndex);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(waterIndexCount_),
                                       MTL::IndexType::IndexTypeUInt32, waterIndexBuffer_,
                                       NS::UInteger{0});
    }

    if (refractingParticles) {
        if (wantsWater) captureScene();
        drawParticles(true);
    }

    if (includeUi && override == nullptr) {
        encodeUi(encoder, width, height, false);
    }

    encoder->endEncoding();
}

void Renderer::encodeUi(MTL::RenderCommandEncoder* encoder, unsigned int width,
                        unsigned int height, bool glass) noexcept {
    const bool hasUiGeometry = std::any_of(
        uiCapacityReport_.layers.begin(), uiCapacityReport_.layers.end(),
        [](const ui::UiLayerUsage& usage) { return usage.uploaded > 0; });
    const bool hasMinimap = minimapTexture_ != nullptr && minimapRect_[2] > 0.0f
                         && minimapRect_[3] > 0.0f;
    if (!hasUiGeometry && !hasMinimap) return;

    encoder->setFragmentSamplerState(fontSampler_, NS::UInteger{0});
    const ui::Extent hudExtent = uiViewport_.hudExtent();
    const simd_float2 viewport{
        hudExtent.width > 0.0f ? hudExtent.width : static_cast<float>(width),
        hudExtent.height > 0.0f ? hudExtent.height : static_cast<float>(height)};
    encoder->setVertexBytes(&viewport, sizeof(viewport), kUniformBufferIndex);

    const std::size_t slotBase = instanceSlot_ * ui::kUiVerticesPerFrame;
    const auto count = [&](ui::UiLayer layer, std::size_t stream) {
        return uiLayerVertexCounts_[ui::uiLayerIndex(layer)][stream];
    };
    const auto offset = [&](ui::UiLayer layer, std::size_t stream) {
        const std::size_t index = ui::uiLayerIndex(layer);
        return slotBase + index * ui::kUiLayerVertexCapacity
             + (stream == 0 ? 0 : uiLayerVertexCounts_[index][0]);
    };
    const auto bindRange = [&](ui::UiLayer layer, std::size_t stream) {
        encoder->setVertexBuffer(
            uiBuffer_,
            static_cast<NS::UInteger>(offset(layer, stream) * sizeof(text::TextVertex)),
            kVertexBufferIndex);
    };
    const auto drawSolid = [&](ui::UiLayer layer, std::size_t stream) {
        const std::size_t vertices = count(layer, stream);
        if (vertices == 0 || solidPipeline_ == nullptr) return;
        encoder->setRenderPipelineState(solidPipeline_);
        bindRange(layer, stream);
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                                static_cast<NS::UInteger>(vertices));
    };
    const auto drawImage = [&](ui::UiLayer layer, std::size_t stream) {
        const std::size_t vertices = count(layer, stream);
        if (vertices == 0 || imagePipeline_ == nullptr || iconAtlas_ == nullptr) return;
        encoder->setRenderPipelineState(imagePipeline_);
        bindRange(layer, stream);
        encoder->setFragmentTexture(iconAtlas_, NS::UInteger{0});
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                                static_cast<NS::UInteger>(vertices));
    };
    const auto drawText = [&](ui::UiLayer layer, MTL::Texture* atlas) {
        const std::size_t vertices = count(layer, 0);
        if (vertices == 0 || textPipeline_ == nullptr || atlas == nullptr) return;
        encoder->setRenderPipelineState(textPipeline_);
        bindRange(layer, 0);
        encoder->setFragmentTexture(atlas, NS::UInteger{0});
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                                static_cast<NS::UInteger>(vertices));
    };

    drawImage(ui::UiLayer::WorldOverlay, 0);
    drawSolid(ui::UiLayer::WorldOverlay, 1);

    // Glass replaces the world below panel surfaces, so it must precede the minimap's own
    // artwork. The opaque Off material retains the historical tint-over-preview ordering.
    if (glass && count(ui::UiLayer::PanelSurface, 0) > 0) {
        std::array<float, 4> material{{uiMaterial_[0], uiMaterial_[1], uiMaterial_[2], 0.0f}};
        if (uiEffects_ == ui::EffectsLevel::Reduced) {
            material[0] = std::min(material[0] + 0.12f, 1.0f);
            material[1] *= 0.75f;
            material[2] *= 0.88f;
        }
        encoder->setRenderPipelineState(glassPipeline_);
        bindRange(ui::UiLayer::PanelSurface, 0);
        encoder->setFragmentTexture(blurB_, NS::UInteger{0});
        encoder->setFragmentBytes(material.data(), sizeof(material), kUniformBufferIndex);
        encoder->drawPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
            static_cast<NS::UInteger>(count(ui::UiLayer::PanelSurface, 0)));
    }

    if (hasMinimap && imagePipeline_ != nullptr) {
        const float x0 = minimapRect_[0];
        const float y0 = minimapRect_[1];
        const float x1 = x0 + minimapRect_[2];
        const float y1 = y0 + minimapRect_[3];
        const std::array<float, 4> white{{1.0f, 1.0f, 1.0f, 1.0f}};
        const std::array<text::TextVertex, 6> quad{{
            {{x0, y0}, {0.0f, 0.0f}, white}, {{x1, y0}, {1.0f, 0.0f}, white},
            {{x1, y1}, {1.0f, 1.0f}, white}, {{x0, y0}, {0.0f, 0.0f}, white},
            {{x1, y1}, {1.0f, 1.0f}, white}, {{x0, y1}, {0.0f, 1.0f}, white},
        }};
        encoder->setRenderPipelineState(imagePipeline_);
        encoder->setVertexBytes(quad.data(), sizeof(quad), kVertexBufferIndex);
        encoder->setFragmentTexture(minimapTexture_, NS::UInteger{0});
        encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger{0},
                                NS::UInteger{6});
        if (hasFog_ && fogTexture_ != nullptr && minimapFogPipeline_ != nullptr) {
            encoder->setRenderPipelineState(minimapFogPipeline_);
            encoder->setFragmentTexture(fogTexture_, NS::UInteger{0});
            encoder->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                    NS::UInteger{0}, NS::UInteger{6});
        }
    }

    if (!glass) {
        drawSolid(ui::UiLayer::PanelSurface, 0);
    }
    drawImage(ui::UiLayer::PanelSurface, 1);
    drawSolid(ui::UiLayer::Chrome, 0);
    drawImage(ui::UiLayer::Icon, 0);
    drawText(ui::UiLayer::Label, labelFont_.atlas);
    drawText(ui::UiLayer::ForegroundReadout, readoutFont_.atlas);
}

void Renderer::drawFrame(CA::MetalDrawable* drawable) noexcept {
    // The drawable is autoreleased by the display link, so every frame needs
    // its own pool or frame objects accumulate until the app exits.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    // Whether beginFrame opened this one, and therefore whether this call owes
    // the semaphore a release. Cleared here so that every path out of this
    // function — including the one where there is no drawable to draw into —
    // settles the debt exactly once.
    const bool opened = frameOpen_;
    frameOpen_ = false;

    // Animation advances only here, in the windowed path. Offscreen captures
    // and benchmarks keep whatever time was set explicitly, because a scene
    // that moves between runs cannot be compared to itself.
    {
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (lastAnimationTick_ > 0.0) {
            animationTime_ += static_cast<float>(now - lastAnimationTick_);
        }
        lastAnimationTick_ = now;
    }

    // Wall time between successive frame starts. With CAMetalDisplayLink this
    // is the display's cadence, not the renderer's throughput — see the comment
    // on the completed handler below.
    double cpuMs = 0.0;
    if (recording_) {
        // steady_clock rather than CACurrentMediaTime: same monotonic guarantee,
        // no extra QuartzCore ObjC header in this translation unit.
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (lastFrameStart_ > 0.0) {
            cpuMs = (now - lastFrameStart_) * 1000.0;
        }
        lastFrameStart_ = now;
    }

    if (drawable != nullptr) {
        MTL::Texture* colorTexture = drawable->texture();
        const auto width = static_cast<unsigned int>(colorTexture->width());
        const auto height = static_cast<unsigned int>(colorTexture->height());
        ensureDepthTexture(width, height);

        MTL::CommandBuffer* commandBuffer = commandQueue_->commandBuffer();

        // Which props are worth drawing from where the camera is, decided once for
        // the whole frame so every pass agrees about what the scenery is.
        cullProps();

        encodeShadowPass(commandBuffer);
        encodeReflectionPass(commandBuffer);

        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();

        MTL::RenderPassColorAttachmentDescriptor* color0 = pass->colorAttachments()->object(0);
        color0->setTexture(colorTexture);
        color0->setLoadAction(MTL::LoadAction::LoadActionClear);
        color0->setStoreAction(MTL::StoreAction::StoreActionStore);
        color0->setClearColor(MTL::ClearColor::Make(kSkyR, kSkyG, kSkyB, 1.0));

        if (depthTexture_ != nullptr) {
            MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
            depth->setTexture(depthTexture_);
            depth->setLoadAction(MTL::LoadAction::LoadActionClear);
            // DontCare: nothing reads the depth buffer after the pass, and on
            // tile-based GPUs this avoids writing it back to memory at all.
            depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
            depth->setClearDepth(1.0);
        }

        // The pass runs even with zero draw calls: a render pass with
        // LoadActionClear is what performs the clear. encodeScene creates and ends
        // the encoder — it may need two of them, around the refraction's blit.
        encodeFrame(commandBuffer, pass, width, height);

        // Releases the ring slot this frame read its instances from. Metal runs
        // this on its own thread; std::counting_semaphore is the synchronisation
        // primitive, so no further locking is needed.
        //
        // ONE handler per committed frame, and the slot goes back LAST inside
        // it. Teardown (~Renderer) drains the semaphore, so "every slot handed
        // back" is also "every capture of `this` has finished running" — two
        // separate handlers would only promise an order between them, not that
        // both finished before the last one signals.
        const double recordedCpuMs = (recording_ && cpuMs > 0.0) ? cpuMs : 0.0;
        if (opened) {
            commandBuffer->addCompletedHandler(MTL::HandlerFunction{
                [this, recordedCpuMs](MTL::CommandBuffer* completed) {
                    if (recordedCpuMs > 0.0) {
                        // GPUEndTime - GPUStartTime is the driver's own
                        // measurement of the work, unaffected by vsync pacing —
                        // the CPU figure is pinned to the display refresh by
                        // CAMetalDisplayLink and says nothing about how fast
                        // the renderer could go. The handler runs on a
                        // Metal-owned thread, hence the mutex. It is taken once
                        // per frame, not per draw.
                        const double gpuMs =
                            (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                        const std::lock_guard lock{benchMutex_};
                        recorder_.add(bench::FrameSample{recordedCpuMs, gpuMs});
                    }
                    framesInFlight_.release();
                }});
        } else if (recordedCpuMs > 0.0) {
            // No slot was acquired this frame, so nothing to hand back — but
            // the buffer below still commits, so it still owes its sample.
            commandBuffer->addCompletedHandler(MTL::HandlerFunction{
                [this, recordedCpuMs](MTL::CommandBuffer* completed) {
                    const double gpuMs =
                        (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                    const std::lock_guard lock{benchMutex_};
                    recorder_.add(bench::FrameSample{recordedCpuMs, gpuMs});
                }});
        }

        commandBuffer->presentDrawable(drawable);
        commandBuffer->commit();

        pass->release();
    } else if (opened) {
        // No drawable this frame — a minimised window, or a layer with no
        // size. Nothing was committed, so no completion handler will ever run
        // and the slot has to be handed back here or the ring starves after
        // kMaxFramesInFlight such frames and the app stops rendering entirely.
        framesInFlight_.release();
    }

    pool->release();
}

} // namespace rm
