// Camera tests. Matrix bugs are miserable to diagnose on screen — a wrong sign
// just shows a black window — so the depth-range and basis conventions are
// pinned here by arithmetic instead.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/camera/OrbitCamera.hpp"

#include <cmath>

using Catch::Approx;
using rm::OrbitCamera;

namespace {

/// Transform a world-space point by a 4x4, returning the homogeneous result.
[[nodiscard]] simd_float4 transform(const simd_float4x4& m, simd_float3 p) {
    return simd_mul(m, simd_make_float4(p.x, p.y, p.z, 1.0f));
}

} // namespace

TEST_CASE("the eye sits at the orbit distance from the target") {
    OrbitCamera camera;
    camera.target = simd_make_float3(100.0f, 20.0f, -50.0f);
    camera.distance = 750.0f;
    camera.yaw = 1.1f;
    camera.pitch = 0.4f;

    REQUIRE(simd_distance(camera.eye(), camera.target) == Approx(750.0f));
}

TEST_CASE("pitch is clamped away from the poles") {
    OrbitCamera camera;

    camera.orbit(0.0f, 100.0f);
    REQUIRE(camera.pitch == Approx(OrbitCamera::kMaxPitch));

    camera.orbit(0.0f, -100.0f);
    REQUIRE(camera.pitch == Approx(OrbitCamera::kMinPitch));

    // Straight up would collapse the look-at basis: the view direction would be
    // parallel to the up vector and the cross product degenerate.
    REQUIRE(camera.pitch > 0.0f);
    REQUIRE(OrbitCamera::kMaxPitch < 1.5707964f);
}

TEST_CASE("yaw is free to wrap but zoom is clamped") {
    OrbitCamera camera;
    camera.orbit(50.0f, 0.0f);
    REQUIRE(camera.yaw == Approx(50.0f));  // no clamping: orbiting is unbounded

    camera.zoom(1e9f);
    REQUIRE(camera.distance == Approx(OrbitCamera::kMaxDistance));

    camera.zoom(1e-9f);
    REQUIRE(camera.distance == Approx(OrbitCamera::kMinDistance));

    // A non-positive factor is ignored rather than flipping the camera inside out.
    const float before = camera.distance;
    camera.zoom(-2.0f);
    REQUIRE(camera.distance == Approx(before));
}

TEST_CASE("the view matrix puts the target straight ahead down -Z") {
    OrbitCamera camera;
    camera.target = simd_make_float3(500.0f, 0.0f, 500.0f);
    camera.distance = 1200.0f;
    camera.yaw = 0.9f;
    camera.pitch = 0.7f;

    const simd_float4 viewTarget = transform(camera.viewMatrix(), camera.target);

    // In view space the camera is at the origin looking down -Z, so the target
    // lands on the negative Z axis at exactly the orbit distance.
    REQUIRE(viewTarget.x == Approx(0.0f).margin(1e-3f));
    REQUIRE(viewTarget.y == Approx(0.0f).margin(1e-3f));
    REQUIRE(viewTarget.z == Approx(-1200.0f).margin(1e-2f));
    REQUIRE(viewTarget.w == Approx(1.0f));
}

TEST_CASE("the eye maps to the view-space origin") {
    OrbitCamera camera;
    camera.target = simd_make_float3(-30.0f, 12.0f, 88.0f);
    camera.distance = 400.0f;

    const simd_float4 viewEye = transform(camera.viewMatrix(), camera.eye());

    REQUIRE(viewEye.x == Approx(0.0f).margin(1e-3f));
    REQUIRE(viewEye.y == Approx(0.0f).margin(1e-3f));
    REQUIRE(viewEye.z == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("projection maps the near plane to depth 0 and the far plane to 1") {
    // This is the Metal-specific convention. The OpenGL matrix would put the
    // near plane at -1, and everything would still *draw* — just with half the
    // depth buffer wasted and z-fighting across the map.
    OrbitCamera camera;
    camera.nearZ = 1.0f;
    camera.farZ = 1000.0f;

    const simd_float4x4 projection = camera.projectionMatrix(1.0f);

    // View space looks down -Z, so the near plane sits at z = -nearZ.
    const simd_float4 nearClip = transform(projection, simd_make_float3(0.0f, 0.0f, -1.0f));
    REQUIRE(nearClip.w == Approx(1.0f));
    REQUIRE(nearClip.z / nearClip.w == Approx(0.0f).margin(1e-5f));

    const simd_float4 farClip = transform(projection, simd_make_float3(0.0f, 0.0f, -1000.0f));
    REQUIRE(farClip.w == Approx(1000.0f));
    REQUIRE(farClip.z / farClip.w == Approx(1.0f).margin(1e-5f));

    // Depth increases monotonically with distance in between.
    const simd_float4 midClip = transform(projection, simd_make_float3(0.0f, 0.0f, -500.0f));
    const float midDepth = midClip.z / midClip.w;
    REQUIRE(midDepth > 0.0f);
    REQUIRE(midDepth < 1.0f);
}

TEST_CASE("aspect ratio scales X and leaves Y alone") {
    OrbitCamera camera;

    const simd_float4x4 wide = camera.projectionMatrix(2.0f);
    const simd_float4x4 square = camera.projectionMatrix(1.0f);

    // A wider viewport must compress X so the vertical field of view is the
    // quantity held fixed.
    REQUIRE(wide.columns[0].x == Approx(square.columns[0].x * 0.5f));
    REQUIRE(wide.columns[1].y == Approx(square.columns[1].y));
}

TEST_CASE("framing a box centres it and pulls back far enough to see it") {
    OrbitCamera camera;

    const simd_float3 lo = simd_make_float3(0.0f, -100.0f, 0.0f);
    const simd_float3 hi = simd_make_float3(4096.0f, 300.0f, 4096.0f);
    camera.frame(lo, hi);

    REQUIRE(camera.target.x == Approx(2048.0f));
    REQUIRE(camera.target.z == Approx(2048.0f));
    REQUIRE(camera.target.y == Approx(100.0f));

    // Far enough that the box's bounding sphere fits the vertical FOV.
    const float radius = simd_length(hi - lo) * 0.5f;
    REQUIRE(camera.distance > radius);

    // And the whole box must land in front of the camera, not behind it.
    const simd_float4x4 view = camera.viewMatrix();
    REQUIRE(transform(view, lo).z < 0.0f);
    REQUIRE(transform(view, hi).z < 0.0f);
}
