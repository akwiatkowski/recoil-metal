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

#include "core/audio/Curve.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rm::audio {

/// Samples all cues are authored at. The output device may run at another rate;
/// the platform layer asks for this one and lets the OS resample. 44.1 kHz is the
/// Audio Queue standard on Apple silicon (AVAudioSession docs); retail .xwb entries
/// sit at 32 kHz so both sides need resampling, but the OS does it for free.
inline constexpr float kSampleRate = 44100.0f;

/// "Unlimited" — the value XACT's authors give a category nobody capped, kept for the
/// instance-limit tables read out of the global settings.
inline constexpr std::uint16_t kUnlimitedInstances = 0xffff;

/// The FX bus layout: every cue plays on one of four input buses (Sfx, Ui, Voice, Music),
/// each with its own gain in [0, 1], summed into Master. The named buses match the four
/// categories retail XACT authors actually wire to (Default/Weapons, UI, Voice, Music,
/// per SupCom.xgs — Xgs.hpp). Master is the synthesised sink the platform layer asks for.
enum class Bus : std::uint8_t { Sfx = 0, Ui = 1, Voice = 2, Music = 3, Master = 4 };

/// Number of input buses; `Bus::Master` is past the end of the input bus array.
inline constexpr std::size_t kBusCount = 4;

/// A sound to play: mono samples at kSampleRate, owned by whoever registered it.
/// The mixer keeps a pointer, so cues must outlive it — which they do, as statics
/// synthesised once (Cues.hpp).
struct Cue {
    std::vector<float> samples;
};

/// A cue with its authored curves: the cue's waveform, an optional distance→volume curve,
/// an optional filter (low-pass cutoff Hz) curve, and a cent range around the cue's authored
/// pitch that the mixer picks per play. Empty curves are identity (1.0 amplitude, full
/// bandwidth, no detune). The shape follows ADR-131.
struct CueDef {
    const Cue* cue = nullptr;
    Curve volumeCurve;
    Curve filterCurve;
    /// Random pitch cent range, in hundredths of a semitone (Xsb.hpp:46-48).
    /// pitch == 0 collapses to identity; pitch != 0 is the half-range of the random.
    std::int16_t pitchCents = 0;
    Bus bus = Bus::Sfx;
};

/// A music layer: a single cue routed to the Music bus, plus the intensity index it answers
/// to. `setMusicLayer` registers layers by name and `setMusicIntensity` selects which one
/// the next music play resolves to.
struct MusicLayer {
    const Cue* cue = nullptr;
    std::uint8_t intensity = 0;
};

class Mixer {
public:
    /// Where the ear is: the camera's look-at point, and how many elmos of world half the
    /// screen spans — the attenuation reference, so "quiet because far" tracks zoom the way
    /// the eye's "small because far" does.
    void setListener(float x, float z, float halfWidthElmos);

    /// Fire a cue at a world position. Steals the quietest voice when the pool is full —
    /// sixty-fifth shot in a volley is not a sound anyone would have picked out.
    void play(const Cue& cue, float x, float z, float gain = 1.0f);

    /// The same, with the cue's authored VARIATION applied: `rate` is the pitch multiplier
    /// (2^(cents/1200) from the bank's pitch range) and `category` the XACT category the cue's
    /// sound names, whose instance limit — when one was authored — caps how many of its voices
    /// may sound at once (a thinned-out barrage, not a stacked one). Over the limit the play
    /// is dropped, XACT's "fail" behaviour.
    void play(const Cue& cue, float x, float z, float gain, float rate, std::uint16_t category);

    /// Plays a `CueDef` with its authored curves evaluated at `param` (the runtime variable
    /// the curves are authored against — distance, health, morale — anything in [0, 1]).
    /// Volume: piecewise-linear dB at `param`, converted to amplitude and multiplied into
    /// the voice's gain. Filter: piecewise-linear Hz at `param`, applied as a one-pole
    /// low-pass on the voice's sample pull. RPC: a uniform random pick in `pitchCents`
    /// half-range, converted to a playback rate. The category limit applies as for `play`.
    void applyCue(const CueDef& def, float x, float z, float param, std::uint16_t category);

    /// An XACT category's authored instance limit, from the global settings. Feeding the
    /// mixer the parsed table is the app's one-time wiring at startup.
    void setCategoryLimit(std::uint16_t category, std::size_t maxInstances);

    /// The LodCutoff distance in elmos: beyond it from the listener a play is refused before
    /// it ever costs a voice — the engine-side meaning of the blueprint's `LodCutoff`
    /// variables, whose curves end in authored silence anyway. Zero (the default) means no
    /// cutoff.
    void setCutoffElmos(float elmos);

    /// The audio thread's pull: `frames` of interleaved stereo into `out`, summed from the
    /// live voices, silence when there are none.
    void render(float* out, std::size_t frames);

    /// Master gain, 0..1. The `--mute` flag sets 0.
    void setMasterGain(float gain);

    /// Per-bus gain in [0, 1]. Bus::Master is the master gain (alias of setMasterGain);
    /// the input buses (Sfx/Ui/Voice/Music) duck or boost their category without touching
    /// the others. Default is 1.0 for every bus.
    void setBusGain(Bus bus, float gain);

    /// Live voices right now — a test's window, not a game feature.
    [[nodiscard]] std::size_t activeVoices();

    /// Registers a music layer by name with a fixed intensity. Re-registering the same name
    /// overwrites the previous layer (the engine treats music as one slot per name).
    void setMusicLayer(std::string_view name, const MusicLayer& layer);

    /// Selects which registered music layer plays on the next music call. Layers carry an
    /// `intensity`; the layer whose intensity matches `level` (0..3) becomes active. Layers
    /// without a matching intensity stay registered but play nothing.
    void setMusicIntensity(int level);

    /// The name of the music layer currently selected by intensity, or empty when no layer
    /// matches. Test window: a layer swap is observable by reading this back.
    [[nodiscard]] std::string activeMusicLayer() const;

    /// Plays the active music layer on the Music bus — no position, music is the listener's
    /// surround. A missing layer is a no-op.
    void playMusic();

private:
    struct Voice {
        const Cue* cue = nullptr;
        double position = 0.0;
        float gainLeft = 0.0f;
        float gainRight = 0.0f;
        float rate = 1.0f;
        std::uint16_t category = kUnlimitedInstances;
        /// Bus the voice rides. Voices on input buses pick up the bus's gain at render time.
        Bus bus = Bus::Sfx;
        /// One-pole low-pass filter state — cutoff Hz above Nyquist is identity, see Curve.hpp.
        float filterState = 0.0f;
        float filterCutoffHz = kSampleRate * 0.5f;
    };

    /// Enough for a large battle's transient pile-up; beyond it the quietest is stolen.
    static constexpr std::size_t kVoices = 64;
    /// Room for twice the capped categories retail authors (16 of 39 in SupCom.xgs) —
    /// a foreign mix with more caps degrades by ignoring the excess, never by faulting.
    static constexpr std::size_t kMaxTrackedCategories = 32;

    /// Picks a uniform random cent in [-pitchCents, pitchCents], returning 1.0 when zero.
    /// The seed is deterministic from the call's position so replays sound identical.
    [[nodiscard]] float rpcRate(std::int16_t pitchCents, float x, float z) const;

    mutable std::mutex mutex_;
    std::array<Voice, kVoices> voices_{};
    std::array<std::pair<std::uint16_t, std::size_t>, kMaxTrackedCategories> limits_{};
    std::size_t limitsSet_ = 0;
    float listenerX_ = 0.0f;
    float listenerZ_ = 0.0f;
    float halfWidthElmos_ = 600.0f;
    float masterGain_ = 0.8f;
    float cutoffElmos_ = 0.0f;
    std::array<float, kBusCount> busGains_{1.0f, 1.0f, 1.0f, 1.0f};
    std::map<std::string, MusicLayer, std::less<>> musicLayers_;
    int musicIntensity_ = 0;
};

} // namespace rm::audio
