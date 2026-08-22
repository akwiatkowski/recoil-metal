#pragma once

// The cue set: every sound the game currently makes, synthesised.
//
// SYNTHESISED RATHER THAN SHIPPED, and the reason is recorded rather than hidden: Forged
// Alliance's audio lives in XACT wave banks (`sounds.scd` holds `.xwb` files) whose entries
// are xWMA — a WMA variant with no decoder this project can reasonably carry. The formats
// converted so far (DDS, SCM, SCMAP, blueprints) all decode with published layouts; xWMA
// needs a licensed codec or ffmpeg, either of which is a dependency ADR-044's standard would
// have to weigh separately. Until then the engine speaks with its own voice: short procedural
// cues, deterministic (a seeded xorshift, no clock, no libc rand), shaped on the same
// principles the real ones follow — a shot is a bright transient, an explosion is low noise
// with a long tail, a completion is a tuned blip.
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
