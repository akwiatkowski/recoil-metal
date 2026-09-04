#pragma once

// Shared prologue, uniform layouts, and the terrain pass.
//
// PART OF ONE MSL TRANSLATION UNIT. `Renderer` concatenates these in a fixed order and hands the
// result to `newLibrary` - Metal compiles the shaders from source at runtime (ADR-002), so there
// is one library and the pieces are strings. **The order is not cosmetic**: a helper defined in
// `Common` is called by every file after it, and MSL has no forward declarations here.
//
// SPLIT OUT OF `Renderer.mm` for PLAN2.md §7 P7.3, which asks for the file to become
// `MapRenderer`, `UnitRenderer`, `FxRenderer` and `UiRenderer` - Recoil's own split
// (`CBaseGroundDrawer`, `CUnitDrawer`, `CProjectileDrawer`, `CMiniMap`). The shaders are the
// half of that split with no risk attached: they are string data, and concatenating them in the
// same order produces the same library byte for byte. 1,291 of `Renderer.mm`'s 4,267 lines were
// this one literal.

namespace rm::shaders {

inline constexpr const char* kCommon = R"MSL(
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
    float hasReflection;
    float hasSceneColour;
    float waterRefractionScale;
    float alphaIsOpacity;
    float3 skyZenithTint;
    float3 sunColour;
    float3 sunAmbience;
    float3 shadowFill;
    float lightingMultiplier;
    float hasFog;
    float fogWidthElmos;
    float fogDepthElmos;
    float4 waveRepeats;
    float4 waveMovements;
    float hasWaterWaves;
    float hasUnitNormals;
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
static float3 skyColour(float3 direction, float3 sunDirection, float3 horizonColour,
                        float3 zenithTint) {
    const float3 d = normalize(direction);

    // Elevation drives the gradient. The power biases the blend toward the
    // horizon, where a real sky spends most of its visible area.
    const float elevation = saturate(d.y);
    // The zenith takes the map's own cirrus colour as a tint rather than a
    // replacement: the skybox block carries no zenith colour at all — its mid colour
    // is three bytes of black on all 60 stock maps — and the cirrus colour is the one
    // thing in it that both varies per map and is a colour of the sky. A neutral map
    // keeps exactly the look this had before; a green-brown one stops sharing a
    // blue-grey with everything else.
    float3 colour = mix(horizonColour, kZenithColour * zenithTint, pow(elevation, 0.55));

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
    return float4(skyColour(in.direction, u.sunDirection, u.fogColour, u.skyZenithTint), 1.0);
}

// How wide a selection outline is drawn, in pixels.
//
// Constant in PIXELS rather than elmos: a selection cue should be the same weight
// on a unit across the map as on one under the cursor. Three is enough to find in a
// crowd at a working zoom and thin enough not to fatten the silhouette.
constant float kOutlinePixels = 3.0;

// The rings' own green (main.mm's kSelectionRingColour), so the two cues agree.
constant float3 kOutlineColour = float3(0.35, 1.0, 0.45);

// How much of the sun a shadowed fragment keeps. Not zero: shadows in daylight
// are lit by the sky, and a black shadow reads as a hole in the ground rather
// than as shade.
constant float kShadowedLight = 0.35;

// How much light unseen ground keeps. Not zero — see the fog block in terrainFragment.
constant float kUnseenGround = 0.42;

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
    float normalTileElmos[9];  ///< the normal maps repeat on their own scale
    float normalPresent[9];
    float enabled;
    float normalStrength;
};

// The blended stratum normal, in the surface's tangent frame.
//
// Ported from terrain.fx's TerrainNormalsXP: the same chain of lerps as the
// albedo, in the same order, over `sample * 2 - 1`.
//
// One asymmetry is faithfully reproduced and is NOT a typo. The albedo pass
// reads its masks EXPANDED — `saturate(mask * 2 - 1)`, which is what ADR-009
// corrected — while the normals pass reads them RAW. Both are in the same file,
// twenty lines apart. Expanding the masks here as well would look tidier and
// would make every stratum's relief cut off at half weight rather than fading
// in across the whole range.
//
// Blue is the up axis, measured across the real textures rather than inferred
// from a shader that never says so (tests/test_real_stratum_normals.cpp) — a
// normal map read on the wrong axis lights bumps as dents, which survives a
// look at the screen.
static float3 blendedStratumNormal(float2 world, float4 mask0, float4 mask1,
                                   constant SplatUniforms& s,
                                   array<texture2d<float>, 9> normals,
                                   sampler splatSampler) {
    const float weights[8] = {mask0.r, mask0.g, mask0.b, mask0.a,
                              mask1.r, mask1.g, mask1.b, mask1.a};

    // Flat — straight up in tangent space — is the honest starting point for a
    // base layer that ships no normal map.
    float3 blended = float3(0.0, 0.0, 1.0);
    if (s.normalPresent[0] > 0.5) {
        blended = normals[0].sample(splatSampler, world / s.normalTileElmos[0]).xyz * 2.0 - 1.0;
    }

    for (uint i = 0; i < 8; ++i) {
        // Two skips, both of which leave the result bit-identical — mixing by
        // zero is what the arithmetic would have done anyway — and which
        // between them halve the cost on a real map.
        //
        // The first is uniform across the draw: a map naming five strata has
        // four slots bound to a fallback texture whose contents are then
        // multiplied away, and sampling those is pure waste. The second is per
        // fragment but spatially coherent, since a stratum covers regions
        // rather than speckle: away from its region its mask weight is zero.
        if (s.normalPresent[i + 1] < 0.5 || weights[i] <= 0.0) {
            continue;
        }

        const float3 layer =
            normals[i + 1].sample(splatSampler, world / s.normalTileElmos[i + 1]).xyz * 2.0 - 1.0;
        blended = mix(blended, layer, weights[i]);
    }

    return normalize(blended);
}

fragment float4 terrainFragment(VertexOut in [[stage_in]],
                                constant Uniforms& u [[buffer(1)]],
                                constant SplatUniforms& s [[buffer(2)]],
                                texture2d<float> ground [[texture(0)]],
                                texture2d<float> maskA [[texture(1)]],
                                texture2d<float> maskB [[texture(2)]],
                                array<texture2d<float>, 10> layers [[texture(3)]],
                                depth2d<float> shadowMap [[texture(13)]],
                                array<texture2d<float>, 9> layerNormals [[texture(15)]],
                                texture2d<float> fogMask [[texture(24)]],
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
    float3 normal = normalize(in.normal);

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
        //
        // The RAW samples are kept as well, because the normals pass reads them
        // unexpanded — see blendedStratumNormal. Two readings of one texture,
        // twenty lines apart in the engine's own file.
        const float4 rawA = maskA.sample(groundSampler, in.uv);
        const float4 rawB = maskB.sample(groundSampler, in.uv);
        const float4 a = saturate(rawA * 2.0 - 1.0);
        const float4 b = saturate(rawB * 2.0 - 1.0);
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

        // The strata's own relief, over the heightfield's. A height sample is 8
        // elmos across, so every feature smaller than a tank — gravel, ripples
        // in sand, the grain of rock — has no geometry it could be expressed in
        // and has to arrive this way or not at all.
        //
        // Perturbing the geometric normal rather than replacing it, which is
        // what the engine effectively does: SupCom's terrain gets its slope
        // from a map-wide normal map, whereas this mesh already carries the
        // real slope in its vertices, and throwing that away to trust a tiled
        // texture would flatten every hillside.
        //
        // The tangent frame needs no basis vectors because the layer uv IS
        // world x and z — the parameterisation is axis-aligned by construction,
        // so tangent x maps to world x, tangent y to world z, and tangent z
        // (blue) to the surface normal.
        if (s.normalStrength > 0.0) {
            const float3 detail = blendedStratumNormal(world, rawA, rawB, s, layerNormals,
                                                       splatSampler);
            normal = normalize(normal + float3(detail.x, 0.0, detail.y) * s.normalStrength);
        }
    } else if (u.hasTexture > 0.5) {
        albedo = ground.sample(groundSampler, in.uv).rgb;
    } else {
        // No .smt: fall back to colouring by elevation so relief still reads.
        const float span = max(u.maxHeight - u.minHeight, 1.0);
        const float t = saturate((in.height - u.minHeight) / span);
        albedo = mix(float3(0.18, 0.34, 0.16), float3(0.66, 0.62, 0.55), t);
    }

    // Lambert against a fixed sun. Computed here rather than beside the normal's
    // declaration, because the splat branch above may have tilted that normal —
    // and lighting the ground by the slope it had BEFORE its stratum relief was
    // applied would compute the detail and then throw it away.
    const float lambert = saturate(dot(normal, u.sunDirection));

    // Shadow attenuates the SUN only. Ambience is the sky, which reaches into
    // shade — dimming it too would make shadowed ground black.
    const float sun = sunlightAt(in.world, lambert, u, shadowMap, shadowSampler);

    // THE MAP'S OWN LIGHT, verbatim from `terrain.fx:2279-2281` — the same three
    // lines every one of that file's terrain pixel shaders ends with (976, 1386,
    // 1779, 2279):
    //
    //     light = SunColor * saturate(dotSunNormal) * shadow + SunAmbience * ao;
    //     light = LightingMultiplier * light + ShadowFillColor * (1 - light);
    //     albedo.rgb = light * (albedo.rgb + specular.rgb);
    //
    // Two terms of that are dropped and it is worth saying which. There is no
    // ambient occlusion — it arrives from a terrain info texture the high-fidelity
    // shaders sample and we do not load, and the engine passes 1 without it. And
    // there is no terrain specular, which needs the map's specular colour and the
    // albedo's alpha as a gloss mask.
    //
    // ONE DELIBERATE DIFFERENCE, per the rule that they get stated at the
    // definition: our `sun` never reaches 0, because `sunlightAt` floors a shadowed
    // fragment at kShadowedLight. That floor exists for exactly the reason
    // ShadowFillColor does — a black shadow reads as a hole — so with the fill term
    // now carrying the map's own shade colour the two overlap, and our shadows come
    // out lighter than the engine's by the amount of the floor. Lowering the floor
    // is a change to the unit shaders as well, so it is not made here.
    const float3 light = u.sunColour * lambert * sun + u.sunAmbience;
    const float3 lit = u.lightingMultiplier * light + u.shadowFill * (1.0 - light);
    float3 colour = albedo * lit;

    // FOG OF WAR (ADR-037). Ground the viewer's side cannot see is darkened, not blacked
    // out: Supreme Commander and Recoil both keep unseen terrain readable, because a player
    // needs to know the shape of the ground they are about to walk into even when they
    // cannot see what is standing on it. What the fog hides is UNITS, and that is the
    // gather's job rather than this shader's.
    //
    // Sampled by world position over the whole map, with the mask's own filtering doing the
    // smoothing — a nearest-sampled grid at 16 elmos a square would draw the vision radius
    // as a staircase of squares, which reads as a rendering artefact rather than as a
    // horizon.
    if (u.hasFog > 0.5) {
        const float2 fogUv = float2(in.world.x / u.fogWidthElmos, in.world.z / u.fogDepthElmos);
        const float seen = fogMask.sample(groundSampler, fogUv).r;
        colour *= mix(kUnseenGround, 1.0, seen);
    }

    return float4(colour, 1.0);
}

)MSL";

} // namespace rm::shaders
