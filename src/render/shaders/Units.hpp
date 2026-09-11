#pragma once

// Unit and prop shading: team colour, and each content family's own mask convention.
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

inline constexpr const char* kUnits = R"MSL(
// --- Units ------------------------------------------------------------------
// packed_float2 for the UVs, NOT float2: float2's alignment would insert padding
// against the C++ ModelVertex's tightly packed 44 bytes and shear the stream.
struct UnitVertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
    packed_float2 uv2;
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
    float builderYaw;       // authored BuilderArmManipulator yaw, per instance
    float builderPitch;     // authored BuilderArmManipulator pitch, per instance
    float recoil;           // 0 at rest to 1 fully kicked, times recoilDistance
};

// Everything the vertex shader needs to find one instance's pose inside the
// batch's baked keyframe buffer. Per batch and under 4 KB, so it rides in the
// command buffer via setVertexBytes rather than needing a buffer of its own.
struct PoseUniforms {
    uint poseCount;   // 1 when the batch does not animate
    uint boneCount;   // stride, in bones, between consecutive poses
    float duration;   // seconds; 0 when the batch does not animate
    float time;       // the batch clock, in seconds
    uint builderAim;
    float recoilDistance;   // full slide travel, elmos; 0 when the type has no rack
    packed_uint2 padding;
    float4 yawPivot;    // xyz pivot; w unused
    float4 yawAxis;     // xyz unit axis; w unused
    float4 pitchPivot;  // xyz pivot; w unused
    float4 pitchAxis;   // xyz unit axis; w unused
};

// One bone's contribution: a rotation and a translation, no scale. Matches the
// C++ BoneTransform exactly (32 bytes) — a quaternion rather than a matrix, so
// there is no row/column convention to get backwards between the two languages.
struct BoneTransformIn {
    packed_float4 rotation;     // w, x, y, z
    packed_float3 translation;
    uint builderFlags;
};

static float3 rotateBuilderAxis(float3 vector, float3 axis, float angle) {
    const float c = cos(angle);
    const float s = sin(angle);
    return vector * c + cross(axis, vector) * s + axis * dot(axis, vector) * (1.0 - c);
}

static float3 applyBuilderAim(float3 point, BoneTransformIn bone, UnitInstanceIn inst,
                              PoseUniforms p) {
    if (p.builderAim == 0) {
        return point;
    }
    if ((bone.builderFlags & 2u) != 0u) {
        const float3 pivot = p.pitchPivot.xyz;
        point = pivot + rotateBuilderAxis(point - pivot, p.pitchAxis.xyz, inst.builderPitch);
    }
    if ((bone.builderFlags & 1u) != 0u) {
        const float3 pivot = p.yawPivot.xyz;
        point = pivot + rotateBuilderAxis(point - pivot, p.yawAxis.xyz, inst.builderYaw);
    }
    // The recoil slide, AFTER the aim rotations: the barrel travels back along
    // where it is pointing, not where it rested. inst.recoil is 0..1 of the
    // batch's travel; the direction rebuilds the aimed +Z from the same angles.
    if ((bone.builderFlags & 4u) != 0u && inst.recoil > 0.0) {
        const float3 barrel =
            rotateBuilderAxis(rotateBuilderAxis(float3(0.0, 0.0, 1.0), p.pitchAxis.xyz,
                                                inst.builderPitch),
                              p.yawAxis.xyz, inst.builderYaw);
        point -= barrel * (inst.recoil * p.recoilDistance);
    }
    return point;

static float3 applyBuilderAimNormal(float3 normal, BoneTransformIn bone, UnitInstanceIn inst,
                                    PoseUniforms p) {
    if (p.builderAim == 0) {
        return normal;
    }
    if ((bone.builderFlags & 2u) != 0u) {
        normal = rotateBuilderAxis(normal, p.pitchAxis.xyz, inst.builderPitch);
    }
    if ((bone.builderFlags & 1u) != 0u) {
        normal = rotateBuilderAxis(normal, p.yawAxis.xyz, inst.builderYaw);
    }
    return normal;
}

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
    float2 uv2;
    float3 world;             // for the view vector the specular term needs
    float4 teamColour [[flat]];  // per instance, so never interpolated
};

// Recoil's own defaults for model specular, from mapinfo.lua's `lighting` table
// (rts/Map/MapInfo.cpp:215-221). The ambient and diffuse constants that used to
// sit beside these are gone: the diffuse light now comes from the same
// map-authored terms the terrain reads (`u.sunColour` and friends), which is
// how both engines light their models — see unitFragment.
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
    const float3 local = applyBuilderAim(
        rotateBy(boneRotation, float3(v.position)) + float3(bone.translation), bone, inst, p);

    // Roll (Z), then pitch (X), then yaw (Y). The C++ side computes pitch/roll
    // in the unit's local frame (after undoing yaw) so that the model's up axis
    // aligns with the terrain normal under its feet, whatever direction the
    // unit is facing.
    const float3 world = unitOrient(local, inst) * inst.scale + float3(inst.position);

    // The normal takes the bone's rotation but not its translation.
    const float3 spunNormal = unitOrient(
        applyBuilderAimNormal(rotateBy(boneRotation, float3(v.normal)), bone, inst, p), inst);

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
    out.uv2 = float2(v.uv2.x, 1.0 - v.uv2.y);
    return out;
}

)MSL";

} // namespace rm::shaders
