#pragma once

#include "core/scene/Particles.hpp"
#include "core/texture/Dds.hpp"
#include "core/vfs/Vfs.hpp"

#include <map>
#include <string>
#include <string_view>

namespace rm {

// Retail EmitterBlueprint/TrailBlueprint/BeamBlueprint use the same numeric modes.
enum class EffectBlend : std::uint32_t {
    Alpha, ModulateInverse, Modulate2xInverse, Add, Premultiplied, Refract,
};

struct EffectCurve {
    float range = 1;
    std::vector<std::array<float, 3>> keys; // time, value, random range
    /// Phase is the fraction of the emitter cycle; random is uniform in [0,1].
    [[nodiscard]] float sample(float phase, float random = 0.5f) const;
    [[nodiscard]] float integral(float phase) const;
};

struct WeaponMaterial {
    std::string emitter;
    dds::Texture texture;
    dds::Texture ramp;
    float width = 0;
    float length = 0;
    EffectBlend blend = EffectBlend::Alpha;
    float emitterLifetime = -1; // seconds; negative means until its owner stops
    float repeatTime = 1;
    float sortOrder = 0;
    EffectCurve emitRate, particleLifetime, startSize, endSize;
    EffectCurve initialRotation, rotationRate, frameRate, textureSelection, rampSelection;
    EffectCurve velocity, spawnRadius;
    std::array<EffectCurve, 3> direction, acceleration, position;
    bool gravity = false;
    bool flat = false;
    bool localVelocity = false;
    bool localAcceleration = false;
    std::uint32_t frames = 1, strips = 1;
    // Same layout as WeaponMaterialIn in the shader: tints, then ramp/beam/repeat/scroll.
    std::array<float, 4> startColour{1,1,1,1};
    std::array<float, 4> endColour{1,1,1,1};
    std::array<float, 4> sampling{0,0,1,0};
};

/// Presentation-only definitions. Keys are projectile blueprint paths or UNIT:WeaponLabel
/// for beams without a projectile. No inference from faction colours or ballistic arcs.
struct WeaponVisuals {
    std::vector<WeaponMaterial> materials;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> definitions;
    std::map<std::string, float, std::less<>> scales;
    std::vector<std::string> unavailable;

    [[nodiscard]] std::span<const std::uint32_t> find(std::string_view key) const;
    [[nodiscard]] float scale(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;
};

/// Execute original script declarations, then load their original emitter/DDS data.
/// Missing content is reported in unavailable; unknown weapons keep the generic fallback.
[[nodiscard]] WeaponVisuals loadWeaponVisuals(const vfs::Vfs& content);
void loadWeaponMaterials(WeaponVisuals& result, const vfs::Vfs& content,
    std::string_view key, std::span<const std::string> emitters);

/// Sample emitter curves at birth. Random state belongs to presentation, never simulation.
[[nodiscard]] Particle makeWeaponParticle(const WeaponMaterial& material, std::uint32_t id,
    float phase, std::array<float, 3> position, std::uint32_t& seed);

void emitWeaponParticles(std::vector<Particle>& out, const WeaponMaterial& material,
    std::uint32_t id, std::array<float, 3> from, std::array<float, 3> to,
    float begin, float end, std::uint32_t seed, std::array<float,3> direction = {},
    bool initial = true);

/// A camera-facing bolt or beam strip. Direction and length are independent of motion.
void appendWeaponVisual(std::vector<Particle>& out, const WeaponVisuals& visuals,
    std::string_view key, std::array<float, 3> from, std::array<float, 3> to,
    float elmosPerPoint, bool beam = false, float duration = 0.25f, bool previewEmitters = true);

} // namespace rm
