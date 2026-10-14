#pragma once

// Selection outlines - the pass that traces a silhouette around what is selected.
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

inline constexpr const char* kOutline = R"MSL(
// --- Selection outlines ------------------------------------------------------
// A shell around a selected unit, drawn from the same geometry pushed out along its
// own normals. What the rings cannot do: at a low camera angle a crowd's rings hide
// behind the units standing on them, which is the one thing removing the white tint
// genuinely lost (ADR-021).
//
// An inverted hull rather than a stencil or an id buffer, because both of those
// would change the shape of the frame — a stencil means a depth-stencil format on
// every pipeline and the depth texture, an id buffer means another attachment — and
// this needs neither. Front faces are culled so what shows is the shell's far side,
// which survives only where it sticks out past the unit.
//
// The width is applied in CLIP space, not world space: offsetting by a fixed number
// of elmos gives an outline that is bold on a near unit and invisible on a far one,
// where a selection cue wants to be the same weight wherever it is.

vertex float4 outlineVertex(uint vid [[vertex_id]],
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
    const float4 boneRotation = float4(bone.rotation);
    // The aim pose too: a silhouette traced around the REST pose would outline a
    // barrel that is not where it is drawn.
    const float3 local = applyBuilderAim(
        rotateBy(boneRotation, float3(v.position)) + float3(bone.translation), bone, inst, p);
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);
    const float3 worldNormal =
        unitOrient(applyBuilderAimNormal(rotateBy(boneRotation, float3(v.normal)), bone, inst, p),
                   inst);

    float4 clip = u.viewProjection * float4(world, 1.0);

    // The normal in clip space, as a direction: w = 0 drops the translation, which is
    // what makes this a direction rather than a point a few elmos away.
    const float3 clipNormal = (u.viewProjection * float4(worldNormal, 0.0)).xyz;
    const float2 screenNormal =
        length(clipNormal.xy) > 1e-6 ? normalize(clipNormal.xy) : float2(0.0);

    // Multiplying by w undoes the perspective divide that is about to happen, so the
    // offset lands as the same number of pixels at any depth. Two over the viewport
    // height converts pixels to normalised device coordinates.
    const float widthNdc = 2.0 * kOutlinePixels / max(u.viewportSize.y, 1.0);
    clip.xy += screenNormal * widthNdc * clip.w;
    return clip;
}

fragment float4 outlineFragment(constant Uniforms& u [[buffer(1)]]) {
    // The same green the rings use, so the two cues read as one thing said twice
    // rather than as two states.
    return float4(kOutlineColour, 1.0);
}

// A PROP's shadow, which needs a fragment shader where the others do not.
//
// The passes above write depth and nothing else, and Metal is happy to rasterise
// with no fragment function at all. A prop cannot: its shape is cut out of its quads
// by the albedo's alpha, so a depth-only pass records the untrimmed quad and a tree
// casts the shadow of the card its leaves are painted on — a scatter of hard
// rectangles, visibly worse than no tree shadow.
//
// The cost of a `discard` is losing early-depth rejection, and it is confined to
// this pipeline: the terrain and the units keep theirs, because that behaviour is a
// property of the pipeline rather than of the pass.
struct PropShadowOut {
    float4 position [[position]];
    float2 uv;
};

vertex PropShadowOut propShadowVertex(uint vid [[vertex_id]],
                                      uint iid [[instance_id]],
                                      const device UnitVertexIn* vertices [[buffer(0)]],
                                      constant Uniforms& u [[buffer(1)]],
                                      const device UnitInstanceIn* instances [[buffer(2)]],
                                      const device BoneTransformIn* bones [[buffer(3)]],
                                      constant PoseUniforms& p [[buffer(4)]]) {
    const UnitVertexIn v = vertices[vid];
    const UnitInstanceIn inst = instances[iid];

    const BoneTransformIn bone = bones[v.boneIndex];  // props never animate
    const float3 local =
        rotateBy(float4(bone.rotation), float3(v.position)) + float3(bone.translation);
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);

    PropShadowOut out;
    out.position = u.lightViewProjection * float4(world, 1.0);
    // The same V flip the visible pass applies, and it has to match: a shadow cut
    // from the mirror image of a leaf is a different leaf.
    out.uv = float2(v.uv.x, 1.0 - v.uv.y);
    return out;
}

// Returns nothing — the depth attachment is still the whole output. All this exists
// to do is refuse the fragments the leaf is not made of.
fragment void propShadowFragment(PropShadowOut in [[stage_in]],
                                 texture2d<float> diffuse [[texture(0)]],
                                 sampler texSampler [[sampler(0)]]) {
    if (diffuse.sample(texSampler, in.uv).a < 0.5) {
        discard_fragment();
    }
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
// The build ghost: a model drawn as the interface's own light rather than as metal.
//
// No textures and no shading model, deliberately — the ghost answers "what would stand
// here", and identity lives in the SILHOUETTE, not in paint. A flat tint would render the
// model as one shapeless slab, so the surface normal contributes just enough modulation
// that a roof reads over a wall; the fraction is small because the moment the interior
// competes with the outline, the ghost starts reading as a built thing.
//
// The tint is the whole vocabulary: the placeable cyan or the blocked red, the same two
// colours the ground ring has always spoken.
fragment float4 unitGhostFragment(UnitOut in [[stage_in]],
                                  constant float4& tint [[buffer(1)]]) {
    const float shape = 0.55 + 0.45 * saturate(in.normal.y * 0.5 + 0.5);
    return float4(tint.rgb * shape, tint.a);
}

// A BUILDING UNDER CONSTRUCTION, materialising the way its faction's engineers work.
//
// WHY THIS EXISTS AT ALL. Nothing stood at a build site until the work finished: a
// `Construction` (`core/sim/Economy.hpp`) is a row in the economy and nothing else, so
// placing a silhouette produced a mass drain, a console line, and an empty patch of ground
// for the next twenty seconds. The player's own report was "when clicking silhouette on
// ground I don't see command was building it", and they were right — there was nothing to see.
//
// THE FOUR EFFECTS are Supreme Commander's own, transcribed rather than invented. The game
// implements them in Lua per faction (`DefaultBuildBehaviors`), and what they have in common
// is a REVEAL FRACTION driven by build progress; what differs is the shape of the reveal and
// the colour of the energy doing it. That is exactly the split here: one uniform block, one
// pipeline, four branches.
//
//   UEF       a horizontal plane sweeps up the model, steel-blue, with a bright scan line at
//             the cut and a wireframe scaffold implied by the seam brightening on edges.
//   Cybran    no plane at all: a hashed dissolve reveals the model in scattered flecks, so it
//             assembles as a swarm rather than a casting. Red on near-black.
//   Aeon      the model rises out of a glowing pad and the UNBUILT part is still drawn, as
//             translucent green light rather than nothing — the shape is promised before it
//             is delivered.
//   Seraphim  a radial sweep in gold: the reveal runs around the model rather than up it,
//             which is what its contracting rings read as.
struct BuildUniforms {
    float4 tint;       // the faction's energy colour
    float progress;    // 0..1, from the construction's own remaining build time
    float baseY;       // world Y of the site: where the reveal starts
    float height;      // how tall the model is, in elmos
    float seconds;     // an animation clock, for the parts that shimmer
    float centreX;     // the site in world elmos — Seraphim's sweep turns about it
    float centreZ;
    uint style;        // 0 UEF, 1 Aeon, 2 Cybran, 3 Seraphim
    uint pad0;
};

/// A cheap stable hash of a world position, for Cybran's dissolve. Deterministic in space so
/// the pattern sticks to the MODEL rather than crawling as the camera moves — a dissolve that
/// swims is a screen-space effect wearing a world-space costume.
static float buildHash(float3 p) {
    return fract(sin(dot(floor(p * 1.7), float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

fragment float4 unitBuildFragment(UnitOut in [[stage_in]],
                                  constant Uniforms& u [[buffer(1)]],
                                  constant BuildUniforms& b [[buffer(5)]],
                                  depth2d<float> shadowMap [[texture(13)]],
                                  sampler shadowSampler [[sampler(2)]]) {
    // How far up this fragment sits within the model's own height, 0 at the ground and 1 at
    // the roof. Guarded because a model with no stated height would divide by zero and take
    // the whole building with it.
    const float span = max(b.height, 0.001);
    const float up = saturate((in.world.y - b.baseY) / span);

    // THE MAP'S OWN LIGHT, same as the finished building will wear — a construction is a
    // real object standing on real ground, so it sits in the map's sun and casts into the
    // map's shade like everything around it. This used to be a fake hemisphere term from
    // the normal's Y (the ghost's shading), which is why a site glowed evenly at dusk and
    // sat flat grey at noon: it was lit by nothing in the scene. The energy layers below
    // — the working edge, the pulse, Aeon's promise — ride on top of the lit metal, since
    // they ARE light and answer to no sun.
    const float3 N = normalize(in.normal);
    const float rawNdotL = saturate(dot(N, u.sunDirection));
    const float sunlight = sunlightAt(in.world, rawNdotL, u, shadowMap, shadowSampler);
    float3 light = u.sunColour * (rawNdotL * sunlight) + u.sunAmbience;
    light = u.lightingMultiplier * light + u.shadowFill * (1.0 - light);

    float revealed = 0.0;   // 1 where the structure exists, 0 where it does not yet
    float edge = 0.0;       // 1 at the working face, where the energy is going in

    if (b.style == 2u) {
        // CYBRAN: a hashed threshold. Every fragment gets its own cut-off, so the model comes
        // in as flecks that thicken rather than as a rising slab. The hash is biased by height
        // so it still fills bottom-up on average — a swarm builds a foundation first too.
        const float threshold = buildHash(in.world) * 0.75 + up * 0.25;
        revealed = threshold < b.progress ? 1.0 : 0.0;
        // A NARROW hot band, and narrower than the others on purpose: a uniform hash puts a
        // great many fragments just under the threshold at once, so a band the width of UEF's
        // turns the whole site white instead of picking out its working edge.
        edge = 1.0 - saturate((b.progress - threshold) * 60.0);
    } else if (b.style == 3u) {
        // SERAPHIM: the sweep runs AROUND the site rather than up it — a rotating wedge
        // closing on itself, which is what its contracting rings read as.
        //
        // ABOUT THE SITE'S OWN AXIS, from world position. Taking the angle from the surface
        // NORMAL instead was the first attempt and it is a different effect entirely: every
        // flat face shares one normal, so a building arrived as whole facets popping in rather
        // than as anything sweeping.
        const float angle = atan2(in.world.z - b.centreZ, in.world.x - b.centreX)
                          / (2.0 * 3.14159265) + 0.5;
        const float threshold = angle * 0.7 + up * 0.3;
        revealed = threshold < b.progress ? 1.0 : 0.0;
        edge = 1.0 - saturate((b.progress - threshold) * 25.0);
    } else {
        // UEF AND AEON: a plane sweeping up the model. The difference is what happens ABOVE
        // it, handled below — UEF discards, Aeon glows.
        revealed = up < b.progress ? 1.0 : 0.0;
        edge = 1.0 - saturate((b.progress - up) * 30.0);
    }

    if (revealed < 0.5) {
        // AEON PROMISES THE SHAPE, the other three do not. Drawing the unbuilt part as
        // translucent light is the one place the four genuinely differ in what is on screen
        // rather than in how it arrives.
        if (b.style == 1u) {
            const float shimmer = 0.35 + 0.15 * sin(b.seconds * 3.0 + in.world.y * 0.25);
            return float4(b.tint.rgb * shimmer, 0.22);
        }
        discard_fragment();
    }

    // The working face, blown out toward white — this is the energy going in, and it is the
    // one part of the effect that reads from across the map.
    const float3 metal = float3(0.42, 0.44, 0.47) * light;
    const float3 built = mix(metal, b.tint.rgb, 0.35);
    const float3 hot = mix(built, float3(1.0), saturate(edge) * 0.85);

    // A faint pulse over the whole thing while it is unfinished, so a site that is PAUSED for
    // want of mass still reads as a site rather than as a finished building of the wrong
    // colour. It stops at completion because `progress` reaches one and the term vanishes.
    const float pulse = 1.0 + 0.06 * sin(b.seconds * 6.0) * (1.0 - b.progress);
    return float4(hot * pulse, 1.0);
}

static float3 derivativeMappedNormal(float3 geometricNormal, float3 world, float2 uv,
                                     float2 packed) {
    const float z = sqrt(saturate(1.0 - dot(packed, packed)));
    const float3 tangentNormal = float3(packed, z);
    float3 shadingNormal = normalize(geometricNormal);

    const float3 dpdx = dfdx(world);
    const float3 dpdy = dfdy(world);
    const float2 dudx = dfdx(uv);
    const float2 dudy = dfdy(uv);
    const float determinant = dudx.x * dudy.y - dudy.x * dudx.y;
    if (abs(determinant) > 1e-12) {
        const float3 tangent = normalize((dpdx * dudy.y - dpdy * dudx.y) / determinant);
        const float3 T = normalize(tangent - shadingNormal * dot(shadingNormal, tangent));
        const float3 B = cross(shadingNormal, T);
        shadingNormal = normalize(T * tangentNormal.x + B * tangentNormal.y
                                  + shadingNormal * tangentNormal.z);
    }
    return shadingNormal;
}

fragment float4 unitFragment(UnitOut in [[stage_in]],
                             constant Uniforms& u [[buffer(1)]],
                             texture2d<float> diffuse [[texture(0)]],
                             texture2d<float> shading [[texture(1)]],
                             texture2d<float> normals [[texture(2)]],
                             texturecube<float> environment [[texture(3)]],
                             texture2d<float> unitLut [[texture(4)]],
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

    // A PROP's normal map, when it has one. The shading slot carries it rather than
    // a unit's channel set — a prop blueprint never names one of those — so
    // hasTexture2 means "there is a normal map here" on this path and "there is a
    // shading texture here" on the unit path, which is why alphaIsOpacity gates it.
    //
    // TWO CHANNELS, and this is the measured part. All 221 prop normal maps in the
    // shipped content are BC3 with red, green and blue equal: one axis replicated
    // across the colour channels, a second in alpha, and the third reconstructed
    // from them. BC3's alpha block is a better encoder than its RGB565 colour block,
    // so an axis kept there survives compression. Read as a stratum map — z in blue
    // (ADR-020) — a prop would be lit from a direction nobody chose.
    //
    // The BASIS comes from screen-space derivatives rather than per-vertex tangent
    // and binormal vectors. Unit normals reuse the same frame below; only their second
    // UV pair is retained, keeping ModelVertex at 44 bytes rather than 68.
    float3 shadingNormal = normalize(in.normal);
    if (u.alphaIsOpacity > 0.5 && u.hasTexture2 > 0.5) {
        const float2 packed = float2(tex2.a, tex2.g) * 2.0 - 1.0;
        shadingNormal = derivativeMappedNormal(in.normal, in.world, in.uv, packed);
    } else if (u.supremeCommanderShading > 0.5 && u.hasUnitNormals > 0.5) {
        // Retail mesh.fx:594 says `.gaa`: X is GREEN, Y is ALPHA, then Z is
        // reconstructed. The former float2(a,g) reading reversed those authored axes.
        const float4 normalSample = normals.sample(texSampler, in.uv2);
        const float2 packed = normalSample.ga * 2.0 - 1.0;
        shadingNormal = derivativeMappedNormal(in.normal, in.world, in.uv2, packed);
    }

    // A PROP's alpha is opacity, not a mask. Trees and bushes are quads with the
    // shape of a leaf cut out of them, so the cutout has to happen before
    // anything else reads that channel — and it is a `discard` rather than a
    // blend because these are scenery drawn in arbitrary order, and alpha
    // blending without sorting puts a near frond behind a far one. A cutout is
    // order-independent, which is what makes 4000 trees a single instanced draw.
    //
    // Threshold rather than 0: the fringe of a DXT-compressed alpha channel is
    // noisy, and keeping everything above zero leaves a halo of interpolated
    // background around every leaf.
    if (u.alphaIsOpacity > 0.5 && tex1.a < 0.5) {
        discard_fragment();
    }

    // Where the team-colour mask lives, and what the shading texture's channels
    // mean, differ between the two content families — see Family in Model.hpp.
    // Each branch below is a port of that family's OWN shader, so neither is
    // inferred from what the data looks like. A prop is a third case: its alpha
    // was just spent on the cutout, so it has no mask at all and keeps its own
    // colours.
    const bool supCom = u.supremeCommanderShading > 0.5;
    const float teamMask =
        u.alphaIsOpacity > 0.5 ? 0.0 : (supCom ? tex2.a : tex1.a);

    const float3 albedo = mix(tex1.rgb, in.teamColour.rgb, teamMask);

    const float3 N = shadingNormal;
    const float3 L = u.sunDirection;
    const float3 V = normalize(u.cameraPosition - in.world);

    const float rawNdotL = saturate(dot(N, L));
    const float sun = sunlightAt(in.world, rawNdotL, u, shadowMap, shadowSampler);
    const float NdotL = rawNdotL * sun;

    // THE MAP'S OWN LIGHT — the same two lines the terrain shader ends with,
    // because they are the same two lines in the game: `mesh.fx`'s ComputeLight
    // (mesh.fx:565-578) is `terrain.fx:2279-2280` with the variables renamed.
    // The engine feeds one lighting block to ground and models alike, and its
    // own comment says why: "This way all dark areas match (very cool.)" — a
    // shadowed hull and the shadowed ground it stands on converge on the same
    // fill colour, and a map's LightingMultiplier scales both or neither.
    //
    // Until now this shader held Recoil's mapinfo.lua defaults (0.4 ambient,
    // 0.7 diffuse) while the terrain read the `.scmap` block, which is exactly
    // why units sat dark on any map whose authored light was not the default.
    // On an SMF map nothing changes: Environment's defaults land within 1% of
    // the constants they replace, in sun and in shade both.
    float3 light = u.sunColour * NdotL + u.sunAmbience;
    light = u.lightingMultiplier * light + u.shadowFill * (1.0 - light);

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
        //
        // The faction selector: the team colour's alpha is NOT transparency —
        // both engines multiply it into the alpha mask (TeamColours.hpp), so
        // it stays 1 on every authored instance. Retail picks the faction
        // shader per blueprint (`ShaderName`); here one fragment reads
        // per-instance channels the host fills from the blueprint faction and
        // wreck state. 0 = base, 1 = Aeon, 2 = Seraphim, 3 = wreckage.
        // Stamped host-side as `1 + v/255` (UnitRenderer.mm): the tint rgb is
        // byte-identical to 1.0, and subtracting 255 recovers 0..3 here.
        const float variant = in.teamColour.a * 255.0 - 255.0;
        const float phongAmountBase = saturate(dot(reflect(-L, N), V));
        if (variant > 0.5 && variant < 1.5) {
            // AEON, from `effects/mesh.fx`'s AeonPS (mesh.fx:2567-2598).
            // TWO differences from NormalMappedPS and nothing else: the
            // highlight is `AeonPhongCoeff * pow(amount, 3)` (mesh.fx:2584) —
            // a tighter lobe in a bluer colour — and the whole light sum is
            // scaled by 0.6 (mesh.fx:2589), which dims Aeon hulls against the
            // same sun. Reflection stays `spec.r * env` WITHOUT the base
            // shader's ×2 (mesh.fx:2585): the dimmer light and the un-doubled
            // reflection are what make Aeon read duskier.
            const float3 aeonAdditive =
                kAeonPhongCoeff * pow(phongAmountBase, 3.0) * tex2.g;
            const float3 aeonMultiplicative =
                environment.sample(texSampler, reflect(-V, N)).rgb * tex2.r;
            const float emissive = kSupComGlowMultiplier * tex2.b;
            float3 aeonLight = u.sunColour * NdotL + u.sunAmbience;
            aeonLight =
                kAeonLightScale * u.lightingMultiplier * aeonLight
                + u.shadowFill * (1.0 - aeonLight);
            float3 blast = float3(0.0);
            for (uint blastB = 0; blastB < 3; ++blastB) {
                float blastDist = length(u.blastPos[blastB].xyz - in.world);
                float blastAtten =
                    saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
                blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
                    * blastAtten * blastAtten;
            }
            return float4(albedo * (emissive + aeonLight + aeonMultiplicative)
                              + aeonAdditive + blast, tex2.b + 0.01);
        }
        if (variant > 1.5 && variant < 2.5) {
            // SERAPHIM, from `effects/mesh.fx`'s UnitFalloffPS (mesh.fx:2649-2684).
            // QUIRK Q2 REPRODUCED: retail forces `shadow = 0` (mesh.fx:2674,
            // the ComputeShadow call commented out), so Seraphim hulls sit in
            // FULL shadow — `light = sunAmbient + (1-sunAmbient)*shadowFill` —
            // whatever the shadow map says. Flagged, not fixed: matching
            // retail's look means matching its darkness here.
            // The view ramp is retail's own texture when bound: `fallOff =
            // tex2D(falloffSampler, float2(NdotV, material.x))` (mesh.fx:2662-2663)
            // where `NdotV = pow(1-dot(view, N), 0.6)` and the row is the
            // per-blueprint `LookupName` (`Falloff_seraphim_lookup.dds`,
            // XSL0101_unit.bp Display.Mesh.LODs). Unbound (`hasUnitLut == 0`)
            // the procedural stand-in below answers: `fallOff.rgb` becomes a
            // grazing sheen `pow(1-NdotV, 2)` tinted by the diffuse alpha, and
            // `fallOff.a` gates the environment the same way the LUT's alpha
            // does (mesh.fx:2671). The V coordinate retail feeds is the
            // per-vertex `colorLookup` (UnitFalloffVS); the .scm carries no such
            // channel, so v = 0.5 reads the ramp's middle row. The highlight keeps
            // retail's own warm coeff and pow9 (mesh.fx:2668).
            const float seraFacing = saturate(dot(normalize(V), N));
            const float seraNdotV = pow(1.0 - seraFacing, 0.6);
            const float3 seraProcedural =
                pow(1.0 - seraFacing, 2.0) * tex1.a;
            const float4 seraFalloff = u.hasUnitLut > 0.5
                ? unitLut.sample(texSampler, float2(seraNdotV, 0.5))
                : float4(seraProcedural, saturate(seraNdotV));
            const float3 seraSheen = seraFalloff.rgb;
            const float seraGate = seraFalloff.a;
            const float seraAmount = saturate(dot(reflect(L, N), V));
            const float3 seraAdditive =
                float3(0.5, 0.6, 0.7) * pow(seraAmount, 9.0) * tex2.g;
            const float3 seraEnv =
                environment.sample(texSampler, reflect(-V, N)).rgb * tex2.r
                * seraGate;
            const float seraShadow = 0.0;
            float3 seraLight =
                u.sunColour * saturate(dot(N, L)) * seraShadow + u.sunAmbience;
            seraLight = seraLight + (1.0 - seraLight) * u.shadowFill;
            float3 blast = float3(0.0);
            for (uint blastB = 0; blastB < 3; ++blastB) {
                float blastDist = length(u.blastPos[blastB].xyz - in.world);
                float blastAtten =
                    saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
                blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
                    * blastAtten * blastAtten;
            }
            const float3 seraColour = tex1.rgb * seraLight + seraEnv + seraAdditive
                + seraSheen;
            return float4(seraColour + blast, tex2.b + 0.01);
        }
        if (variant > 2.5 && variant < 3.5) {
            // WRECKAGE, from `effects/mesh.fx`'s WreckagePS (mesh.fx:2334-2357).
            // SHADOWLESS BY DESIGN: `ComputeLight(dot, 1)` (mesh.fx:2349) —
            // retail's own comment says the crunch "makes for bad artifacts"
            // under shadows (mesh.fx:2348), so a wreck is lit as if unshadowed
            // even mid-map. The specular UV is jittered by the wreck's age
            // (`frac(0.01 * depth.y)`, mesh.fx:2344-2345) and read at 5.15x
            // (mesh.fx:2346); below `spec.g < 0.22` the colour is crushed by
            // `(albedo + spec.r + spec.a) * spec.b * 2.5` (mesh.fx:2351-2352),
            // above by `spec.b * 2` (mesh.fx:2354). Alpha is flat
            // `glowMinimum` (mesh.fx:2356) — wrecks never bloom.
            // The VERTEX crunch (mesh.fx:1176-1182: `sin(14.5…)/cos(10.8…)/
            // sin(20.5…)` over a 0.15-scaled normal) stays CPU-side work the
            // batch's bones would have to carry; the fragment crunch above is
            // what reads on screen, so it lands first.
            const float wreckJitter = fract(0.01 * in.teamColour.g * 255.0);
            const float2 wreckUv = float2(in.uv.x + wreckJitter, in.uv.y - wreckJitter);
            const float4 wreckSpec = shading.sample(texSampler, wreckUv * 5.15);
            float3 wreckLight = u.sunColour * rawNdotL + u.sunAmbience;
            wreckLight = u.lightingMultiplier * wreckLight
                + (1.0 - wreckLight) * u.shadowFill;
            float3 wreckColour = tex1.rgb * wreckLight;
            if (wreckSpec.g < 0.22) {
                wreckColour *= (tex1.a + wreckSpec.r + wreckSpec.a) * wreckSpec.b * 2.5;
            } else {
                wreckColour *= wreckSpec.b * 2.0;
            }
            return float4(wreckColour, 0.01);
        }
        if (variant > 3.5 && variant < 4.5) {
            // METAL, from `effects/mesh.fx`'s NormalMappedMetalPS (mesh.fx:2534-2562).
            // Same skeleton as the base shader with one substitution: the ADDITIVE
            // Phong lobe is the aniso LUT, not a pow — `anisoLookup =
            // float2(dot(reflect(sun, N), -view), dotLightNormal)` (mesh.fx:2546),
            // `anisoAmount = tex2D(anisotropicSampler, anisoLookup)` (mesh.fx:2547),
            // `phongAdditive = anisoAmount * spec.g + (0.5 * spec.r * env, 0)`
            // (mesh.fx:2551). Unbound the lobe falls back to the base pow2 term,
            // so a Metal without its LUT still highlights. Note the reflection is
            // NOT doubled here (mesh.fx:2551 `0.5 *` vs base `2 *`), and the light
            // is the SHARED ComputeLight the base path already computed above.
            const float metalRefl = saturate(dot(reflect(-L, N), V));
            const float4 metalAniso = u.hasUnitLut > 0.5
                ? unitLut.sample(texSampler, float2(metalRefl, rawNdotL))
                : float4(kSupComPhongCoeff * pow(metalRefl, 2.0), 1.0);
            const float3 metalAdditive = metalAniso.rgb * tex2.g
                + 0.5 * tex2.r
                      * environment.sample(texSampler, reflect(-V, N)).rgb;
            const float metalEmissive = kSupComGlowMultiplier * tex2.b;
            float3 blast = float3(0.0);
            for (uint blastB = 0; blastB < 3; ++blastB) {
                float blastDist = length(u.blastPos[blastB].xyz - in.world);
                float blastAtten =
                    saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
                blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
                    * blastAtten * blastAtten;
            }
            return float4(albedo * (metalEmissive + light) + metalAdditive + blast,
                          tex2.b + 0.01);
        }
        if (variant > 4.5 && variant < 5.5) {
            // INSECT, from `effects/mesh.fx`'s NormalMappedInsectPS (mesh.fx:3041-3071).
            // Metal's twin with two deltas: the additive is ALSO masked by
            // `(1 - spec.a)` (mesh.fx:3059), and the diffuse light is DOUBLED
            // (`2 * sunDiffuse * ...`, mesh.fx:3062). Same LUT coordinates as Metal
            // (mesh.fx:3053-3054), same fallback when unbound.
            const float insectRefl = saturate(dot(reflect(-L, N), V));
            const float4 insectAniso = u.hasUnitLut > 0.5
                ? unitLut.sample(texSampler, float2(insectRefl, rawNdotL))
                : float4(kSupComPhongCoeff * pow(insectRefl, 2.0), 1.0);
            const float3 insectAdditive = (insectAniso.rgb * tex2.g
                + 0.5 * tex2.r
                      * environment.sample(texSampler, reflect(-V, N)).rgb)
                * (1.0 - tex2.a);
            const float insectEmissive = kSupComGlowMultiplier * tex2.b;
            float3 insectLight = u.sunColour * saturate(dot(N, L)) * sun * 2.0
                + u.sunAmbience;
            insectLight = u.lightingMultiplier * insectLight
                + (1.0 - insectLight) * u.shadowFill;
            float3 blast = float3(0.0);
            for (uint blastB = 0; blastB < 3; ++blastB) {
                float blastDist = length(u.blastPos[blastB].xyz - in.world);
                float blastAtten =
                    saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
                blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
                    * blastAtten * blastAtten;
            }
            return float4(albedo * (insectEmissive + insectLight) + insectAdditive
                              + blast, tex2.b + 0.01);
        }
        const float phongAmount = phongAmountBase;
        const float3 phongAdditive = kSupComPhongCoeff * pow(phongAmount, 2.0) * tex2.g;
        const float3 phongMultiplicative =
            2.0 * environment.sample(texSampler, reflect(-V, N)).rgb * tex2.r;
        const float emissive = kSupComGlowMultiplier * tex2.b;
        // Fresnel rim — our addition, not retail's: at grazing incidence the hull
        // picks up the environment, which is what separates an edge from whatever
        // is behind it. The specular channel masks it so matte panels stay matte.
        const float3 rim = kSupComPhongCoeff * tex2.g
                           * pow(1.0 - saturate(dot(N, V)), 3.0) * 0.6;

        float3 blast = float3(0.0);
        for (uint blastB = 0; blastB < 3; ++blastB) {
            float blastDist = length(u.blastPos[blastB].xyz - in.world);
            float blastAtten =
                saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
            blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
                * blastAtten * blastAtten;
        }
        return float4(albedo * (emissive + light + phongMultiplicative)
                          + phongAdditive + rim + blast, 1.0);
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
    // Fresnel rim on the same mask: the reflectivity channel decides how much
    // sky an edge catches, so matte surfaces do not glow.
    specular += kUnitSpecular * shininess * pow(1.0 - saturate(dot(N, V)), 3.0) * 0.5;
    float3 blast = float3(0.0);
    for (uint blastB = 0; blastB < 3; ++blastB) {
        float blastDist = length(u.blastPos[blastB].xyz - in.world);
        float blastAtten =
            saturate(1.0 - blastDist / max(u.blastPos[blastB].w, 0.001));
        blast += u.blastColour[blastB].rgb * u.blastColour[blastB].a
            * blastAtten * blastAtten;
    }
    light = mix(light, kRecoilEnvironment, shininess);  // Recoil's flat sky
    light += float3(selfIllum);                   // self-illum
    return float4(albedo * light + specular + blast, 1.0);
}

// SHIELD SHELLS, from `effects/mesh.fx`'s per-faction shield shaders.
// Retail draws a shield as a SECOND pass over the unit: the opaque hull first,
// then an electricity shell at a 0.05 offset (PhaseShield techniques,
// mesh.fx:5805-5958). Here the same idea is one fragment on the unit's own
// geometry — the offset becomes a view-facing lift in `shieldVertex` —
// because there is no second-pass plumbing yet (see the gap note at the end).
struct ShieldOut {
    float4 position [[position]];
    float2 uv;
    float3 world;
    float3 normal;
    float health;   // 0..1, the shield's remaining fraction
    float seconds;  // animation clock, retail `time`/`material.x`
    uint kind;      // 0 UEF, 1 Cybran, 2 Aeon, 3 Seraphim
};

// Lifts the shell toward the viewer so it floats just off the hull — retail's
// 0.05 pass offset (mesh.fx:5805-5958) without a second draw. In NDC so the
// gap is constant on screen: `clip.z += 0.05 * clip.w` pushes depth away,
// matching a shell drawn slightly proud of the surface.
vertex ShieldOut shieldVertex(uint vid [[vertex_id]],
                              uint iid [[instance_id]],
                              const device UnitVertexIn* vertices [[buffer(0)]],
                              constant Uniforms& u [[buffer(1)]],
                              const device UnitInstanceIn* instances [[buffer(2)]],
                              const device BoneTransformIn* bones [[buffer(3)]],
                              constant PoseUniforms& p [[buffer(4)]],
                              constant float4& shieldParams [[buffer(5)]]) {
    const UnitVertexIn v = vertices[vid];
    const UnitInstanceIn inst = instances[iid];
    const uint poseIndex = poseIndexFor(p, inst.animationPhase);
    const BoneTransformIn bone = bones[poseIndex * p.boneCount + v.boneIndex];
    const float4 boneRotation = float4(bone.rotation);
    const float3 local = applyBuilderAim(
        rotateBy(boneRotation, float3(v.position)) + float3(bone.translation), bone, inst, p);
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);
    ShieldOut out;
    float4 clip = u.viewProjection * float4(world, 1.0);
    clip.z += 0.05 * clip.w;
    out.position = clip;
    out.uv = float2(v.uv.x, 1.0 - v.uv.y);
    out.world = world;
    out.normal = unitOrient(
        applyBuilderAimNormal(rotateBy(boneRotation, float3(v.normal)), bone, inst, p), inst);
    out.health = shieldParams.x;
    out.seconds = shieldParams.y;
    out.kind = uint(shieldParams.z);
    return out;
}

// The four faction shells. Each branch is its retail PS verbatim in structure;
// `in.seconds` stands in for retail's per-shield `material.x` clock.
// Sampler inputs reuse the unit's own textures (albedo/specular): retail reads
// SEPARATE shield maps, so these are the closest bound analogues until shield
// textures upload.
fragment float4 shieldFragment(ShieldOut in [[stage_in]],
                               constant Uniforms& u [[buffer(1)]],
                               texture2d<float> albedoTex [[texture(0)]],
                               texture2d<float> specTex [[texture(1)]],
                               sampler texSampler [[sampler(0)]]) {
    // UEF ShieldPS (mesh.fx:3076-3118): normal-modulated blue shell whose
    // alpha picks from a decision tree over (spec.g vs albedo.r, spec.b vs
    // albedo.g, …), reddening as `material.y` (health) drops (mesh.fx:3111),
    // masked by `colorMask.a` against sphere-UV pinching (mesh.fx:3116).
    if (in.kind == 0u) {
        const float4 colorMask = albedoTex.sample(texSampler, in.uv);
        const float4 albedo = albedoTex.sample(texSampler, in.uv);
        const float3 nrm =
            specTex.sample(texSampler, in.uv).gaa * 2.0 - 1.0;
        const float nz = sqrt(saturate(1.0 - nrm.x * nrm.x - nrm.y * nrm.y));
        const float3 spec = specTex.sample(texSampler, in.uv).rgb;
        float4 colour = float4(albedo.r * nrm.x + albedo.g * nrm.y
                                   + albedo.r * nz + 0.0,
                               albedo.r * nrm.x + albedo.g * nrm.y
                                   + albedo.g * nz + 0.0,
                               albedo.r * nrm.x + albedo.g * nrm.y
                                   + albedo.r * nz + 0.25,
                               1.0);
        const float pulse = sin(fract(0.01 * in.seconds) * 3.14);
        const float pulse6 = sin(fract(0.06 * in.seconds) * 3.14);
        if (spec.g <= albedo.r) {
            if (spec.b >= albedo.g) {
                colour.a = (colour.b >= nz) ? 0.12 : mix(0.05, 0.0, pulse);
            } else {
                colour.a = (nz >= albedo.r) ? 0.2 : mix(0.01, 0.1, pulse);
            }
        } else {
            if (spec.r >= albedo.r) {
                colour.a = (spec.b >= albedo.r) ? 0.025 : 0.1;
            } else {
                colour.a = (spec.g >= albedo.g) ? 0.02 : mix(0.37, 0.46, pulse);
            }
        }
        colour.rgb += float3(0.0, 0.0, 0.15);
        float4 mod1 = mix(float4(0.5, 0.0, 0.0, 0.05), colour, 0.5);
        mod1 = mix(colour, mod1 + colour, pulse6);
        colour = mix(mod1, colour, in.health);
        colour += colorMask.b * 0.95;
        colour.a *= colorMask.a;
        return colour;
    }
    // CYBRAN ShieldCybranPS (mesh.fx:3145-3190, default alpha 0.17/0.24 per
    // technique): colour-wackiness over two albedo and two specular taps,
    // health-lerped from near-black red (mesh.fx:3170), floored to
    // (0.15,0.15,0.3) when the sum dies (mesh.fx:3181).
    if (in.kind == 1u) {
        const float4 albedo = albedoTex.sample(texSampler, in.uv);
        const float3 spec = specTex.sample(texSampler, in.uv).rgb;
        const float3 color4 =
            ((albedo.g - spec.b) * spec.b) * albedo.a;
        const float3 color2 = albedo.b * spec.g * 3.0;
        const float3 color3 = spec.g * albedo.a;
        float3 final = float3(0.05, 0.0, 0.3) + color4 - color2 * color3;
        float3 mod1 = mix(float3(0.2, 0.0, 0.0), final, 0.5);
        mod1 = mix(final, (mod1 - final) + (color4 + mod1),
                   sin(fract(0.06 * in.seconds) * 3.14));
        final = mix(mod1, final, in.health);
        final += (albedo.r + albedo.r) * 0.1;
        final -= 1.0 - albedo.a;
        const float clradd = final.r + final.g + final.b;
        if (clradd < 0.1) {
            final = float3(0.15, 0.15, 0.3);
        } else if (clradd > 0.1 && clradd < 0.2) {
            final = spec.b;
        }
        final += (albedo.r + albedo.r) * float3(0.0, 0.0, 0.3);
        const float alpha = 0.17 + (albedo.r + albedo.r) * 0.2;
        return float4(final, alpha);
    }
    // AEON ShieldAeonPS (mesh.fx:3210-3243, alpha 0.5 per technique):
    // phong-plus-environment magic scaled by two pulsing specular layers,
    // health-lerped from pale red (mesh.fx:3238).
    if (in.kind == 2u) {
        const float4 albedo = albedoTex.sample(texSampler, in.uv);
        const float3 spec = specTex.sample(texSampler, in.uv).rgb;
        const float3 N = normalize(in.normal);
        const float3 V = normalize(u.cameraPosition - in.world);
        const float phong =
            saturate(dot(reflect(-V, N), V)) * 0.6;
        const float3 env =
            albedoTex.sample(texSampler, reflect(-V, N).xy * 0.5 + 0.5).rgb;
        const float3 color1 = phong + env - albedo.ggg;
        const float3 color2 =
            spec.rrr * mix(0.6, 1.3, sin(fract(0.015 * in.seconds) * 3.14));
        const float3 color3 =
            spec.rrr * mix(2.0, 2.2, sin(fract(0.0045 * in.seconds) * 3.14));
        float3 final = (color1 * color2) * color3;
        final = (final * N) * 0.65 + final;
        final = final * env * albedo.a;
        const float3 mod1 = mix(float3(0.7, 0.3, 0.3), final, 0.9);
        final = mix(mod1, final, in.health);
        const float alpha =
            0.707 * ((env.r + env.g + env.b) * 0.25) + albedo.b * 0.5;
        return float4(mix(mod1, final, in.health), alpha);
    }
    // SERAPHIM ShieldSeraphimPS (mesh.fx:3260-3304, additive): a dome whose
    // blue tint falls off as `dp*dp` from the world-up axis (mesh.fx:3302)
    // and whose alpha fades near the crown past `t = 0.753` (mesh.fx:3285-3299).
    // QUIRK REPRODUCED: `normal_pixel.g - 0.5` (mesh.fx:3277) and
    // `dp = |cos(dot(up, N))|` (mesh.fx:3279) are kept as written.
    const float3 N = normalize(in.normal);
    const float4 normalPixel = specTex.sample(texSampler, in.uv);
    const float4 uvAddr = specTex.sample(texSampler, in.uv);
    const float4 spec = specTex.sample(texSampler, in.uv + uvAddr.rb * 0.1);
    float m = abs(normalPixel.g - 0.5);
    const float dp = abs(dot(float3(0.0, 1.0, 0.0), N));
    const float t = abs(dot(float3(0.0, 1.0, 0.0), normalize(in.normal)));
    const float cutoff = 0.753;
    if (t >= cutoff) {
        m = 1.0 - 0.7 * (t - cutoff) / (1.0 - cutoff);
    } else {
        m = 1.0;
    }
    const float dp2 = abs(dot(normalize(u.cameraPosition - in.world), N));
    const float maxB = 0.453;
    const float channel = maxB - clamp(1.0 - dp, 0.0, maxB);
    const float alpha = m * (dp2 * 0.3 + channel) * 1.75;
    return float4(0.425 * dp * dp * spec.r, 0.76274 * dp * dp * spec.g,
                  1.0 * dp * dp * spec.b, alpha);

// PIPELINE GAP (shields not yet drawn): `shieldVertex`/`shieldFragment` compile
// with the library, which proves the equations, but no pipeline binds them.
// To draw: (1) a `shieldPipeline_` (additive for Seraphim, StraightAlpha for
// the rest) built in Renderer.mm beside `unitPipeline_`; (2) shield-texture
// uploads (retail reads dedicated albedo/specular shield maps, not the unit's
// pair); (3) per-shield health + clock uniforms (`shieldParams`: health,
// seconds, kind) fed from the sim's shield state, which has no renderer
// plumbing yet; (4) a second draw per shielded batch AFTER the hull (retail's
// 2-pass structure) — all in Renderer.mm/FxRenderer scope, outside this slice.
}
)MSL";

} // namespace rm::shaders
