#include "core/audio/CueEvents.hpp"

#include "core/audio/Cues.hpp"

namespace rm::audio {

void playForEvents(Mixer& mixer, std::span<const sim::Event> events,
                   const WaveBank* explosions, const WaveBank* impacts,
                   const WeaponSounds* weapons) {
    // A deterministic pick from a bank: the unit's own handle scrambles into an index, so
    // two runs of one match boom identically and neighbouring deaths do not all share one
    // sample. Not a real random — sound is presentation, but a replay should still SOUND
    // like the match it replays.
    const auto pick = [](const WaveBank& bank, const sim::Event& event) -> const Cue& {
        const std::size_t index =
            (static_cast<std::size_t>(event.unit.index) * 7u + event.unit.generation)
            % bank.entries.size();
        return bank.entries[index];
    };
    for (const sim::Event& event : events) {
        const float x = sim::fxToFloat(event.at[0]);
        const float z = sim::fxToFloat(event.at[2]);
        switch (event.kind) {
        case sim::EventKind::WeaponFired:
            // The weapon's own cue when the blueprint names one and the bank is loaded; the
            // shooter's handle picks among the cue's takes, as a death picks its boom, and
            // the bank's authored pitch range detunes it so a volley is not one note.
            if (weapons != nullptr) {
                if (const WeaponSounds::Take take = weapons->takeFor(
                        event.visualId, event.unit.index * 7u + event.unit.generation);
                    take.cue != nullptr) {
                    mixer.play(*take.cue, x, z, 0.5f, take.rate, take.category);
                    break;
                }
            }
            mixer.play(shotCue(), x, z, 0.5f);
            break;
        case sim::EventKind::BeamFired:
            mixer.play(beamCue(), x, z, 0.5f);
            break;
        case sim::EventKind::ProjectileImpact:
            if (impacts != nullptr && !impacts->entries.empty()) {
                mixer.play(pick(*impacts, event), x, z, 0.35f);
            }
            break;
        case sim::EventKind::UnitDestroyed:
            if (explosions != nullptr && !explosions->entries.empty()) {
                mixer.play(pick(*explosions, event), x, z, 1.0f);
            } else {
                mixer.play(explosionCue(), x, z, 0.9f);
            }
            break;
        case sim::EventKind::ConstructionFinished:
            mixer.play(chimeCue(), x, z, 0.35f);
            break;
        default:
            break;
        }
    }
}

} // namespace rm::audio
