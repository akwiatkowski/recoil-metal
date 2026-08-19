#pragma once

#include "core/map/HeightField.hpp"
#include "core/map/PropBlueprint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rm {

// One particle, laid out for direct upload with no repacking.
//
// What is stored is a particle's WHOLE HISTORY rather than its current state: the
// point it was born at, the velocity it was born with, and how long ago that was.
// Where it is now is arithmetic the vertex shader does — origin + v*t + ½g*t² —
// so the CPU never integrates a position and never writes one.
//
// That is not a micro-optimisation, it is what keeps the thing simple. The
// alternative walks every particle every frame to move it, which is the same
// arithmetic done in the slower place and then uploaded; here the only per-frame
// CPU work is advancing one float per particle and dropping the ones that expired.
struct Particle {
    std::array<float, 3> origin;    ///< where it was born, world elmos
    float age = 0.0f;               ///< seconds since birth
    std::array<float, 3> velocity;  ///< elmos per second, at birth
    float lifetime = 1.0f;          ///< seconds; past this it is not drawn

    /// Colour at birth, PREMULTIPLIED by alpha.
    ///
    /// Premultiplied so that one pipeline covers both kinds of particle this will
    /// ever want. Ordinary translucency is (rgb·a, a) and blends over the scene;
    /// an additive spark or muzzle flash is (rgb, 0), which adds its colour and
    /// obscures nothing. With straight alpha those need two blend states and
    /// therefore two pipelines, and dust and sparks would not share a draw.
    std::array<float, 4> colour;

    float size = 1.0f;   ///< elmos across, at birth
    float growth = 0.0f; ///< elmos per second; dust swells as it disperses
};

static_assert(sizeof(Particle) == 56,
              "Particle must stay tightly packed — the shader reads it as two "
              "packed_float3s with a float each, a packed_float4 and two floats. "
              "56 rather than a round number: std::array<float,3> is 12 bytes and "
              "4-aligned, unlike simd_float3, which is what keeps these fields "
              "packed at all (see TerrainVertex for the same reasoning)");

/// The most particles drawn at once.
///
/// 4096 is about eight seconds of dust from two hundred moving units, and 192 KB
/// of buffer. Past it, new particles are dropped rather than the buffer grown: it
/// cannot be resized while the GPU may be reading it, and a scene that wants more
/// dust than this wants a different system rather than a bigger number.
inline constexpr std::size_t kMaxParticles = 4096;

/// Ages every particle and drops the ones whose time is up.
///
/// The whole of the per-frame CPU cost. Order is not preserved — a dead particle
/// is replaced by the last one in the list — because nothing about a particle
/// depends on its neighbours and the alternative is a shuffle for no reason.
void advanceParticles(std::vector<Particle>& particles, float seconds);

/// How fast a unit must be CAPABLE of moving before it kicks up dust, in elmos
/// per second.
///
/// A floor on the unit rather than on its current motion: something that creeps
/// does not throw grit however far it goes. Roughly a tenth of a tank's top speed.
inline constexpr float kDustSpeedThreshold = 4.0f;

/// How far above the ground a puff is born, in elmos.
///
/// It exists to win a depth test, not to hover — the same reason a selection ring
/// is lifted (GroundDecals.hpp). A puff born exactly on the surface is coplanar
/// with the terrain that was drawn there, and the particle pass is depth-tested, so
/// it loses. That failure is total and looks like the feature not working at all:
/// the draw is issued, the count is right, and nothing appears.
///
/// One elmo is an eighth of a heightmap square, invisible at any zoom, and enough
/// to clear the rounding at whole-map distance.
inline constexpr float kDustLiftElmos = 1.0f;

/// Dust puffs per second from one moving unit.
///
/// 12 is a continuous trail at a walking pace without being a smoke screen, and
/// with kMaxParticles it means 200 units can all move at once before the cap
/// starts dropping anything.
inline constexpr float kDustPerSecond = 12.0f;

// One unit that might be kicking up dust.
//
// `moving` is what decides it, and the distinction cost a bug worth stating: the
// sim's MoveState::speedElmosPerSecond is a unit's TOP speed — a capability it
// carries whether or not it has anywhere to be — while `moving` is the flag the sim
// maintains for "has an order and has not arrived". Gating on the speed field made
// every parked unit smoke, and the giveaway was a scene reporting 840 particles
// after 0 of 60 units found a route.
struct DustEmitter {
    std::array<float, 3> position;

    /// Whether the unit is actually under way — MoveState::moving.
    bool moving = false;

    /// The unit's top speed, which decides how hard it throws dust rather than
    /// whether it throws any.
    float topSpeedElmosPerSecond = 0.0f;

    float radiusElmos = 1.0f;
};

// Spawns dust behind moving units.
//
// `debt` carries the fractional puff between frames, so the rate does not depend on
// the frame rate — without it a slow frame emits the same number of puffs as a fast
// one and the trail thins out exactly when the machine is struggling.
//
// Deterministic for a given seed, like every other scatter here: a screenshot that
// changes between runs is hard to compare, and a benchmark whose scene changes is
// not a benchmark.

void emitDust(std::vector<Particle>& particles, std::span<const DustEmitter> emitters,
              const HeightField& field, float seconds, float& debt, std::uint32_t& seed);

// --- Ambient effects ---------------------------------------------------------
// Steam over lava, mist on water, bubbles under it, sand and snow on the wind. A map
// marks where each belongs — eight blueprints that draw nothing and name the effect —
// and says nothing at all about how it should look, because that lives in Lua this
// project does not run.
//
// So every constant behind these is OURS, in the way ADR-018's water and sky are:
// the structure is the game's, the numbers are named stand-ins. What each looks like
// is in Particles.cpp beside the dust's.

/// One place an ambient effect happens, as a map marks it.
struct AmbientEmitter {
    std::array<float, 3> position;
    prop::Effect effect = prop::Effect::None;
};

/// Spawns ambient particles at each marked place.
///
/// `debt` and `seed` work as they do for the dust: the first carries the fractional
/// particle between frames so the rate does not depend on the frame rate, and the
/// second keeps the whole thing deterministic without a global.
///
/// Unlike dust, these emit CONTINUOUSLY — a map's lava does not stop steaming — so
/// there is no equivalent of the "is it moving" test and no debt to clear.
void emitAmbient(std::vector<Particle>& particles, std::span<const AmbientEmitter> emitters,
                 float seconds, float& debt, std::uint32_t& seed);

} // namespace rm
