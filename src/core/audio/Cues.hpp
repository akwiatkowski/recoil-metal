#pragma once

// The synthesised cue set: the engine's own voice, for whatever the shipped banks do not
// yet name.
//
// This file first shipped believing the retail wave banks were xWMA and undecodable. That
// was measured wrong (core/audio/Xwb.hpp: all 1,737 entries are plain PCM), and the banks
// now play — deaths and impacts by bank index, weapon fire by the blueprint's own cue through
// the sound bank (core/audio/Xsb.hpp). These procedural cues remain as the fallback for a
// procedural map, a missing drive, or a weapon whose bank is not loaded: short, deterministic
// (a seeded xorshift, no clock, no libc rand), shaped on the same principles the real ones
// follow — a shot is a bright transient, an explosion is low noise with a long tail, a
// completion is a tuned blip.
//
// Every cue is generated ONCE (static locals) at first use, mono float at Mixer::kSampleRate.

#include "core/audio/Mixer.hpp"

namespace rm::audio {

/// A cannon shot: a click transient over a fast-decaying noise burst.
[[nodiscard]] const Cue& shotCue();

/// A beam pulse: a descending zap — pitch sweep with a touch of noise.
[[nodiscard]] const Cue& beamCue();

/// A death: low rumble with a slow decay and a crack at the front.
[[nodiscard]] const Cue& explosionCue();

/// A construction finished: two tuned partials, short and polite.
[[nodiscard]] const Cue& chimeCue();

} // namespace rm::audio
