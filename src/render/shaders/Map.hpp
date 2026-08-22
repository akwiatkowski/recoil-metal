#pragma once

// The water pass - a plane at the map's own elevation, refracted and reflected.
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

inline constexpr const char* kMap = R"MSL(
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

// How far a unit of wave slope moves the refracted sample, in screen widths.
//
// The map states a `refractionScale` (0.015 for most, and water2.fx's own
// default) but that is in the engine's own units against its own normal maps, and
// this water builds its normal from two analytic wave trains instead. So one
// constant of ours bridges the two, chosen by eye against a shoreline: at 0.6 the
// bend is invisible, at 6 the sea looks like frosted glass.
constant float kRefractionUv = 12.0;

// The depth over which the refraction fades in, in elmos.
//
// Not a taste: at the waterline there is nothing correct to bend towards, so any
// offset there samples dry ground or sky and paints it in the water as a bright
// fringe along the coast. 40 elmos is five heightmap squares, which is about the
// width the fringe would otherwise be visible over.
constant float kRefractionFadeDepth = 8.0;

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
                              texture2d<float> sceneColour [[texture(15)]],
                              texture2d<float> waveNormals [[texture(16)]],
                              texture2d<float> fogMask [[texture(24)]],
                              sampler reflectionSampler [[sampler(3)]]) {
    // Tiling, unlike the screen-space samplers above: wave UVs are world-space and repeat.
    constexpr sampler waveSampler(address::repeat, filter::linear, mip_filter::linear);
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

    // What is seen through the water: the real scene behind it, REFRACTED, then
    // absorbed with depth.
    //
    // Absorption first, because it decides how much refraction there is. Water
    // removes light exponentially with the distance travelled through it and the
    // red end goes first — Beer-Lambert rather than a lerp to a colour, which is
    // why a shallow sandy bottom stays sandy and a deep one goes blue without
    // either being painted that way. The path length is doubled: light goes down
    // to the bottom and back up.
    const float3 absorption = float3(0.030, 0.012, 0.008);
    const float3 attenuation = exp(-absorption * in.depth * 2.0);

    // The refraction is the engine's, and it needs a copy of the colour target:
    // `behind` is a framebuffer fetch, which reads this pixel and no other, so
    // with only that the water can absorb what is under it but not bend it. Where
    // a copy is bound the sample moves with the wave normal, which is what makes
    // a submerged shelf waver instead of lying flat under glass.
    //
    // The offset is weighted by HOW MUCH BOTTOM STILL SHOWS, which is the same
    // attenuation the colour gets. That is both the physics — there is nothing to
    // bend once the water has swallowed the view — and the fix for the artefact
    // this feature arrives with: the wave normal here is two analytic trains
    // standing in for the engine's four scrolling normal maps, and offsetting a
    // sample by a field that regular draws its lattice across open water as a
    // grid of dark rings. Refraction belongs in the shallows, where there is a
    // bottom to see and the pattern has real relief to disturb.
    //
    // It also fades in over the first few elmos: at the waterline itself any
    // offset samples dry ground or sky and paints it inside the water, which
    // reads as a bright fringe following the coast.
    //
    // The scale is the map's own `refractionScale` (water2.fx), times one constant
    // of ours to reach screen space — the wave slope is a gradient, and this water
    // builds it analytically rather than from the engine's textures, so no factor
    // the engine states would carry over.
    float3 refracted = behind.rgb;
    if (u.hasSceneColour > 0.5) {
        // Its own, FINER wave field, not the one that lights the surface.
        //
        // The two trains above are 300-elmo swells — the right scale for shading,
        // since that is the scale a sea's shape reads at. Bending a screen-space
        // sample by them draws their lattice over the water as a chain of rings
        // tens of pixels across, which is the artefact this feature arrives with
        // and the reason it is a setting. Real refraction is driven by RIPPLES:
        // the wavelength that matters is comparable to the depth of water being
        // seen through, not to the swell crossing it.
        //
        // Eight times the frequency, so a ~37-elmo ripple, and at incommensurable
        // angles to each other as the swells are — the lattice does not go away,
        // it goes small enough to read as disturbed water rather than as a grid.
        float2 ripple;
        if (u.hasWaterWaves > 0.5) {
            // THE MAP'S OWN RIPPLE: the engine's wave-normal texture, sampled twice with
            // the water block's own repeats and scroll vectors (water2.fx's exact recipe).
            // The analytic trains below stay as the stand-in for a map that names none.
            const float2 uvA = in.world.xz * u.waveRepeats.x + u.waveMovements.xy * t * 0.02;
            const float2 uvB = in.world.xz * u.waveRepeats.y + u.waveMovements.zw * t * 0.02;
            const float2 nA = waveNormals.sample(waveSampler, uvA).rg * 2.0 - 1.0;
            const float2 nB = waveNormals.sample(waveSampler, uvB).rg * 2.0 - 1.0;
            ripple = (nA + nB) * 0.5 * 0.3;
        } else {
            const float2 third = float2(0.71f, 0.70f);
            const float2 fourth = float2(-0.68f, 0.73f);
            const float ripple1 = dot(in.world.xz, third) * 0.168 + t * 2.3;
            const float ripple2 = dot(in.world.xz, fourth) * 0.139 - t * 1.9;
            ripple = third * cos(ripple1) * 0.168 + fourth * cos(ripple2) * 0.139;
        }

        const float visible = max(attenuation.r, max(attenuation.g, attenuation.b));
        const float2 bend = ripple * u.waterRefractionScale * kRefractionUv * visible
                          * saturate(in.depth / kRefractionFadeDepth);
        // Clamped rather than wrapped: a sample that runs off the edge of the
        // screen should hold the edge pixel, not fetch the opposite side of the
        // frame, which is a bright smear along whichever border the waves lean
        // towards.
        const float2 screen = in.position.xy / float2(u.viewportSize);
        refracted = sceneColour.sample(reflectionSampler,
                                       clamp(screen + bend, float2(0.0), float2(1.0))).rgb;
    }

    const float3 transmitted = refracted * attenuation;

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
    //
    // With the planar pass switched off the texture still exists but holds
    // whatever was last rendered into it, so its coverage must be forced to
    // zero here rather than trusted — otherwise disabling reflections freezes
    // a stale mirror on the water instead of falling back to the sky.
    float4 planar = float4(0.0);
    if (u.hasReflection > 0.5) {
        const float2 screen = in.position.xy / float2(u.viewportSize);
        // A few pixels of wobble, not a few hundred. The slope is a gradient,
        // so scaling it into UV space needs a small number: 0.35 shifts the
        // sample by a third of the screen and the reflection dapples with
        // whatever happens to be there.
        const float2 reflectionUv = saturate(screen + slope * 0.012);
        planar = reflection.sample(reflectionSampler, reflectionUv);
    }

    const float3 sky = skyColour(mirrored, u.sunDirection, u.fogColour, u.skyZenithTint);
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
    float3 surface = mix(behind.rgb, colour, shoreline);

    // The same fog the ground gets. Without it the sea stays at full brightness inside the
    // fog and the coastline reads as a hard edge between two different times of day —
    // which looks like a bug in the water rather than like fog of war.
    if (u.hasFog > 0.5) {
        const float2 fogUv = float2(in.world.x / u.fogWidthElmos, in.world.z / u.fogDepthElmos);
        const float seen = fogMask.sample(reflectionSampler, fogUv).r;
        surface *= mix(kUnseenGround, 1.0, seen);
    }

    return float4(surface, 1.0);
}

)MSL";

} // namespace rm::shaders
