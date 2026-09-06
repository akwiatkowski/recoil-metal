#include "core/audio/WeaponSounds.hpp"

#include "core/unit/UnitDef.hpp"

#include <utility>

namespace rm::audio {

WeaponSounds::WeaponSounds(std::filesystem::path soundsDir) : dir_(std::move(soundsDir)) {}

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
            std::vector<const Cue*> waves;
            for (const SoundBank::Track& track : cue->second) {
                // A track names its wave bank by index into the sound bank's list; retail's
                // pairs share a name, but the list is what says so.
                const std::string waveBankName =
                    track.waveBank < bank->waveBanks.size() ? bank->waveBanks[track.waveBank]
                                                            : weapon.fireSound->bank;
                const WaveBank* bankWaves = wavesNamed(waveBankName);
                if (bankWaves == nullptr || track.entry >= bankWaves->entries.size()) continue;
                const Cue& wave = bankWaves->entries[track.entry];
                if (!wave.samples.empty()) waves.push_back(&wave);
            }
            if (!waves.empty()) {
                byKey_[def->name + ":" + weapon.label] = std::move(waves);
            }
        }
    }
}

const Cue* WeaponSounds::cueFor(std::string_view key, std::uint32_t seed) const {
    const auto found = byKey_.find(key);
    if (found == byKey_.end() || found->second.empty()) return nullptr;
    return found->second[seed % found->second.size()];
}

} // namespace rm::audio
