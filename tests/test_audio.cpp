// The mixer and the cues: the engine's first sound, tested without a device.
//
// Everything here runs the audio path except the speaker: synthesis is deterministic by
// construction (seeded xorshift, no clock), and the mixer is plain arithmetic over a voice
// pool — which is exactly why it lives in core and not in the platform layer.
#include <catch2/catch_test_macros.hpp>

#include "core/audio/CueEvents.hpp"
#include "core/audio/Cues.hpp"
#include "core/audio/Mixer.hpp"

#include <cmath>
#include <vector>

TEST_CASE("cues are synthesised once, non-silent, and identical on every call") {
    const rm::audio::Cue& shot = rm::audio::shotCue();
    REQUIRE_FALSE(shot.samples.empty());
    float peak = 0.0f;
    for (const float sample : shot.samples) {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.1f);
    CHECK(peak <= 1.5f);  // the mixer's tanh knee handles the sum; a cue itself stays tame
    // The same static object every time — the mixer stores pointers, so this is the
    // lifetime contract, not an optimisation.
    CHECK(&shot == &rm::audio::shotCue());
}

TEST_CASE("the mixer attenuates with distance and pans with offset") {
    rm::audio::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setListener(0.0f, 0.0f, 600.0f);

    std::vector<float> nearOut(512 * 2);
    mixer.play(rm::audio::shotCue(), 0.0f, 0.0f);
    mixer.render(nearOut.data(), 512);

    rm::audio::Mixer farMixer;
    farMixer.setMasterGain(1.0f);
    farMixer.setListener(0.0f, 0.0f, 600.0f);
    std::vector<float> farOut(512 * 2);
    farMixer.play(rm::audio::shotCue(), 3000.0f, 0.0f);
    farMixer.render(farOut.data(), 512);

    float nearPeak = 0.0f;
    float farPeak = 0.0f;
    float farLeft = 0.0f;
    float farRight = 0.0f;
    for (std::size_t i = 0; i < 512; ++i) {
        nearPeak = std::max(nearPeak, std::abs(nearOut[i * 2]));
        farPeak = std::max(farPeak, std::abs(farOut[i * 2]));
        farLeft = std::max(farLeft, std::abs(farOut[i * 2]));
        farRight = std::max(farRight, std::abs(farOut[i * 2 + 1]));
    }
    CHECK(nearPeak > farPeak);       // closer is louder
    CHECK(farRight > farLeft);       // and a shot to the +x side leans into the right ear
}

TEST_CASE("a voice frees its slot when the cue ends, and mute is silence") {
    rm::audio::Mixer mixer;
    mixer.setListener(0.0f, 0.0f, 600.0f);
    mixer.play(rm::audio::chimeCue(), 0.0f, 0.0f);
    CHECK(mixer.activeVoices() == 1);

    // Render past the cue's whole length; the voice must have retired.
    const std::size_t frames = rm::audio::chimeCue().samples.size() + 64;
    std::vector<float> out(frames * 2);
    mixer.render(out.data(), frames);
    CHECK(mixer.activeVoices() == 0);

    mixer.setMasterGain(0.0f);
    mixer.play(rm::audio::chimeCue(), 0.0f, 0.0f);
    std::vector<float> muted(256 * 2, 1.0f);
    mixer.render(muted.data(), 256);
    for (const float sample : muted) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("events map to cues: a shot sounds, a move does not") {
    rm::audio::Mixer mixer;
    mixer.setListener(0.0f, 0.0f, 600.0f);
    const std::vector<rm::sim::Event> events{
        {.kind = rm::sim::EventKind::WeaponFired, .at = {}},
        {.kind = rm::sim::EventKind::UnitCreated, .at = {}},
        {.kind = rm::sim::EventKind::UnitDestroyed, .at = {}},
    };
    rm::audio::playForEvents(mixer, events);
    CHECK(mixer.activeVoices() == 2);  // the shot and the death; a creation is not a moment
}
