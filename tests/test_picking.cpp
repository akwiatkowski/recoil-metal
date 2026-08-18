// Picking tests — turning a click into a place on the map.
//
// Written to be convention-independent wherever possible: rather than asserting
// literal coordinates that would only re-state whatever the implementation
// happens to do, these check relationships that must hold for picking to feel
// correct — the centre of the screen picks what the camera is looking at, the
// right half of the screen picks to the right, and a ray aimed at a unit picks
// that unit and not its neighbour.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/camera/OrbitCamera.hpp"
#include "core/map/HeightField.hpp"
#include "core/scene/Picking.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using Catch::Approx;
using rm::HeightField;
using rm::OrbitCamera;
using rm::UnitInstance;

namespace {

constexpr float kViewWidth = 1600.0f;
constexpr float kViewHeight = 1000.0f;

/// A flat 800x800-elmo field at y = 0.
[[nodiscard]] HeightField flatField() {
    HeightField field;
    field.squaresX = 100;
    field.squaresZ = 100;
    field.baseHeight = 0.0f;
    field.heightScale = 1.0f;
    field.raw.assign(field.sampleCount(), std::uint16_t{0});
    return field;
}

/// The same field ramping to y = 500 along +X.
[[nodiscard]] HeightField rampField() {
    HeightField field = flatField();
    field.heightScale = 1.0f;
    for (int z = 0; z < field.verticesZ(); ++z) {
        for (int x = 0; x < field.verticesX(); ++x) {
            const auto index = static_cast<std::size_t>(z)
                                 * static_cast<std::size_t>(field.verticesX())
                             + static_cast<std::size_t>(x);
            field.raw[index] = static_cast<std::uint16_t>(x * 5);
        }
    }
    return field;
}

/// A camera looking down at the middle of the map from a normal working angle.
[[nodiscard]] OrbitCamera overheadCamera() {
    OrbitCamera camera;
    camera.target = simd_make_float3(400.0f, 0.0f, 400.0f);
    camera.distance = 900.0f;
    camera.yaw = 0.4f;
    camera.pitch = 0.9f;
    return camera;
}

/// The centre of the viewport.
constexpr float kCentreX = kViewWidth * 0.5f;
constexpr float kCentreY = kViewHeight * 0.5f;

[[nodiscard]] UnitInstance unitAt(float x, float y, float z) {
    UnitInstance instance{};
    instance.position = {{x, y, z}};
    instance.scale = 1.0f;
    return instance;
}

} // namespace

TEST_CASE("the centre pixel's ray runs from the eye toward the camera target") {
    const OrbitCamera camera = overheadCamera();
    const rm::Ray ray = rm::screenRay(camera, kCentreX, kCentreY, kViewWidth, kViewHeight);

    // Every ray starts at the centre of projection.
    CHECK(simd_distance(ray.origin, camera.eye()) == Approx(0.0f).margin(1e-3));

    // And the middle of the screen is, by definition, where the camera looks.
    const simd_float3 toTarget = simd_normalize(camera.target - camera.eye());
    CHECK(simd_dot(ray.direction, toTarget) == Approx(1.0f).margin(1e-4));

    // Normalised, so callers can treat the parameter as a distance in elmos.
    CHECK(simd_length(ray.direction) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("rays point where the pixel is") {
    const OrbitCamera camera = overheadCamera();
    const simd_float3 forward = simd_normalize(camera.target - camera.eye());
    const simd_float3 right = simd_normalize(simd_cross(forward, simd_make_float3(0, 1, 0)));
    const simd_float3 up = simd_cross(right, forward);

    const rm::Ray centre = rm::screenRay(camera, kCentreX, kCentreY, kViewWidth, kViewHeight);

    SECTION("a pixel to the right leans right") {
        const rm::Ray ray =
            rm::screenRay(camera, kViewWidth * 0.75f, kCentreY, kViewWidth, kViewHeight);
        CHECK(simd_dot(ray.direction, right) > simd_dot(centre.direction, right));
        CHECK(simd_dot(ray.direction, right) > 0.0f);
    }

    SECTION("a pixel above the centre leans up") {
        // AppKit's view coordinates put the origin at the bottom-left, so a
        // larger y is higher on screen. Getting this backwards renders as
        // picking that works but is mirrored vertically, which is maddening
        // rather than obvious.
        const rm::Ray ray =
            rm::screenRay(camera, kCentreX, kViewHeight * 0.75f, kViewWidth, kViewHeight);
        CHECK(simd_dot(ray.direction, up) > simd_dot(centre.direction, up));
        CHECK(simd_dot(ray.direction, up) > 0.0f);
    }

    SECTION("opposite corners lean opposite ways") {
        const rm::Ray a = rm::screenRay(camera, 0.0f, 0.0f, kViewWidth, kViewHeight);
        const rm::Ray b = rm::screenRay(camera, kViewWidth, kViewHeight, kViewWidth, kViewHeight);
        CHECK(simd_dot(a.direction, right) < 0.0f);
        CHECK(simd_dot(b.direction, right) > 0.0f);
        CHECK(simd_dot(a.direction, up) < 0.0f);
        CHECK(simd_dot(b.direction, up) > 0.0f);
    }
}

TEST_CASE("the centre pixel picks the ground the camera is looking at") {
    const HeightField field = flatField();
    const OrbitCamera camera = overheadCamera();
    const rm::Ray ray = rm::screenRay(camera, kCentreX, kCentreY, kViewWidth, kViewHeight);

    const std::optional<simd_float3> hit = rm::pickGround(ray, field);

    REQUIRE(hit.has_value());
    // The target sits on the flat ground, so this is the one pick whose answer
    // is known exactly. A whole elmo of tolerance on an 800-elmo map is the
    // marching step's business, not a fudge.
    CHECK(hit->x == Approx(camera.target.x).margin(1.0));
    CHECK(hit->z == Approx(camera.target.z).margin(1.0));
    CHECK(hit->y == Approx(0.0f).margin(0.5));
}

TEST_CASE("a picked point lies on the terrain, not merely near it") {
    const HeightField field = rampField();
    const OrbitCamera camera = overheadCamera();

    // Sweep the viewport rather than trusting one pixel: a marcher that steps
    // too coarsely passes at the centre and fails at a grazing angle.
    for (int i = 1; i < 10; ++i) {
        const float x = kViewWidth * static_cast<float>(i) / 10.0f;
        for (int j = 1; j < 10; ++j) {
            const float y = kViewHeight * static_cast<float>(j) / 10.0f;
            const rm::Ray ray = rm::screenRay(camera, x, y, kViewWidth, kViewHeight);
            const std::optional<simd_float3> hit = rm::pickGround(ray, field);
            if (!hit.has_value()) {
                continue;  // looking past the map edge is a legitimate miss
            }
            REQUIRE(hit->y == Approx(field.heightAtWorld(hit->x, hit->z)).margin(1.0));
        }
    }
}

TEST_CASE("a ray pointing away from the terrain picks nothing") {
    const HeightField field = flatField();

    const rm::Ray skyward{.origin = simd_make_float3(400.0f, 100.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, 1.0f, 0.0f)};
    CHECK_FALSE(rm::pickGround(skyward, field).has_value());

    // Level and above the terrain: never descends, so it never crosses.
    const rm::Ray level{.origin = simd_make_float3(400.0f, 100.0f, 400.0f),
                        .direction = simd_make_float3(1.0f, 0.0f, 0.0f)};
    CHECK_FALSE(rm::pickGround(level, field).has_value());
}

TEST_CASE("a ray straight down picks the ground beneath it") {
    const HeightField field = rampField();
    const rm::Ray down{.origin = simd_make_float3(300.0f, 2000.0f, 500.0f),
                       .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};

    const std::optional<simd_float3> hit = rm::pickGround(down, field);

    REQUIRE(hit.has_value());
    CHECK(hit->x == Approx(300.0f).margin(0.1));
    CHECK(hit->z == Approx(500.0f).margin(0.1));
    CHECK(hit->y == Approx(field.heightAtWorld(300.0f, 500.0f)).margin(0.5));
}

TEST_CASE("picking a unit") {
    const std::vector<UnitInstance> units{
        unitAt(100.0f, 0.0f, 100.0f),
        unitAt(400.0f, 0.0f, 400.0f),
        unitAt(700.0f, 0.0f, 100.0f),
    };
    constexpr float kRadius = 40.0f;

    SECTION("a ray through a unit picks it") {
        const rm::Ray ray{.origin = simd_make_float3(400.0f, 500.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
        const std::optional<std::size_t> hit = rm::pickUnit(ray, units, kRadius);
        REQUIRE(hit.has_value());
        CHECK(*hit == 1);
    }

    SECTION("a ray through empty ground picks nothing") {
        const rm::Ray ray{.origin = simd_make_float3(250.0f, 500.0f, 250.0f),
                          .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
        CHECK_FALSE(rm::pickUnit(ray, units, kRadius).has_value());
    }

    SECTION("a near miss inside the radius still picks") {
        const rm::Ray ray{.origin = simd_make_float3(420.0f, 500.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
        const std::optional<std::size_t> hit = rm::pickUnit(ray, units, kRadius);
        REQUIRE(hit.has_value());
        CHECK(*hit == 1);
    }

    SECTION("with two candidates the closer to the ray wins") {
        const std::vector<UnitInstance> pair{
            unitAt(400.0f, 0.0f, 400.0f),
            unitAt(430.0f, 0.0f, 400.0f),
        };
        const rm::Ray ray{.origin = simd_make_float3(425.0f, 500.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
        const std::optional<std::size_t> hit = rm::pickUnit(ray, pair, 50.0f);
        REQUIRE(hit.has_value());
        CHECK(*hit == 1);
    }

    SECTION("units behind the camera are not picked") {
        // Same alignment, opposite direction: a ray must not select something
        // it is pointing away from, or clicking the sky selects whatever is
        // behind the viewer.
        const rm::Ray ray{.origin = simd_make_float3(400.0f, 500.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, 1.0f, 0.0f)};
        CHECK_FALSE(rm::pickUnit(ray, units, kRadius).has_value());
    }

    SECTION("no units means no pick") {
        const rm::Ray ray{.origin = simd_make_float3(400.0f, 500.0f, 400.0f),
                          .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
        CHECK_FALSE(rm::pickUnit(ray, {}, kRadius).has_value());
    }
}

TEST_CASE("distanceToRay measures perpendicular distance, and rejects what is behind") {
    const rm::Ray down{.origin = simd_make_float3(400.0f, 500.0f, 400.0f),
                       .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};

    // Directly below the origin: on the ray, so no distance at all.
    CHECK(rm::distanceToRay(down, simd_make_float3(400.0f, 0.0f, 400.0f))
          == Approx(0.0f).margin(1e-4));

    // Offset in X only, so the distance is that offset.
    CHECK(rm::distanceToRay(down, simd_make_float3(430.0f, 0.0f, 400.0f)) == Approx(30.0f));

    // Behind the origin — above a downward ray — loses every comparison.
    CHECK(std::isinf(rm::distanceToRay(down, simd_make_float3(400.0f, 900.0f, 400.0f))));
}

TEST_CASE("picking is safe on a field with no samples") {
    const HeightField empty;
    const rm::Ray down{.origin = simd_make_float3(0.0f, 100.0f, 0.0f),
                       .direction = simd_make_float3(0.0f, -1.0f, 0.0f)};
    CHECK_FALSE(rm::pickGround(down, empty).has_value());
}

TEST_CASE("a degenerate viewport does not divide by zero") {
    const OrbitCamera camera = overheadCamera();
    // A window being resized to nothing, or a screenshot at zero height.
    const rm::Ray ray = rm::screenRay(camera, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(std::isfinite(ray.direction.x));
    CHECK(std::isfinite(ray.direction.y));
    CHECK(std::isfinite(ray.direction.z));
}
