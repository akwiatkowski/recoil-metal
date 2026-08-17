#pragma once

// <simd/simd.h> is a system header shipped with the OS, not a framework, so
// core/ stays free of AppKit and Metal and these types remain unit-testable on
// their own. The repo is macOS-only by settled decision (AGENT.md), so reaching
// for the platform's own vector maths beats hand-rolling a Mat4 that would only
// ever be a worse copy of it.
#include <simd/simd.h>

namespace rm {

// A camera that orbits a point on a sphere — the standard RTS/map-inspection
// control scheme, and the one Recoil's own overhead camera approximates.
//
// Angles are radians. yaw rotates about +Y; pitch is elevation above the
// horizontal plane, clamped away from the poles so the up-vector never becomes
// degenerate in the look-at basis.
struct OrbitCamera {
    simd_float3 target = {0.0f, 0.0f, 0.0f};  ///< the orbited point, in elmos
    float distance = 1000.0f;                 ///< eye-to-target distance, elmos
    float yaw = 0.0f;
    float pitch = 0.6f;

    // Vertical field of view. 60 degrees is the usual compromise: wide enough to
    // see a useful slice of terrain, narrow enough that edge distortion stays
    // unobtrusive.
    float fovY = 1.0471976f;  // pi/3

    // Depth range in elmos. A Recoil map is at most 32 768 elmos across, so a
    // 100 000 far plane clears the diagonal of even an 81x81 km map, while a
    // near plane of 1 elmo keeps enough depth precision for terrain at this
    // scale (reversed-Z would buy more, and is a milestone-4 concern).
    float nearZ = 1.0f;
    float farZ = 100000.0f;

    /// Pitch is held inside this band, exclusive of straight up/down.
    static constexpr float kMinPitch = 0.05f;
    static constexpr float kMaxPitch = 1.5533431f;  // ~89 degrees

    static constexpr float kMinDistance = 10.0f;
    static constexpr float kMaxDistance = 200000.0f;

    /// Applies a relative orbit, clamping pitch into the legal band.
    void orbit(float deltaYaw, float deltaPitch) noexcept;

    /// Scales the orbit distance, clamped. factor > 1 moves away.
    void zoom(float factor) noexcept;

    /// Places the camera so an axis-aligned world box comfortably fills the view.
    void frame(simd_float3 boxMin, simd_float3 boxMax) noexcept;

    [[nodiscard]] simd_float3 eye() const noexcept;

    [[nodiscard]] simd_float4x4 viewMatrix() const noexcept;

    /// Perspective projection for Metal's [0, 1] clip-space depth range — note
    /// this is NOT the OpenGL [-1, 1] matrix; using the GL one here would halve
    /// the usable depth buffer and z-fight across the whole map.
    [[nodiscard]] simd_float4x4 projectionMatrix(float aspect) const noexcept;

    [[nodiscard]] simd_float4x4 viewProjection(float aspect) const noexcept;
};

} // namespace rm
