#pragma once

// The mixer: the first thing in this engine that has ever made a sound.
//
// PLAN2 §2 weighs Sound at 16% of the engine and it sat at nothing — no mixer, no listener,
// no cue. This is the smallest honest version of all three: a voice pool summed in the audio
// callback, a listener the camera moves, and per-voice distance attenuation and pan. It is
// deliberately NOT spatial audio — an RTS hears the battlefield from above, and what matters
// is "a fight is happening left of my screen, and it is far", which pan and attenuation say.
//
// WHY A HAND MIXER rather than AVAudioPlayerNode-per-sound: a battle is hundreds of shots a
// minute, and node churn per shot is exactly the allocation-per-frame pattern the renderer
// already avoids. One render callback pulling from a fixed voice pool costs nothing when
// silent and degrades by voice-stealing when loud, which is what a game mixer does.
//
// THREADING. `play` and `setListener` run on the game thread; `render` runs on the audio
// thread. A mutex guards the voice pool — the critical sections are microseconds (no
// allocation on the audio path; a stolen voice reuses its slot), which at a 512-frame buffer
// is far below the deadline. Lock-free would be faster and harder to show correct; this is
// the version whose correctness is visible.
//
// NOT PART OF THE SIM. Nothing here feeds back into anything hashed — sound is presentation,
// like the renderer, and lives beside it.

#include <array>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace rm::audio {

/// Samples all cues are authored at. The output device may run at another rate;
/// the platform layer asks for this one and lets the OS resample.
inline constexpr float kSampleRate = 48000.0f;

/// A sound to play: mono samples at kSampleRate, owned by whoever registered it.
/// The mixer keeps a pointer, so cues must outlive it — which they do, as statics
/// synthesised once (Cues.hpp).
struct Cue {
    std::vector<float> samples;
};

class Mixer {
public:
    /// Where the ear is: the camera's look-at point, and how many elmos of world half the
    /// screen spans — the attenuation reference, so "quiet because far" tracks zoom the way
    /// the eye's "small because far" does.
    void setListener(float x, float z, float halfWidthElmos);

    /// Fire a cue at a world position. Steals the quietest voice when the pool is full —
    /// sixty-five th shot in a volley is not a sound anyone would have picked out.
    void play(const Cue& cue, float x, float z, float gain = 1.0f);

    /// The audio thread's pull: `frames` of interleaved stereo into `out`, summed from the
    /// live voices, silence when there are none.
    void render(float* out, std::size_t frames);

    /// Master gain, 0..1. The `--mute` flag sets 0.
    void setMasterGain(float gain);

    /// Live voices right now — a test's window, not a game feature.
    [[nodiscard]] std::size_t activeVoices();

private:
    struct Voice {
        const Cue* cue = nullptr;
        std::size_t position = 0;
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
    };

    /// Enough for a large battle's transient pile-up; beyond it the quietest is stolen.
    static constexpr std::size_t kVoices = 64;

    std::mutex mutex_;
    std::array<Voice, kVoices> voices_{};
    float listenerX_ = 0.0f;
    float listenerZ_ = 0.0f;
    float halfWidthElmos_ = 600.0f;
    float masterGain_ = 0.8f;
};

} // namespace rm::audio
