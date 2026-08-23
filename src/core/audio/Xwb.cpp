#include "core/audio/Xwb.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace rm::audio {
namespace {

[[nodiscard]] std::uint32_t u32(const std::vector<std::byte>& data, std::size_t at) {
    if (at + 4 > data.size()) {
        return 0;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + at, 4);
    return value;  // little-endian file, little-endian machine — arm64 macOS is
}

/// int16 PCM (or uint8, the format's other arm) into mono float at the mixer's rate.
/// Linear resampling: these are 0.1–3 s battlefield effects at 32 kHz going to 48 kHz,
/// where linear's error is far below the mix floor.
[[nodiscard]] Cue decodePcm(const std::vector<std::byte>& data, std::size_t offset,
                            std::size_t length, unsigned channels, unsigned sourceRate,
                            bool sixteenBit) {
    Cue cue;
    if (channels == 0 || sourceRate == 0 || offset + length > data.size()) {
        return cue;
    }
    const std::size_t bytesPerSample = sixteenBit ? 2 : 1;
    const std::size_t frames = length / (bytesPerSample * channels);
    if (frames == 0) {
        return cue;
    }

    std::vector<float> mono(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (unsigned channel = 0; channel < channels; ++channel) {
            const std::size_t at = offset + (frame * channels + channel) * bytesPerSample;
            if (sixteenBit) {
                std::int16_t sample = 0;
                std::memcpy(&sample, data.data() + at, 2);
                sum += static_cast<float>(sample) / 32768.0f;
            } else {
                sum += (static_cast<float>(std::to_integer<std::uint8_t>(data[at])) - 128.0f)
                       / 128.0f;
            }
        }
        mono[frame] = sum / static_cast<float>(channels);
    }

    const float step = static_cast<float>(sourceRate) / kSampleRate;
    const auto outFrames =
        static_cast<std::size_t>(static_cast<float>(frames) / step);
    cue.samples.resize(outFrames);
    for (std::size_t i = 0; i < outFrames; ++i) {
        const float position = static_cast<float>(i) * step;
        const auto whole = static_cast<std::size_t>(position);
        const float fraction = position - static_cast<float>(whole);
        const float a = mono[whole];
        const float b = whole + 1 < frames ? mono[whole + 1] : a;
        cue.samples[i] = a + (b - a) * fraction;
    }
    return cue;
}

} // namespace

std::optional<WaveBank> loadWaveBank(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return std::nullopt;
    }
    const std::streamsize size = in.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    if (data.size() < 52 || std::memcmp(data.data(), "WBND", 4) != 0) {
        return std::nullopt;
    }

    const std::uint32_t version = u32(data, 4);
    // XACT2 (42+) carries a second header-version word before the segment table; retail FA
    // is 43 throughout. Older layouts are not decoded — nothing here ships them.
    if (version < 42) {
        return std::nullopt;
    }
    struct Segment {
        std::uint32_t offset;
        std::uint32_t length;
    };
    Segment segments[5];
    for (std::size_t i = 0; i < 5; ++i) {
        segments[i] = {u32(data, 12 + i * 8), u32(data, 16 + i * 8)};
    }
    const std::uint32_t bank = segments[0].offset;
    const std::uint32_t entryCount = u32(data, bank + 4);
    const std::uint32_t metaSize = u32(data, bank + 72);
    const std::uint32_t nameSize = u32(data, bank + 76);
    if (entryCount == 0 || metaSize < 24) {
        return std::nullopt;
    }

    WaveBank loaded;
    const char* bankName = reinterpret_cast<const char*>(data.data() + bank + 8);
    loaded.name.assign(bankName, strnlen(bankName, 64));

    const Segment meta = segments[1];
    const Segment names = segments[3];
    const Segment waves = segments[4];
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        const std::size_t at = meta.offset + static_cast<std::size_t>(i) * metaSize;
        const std::uint32_t format = u32(data, at + 4);
        const std::uint32_t playOffset = u32(data, at + 8);
        const std::uint32_t playLength = u32(data, at + 12);

        const std::uint32_t tag = format & 0b11;
        const unsigned channels = (format >> 2) & 0b111;
        const unsigned rate = (format >> 5) & 0x3FFFF;
        const bool sixteenBit = ((format >> 31) & 1) != 0;

        std::string cueName;
        if (nameSize > 0 && names.length >= (i + 1) * nameSize) {
            const char* text =
                reinterpret_cast<const char*>(data.data() + names.offset + i * nameSize);
            cueName.assign(text, strnlen(text, nameSize));
        }
        if (tag != 0) {
            loaded.entries.emplace_back();  // hold the index; see the header note
            continue;
        }
        Cue cue = decodePcm(data, waves.offset + playOffset, playLength, channels, rate,
                            sixteenBit);
        if (!cueName.empty() && !cue.samples.empty()) {
            loaded.cues.emplace(std::move(cueName), cue);
        }
        loaded.entries.push_back(std::move(cue));
    }
    if (loaded.entries.empty()) {
        return std::nullopt;
    }
    return loaded;
}

} // namespace rm::audio
