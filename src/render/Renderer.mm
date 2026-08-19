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

#include "core/Error.hpp"
#include "core/camera/Frustum.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <string>

namespace {

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
};


static_assert(offsetof(TerrainUniforms, clipBelowY) == 380, "clipBelowY packs into the tail");
static_assert(sizeof(TerrainUniforms) == 400, "a float2 realigns to 8 bytes after clipBelowY");
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

struct SplatUniforms {
    float tileElmos[kSplatLayerCount];
    float present[kSplatLayerCount];
    float enabled;
};

static_assert(sizeof(SplatUniforms) == 84, "SplatUniforms must match the MSL layout");
static_assert(offsetof(SplatUniforms, present) == 40, "float[10] must pack contiguously");
static_assert(offsetof(SplatUniforms, enabled) == 80, "unexpected padding before enabled");

// Buffer/texture binding indices, shared between the C++ and MSL sides.
constexpr NS::UInteger kVertexBufferIndex = 0;
constexpr NS::UInteger kUniformBufferIndex = 1;
constexpr NS::UInteger kInstanceBufferIndex = 2;
constexpr NS::UInteger kBoneBufferIndex = 3;
constexpr NS::UInteger kPoseUniformBufferIndex = 4;

// Well clear of the splat's ten layer slots, which run from kSplatLayerBaseIndex.
constexpr NS::UInteger kShadowTextureIndex = 13;
constexpr NS::UInteger kShadowSamplerIndex = 2;
constexpr NS::UInteger kReflectionTextureIndex = 14;
constexpr NS::UInteger kReflectionSamplerIndex = 3;

// Mirrors the MSL PoseUniforms exactly. Small and per batch, so it goes up with
// setVertexBytes rather than as a buffer.
struct PoseUniforms {
    std::uint32_t poseCount = 1;
    std::uint32_t boneCount = 0;
    float duration = 0.0f;
    float time = 0.0f;
};
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
/// The model shading texture — S3O's tex2, and the same slot Supreme Commander's
/// `_specTeam` texture will take.
constexpr NS::UInteger kShadingTextureIndex = 1;
constexpr NS::UInteger kGroundSamplerIndex = 0;
/// The repeating sampler the splat layers need — see where it is created.
constexpr NS::UInteger kSplatSamplerIndex = 1;

// Compiled from source at runtime — there is no Xcode on this machine, hence no
// offline `metal` compiler. Metal's runtime compiler ships with macOS itself,
// which also buys shader hot-reload later with no build step.
constexpr const char* kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

// packed_float3 (12 bytes, no padding) matches the C++ TerrainVertex exactly.
// Plain float3 would be 16-byte aligned and would shear the vertex stream.
struct TerrainVertexIn {
    packed_float3 position;
    packed_float3 normal;
};

struct Uniforms {
    float4x4 viewProjection;
    float3 sunDirection;
    float minHeight;
    float maxHeight;
    float mapWidth;
    float mapDepth;
    float hasTexture;
    float3 cameraPosition;
    float hasTexture2;
    float waterLevel;
    float supremeCommanderShading;
    float4x4 lightViewProjection;
    float hasShadows;
    float animationTime;
    float4x4 inverseViewProjection;
    float3 fogColour;
    float waterFresnelBias;
    float3 waterSurfaceColour;
    float waterFresnelPower;
    float3 waterSunColour;
    float waterColourLerp;
    float waterSkyReflection;
    float waterSunShininess;
    float clipBelowY;
    float2 viewportSize;
};

// The sky, as Supreme Commander's own `effects/sky.fx` builds it: a lerp
// between a horizon colour and a zenith colour, driven by elevation
// (`AtmospherePS`, sky.fx:229-236). The engine drives the blend through a
// lookup texture and takes both colours per map; a `.scmap` carries them in
// its skybox block, which this loader parses but does not yet expose — so
// these are stand-ins with the engine's structure rather than its values.
constant float3 kHorizonColour = float3(0.52, 0.60, 0.70);   // fallback; maps override
constant float3 kZenithColour = float3(0.11, 0.24, 0.48);

/// Sky colour along a view direction. Shared by the sky pass and the water's
/// reflection, so the sea reflects the sky that is actually there.
static float3 skyColour(float3 direction, float3 sunDirection, float3 horizonColour) {
    const float3 d = normalize(direction);

    // Elevation drives the gradient. The power biases the blend toward the
    // horizon, where a real sky spends most of its visible area.
    const float elevation = saturate(d.y);
    float3 colour = mix(horizonColour, kZenithColour, pow(elevation, 0.55));

    // A sun, and the broad glow around it that makes a sky look lit rather
    // than painted.
    const float towardSun = saturate(dot(d, sunDirection));
    colour += float3(1.0, 0.86, 0.66) * pow(towardSun, 8.0) * 0.18;
    colour += float3(1.0, 0.94, 0.82) * pow(towardSun, 900.0) * 3.0;

    // Below the horizon there is no sky to describe, but an RTS camera looks
    // DOWN — so most of the frame is below-horizon rays and holding one flat
    // colour there makes the whole background a single slab. Fade gently
    // toward a dimmer ground haze instead, which reads as distance.
    if (d.y < 0.0) {
        const float belowness = saturate(-d.y * 1.6);
        colour = mix(colour, horizonColour * 0.72, belowness);
    }
    return colour;
}

struct SkyOut {
    float4 position [[position]];
    float3 direction;
};

// A full-screen triangle, generated from the vertex id — no buffer, no mesh.
// Bigger than the screen on purpose: one triangle rasterises without the seam
// two would leave down the diagonal.
vertex SkyOut skyVertex(uint vid [[vertex_id]], constant Uniforms& u [[buffer(1)]]) {
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    const float2 ndc = corners[vid];

    // Unproject the far plane to get the world-space ray through this pixel.
    const float4 far = u.inverseViewProjection * float4(ndc, 1.0, 1.0);

    SkyOut out;
    // z = 1 puts the sky at the far plane, so anything in the scene wins the
    // depth test against it without the sky needing to be drawn last.
    out.position = float4(ndc, 1.0, 1.0);
    out.direction = far.xyz / far.w - u.cameraPosition;
    return out;
}

fragment float4 skyFragment(SkyOut in [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
    // The map's own fog colour is what its horizon fades to — stated by the
    // .scmap's lighting block rather than chosen here.
    return float4(skyColour(in.direction, u.sunDirection, u.fogColour), 1.0);
}

// How much of the sun a shadowed fragment keeps. Not zero: shadows in daylight
// are lit by the sky, and a black shadow reads as a hole in the ground rather
// than as shade.
constant float kShadowedLight = 0.35;

/// Fraction of the sun reaching a world position, 1 outside the shadow map.
///
/// Four taps in a rotated square rather than one, which is the cheapest thing
/// that stops a shadow edge from being a staircase of shadow-map texels. The
/// comparison is done by the sampler, so each tap is already a 0/1 the hardware
/// filters — a plain sample and manual compare would give hard edges however
/// many taps it took.
static float sunlightAt(float3 world, float ndotl, constant Uniforms& u,
                        depth2d<float> shadowMap, sampler shadowSampler) {
    if (u.hasShadows < 0.5) {
        return 1.0;
    }

    const float4 lightClip = u.lightViewProjection * float4(world, 1.0);
    if (lightClip.w <= 0.0) {
        return 1.0;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;

    // Metal's clip space is x,y in [-1,1] with +Y up, and a texture's origin is
    // top-left — hence the flip on V. Depth is already [0,1].
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) {
        return 1.0;  // outside what the shadow map covers
    }

    // Bias against shadow acne — a surface shadowing itself because one shadow
    // texel covers several elmos of ground and stores a single depth for all of
    // it. Applied here rather than through setDepthBias, whose units are the
    // depth format's smallest resolvable step and not a quantity worth guessing
    // at over a map-sized orthographic range.
    //
    // Scaled by how obliquely the sun strikes the surface: a face nearly edge-on
    // to the light spans far more depth across one texel than one facing it, and
    // a single constant either leaves the oblique case striped or lifts shadows
    // clean off the ground everywhere else.
    const float slopeFactor = saturate(1.0 - ndotl);
    const float compareDepth = ndc.z - (2.0e-4 + 2.0e-3 * slopeFactor);

    const float texel = 1.0 / float(shadowMap.get_width());
    float sum = 0.0;
    sum += shadowMap.sample_compare(shadowSampler, uv + float2(-texel, -texel), compareDepth);
    sum += shadowMap.sample_compare(shadowSampler, uv + float2( texel, -texel), compareDepth);
    sum += shadowMap.sample_compare(shadowSampler, uv + float2(-texel,  texel), compareDepth);
    sum += shadowMap.sample_compare(shadowSampler, uv + float2( texel,  texel), compareDepth);
    const float lit = sum * 0.25;

    return mix(kShadowedLight, 1.0, lit);
}

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float height;
    float3 world;   // for the shadow lookup
};

vertex VertexOut terrainVertex(uint vid [[vertex_id]],
                               const device TerrainVertexIn* vertices [[buffer(0)]],
                               constant Uniforms& u [[buffer(1)]]) {
    const float3 worldPosition = float3(vertices[vid].position);

    VertexOut out;
    out.position = u.viewProjection * float4(worldPosition, 1.0);
    out.normal = float3(vertices[vid].normal);
    // The ground texture covers the map exactly — 1 texel per elmo — so the UV
    // is just the world position normalised by the map extent. No per-vertex UV
    // needs storing, which saves 8 MB on a million-vertex map.
    out.uv = float2(worldPosition.x / u.mapWidth, worldPosition.z / u.mapDepth);
    out.height = worldPosition.y;
    out.world = worldPosition;
    return out;
}

// The Supreme Commander ground splat. Nine tiled layers, the upper eight
// weighted per texel by two masks — SupCom bakes no ground image, so this is
// assembled every frame from textures that live in the game's archives.
struct SplatUniforms {
    float tileElmos[10];  ///< ground covered by one repeat of each layer
    float present[10];    ///< 0 for an unused slot, whose mask channel is unreliable
    float enabled;
};

fragment float4 terrainFragment(VertexOut in [[stage_in]],
                                constant Uniforms& u [[buffer(1)]],
                                constant SplatUniforms& s [[buffer(2)]],
                                texture2d<float> ground [[texture(0)]],
                                texture2d<float> maskA [[texture(1)]],
                                texture2d<float> maskB [[texture(2)]],
                                array<texture2d<float>, 10> layers [[texture(3)]],
                                depth2d<float> shadowMap [[texture(13)]],
                                sampler groundSampler [[sampler(0)]],
                                sampler splatSampler [[sampler(1)]],
                                sampler shadowSampler [[sampler(2)]]) {
    // The reflection pass clips everything below the water: a mirror cannot
    // show the seabed, and without this the reflection is of the ground under
    // the water rather than the world above it.
    if (in.world.y < u.clipBelowY) {
        discard_fragment();
    }

    // Interpolating unit normals across a triangle does not preserve length.
    const float3 normal = normalize(in.normal);

    // Lambert against a fixed sun.
    const float lambert = saturate(dot(normal, u.sunDirection));

    float3 albedo;
    if (s.enabled > 0.5) {
        // Layer UVs are WORLD space, not map-normalised: a layer repeats every
        // tileElmos regardless of how big the map is. The world position is
        // recovered from the map-normalised uv rather than interpolated
        // separately, which keeps the vertex format unchanged.
        const float2 world = float2(in.uv.x * u.mapWidth, in.uv.y * u.mapDepth);

        float3 colour = layers[0].sample(splatSampler, world / s.tileElmos[0]).rgb;

        // The masks are map-wide, so they use the normalised uv. Metal presents
        // a BGRA8 texture as rgba in the shader — the format describes memory
        // order, not channel meaning — so no swizzle is needed here.
        //
        // EXPANDED, not raw: the engine's own shader reads every mask as
        // saturate(m * 2 - 1) (terrain.fx, TerrainAlbedoXP). So the bottom half
        // of the range means "absent" rather than "a little", and only 0.5..1.0
        // carries weight. Using the raw value bleeds every stratum across the
        // whole map at up to half strength.
        const float4 a = saturate(maskA.sample(groundSampler, in.uv) * 2.0 - 1.0);
        const float4 b = saturate(maskB.sample(groundSampler, in.uv) * 2.0 - 1.0);
        const float weights[8] = {a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a};

        // Strictly ordered: each stratum is laid over everything beneath it, so
        // this is a chain of mixes and not a weighted sum. A sum would wash out
        // wherever two strata overlap, which on these maps is most edges.
        for (uint i = 0; i < 8; ++i) {
            const float weight = weights[i] * s.present[i + 1];
            const float3 layer =
                layers[i + 1].sample(splatSampler, world / s.tileElmos[i + 1]).rgb;
            colour = mix(colour, layer, weight);
        }

        // The macrotexture, last and over everything, keyed on its OWN alpha
        // rather than on a mask channel — `lerp(albedo, upper.rgb, upper.w)` in
        // terrain.fx. It is the one layer whose coverage travels with the
        // texture instead of with the map.
        if (s.present[9] > 0.5) {
            const float4 upper = layers[9].sample(splatSampler, world / s.tileElmos[9]);
            colour = mix(colour, upper.rgb, upper.a);
        }

        albedo = colour;
    } else if (u.hasTexture > 0.5) {
        albedo = ground.sample(groundSampler, in.uv).rgb;
    } else {
        // No .smt: fall back to colouring by elevation so relief still reads.
        const float span = max(u.maxHeight - u.minHeight, 1.0);
        const float t = saturate((in.height - u.minHeight) / span);
        albedo = mix(float3(0.18, 0.34, 0.16), float3(0.66, 0.62, 0.55), t);
    }

    // A little ambient so slopes facing away from the sun stay readable.
    const float ambient = 0.35;
    // Shadow attenuates the SUN only. Ambient is the sky, which reaches into
    // shade — dimming it too would make shadowed ground black.
    const float sun = sunlightAt(in.world, lambert, u, shadowMap, shadowSampler);
    return float4(albedo * (ambient + lambert * 0.8 * sun), 1.0);
}

// --- Water ------------------------------------------------------------------
// Recoil's water is a plane hard-coded at y = 0 (rts/Map/Ground.h:32); Supreme
// Commander stores an elevation per map. One uniform covers both. Four vertices
// generated from the map extents; no buffer needed.
struct WaterOut {
    float4 position [[position]];
    float3 world;
    float depth;
};

// One water vertex: a position on the plane, and how deep the water is there.
// Depth is baked at upload from the terrain under it — cheaper than sampling a
// height texture per fragment, and it is what makes a shoreline fade instead of
// ending in a hard blue line.
struct WaterVertexIn {
    packed_float3 position;
    float depth;   ///< elmos of water above the ground; <= 0 on dry land
};

vertex WaterOut waterVertex(uint vid [[vertex_id]],
                            const device WaterVertexIn* vertices [[buffer(0)]],
                            constant Uniforms& u [[buffer(1)]]) {
    const float3 world = float3(vertices[vid].position);

    WaterOut out;
    out.position = u.viewProjection * float4(world, 1.0);
    out.world = world;
    out.depth = vertices[vid].depth;
    return out;
}

// Supreme Commander's own water constants, from `effects/water2.fx`. The names
// are the file's; the values are the file's defaults.
//
//   waterColor         (132)  the tint mixed into what is seen THROUGH the water
//   waterLerp          (138)  how much of it — 0.3, and note the engine's
//                             clamp(depth, 0.3, 0.3) makes this a constant, not
//                             a depth ramp, which is not what one would guess
//   fresnelBias/Power  (148)  the engine reads Fresnel from a lookup texture
//                             built from these; this evaluates them directly
//   skyreflectionAmount(160)  scaled by saturate(depth * 10), so shallow water
//                             reflects less sky — that IS the depth dependence
//   SunShininess/Color (181)  a tight, warm glint
constant float3 kWaterColour = float3(0.0, 0.7, 1.5);
constant float kWaterLerp = 0.3;
constant float kFresnelBias = 0.1;
constant float kFresnelPower = 1.5;
constant float kSkyReflectionAmount = 1.5;
constant float kSunShininess = 50.0;
constant float3 kWaterSunColour = float3(0.80, 0.47, 0.33);   // normalize(1.2, 0.7, 0.5)
constant float3 kWaveCrestColour = float3(1.0, 1.0, 1.0);

// What the water is seen to be sitting on, at the two ends of the depth ramp.
// This stands in for the engine's REFRACTION target — the scene rendered behind
// the water — which needs a second pass this renderer does not have. Shallow
// water shows the ground and reads green-grey; deep water absorbs the red end
// first, which is why the sea is blue.
constant float3 kShallowWater = float3(0.18, 0.34, 0.36);
constant float3 kDeepWater = float3(0.03, 0.10, 0.22);

/// Elmos of water below which the surface is treated as fully deep.
constant float kWaterDepthRange = 60.0;

// `behind` is the colour already in the framebuffer — the terrain and units
// drawn under the water this frame. Reading the render target inside the
// fragment shader is free on a tile-based GPU: the value is still in tile
// memory and never went to main memory at all.
//
// This is the engine's REFRACTION input arriving without the render-to-texture
// pass water2.fx needs, and it is what lets the water absorb what is actually
// under it rather than a depth ramp standing in for it. What it cannot do is
// the engine's refraction OFFSET — a framebuffer fetch reads this pixel and no
// other, so bending the view needs a copy of the target to sample freely.
fragment float4 waterFragment(WaterOut in [[stage_in]], float4 behind [[color(0)]],
                              constant Uniforms& u [[buffer(1)]],
                              texture2d<float> reflection [[texture(14)]],
                              sampler reflectionSampler [[sampler(3)]]) {
    // Above the waterline there is nothing to draw. The mesh covers the whole
    // map so that one buffer serves any water level, and the dry part is
    // discarded rather than uploaded conditionally.
    if (in.depth <= 0.0) {
        discard_fragment();
    }

    const float deep = saturate(in.depth / kWaterDepthRange);

    // Two crossing wave trains standing in for the engine's four scrolling
    // normal maps, perturbing the NORMAL only. Both run obliquely and at
    // incommensurable angles: aligned to X and Z their crests intersect on a
    // regular lattice, and open water reads as tiled graph paper.
    const float t = u.animationTime;
    const float2 first = float2(0.92f, 0.39f);
    const float2 second = float2(-0.36f, 0.93f);
    const float phase1 = dot(in.world.xz, first) * 0.021 + t * 0.9;
    const float phase2 = dot(in.world.xz, second) * 0.017 - t * 0.7;

    const float2 slope = (first * cos(phase1) * 0.021 + second * cos(phase2) * 0.017) * 2.4;
    const float3 normal = normalize(float3(-slope.x, 1.0, -slope.y));

    const float3 view = normalize(u.cameraPosition - in.world);
    const float3 mirrored = reflect(-view, normal);

    // The engine reads Fresnel from a texture indexed by (depth, N.V); this
    // evaluates the bias and power that texture is built from. Note how much
    // softer it is than a physical Schlick term — power 1.5 against 5 — which
    // is why the engine's water reflects noticeably even looking straight down.
    const float NdotV = saturate(dot(normal, view));
    const float fresnel = u.waterFresnelBias
                        + (1.0 - u.waterFresnelBias) * pow(1.0 - NdotV, u.waterFresnelPower);

    // What is seen through the water: the real scene behind it, absorbed with
    // depth. Beer-Lambert rather than a lerp to a colour — water removes light
    // exponentially with the distance travelled through it, and the red end
    // goes first, which is why a shallow sandy bottom stays sandy and a deep
    // one goes blue without either being painted that way.
    //
    // The path length is doubled: light goes down to the bottom and back up.
    const float3 absorption = float3(0.030, 0.012, 0.008);
    const float3 transmitted = behind.rgb * exp(-absorption * in.depth * 2.0);

    // The engine's tint over the top, at its fixed lerp.
    float3 through = mix(transmitted, u.waterSurfaceColour, u.waterColourLerp);

    // What the surface reflects. The planar pass has the world mirrored in the
    // water plane, which is the only thing that can show a cliff standing in
    // its own reflection — but it only covers what was on screen, so where it
    // has nothing the sky function fills in.
    //
    // Sampled at this fragment's own screen position, perturbed by the wave
    // normal. That perturbation is what makes the reflection ripple rather than
    // sit on the water like a decal.
    const float2 screen = in.position.xy / float2(u.viewportSize);
    // A few pixels of wobble, not a few hundred. The slope is a gradient, so
    // scaling it into UV space needs a small number: 0.35 shifts the sample by
    // a third of the screen and the reflection dapples with whatever happens to
    // be there.
    const float2 reflectionUv = saturate(screen + slope * 0.012);
    const float4 planar = reflection.sample(reflectionSampler, reflectionUv);

    const float3 sky = skyColour(mirrored, u.sunDirection, u.fogColour);
    // Alpha is the mirror's coverage: 1 where it drew world, 0 where it drew
    // nothing and the sky is the truthful answer.
    const float3 reflected = mix(sky, planar.rgb, planar.a);

    // Shallow water reflects less sky. This is where the engine's depth
    // dependence lives, rather than in the tint.
    const float skyAmount = u.waterSkyReflection * saturate(in.depth * 0.1);
    float3 colour = mix(through, reflected, saturate(skyAmount * fresnel));

    // A tight warm glint, added through the Fresnel term as the engine does.
    const float3 glint = pow(saturate(dot(mirrored, u.sunDirection)), max(u.waterSunShininess, 1.0))
                       * u.waterSunColour;
    colour += glint * fresnel;

    // Wave crests, from the same trains that made the normal. The engine gates
    // these on a threshold too (waveCrestThreshold, water2.fx:21); kept rare
    // and faint here, because two sine trains crest on a regular grid and a
    // strong term turns that into visible polka dots.
    const float crest = saturate((sin(phase1) + sin(phase2)) * 0.5 - 0.8);
    colour = mix(colour, kWaveCrestColour, crest * 0.10);

    // Composited here rather than by the blender: the shader already holds
    // what is behind, so hardware blending would be a second, redundant mix —
    // and doing it here is what lets the absorption above be a function of
    // depth rather than a single alpha.
    //
    // Very shallow water still fades out, so the shoreline dissolves instead
    // of ending in a line.
    const float shoreline = saturate(in.depth * 0.25);
    return float4(mix(behind.rgb, colour, shoreline), 1.0);
}

// --- Units ------------------------------------------------------------------
// packed_float2 for the UV, NOT float2: float2 is 8-byte aligned and would pad
// this struct to 40 bytes against the C++ ModelVertex's 36, shearing the stream.
struct UnitVertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
    uint boneIndex;
};

struct UnitInstanceIn {
    packed_float3 position;
    float rotationY;
    float scale;
    packed_float4 teamColour;
    float animationPhase;   // cycles, added to the batch clock
    float rotationX;        // pitch, radians about +X
    float rotationZ;        // roll, radians about +Z
};

// Everything the vertex shader needs to find one instance's pose inside the
// batch's baked keyframe buffer. Per batch and under 4 KB, so it rides in the
// command buffer via setVertexBytes rather than needing a buffer of its own.
struct PoseUniforms {
    uint poseCount;   // 1 when the batch does not animate
    uint boneCount;   // stride, in bones, between consecutive poses
    float duration;   // seconds; 0 when the batch does not animate
    float time;       // the batch clock, in seconds
};

// One bone's contribution: a rotation and a translation, no scale. Matches the
// C++ BoneTransform exactly (32 bytes) — a quaternion rather than a matrix, so
// there is no row/column convention to get backwards between the two languages.
struct BoneTransformIn {
    packed_float4 rotation;     // w, x, y, z
    packed_float3 translation;
    float padding;
};

/// Rotates a vector by a quaternion, the same sandwich the C++ side uses.
static float3 rotateBy(float4 q, float3 v) {
    const float3 u = q.yzw;
    const float3 t = cross(u, v) + q.x * v;
    return v + 2.0 * cross(u, t);
}

struct UnitOut {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float3 world;             // for the view vector the specular term needs
    float4 teamColour [[flat]];  // per instance, so never interpolated
};

// Recoil's own defaults for model lighting, from mapinfo.lua's `lighting` table
// (rts/Map/MapInfo.cpp:215-220). A map may override all of these; we do not read
// that table yet, so these stand in — and being the engine's defaults, they are
// what a map that says nothing gets there too.
constant float3 kUnitAmbient = float3(0.4);   // unitAmbientColor
constant float3 kUnitDiffuse = float3(0.7);   // unitDiffuseColor
constant float3 kUnitSpecular = float3(0.7);  // unitSpecularColor, defaults to diffuse
constant float kSpecularExponent = 100.0;     // specularExponent (MapInfo.cpp:221)

// Stands in for Recoil's reflection cubemap, which the shading texture's green
// channel mixes into the light (ModelFragProgGL4.glsl:130). We have no env cube
// yet, so a shiny surface reflects a flat sky — the same colour the frame is
// cleared to. Swapping a real cubemap in later changes this one line.
constant float3 kEnvironment = float3(0.09, 0.12, 0.18);

// Supreme Commander's own model constants, read from the game's HLSL rather
// than guessed — `effects/mesh.fx` in `gamedata/effects.scd`, which ADR-009
// established should be consulted before inferring anything about rendering.
//
// The phong coefficient is a *colour*, not a scalar (mesh.fx:97), so the
// engine's highlights are faintly blue. Glow multiplies the emissive channel
// (mesh.fx:56) before it joins the light sum.
constant float3 kSupComPhongCoeff = float3(0.6, 0.80, 0.90);  // NormalMappedPhongCoeff
constant float kSupComGlowMultiplier = 2.0;                   // glowMultiplier

/// Which keyframe an instance is on. Shared so the shadow pass poses a unit
/// exactly as the visible pass does — a unit casting the shadow of a different
/// frame of its walk cycle is the kind of wrongness nobody would think to look
/// for.
static uint poseIndexFor(PoseUniforms p, float animationPhase) {
    if (p.poseCount <= 1 || p.duration <= 0.0) {
        return 0;
    }
    const float phase = fract(p.time / p.duration + animationPhase);
    return min(uint(phase * float(p.poseCount)), p.poseCount - 1);
}

/// Rolls, pitches and yaws a model-space direction into world space. Applied to
/// positions and normals alike, which is why it takes a direction and the
/// caller adds the instance's position.
static float3 unitOrient(float3 local, UnitInstanceIn inst) {
    const float cosR = cos(inst.rotationZ);
    const float sinR = sin(inst.rotationZ);
    const float3 rolled = float3(cosR * local.x - sinR * local.y,
                                 sinR * local.x + cosR * local.y,
                                 local.z);

    const float cosP = cos(inst.rotationX);
    const float sinP = sin(inst.rotationX);
    const float3 pitched = float3(rolled.x,
                                  cosP * rolled.y - sinP * rolled.z,
                                  sinP * rolled.y + cosP * rolled.z);

    const float s = sin(inst.rotationY);
    const float c = cos(inst.rotationY);
    return float3(c * pitched.x + s * pitched.z, pitched.y,
                  -s * pitched.x + c * pitched.z);
}

vertex UnitOut unitVertex(uint vid [[vertex_id]],
                          uint iid [[instance_id]],
                          const device UnitVertexIn* vertices [[buffer(0)]],
                          constant Uniforms& u [[buffer(1)]],
                          const device UnitInstanceIn* instances [[buffer(2)]],
                          const device BoneTransformIn* bones [[buffer(3)]],
                          constant PoseUniforms& p [[buffer(4)]]) {
    const UnitVertexIn v = vertices[vid];
    const UnitInstanceIn inst = instances[iid];

    // Which keyframe this INSTANCE is on. The whole buffer is bound and the
    // pose is chosen here, per instance, rather than the CPU binding the buffer
    // at one pose's offset for the whole batch — that gave every unit in a
    // batch the same clock, so a squad walked in perfect lockstep.
    //
    // Still no per-frame CPU work and still nothing written: poses were baked
    // once at upload (ADR-006), and the per-instance part is a constant offset
    // added to a clock that advances by itself. fract, so a phase of any
    // magnitude or sign lands somewhere sensible in the cycle.
    const uint poseIndex = poseIndexFor(p, inst.animationPhase);

    // The bone transform is applied here rather than baked into the vertices, so
    // one vertex buffer serves every instance AND every frame of an animation.
    // At rest this is a translation for Recoil content and the identity for
    // Supreme Commander content, whose vertices are already posed
    // (core/model/Pose.hpp); under animation it is the full rigid transform.
    const BoneTransformIn bone = bones[poseIndex * p.boneCount + v.boneIndex];
    const float4 boneRotation = float4(bone.rotation);
    const float3 local = rotateBy(boneRotation, float3(v.position)) + float3(bone.translation);

    // Roll (Z), then pitch (X), then yaw (Y). The C++ side computes pitch/roll
    // in the unit's local frame (after undoing yaw) so that the model's up axis
    // aligns with the terrain normal under its feet, whatever direction the
    // unit is facing.
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);

    // The normal takes the bone's rotation but not its translation.
    const float3 spunNormal = unitOrient(rotateBy(boneRotation, float3(v.normal)), inst);

    UnitOut out;
    out.position = u.viewProjection * float4(world, 1.0);
    out.normal = spunNormal;
    out.world = world;
    out.teamColour = float4(inst.teamColour);
    // S3O texture coordinates are authored against OpenGL, whose texture origin
    // is bottom-left; Metal's is top-left and the DDS is uploaded unflipped, so
    // V is inverted here. Flipping the compressed blocks instead would mean
    // reordering bits inside every one of them.
    out.uv = float2(v.uv.x, 1.0 - v.uv.y);
    return out;
}

// --- Shadow pass -------------------------------------------------------------
// Depth only, from the sun's point of view. No fragment shader at all: the
// depth attachment is the entire output, and Metal is happy to run a pipeline
// with none.

vertex float4 terrainShadowVertex(uint vid [[vertex_id]],
                                  const device TerrainVertexIn* vertices [[buffer(0)]],
                                  constant Uniforms& u [[buffer(1)]]) {
    return u.lightViewProjection * float4(float3(vertices[vid].position), 1.0);
}

vertex float4 unitShadowVertex(uint vid [[vertex_id]],
                               uint iid [[instance_id]],
                               const device UnitVertexIn* vertices [[buffer(0)]],
                               constant Uniforms& u [[buffer(1)]],
                               const device UnitInstanceIn* instances [[buffer(2)]],
                               const device BoneTransformIn* bones [[buffer(3)]],
                               constant PoseUniforms& p [[buffer(4)]]) {
    const UnitVertexIn v = vertices[vid];
    const UnitInstanceIn inst = instances[iid];

    const uint poseIndex = poseIndexFor(p, inst.animationPhase);
    const BoneTransformIn bone = bones[poseIndex * p.boneCount + v.boneIndex];
    const float3 local =
        rotateBy(float4(bone.rotation), float3(v.position)) + float3(bone.translation);

    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);
    return u.lightViewProjection * float4(world, 1.0);
}

// Ported from Recoil's own model shader (ModelFragProgGL4.glsl:92-131), which is
// the authority on what the two textures mean:
//
//   tex1  rgb = albedo, a = TEAM COLOUR MASK  (1 = fully team-coloured)
//   tex2  r = self-illumination, g = reflectivity/specular strength,
//         a = a one-bit alpha mask, b unused by the forward path
//
// Without tex2 a model is flat-lit and never shines — which is why every BAR
// unit looked like painted cardboard until this landed.
fragment float4 unitFragment(UnitOut in [[stage_in]],
                             constant Uniforms& u [[buffer(1)]],
                             texture2d<float> diffuse [[texture(0)]],
                             texture2d<float> shading [[texture(1)]],
                             depth2d<float> shadowMap [[texture(13)]],
                             sampler texSampler [[sampler(0)]],
                             sampler shadowSampler [[sampler(2)]]) {
    if (in.world.y < u.clipBelowY) {
        discard_fragment();
    }

    // Without a diffuse the model shades flat grey with no team colour, since
    // the mask lives in that texture's alpha and there is nothing to read.
    float4 tex1 = float4(0.62, 0.62, 0.60, 0.0);
    if (u.hasTexture > 0.5) {
        tex1 = diffuse.sample(texSampler, in.uv);
    }

    // Neutral shading texture: no self-illumination, no reflectivity, and no
    // team colour. The alpha matters more than it looks — under Supreme
    // Commander's layout it IS the team mask (ADR-012), so a model whose
    // `_SpecTeam` is missing would be painted ENTIRELY in its team's colour if
    // this defaulted to 1, losing its albedo completely. Recoil reads its mask
    // from tex1 and is unaffected either way.
    float4 tex2 = float4(0.0, 0.0, 0.0, 0.0);
    if (u.hasTexture2 > 0.5) {
        tex2 = shading.sample(texSampler, in.uv);
    }

    // Where the team-colour mask lives, and what the shading texture's channels
    // mean, differ between the two content families — see Family in Model.hpp.
    // Each branch below is a port of that family's OWN shader, so neither is
    // inferred from what the data looks like.
    const bool supCom = u.supremeCommanderShading > 0.5;
    const float teamMask = supCom ? tex2.a : tex1.a;

    const float3 albedo = mix(tex1.rgb, in.teamColour.rgb, teamMask);

    const float3 N = normalize(in.normal);
    const float3 L = u.sunDirection;
    const float3 V = normalize(u.cameraPosition - in.world);

    const float rawNdotL = saturate(dot(N, L));
    const float sun = sunlightAt(in.world, rawNdotL, u, shadowMap, shadowSampler);
    const float NdotL = rawNdotL * sun;
    float3 light = kUnitAmbient + NdotL * kUnitDiffuse;

    if (supCom) {
        // Supreme Commander, from `effects/mesh.fx`'s NormalMappedPS
        // (mesh.fx:2184-2201). `_SpecTeam` carries four independent things:
        //
        //   .r  multiplies the environment reflection
        //   .g  scales an ADDITIVE Phong highlight
        //   .b  emissive, scaled by glowMultiplier
        //   .a  the team-colour mask (already applied above)
        //
        // Emissive and reflection join the light sum and are therefore tinted
        // by the albedo; only the highlight is added on top. Getting that
        // grouping wrong makes glowing panels wash out to white instead of
        // burning in their own colour.
        //
        // Classic Phong, not Blinn: the engine reflects the sun about the
        // normal and dots against the view. Note it writes
        // `dot(reflect(sunDirection, N), -viewDirection)`, which is the same
        // value as reflecting the incident direction and dotting the direction
        // to the camera — the two negations cancel.
        const float phongAmount = saturate(dot(reflect(-L, N), V));
        const float3 phongAdditive = kSupComPhongCoeff * pow(phongAmount, 2.0) * tex2.g;
        const float3 phongMultiplicative = 2.0 * kEnvironment * tex2.r;
        const float emissive = kSupComGlowMultiplier * tex2.b;

        return float4(albedo * (emissive + light + phongMultiplicative) + phongAdditive, 1.0);
    }

    // Recoil: tex1.a is the mask; tex2.r is self-illumination and tex2.g is
    // reflectivity (ModelFragProgGL4.glsl:101,129-131).
    const float selfIllum = tex2.r;
    const float shininess = tex2.g;

    const float3 H = normalize(L + V);
    const float HdotN = saturate(dot(N, H));

    // Blinn-Phong at 2.5x the Phong exponent, plus a wide low lobe — the exact
    // expression the engine uses, comment and all.
    float3 specular = kUnitSpecular * min(pow(HdotN, 2.5 * kSpecularExponent)
                                              + 0.3 * pow(HdotN, 2.0 * 3.0),
                                          1.0);
    specular *= shininess * 4.0;

    light = mix(light, kEnvironment, shininess);  // reflection
    light += float3(selfIllum);                   // self-illum

    // tex2.a is a one-bit mask the engine only *discards* on in its alpha pass,
    // where alphaCtrl is set (ModelFragProgGL4.glsl:62-70,97). The default
    // control always passes, so an opaque pass — which is all we have — must not
    // discard, or every unit with a masked tex2 loses geometry it should keep.
    return float4(albedo * light + specular, 1.0);
}
)MSL";

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
[[noreturn]] void throwMetalError(const char* what, NS::Error* err) {
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
[[nodiscard]] MTL::RenderPipelineState* makeDepthOnlyPipeline(MTL::Device* device,
                                                              MTL::Library* library,
                                                              const char* vertexName) {
    MTL::Function* vertexFn =
        library->newFunction(NS::String::string(vertexName, NS::UTF8StringEncoding));

    auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertexFn);
    descriptor->setDepthAttachmentPixelFormat(kShadowFormat);

    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pipeline = device->newRenderPipelineState(descriptor, &error);

    descriptor->release();
    if (vertexFn != nullptr) vertexFn->release();
    if (pipeline == nullptr) {
        throwMetalError("failed to create shadow pipeline", error);
    }
    return pipeline;
}

[[nodiscard]] MTL::RenderPipelineState* makePipeline(MTL::Device* device, MTL::Library* library,
                                                     const char* vertexName,
                                                     const char* fragmentName, bool blend) {
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
    if (blend) {
        color0->setBlendingEnabled(true);
        color0->setSourceRGBBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        color0->setDestinationRGBBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
        color0->setSourceAlphaBlendFactor(MTL::BlendFactor::BlendFactorSourceAlpha);
        color0->setDestinationAlphaBlendFactor(MTL::BlendFactor::BlendFactorOneMinusSourceAlpha);
    }
    // The pipeline must know the depth format or the render pass silently
    // refuses to write depth.
    descriptor->setDepthAttachmentPixelFormat(kDepthFormat);

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
    MTL::Library* library = device_->newLibrary(
        NS::String::string(kShaderSource, NS::UTF8StringEncoding),
        nullptr, // default compile options
        &error);
    if (library == nullptr) {
        throwMetalError("runtime shader compilation failed", error);
    }

    terrainShadowPipeline_ = makeDepthOnlyPipeline(device_, library, "terrainShadowVertex");
    unitShadowPipeline_ = makeDepthOnlyPipeline(device_, library, "unitShadowVertex");
    terrainPipeline_ = makePipeline(device_, library, "terrainVertex", "terrainFragment",
                                    /*blend=*/false);
    skyPipeline_ = makePipeline(device_, library, "skyVertex", "skyFragment", /*blend=*/false);
    // No hardware blending: the water shader reads the framebuffer itself and
    // composites, which is what lets it absorb by depth rather than by a single
    // alpha. Leaving blending on would mix the result a second time.
    waterPipeline_ = makePipeline(device_, library, "waterVertex", "waterFragment",
                                  /*blend=*/false);
    unitPipeline_ = makePipeline(device_, library, "unitVertex", "unitFragment",
                                 /*blend=*/false);
    library->release();

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
    depthDescriptor->release();

    if (depthState_ == nullptr || waterDepthState_ == nullptr) {
        throw RendererError{"failed to create depth-stencil state"};
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
    // Reverse acquisition order; all are +1 objects from newXxx()/CreateXxx.
    releaseWaterBuffers();
    if (reflectionSampler_ != nullptr) reflectionSampler_->release();
    if (reflectionDepth_ != nullptr) reflectionDepth_->release();
    if (reflectionColour_ != nullptr) reflectionColour_->release();
    if (skyDepthState_ != nullptr) skyDepthState_->release();
    if (skyPipeline_ != nullptr) skyPipeline_->release();
    if (shadowSampler_ != nullptr) shadowSampler_->release();
    if (shadowMap_ != nullptr) shadowMap_->release();
    if (unitShadowPipeline_ != nullptr) unitShadowPipeline_->release();
    if (terrainShadowPipeline_ != nullptr) terrainShadowPipeline_->release();
    releaseTerrainBuffers();
    releaseUnitBuffers();  // frees the unit textures too
    unitPipeline_->release();
    releaseSplat();
    if (groundTexture_ != nullptr) groundTexture_->release();
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

void Renderer::releaseTerrainBuffers() noexcept {
    if (indexBuffer_ != nullptr) {
        indexBuffer_->release();
        indexBuffer_ = nullptr;
    }
    if (vertexBuffer_ != nullptr) {
        vertexBuffer_->release();
        vertexBuffer_ = nullptr;
    }
    indexCount_ = 0;
}

void Renderer::setTerrain(const TerrainMesh& mesh) {
    releaseTerrainBuffers();

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }

    const std::size_t vertexBytes = mesh.vertices.size() * sizeof(TerrainVertex);
    const std::size_t indexBytes = mesh.indices.size() * sizeof(std::uint32_t);

    // StorageModeShared is the right default on Apple Silicon: CPU and GPU share
    // one physical memory pool, so there is nothing to blit and no private copy
    // to keep coherent.
    vertexBuffer_ = device_->newBuffer(mesh.vertices.data(), vertexBytes,
                                       MTL::ResourceStorageModeShared);
    indexBuffer_ = device_->newBuffer(mesh.indices.data(), indexBytes,
                                      MTL::ResourceStorageModeShared);

    if (vertexBuffer_ == nullptr || indexBuffer_ == nullptr) {
        releaseTerrainBuffers();
        throw RendererError{"failed to allocate terrain vertex/index buffers ("
                            + std::to_string((vertexBytes + indexBytes) / (1024 * 1024))
                            + " MiB)"};
    }

    indexCount_ = mesh.indices.size();
    terrainChunks_ = mesh.chunks;
    terrainMinY_ = mesh.minY;
    terrainMaxY_ = mesh.maxY;
    mapWidth_ = mesh.maxX - mesh.minX;
    mapDepth_ = mesh.maxZ - mesh.minZ;

    camera_.frame(simd_make_float3(mesh.minX, mesh.minY, mesh.minZ),
                  simd_make_float3(mesh.maxX, mesh.maxY, mesh.maxZ));

    // The far plane must clear the map diagonal from any orbit position, or the
    // far edge of a large map is clipped away.
    camera_.farZ = camera_.distance * 4.0f + mapWidth_ * 2.0f;

    // The water surface samples the ground under it, so it is rebuilt with the
    // terrain rather than kept as a fixed quad.
    cacheWaterGround(mesh);
    buildWaterMesh();
}

void Renderer::releaseWaterBuffers() noexcept {
    if (waterIndexBuffer_ != nullptr) waterIndexBuffer_->release();
    if (waterVertexBuffer_ != nullptr) waterVertexBuffer_->release();
    waterIndexBuffer_ = nullptr;
    waterVertexBuffer_ = nullptr;
    waterIndexCount_ = 0;
}

void Renderer::cacheWaterGround(const TerrainMesh& mesh) {
    waterGroundHeights_.clear();
    if (mesh.verticesX <= 1 || mesh.verticesZ <= 1) {
        return;
    }

    waterGroundHeights_.reserve(static_cast<std::size_t>(kWaterSpans + 1)
                                * static_cast<std::size_t>(kWaterSpans + 1));
    for (int z = 0; z <= kWaterSpans; ++z) {
        for (int x = 0; x <= kWaterSpans; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kWaterSpans);
            const float v = static_cast<float>(z) / static_cast<float>(kWaterSpans);

            // Nearest terrain vertex. The water grid is much coarser than the
            // terrain, so interpolating here would only blur a value that is
            // already interpolated across the water triangle.
            const int tx = static_cast<int>(std::lround(u * static_cast<float>(mesh.verticesX - 1)));
            const int tz = static_cast<int>(std::lround(v * static_cast<float>(mesh.verticesZ - 1)));
            waterGroundHeights_.push_back(mesh.heightAt(tx, tz));
        }
    }
}

void Renderer::buildWaterMesh() {
    releaseWaterBuffers();
    if (waterGroundHeights_.empty()) {
        return;
    }

    constexpr int kSpans = kWaterSpans;

    struct WaterVertex {
        std::array<float, 3> position;
        float depth;
    };
    static_assert(sizeof(WaterVertex) == 16, "must match the MSL WaterVertexIn");

    std::vector<WaterVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(kSpans + 1) * static_cast<std::size_t>(kSpans + 1));

    for (int z = 0; z <= kSpans; ++z) {
        for (int x = 0; x <= kSpans; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kSpans);
            const float v = static_cast<float>(z) / static_cast<float>(kSpans);
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(kSpans + 1)
                             + static_cast<std::size_t>(x);

            vertices.push_back(WaterVertex{
                .position = {u * mapWidth_, waterLevel_, v * mapDepth_},
                .depth = waterLevel_ - waterGroundHeights_[index],
            });
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(kSpans) * static_cast<std::size_t>(kSpans) * 6);
    for (int z = 0; z < kSpans; ++z) {
        for (int x = 0; x < kSpans; ++x) {
            const auto row = static_cast<std::uint32_t>(kSpans + 1);
            const auto v00 = static_cast<std::uint32_t>(z) * row + static_cast<std::uint32_t>(x);
            const std::uint32_t v10 = v00 + 1;
            const std::uint32_t v01 = v00 + row;
            const std::uint32_t v11 = v01 + 1;

            indices.insert(indices.end(), {v00, v01, v11, v00, v11, v10});
        }
    }

    waterVertexBuffer_ = device_->newBuffer(vertices.data(), vertices.size() * sizeof(WaterVertex),
                                            MTL::ResourceStorageModeShared);
    waterIndexBuffer_ = device_->newBuffer(indices.data(), indices.size() * sizeof(std::uint32_t),
                                           MTL::ResourceStorageModeShared);
    if (waterVertexBuffer_ == nullptr || waterIndexBuffer_ == nullptr) {
        releaseWaterBuffers();
        throw RendererError{"failed to allocate water buffers"};
    }
    waterIndexCount_ = indices.size();
}

void Renderer::releaseUnitBuffers() noexcept {
    for (GpuUnitBatch& batch : unitBatches_) {
        if (batch.instanceBuffer != nullptr) batch.instanceBuffer->release();
        if (batch.boneBuffer != nullptr)     batch.boneBuffer->release();
        if (batch.indexBuffer != nullptr)    batch.indexBuffer->release();
        if (batch.vertexBuffer != nullptr)   batch.vertexBuffer->release();
    }
    unitBatches_.clear();

    for (MTL::Texture* texture : unitTextures_) {
        if (texture != nullptr) {
            texture->release();
        }
    }
    unitTextures_.clear();
    batchForSourceIndex_.clear();
}

void Renderer::setUnits(std::span<const dds::Texture> textures,
                        std::span<const UnitBatch> batches) {
    releaseUnitBuffers();

    if (batches.empty()) {
        return;
    }

    // Every caller index starts unmapped, so a batch skipped below stays that
    // way and setInstances on it is a no-op rather than a stray write.
    batchForSourceIndex_.assign(batches.size(), kNoBatch);

    // Textures first: a failed upload here should leave nothing half-built, and
    // the batches below reference these by index.
    unitTextures_.reserve(textures.size());
    for (const dds::Texture& texture : textures) {
        // A texture that failed to load is kept as a null slot rather than
        // renumbering everything after it — the indices in the batches were
        // decided by the caller and must keep meaning what they meant.
        unitTextures_.push_back(texture.data.empty() ? nullptr
                                                     : uploadTexture(texture, "unit"));
    }

    // Upload in the order the draws will run, so encodeScene is a plain walk.
    std::vector<TexturePair> pairs;
    pairs.reserve(batches.size());
    for (const UnitBatch& batch : batches) {
        pairs.push_back(batch.textures);
    }
    const std::vector<std::size_t> order = orderByTexturePair(pairs);

    unitBatches_.reserve(batches.size());
    for (const std::size_t index : order) {
        const UnitBatch& batch = batches[index];
        if (batch.model == nullptr || batch.model->empty() || batch.model->bones.empty()
            || batch.instances.empty()) {
            continue;
        }
        const Model& model = *batch.model;

        // Every pose the batch will ever need, baked now and concatenated into
        // one buffer: the rest pose alone, or one pose per keyframe when an
        // animation is attached. Playing it back is then a buffer offset — no
        // per-frame CPU work, and nothing writes to a buffer the GPU may still
        // be reading from a frame in flight.
        //
        // restPose is where the two families' vertex conventions are reconciled
        // (core/model/Pose.hpp): a translation for Recoil's bone-local vertices,
        // the identity for Supreme Commander's model-space ones.
        std::vector<BoneTransform> poses = restPose(model);
        std::size_t poseCount = 1;
        float duration = 0.0f;

        if (batch.animation != nullptr && !batch.animation->empty()) {
            const std::vector<int> boneMap = mapBonesToAnimation(model, *batch.animation);
            const bool drivesAnything =
                std::any_of(boneMap.begin(), boneMap.end(), [](int i) { return i >= 0; });

            if (drivesAnything) {
                poses.clear();
                poses.reserve(model.bones.size() * batch.animation->frames.size());
                for (const sca::Frame& frame : batch.animation->frames) {
                    const std::vector<BoneTransform> pose =
                        poseAt(model, *batch.animation, boneMap, frame.time);
                    poses.insert(poses.end(), pose.begin(), pose.end());
                }
                poseCount = batch.animation->frames.size();
                duration = batch.animation->duration;
            }
        }

        GpuUnitBatch uploaded;
        uploaded.textures = batch.textures;
        uploaded.supremeCommanderShading = model.family == Family::SupremeCommander;
        uploaded.poseCount = poseCount;
        uploaded.boneStrideBytes = model.bones.size() * sizeof(BoneTransform);
        uploaded.duration = duration;
        uploaded.animationDrivenByInstance = batch.animationDrivenByInstance;
        uploaded.vertexBuffer =
            device_->newBuffer(model.vertices.data(), model.vertices.size() * sizeof(ModelVertex),
                               MTL::ResourceStorageModeShared);
        uploaded.indexBuffer =
            device_->newBuffer(model.indices.data(), model.indices.size() * sizeof(std::uint32_t),
                               MTL::ResourceStorageModeShared);
        uploaded.boneBuffer = device_->newBuffer(poses.data(), poses.size() * sizeof(BoneTransform),
                                                 MTL::ResourceStorageModeShared);
        // kMaxFramesInFlight copies of the instances, not one: a scene whose
        // units move rewrites this every frame, and a single copy would be
        // written while the GPU is still reading it. Seeded with the same data
        // in every slot so a batch that is never updated draws correctly from
        // whichever slot the frame lands on.
        uploaded.instanceCapacity = batch.instances.size();
        const std::size_t instanceBytes = uploaded.instanceCapacity * sizeof(UnitInstance);
        uploaded.instanceBuffer = device_->newBuffer(instanceBytes * kMaxFramesInFlight,
                                                     MTL::ResourceStorageModeShared);
        if (uploaded.instanceBuffer != nullptr) {
            auto* slots = static_cast<std::byte*>(uploaded.instanceBuffer->contents());
            for (std::size_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
                std::memcpy(slots + slot * instanceBytes, batch.instances.data(), instanceBytes);
            }
        }
        uploaded.indexCount = model.indices.size();
        uploaded.instanceCount = batch.instances.size();

        if (uploaded.vertexBuffer == nullptr || uploaded.indexBuffer == nullptr
            || uploaded.boneBuffer == nullptr || uploaded.instanceBuffer == nullptr) {
            // Push what was allocated so releaseUnitBuffers frees it — throwing
            // with buffers stranded in locals would leak them.
            unitBatches_.push_back(uploaded);
            releaseUnitBuffers();
            throw RendererError{"failed to allocate unit buffers for " + model.name};
        }

        batchForSourceIndex_[index] = unitBatches_.size();
        unitBatches_.push_back(uploaded);
    }
}

void Renderer::beginFrame() noexcept {
    // Blocks until at most kMaxFramesInFlight - 1 frames are still outstanding,
    // which is precisely the condition for the slot chosen next to be free: it
    // was last used kMaxFramesInFlight frames ago, and that frame's completion
    // handler has now run.
    framesInFlight_.acquire();
    instanceSlot_ = (instanceSlot_ + 1) % kMaxFramesInFlight;
    frameOpen_ = true;
}

void Renderer::setInstances(std::size_t batchIndex,
                            std::span<const UnitInstance> instances) noexcept {
    if (batchIndex >= batchForSourceIndex_.size()) {
        return;
    }

    const std::size_t target = batchForSourceIndex_[batchIndex];
    if (target == kNoBatch || target >= unitBatches_.size()) {
        return;  // the batch was skipped at upload; nothing to move
    }

    GpuUnitBatch& batch = unitBatches_[target];
    if (batch.instanceBuffer == nullptr || batch.instanceCapacity == 0) {
        return;
    }

    const std::size_t count = std::min(instances.size(), batch.instanceCapacity);
    const std::size_t slotBytes = batch.instanceCapacity * sizeof(UnitInstance);

    auto* slots = static_cast<std::byte*>(batch.instanceBuffer->contents());
    std::memcpy(slots + instanceSlot_ * slotBytes, instances.data(),
                count * sizeof(UnitInstance));

    // Drawing fewer than were uploaded is fine — the tail of the slot simply
    // goes unread — so a caller may shrink a batch without reallocating.
    batch.instanceCount = count;
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

void Renderer::setGroundTexture(const TileAtlas& atlas) {
    if (atlas.widthTexels <= 0 || atlas.heightTexels <= 0 || atlas.data.empty()) {
        return;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        kGroundFormat, static_cast<NS::UInteger>(atlas.widthTexels),
        static_cast<NS::UInteger>(atlas.heightTexels), /*mipmapped=*/false);
    descriptor->setMipmapLevelCount(static_cast<NS::UInteger>(atlas.mipLevels));
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* texture = device_->newTexture(descriptor);
    if (texture == nullptr) {
        throw RendererError{"failed to allocate the "
                            + std::to_string(atlas.data.size() / (1024 * 1024))
                            + " MiB ground texture"};
    }

    // The .smt supplies four mip levels per tile, so the atlas has four too.
    // Each is uploaded whole — the blocks are already in the layout Metal wants.
    for (int level = 0; level < atlas.mipLevels; ++level) {
        const std::span<const std::byte> mip = atlas.mip(level);
        if (mip.empty()) {
            continue;
        }
        texture->replaceRegion(
            MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(atlas.mipWidth(level)),
                                static_cast<NS::UInteger>(atlas.mipHeight(level))),
            static_cast<NS::UInteger>(level), mip.data(),
            static_cast<NS::UInteger>(atlas.mipBytesPerRow(level)));
    }

    if (groundTexture_ != nullptr) {
        groundTexture_->release();
    }
    groundTexture_ = texture;
    hasGroundTexture_ = true;
}

void Renderer::releaseSplat() noexcept {
    for (MTL::Texture*& layer : splatLayers_) {
        if (layer != nullptr) {
            layer->release();
            layer = nullptr;
        }
    }
    if (splatMaskA_ != nullptr) {
        splatMaskA_->release();
        splatMaskA_ = nullptr;
    }
    if (splatMaskB_ != nullptr) {
        splatMaskB_->release();
        splatMaskB_ = nullptr;
    }
    splatEnabled_ = false;
}

void Renderer::setSplat(std::span<const SplatLayer> layers, const dds::Texture& maskA,
                        const dds::Texture& maskB) {
    releaseSplat();

    // Both masks are required. Without them every stratum weight is undefined
    // and the map would render as its base layer alone — which looks like a
    // finished image rather than like a missing one, so refuse instead.
    if (layers.empty() || maskA.width <= 0 || maskB.width <= 0) {
        return;
    }

    splatMaskA_ = uploadTexture(maskA, "splat mask A");
    splatMaskB_ = uploadTexture(maskB, "splat mask B");

    for (std::size_t i = 0; i < kSplatLayers; ++i) {
        const bool have = i < layers.size() && layers[i].present();

        // Every slot must be bound: sampling an unbound texture is undefined,
        // and the shader references all nine unconditionally. Unused slots get
        // the same 4x4 fallback the ground texture uses, and are weighted to
        // zero by `present` so what they hold never reaches the image.
        splatLayers_[i] = have ? uploadTexture(layers[i].texture, "splat layer") : nullptr;
        splatPresent_[i] = have ? 1.0f : 0.0f;

        // Never zero, even for an absent layer: the shader divides by this to
        // build the layer UV, and a division by zero would produce NaNs that
        // survive the multiply by a zero weight.
        splatTileElmos_[i] = (have && layers[i].tileElmos > 0.0f) ? layers[i].tileElmos : 1.0f;
    }

    // The base layer is not optional — everything else is laid over it.
    splatEnabled_ = splatPresent_[0] > 0.5f;
}

void Renderer::setGroundColourMap(const ColourImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.empty()) {
        return;
    }

    // RGBA8 rather than the terrain path's BC1: this image is generated here,
    // not decoded from an asset, and compressing it would cost a block encoder
    // to save memory a terrain-type map does not use much of — a 2048-square map
    // is 16 MiB.
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormat::PixelFormatRGBA8Unorm, static_cast<NS::UInteger>(image.width),
        static_cast<NS::UInteger>(image.height), /*mipmapped=*/false);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModeShared);

    MTL::Texture* texture = device_->newTexture(descriptor);
    if (texture == nullptr) {
        throw RendererError{"failed to allocate the ground colour map"};
    }

    texture->replaceRegion(MTL::Region::Make2D(0, 0, static_cast<NS::UInteger>(image.width),
                                               static_cast<NS::UInteger>(image.height)),
                           0, image.rgba.data(),
                           static_cast<NS::UInteger>(image.width) * 4);

    if (groundTexture_ != nullptr) {
        groundTexture_->release();
    }
    groundTexture_ = texture;
    hasGroundTexture_ = true;
}

void Renderer::setEnvironment(const Environment& environment) noexcept {
    environment_ = environment;
}

void Renderer::setWater(bool enabled, float levelElmos) noexcept {
    const bool levelChanged = waterLevel_ != levelElmos;
    hasWater_ = enabled;
    waterLevel_ = levelElmos;

    // The surface bakes the level into its vertices and its depths, and this
    // call arrives AFTER setTerrain — so without the rebuild every map would
    // wear the default level of zero.
    if (levelChanged && !waterGroundHeights_.empty()) {
        buildWaterMesh();
    }
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

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
    encodeScene(encoder, width, height);
    encoder->endEncoding();

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

        MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);
        encodeScene(encoder, width, height);
        encoder->endEncoding();

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

void Renderer::encodeScene(MTL::RenderCommandEncoder* encoder, unsigned int width,
                           unsigned int height, const SceneOverride* override) noexcept {
    if (height == 0) {
        return;
    }

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
        for (std::size_t i = 0; i < kSplatLayers; ++i) {
            splat.tileElmos[i] = splatTileElmos_[i];
            splat.present[i] = splatPresent_[i];
            encoder->setFragmentTexture(
                splatLayers_[i] != nullptr ? splatLayers_[i] : groundTexture_,
                kSplatLayerBaseIndex + static_cast<NS::UInteger>(i));
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
        });
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
            MTL::Texture* diffuse = textureAt(batch.textures.diffuse);
            MTL::Texture* shading = textureAt(batch.textures.shading);

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

            // Per batch, not per pair: which family's channel layout to read is
            // a property of the MODEL, and two models could share a texture pair
            // without sharing a family. Under 4 KB, so Metal copies this into
            // the command buffer inline — there is no buffer to rebind.
            TerrainUniforms unitUniforms = uniforms;
            unitUniforms.hasTexture = diffuse != nullptr ? 1.0f : 0.0f;
            unitUniforms.hasTexture2 = shading != nullptr ? 1.0f : 0.0f;
            unitUniforms.supremeCommanderShading = batch.supremeCommanderShading ? 1.0f : 0.0f;
            encoder->setFragmentBytes(&unitUniforms, sizeof(unitUniforms), kUniformBufferIndex);
            encoder->setFragmentTexture(shadowMap_, kShadowTextureIndex);
            encoder->setFragmentSamplerState(shadowSampler_, kShadowSamplerIndex);

            encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
            // This frame's ring slot: an offset, not a rebind. UnitInstance is
            // 40 bytes, so a slot's stride is a multiple of 4 and satisfies
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

    // --- Water -------------------------------------------------------------
    // Last, so it blends over whatever terrain and units sit below y = 0. Only
    // worth drawing when something actually is below it.
    if (waterIndexCount_ > 0 && hasWater_ && terrainMinY_ < waterLevel_
        && (override == nullptr || !override->skipWater)) {
        encoder->setRenderPipelineState(waterPipeline_);
        encoder->setDepthStencilState(waterDepthState_);
        encoder->setFragmentTexture(reflectionColour_, kReflectionTextureIndex);
        encoder->setFragmentSamplerState(reflectionSampler_, kReflectionSamplerIndex);
        encoder->setVertexBuffer(waterVertexBuffer_, 0, kVertexBufferIndex);
        encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->setFragmentBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);
        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(waterIndexCount_),
                                       MTL::IndexType::IndexTypeUInt32, waterIndexBuffer_,
                                       NS::UInteger{0});
    }
}

template <typename Predicate>
void Renderer::drawTerrainChunks(MTL::RenderCommandEncoder* encoder, Predicate keep) noexcept {
    std::size_t runFirst = 0;
    std::size_t runCount = 0;

    const auto flush = [&] {
        if (runCount == 0) {
            return;
        }
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle, static_cast<NS::UInteger>(runCount),
            MTL::IndexType::IndexTypeUInt32, indexBuffer_,
            static_cast<NS::UInteger>(runFirst * sizeof(std::uint32_t)));
        runCount = 0;
    };

    for (const TerrainChunk& chunk : terrainChunks_) {
        if (!keep(chunk)) {
            flush();
            continue;
        }
        if (runCount == 0) {
            runFirst = chunk.firstIndex;
        }
        runCount += chunk.indexCount;
    }
    flush();
}

void Renderer::updateLightMatrix() noexcept {
    hasShadows_ = false;
    if (shadowMap_ == nullptr || indexCount_ == 0) {
        return;
    }

    // The light's box follows the CAMERA, not the map.
    //
    // Covering the whole map sounds tidier and is much worse: on an 8192-elmo
    // map a 2048-texel shadow map is 4 elmos per texel, and the depth range
    // spans the map's diagonal — so the bias needed to stop the ground
    // shadowing itself is several elmos, which is most of a tank's height. The
    // shadow then lifts clean off its caster and nothing appears at all.
    //
    // Sized to what the camera can actually see, the same map is fractions of
    // an elmo per texel and the bias costs nothing visible.
    const float extent = std::clamp(camera_.distance, 100.0f, 6000.0f);
    const simd_float3 centre = camera_.target;

    // Stand the light off far enough to catch anything that could cast into
    // view, and give the box enough depth to hold it.
    const float depthRange = extent * 8.0f;
    const simd_float3 eye = centre + kSunDirection * extent * 4.0f;

    const simd_float3 forward = simd_normalize(centre - eye);
    const simd_float3 reference =
        std::abs(forward.y) > 0.99f ? simd_make_float3(0, 0, 1) : simd_make_float3(0, 1, 0);
    const simd_float3 right = simd_normalize(simd_cross(reference, forward));
    const simd_float3 up = simd_cross(forward, right);

    const simd_float4x4 view = simd_matrix(
        simd_make_float4(right.x, up.x, forward.x, 0.0f),
        simd_make_float4(right.y, up.y, forward.y, 0.0f),
        simd_make_float4(right.z, up.z, forward.z, 0.0f),
        simd_make_float4(-simd_dot(right, eye), -simd_dot(up, eye), -simd_dot(forward, eye),
                         1.0f));

    // Orthographic, with Metal's [0, 1] depth range — the same convention the
    // camera's perspective matrix uses, and for the same reason.
    const simd_float4x4 projection = simd_matrix(
        simd_make_float4(1.0f / extent, 0.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, 1.0f / extent, 0.0f, 0.0f),
        simd_make_float4(0.0f, 0.0f, 1.0f / depthRange, 0.0f),
        simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f));

    lightViewProjection_ = simd_mul(projection, view);
    lightCentre_ = centre;
    lightExtent_ = extent;
    hasShadows_ = true;
}

void Renderer::encodeReflectionPass(MTL::CommandBuffer* commandBuffer) noexcept {
    if (reflectionColour_ == nullptr || commandBuffer == nullptr || indexCount_ == 0
        || !hasWater_ || terrainMinY_ >= waterLevel_) {
        return;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* colour = pass->colorAttachments()->object(0);
    colour->setTexture(reflectionColour_);
    colour->setLoadAction(MTL::LoadAction::LoadActionClear);
    colour->setStoreAction(MTL::StoreAction::StoreActionStore);
    // Cleared to ZERO alpha, which is how the water tells "the mirror saw
    // something here" from "the mirror saw nothing". Clearing to the sky
    // colour with alpha 1 makes every texel look like geometry, and the water
    // then reflects the clear colour everywhere the world is not.
    colour->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));

    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(reflectionDepth_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionDontCare);
    depth->setClearDepth(1.0);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

    // The view matrix, composed with a reflection in the water plane.
    //
    // NOT a mirrored OrbitCamera: that camera derives its eye from a pitch it
    // clamps to a positive band, and a reflected camera looks UP from below the
    // surface — a negative pitch it cannot express at all. Reflecting the
    // matrix sidesteps the parameterisation entirely.
    //
    //   y' = 2 * waterLevel - y
    //
    // as a matrix, applied to the world before the ordinary view transform.
    const simd_float4x4 reflect = simd_matrix(
        simd_make_float4(1.0f, 0.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, -1.0f, 0.0f, 0.0f),
        simd_make_float4(0.0f, 0.0f, 1.0f, 0.0f),
        simd_make_float4(0.0f, 2.0f * waterLevel_, 0.0f, 1.0f));

    const float aspect =
        static_cast<float>(kReflectionWidth) / static_cast<float>(kReflectionHeight);

    SceneOverride override;
    override.viewProjection =
        simd_mul(camera_.projectionMatrix(aspect), simd_mul(camera_.viewMatrix(), reflect));
    // Anything under the surface is not in the mirror. A little above it, so
    // the shoreline itself does not shimmer in and out along the seam.
    override.clipBelowY = waterLevel_ - 1.0f;
    override.skipWater = true;

    encodeScene(encoder, kReflectionWidth, kReflectionHeight, &override);

    encoder->endEncoding();
    pass->release();
}

void Renderer::encodeShadowPass(MTL::CommandBuffer* commandBuffer) noexcept {
    // The box follows the camera, so it is rebuilt every frame — a handful of
    // matrix operations against a pass that draws the whole scene.
    updateLightMatrix();
    if (!hasShadows_ || shadowMap_ == nullptr || commandBuffer == nullptr) {
        return;
    }

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassDepthAttachmentDescriptor* depth = pass->depthAttachment();
    depth->setTexture(shadowMap_);
    depth->setLoadAction(MTL::LoadAction::LoadActionClear);
    depth->setStoreAction(MTL::StoreAction::StoreActionStore);  // the fragment shaders read it
    depth->setClearDepth(1.0);

    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

    // Only the light matrix is read by the shadow vertex shaders, but the whole
    // struct goes up: one layout, one place to get it wrong.
    TerrainUniforms uniforms{};
    uniforms.lightViewProjection = lightViewProjection_;
    uniforms.hasShadows = 1.0f;

    // No encoder depth bias: setDepthBias works in units of the depth format's
    // smallest resolvable step, which for a 32-bit float over a map-sized
    // orthographic range is not a quantity worth guessing at. The bias is
    // applied in the shader instead, in clip-space units this code chooses.
    encoder->setDepthStencilState(depthState_);
    encoder->setCullMode(MTL::CullMode::CullModeNone);
    encoder->setVertexBytes(&uniforms, sizeof(uniforms), kUniformBufferIndex);

    if (indexCount_ > 0) {
        encoder->setRenderPipelineState(terrainShadowPipeline_);
        encoder->setVertexBuffer(vertexBuffer_, 0, kVertexBufferIndex);

        // Only the chunks the light's box touches. The box follows the camera
        // and covers a fraction of the map, so on a large map this is a handful
        // of chunks against sixteen by sixteen — and it was the whole reason
        // the shadow pass cost as much as it did.
        //
        // Tested in the light's own axes as a sphere against the box: the box
        // is oriented along the sun, so an axis-aligned test in world space
        // would be wrong, and a chunk's bounding sphere is cheap and never too
        // small. Too generous is harmless here; too tight drops shadows.
        // Only the chunks the light's box touches. The box follows the camera
        // and covers a fraction of the map, so this is usually a handful of
        // chunks against sixteen by sixteen — and it is why the shadow pass no
        // longer costs what it did.
        //
        // Tested as a sphere against the box in the LIGHT's axes: the box is
        // oriented along the sun, so an axis-aligned world-space test would be
        // wrong. A chunk's bounding sphere is cheap and never too small, and
        // here too generous is harmless while too tight drops shadows.
        drawTerrainChunks(encoder, [this](const TerrainChunk& chunk) {
            const simd_float3 centre = simd_make_float3((chunk.minX + chunk.maxX) * 0.5f,
                                                        (chunk.minY + chunk.maxY) * 0.5f,
                                                        (chunk.minZ + chunk.maxZ) * 0.5f);
            const float radius = 0.5f * simd_length(simd_make_float3(chunk.maxX - chunk.minX,
                                                                     chunk.maxY - chunk.minY,
                                                                     chunk.maxZ - chunk.minZ));
            const simd_float3 offset = centre - lightCentre_;
            const simd_float3 along = kSunDirection * simd_dot(offset, kSunDirection);
            return simd_length(offset - along) <= lightExtent_ + radius;
        });
    }

    encoder->setRenderPipelineState(unitShadowPipeline_);
    for (const GpuUnitBatch& batch : unitBatches_) {
        if (batch.instanceCount == 0) {
            continue;
        }

        encoder->setVertexBuffer(batch.vertexBuffer, 0, kVertexBufferIndex);
        encoder->setVertexBuffer(
            batch.instanceBuffer,
            static_cast<NS::UInteger>(instanceSlot_ * batch.instanceCapacity
                                      * sizeof(UnitInstance)),
            kInstanceBufferIndex);
        encoder->setVertexBuffer(batch.boneBuffer, 0, kBoneBufferIndex);

        PoseUniforms pose;
        pose.poseCount = static_cast<std::uint32_t>(batch.poseCount);
        pose.boneCount =
            static_cast<std::uint32_t>(batch.boneStrideBytes / sizeof(BoneTransform));
        pose.duration = batch.duration;
        pose.time = batch.animationDrivenByInstance ? 0.0f : animationTime_;
        encoder->setVertexBytes(&pose, sizeof(pose), kPoseUniformBufferIndex);

        encoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(batch.indexCount),
                                       MTL::IndexType::IndexTypeUInt32, batch.indexBuffer,
                                       /*indexBufferOffset=*/0,
                                       static_cast<NS::UInteger>(batch.instanceCount));
    }

    encoder->endEncoding();
    pass->release();
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

        // The encoder exists (and must end) even with zero draw calls: a
        // render pass with LoadActionClear is what performs the clear.
        MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(pass);

        encodeScene(encoder, width, height);

        encoder->endEncoding();

        // Releases the ring slot this frame read its instances from. Metal runs
        // this on its own thread; std::counting_semaphore is the synchronisation
        // primitive, so no further locking is needed.
        if (opened) {
            commandBuffer->addCompletedHandler(
                MTL::HandlerFunction{[this](MTL::CommandBuffer*) { framesInFlight_.release(); }});
        }

        if (recording_ && cpuMs > 0.0) {
            // GPUEndTime - GPUStartTime is the driver's own measurement of the
            // work, so it is unaffected by vsync pacing. That makes it the
            // meaningful number here: the CPU figure is pinned to the display
            // refresh by CAMetalDisplayLink and says nothing about how fast the
            // renderer could go.
            //
            // The handler runs on a Metal-owned thread, hence the mutex. It is
            // taken once per frame, not per draw.
            // Explicit HandlerFunction: a bare lambda is ambiguous between the
            // std::function and ObjC-block overloads.
            commandBuffer->addCompletedHandler(MTL::HandlerFunction{
                [this, cpuMs](MTL::CommandBuffer* completed) {
                    const double gpuMs =
                        (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
                    const std::lock_guard lock{benchMutex_};
                    recorder_.add(bench::FrameSample{cpuMs, gpuMs});
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
