#pragma once

#include "core/map/HeightField.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace rm {

// A ground decal: coloured geometry laid over the terrain, following its shape.
//
// Two kinds so far — the ring under a selected unit, and the marker where an
// order was given — and they share everything but their outline. Both conform to
// the ground rather than being flat quads tilted by a surface normal, both ride
// the same buffer, and both are drawn in one call, which is why the colour is per
// vertex rather than per draw.
//
// One vertex, laid out for direct upload with no repacking.
//
// The colour rides per vertex rather than per draw so that one buffer covers a
// whole mixed selection — several teams, or a unit highlighted differently from
// the rest — in a single draw call. Twelve bytes a vertex is a poor trade in the
// abstract and a good one here, because a selection is tens of units and the
// alternative is a draw call each.
struct DecalVertex {
    std::array<float, 3> position;  ///< world elmos, already lifted clear of the ground
    std::array<float, 4> colour;    ///< straight (non-premultiplied) rgba
};

static_assert(sizeof(DecalVertex) == 28,
              "DecalVertex must stay tightly packed — the shader reads it as a "
              "packed_float3 and a packed_float4");

/// Segments around a selection ring.
///
/// 32 is a ring whose facets are under a pixel at the closest an RTS camera
/// gets, and 192 vertices — small enough that a hundred of them is a megabyte.
inline constexpr int kRingSegments = 32;

/// How thick the drawn band is, in elmos.
///
/// A constant width rather than a fraction of the radius: the ring is a piece of
/// interface, and interface should not get fatter because the unit is bigger.
inline constexpr float kRingThicknessElmos = 2.5f;

/// How far the ring floats above the ground it samples, in elmos.
///
/// It exists to lose a depth fight, not to be seen: the ring samples the same
/// heightfield the terrain is built from, so without a lift the two surfaces are
/// coplanar and z-fighting speckles the band. Small enough that it does not read
/// as hovering, and it is drawn without writing depth so nothing hides behind it.
inline constexpr float kRingLiftElmos = 1.5f;

// Appends one ring, as a triangle list, following the ground under it.
//
// Conforming rather than a flat disc tilted by the terrain normal, which is the
// cheaper and more common approach. A ring is 8 to 40 elmos across and a
// heightfield square is 8, so a flat ring spans several squares of real relief
// and buries a third of itself in any slope that is not planar — exactly the
// broken ground where knowing what is selected matters most.
//
// `centre` is the unit's world position; only x and z are read, since the ring's
// height comes from the ground at each point around it rather than from the
// unit. That is what keeps a ring on a hillside level with the hill instead of
// with the unit standing on it.
void appendSelectionRing(std::vector<DecalVertex>& out, const HeightField& field,
                         std::array<float, 3> centre, float radiusElmos,
                         std::array<float, 4> colour,
                         float thicknessElmos = kRingThicknessElmos,
                         int segments = kRingSegments);

/// How wide the marker at a move order is drawn, in elmos.
///
/// 14 is a little under two heightmap squares — big enough to find on a map at a
/// working zoom, small enough that it does not claim ground the order did not.
inline constexpr float kOrderMarkerRadiusElmos = 14.0f;

/// How long a marker stays, in seconds.
///
/// It answers "did that click land, and where" and then stops being useful. A
/// marker that outlived the walk would leave the map littered with places units
/// have already been, and a permanent one would need a way to be dismissed.
inline constexpr float kOrderMarkerSecondsToLive = 1.6f;

// Appends a marker where an order was given: a ring, and a cross through it.
//
// The cross is the point. A ring alone is what a SELECTION looks like, and the two
// appear within a second of each other on the same ground — a right-click follows
// a left-click — so two rings of different colours would be one distinction too
// subtle in the one place the player is looking. Different shape, not just
// different hue, and it survives being colour-blind or being small.
//
// `age` is how long the marker has existed, in seconds. It shrinks and fades over
// kOrderMarkerSecondsToLive, and past that contributes nothing at all — a caller
// can keep asking without checking, which is what lets the app hold one marker per
// order and let them expire on their own.
void appendOrderMarker(std::vector<DecalVertex>& out, const HeightField& field,
                       std::array<float, 3> centre, std::array<float, 4> colour, float age,
                       float radiusElmos = kOrderMarkerRadiusElmos);

/// Vertices one ring contributes: two triangles per segment.
[[nodiscard]] constexpr std::size_t ringVertexCount(int segments = kRingSegments) noexcept {
    return static_cast<std::size_t>(segments) * 6u;
}

} // namespace rm
