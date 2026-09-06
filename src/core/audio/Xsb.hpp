#pragma once

// XACT sound banks — the names the blueprints use, resolved to wave-bank entries.
//
// A wave bank (`Xwb.hpp`) is an array of waves with no names; the companion `.xsb` is where
// `Audio.Fire = Sound { Bank = 'UELWeapon', Cue = 'UEL0201_Cannon_Sgl' }` becomes "entries 6,
// 5 or 7 of UELWeapon.xwb, one of them per shot". Retail FA ships 80 of them beside the 78
// wave banks under `<install>/sounds`, all XACT2 format 43.
//
// THE WALK, read off the retail bytes rather than a spec (the layout notes below name the
// offsets measured on `UELWeapon.xsb`): header → cue names (null-terminated, in cue order) →
// complex cues (15 bytes each; flag bit 2 means the entry points straight at a sound, else at
// a variation table) → sound (9-byte header; complex sounds own clips, simple ones name one
// track) → clip → events. Two event kinds carry waves in the retail banks: type 4, one track
// with pitch and volume ranges, and type 6, a weighted list of tracks. Only track indices are
// kept; pitch and volume variation, RPC curves, categories and instance limits are not
// interpreted yet, and every one of those omissions is a cue playing at its authored wave and
// the mixer's gain rather than a wrong cue.
//
// Anything the walk does not understand ends that cue's list where it stands, so a malformed
// or unexpected bank degrades per cue and never throws.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rm::audio {

struct SoundBank {
    std::string name;
    /// The wave banks this bank's tracks index into, in the order the entries name them.
    std::vector<std::string> waveBanks;
    /// One track per playable variation: which wave bank (index into `waveBanks`) and which
    /// entry in it.
    struct Track {
        std::uint16_t entry = 0;
        std::uint8_t waveBank = 0;
    };
    /// Cue name -> its tracks. A cue with an empty list resolved to no wave this walk
    /// understands.
    std::map<std::string, std::vector<Track>, std::less<>> cues;
};

/// Parses one sound bank from its bytes, or nothing when they are not an XACT2 sound bank.
[[nodiscard]] std::optional<SoundBank> parseSoundBank(const std::vector<std::uint8_t>& bytes);

/// Loads one bank from disk; absent or foreign files yield nothing.
[[nodiscard]] std::optional<SoundBank> loadSoundBank(const std::filesystem::path& path);

} // namespace rm::audio
