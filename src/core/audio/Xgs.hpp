#pragma once

// XACT global settings — SupCom.xgs, the authored mix the banks assume.
//
// The sound banks name CATEGORIES (Xsb.hpp: a cue's sound record carries a category index);
// the global settings file is where those categories are declared, and where two things this
// engine wants live:
//
//   * PER-CATEGORY INSTANCE LIMITS — how many voices of "Weapons" or "Default" may sound at
//     once. 0xffff means unlimited. Measured on the retail file: 39 categories, "Weapons" is
//     unlimited, "Global" caps at 24, "Default" at 4.
//   * DISTANCE→VOLUME CURVES (RPCs) whose far end is -9600 (XACT hundredths of a dB = silence).
//     The engine-side LodCutoff the blueprints name (`LodCutoff = 'Weapon_LodCutoff'`) rides
//     these curves: beyond the last point the cue is inaudible, whatever its volume. The
//     record header that binds a curve to its variable is not yet decoded, so the parser
//     collects every curve's silence distance and exposes their median as the default cutoff
//     — a heuristic, stated as one, pending a full RPC record walk.
//
// The walk: header (counts, then a table of section offsets) → category records (u32 name
// offset + u16 max instances, six bytes each) → the RPC curve section, scanned for descending
// point runs that end in silence. All offsets measured on the retail SupCom.xgs, format 42.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rm::audio {

struct GlobalSettings {
    struct Category {
        std::string name;
        /// Voices of this category allowed at once; 0xffff is the author's "unlimited".
        std::uint16_t maxInstances = 0xffff;
    };

    std::vector<Category> categories;

    /// Every curve's silence distance, in elmos — the x of the last point of each
    /// distance→volume run that ends at silence.
    std::vector<float> silenceDistances;

    /// The default LodCutoff: the median silence distance among world-scale curves. Retail
    /// weapon curves cut between ~2450 and ~6500 elmos; the median keeps one UI blip's
    /// 180-elmo curve from pulling the whole battlefield silent.
    [[nodiscard]] float cutoffElmos() const;

    /// The instance limit a category name carries, or 0xffff when the name is unknown —
    /// unlimited, the same default XACT gives a category nobody capped.
    [[nodiscard]] std::uint16_t maxInstancesOf(const std::string& name) const;
};

/// Parses one global settings file, or nothing when the bytes are not an XGSF bank.
[[nodiscard]] std::optional<GlobalSettings> parseGlobalSettings(const std::vector<std::uint8_t>& bytes);

/// Loads one settings file from disk; absent or foreign files yield nothing.
[[nodiscard]] std::optional<GlobalSettings> loadGlobalSettings(const std::filesystem::path& path);

} // namespace rm::audio
