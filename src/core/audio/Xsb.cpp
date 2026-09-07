#include "core/audio/Xsb.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace rm::audio {
namespace {

/// Bounds-checked little-endian reads. Out of range reads yield zero, and every consumer
/// treats zero as "nothing here", so a truncated bank ends a walk instead of faulting.
struct Reader {
    const std::vector<std::uint8_t>& data;

    [[nodiscard]] bool has(std::size_t at, std::size_t bytes) const noexcept {
        return at <= data.size() && bytes <= data.size() - at;
    }
    [[nodiscard]] std::uint8_t u8(std::size_t at) const noexcept {
        return has(at, 1) ? data[at] : 0;
    }
    [[nodiscard]] std::uint16_t u16(std::size_t at) const noexcept {
        return has(at, 2) ? static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8)) : 0;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const noexcept {
        return has(at, 4) ? (std::uint32_t{data[at]} | (std::uint32_t{data[at + 1]} << 8)
                             | (std::uint32_t{data[at + 2]} << 16)
                             | (std::uint32_t{data[at + 3]} << 24))
                          : 0;
    }
    [[nodiscard]] std::string fixedName(std::size_t at, std::size_t length) const {
        if (!has(at, length)) return {};
        const auto* begin = reinterpret_cast<const char*>(data.data() + at);
        return std::string{begin, static_cast<std::size_t>(std::strlen(begin) < length
                                                                ? std::strlen(begin)
                                                                : length)};
    }
};

// Header layout, measured on retail format 43 (UELWeapon.xsb):
//   0 'SDBK'  4 tool u16  6 format u16  8 crc u16  10 modified u64  18 platform u8
//  19 simple cues u16  21 complex cues u16  23 unused u16  25 total cues u16
//  27 wave banks u8  28 sounds u16  30 cue name table length u16  32 unused u16
//  34 simple cues offset  38 complex cues offset  42 cue names offset  46, 50, 54 unused
//  58 wave bank names offset  62 cue name hash table  66 cue name hash values
//  70 sounds offset  74 bank name (64)
constexpr std::size_t kHeaderSize = 138;
constexpr std::uint32_t kAbsent = 0xffffffffu;
constexpr std::size_t kNameLength = 64;
constexpr std::size_t kComplexCueSize = 15;
constexpr std::size_t kSimpleCueSize = 5;
constexpr std::uint8_t kCueHasSound = 0x04;   ///< complex cue: offset is a sound, not a table
constexpr std::uint8_t kSoundComplex = 0x01;  ///< sound owns clips rather than one track
constexpr std::uint8_t kSoundHasRpc = 0x0e;   ///< an RPC block (u16 length, inclusive) follows
constexpr std::uint8_t kSoundHasDsp = 0x10;   ///< a DSP block (u16 length, inclusive) follows
constexpr std::size_t kSoundHeader = 9;
constexpr std::size_t kClipEntry = 5;         ///< volume u8 + clip offset u32
constexpr std::size_t kEventHeader = 6;       ///< info u32 (type in the low 5 bits) + random u16
constexpr std::size_t kVariationEntry = 5;    ///< track u16, wave bank u8, weight min/max u8

/// Appends the tracks of the wave events in one clip, and the first authored pitch range it
/// carries. Pitch lives per EVENT (the whole variation list shares it): type 4 names one wave
/// with ranges after track/bank, type 6 is a weighted list whose ranges sit three bytes
/// earlier because it carries no track/bank of its own. Hundredths of a semitone, measured on
/// the retail banks: weapon cues sit in ±100..±300.
void readClip(const Reader& r, std::size_t at, std::vector<SoundBank::Track>& out,
              std::int16_t& pitchMin, std::int16_t& pitchMax) {
    const std::uint8_t events = r.u8(at);
    std::size_t o = at + 1;
    for (std::uint8_t e = 0; e < events; ++e) {
        const std::uint32_t info = r.u32(o);
        const std::uint32_t type = info & 0x1fu;
        o += kEventHeader;
        switch (type) {
        case 1:  // play one wave: flags u8, track u16, bank u8, loop u8, pan u16 x2
            out.push_back({.entry = r.u16(o + 1), .waveBank = r.u8(o + 3)});
            o += 9;
            break;
        case 4: {  // one wave with pitch/volume/filter ranges: flags, loop, track, bank, 12 bytes
            out.push_back({.entry = r.u16(o + 2), .waveBank = r.u8(o + 4)});
            pitchMin = static_cast<std::int16_t>(r.u16(o + 10));
            pitchMax = static_cast<std::int16_t>(r.u16(o + 12));
            o += 17;
            break;
        }
        case 3:    // weighted track list without ranges
        case 6: {  // weighted track list with pitch/volume/filter ranges (12 bytes)
            if (type == 6) {
                pitchMin = static_cast<std::int16_t>(r.u16(o + 7));
                pitchMax = static_cast<std::int16_t>(r.u16(o + 9));
                o += 14;
            } else {
                o += 2;
            }
            const std::uint16_t count = r.u16(o);
            o += 2 + 6;  // count, then variation flags u16 and four unused bytes
            for (std::uint16_t v = 0; v < count; ++v) {
                if (!r.has(o, kVariationEntry)) return;
                out.push_back({.entry = r.u16(o), .waveBank = r.u8(o + 2)});
                o += kVariationEntry;
            }
            break;
        }
        default:
            return;  // stop, marker, or a kind this walk does not know: the cue ends here
        }
    }
}

/// Appends the tracks of one sound record. The category the sound names rides along: an
/// index into the global settings' category table, where the per-category instance limits
/// live.
void readSound(const Reader& r, std::size_t at, std::vector<SoundBank::Track>& out,
               std::int16_t& pitchMin, std::int16_t& pitchMax, std::uint16_t& category) {
    if (!r.has(at, kSoundHeader)) return;
    const std::uint8_t flags = r.u8(at);
    category = r.u16(at + 1);
    std::size_t o = at + kSoundHeader;
    std::uint8_t clips = 0;
    if ((flags & kSoundComplex) != 0) {
        clips = r.u8(o);
        o += 1;
    } else {
        out.push_back({.entry = r.u16(o), .waveBank = r.u8(o + 2)});
        o += 3;
    }
    if ((flags & kSoundHasRpc) != 0) o += r.u16(o);
    if ((flags & kSoundHasDsp) != 0) o += r.u16(o);
    for (std::uint8_t c = 0; c < clips; ++c) {
        const std::uint32_t clipAt = r.u32(o + 1 + c * kClipEntry);
        if (clipAt != 0 && r.has(clipAt, 1)) readClip(r, clipAt, out, pitchMin, pitchMax);
    }
}

/// A variation table: a cue that picks among sounds or tracks by weight.
void readVariationTable(const Reader& r, std::size_t at, std::vector<SoundBank::Track>& out,
                        std::int16_t& pitchMin, std::int16_t& pitchMax,
                        std::uint16_t& category) {
    const std::uint16_t count = r.u16(at);
    const std::uint16_t flags = r.u16(at + 2);
    const std::uint16_t kind = static_cast<std::uint16_t>((flags >> 3) & 0x7u);
    std::size_t o = at + 7;  // count, flags, one unused byte, variation flags u16
    for (std::uint16_t i = 0; i < count; ++i) {
        switch (kind) {
        case 0:  // wave: track u16, bank u8, weight min/max
            out.push_back({.entry = r.u16(o), .waveBank = r.u8(o + 2)});
            o += 5;
            break;
        case 4:  // compact wave: track u16, bank u8
            out.push_back({.entry = r.u16(o), .waveBank = r.u8(o + 2)});
            o += 3;
            break;
        case 1:  // sound offset u32, weight min/max u8
            readSound(r, r.u32(o), out, pitchMin, pitchMax, category);
            o += 6;
            break;
        case 3:  // sound offset u32, weights f32 x2, flags u32
            readSound(r, r.u32(o), out, pitchMin, pitchMax, category);
            o += 16;
            break;
        default:
            return;
        }
    }
}

} // namespace

std::optional<SoundBank> parseSoundBank(const std::vector<std::uint8_t>& bytes) {
    const Reader r{bytes};
    if (!r.has(0, kHeaderSize) || std::memcmp(bytes.data(), "SDBK", 4) != 0) {
        return std::nullopt;
    }
    SoundBank bank;
    bank.name = r.fixedName(74, kNameLength);
    const std::uint16_t simpleCues = r.u16(19);
    const std::uint16_t complexCues = r.u16(21);
    const std::uint16_t totalCues = r.u16(25);
    const std::uint8_t waveBanks = r.u8(27);
    const std::uint32_t simpleAt = r.u32(34);
    const std::uint32_t complexAt = r.u32(38);
    const std::uint32_t namesAt = r.u32(42);
    const std::uint32_t waveBankNamesAt = r.u32(58);

    for (std::uint8_t i = 0; i < waveBanks && waveBankNamesAt != kAbsent; ++i) {
        bank.waveBanks.push_back(r.fixedName(waveBankNamesAt + i * kNameLength, kNameLength));
    }

    // Cue names are packed null-terminated strings, simple cues first, in entry order.
    std::vector<std::string> names;
    if (namesAt != kAbsent) {
        std::size_t o = namesAt;
        for (std::uint16_t i = 0; i < totalCues && o < bytes.size(); ++i) {
            const auto* begin = reinterpret_cast<const char*>(bytes.data() + o);
            const std::size_t room = bytes.size() - o;
            const std::size_t length = static_cast<std::size_t>(strnlen(begin, room));
            names.emplace_back(begin, length);
            o += length + 1;
        }
    }

    for (std::uint16_t i = 0; i < totalCues && i < names.size(); ++i) {
        std::vector<SoundBank::Track> tracks;
        std::int16_t pitchMin = 0;
        std::int16_t pitchMax = 0;
        std::uint16_t category = 0xffff;
        if (i < simpleCues) {
            if (simpleAt != kAbsent) {
                readSound(r, r.u32(simpleAt + i * kSimpleCueSize + 1), tracks, pitchMin,
                          pitchMax, category);
            }
        } else if (complexAt != kAbsent) {
            const std::size_t at = complexAt + (i - simpleCues) * kComplexCueSize;
            const std::uint8_t flags = r.u8(at);
            const std::uint32_t target = r.u32(at + 1);
            if ((flags & kCueHasSound) != 0) {
                readSound(r, target, tracks, pitchMin, pitchMax, category);
            } else {
                readVariationTable(r, target, tracks, pitchMin, pitchMax, category);
            }
        }
        bank.cues[names[i]] = SoundBank::Cue{.tracks = std::move(tracks),
                                             .pitchMin = pitchMin,
                                             .pitchMax = pitchMax,
                                             .category = category};
    }
    (void)complexCues;
    return bank;
}

std::optional<SoundBank> loadSoundBank(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    return parseSoundBank(bytes);
}

} // namespace rm::audio
