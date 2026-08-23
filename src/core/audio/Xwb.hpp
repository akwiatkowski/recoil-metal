#pragma once

// XACT wave banks — Forged Alliance's own sounds, decoded.
//
// The engine's audio ships as 89 loose `.xwb` files under `<install>/sounds`, and the cue
// system this project first shipped assumed they were xWMA and synthesised placeholders
// instead. MEASURED, that assumption was wrong: every one of the 1,737 entries across all
// 89 retail banks is tag 0 — plain little-endian PCM, mostly 16-bit mono at 32 kHz — so
// decoding them needs a header walk and a resample, not a codec. The blueprints wire units
// to entries BY NAME (`Audio.Fire = Sound { Bank = 'UELWeapon', Cue = 'UEL0201_Cannon_Sgl' }`),
// which is why a bank loads into a name-keyed cue map.
//
// The layout is XACT2's (dwVersion 42+, retail FA ships 43): 'WBND', two version words, a
// five-segment table (bank data, entry metadata, seek tables, entry names, wave data), and
// per-entry a packed format DWORD — tag:2, channels:3, rate:18, align:8, bits:1 — plus the
// play region into the wave-data segment. Only the PCM tag is decoded; any other tag skips
// the entry with its name intact, so a hypothetical modded bank degrades per-cue rather
// than per-bank.

#include "core/audio/Mixer.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rm::audio {

struct WaveBank {
    std::string name;

    /// Every entry in bank order, decoded — the address space the companion `.xsb` cue
    /// banks resolve into. Retail FA's wave banks carry NO entry names (their flags say
    /// seek-tables only), so until the `.xsb` sound/clip/event chain is reversed, INDEX is
    /// the honest address: an explosions bank is thirteen interchangeable booms, and
    /// picking one deterministically needs no name.
    std::vector<Cue> entries;

    /// Entry name -> entry, for a bank that does carry names. Empty on all 89 retail
    /// banks; kept because the loader reads the segment when present.
    std::map<std::string, Cue, std::less<>> cues;
};

/// Loads one bank, or nothing when the file is absent or not a wave bank. Stereo entries
/// downmix, foreign rates linear-resample to the mixer's — battlefield effects, not
/// mastering work.
[[nodiscard]] std::optional<WaveBank> loadWaveBank(const std::filesystem::path& path);

} // namespace rm::audio
