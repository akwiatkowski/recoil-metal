#pragma once

// Selection rings and text.
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

inline constexpr const char* kUi = R"MSL(
// --- Selection rings ---------------------------------------------------------
// A band on the ground under each selected unit, built on the CPU to follow the
// terrain (core/scene/GroundDecals.hpp) and drawn as one triangle list.
//
// Deliberately unlit and unshadowed. This is interface, not scenery: a ring that
// dimmed in shade or on a slope facing away from the sun would be least visible
// exactly where a unit is hardest to pick out.

struct DecalVertexIn {
    packed_float3 position;
    packed_float4 colour;
};

struct DecalOut {
    float4 position [[position]];
    float4 colour;
};

vertex DecalOut decalVertex(uint vid [[vertex_id]],
                          const device DecalVertexIn* vertices [[buffer(0)]],
                          constant Uniforms& u [[buffer(1)]]) {
    DecalOut out;
    out.position = u.viewProjection * float4(float3(vertices[vid].position), 1.0);
    out.colour = float4(vertices[vid].colour);
    return out;
}

fragment float4 decalFragment(DecalOut in [[stage_in]]) {
    return in.colour;
}

// --- Text --------------------------------------------------------------------
// The HUD. Positions arrive in authored HUD points from the viewport's top-left and are turned
// into clip space here, so a caller places text where it means to and nothing downstream has
// to know the viewport's size but this.
//
// The atlas is single-channel COVERAGE, not colour: the glyph decides how much of the
// vertex's colour lands, which is what lets one grey atlas draw white text, red text and a
// drop shadow without three textures.

struct TextVertexIn {
    packed_float2 position;
    packed_float2 uv;
    packed_float4 colour;
};

struct TextOut {
    float4 position [[position]];
    float2 uv;
    float2 screenUv;
    float4 colour;
};

vertex TextOut textVertex(uint vid [[vertex_id]],
                          const device TextVertexIn* vertices [[buffer(0)]],
                          constant float2& viewport [[buffer(1)]]) {
    const TextVertexIn v = vertices[vid];

    // Authored HUD points to clip space. Y is flipped because a window's origin is top-left and
    // clip space's is centre-up; getting that backwards renders the HUD upside down at the
    // bottom of the screen, which looks like a layout bug rather than a sign error.
    const float2 ndc = float2(v.position.x / max(viewport.x, 1.0) * 2.0 - 1.0,
                              1.0 - v.position.y / max(viewport.y, 1.0) * 2.0);

    TextOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.uv = float2(v.uv);
    out.screenUv = v.position / max(viewport, float2(1.0));
    out.colour = float4(v.colour);
    return out;
}

fragment float4 textFragment(TextOut in [[stage_in]],
                             texture2d<float> atlas [[texture(0)]],
                             sampler atlasSampler [[sampler(0)]]) {
    const float coverage = atlas.sample(atlasSampler, in.uv).r;

    // Premultiplied out, because the pipeline blends that way: the colour is scaled by how
    // much of the pixel the glyph covers, and so is the alpha.
    return float4(in.colour.rgb * in.colour.a * coverage, in.colour.a * coverage);
}

// Solid UI geometry has no texture dependency. Keeping it separate means panels, bars and
// battlefield overlays do not borrow an opaque texel from whichever font happened to build.
fragment float4 solidFragment(TextOut in [[stage_in]]) {
    return float4(in.colour.rgb * in.colour.a, in.colour.a);
}

struct ScreenOut {
    float4 position [[position]];
    float2 uv;
};

// One oversized triangle, used both to quarter the scene and to put the sharp world back.
vertex ScreenOut screenVertex(uint vid [[vertex_id]]) {
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    const float2 ndc = corners[vid];
    ScreenOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.uv = float2((ndc.x + 1.0) * 0.5, (1.0 - ndc.y) * 0.5);
    return out;
}

fragment float4 screenFragment(ScreenOut in [[stage_in]],
                               texture2d<float> image [[texture(0)]],
                               sampler imageSampler [[sampler(0)]]) {
    return image.sample(imageSampler, in.uv);
}

// The bloom bright pass, into its own quarter-res target — NOT the glass pair, which
// the panel blur owns and the bloom pass must not fight mid-frame. The knee sits at
// 0.8, just under the tone curve's shoulder: pixels still inside the curve keep
// nothing, and what has started rolling toward white — fire, additive glows, sun
// glints — keeps its excess. A 1.0 knee would extract nothing at all: the authored
// sprites cap near it rather than past it.
fragment float4 thresholdFragment(ScreenOut in [[stage_in]],
                                  texture2d<float> image [[texture(0)]],
                                  sampler imageSampler [[sampler(0)]]) {
    const float3 colour = image.sample(imageSampler, in.uv).rgb;
    return float4(max(colour - float3(0.8), float3(0.0)), 1.0);
}

// Screen-space ambient occlusion. The world pass already pays for a full-res depth
// buffer; this pass reads it back at quarter res and asks, for each pixel, how much
// of a small sphere around the reconstructed view position is blocked by geometry
// in front of it. There is no G-buffer, so there are no normals to gate a
// hemisphere — the sphere-with-range-check version is what the depth alone can
// honestly answer, and at RTS pitch it is exactly the ground-contact shadow the
// effect exists for. Depth is Metal's [0,1] perspective clip, so every sample
// re-inverts the projection: near/far/fov come in through `AoParams` rather than
// being constants, because the camera owns them.
struct AoParams {
    float depthA;     // far / (far - near)
    float depthB;     // far * near / (far - near)
    float2 projScale; // (aspect * tan(fov/2), tan(fov/2)) — ndc = view.xy / (projScale * viewZ)
    float radius;     // sample sphere, elmos
    float intensity;  // how dark full occlusion goes
    float bias;       // elmos of depth gap before a sample counts as occluded
    float biasScale;  // extra bias per elmo of view depth — one depth texel is many
                      // elmos wide at range, and a slope inside it reads as an occluder
    float fadeStart;  // viewZ where occlusion starts thinning out
    float fadeEnd;    // viewZ where it is gone entirely
};

fragment float aoFragment(ScreenOut in [[stage_in]],
                          depth2d<float> depth [[texture(0)]],
                          sampler depthSampler [[sampler(0)]],
                          constant AoParams& p [[buffer(0)]]) {
    const float2 uvToNdc = float2(2.0, -2.0);
    const float2 uvBiasNdc = float2(-1.0, 1.0);
    const auto viewAt = [&](float2 uv) {
        const float d = depth.sample(depthSampler, uv);
        const float viewZ = p.depthB / (p.depthA - d);
        const float2 ndc = uv * uvToNdc + uvBiasNdc;
        return float3(ndc * p.projScale * viewZ, -viewZ);
    };
    const float3 view = viewAt(in.uv);

    // The surface normal, reconstructed from the depth buffer itself: there is no
    // G-buffer, and without it the sample sphere dips under any rising slope and
    // the whole terrain reports itself buried. Finite differences one texel over —
    // the FULL-resolution depth's texel, not the AO target's, or the normal smears
    // four pixels wide.
    const float2 texel = float2(1.0 / float(depth.get_width()),
                                1.0 / float(depth.get_height()));
    const float3 viewX = viewAt(in.uv + float2(texel.x, 0.0));
    const float3 viewY = viewAt(in.uv + float2(0.0, texel.y));
    float3 normal = normalize(cross(viewX - view, viewY - view));
    // Face it toward the camera, which sits at the view-space origin looking
    // down -Z: a surface's normal opposes the direction to the eye.
    if (dot(normal, -view) < 0.0) {
        normal = -normal;
    }

    float occluded = 0.0;
    float counted = 0.0;
    for (int i = 0; i < 8; ++i) {
        // A deterministic spiral over the sphere, jittered by pixel so the eight
        // taps do not line up into rings. The blur pass smears the rest.
        const float fi = float(i);
        const float angle = fi * 2.3999632 + in.uv.x * 17.0 + in.uv.y * 13.0;
        const float3 offset =
            float3(cos(angle), sin(angle), fract(fi * 0.6180339) * 2.0 - 1.0) * p.radius;
        // Hemisphere, not sphere: samples under the surface occlude nothing real,
        // they only report the slope they are inside.
        if (dot(offset, normal) <= 0.0) {
            continue;
        }
        counted += 1.0;
        const float3 sample3 = view + offset;
        const float sampleZ = -sample3.z;
        if (sampleZ <= 0.0) {
            continue;
        }
        const float2 sampleNdc = sample3.xy / (p.projScale * sampleZ);
        const float2 sampleUv = sampleNdc * float2(0.5, -0.5) + 0.5;
        const float sceneZ = p.depthB / (p.depthA - depth.sample(depthSampler, sampleUv));
        const float gap = sampleZ - sceneZ;
        const float bias = p.bias + sceneZ * p.biasScale;
        occluded += step(bias, gap) * (1.0 - saturate(gap / p.radius));
    }
    // The effect is a near-field contact cue: beyond the fade band a four-elmo
    // sphere is sub-texel noise, and distant slopes false-positive on depth
    // aliasing before the bias can save them.
    const float fade = 1.0 - saturate((-view.z - p.fadeStart) / (p.fadeEnd - p.fadeStart));
    return saturate(1.0 - occluded / max(counted, 1.0) * p.intensity * fade);
}

// The final composite: linear HDR world to display. ACES-approximated filmic
// curve (Narkowicz) so fire rolls off instead of clipping, then sRGB encode.
// Exposure is 1.0 — the maps' authored light already balances the frame, and a
// knob arrives when a map proves it needs one. Only the composite uses this;
// the downsample keeps screenFragment so the blur works in linear.
//
// The AO multiplies the world FIRST — it is a property of the surfaces, so it
// dims them before the bloom adds light on top — and the blurred bright pass
// folds back in BEFORE the curve, scaled by the map's own `bloom` gain: adding
// it after tone-mapping would halo already-gamma-encoded colour and
// double-encode it.
struct CompositeParams {
    float bloomGain;
    float aoStrength;  // 0 binds AO out — for frames whose AO pass did not run
};

fragment float4 compositeFragment(ScreenOut in [[stage_in]],
                                  texture2d<float> image [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]],
                                  texture2d<float> ao [[texture(2)]],
                                  sampler imageSampler [[sampler(0)]],
                                  constant CompositeParams& params [[buffer(0)]]) {
    const float occlusion = mix(1.0, ao.sample(imageSampler, in.uv).r, params.aoStrength);
    float3 colour = image.sample(imageSampler, in.uv).rgb * occlusion
                  + bloom.sample(imageSampler, in.uv).rgb * params.bloomGain;
    const float3 x = colour;
    colour = x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14);
    return float4(pow(saturate(colour), float3(1.0 / 2.2)), 1.0);
}

// The one shared blurred scene is sampled through every solid panel surface. The result is
// opaque: it replaces the sharp world already composed at that pixel, then absorbs it into the
// panel tint. Chrome remains a later semantic layer and supplies the material's edge.
fragment float4 glassFragment(TextOut in [[stage_in]],
                              texture2d<float> backdrop [[texture(0)]],
                              sampler backdropSampler [[sampler(0)]],
                              constant float4& material [[buffer(1)]]) {
    // Panel shadows share this solid stream but are not glass. Preserve their ordinary
    // premultiplied source-over output, especially for classic nine-slice panels.
    if (dot(in.colour.rgb, float3(1.0)) < 0.001) {
        return float4(0.0, 0.0, 0.0, in.colour.a);
    }
    const float3 blurred = backdrop.sample(backdropSampler, in.screenUv).rgb;
    const float luminance = dot(blurred, float3(0.2126, 0.7152, 0.0722));
    const float3 absorbed = mix(float3(luminance), blurred, material.y) * material.z;
    const float strength = saturate(material.x * in.colour.a);
    return float4(mix(absorbed, in.colour.rgb, strength), 1.0);
}

// The same geometry, sampling a full-colour IMAGE rather than a coverage mask.
//
// A SECOND FRAGMENT FUNCTION rather than a branch in the first, and the reason is what the two
// do with the texture: `textFragment` reads `.r` as COVERAGE and paints the vertex colour
// through it, which is exactly right for a glyph and turns a photograph into a red-channel
// silhouette tinted one colour. An image wants its own pixels.
//
// The vertex colour survives as a TINT and a FADE — rgb multiplies, alpha scales — so the same
// quad can be dimmed or washed toward the interface's hue without a third shader. White at full
// alpha is the identity, which is what the minimap passes.
fragment float4 imageFragment(TextOut in [[stage_in]],
                              texture2d<float> image [[texture(0)]],
                              sampler imageSampler [[sampler(0)]]) {
    const float4 texel = image.sample(imageSampler, in.uv);
    const float alpha = texel.a * in.colour.a;
    return float4(texel.rgb * in.colour.rgb * alpha, alpha);
}

// The terrain's R8 vision mask over the minimap thumbnail. Zero means unseen; one leaves the
// picture untouched. Output is premultiplied because the UI pipeline blends that way.
fragment float4 minimapFogFragment(TextOut in [[stage_in]],
                                   texture2d<float> mask [[texture(0)]],
                                   sampler imageSampler [[sampler(0)]]) {
    const float hidden = 1.0 - mask.sample(imageSampler, in.uv).r;
    const float alpha = hidden * 0.72;
    return float4(float3(0.015, 0.025, 0.035) * alpha, alpha);
}

)MSL";

} // namespace rm::shaders
