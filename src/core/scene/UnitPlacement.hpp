#pragma once

#include "core/map/HeightField.hpp"
#include "core/map/MapInfo.hpp"
#include "core/scene/TeamColours.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rm {

struct BuilderAimRig;

// One drawn instance of a model. Uploaded straight to the GPU as per-instance
// data, so the layout is fixed and asserted below.
//
// Body rotation is yaw plus terrain pitch/roll. Builder-arm yaw/pitch are separate: they
// rotate only authored bone subtrees in the shader and may differ between instances in one draw.
//
// The team colour is per instance rather than per draw because that is the
// cheapest thing that lets one instanced draw cover several armies. Both
// engines treat it the same way — as a tint applied through a mask channel the
// texture carries — so this field serves .s3o and .scm alike.
struct UnitInstance {
    std::array<float, 3> position;  ///< world, elmos
    float rotationY;                ///< radians about +Y (yaw)
    float scale;
    TeamColour teamColour = kTeamColours[0];

    // Where this instance is in its animation, in CYCLES rather than seconds —
    // the shader adds it to the batch's clock and keeps the fractional part, so
    // any value is legal and 0.25 means "a quarter of the way in" whatever the
    // animation's duration turns out to be.
    //
    // Per instance because otherwise a batch shares one clock and a squad walks
    // in perfect lockstep, which reads as a single unit smeared across the map
    // rather than as several units. It costs four bytes and no per-frame work:
    // the offset is constant, and the clock it is added to already advances on
    // its own.
    float animationPhase = 0.0f;

    // Slope alignment: pitch (rotation about +X) and roll (rotation about +Z).
    // These tilt the model to match the terrain under its feet. They are
    // derived from the terrain normal and the unit's yaw, so a unit facing any
    // direction plants both feet on the same slope.
    float rotationX = 0.0f;
    float rotationZ = 0.0f;

    /// Per-instance `BuilderArmManipulator` pose, radians around the batch's resolved yaw and
    /// pitch axes. Zero leaves every bone in its ordinary animation pose.
    float builderYaw = 0.0f;
    float builderPitch = 0.0f;

    /// Per-instance TURRET pose, radians around the batch's resolved ring and
    /// trunnion axes — separate fields from the builder's because a unit can carry
    /// both (the ACU does), and the subtrees are disjoint flag bits in the bone
    /// buffer. The SIM's `MoveState::turretYaw`/`turretPitch` projected to float:
    /// presentation slews nothing of its own, which is what keeps the drawn
    /// barrel, the muzzle flash and the shot's launch on one line.
    float turretYaw = 0.0f;
    float turretPitch = 0.0f;

    /// The dual manipulator's own aim — `MoveState::turretYaw2`/`turretPitch2`:
    /// the second arm yaws AND pitches about its own trunnion on top of the ring,
    /// which is the only way two splayed barrels both reach the target.
    float turretYaw2 = 0.0f;
    float turretPitch2 = 0.0f;

    /// Per-instance recoil slide, 0 at rest to 1 fully kicked. Multiplied by the
    /// batch's travel distance in the vertex shader; zero leaves every bone put.
    float recoil = 0.0f;

    /// CPU measurement of the current drawn barrel versus a world-space direction
    /// (e.g. initial projectile velocity): 0 degrees aligned, 180 reversed.
    /// Supply independently identified base/tip points on the SAME barrel bone,
    /// in model space AFTER animation but BEFORE turret and instance transforms.
    /// Does not call the aim solver or sample animation/GPU output. Shared recoil
    /// translation cancels from the axis. Call on the instance gathered for the
    /// frame being tested, and REQUIRE a value before asserting a tolerance.
    /// Returns nullopt for missing bones, degenerate or non-finite geometry.
    [[nodiscard]] std::optional<float> barrelAlignmentErrorDegrees(
        const BuilderAimRig& rig, std::size_t barrelBone,
        const std::array<float, 3>& barrelBase, const std::array<float, 3>& barrelTip,
        const std::array<float, 3>& worldDirection) const noexcept;
};

static_assert(sizeof(UnitInstance) == 76,
              "UnitInstance must stay tightly packed — the shader reads it as a "
              "packed_float3, two floats, a packed_float4 and ten floats");

// Scatters instances across the map's land, sitting on the terrain.
//
// Deterministic for a given seed: a benchmark that changes its scene between
// runs is not a benchmark, and a screenshot that changes every launch is hard to
// compare. Uses a seeded generator rather than any global RNG.
//
// Only positions at or above `minHeight` are accepted, which defaults to the
// water plane Recoil fixes at y = 0 (rts/Map/Ground.h:32) — otherwise most of a
// map like Angel Crossing spawns units underwater. Candidates below it are
// resampled, so a nearly-submerged map yields fewer instances than requested
// rather than looping forever; the count actually placed is what is returned.
[[nodiscard]] std::vector<UnitInstance> scatterOnLand(const HeightField& field,
                                                      std::size_t count, std::uint32_t seed,
                                                      float scale = 1.0f,
                                                      float minHeight = 0.0f);

// One instance per team start position, dropped onto the terrain.
//
// Y comes from sampling the heightmap because mapinfo.lua gives only X and Z —
// which is also what the engine does.
[[nodiscard]] std::vector<UnitInstance> atStartPositions(
    const HeightField& field, std::span<const mapinfo::StartPosition> positions,
    float scale = 1.0f);

} // namespace rm
