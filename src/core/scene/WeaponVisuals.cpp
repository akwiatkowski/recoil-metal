#include "core/scene/WeaponVisuals.hpp"
#include "core/lua/LuaTable.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <optional>

namespace rm {
namespace {
std::string folded(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::optional<dds::Texture> readTexture(const vfs::Vfs& content, std::string_view path) {
    const auto bytes = content.read(path);
    if (!bytes) return std::nullopt;
    auto texture = dds::load(*bytes);
    if (!texture) return std::nullopt;
    return std::move(*texture);
}
EffectCurve readCurve(const lua::Value& table, std::string_view name) {
    EffectCurve curve;
    const auto* value = table.find(name);
    if (!value) return curve;
    curve.range = static_cast<float>(value->numberAt("XRange").value_or(1));
    if (const auto* keys = value->find("Keys")) {
        for (const auto& key : keys->items) {
            curve.keys.push_back({static_cast<float>(key.numberAt("x").value_or(0)),
                static_cast<float>(key.numberAt("y").value_or(0)),
                static_cast<float>(key.numberAt("z").value_or(0))});
        }
    }
    std::ranges::stable_sort(curve.keys, {}, [](const auto& key) { return key[0]; });
    return curve;
}
}

float EffectCurve::sample(float phase, float random) const {
    if (keys.empty()) return 0;
    const float time = std::clamp(phase, 0.0f, 1.0f) * range;
    const auto right = std::ranges::upper_bound(keys, time, {}, [](const auto& key) { return key[0]; });
    auto value = right == keys.begin() ? keys.front() : *(right-1);
    if (right != keys.end() && right != keys.begin()) {
        const float span = (*right)[0] - value[0];
        const float t = span > 0 ? (time-value[0])/span : 0;
        value[1] = std::lerp(value[1], (*right)[1], t);
        value[2] = std::lerp(value[2], (*right)[2], t);
    }
    // InitialRotation=180, range=360 represents a complete [0,360] turn.
    return value[1] + (std::clamp(random, 0.0f, 1.0f)-0.5f)*value[2];
}

float EffectCurve::integral(float phase) const {
    const float end = std::clamp(phase, 0.0f, 1.0f);
    float area = 0, previous = 0;
    const auto segment = [&](float a, float b) {
        const float left = sample(a), right = sample(b);
        if (left >= 0 && right >= 0) return (left+right)*0.5f*(b-a);
        if (left <= 0 && right <= 0) return 0.0f;
        const float positive = std::max(left, right);
        return positive*positive / (2*std::abs(right-left)) * (b-a);
    };
    for (const auto& key : keys) {
        const float time = range > 0 ? std::clamp(key[0]/range, 0.0f, end) : end;
        area += segment(previous, time);
        previous = time;
        if (time >= end) break;
    }
    return area + segment(previous, end);
}

std::span<const std::uint32_t> WeaponVisuals::find(std::string_view key) const {
    const auto it = definitions.find(folded(key));
    return it == definitions.end() ? std::span<const std::uint32_t>{} : it->second;
}

float WeaponVisuals::scale(std::string_view key) const {
    const auto it = scales.find(folded(key));
    return it == scales.end() ? 1.0f : it->second;
}

bool WeaponVisuals::contains(std::string_view key) const { return definitions.contains(folded(key)); }
bool WeaponVisuals::hasMesh(std::string_view key) const { return meshed.contains(folded(key)); }
std::string foldedVisualKey(std::string_view key) { return folded(key); }

void loadWeaponMaterials(WeaponVisuals& result, const vfs::Vfs& content,
    std::string_view key, std::span<const std::string> emitters) {
    for (const auto& path : emitters) {
        const auto cached = std::ranges::find(result.materials, path, &WeaponMaterial::emitter);
        if (cached != result.materials.end()) {
            result.definitions[folded(key)].push_back(static_cast<std::uint32_t>(cached-result.materials.begin()));
            continue;
        }
        const auto bytes = content.read(path);
        if (!bytes) { result.unavailable.push_back(path + ": missing emitter"); continue; }
        const auto table = lua::parseTable({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        if (!table) { result.unavailable.push_back(path + ": unsupported emitter data"); continue; }
        const auto texturePath = table->stringAt("TextureName").value_or(
            table->stringAt("RepeatTexture").value_or(table->stringAt("Texture").value_or("")));
        auto texture = readTexture(content, texturePath);
        if (!texture) { result.unavailable.push_back(path + ": missing/unsupported texture " + std::string(texturePath)); continue; }
        WeaponMaterial material;
        material.emitter = path;
        material.texture = std::move(*texture);
        const auto mode = static_cast<int>(table->numberAt("Blendmode").value_or(
            table->numberAt("BlendMode").value_or(0)));
        if (mode < 0 || mode > 5) {
            result.unavailable.push_back(path + ": unknown blend mode"); continue;
        }
        material.blend = static_cast<EffectBlend>(mode);
        material.emitRate = readCurve(*table, "EmitRateCurve");
        material.particleLifetime = readCurve(*table, "LifetimeCurve");
        material.startSize = readCurve(*table, "StartSizeCurve");
        material.endSize = readCurve(*table, "EndSizeCurve");
        material.initialRotation = readCurve(*table, "InitialRotationCurve");
        material.rotationRate = readCurve(*table, "RotationRateCurve");
        material.frameRate = readCurve(*table, "FrameRateCurve");
        material.textureSelection = readCurve(*table, "TextureSelectionCurve");
        material.rampSelection = readCurve(*table, "RampSelectionCurve");
        material.velocity = readCurve(*table, "VelocityCurve");
        material.spawnRadius = readCurve(*table, "SizeCurve");
        const std::array<const char*,3> axes{"X", "Y", "Z"};
        for (std::size_t axis=0; axis<3; ++axis) {
            material.direction[axis] = readCurve(*table, std::string(axes[axis])+"DirectionCurve");
            material.acceleration[axis] = readCurve(*table, std::string(axes[axis])+"AccelCurve");
            material.position[axis] = readCurve(*table, std::string(axes[axis])+"PosCurve");
        }
        const auto flag = [&](const char* name) {
            const auto* value = table->find(name);
            return value && value->boolean;
        };
        material.gravity = flag("Gravity");
        material.flat = flag("Flat");
        material.localVelocity = flag("LocalVelocity");
        material.localAcceleration = flag("LocalAcceleration");
        // Emitter cycle/lifetime fields use the original ten-Hz content clock.
        material.emitterLifetime = static_cast<float>(table->numberAt("Lifetime").value_or(-1)) * 0.1f;
        material.repeatTime = std::max(0.1f, static_cast<float>(table->numberAt("Repeattime").value_or(10))*0.1f);
        material.frames = static_cast<std::uint32_t>(std::max(1.0, table->numberAt("TextureFramecount").value_or(1)));
        material.strips = static_cast<std::uint32_t>(std::max(1.0, table->numberAt("TextureStripcount").value_or(1)));
        material.sortOrder = static_cast<float>(table->numberAt("SortOrder").value_or(0));
        if (const auto rampPath = table->stringAt("RampTexture")) {
            auto ramp = readTexture(content, *rampPath);
            if (!ramp) { result.unavailable.push_back(path + ": missing/unsupported ramp " + std::string(*rampPath)); continue; }
            material.ramp = std::move(*ramp);
        }
        const bool beam = table->find("Thickness") != nullptr;
        material.ribbon = table->find("TrailLength") != nullptr;
        material.sampling = {material.ramp.data.empty() ? 0.0f : 1.0f, beam ? 1.0f : 0.0f,
            static_cast<float>(table->numberAt("RepeatRate").value_or(0)),
            static_cast<float>(table->numberAt("VShift").value_or(0)) * 10};
        if (material.ribbon) {
            // TrailBlueprint.TextureRepeatRate: "how often the texture is repeated" along the
            // trail. Read as repeats per ogrid of TrailLength, the same convention as a beam's
            // RepeatRate per ogrid of length, folded into one whole-trail repeat count here
            // because a ribbon's segments do not know the trail's length in the shader.
            material.sampling[2] = static_cast<float>(table->numberAt("TextureRepeatRate").value_or(
                table->numberAt("RepeatRate").value_or(1)))
                * static_cast<float>(table->numberAt("TrailLength").value_or(0));
        }
        const std::array<const char*,4> channels{"x","y","z","w"};
        for (std::size_t i=0; i<channels.size(); ++i) {
            if (const auto* colour = table->find("StartColor"))
                material.startColour[i] = static_cast<float>(colour->numberAt(channels[i]).value_or(1));
            if (const auto* colour = table->find("EndColor"))
                material.endColour[i] = static_cast<float>(colour->numberAt(channels[i]).value_or(1));
        }
        // Effect blueprints use ogrids; the renderer's world uses eight elmos per ogrid.
        material.width = 8.0f * static_cast<float>(table->numberAt("Thickness").value_or(
            table->numberAt("Size").value_or(material.startSize.sample(0))));
        material.width *= 2; // Retail BeamVS/TrailVS offsets extend on both sides of the centre.
        material.length = 8.0f * static_cast<float>(table->numberAt("TrailLength").value_or(table->numberAt("Length").value_or(0)));
        if (material.width < 0 || !std::isfinite(material.width) || !std::isfinite(material.length)) {
            result.unavailable.push_back(path + ": invalid dimensions"); continue;
        }
        result.definitions[folded(key)].push_back(static_cast<std::uint32_t>(result.materials.size()));
        result.materials.push_back(std::move(material));
    }
}

void appendWeaponVisual(std::vector<Particle>& out, const WeaponVisuals& visuals,
    std::string_view key, std::array<float, 3> from, std::array<float, 3> to,
    float elmosPerPoint, bool beam, float duration, bool previewEmitters, bool straightRibbons) {
    std::array<float, 3> axis{to[0]-from[0], to[1]-from[1], to[2]-from[2]};
    const float distance = std::sqrt(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
    if (distance > 0) for (auto& value : axis) value /= distance;
    for (const auto id : visuals.find(key)) {
        const auto& material = visuals.materials[id];
        if (!beam && material.ribbon && !straightRibbons) continue;
        if (!beam && !material.emitRate.keys.empty()) {
            if (!previewEmitters) continue;
            // Preview of an attached emitter; persistent emission is handled on the tick path.
            std::uint32_t seed = id + 1;
            auto particle = makeWeaponParticle(material, id, 0, from, seed);
            particle.age = std::min(0.2f, particle.lifetime * 0.5f);
            out.push_back(particle);
            continue;
        }
        const float length = beam ? distance : material.length;
        // A bolt ends at its simulated head; a beam starts at its simulated muzzle.
        auto origin = from;
        if (!beam) for (std::size_t i=0; i<3; ++i) origin[i] -= axis[i]*length;
        out.push_back(Particle{
            .origin = origin, .age = beam ? 0.0f : 0.2f, .velocity = {},
            .lifetime = beam ? std::max(duration, 0.01f) : 1.0f,
            .colour = {1,1,1,0}, .size = std::max(material.width, elmosPerPoint),
            .axis = axis, .length = length, .material = id,
        });
    }
}

Particle makeWeaponParticle(const WeaponMaterial& material, std::uint32_t id,
    float phase, std::array<float, 3> position, std::uint32_t& seed) {
    const auto random = [&]() {
        if (seed == 0) seed = 1;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    const auto sample = [&](const EffectCurve& curve) { return curve.sample(phase, random()); };
    const float lifetime = std::max(0.001f, sample(material.particleLifetime));
    // WorldVS multiplies a [-1,1] quad by authored size: convert radius to full width.
    const float size = std::max(0.0f, sample(material.startSize)) * 16;
    const float end = std::max(0.0f, sample(material.endSize)) * 16;
    const float speed = sample(material.velocity) * 80; // ogrids/tick -> elmos/second
    Particle particle{.origin = position, .lifetime = lifetime, .colour = {1,1,1,1},
        .size = size, .growth = (end-size)/lifetime, .material = id};
    particle.flags = material.flat ? 1u : 0u;
    for (std::size_t axis=0; axis<3; ++axis) {
        particle.origin[axis] += sample(material.position[axis]) * 8;
        particle.velocity[axis] = sample(material.direction[axis]) * speed;
        particle.acceleration[axis] = sample(material.acceleration[axis]) * 800;
    }
    const float radius = std::max(0.0f, sample(material.spawnRadius))*8*std::sqrt(random());
    const float angle = random()*6.283185307179586f;
    particle.origin[0] += radius*std::cos(angle);
    particle.origin[2] += radius*std::sin(angle);
    if (material.gravity) particle.acceleration[1] -= 4.9f * 8;
    constexpr float radiansPerDegree = 0.017453292519943295f;
    particle.rotation = sample(material.initialRotation) * radiansPerDegree;
    particle.rotationRate = sample(material.rotationRate) * radiansPerDegree;
    particle.animation = {sample(material.frameRate), sample(material.textureSelection),
        sample(material.rampSelection)};
    return particle;
}

void emitWeaponParticles(std::vector<Particle>& out, const WeaponMaterial& material,
    std::uint32_t id, std::array<float, 3> from, std::array<float, 3> to,
    float begin, float end, std::uint32_t seed, std::array<float,3> direction, bool initial) {
    if (end < begin || (end == begin && !initial) || material.emitRate.keys.empty()) return;
    const float cycle = material.repeatTime;
    const auto total = [&](float time) {
        time = std::max(0.0f, time);
        if (material.emitterLifetime >= 0) time = std::min(time, material.emitterLifetime);
        const float cycles = std::floor(time/cycle);
        return cycle * (cycles*material.emitRate.integral(1)
            + material.emitRate.integral((time-cycles*cycle)/cycle));
    };
    // Integrate authored rate, rather than rounding particles/step. This retains fractional
    // emission across steps and supports changing rates without a frame-rate dependency.
    // The initial particle is born at creation, not after one rate interval. Otherwise
    // short muzzle emitters (Lifetime=.1, EmitRate=6) finish without ever producing a flash.
    const auto first = initial && begin == 0 && material.emitRate.sample(0) > 0 ? 0
        : static_cast<std::uint64_t>(std::floor(total(begin)+0.00001f))+1;
    const auto last = static_cast<std::uint64_t>(std::floor(total(end)+0.00001f));
    std::array<float,3> forward{to[0]-from[0], to[1]-from[1], to[2]-from[2]};
    if (direction != std::array<float,3>{}) forward = direction;
    const float distance = std::hypot(forward[0], forward[1], forward[2]);
    if (distance > 0) for (auto& value : forward) value /= distance;
    else forward = {0,0,1};
    std::array<float,3> right{forward[2], 0, -forward[0]};
    const float horizontal = std::hypot(right[0], right[2]);
    if (horizontal > 0) for (auto& value : right) value /= horizontal;
    else right = {1,0,0};
    const std::array<float,3> up{forward[1]*right[2], forward[2]*right[0]-forward[0]*right[2], -forward[1]*right[0]};
    const auto rotate = [&](std::array<float,3> value) {
        std::array<float,3> result{};
        for (std::size_t axis=0; axis<3; ++axis)
            result[axis] = right[axis]*value[0]+up[axis]*value[1]+forward[axis]*value[2];
        return result;
    };
    for (auto ordinal=first; ordinal<=last && out.size()<kMaxParticles; ++ordinal) {
        float low=begin, high=end;
        // Invert the monotonic integrated rate to locate each birth within this step.
        for (int i=0; i<24; ++i) {
            const float mid = (low+high)*0.5f;
            if (total(mid) < static_cast<float>(ordinal)) low=mid; else high=mid;
        }
        const float birth = ordinal == 0 ? 0.0f : high;
        auto random = seed ^ (static_cast<std::uint32_t>(ordinal)*0x9e3779b9u);
        auto particle = makeWeaponParticle(material, id, std::fmod(birth,cycle)/cycle, {}, random);
        particle.origin = rotate(particle.origin);
        const float fraction = end > begin ? (birth-begin)/(end-begin) : 0;
        for (std::size_t axis=0; axis<3; ++axis) particle.origin[axis] += std::lerp(from[axis],to[axis],fraction);
        if (material.localVelocity) particle.velocity = rotate(particle.velocity);
        if (material.localAcceleration) {
            if (material.gravity) particle.acceleration[1] += 4.9f*8;
            particle.acceleration = rotate(particle.acceleration);
            if (material.gravity) particle.acceleration[1] -= 4.9f*8;
        }
        particle.age = end-birth;
        if (particle.age < particle.lifetime) out.push_back(particle);
    }
}
} // namespace rm
