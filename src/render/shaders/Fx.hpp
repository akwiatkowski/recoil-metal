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
    packed_float3 axis;
    float length;
    uint material;
    packed_float3 acceleration;
    float rotation;
    float rotationRate;
    packed_float3 animation;
    uint flags;
    packed_float2 trailRange;
};

struct ParticleOut {
    float4 position [[position]];
    float4 colour;
    float2 offset;  // -1..1 across the quad, for the round falloff
    uint material [[flat]];
    uint flags [[flat]];
    float2 trailRange;
    float along;
    float age;
    float length;
    float4 animation;
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
    const bool textured = p.material != 0xffffffffu;

    // Where it is now. The whole of the simulation, and it costs one multiply-add
    // per axis rather than a per-frame pass over the buffer on the CPU.
    float3 world = float3(p.origin) + float3(p.velocity) * t
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

    float3 corner3 = world + (right * corner.x + up * corner.y) * size * 0.5;
    float along = (vid >= 2) ? 1.0 : 0.0;
    float side = (vid == 1 || vid == 3) ? 1.0 : -1.0;
    if (textured) {
        world = float3(p.origin) + float3(p.velocity) * t + 0.5 * float3(p.acceleration) * t * t;
        if (p.length > 0) {
            const float3 axis = float3(p.axis);
            const float3 view = normalize(cross(right, up));
            float3 across = cross(view, axis);
            across = dot(across, across) > 0.00001 ? normalize(across) : right;
            corner3 = world + axis * (p.length * along) + across * (side * p.size * 0.5);
        } else {
            const float angle = p.rotation + p.rotationRate * t;
            const float2 corner = float2(side, along * 2.0 - 1.0);
            const float2 rotated = float2(corner.x*cos(angle)-corner.y*sin(angle),
                                         corner.x*sin(angle)+corner.y*cos(angle));
            const float3 spriteRight = (p.flags & 1u) ? float3(1,0,0) : right;
            const float3 spriteUp = (p.flags & 1u) ? float3(0,0,1) : up;
            corner3 = world + (spriteRight * rotated.x + spriteUp * rotated.y) * max(0.0, size) * 0.5;
        }
    }

    ParticleOut out;
    out.position = u.viewProjection * float4(corner3, 1.0);
    // Premultiplied, so scaling the whole thing by alpha is the correct fade for
    // both a translucent puff and an additive spark.
    out.colour = float4(p.colour) * (textured ? 1.0 : alpha);
    out.material = p.material;
    out.flags = p.flags;
    out.trailRange = float2(p.trailRange);
    out.along = along;
    out.age = p.age;
    out.length = p.length;
    out.animation = float4(float3(p.animation), t / life);
    out.offset = textured ? float2(side, along * 2.0 - 1.0) : corner;

    // A particle past its life is collapsed rather than branched around: a vertex
    // shader cannot decline to emit, and the CPU has already dropped the expired
    // ones — this only covers the frame in which one expires mid-flight.
    if (t >= p.lifetime) {
        out.position = float4(0.0, 0.0, -1.0, 1.0);  // behind the near plane
    }
    return out;
}

struct WeaponMaterialIn { float4 startColour; float4 endColour; float4 sampling; float4 format; };
fragment float4 particleFragment(ParticleOut in [[stage_in]],
    texture2d<float> texture [[texture(0)]], texture2d<float> ramp [[texture(1)]],
    texture2d<float> background [[texture(2)]],
    constant WeaponMaterialIn& material [[buffer(0)]]) {
    if (in.material != 0xffffffffu) {
        constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);
        constexpr sampler wrappedSampler(filter::linear, address::repeat);
        const bool beam = material.sampling.y > 0.5;
        const bool ribbon = (in.flags & 2u) != 0u;
        const float across = in.offset.x * 0.5 + 0.5;
        // A ribbon segment is one piece of a longer trail: its texture position is the
        // trail-relative coordinate, 0 at the head and 1 at the authored length, and the
        // repeat count for the whole trail was folded into sampling.z at load.
        const float along = ribbon ? mix(in.trailRange.x, in.trailRange.y, in.along) : in.along;
        const float repeats = ribbon ? material.sampling.z
            : beam && material.sampling.z > 0 ? in.length * 0.125 * material.sampling.z : 1.0;
        float2 uv = float2(across, along * repeats + in.age * material.sampling.w);
        if (in.length == 0) {
            const float frames = max(1.0, material.format.y);
            const float strips = max(1.0, material.format.z);
            const float frame = fmod(floor(in.age * max(0.0, in.animation.x)), frames);
            const float strip = clamp(floor(in.animation.y * strips), 0.0, strips-1.0);
            uv = (float2(across, in.along) + float2(frame, strip)) / float2(frames, strips);
        }
        const float4 texel = texture.sample(wrappedSampler, uv);
        float4 colour = texel;
        if (material.sampling.x > 0.5) {
            // TrailBlueprint: the ramp's left edge is the head of the trail, its right edge
            // the tail. A bolt strip is drawn tail to head, so its ramp reads backwards.
            const float rampTime = ribbon ? along : in.length > 0 ? 1.0 - in.along : in.animation.w;
            const float rampRow = in.length > 0 ? across : in.animation.z;
            colour *= ramp.sample(linearSampler, float2(rampTime, rampRow));
        }
        colour *= mix(material.startColour, material.endColour, along);
        const uint blend = uint(material.format.x);
        if (blend == 5) {
            // Retail particle.fx WorldRefractPS: RG is an offset, alpha masks distortion.
            const float2 screen = in.position.xy / float2(background.get_width(), background.get_height());
            const float2 offset = 0.005 * (2.0 * texel.rg - 1.0);
            return float4(background.sample(linearSampler, screen+offset).rgb * colour.a, colour.a);
        }
        // Retail particle.fx uses distinct blend states. Premultiplying only the alpha
        // mode lets it share the procedural pipeline; inverse modes need their own states.
        if (blend == 1 || blend == 2) return float4(colour.rgb * in.colour.rgb, 0);
        if (blend == 3) return float4(colour.rgb * (beam ? 1.0 : colour.a) * in.colour.rgb, 0);
        if (blend == 4) return float4(colour.rgb * in.colour.rgb, colour.a);
        return float4(colour.rgb * colour.a * in.colour.rgb, colour.a);

    }
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
