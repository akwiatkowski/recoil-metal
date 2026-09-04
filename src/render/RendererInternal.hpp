#pragma once

// The renderer's file-local furniture, shared by its five translation units.
//
// WHY THIS EXISTS (PLAN2.md §7 P7.3). `Renderer.mm` was 4,267 lines, and splitting it into the
// four files §7 names — `MapRenderer`, `UnitRenderer`, `FxRenderer`, `UiRenderer` — meant the
// uniform layouts, the buffer-index constants and the small helpers stopped being file-local.
// They were in an anonymous namespace, which is exactly right for one file and unavailable
// across five.
//
// NOT PART OF THE PUBLIC HEADER, deliberately. `Renderer.hpp` is what the app compiles against
// and it names no Metal type; everything here is `MTL::` and `simd_`, and putting it there would
// pull Metal into every translation unit that wants to draw a frame. The `Internal` in the name
// is the contract: nothing outside `src/render/` may include this.
//
// THE UNIFORM LAYOUTS MUST MATCH THE MSL EXACTLY, and the `static_assert`s below are the
// enforcement — a mismatch produces geometry that is subtly wrong rather than absent, which is
// far harder to notice. That was true when they lived in one file and it is the reason they are
// asserted rather than commented.

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "core/Error.hpp"
#include "core/camera/Frustum.hpp"
#include "render/Renderer.hpp"
#include "render/shaders/Common.hpp"
#include "render/shaders/Fx.hpp"
#include "render/shaders/Map.hpp"
#include "render/shaders/Outline.hpp"
#include "render/shaders/Ui.hpp"
#include "render/shaders/Units.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace rm::render_detail {


// Uniforms shared by every stage. The layout must match the MSL struct below
// exactly; the static_asserts are the enforcement, because a mismatch produces
// geometry that is subtly wrong rather than absent — much harder to notice.
struct TerrainUniforms {
    simd_float4x4 viewProjection;
    simd_float3 sunDirection;
    float minHeight;
    float maxHeight;
    float mapWidth;
    float mapDepth;
    float hasTexture;  ///< float, not bool: bool packing across the ABI is a trap
    simd_float3 cameraPosition;  ///< eye in world space, for the specular half-vector
    float hasTexture2;           ///< the model's shading texture, if it has one
    float waterLevel;            ///< elmos; Recoil's is always 0, SupCom's is per map
    float supremeCommanderShading;  ///< 1 when the model follows SupCom's channel layout

    // Appended rather than inserted: every offsetof below pins a field's place
    // in the MSL layout, and adding in the middle would move all of them.
    simd_float4x4 lightViewProjection;  ///< world -> shadow map clip space
    float hasShadows;                   ///< 0 when no shadow map is bound
    float animationTime;                ///< seconds; the water's wave clock
    simd_float4x4 inverseViewProjection; ///< clip -> world, for the sky's rays

    // The map's own settings, from its lighting and water blocks. Defaulted to
    // water2.fx's stock values so an SMF map — which has no such blocks — still
    // gets the engine's look rather than black.
    simd_float3 fogColour;
    float waterFresnelBias;
    simd_float3 waterSurfaceColour;
    float waterFresnelPower;
    simd_float3 waterSunColour;
    float waterColourLerp;
    float waterSkyReflection;
    float waterSunShininess;
    float clipBelowY;   ///< the reflection pass discards anything under this
    simd_float2 viewportSize;  ///< pixels, so the water can find its own screen position
    float hasReflection;       ///< 0 when the planar pass did not run this frame

    /// 1 when a copy of the colour target is bound for the water to sample.
    ///
    /// Without it the water reads the framebuffer at its own pixel and can only
    /// absorb what is directly behind it; with it, it can sample somewhere else
    /// and bend the view. See kRefractionUv in the MSL below.
    float hasSceneColour;

    /// How far the water bends what is under it, from the map's own water block
    /// (`refractionScale` in `water2.fx`, 0.015 by default).
    float waterRefractionScale;

    /// 1 when the diffuse texture's alpha is OPACITY rather than a team-colour
    /// mask — which is what it is for every Supreme Commander prop.
    ///
    /// The two families disagree about that channel and a prop disagrees with
    /// both: Recoil puts its team mask in tex1's alpha, SupCom puts it in
    /// `_SpecTeam`, and a prop's albedo alpha cuts the shape of a leaf out of the
    /// quad it is drawn on. Read as a mask, a palm frond renders as a solid green
    /// card painted in the player's colour.
    float alphaIsOpacity;

    /// The map's own cirrus colour, tinting the zenith. Appended at the END, as
    /// every field here has to be — see the offsets below.
    simd_float3 skyZenithTint;

    /// The ground's light, from the map's lighting block: `terrain.fx`'s SunColor,
    /// SunAmbience, ShadowFillColor and LightingMultiplier. At the end, same rule.
    simd_float3 sunColour;
    simd_float3 sunAmbience;
    simd_float3 shadowFill;
    float lightingMultiplier;

    /// The fog of war mask's geometry, or `hasFog == 0` for a scene that has none —
    /// an observer, a `--units` crowd, and everything that predates ADR-037.
    float hasFog;
    float fogWidthElmos;
    float fogDepthElmos;

    /// The water's scrolling wave-normal layers: xy = the two repeats (per elmo),
    /// movements = layer A scroll in xy and layer B in zw (per second), and whether a
    /// texture is bound at all. Appended at the end, same rule as everything above.
    simd_float4 waveRepeats;
    simd_float4 waveMovements;
    float hasWaterWaves;
};


static_assert(offsetof(TerrainUniforms, clipBelowY) == 380, "clipBelowY packs into the tail");
// hasReflection went into the eight bytes the struct was already padding out to
// its 16-byte alignment, so the size did NOT change and every offset below still
// holds. That is luck rather than design — the next field added here will grow
// the struct to 416 and is fine, but it must be added at the END for the same
// reason the light matrix was.
static_assert(offsetof(TerrainUniforms, viewportSize) == 384, "float2 realigns to 8 bytes");
static_assert(offsetof(TerrainUniforms, hasReflection) == 392,
              "hasReflection must land in the tail padding, not grow the struct");
static_assert(offsetof(TerrainUniforms, hasSceneColour) == 396,
              "hasSceneColour must pack against hasReflection in the tail");
static_assert(offsetof(TerrainUniforms, waterRefractionScale) == 400,
              "and the next two start a fresh 16-byte slot");
static_assert(offsetof(TerrainUniforms, alphaIsOpacity) == 404, "packed against it");
static_assert(offsetof(TerrainUniforms, skyZenithTint) == 416,
              "a float3 is 16-aligned, so it starts a fresh slot after alphaIsOpacity");
// Three float3s in a row, so each takes a full 16-byte slot of its own — no packing
// to hope for and none assumed.
static_assert(offsetof(TerrainUniforms, sunColour) == 432, "the ground's light follows the sky's");
static_assert(offsetof(TerrainUniforms, sunAmbience) == 448, "a float3 takes a whole slot");
static_assert(offsetof(TerrainUniforms, shadowFill) == 464, "and so does the next");
static_assert(offsetof(TerrainUniforms, lightingMultiplier) == 480,
              "the scalar starts the slot after the last float3");
static_assert(offsetof(TerrainUniforms, hasFog) == 484, "the fog block packs against it");
static_assert(offsetof(TerrainUniforms, fogWidthElmos) == 488, "and stays in the same slot");
static_assert(offsetof(TerrainUniforms, fogDepthElmos) == 492, "filling it exactly");
static_assert(offsetof(TerrainUniforms, waveRepeats) == 496,
              "the wave block starts on the fresh 16-byte slot after the fog block");
static_assert(sizeof(TerrainUniforms) == 544,
              "the wave block grows the struct by three 16-byte slots");
static_assert(offsetof(TerrainUniforms, fogColour) == 288, "the map block follows the matrices");
// A float3 is sixteen bytes AND sixteen-aligned, so the float after one does
// NOT pack into its tail — it starts a fresh slot and the next float3 realigns
// past it. Assuming otherwise costs 16 bytes of silent shear per pair.
static_assert(offsetof(TerrainUniforms, waterSurfaceColour) == 320, "float3 realignment");
static_assert(offsetof(TerrainUniforms, waterSunColour) == 352, "float3 realignment");
static_assert(offsetof(TerrainUniforms, lightViewProjection) == 144,
              "the light matrix must follow the existing fields, not displace them");
static_assert(offsetof(TerrainUniforms, hasShadows) == 208, "unexpected padding before hasShadows");
static_assert(offsetof(TerrainUniforms, animationTime) == 212,
              "animationTime must pack against hasShadows, not start a new 16-byte slot");
static_assert(offsetof(TerrainUniforms, inverseViewProjection) == 224,
              "a float4x4 realigns to 16 bytes after the two trailing floats");
static_assert(offsetof(TerrainUniforms, sunDirection) == 64, "float3 is 16-byte aligned in MSL");
static_assert(offsetof(TerrainUniforms, minHeight) == 80, "unexpected padding before minHeight");
static_assert(offsetof(TerrainUniforms, hasTexture) == 96, "unexpected padding before hasTexture");
static_assert(offsetof(TerrainUniforms, cameraPosition) == 112,
              "float3 realigns to 16 bytes, leaving 12 bytes of pad after hasTexture");
static_assert(offsetof(TerrainUniforms, hasTexture2) == 128, "unexpected padding before hasTexture2");
static_assert(offsetof(TerrainUniforms, waterLevel) == 132, "unexpected padding before waterLevel");
static_assert(offsetof(TerrainUniforms, supremeCommanderShading) == 136,
              "unexpected padding before supremeCommanderShading");

// The splat's constant buffer. Must match `SplatUniforms` in the MSL below.
//
// Plain float arrays rather than float4s: MSL lays out a struct in the constant
// address space with C rules, so `float[9]` is 36 contiguous bytes on both
// sides. The static_assert is what keeps that claim honest.
constexpr std::size_t kSplatLayerCount = 10;

constexpr std::size_t kSplatNormalCount = 9;

struct SplatUniforms {
    float tileElmos[kSplatLayerCount];
    float present[kSplatLayerCount];
    float normalTileElmos[kSplatNormalCount];
    float normalPresent[kSplatNormalCount];
    float enabled;
    /// How strongly the blended stratum normal tilts the geometric one.
    float normalStrength;
};

static_assert(sizeof(SplatUniforms) == 160, "SplatUniforms must match the MSL layout");
static_assert(offsetof(SplatUniforms, present) == 40, "float[10] must pack contiguously");
static_assert(offsetof(SplatUniforms, normalTileElmos) == 80, "the normal block follows");
static_assert(offsetof(SplatUniforms, normalPresent) == 116, "float[9] must pack contiguously");
static_assert(offsetof(SplatUniforms, enabled) == 152, "unexpected padding before enabled");
static_assert(offsetof(SplatUniforms, normalStrength) == 156, "normalStrength packs after it");

// Buffer/texture binding indices, shared between the C++ and MSL sides.
constexpr NS::UInteger kVertexBufferIndex = 0;
constexpr NS::UInteger kUniformBufferIndex = 1;
constexpr NS::UInteger kInstanceBufferIndex = 2;
constexpr NS::UInteger kBoneBufferIndex = 3;
constexpr NS::UInteger kPoseUniformBufferIndex = 4;

// The build effect's own block, beside the frame uniforms rather than in their
// slot: it used to squat on kUniformBufferIndex, which is why the construction
// shader could not see the map's light or the shadow map — the frame block was
// evicted by its own effect parameters.
constexpr NS::UInteger kBuildUniformBufferIndex = 5;

// Well clear of the splat's ten layer slots, which run from kSplatLayerBaseIndex.
constexpr NS::UInteger kShadowTextureIndex = 13;
constexpr NS::UInteger kShadowSamplerIndex = 2;
constexpr NS::UInteger kReflectionTextureIndex = 14;

// The copy of the colour target the water refracts through — see the grab in
// encodeScene. Its own slot rather than sharing the reflection's: the water reads
// both in the same fragment.
constexpr NS::UInteger kSceneColourTextureIndex = 15;
constexpr NS::UInteger kReflectionSamplerIndex = 3;

// Mirrors the MSL PoseUniforms exactly. Small and per batch, so it goes up with
// setVertexBytes rather than as a buffer.
struct PoseUniforms {
    std::uint32_t poseCount = 1;
    std::uint32_t boneCount = 0;
    float duration = 0.0f;
    float time = 0.0f;
    std::uint32_t builderAim = 0;
    std::array<std::uint32_t, 3> padding{};
    std::array<float, 4> yawPivot{};
    std::array<float, 4> yawAxis{};
    std::array<float, 4> pitchPivot{};
    std::array<float, 4> pitchAxis{};
};
static_assert(sizeof(PoseUniforms) == 96, "PoseUniforms must match the MSL layout");
static_assert(offsetof(PoseUniforms, yawPivot) == 32, "builder vectors start on float4 alignment");

inline void setBuilderAimUniforms(PoseUniforms& pose, const BuilderAimRig& rig) noexcept {
    if (!rig.exists()) {
        return;
    }
    pose.builderAim = 1;
    for (std::size_t i = 0; i < 3; ++i) {
        pose.yawPivot[i] = rig.yawPivot[i];
        pose.yawAxis[i] = rig.yawAxis[i];
        pose.pitchPivot[i] = rig.pitchPivot[i];
        pose.pitchAxis[i] = rig.pitchAxis[i];
    }
}
// The splat's own constant buffer, kept separate from the shared Uniforms
// rather than appended to it: Uniforms' layout is pinned by static_asserts that
// several stages depend on, and growing it to serve one stage would mean
// re-verifying all of them. Index 2 collides with kInstanceBufferIndex only in
// spelling — that one is a vertex binding and this is a fragment binding, and
// the two stages have separate binding spaces.
constexpr NS::UInteger kSplatUniformBufferIndex = 2;

constexpr NS::UInteger kGroundTextureIndex = 0;
constexpr NS::UInteger kSplatMaskAIndex = 1;
constexpr NS::UInteger kSplatMaskBIndex = 2;
/// The ten layer textures occupy indices 3..12, matching the array in the MSL.
constexpr NS::UInteger kSplatLayerBaseIndex = 3;
/// The nine stratum normal maps, past the shadow and reflection slots: 15..23.
constexpr NS::UInteger kSplatNormalBaseIndex = 15;

/// The fog of war mask: one texel per square of the viewer's vision grid, past the normals.
constexpr NS::UInteger kFogTextureIndex = 24;

/// Ring vertices the buffer holds per frame in flight.
///
/// 65536 is about 340 rings at the default 32 segments, and 1.8 MB a slot. A
/// selection larger than that is a select-all on a big army, where the rings
/// would be a solid mat of colour and the ones that go missing are the ones
/// nobody could have picked out anyway.
constexpr std::size_t kMaxDecalVertices = 65536;

/// How far the blended stratum normal tilts the geometric one.
///
/// Not from the engine, and it cannot be: SupCom renders terrain normals to a
/// screen-space buffer where the stratum normal simply IS the surface normal,
/// because its ground gets its slope from a map-wide normal map rather than
/// from geometry. This mesh already carries the real slope in its vertices, so
/// the stratum map is detail added on top — a different composition, which
/// needs a weight the original has no reason to state.
///
/// Chosen by measurement rather than taste. At 0.6 the mean pixel difference
/// against no normal maps at all is 1.13/255 over a close view — present in
/// the numbers, invisible on the screen, which is the worst of both. 2.5 reads
/// as noise on open ground. 1.5 puts grain in grass and relief on a hillside
/// at the scale a unit stands on, which is the scale a heightfield sample
/// (8 elmos) cannot express at all.
constexpr float kStratumNormalStrength = 1.5f;
/// The model shading texture — S3O's tex2, and the same slot Supreme Commander's
/// `_specTeam` texture will take.
constexpr NS::UInteger kShadingTextureIndex = 1;
constexpr NS::UInteger kGroundSamplerIndex = 0;
/// The repeating sampler the splat layers need — see where it is created.
constexpr NS::UInteger kSplatSamplerIndex = 1;

// Compiled from source at runtime — there is no Xcode on this machine, hence no
// offline `metal` compiler. Metal's runtime compiler ships with macOS itself,
// which also buys shader hot-reload later with no build step.
// The shaders, assembled from `render/shaders/` (§7 P7.3).
//
// ONE LIBRARY, six strings. Metal compiles from source at runtime (ADR-002), so the pieces are
// concatenated here and handed to `newLibrary` as one translation unit. **The order is the
// dependency order**: `Common` defines the uniform layouts and the lighting helpers that every
// later piece calls, and MSL has no forward declarations for them.
//
// Assembled once, at construction, into a `std::string` that outlives the call — not a
// temporary, which is the sharp edge here: `NS::String::string` does not copy, so a temporary
// would be freed before the compiler read it.
[[nodiscard]] inline std::string assembleShaderSource() {
    std::string source;
    source.reserve(48 * 1024);
    for (const char* part : {rm::shaders::kCommon, rm::shaders::kMap, rm::shaders::kUnits, rm::shaders::kUi,
                             rm::shaders::kFx, rm::shaders::kOutline}) {
        source += part;
        source += '\n';
    }
    return source;
}

// Sun direction, pointing from the ground *towards* the sun so the fragment
// shader can dot it against the surface normal directly. High and to one side
// so slopes on both axes are differently lit.
const simd_float3 kSunDirection = simd_normalize(simd_make_float3(0.45f, 0.78f, 0.44f));

// Sky colour behind the terrain. Fixed rather than animated: a screenshot is
// this milestone's acceptance criterion and it should be reproducible.
constexpr double kSkyR = 0.09;
constexpr double kSkyG = 0.12;
constexpr double kSkyB = 0.18;

constexpr MTL::PixelFormat kColorFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
constexpr MTL::PixelFormat kDepthFormat = MTL::PixelFormat::PixelFormatDepth32Float;

/// The shadow map's format and size. 2048 across the whole map is ~16 elmos per
/// texel on a 1024-square map — coarse enough to be cheap, fine enough that a
/// unit-sized shadow is several texels rather than one.
constexpr MTL::PixelFormat kShadowFormat = MTL::PixelFormat::PixelFormatDepth32Float;
constexpr unsigned int kShadowResolution = 2048;

/// Spans across the water surface grid. Far coarser than the terrain: the
/// surface is flat and its depth varies slowly, so this exists only to carry
/// depth to the fragment shader and give the shoreline somewhere to fade
/// across.
///
/// 256 rather than 128: at 128 the depth ramp is interpolated over 64-elmo
/// triangles on a large map and the shoreline visibly facets. Even at 256 this
/// is 131k triangles against the terrain's two million.
constexpr int kWaterSpans = 256;

/// The reflection target's size. Half of 1080p in each axis: a reflection seen
/// through a rippling surface cannot be examined closely, and this quarters the
/// cost of the extra scene pass that fills it.
constexpr unsigned int kReflectionWidth = 960;
constexpr unsigned int kReflectionHeight = 540;


constexpr MTL::PixelFormat kGroundFormat = MTL::PixelFormat::PixelFormatBC1_RGBA;

// A single BC1 block of mid-grey, used as the always-bound fallback so the
// fragment shader never references an unbound texture (which is undefined even
// on the branch that does not sample it). RGB565 0x8410 in both endpoints with
// all-zero selectors = one flat colour.
constexpr unsigned char kFallbackBlock[8] = {0x10, 0x84, 0x10, 0x84, 0x00, 0x00, 0x00, 0x00};

// Wraps NS::Error text into our exception type. [[noreturn]] so call sites
// read as plain control flow: `if (!x) throwMetalError(...);`.
[[noreturn]] inline void throwMetalError(const char* what, NS::Error* err) {
    std::string message{what};
    if (err != nullptr && err->localizedDescription() != nullptr) {
        message += ": ";
        message += err->localizedDescription()->utf8String();
    }
    throw rm::RendererError{message};
}

/// Builds a pipeline from two named functions in an already-compiled library.
/// A pipeline with no colour attachment and no fragment shader: the depth
/// buffer is the whole output. That is what a shadow map is, and Metal is
/// perfectly happy to rasterise with nothing bound to write colour into.
[[nodiscard]] inline MTL::RenderPipelineState* makeDepthOnlyPipeline(MTL::Device* device,
                                                              MTL::Library* library,
                                                              const char* vertexName,
                                                              const char* fragmentName = nullptr) {
    MTL::Function* vertexFn =
        library->newFunction(NS::String::string(vertexName, NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    // A fragment function only where one is needed: an alpha-cut caster has to be
    // able to refuse fragments, and it pays for that with early-depth rejection —
    // which is per pipeline, so the casters that need no cutout keep theirs.
    MTL::Function* fragmentFn = nullptr;
    if (fragmentName != nullptr) {
        fragmentFn = library->newFunction(NS::String::string(fragmentName, NS::UTF8StringEncoding));
        descriptor->setFragmentFunction(fragmentFn);
    }
    descriptor->setDepthAttachmentPixelFormat(kShadowFormat);

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = device->newRenderPipelineState(descriptor, &error);

    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (fragmentFn != nullptr) fragmentFn->release();
    if (pipeline == nullptr) {
        throwMetalError("failed to create shadow pipeline", error);
    }
    return pipeline;
}

// The two faces, and why each is what it is.
//
// LABELS: Avenir Next Condensed Demi Bold. Geometric, narrow, and it holds up uppercase and
// letterspaced, which is what makes a label read as silkscreened onto a panel rather than
// typed into it. Condensed also buys width back in an interface with a lot of short words.
//
// READOUTS: Menlo. TABULAR figures, which is the whole reason — in a proportional face a live
// mass figure jitters sideways as its digits change width, and the eye reads that as the text
// being unstable rather than the number being live.
//
// The pairing is the point: an engraved label against a mechanical counter. Both are on every
// Mac, so neither is a dependency.
constexpr const char* kLabelFontName = "AvenirNextCondensed-DemiBold";
constexpr const char* kReadoutFontName = "Menlo";
constexpr float kLabelPointSize = 15.0f;
constexpr float kReadoutPointSize = 15.0f;

enum class BlendMode {
    Opaque,
    StraightAlpha,
    PremultipliedAlpha,
};

[[nodiscard]] constexpr MTL::BlendFactor sourceRgbBlendFactor(BlendMode mode) noexcept {
    return mode == BlendMode::StraightAlpha ? MTL::BlendFactor::BlendFactorSourceAlpha
                                            : MTL::BlendFactor::BlendFactorOne;
}

[[nodiscard]] constexpr MTL::BlendFactor sourceAlphaBlendFactor(BlendMode /*mode*/) noexcept {
    // Porter-Duff source-over always contributes source alpha once, regardless of whether
    // RGB arrived straight or premultiplied.
    return MTL::BlendFactor::BlendFactorOne;
}

static_assert(sourceRgbBlendFactor(BlendMode::StraightAlpha)
              == MTL::BlendFactor::BlendFactorSourceAlpha);
static_assert(sourceRgbBlendFactor(BlendMode::PremultipliedAlpha)
              == MTL::BlendFactor::BlendFactorOne);
static_assert(sourceAlphaBlendFactor(BlendMode::StraightAlpha)
              == MTL::BlendFactor::BlendFactorOne);
static_assert(sourceAlphaBlendFactor(BlendMode::PremultipliedAlpha)
              == MTL::BlendFactor::BlendFactorOne);

[[nodiscard]] inline MTL::RenderPipelineState* makePipeline(MTL::Device* device, MTL::Library* library,
                                                      const char* vertexName,
                                                      const char* fragmentName, BlendMode blend,
                                                      MTL::PixelFormat depthFormat = kDepthFormat) {
    MTL::Function* vertexFn =
        library->newFunction(NS::String::string(vertexName, NS::UTF8StringEncoding));
    MTL::Function* fragmentFn =
        library->newFunction(NS::String::string(fragmentName, NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    descriptor->setFragmentFunction(fragmentFn);

    MTL::RenderPipelineColorAttachmentDescriptor* color0 =
        descriptor->colorAttachments()->object(0);
    color0->setPixelFormat(kColorFormat);
    if (blend != BlendMode::Opaque) {
        color0->setBlendingEnabled(true);
        color0->setSourceRGBBlendFactor(sourceRgbBlendFactor(blend));
        color0->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        color0->setSourceAlphaBlendFactor(sourceAlphaBlendFactor(blend));
        color0->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    }
    // The pipeline must know the depth format or the render pass silently
    // refuses to write depth.
    descriptor->setDepthAttachmentPixelFormat(depthFormat);

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = device->newRenderPipelineState(descriptor, &error);

    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (fragmentFn != nullptr) fragmentFn->release();

    if (pipeline == nullptr) {
        throwMetalError("failed to create render pipeline", error);
    }
    return pipeline;
}


/// Pixels of empty space around each glyph in the atlas.
///
/// One, and it earns its keep: the sampler is LINEAR, so a glyph touching its neighbour
/// bleeds a sliver of that neighbour's ink along the shared edge. It shows up as a faint
/// vertical smear beside letters and reads as a bad font rather than a packing bug.
constexpr int kGlyphPadding = 1;

} // namespace rm::render_detail

namespace rm {

// `Renderer::drawTerrainChunks` is a TEMPLATE, so its definition has to be visible wherever it
// is called — and after the §7 P7.3 split that is three translation units: the scene pass in
// `Renderer.mm`, the reflection pass in `MapRenderer.mm`, and the shadow pass in
// `FxRenderer.mm`. A template member cannot be defined in one `.mm` and used in another; the
// compiler says so plainly ("its type does not have linkage", because the predicate is a
// lambda).
//
// So it lives here rather than in the public header, for the same reason everything else in
// this file does: its body names `MTL::` types, and `Renderer.hpp` deliberately names none.

// The uniform layouts and buffer indices this body uses come from the namespace above.
using namespace render_detail;  // NOLINT(google-build-using-namespace)

template <typename Predicate>
void Renderer::drawTerrainChunks(MTL::RenderCommandEncoder* encoder, Predicate keep,
                                 simd_float3 detailFrom, bool varyDetail) noexcept {
    // Which ranges to draw is arithmetic over the chunk bounds, and lives in
    // core/ where a draw count is a test rather than something nothing can see.
    chunkDraws_.clear();
    appendChunkDraws(chunkDraws_, terrainChunks_, keep,
                     [detailFrom, varyDetail](const TerrainChunk& chunk) {
                         if (!varyDetail) {
                             return 0;  // the shadow pass: shade needs no detail
                         }
                         const simd_float3 centre =
                             simd_make_float3((chunk.minX + chunk.maxX) * 0.5f,
                                              (chunk.minY + chunk.maxY) * 0.5f,
                                              (chunk.minZ + chunk.maxZ) * 0.5f);
                         return lodForDistance(simd_distance(centre, detailFrom),
                                               chunk.maxX - chunk.minX);
                     });

    for (const ChunkDraw& draw : chunkDraws_) {
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle,
            static_cast<NS::UInteger>(draw.indexCount), MTL::IndexType::IndexTypeUInt32,
            indexBuffer_, static_cast<NS::UInteger>(draw.firstIndex * sizeof(std::uint32_t)));
    }
}

} // namespace rm
