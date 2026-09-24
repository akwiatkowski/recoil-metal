#include "core/audio/Mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rm::audio {

namespace {

/// Inverse-square attenuation, clamped at the listener's `halfWidth`: a near shot is full
/// volume, a far one falls off as 1/d². Below `kAttenuationFloor` the voice is not worth a
/// slot at all — the floor mirrors the XACT `LodCutoff` shape, with a non-zero epsilon so
/// a single far shot does not eat a slot at threshold.
constexpr float kAttenuationFloor = 0.02f;

} // namespace

void Mixer::setListener(float x, float z, float halfWidthElmos) {
    const std::lock_guard<std::mutex> lock(mutex_);
    listenerX_ = x;
    listenerZ_ = z;
    halfWidthElmos_ = std::max(50.0f, halfWidthElmos);
}

void Mixer::setMasterGain(float gain) {
    const std::lock_guard<std::mutex> lock(mutex_);
    masterGain_ = std::clamp(gain, 0.0f, 1.0f);
}

void Mixer::setBusGain(Bus bus, float gain) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (bus == Bus::Master) {
        masterGain_ = std::clamp(gain, 0.0f, 1.0f);
        return;
    }
    busGains_[static_cast<std::size_t>(bus)] = std::clamp(gain, 0.0f, 1.0f);
}

void Mixer::setCategoryLimit(std::uint16_t category, std::size_t maxInstances) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < limitsSet_; ++i) {
        if (limits_[i].first == category) {
            limits_[i].second = maxInstances;
            return;
        }
    }
    if (limitsSet_ < limits_.size()) {
        limits_[limitsSet_++] = {category, maxInstances};
    }
}

void Mixer::setCutoffElmos(float elmos) {
    const std::lock_guard<std::mutex> lock(mutex_);
    cutoffElmos_ = std::max(0.0f, elmos);
}

void Mixer::play(const Cue& cue, float x, float z, float gain) {
    play(cue, x, z, gain, 1.0f, kUnlimitedInstances);
}

void Mixer::play(const Cue& cue, float x, float z, float gain, float rate,
                 std::uint16_t category) {
    if (cue.samples.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);

    // THE LOD CUTOFF, before anything else costs a slot: a cue beyond the authored silence
    // distance is not quiet, it is absent — the curves the global settings carry end in
    // silence, and honouring that is cheaper than attenuating toward it.
    if (cutoffElmos_ > 0.0f) {
        const float dx = x - listenerX_;
        const float dz = z - listenerZ_;
        if (std::sqrt(dx * dx + dz * dz) > cutoffElmos_) {
            return;
        }
    }

    // The category's authored instance limit: over it the play is dropped, XACT's "fail"
    // behaviour — a capped category should thin out, not stack.
    for (std::size_t i = 0; i < limitsSet_; ++i) {
        if (limits_[i].first != category) continue;
        const std::size_t active = static_cast<std::size_t>(std::count_if(
            voices_.begin(), voices_.end(), [&](const Voice& voice) {
                return voice.cue != nullptr && voice.category == category;
            }));
        if (active >= limits_[i].second) {
            return;
        }
        break;
    }

    // Inverse-square attenuation: full inside halfWidth, falls off as 1/(1 + (d/refD)^2).
    // The reference distance is `halfWidthElmos` — the screen edge — which is exactly what
    // makes "off screen is far away" track zoom. The floor drops a near-silent shot before
    // it costs a slot.
    const float dx = x - listenerX_;
    const float dz = z - listenerZ_;
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float screens = distance / halfWidthElmos_;
    const float attenuation = 1.0f / (1.0f + screens * screens);
    const float busGain = busGains_[static_cast<std::size_t>(Bus::Sfx)];
    const float level = gain * attenuation * busGain;
    if (level < kAttenuationFloor) {
        return;
    }

    // Constant-power pan from the horizontal offset: a fight at the screen's left edge
    // sits in the left ear without ever leaving the right one empty.
    const float pan = std::clamp(dx / (halfWidthElmos_ * 1.5f), -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;

    Voice* slot = nullptr;
    for (Voice& voice : voices_) {
        if (voice.cue == nullptr) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        // Steal the quietest. The sixty-fifth shot in a volley was not a sound anyone
        // would have picked out of the din.
        slot = &voices_[0];
        for (Voice& voice : voices_) {
            if (voice.gainLeft + voice.gainRight < slot->gainLeft + slot->gainRight) {
                slot = &voice;
            }
        }
    }
    slot->cue = &cue;
    slot->position = 0.0;
    slot->gainLeft = level * std::cos(angle);
    slot->gainRight = level * std::sin(angle);
    // The authored pitch variation, as a playback rate: 2^(cents/1200), the same relation
    // a semitone always has to a frequency.
    slot->rate = std::max(0.25f, std::min(4.0f, rate));
    slot->category = category;
    slot->bus = Bus::Sfx;
    slot->filterState = 0.0f;
    slot->filterCutoffHz = filterIdentityHz(kSampleRate);
}

float Mixer::rpcRate(std::int16_t pitchCents, float x, float z) const {
    if (pitchCents == 0) {
        return 1.0f;
    }
    // Deterministic per (x, z) seed: a replay's shot at the same world point picks the
    // same detune. The mixer's seeded RNG would also work but doubles the audio path's
    // state, and a per-call hash is enough — the seed is the world's coordinates.
    const auto h = std::hash<float>{}(x) ^ (std::hash<float>{}(z) << 1);
    const std::uint32_t seed = static_cast<std::uint32_t>(h);
    const std::uint32_t range = static_cast<std::uint32_t>(pitchCents) * 2u + 1u;
    const int cents = static_cast<int>(seed % range) - static_cast<int>(pitchCents);
    return std::exp2(static_cast<float>(cents) / 1200.0f);
}

void Mixer::applyCue(const CueDef& def, float x, float z, float param,
                     std::uint16_t category) {
    if (def.cue == nullptr || def.cue->samples.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);

    // THE LOD CUTOFF, before anything else costs a slot: a cue beyond the authored silence
    // distance is not quiet, it is absent.
    if (cutoffElmos_ > 0.0f) {
        const float dx = x - listenerX_;
        const float dz = z - listenerZ_;
        if (std::sqrt(dx * dx + dz * dz) > cutoffElmos_) {
            return;
        }
    }

    // The category's authored instance limit: over it the play is dropped.
    for (std::size_t i = 0; i < limitsSet_; ++i) {
        if (limits_[i].first != category) continue;
        const std::size_t active = static_cast<std::size_t>(std::count_if(
            voices_.begin(), voices_.end(), [&](const Voice& voice) {
                return voice.cue != nullptr && voice.category == category;
            }));
        if (active >= limits_[i].second) {
            return;
        }
        break;
    }

    // EVALUATE THE CURVES, before picking a slot. Volume is a piecewise-linear dB curve
    // over the runtime parameter; its dB units are converted to amplitude at the end.
    const float volumeDb = def.volumeCurve.evaluate(std::clamp(param, 0.0f, 1.0f), 0.0f);
    const float filterHz = def.filterCurve.evaluate(std::clamp(param, 0.0f, 1.0f),
                                                     filterIdentityHz(kSampleRate));
    const float curveAmp = volumeDbToAmplitude(volumeDb);

    const float dx = x - listenerX_;
    const float dz = z - listenerZ_;
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float screens = distance / halfWidthElmos_;
    const float attenuation = 1.0f / (1.0f + screens * screens);
    const float busGain = busGains_[static_cast<std::size_t>(def.bus)];
    const float level = curveAmp * attenuation * busGain;
    if (level < kAttenuationFloor) {
        return;
    }

    const float pan = std::clamp(dx / (halfWidthElmos_ * 1.5f), -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;

    Voice* slot = nullptr;
    for (Voice& voice : voices_) {
        if (voice.cue == nullptr) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &voices_[0];
        for (Voice& voice : voices_) {
            if (voice.gainLeft + voice.gainRight < slot->gainLeft + slot->gainRight) {
                slot = &voice;
            }
        }
    }
    slot->cue = def.cue;
    slot->position = 0.0;
    slot->gainLeft = level * std::cos(angle);
    slot->gainRight = level * std::sin(angle);
    // The RPC: random pitch within the cue's authored cent range. Same seed for the same
    // world point keeps a replay's shots identical.
    slot->rate = std::max(0.25f, std::min(4.0f, rpcRate(def.pitchCents, x, z)));
    slot->category = category;
    slot->bus = def.bus;
    slot->filterState = 0.0f;
    slot->filterCutoffHz = std::clamp(filterHz, 20.0f, filterIdentityHz(kSampleRate));
}

void Mixer::render(float* out, std::size_t frames) {
    std::fill(out, out + frames * 2, 0.0f);
    const std::lock_guard<std::mutex> lock(mutex_);
    // Pre-compute bus gains for the audio path. The master is final; the input buses are
    // a multiplier on each voice's level at the slot level.
    for (Voice& voice : voices_) {
        if (voice.cue == nullptr) {
            continue;
        }
        const std::span<const float> samples = voice.cue->samples;
        // The one-pole low-pass: y[n] = y[n-1] + alpha * (x[n] - y[n-1]), with alpha
        // derived from the cutoff in Hz. Above Nyquist the alpha is 1.0 (identity).
        const float alpha = voice.filterCutoffHz >= filterIdentityHz(kSampleRate)
                                ? 1.0f
                                : static_cast<float>(1.0 - std::exp(
                                      -2.0 * 3.14159265f * voice.filterCutoffHz / kSampleRate));
        // A fractional playhead: the pitch variation resamples the take as it reads it,
        // linearly interpolated — two semitones of shift should not buy zipper noise.
        for (std::size_t i = 0; i < frames; ++i) {
            const auto whole = static_cast<std::size_t>(voice.position);
            if (whole + 1 >= samples.size()) {
                voice.cue = nullptr;
                break;
            }
            const float frac = static_cast<float>(voice.position - static_cast<double>(whole));
            const float sample = samples[whole] * (1.0f - frac) + samples[whole + 1] * frac;
            voice.filterState += alpha * (sample - voice.filterState);
            out[i * 2] += voice.filterState * voice.gainLeft * masterGain_;
            out[i * 2 + 1] += voice.filterState * voice.gainRight * masterGain_;
            voice.position += voice.rate;
        }
        const auto whole = static_cast<std::size_t>(voice.position);
        if (voice.cue != nullptr && whole + 1 >= samples.size()) {
            voice.cue = nullptr;
        }
    }
    // A soft knee rather than a hard clip: a battle's worth of voices sums past one, and
    // tanh keeps the pile-up loud without the crackle a clamp would add.
    for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] = std::tanh(out[i]);
    }
}

std::size_t Mixer::activeVoices() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(voices_.begin(), voices_.end(),
                      [](const Voice& voice) { return voice.cue != nullptr; }));
}

void Mixer::setMusicLayer(std::string_view name, const MusicLayer& layer) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string key{name};
    if (layer.cue == nullptr) {
        musicLayers_.erase(key);
        return;
    }
    musicLayers_[key] = layer;
}

void Mixer::setMusicIntensity(int level) {
    const std::lock_guard<std::mutex> lock(mutex_);
    musicIntensity_ = std::clamp(level, 0, 3);
}

std::string Mixer::activeMusicLayer() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, layer] : musicLayers_) {
        if (static_cast<int>(layer.intensity) == musicIntensity_) {
            return name;
        }
    }
    return {};
}

void Mixer::playMusic() {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Resolve the active layer at play time, so a `setMusicIntensity` between two plays
    // is observable on the next music call without forcing the engine to re-queue.
    const MusicLayer* picked = nullptr;
    for (const auto& [name, layer] : musicLayers_) {
        if (static_cast<int>(layer.intensity) == musicIntensity_ && layer.cue != nullptr) {
            picked = &layer;
            break;
        }
    }
    if (picked == nullptr || picked->cue->samples.empty()) {
        return;
    }

    // Music is the listener's surround: no position, no pan, no attenuation. The layer
    // rides the Music bus, with the bus's own gain. The pool is shared with Sfx — music
    // IS one of the voices, and voice-stealing applies.
    Voice* slot = nullptr;
    for (Voice& voice : voices_) {
        if (voice.cue == nullptr) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &voices_[0];
        for (Voice& voice : voices_) {
            if (voice.gainLeft + voice.gainRight < slot->gainLeft + slot->gainRight) {
                slot = &voice;
            }
        }
    }
    const float busGain = busGains_[static_cast<std::size_t>(Bus::Music)];
    slot->cue = picked->cue;
    slot->position = 0.0;
    slot->gainLeft = busGain;
    slot->gainRight = busGain;
    slot->rate = 1.0f;
    slot->category = kUnlimitedInstances;
    slot->bus = Bus::Music;
    slot->filterState = 0.0f;
    slot->filterCutoffHz = filterIdentityHz(kSampleRate);
}

} // namespace rm::audio
