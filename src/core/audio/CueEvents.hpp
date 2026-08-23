#pragma once

// Events into sound: the one place the mapping lives.
//
// The same seam CombatEffects uses for particles — a consumer of the tick's event queue,
// after the sim and outside it. Every kind that reads as a moment gets a cue; movement and
// bookkeeping kinds stay silent, which is what the game does too.

#include "core/audio/Mixer.hpp"
#include "core/audio/Xwb.hpp"
#include "core/sim/Events.hpp"

#include <span>

namespace rm::audio {

/// Plays this tick's noises. `WeaponFired` is intensity-limited at the mixer by voice
/// stealing, not here — a barrage should sound like one.
///
/// `explosions` and `impacts`, when given, are the game's own wave banks: a death picks a
/// boom from the explosions bank (deterministically, by the unit's own handle, so a replay
/// booms identically) and an impact a thud from the impacts bank. Null falls back to the
/// synthesised cues — the engine's own voice, as before.
void playForEvents(Mixer& mixer, std::span<const sim::Event> events,
                   const WaveBank* explosions = nullptr, const WaveBank* impacts = nullptr);

} // namespace rm::audio
