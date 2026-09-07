#include "core/audio/WeaponSounds.hpp"

#include "core/unit/UnitDef.hpp"

#include <cmath>
#include <utility>

namespace rm::audio {

WeaponSounds::WeaponSounds(std::filesystem::path soundsDir) : dir_(std::move(soundsDir)) {
    settings_ = loadGlobalSettings(dir_ / "SupCom.xgs");
}

const WaveBank* WeaponSounds::wavesNamed(const std::string& name) {
    auto found = waves_.find(name);
    if (found == waves_.end()) {
        found = waves_.emplace(name, loadWaveBank(dir_ / (name + ".xwb"))).first;
        if (found->second) ++loaded_;
    }
    return found->second ? &*found->second : nullptr;
}

const SoundBank* WeaponSounds::cuesNamed(const std::string& name) {
    auto found = cues_.find(name);
    if (found == cues_.end()) {
        found = cues_.emplace(name, loadSoundBank(dir_ / (name + ".xsb"))).first;
    }
    return found->second ? &*found->second : nullptr;
}

void WeaponSounds::learn(const sim::UnitCatalog& catalog) {
    for (; learnedTypes_ < catalog.size(); ++learnedTypes_) {
        const unitdef::UnitDef* def = catalog.def(static_cast<UnitTypeIndex>(learnedTypes_));
        if (def == nullptr) continue;
        for (const unitdef::Weapon& weapon : def->weapons) {
            if (!weapon.fireSound) continue;
            const SoundBank* bank = cuesNamed(weapon.fireSound->bank);
            if (bank == nullptr) continue;
            const auto cue = bank->cues.find(weapon.fireSound->cue);
            if (cue == bank->cues.end()) continue;
            Resolved resolved{.pitchMin = cue->second.pitchMin,
                              .pitchMax = cue->second.pitchMax,
                              .category = cue->second.category};
            for (const SoundBank::Track& track : cue->second.tracks) {
                // A track names its wave bank by index into the sound bank's list; retail's
                // pairs share a name, but the list is what says so.
                const std::string waveBankName =
                    track.waveBank < bank->waveBanks.size() ? bank->waveBanks[track.waveBank]
                                                            : weapon.fireSound->bank;
                const WaveBank* bankWaves = wavesNamed(waveBankName);
                if (bankWaves == nullptr || track.entry >= bankWaves->entries.size()) continue;
                const Cue& wave = bankWaves->entries[track.entry];
                if (!wave.samples.empty()) resolved.takes.push_back(&wave);
            }
            if (!resolved.takes.empty()) {
                byKey_[def->name + ":" + weapon.label] = std::move(resolved);
            }
        }
    }
}

const Cue* WeaponSounds::cueFor(std::string_view key, std::uint32_t seed) const {
    const auto found = byKey_.find(key);
    if (found == byKey_.end() || found->second.takes.empty()) return nullptr;
    return found->second.takes[seed % found->second.takes.size()];
}

WeaponSounds::Take WeaponSounds::takeFor(std::string_view key, std::uint32_t seed) const {
    const auto found = byKey_.find(key);
    if (found == byKey_.end() || found->second.takes.empty()) return {};
    const Resolved& resolved = found->second;
    Take take{.cue = resolved.takes[seed % resolved.takes.size()],
              .category = resolved.category};
    // The authored pitch range, in hundredths of a semitone, as a playback rate. The same
    // seed picks the take and the detune, so a replay is the match it replays, detune and
    // all. A flat range (the retail default is ±0 for many cues) collapses to 1.0 exactly.
    const std::int16_t cents = [resolved, seed] {
        if (resolved.pitchMax <= resolved.pitchMin) return resolved.pitchMin;
        return static_cast<std::int16_t>(
            resolved.pitchMin
            + static_cast<std::int32_t>(seed % static_cast<std::uint32_t>(
                                                    resolved.pitchMax - resolved.pitchMin + 1)));
    }();
    take.rate = std::exp2(static_cast<float>(cents) / 1200.0f);
    return take;
}

} // namespace rm::audio
