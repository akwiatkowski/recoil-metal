#pragma once

#include "core/scene/Particles.hpp"
#include "core/scene/UnitPlacement.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm {

// What a unit looks like when it is too far away to look like anything.
//
// Supreme Commander's signature view: zoom out and the models give way to flat symbols, so
// a 20-kilometre map is readable at a glance instead of dissolving into speckle. This is
// the same idea at its simplest — a camera-facing coloured square in the unit's army
// colour, drawn where the mesh has shrunk past the point of conveying anything.
//
// NO NEW RENDER PASS. An icon is a stationary, non-ageing, camera-facing quad, and that is
// precisely what a Particle already is: `origin` is the unit, `velocity` is zero, `age` is
// zero against a lifetime it never reaches, and the quad is expanded from the vertex id in
// the shader (ADR-025). So icons ride the particle pipeline and cost one more buffer's
// worth of instances rather than a pipeline, a shader and a pass.

/// Below this many points across, a unit is drawn as an icon.
///
/// Eight points. A model smaller than this is a handful of pixels in which no silhouette
/// survives — the geometry is still drawn, and still correct, and conveys nothing. Above
/// it, the mesh reads and an icon would be a square pasted over a recognisable unit.
inline constexpr float kIconThresholdPoints = 8.0f;

/// How big an icon is drawn, in points. Slightly larger than the threshold, so a unit
/// crossing it grows a little rather than popping from 8 points to 3.
inline constexpr float kIconSizePoints = 11.0f;

/// The viewport height the point sizes above are measured against.
///
/// The absolute value does not matter and cancels: it appears in `elmosPerPoint` and again
/// when a size in points is multiplied back into elmos. What matters is that ONE value is
/// used for both, so a threshold in points means the same thing everywhere. 1000 is a
/// plausible window.
inline constexpr float kIconReferenceHeightPoints = 1000.0f;

/// How long an icon claims to live, in seconds.
///
/// It is rebuilt every frame from the units' live positions, so this only has to outlast
/// the frame it is drawn in. What it really controls is the shader's fade curve, which is
/// expressed as fractions of a lifetime.
inline constexpr float kIconLifetimeSeconds = 1.0f;

/// How old an icon is born, in seconds, as a fraction of its lifetime.
///
/// THE PARTICLE SHADER FADES A PARTICLE IN: `fadeIn = saturate(age / (0.15 * lifetime))`,
/// so that a puff of dust rises into view rather than popping. At an age of zero that
/// factor is exactly ZERO — a particle born this instant is perfectly transparent, which
/// for a rebuilt-every-frame icon means one that is never visible at all.
///
/// So an icon is born already grown up. A fifth of its lifetime is past the 0.15 the
/// fade-in spans, and far enough from the far end that the shader's `remaining²` decay has
/// taken almost nothing off it.
///
/// Cost me a screenshot session to find, because every plausible explanation — size, depth
/// against the terrain, the pass being skipped in a capture — looks identical to a fully
/// transparent quad.
inline constexpr float kIconBirthAgeFraction = 0.2f;

/// How far above the ground an icon floats, in elmos.
///
/// Half a heightmap square. Enough that a icon is not eaten by the terrain it stands on —
/// the same failure that cost the selection rings an elmo of lift (AGENT.md) — and little
/// enough that it still reads as being AT the unit.
inline constexpr float kIconLiftElmos = 4.0f;

/// The apparent size of something `radiusElmos` across, in points, from `elmosPerPoint`.
[[nodiscard]] float apparentPoints(float radiusElmos, float elmosPerPoint) noexcept;

/// Appends an icon for every unit too small to read, and returns how many.
///
/// `elmosPerPoint` comes from the camera (OrbitCamera::elmosPerPoint), measured against
/// `kIconReferenceHeightPoints`, so this needs no viewport and no projection matrix — which
/// is what keeps it in core/ and testable.
///
/// A unit with a zero radius gets no icon: that is how a retired corpse is marked
/// (`retireDead`), and an icon for it would leave a marker where a unit used to be, which
/// is exactly the wrong thing to draw on a battlefield.
std::size_t appendUnitIcons(std::vector<Particle>& into,
                            std::span<const UnitInstance> instances,
                            std::span<const float> radiiElmos, float elmosPerPoint);

} // namespace rm
