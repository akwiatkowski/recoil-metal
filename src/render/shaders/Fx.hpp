#pragma once

// Particles, and the shadow depth pass they share a buffer with.
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

inline constexpr const char* kFx = R"MSL(
// --- Particles ---------------------------------------------------------------
// Dust behind moving units. Each particle is a camera-facing quad expanded here
// from four vertices, and it is AGED HERE TOO: the CPU uploads where a particle
// was born, the velocity it was born with and how long ago that was, and this
// works out the rest. Nothing on the CPU ever integrates a position.

struct ParticleIn {
    packed_float3 origin;
    float age;
    packed_float3 velocity;
    float lifetime;
    packed_float4 colour;   // premultiplied
    float size;
    float growth;
};

struct ParticleOut {
    float4 position [[position]];
    float4 colour;
    float2 offset;  // -1..1 across the quad, for the round falloff
};

// How hard dust is pulled back down, in elmos per second squared.
//
// Not gravity: a dust cloud is suspended in air and settles far more slowly than a
// stone falls. Small enough that a 1.1-second puff rises for most of its life and
// only just begins to sink, which is what disturbed dust does.
constant float kDustSettle = 6.0;

vertex ParticleOut particleVertex(uint vid [[vertex_id]],
                                  uint iid [[instance_id]],
                                  const device ParticleIn* particles [[buffer(0)]],
                                  constant Uniforms& u [[buffer(1)]]) {
    const ParticleIn p = particles[iid];
    const float t = p.age;

    // Where it is now. The whole of the simulation, and it costs one multiply-add
    // per axis rather than a per-frame pass over the buffer on the CPU.
    const float3 world = float3(p.origin) + float3(p.velocity) * t
                       + float3(0.0, -0.5 * kDustSettle * t * t, 0.0);

    const float life = max(p.lifetime, 1e-4);
    const float remaining = saturate(1.0 - t / life);

    // Swells as it disperses, which is what makes a puff read as a cloud rather
    // than a moving dot.
    const float size = p.size + p.growth * t;

    // Fades in fast and out slow. A puff that appeared at full opacity would pop,
    // and one that vanished at full opacity would blink out; the product of a
    // quick rise and a slow decay is what a real puff looks like without needing a
    // curve stored per particle.
    const float fadeIn = saturate(t / (0.15 * life));
    const float alpha = fadeIn * remaining * remaining;

    // A camera-facing quad, from the vertex id alone — no index buffer and no
    // per-particle geometry. The two screen axes come out of the view-projection's
    // own columns, which is what keeps the quad facing the camera at any angle
    // without the CPU sending a basis.
    const float2 corner = float2((vid == 1 || vid == 2) ? 1.0 : -1.0,
                                 (vid >= 2) ? 1.0 : -1.0);

    // The rows of the view matrix are the camera's axes in world space; taking
    // them from the view-projection means reading its columns, since MSL's float4x4
    // is column-major.
    const float3 right = normalize(float3(u.viewProjection[0][0], u.viewProjection[1][0],
                                          u.viewProjection[2][0]));
    const float3 up = normalize(float3(u.viewProjection[0][1], u.viewProjection[1][1],
                                       u.viewProjection[2][1]));

    const float3 corner3 = world + (right * corner.x + up * corner.y) * size * 0.5;

    ParticleOut out;
    out.position = u.viewProjection * float4(corner3, 1.0);
    // Premultiplied, so scaling the whole thing by alpha is the correct fade for
    // both a translucent puff and an additive spark.
    out.colour = float4(p.colour) * alpha;
    out.offset = corner;

    // A particle past its life is collapsed rather than branched around: a vertex
    // shader cannot decline to emit, and the CPU has already dropped the expired
    // ones — this only covers the frame in which one expires mid-flight.
    if (t >= p.lifetime) {
        out.position = float4(0.0, 0.0, -1.0, 1.0);  // behind the near plane
    }
    return out;
}

fragment float4 particleFragment(ParticleOut in [[stage_in]]) {
    // Round, and soft at the edge. A square puff reads as a square, and a hard
    // circle reads as a coin; the falloff is what makes overlapping puffs merge
    // into a cloud rather than stacking as discs.
    const float r = length(in.offset);
    // Linear rather than squared. Squared looked right in the abstract and shrank
    // the visible core of a puff to a fraction of its quad, so a 17-elmo puff read
    // as a speck — most of the sprite was spent on a gradient too faint to see.
    const float falloff = saturate(1.0 - r);
    return in.colour * falloff;
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

)MSL";

} // namespace rm::shaders
