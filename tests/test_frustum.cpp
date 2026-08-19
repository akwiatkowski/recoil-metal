// Frustum tests. Culling is the one optimisation that is invisible when it
// works and looks like missing geometry when it does not, so the box test is
// pinned here rather than judged on screen.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/camera/Frustum.hpp"
#include "core/camera/OrbitCamera.hpp"

using Catch::Approx;
using rm::OrbitCamera;

namespace {

constexpr float kAspect = 16.0f / 9.0f;

/// A camera looking at the origin from 500 elmos up and back.
[[nodiscard]] OrbitCamera camera() {
    OrbitCamera c;
    c.target = simd_make_float3(0.0f, 0.0f, 0.0f);
    c.distance = 500.0f;
    c.yaw = 0.0f;
    c.pitch = 0.7f;
    return c;
}

} // namespace

TEST_CASE("what the camera is pointed at is inside the frustum") {
    const rm::Frustum frustum = rm::frustumOf(camera().viewProjection(kAspect));

    // A box around the target — the one place that must never be culled.
    CHECK(frustum.intersectsBox(simd_make_float3(-50, -50, -50),
                                simd_make_float3(50, 50, 50)));
}

TEST_CASE("what is behind the camera is outside it") {
    const OrbitCamera c = camera();
    const rm::Frustum frustum = rm::frustumOf(c.viewProjection(kAspect));

    // Twice the orbit distance the other side of the eye: unambiguously behind.
    const simd_float3 behind = c.eye() + (c.eye() - c.target) * 2.0f;
    CHECK_FALSE(frustum.intersectsBox(behind - 20.0f, behind + 20.0f));
}

TEST_CASE("what is far off to the side is outside it") {
    const rm::Frustum frustum = rm::frustumOf(camera().viewProjection(kAspect));

    // Well beyond any sane field of view at this distance.
    CHECK_FALSE(frustum.intersectsBox(simd_make_float3(20000, -50, -50),
                                      simd_make_float3(20100, 50, 50)));
    CHECK_FALSE(frustum.intersectsBox(simd_make_float3(-20100, -50, -50),
                                      simd_make_float3(-20000, 50, 50)));
}

TEST_CASE("a box that merely straddles a plane is kept") {
    // The direction that matters. A test that culls anything not wholly inside
    // clips geometry at the screen edge, which is the classic frustum bug and
    // is only visible when the camera moves.
    const rm::Frustum frustum = rm::frustumOf(camera().viewProjection(kAspect));

    // Enormous box centred well off to one side but reaching back over the
    // target: partly outside, so it must survive.
    CHECK(frustum.intersectsBox(simd_make_float3(-30000, -100, -100),
                                simd_make_float3(0, 100, 100)));
}

TEST_CASE("a box beyond the far plane is outside it") {
    OrbitCamera c = camera();
    c.farZ = 1000.0f;
    const rm::Frustum frustum = rm::frustumOf(c.viewProjection(kAspect));

    // Along the view direction, past the far plane.
    const simd_float3 forward = simd_normalize(c.target - c.eye());
    const simd_float3 tooFar = c.eye() + forward * 5000.0f;
    CHECK_FALSE(frustum.intersectsBox(tooFar - 10.0f, tooFar + 10.0f));
}

TEST_CASE("a degenerate box is handled as a point") {
    const rm::Frustum frustum = rm::frustumOf(camera().viewProjection(kAspect));

    const simd_float3 origin = simd_make_float3(0, 0, 0);
    CHECK(frustum.intersectsBox(origin, origin));
}
