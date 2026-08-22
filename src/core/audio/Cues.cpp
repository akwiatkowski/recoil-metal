#include "core/audio/Cues.hpp"

#include <cmath>
#include <cstdint>

namespace rm::audio {
namespace {

constexpr float kTau = 6.28318530f;

/// Deterministic noise: xorshift32, seeded per cue. Not libc rand — the cue must be the
/// same waveform every run on every machine, for the same reason everything else here is.
struct Noise {
    std::uint32_t state;
    explicit Noise(std::uint32_t seed) : state(seed) {}
    float operator()() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;  // roughly [-1, 1)
    }
};

[[nodiscard]] std::size_t frames(float seconds) {
    return static_cast<std::size_t>(seconds * kSampleRate);
}

/// An exponential decay from 1 toward 0, reaching ~5% at `seconds`.
[[nodiscard]] float decay(std::size_t i, float seconds) {
    return std::exp(-3.0f * static_cast<float>(i) / (seconds * kSampleRate));
}

} // namespace

const Cue& shotCue() {
    static const Cue cue = [] {
        Cue built;
        const std::size_t n = frames(0.12f);
        built.samples.resize(n);
        Noise noise(0x5254531u);
        // A one-pole lowpass over the noise keeps the burst punchy rather than hissy; the
        // 300 Hz sine underneath is the gun's thump.
        float filtered = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            filtered += 0.35f * (noise() - filtered);
            const float t = static_cast<float>(i) / kSampleRate;
            const float thump = 0.6f * std::sin(kTau * 300.0f * t) * decay(i, 0.03f);
            built.samples[i] = (0.7f * filtered * decay(i, 0.05f) + thump) * 0.8f;
        }
        return built;
    }();
    return cue;
}

const Cue& beamCue() {
    static const Cue cue = [] {
        Cue built;
        const std::size_t n = frames(0.22f);
        built.samples.resize(n);
        Noise noise(0x42454du);
        float phase = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            // 2 kHz falling to 400 Hz: the zap. Phase integrated rather than computed from
            // t so the sweep has no discontinuities.
            const float hz = 2000.0f - 1600.0f * t;
            phase += kTau * hz / kSampleRate;
            built.samples[i] =
                (0.6f * std::sin(phase) + 0.15f * noise()) * decay(i, 0.15f) * 0.7f;
        }
        return built;
    }();
    return cue;
}

const Cue& explosionCue() {
    static const Cue cue = [] {
        Cue built;
        const std::size_t n = frames(0.9f);
        built.samples.resize(n);
        Noise noise(0x424f4fdu);
        // Brown-ish noise: integrated white, leaked so it stays bounded. The crack at the
        // front is two milliseconds of raw white at full level.
        float brown = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            brown = 0.985f * brown + 0.12f * noise();
            float sample = 2.2f * brown * decay(i, 0.55f);
            if (i < frames(0.002f)) {
                sample += 0.9f * noise();
            }
            built.samples[i] = sample;
        }
        return built;
    }();
    return cue;
}

const Cue& chimeCue() {
    static const Cue cue = [] {
        Cue built;
        const std::size_t n = frames(0.3f);
        built.samples.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            built.samples[i] = (0.4f * std::sin(kTau * 880.0f * t)
                                + 0.25f * std::sin(kTau * 1320.0f * t))
                               * decay(i, 0.25f) * 0.6f;
        }
        return built;
    }();
    return cue;
}

} // namespace rm::audio
