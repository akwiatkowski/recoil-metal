#include "core/audio/Xgs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace rm::audio {
namespace {

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
    [[nodiscard]] float f32(std::size_t at) const noexcept {
        if (!has(at, 4)) return 0.0f;
        float value = 0.0f;
        std::memcpy(&value, data.data() + at, 4);
        return value;
    }
    [[nodiscard]] std::string name(std::size_t at) const {
        if (!has(at, 1)) return {};
        const auto* begin = reinterpret_cast<const char*>(data.data() + at);
        return std::string{begin, strnlen(begin, data.size() - at)};
    }
};

// Header layout, measured on retail format 42 (SupCom.xgs): 0 'XGSF'  4 tool u16  6 format
// u16  8 crc u16  10 modified u64  18 platform u8  19 categories u16  21 variables u16
// 23 curves u16  25 dsp u16  27 global variations u16  29..32 zero  then section offsets u32:
// 33, 37 (curve section), 41, 45 (CATEGORY RECORDS), 49, 53, 57 (category names), 61
// (variable names), 65 (variable section length).
constexpr std::size_t kHeaderSize = 69;
constexpr std::uint32_t kAbsent = 0xffffffffu;
constexpr std::size_t kCategoryRecord = 6;  // u32 name offset + u16 max instances

/// Silence, in XACT hundredths of a dB: every authored distance curve ends at it.
constexpr float kSilence = -9600.0f;

} // namespace

std::optional<GlobalSettings> parseGlobalSettings(const std::vector<std::uint8_t>& bytes) {
    const Reader r{bytes};
    if (!r.has(0, kHeaderSize) || std::memcmp(bytes.data(), "XGSF", 4) != 0) {
        return std::nullopt;
    }
    GlobalSettings settings;
    const std::uint16_t categories = r.u16(19);
    const std::uint32_t categoryRecords = r.u32(45);
    for (std::uint16_t i = 0; i < categories && categoryRecords != kAbsent; ++i) {
        const std::size_t at = categoryRecords + i * kCategoryRecord;
        if (!r.has(at, kCategoryRecord)) break;
        settings.categories.push_back(
            {.name = r.name(r.u32(at)), .maxInstances = r.u16(at + 4)});
    }

    // The curve section: distance→volume points as (f32 x, f32 y) pairs on a NINE-BYTE
    // stride in the retail file (two floats and a trailing byte this walk does not name;
    // eight in synthetic fixtures). The record headers are not decoded, so the scan reads
    // every alignment the bytes offer and keeps runs of two or more points with ascending x
    // and non-positive y descending toward silence; the last x of such a run is a distance
    // beyond which the author made the cue inaudible.
    const std::uint32_t curvesAt = r.u32(37);
    const std::uint32_t curvesEnd = r.u32(41);
    if (curvesAt != kAbsent && curvesEnd != kAbsent && curvesAt < curvesEnd
        && curvesEnd <= bytes.size()) {
        struct Point {
            float x;
            float y;
        };
        std::vector<std::pair<std::size_t, Point>> found;
        for (std::size_t o = curvesAt; o + 8 <= curvesEnd; ++o) {
            const float x = r.f32(o);
            const float y = r.f32(o + 4);
            if (x == x && y == y && x > 1.0f && x < 200000.0f && y <= 0.0f && y >= kSilence) {
                found.emplace_back(o, Point{x, y});
            }
        }
        std::size_t i = 0;
        while (i < found.size()) {
            std::size_t run = i;
            while (run + 1 < found.size()) {
                const std::size_t stride = found[run + 1].first - found[run].first;
                if ((stride != 8 && stride != 9)
                    || found[run + 1].second.x <= found[run].second.x
                    || found[run + 1].second.y > found[run].second.y) {
                    break;
                }
                ++run;
            }
            if (run > i && found[run].second.y <= kSilence + 100.0f) {
                settings.silenceDistances.push_back(found[run].second.x);
            }
            i = run + 1;
        }
    }
    return settings;
}

float GlobalSettings::cutoffElmos() const {
    // World-scale curves only: a UI blip's 180-elmo cutoff is real data about a different
    // question. The median of what remains is the default; no data means no cutoff.
    std::vector<float> world;
    for (const float distance : silenceDistances) {
        if (distance >= 500.0f) world.push_back(distance);
    }
    if (world.empty()) return 0.0f;
    std::sort(world.begin(), world.end());
    return world[world.size() / 2];
}

std::uint16_t GlobalSettings::maxInstancesOf(const std::string& name) const {
    for (const auto& category : categories) {
        if (category.name == name) return category.maxInstances;
    }
    return 0xffff;
}

std::optional<GlobalSettings> loadGlobalSettings(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::nullopt;
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
    return parseGlobalSettings(bytes);
}

} // namespace rm::audio
