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
    const float3 local = rotateBy(boneRotation, float3(v.position)) + float3(bone.translation);
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);
    const float3 worldNormal = unitOrient(rotateBy(boneRotation, float3(v.normal)), inst);

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
                                  constant BuildUniforms& b [[buffer(1)]]) {
    // How far up this fragment sits within the model's own height, 0 at the ground and 1 at
    // the roof. Guarded because a model with no stated height would divide by zero and take
    // the whole building with it.
    const float span = max(b.height, 0.001);
    const float up = saturate((in.world.y - b.baseY) / span);

    // The shading the ghost uses, for the same reason: a flat tint renders a model as one
    // shapeless slab and the silhouette is where identity lives.
    const float shape = 0.55 + 0.45 * saturate(in.normal.y * 0.5 + 0.5);

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
    const float3 metal = float3(0.42, 0.44, 0.47) * shape;
    const float3 built = mix(metal, b.tint.rgb, 0.35);
    const float3 hot = mix(built, float3(1.0), saturate(edge) * 0.85);

    // A faint pulse over the whole thing while it is unfinished, so a site that is PAUSED for
    // want of mass still reads as a site rather than as a finished building of the wrong
    // colour. It stops at completion because `progress` reaches one and the term vanishes.
    const float pulse = 1.0 + 0.06 * sin(b.seconds * 6.0) * (1.0 - b.progress);
    return float4(hot * pulse, 1.0);
}

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
    // The BASIS comes from screen-space derivatives rather than from per-vertex
    // tangents. The .scm format does carry a tangent and a binormal per vertex, and
    // this loader reads past them, deliberately: plumbing them through would grow
    // ModelVertex from 36 bytes to 60 for EVERY model in the project — 2000 BAR unit
    // meshes included — to normal-map scenery. Derivatives cost a few instructions
    // on this path alone and no memory anywhere.
    float3 shadingNormal = normalize(in.normal);
    if (u.alphaIsOpacity > 0.5 && u.hasTexture2 > 0.5) {
        const float2 packed = float2(tex2.a, tex2.g) * 2.0 - 1.0;
        // Reconstructed rather than stored, which is the point of keeping two:
        // clamped because a compressed pair can leave the unit disc, and a negative
        // radicand would come back NaN and paint the fragment black.
        const float z = sqrt(saturate(1.0 - dot(packed, packed)));
        const float3 tangentNormal = float3(packed, z);

        // The tangent frame, per pixel, from how the world position and the uv change
        // across the triangle. Gram-Schmidt against the interpolated normal so the
        // frame stays orthogonal where the derivatives disagree with it.
        const float3 dpdx = dfdx(in.world);
        const float3 dpdy = dfdy(in.world);
        const float2 dudx = dfdx(in.uv);
        const float2 dudy = dfdy(in.uv);

        const float determinant = dudx.x * dudy.y - dudy.x * dudx.y;
        if (abs(determinant) > 1e-12) {
            const float3 tangent =
                normalize((dpdx * dudy.y - dpdy * dudx.y) / determinant);
            const float3 T = normalize(tangent - shadingNormal * dot(shadingNormal, tangent));
            const float3 B = cross(shadingNormal, T);
            shadingNormal = normalize(T * tangentNormal.x + B * tangentNormal.y
                                      + shadingNormal * tangentNormal.z);
        }
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

} // namespace rm::shaders
