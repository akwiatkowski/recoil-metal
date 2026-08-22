#include "core/audio/CueEvents.hpp"

#include "core/audio/Cues.hpp"

namespace rm::audio {

void playForEvents(Mixer& mixer, std::span<const sim::Event> events) {
    for (const sim::Event& event : events) {
        const float x = sim::fxToFloat(event.at[0]);
        const float z = sim::fxToFloat(event.at[2]);
        switch (event.kind) {
        case sim::EventKind::WeaponFired:
            mixer.play(shotCue(), x, z, 0.5f);
            break;
        case sim::EventKind::BeamFired:
            mixer.play(beamCue(), x, z, 0.5f);
            break;
        case sim::EventKind::UnitDestroyed:
            mixer.play(explosionCue(), x, z, 0.9f);
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
