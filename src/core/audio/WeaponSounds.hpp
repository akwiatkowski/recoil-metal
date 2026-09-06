#pragma once

// Weapon fire, by the blueprint's own cue.
//
// `Audio.Fire = Sound { Bank = 'UELWeapon', Cue = 'UEL0201_Cannon_Sgl' }` names a sound bank
// (`Xsb.hpp`) and a cue in it; the cue names one or more entries of the like-named wave bank
// (`Xwb.hpp`). This class holds that chain for every weapon the catalog knows: banks load on
// first reference and stay loaded, and a `WeaponFired` event's `visualId` — the same
// `UNIT:Label` key the weapon visuals use — resolves to one of the cue's waves. Types are
// learned incrementally, because factories register new types while the match runs.
//
// Presentation only, and deterministic: the variation is picked by the shooter's handle, not
// by a clock, so a replay sounds like the match it replays.

#include "core/audio/Mixer.hpp"
#include "core/audio/Xsb.hpp"
#include "core/audio/Xwb.hpp"
#include "core/sim/UnitCatalog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::audio {

class WeaponSounds {
public:
    /// `soundsDir` is `<install>/sounds`, where retail keeps its 80 `.xsb`/`.xwb` pairs.
    explicit WeaponSounds(std::filesystem::path soundsDir);

    /// Learns the fire cues of every type registered since the last call. Cheap when nothing
    /// new was registered; loads a bank pair the first time a weapon names it.
    void learn(const sim::UnitCatalog& catalog);

    /// The wave for this shot, or null when the weapon names no bank, the bank is absent, or
    /// the cue resolved to no wave. `seed` picks among the cue's variations.
    [[nodiscard]] const Cue* cueFor(std::string_view key, std::uint32_t seed) const;

    /// Banks loaded so far, and weapons with a playable cue — for the startup report.
    [[nodiscard]] std::size_t bankCount() const noexcept { return loaded_; }
    [[nodiscard]] std::size_t weaponCount() const noexcept { return byKey_.size(); }

private:
    /// A wave bank by name, or nothing after a failed attempt so a missing file is asked for
    /// once. Nodes never move, so pointers into `entries` stay valid for the match.
    const WaveBank* wavesNamed(const std::string& name);
    const SoundBank* cuesNamed(const std::string& name);

    std::filesystem::path dir_;
    std::map<std::string, std::optional<WaveBank>> waves_;
    std::map<std::string, std::optional<SoundBank>> cues_;
    std::map<std::string, std::vector<const Cue*>, std::less<>> byKey_;
    std::size_t learnedTypes_ = 0;
    std::size_t loaded_ = 0;
};

} // namespace rm::audio
