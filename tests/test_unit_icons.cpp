// When a unit stops being worth drawing as a unit.
//
// Pure geometry, so tested here rather than judged by squinting at a screenshot — which is
// exactly what this feature exists to make unnecessary.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/scene/UnitIcons.hpp"

#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] rm::UnitInstance unitAt(float x, float z, rm::TeamColour colour) {
    rm::UnitInstance instance{};
    instance.position = {x, 0.0f, z};
    instance.scale = 1.0f;
    instance.teamColour = colour;
    return instance;
}

} // namespace

TEST_CASE("apparent size is the diameter, not the radius") {
    // The thing one sees is how wide it looks. Using the radius would make every unit read
    // as half its size and put the icon threshold twice as far out as intended.
    CHECK(rm::apparentPoints(4.0f, 1.0f) == Approx(8.0f));
    CHECK(rm::apparentPoints(4.0f, 2.0f) == Approx(4.0f));

    // A degenerate scale is zero rather than an infinity: a camera mid-construction can
    // report one, and an inf would size an icon to cover the map.
    CHECK(rm::apparentPoints(4.0f, 0.0f) == Approx(0.0f));
}

TEST_CASE("a unit gets an icon only once it is too small to read") {
    const std::vector<rm::UnitInstance> units{unitAt(100.0f, 100.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};  // a medium tank, near enough

    // Zoomed IN: one elmo per point makes the tank 8 points across, which is exactly the
    // threshold — and the threshold is exclusive, so the mesh still speaks for itself.
    std::vector<rm::Particle> icons;
    CHECK(rm::appendUnitIcons(icons, units, radii, 1.0f) == 0);
    CHECK(icons.empty());

    // Zoomed OUT: ten elmos per point makes it 0.8 points, which is nothing.
    CHECK(rm::appendUnitIcons(icons, units, radii, 10.0f) == 1);
    REQUIRE(icons.size() == 1);
}

TEST_CASE("an icon is the same size on screen however far away it is") {
    // The whole point of an icon. A size in elmos that did not scale with the camera would
    // shrink away with the unit it replaced.
    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};

    std::vector<rm::Particle> near;
    std::vector<rm::Particle> far;
    REQUIRE(rm::appendUnitIcons(near, units, radii, 10.0f) == 1);
    REQUIRE(rm::appendUnitIcons(far, units, radii, 100.0f) == 1);

    // Ten times further out, ten times bigger in elmos — so identical in points.
    CHECK(far.front().size == Approx(near.front().size * 10.0f));
    CHECK(near.front().size / 10.0f == Approx(rm::kIconSizePoints));
    CHECK(far.front().size / 100.0f == Approx(rm::kIconSizePoints));
}

TEST_CASE("an icon wears its army's colour, premultiplied") {
    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[1])};
    const std::vector<float> radii{4.0f};

    std::vector<rm::Particle> icons;
    REQUIRE(rm::appendUnitIcons(icons, units, radii, 50.0f) == 1);

    // Premultiplied, which is what the particle pipeline takes (ADR-025): rgb carries the
    // alpha already. Read as straight alpha an icon would be too bright and the wrong hue.
    const float alpha = icons.front().colour[3];
    CHECK(alpha < 1.0f);  // not solid: a block would hide the ground it stands on
    CHECK(icons.front().colour[0] == Approx(rm::kTeamColours[1][0] * alpha));
    CHECK(icons.front().colour[1] == Approx(rm::kTeamColours[1][1] * alpha));
    CHECK(icons.front().colour[2] == Approx(rm::kTeamColours[1][2] * alpha));
}

TEST_CASE("an icon floats clear of the ground it stands on") {
    // The failure that cost the selection rings an elmo of lift: anything depth-tested at
    // exactly ground height loses to the terrain, silently and totally.
    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};

    std::vector<rm::Particle> icons;
    REQUIRE(rm::appendUnitIcons(icons, units, radii, 50.0f) == 1);
    CHECK(icons.front().origin[1] == Approx(rm::kIconLiftElmos));
}

TEST_CASE("an icon does not move, and does not age away") {
    const std::vector<rm::UnitInstance> units{unitAt(500.0f, 700.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};

    std::vector<rm::Particle> icons;
    REQUIRE(rm::appendUnitIcons(icons, units, radii, 50.0f) == 1);

    // Stationary and unborn: the list is rebuilt every frame from the units' live
    // positions, so an icon that drifted or faded would be showing the past.
    CHECK(icons.front().velocity[0] == Approx(0.0f));
    CHECK(icons.front().velocity[1] == Approx(0.0f));
    CHECK(icons.front().velocity[2] == Approx(0.0f));
    CHECK(icons.front().growth == Approx(0.0f));

    // BORN ALREADY GROWN UP, and this is the whole reason the constant exists: the particle
    // shader fades a particle IN over the first 15% of its life, so an icon born at age
    // zero is perfectly transparent — and one rebuilt every frame is therefore never
    // visible at all. Cost a screenshot session to find, because a fully transparent quad
    // is indistinguishable from a pass that never ran.
    CHECK(icons.front().age > 0.15f * icons.front().lifetime);
    CHECK(icons.front().age < icons.front().lifetime);

    CHECK(icons.front().origin[0] == Approx(500.0f));
    CHECK(icons.front().origin[2] == Approx(700.0f));
}

TEST_CASE("a retired corpse gets no icon") {
    // `retireDead` marks a dead unit with a zero radius. An icon for it would leave a
    // marker where a unit used to be, which on a battlefield is the worst possible thing to
    // draw: it reads as a live enemy.
    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[0]),
                                              unitAt(50.0f, 0.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{0.0f, 4.0f};  // the first is dead

    std::vector<rm::Particle> icons;
    CHECK(rm::appendUnitIcons(icons, units, radii, 50.0f) == 1);
    REQUIRE(icons.size() == 1);
    CHECK(icons.front().origin[0] == Approx(50.0f));  // the living one
}

TEST_CASE("icons are appended, so dust and icons share one draw") {
    // They ride the same pipeline, which is the reason this needed no new pass. A function
    // that CLEARED its output would throw away the frame's dust.
    std::vector<rm::Particle> particles;
    particles.push_back(rm::Particle{.origin = {1.0f, 2.0f, 3.0f}});

    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};
    CHECK(rm::appendUnitIcons(particles, units, radii, 50.0f) == 1);

    REQUIRE(particles.size() == 2);
    CHECK(particles.front().origin[0] == Approx(1.0f));  // the dust survived
}

TEST_CASE("a radius list shorter than the instances is not a crash") {
    // Parallel arrays that disagree are a caller bug; reading past the short one would be a
    // far worse answer than treating the missing entries as having no radius.
    const std::vector<rm::UnitInstance> units{unitAt(0.0f, 0.0f, rm::kTeamColours[0]),
                                              unitAt(10.0f, 0.0f, rm::kTeamColours[0])};
    const std::vector<float> radii{4.0f};

    std::vector<rm::Particle> icons;
    CHECK(rm::appendUnitIcons(icons, units, radii, 50.0f) == 1);
}
