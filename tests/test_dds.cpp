// DDS reader tests. Headers are built inline — the format is a fixed 128-byte
// preamble, so a helper here beats a separate writer module.
//
// The real BAR texture set (2552 files) is swept in the [real-model] tag section
// at the end, which is what proves the mip arithmetic against files nobody here
// wrote.
#include <catch2/catch_test_macros.hpp>

#include "core/texture/Dds.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void putU32(std::vector<std::byte>& out, std::size_t at, std::uint32_t v) {
    out[at + 0] = static_cast<std::byte>(v & 0xFFu);
    out[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFu);
    out[at + 2] = static_cast<std::byte>((v >> 16) & 0xFFu);
    out[at + 3] = static_cast<std::byte>((v >> 24) & 0xFFu);
}

struct DdsSpec {
    std::uint32_t width = 16;
    std::uint32_t height = 16;
    std::uint32_t mipCount = 1;
    bool mipCountFlagSet = true;
    std::string fourCc = "DXT1";
    bool validMagic = true;
    bool fourCcFlagSet = true;
    std::uint32_t headerSize = 124;
    /// Payload bytes to write; -1 means "exactly what the mip chain needs".
    std::int64_t payloadBytes = -1;
    /// Uncompressed 32-bit BGRA, the layout SupCom's embedded splat masks use.
    /// Clears the FOURCC flag and writes the channel masks the reader checks.
    bool bgra8 = false;
    /// Only meaningful with bgra8: writes a different channel order, to prove
    /// the reader checks rather than assumes.
    bool wrongChannelOrder = false;
};

[[nodiscard]] std::size_t blocksFor(std::uint32_t w, std::uint32_t h, std::size_t blockSize,
                                    std::uint32_t blockSpan = 4) {
    return static_cast<std::size_t>((std::max(1u, w) + blockSpan - 1) / blockSpan)
         * static_cast<std::size_t>((std::max(1u, h) + blockSpan - 1) / blockSpan) * blockSize;
}

[[nodiscard]] std::vector<std::byte> writeDds(const DdsSpec& spec) {
    std::vector<std::byte> out(rm::dds::kDataOffset, std::byte{0});

    const char* magic = spec.validMagic ? "DDS " : "XXX ";
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>(magic[i]);
    }

    putU32(out, 4, spec.headerSize);
    putU32(out, 8, spec.mipCountFlagSet ? 0x00020000u : 0u);  // DDSD_MIPMAPCOUNT
    putU32(out, 12, spec.height);
    putU32(out, 16, spec.width);
    putU32(out, 28, spec.mipCount);
    putU32(out, 76, 32);                                       // DDS_PIXELFORMAT.dwSize
    const bool fourCcFlag = spec.fourCcFlagSet && !spec.bgra8;
    putU32(out, 80, fourCcFlag ? 0x00000004u : 0u);            // DDPF_FOURCC
    for (std::size_t i = 0; i < 4 && i < spec.fourCc.size(); ++i) {
        out[84 + i] = static_cast<std::byte>(spec.fourCc[i]);
    }

    if (spec.bgra8) {
        putU32(out, 88, 32);  // dwRGBBitCount
        // Red at 0x00FF0000 is B G R A in memory order — what SupCom writes.
        putU32(out, 92, spec.wrongChannelOrder ? 0x000000FFu : 0x00FF0000u);
        putU32(out, 96, 0x0000FF00u);
        putU32(out, 100, spec.wrongChannelOrder ? 0x00FF0000u : 0x000000FFu);
        putU32(out, 104, 0xFF000000u);
    }

    // Payload: the full declared mip chain unless overridden. An uncompressed
    // format is a 1x1-block format, which is exactly how the reader sizes it.
    const std::size_t blockSize = spec.bgra8 ? 4u : ((spec.fourCc == "DXT1") ? 8u : 16u);
    const std::uint32_t blockSpan = spec.bgra8 ? 1u : 4u;
    std::size_t needed = 0;
    for (std::uint32_t level = 0; level < std::max(1u, spec.mipCount); ++level) {
        needed += blocksFor(spec.width >> level, spec.height >> level, blockSize, blockSpan);
    }

    const std::size_t payload =
        spec.payloadBytes < 0 ? needed : static_cast<std::size_t>(spec.payloadBytes);

    // Fill with the level index so slicing is observable.
    for (std::size_t i = 0; i < payload; ++i) {
        out.push_back(static_cast<std::byte>(i & 0xFF));
    }
    return out;
}

} // namespace

TEST_CASE("a DXT1 texture reports its dimensions and format") {
    const auto texture = rm::dds::load(writeDds(DdsSpec{}));

    REQUIRE(texture.has_value());
    REQUIRE(texture->width == 16);
    REQUIRE(texture->height == 16);
    REQUIRE(texture->format == rm::dds::Format::Bc1);
    REQUIRE(texture->mipLevels == 1);
}

TEST_CASE("each DXT variant maps to the right block format and size") {
    // DXT1 is 8 bytes per 4x4 block; DXT3 and DXT5 are 16.
    struct Case {
        std::string fourCc;
        rm::dds::Format format;
        std::size_t bytesPerBlock;
    };

    for (const Case& c : {Case{"DXT1", rm::dds::Format::Bc1, 8u},
                          Case{"DXT3", rm::dds::Format::Bc2, 16u},
                          Case{"DXT5", rm::dds::Format::Bc3, 16u}}) {
        DdsSpec spec;
        spec.fourCc = c.fourCc;

        const auto texture = rm::dds::load(writeDds(spec));
        REQUIRE(texture.has_value());
        REQUIRE(texture->format == c.format);
        REQUIRE(rm::dds::bytesPerBlock(texture->format) == c.bytesPerBlock);

        // 16x16 = 4x4 blocks.
        REQUIRE(texture->mipBytes(0) == 16 * c.bytesPerBlock);
        REQUIRE(texture->mipBytesPerRow(0) == 4 * c.bytesPerBlock);
    }
}

TEST_CASE("the mip chain slices at the right offsets") {
    DdsSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.mipCount = 5;  // 16, 8, 4, 2, 1

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE(texture.has_value());
    REQUIRE(texture->mipLevels == 5);

    REQUIRE(texture->mipWidth(0) == 16);
    REQUIRE(texture->mipWidth(2) == 4);
    // Dimensions clamp at 1 rather than reaching zero.
    REQUIRE(texture->mipWidth(4) == 1);
    REQUIRE(texture->mipHeight(4) == 1);

    // DXT1: 16x16 = 16 blocks = 128 B, 8x8 = 4 blocks = 32 B, then 8, 8, 8 —
    // levels below 4x4 texels still occupy a whole block.
    REQUIRE(texture->mipBytes(0) == 128);
    REQUIRE(texture->mipBytes(1) == 32);
    REQUIRE(texture->mipBytes(2) == 8);
    REQUIRE(texture->mipBytes(3) == 8);
    REQUIRE(texture->mipBytes(4) == 8);

    REQUIRE(texture->mipOffset(0) == 0);
    REQUIRE(texture->mipOffset(1) == 128);
    REQUIRE(texture->mipOffset(2) == 160);

    REQUIRE(texture->data.size() == 128 + 32 + 8 + 8 + 8);
    REQUIRE(texture->mip(0).size() == 128);
    REQUIRE(texture->mip(5).empty());
}

TEST_CASE("non-power-of-two dimensions round up to whole blocks") {
    DdsSpec spec;
    spec.width = 10;  // 3 blocks across
    spec.height = 6;  // 2 block rows

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE(texture.has_value());
    REQUIRE(texture->mipBytesPerRow(0) == 3 * 8);
    REQUIRE(texture->mipBytes(0) == 3 * 2 * 8);
}

TEST_CASE("mip count is ignored unless its flag is set") {
    // dwMipMapCount is only meaningful with DDSD_MIPMAPCOUNT; a stale value in an
    // otherwise mip-less file must not make us read past the payload.
    DdsSpec spec;
    spec.mipCount = 9;
    spec.mipCountFlagSet = false;
    spec.payloadBytes = 128;  // exactly level 0 of a 16x16 DXT1

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE(texture.has_value());
    REQUIRE(texture->mipLevels == 1);
}

TEST_CASE("a truncated mip chain yields the levels that are actually present") {
    // Tooling output with a short chain is common; refusing the whole texture
    // would be worse than sampling fewer levels.
    DdsSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.mipCount = 5;
    spec.payloadBytes = 128 + 32;  // only levels 0 and 1

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE(texture.has_value());
    REQUIRE(texture->mipLevels == 2);
    REQUIRE(texture->data.size() == 128 + 32);
}

TEST_CASE("a payload too small for even level 0 is rejected") {
    DdsSpec spec;
    spec.payloadBytes = 16;  // a 16x16 DXT1 needs 128

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().code == rm::MapError::Code::Truncated);
}

TEST_CASE("a file without the DDS magic is rejected") {
    DdsSpec spec;
    spec.validMagic = false;

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().code == rm::MapError::Code::NotSmf);
}

TEST_CASE("an uncompressed BGRA8 DDS loads, sized as 1x1 blocks") {
    // The layout SupCom embeds its two splat weight masks in. Sizing is the
    // thing to get right: 4 bytes per texel with no 4x4 rounding, so a 16x16
    // surface is 1024 bytes and its row is 64 — not the 128-byte row a
    // block-compressed format of the same width would have.
    DdsSpec spec;
    spec.bgra8 = true;

    const auto texture = rm::dds::load(writeDds(spec));

    REQUIRE(texture.has_value());
    REQUIRE(texture->format == rm::dds::Format::Bgra8);
    REQUIRE(texture->width == 16);
    REQUIRE(texture->height == 16);
    REQUIRE(texture->mipBytesPerRow(0) == 16 * 4);
    REQUIRE(texture->mipBytes(0) == 16 * 16 * 4);
    REQUIRE(texture->mip(0).size() == 16 * 16 * 4);
}

TEST_CASE("a non-square uncompressed surface is not rounded up to blocks") {
    // 6x3 is the case that separates the two sizings: as 1x1 blocks it is
    // 6*3*4 = 72 bytes, but rounded to 4x4 blocks it would be 2*1 blocks.
    DdsSpec spec;
    spec.bgra8 = true;
    spec.width = 6;
    spec.height = 3;

    const auto texture = rm::dds::load(writeDds(spec));

    REQUIRE(texture.has_value());
    REQUIRE(texture->mipBytesPerRow(0) == 6 * 4);
    REQUIRE(texture->mipBytes(0) == 6 * 3 * 4);
}

TEST_CASE("an uncompressed DDS in another channel order is declined") {
    // BGRA is accepted because it is what SupCom writes and what Metal takes
    // natively. RGBA is legal DDS and would load as a recoloured image — which
    // reads as an art bug, not a reader bug — so it is refused instead.
    DdsSpec spec;
    spec.bgra8 = true;
    spec.wrongChannelOrder = true;

    const auto texture = rm::dds::load(writeDds(spec));

    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().message.find("channel layout") != std::string::npos);
}

TEST_CASE("an unsupported FOURCC names itself in the error") {
    // DX10-extended headers land here. The message must say which code it saw,
    // because "unsupported format" alone is useless when debugging one file out
    // of thousands.
    DdsSpec spec;
    spec.fourCc = "DX10";

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().message.find("DX10") != std::string::npos);
}

TEST_CASE("a wrong header size is rejected") {
    DdsSpec spec;
    spec.headerSize = 32;

    const auto texture = rm::dds::load(writeDds(spec));
    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().code == rm::MapError::Code::BadHeader);
}

TEST_CASE("a DDS shorter than its header is rejected") {
    auto bytes = writeDds(DdsSpec{});
    bytes.resize(rm::dds::kDataOffset - 1);

    const auto texture = rm::dds::load(bytes);
    REQUIRE_FALSE(texture.has_value());
    REQUIRE(texture.error().code == rm::MapError::Code::Truncated);
}

// --- Real corpus ------------------------------------------------------------

namespace {

[[nodiscard]] std::filesystem::path textureDir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::filesystem::path{home}
         / "projects/llm/games/forged-alliance-reborn/reference/BAR/unittextures";
}

} // namespace

TEST_CASE("the real BAR texture corpus parses", "[real-model]") {
    std::error_code ec;
    if (!std::filesystem::is_directory(textureDir(), ec)) {
        SKIP("no .dds corpus at " + textureDir().string());
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{textureDir(), ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".dds") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        SKIP("no .dds files found");
    }

    std::size_t bc1 = 0;
    std::size_t bc2 = 0;
    std::size_t bc3 = 0;
    std::size_t bgra8 = 0;
    std::vector<std::string> declined;

    for (const auto& path : files) {
        const auto texture = rm::dds::loadFile(path);
        if (!texture) {
            declined.push_back(path.filename().string() + ": " + texture.error().message);
            continue;
        }

        switch (texture->format) {
            case rm::dds::Format::Bc1: ++bc1; break;
            case rm::dds::Format::Bc2: ++bc2; break;
            case rm::dds::Format::Bc3: ++bc3; break;
            case rm::dds::Format::Bgra8: ++bgra8; break;
        }

        // Every declared level must be sliceable — this is the check that fires
        // if the block arithmetic is wrong for an odd size.
        for (int level = 0; level < texture->mipLevels; ++level) {
            REQUIRE_FALSE(texture->mip(level).empty());
        }
        REQUIRE(texture->width > 0);
        REQUIRE(texture->height > 0);
    }

    // DXT5 dominates BAR's unit textures, which is why BC3 support was not
    // optional. Recorded as a proportion rather than a count so a corpus refresh
    // does not fail the test.
    REQUIRE(bc3 > bc1);
    REQUIRE(bc1 + bc2 + bc3 + bgra8 > files.size() / 2);

    // Anything declined should be a genuinely exotic format, not a bulk failure.
    if (declined.size() > files.size() / 10) {
        std::string report = std::to_string(declined.size()) + " of "
                           + std::to_string(files.size()) + " textures declined:";
        for (std::size_t i = 0; i < declined.size() && i < 10; ++i) {
            report += "\n  " + declined[i];
        }
        FAIL(report);
    }
}
