#include "core/texture/Dds.hpp"

#include "core/map/ByteReader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Field offsets, counting from the start of the file (i.e. including the 4-byte
// magic), per the DDS_HEADER / DDS_PIXELFORMAT layout.
constexpr std::size_t kOffHeaderSize   = 4;    ///< must be 124
constexpr std::size_t kOffFlags        = 8;
constexpr std::size_t kOffHeight       = 12;
constexpr std::size_t kOffWidth        = 16;
constexpr std::size_t kOffMipCount     = 28;
constexpr std::size_t kOffPixelFlags   = 80;
constexpr std::size_t kOffFourCc       = 84;
// DDS_PIXELFORMAT's uncompressed description, used only when DDPF_FOURCC is off.
constexpr std::size_t kOffRgbBitCount  = 88;
constexpr std::size_t kOffRedMask      = 92;
constexpr std::size_t kOffGreenMask    = 96;
constexpr std::size_t kOffBlueMask     = 100;

/// DDSD_MIPMAPCOUNT — whether dwMipMapCount is meaningful.
constexpr std::uint32_t kFlagMipMapCount = 0x00020000u;
/// DDPF_FOURCC — whether the pixel format names a fourcc rather than masks.
constexpr std::uint32_t kPixelFlagFourCc = 0x00000004u;

using rm::MapError;

[[nodiscard]] const char* formatName(rm::dds::Format format) {
    switch (format) {
        case rm::dds::Format::Bc1: return "DXT1";
        case rm::dds::Format::Bc2: return "DXT3";
        case rm::dds::Format::Bc3: return "DXT5";
        case rm::dds::Format::Bgra8: return "BGRA8";
    }
    return "unknown";
}

[[nodiscard]] std::string fourCcText(std::span<const std::byte> bytes, std::size_t offset) {
    std::string text;
    for (std::size_t i = 0; i < 4 && offset + i < bytes.size(); ++i) {
        const auto c = static_cast<char>(bytes[offset + i]);
        text.push_back(std::isprint(static_cast<unsigned char>(c)) != 0 ? c : '?');
    }
    return text;
}

} // namespace

namespace rm::dds {

int Texture::mipWidth(int level) const noexcept {
    return std::max(1, width >> level);
}

int Texture::mipHeight(int level) const noexcept {
    return std::max(1, height >> level);
}

std::size_t Texture::mipBytesPerRow(int level) const noexcept {
    const int texels = blockTexels(format);
    const auto blocks = static_cast<std::size_t>((mipWidth(level) + texels - 1) / texels);
    return blocks * bytesPerBlock(format);
}

std::size_t Texture::mipBytes(int level) const noexcept {
    const int texels = blockTexels(format);
    const auto rows = static_cast<std::size_t>((mipHeight(level) + texels - 1) / texels);
    return mipBytesPerRow(level) * rows;
}

std::size_t Texture::mipOffset(int level) const noexcept {
    std::size_t offset = 0;
    for (int i = 0; i < level; ++i) {
        offset += mipBytes(i);
    }
    return offset;
}

std::span<const std::byte> Texture::mip(int level) const noexcept {
    if (level < 0 || level >= mipLevels) {
        return {};
    }
    const std::size_t offset = mipOffset(level);
    const std::size_t length = mipBytes(level);
    if (offset + length > data.size()) {
        return {};
    }
    return std::span{data}.subspan(offset, length);
}

std::expected<Texture, MapError> load(std::span<const std::byte> bytes) {
    if (bytes.size() < kDataOffset) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "file is " + std::to_string(bytes.size()) + " bytes, shorter than the "
                + std::to_string(kDataOffset) + "-byte DDS magic and header"});
    }

    if (std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        return std::unexpected(MapError{MapError::Code::NotSmf,
                                        "not a DDS file: expected magic \"DDS \""});
    }

    if (const std::uint32_t headerSize = readU32(bytes, kOffHeaderSize); headerSize != kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "DDS header claims to be " + std::to_string(headerSize) + " bytes, expected "
                + std::to_string(kHeaderSize)});
    }

    const std::uint32_t pixelFlags = readU32(bytes, kOffPixelFlags);

    Format format{};
    if ((pixelFlags & kPixelFlagFourCc) == 0) {
        // Uncompressed. Supreme Commander's embedded splat masks are stored this
        // way, and only in one layout: 32 bits per texel with the red channel at
        // 0x00FF0000, which is B G R A in memory order and a native Metal
        // format. Any other uncompressed layout is refused rather than
        // reinterpreted, because a channel order guessed wrong is a recoloured
        // image rather than an error anyone would notice.
        const std::uint32_t bits = readU32(bytes, kOffRgbBitCount);
        const std::uint32_t redMask = readU32(bytes, kOffRedMask);
        const std::uint32_t greenMask = readU32(bytes, kOffGreenMask);
        const std::uint32_t blueMask = readU32(bytes, kOffBlueMask);

        if (bits != 32 || redMask != 0x00FF0000u || greenMask != 0x0000FF00u
            || blueMask != 0x000000FFu) {
            return std::unexpected(MapError{
                MapError::Code::BadHeader,
                "uncompressed DDS is " + std::to_string(bits)
                    + "-bit with an unrecognised channel layout; only 32-bit BGRA is handled"});
        }
        format = Format::Bgra8;
    } else if (std::memcmp(bytes.data() + kOffFourCc, "DXT1", 4) == 0) {
        format = Format::Bc1;
    } else if (std::memcmp(bytes.data() + kOffFourCc, "DXT3", 4) == 0) {
        format = Format::Bc2;
    } else if (std::memcmp(bytes.data() + kOffFourCc, "DXT5", 4) == 0) {
        format = Format::Bc3;
    } else {
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "unsupported DDS FOURCC \"" + fourCcText(bytes, kOffFourCc)
                + "\"; only DXT1, DXT3, DXT5 and uncompressed BGRA8 are handled"
                  " (DX10-extended headers included)"});
    }

    const auto width = static_cast<int>(readU32(bytes, kOffWidth));
    const auto height = static_cast<int>(readU32(bytes, kOffHeight));
    if (width <= 0 || height <= 0) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "DDS is " + std::to_string(width) + "x" + std::to_string(height)});
    }

    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.format = format;

    // dwMipMapCount is only meaningful when its flag is set; a file with no mips
    // still has its base level.
    const std::uint32_t flags = readU32(bytes, kOffFlags);
    int declaredMips = 1;
    if ((flags & kFlagMipMapCount) != 0) {
        declaredMips = std::max(1, static_cast<int>(readU32(bytes, kOffMipCount)));
    }

    // Trust the payload over the header: take as many levels as the file
    // actually contains, up to what it declares. A truncated mip chain is common
    // enough in tooling output that refusing the whole texture would be worse
    // than sampling fewer levels.
    const std::size_t available = bytes.size() - kDataOffset;
    std::size_t needed = 0;
    int usableMips = 0;

    for (int level = 0; level < declaredMips; ++level) {
        texture.mipLevels = level + 1;  // so mipBytes() can size this level
        const std::size_t levelBytes = texture.mipBytes(level);
        if (needed + levelBytes > available) {
            break;
        }
        needed += levelBytes;
        usableMips = level + 1;
    }

    texture.mipLevels = usableMips;
    if (usableMips == 0) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "DDS payload is " + std::to_string(available) + " bytes, too small for even level 0 of a "
                + std::to_string(width) + "x" + std::to_string(height) + " "
                + formatName(format) + " image"});
    }

    texture.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kDataOffset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(kDataOffset + needed));

    return texture;
}

std::expected<Texture, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(MapError{MapError::Code::Truncated,
                                        "could not open \"" + path.string() + "\""});
    }

    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    return load(std::as_bytes(std::span{data}));
}

} // namespace rm::dds
