// What a Supreme Commander stratum normal map actually stores.
//
// The blend is settled — terrain.fx's TerrainNormalsXP is a chain of lerps, the
// same shape as the albedo chain (ADR-009). What the shader does NOT say is
// which channel points up: it samples `tex2D(...)*2-1`, hands the result
// straight to CalculateLighting, and that function's own space is muddled by
// .xzy swizzles elsewhere in the file. Porting it needs the axis convention,
// and inferring one from a shader that never states it is the mistake ADR-009
// exists to prevent.
//
// So it is measured against the real textures. A tangent-space normal map is
// "flat" almost everywhere, so the channel whose mean sits near 255 — decoding
// to +1 — is the up axis, and the other two sit near 128, decoding to 0.
#include <catch2/catch_test_macros.hpp>

#include "core/texture/Dds.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Where the stratum textures were extracted — the same root main.mm uses.
[[nodiscard]] std::filesystem::path envRoot() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home == nullptr ? "" : home} / "projects/llm/input/faf/env";
}

[[nodiscard]] std::vector<std::filesystem::path> normalMaps() {
    std::vector<std::filesystem::path> found;

    std::error_code ec;
    if (!std::filesystem::is_directory(envRoot(), ec)) {
        return found;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator{envRoot(), ec}) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".dds") {
            continue;
        }
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("normal") != std::string::npos) {
            found.push_back(entry.path());
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

/// Mean r, g, b of a texture, from each block's two endpoint colours.
///
/// Not a decoder. A BC block interpolates between two RGB565 endpoints, so
/// averaging the endpoints estimates the block's mean colour closely enough to
/// tell 255 from 128 — which is the entire question here. Writing a real BC
/// decoder to answer it would be a decoder nothing else in this project needs,
/// since blocks go to the GPU verbatim.
[[nodiscard]] std::array<double, 3> endpointMeans(const rm::dds::Texture& texture) {
    const std::span<const std::byte> level0 = texture.mip(0);
    const std::size_t blockBytes = rm::dds::bytesPerBlock(texture.format);

    // Where the colour endpoints start within a block. BC3 spends its first
    // eight bytes on alpha; BC1 and BC2 put colour first and last respectively.
    const std::size_t colourOffset = texture.format == rm::dds::Format::Bc3   ? 8u
                                   : texture.format == rm::dds::Format::Bc2   ? 8u
                                                                              : 0u;

    std::array<double, 3> sum{};
    std::size_t counted = 0;

    for (std::size_t at = 0; at + blockBytes <= level0.size(); at += blockBytes) {
        for (std::size_t endpoint = 0; endpoint < 2; ++endpoint) {
            const std::size_t at565 = at + colourOffset + endpoint * 2;
            const auto low = static_cast<std::uint16_t>(level0[at565]);
            const auto high = static_cast<std::uint16_t>(level0[at565 + 1]);
            const auto packed = static_cast<std::uint16_t>(low | (high << 8));

            // RGB565 -> 0..255, replicating the high bits into the low ones as
            // every hardware decoder does.
            const auto r5 = static_cast<double>((packed >> 11) & 0x1F);
            const auto g6 = static_cast<double>((packed >> 5) & 0x3F);
            const auto b5 = static_cast<double>(packed & 0x1F);
            sum[0] += r5 * 255.0 / 31.0;
            sum[1] += g6 * 255.0 / 63.0;
            sum[2] += b5 * 255.0 / 31.0;
            ++counted;
        }
    }

    if (counted == 0) {
        return {-1.0, -1.0, -1.0};
    }
    for (double& channel : sum) {
        channel /= static_cast<double>(counted);
    }
    return sum;
}

} // namespace

TEST_CASE("a stratum normal map points up in BLUE, so blue is the axis to keep") {
    const std::vector<std::filesystem::path> maps = normalMaps();
    if (maps.empty()) {
        SKIP("extracted Supreme Commander env textures not present");
    }

    // A sample rather than all of them: the convention is a format-wide fact,
    // and reading every file costs seconds for no more certainty.
    const std::size_t sample = std::min<std::size_t>(maps.size(), 30);

    std::size_t checked = 0;
    for (std::size_t i = 0; i < sample; ++i) {
        const auto texture = rm::dds::loadFile(maps[i]);
        if (!texture) {
            continue;  // a format the loader does not read is not this test's subject
        }
        if (texture->format == rm::dds::Format::Bgra8) {
            continue;  // the endpoint trick above is about block formats
        }

        const std::array<double, 3> mean = endpointMeans(*texture);
        if (mean[0] < 0.0) {
            continue;
        }

        INFO(maps[i].filename().string() << "  r=" << mean[0] << " g=" << mean[1]
                                         << " b=" << mean[2]);

        // Blue near +1, red and green near 0. If this ever fails on a real
        // file, the convention is not what the shader port assumes — and a
        // normal map read on the wrong axis lights bumps as dents, which is
        // subtle enough to survive a look at the screen.
        CHECK(mean[2] > mean[0]);
        CHECK(mean[2] > mean[1]);
        CHECK(mean[2] > 190.0);
        ++checked;
    }

    REQUIRE(checked > 0);
}
