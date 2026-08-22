#include "core/audio/Mixer.hpp"

#include <algorithm>
#include <cmath>

namespace rm::audio {

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

void Mixer::play(const Cue& cue, float x, float z, float gain) {
    if (cue.samples.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);

    // Attenuation: full inside half a screen, then inverse with distance measured in
    // screens — the audible version of "off screen is far away". Below a floor the voice
    // is not worth a slot at all.
    const float dx = x - listenerX_;
    const float dz = z - listenerZ_;
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float screens = distance / halfWidthElmos_;
    const float attenuation = 1.0f / (1.0f + 2.0f * std::max(0.0f, screens - 0.5f));
    const float level = gain * attenuation;
    if (level < 0.02f) {
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
    slot->position = 0;
    slot->gainLeft = level * std::cos(angle);
    slot->gainRight = level * std::sin(angle);
}

void Mixer::render(float* out, std::size_t frames) {
    std::fill(out, out + frames * 2, 0.0f);
    const std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& voice : voices_) {
        if (voice.cue == nullptr) {
            continue;
        }
        const std::span<const float> samples = voice.cue->samples;
        const std::size_t remaining = samples.size() - voice.position;
        const std::size_t count = std::min(frames, remaining);
        const float* source = samples.data() + voice.position;
        for (std::size_t i = 0; i < count; ++i) {
            out[i * 2] += source[i] * voice.gainLeft * masterGain_;
            out[i * 2 + 1] += source[i] * voice.gainRight * masterGain_;
        }
        voice.position += count;
        if (voice.position >= samples.size()) {
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

} // namespace rm::audio
