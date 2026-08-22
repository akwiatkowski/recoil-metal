#pragma once

// The audio device: AVAudioEngine pulling from the mixer.
//
// A pimpl over the Objective-C types, the same shape Window takes for AppKit — the app layer
// includes this and never sees AVFoundation. One AVAudioSourceNode renders by calling
// `Mixer::render` on the audio thread; the OS owns the resampling from the mixer's 48 kHz to
// whatever the device runs at.

#include <memory>

namespace rm::audio {

class Mixer;

class Output {
public:
    Output();
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    /// Starts pulling from `mixer`, which must outlive the output. False when the device
    /// declined — a headless CI box has no output, and a game without sound still runs.
    [[nodiscard]] bool start(Mixer& mixer);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rm::audio
