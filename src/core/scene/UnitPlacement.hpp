#pragma once

#include "core/map/HeightField.hpp"
#include "core/map/MapInfo.hpp"
#include "core/scene/TeamColours.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace rm {

// One drawn instance of a model. Uploaded straight to the GPU as per-instance
// data, so the layout is fixed: 12 + 4 + 4 + 16 = 36 bytes, tightly packed.
//
// Rotation is a single yaw because that is all ground units need and all the
// S3O hierarchy can express anyway (the format carries no rotation at all —
// s3o.h:20-22). A full transform belongs here when animation arrives.
//
// The team colour is per instance rather than per draw because that is the
// cheapest thing that lets one instanced draw cover several armies. Both
// engines treat it the same way — as a tint applied through a mask channel the
// texture carries — so this field serves .s3o and .scm alike.
struct UnitInstance {
    std::array<float, 3> position;  ///< world, elmos
    float rotationY;                ///< radians about +Y
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
};

static_assert(sizeof(UnitInstance) == 40,
              "UnitInstance must stay tightly packed — the shader reads it as a "
              "packed_float3, two floats, a packed_float4 and a float");

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
