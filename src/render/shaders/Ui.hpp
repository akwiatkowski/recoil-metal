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
