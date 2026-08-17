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

/// DDSD_MIPMAPCOUNT — whether dwMipMapCount is meaningful.
constexpr std::uint32_t kFlagMipMapCount = 0x00020000u;
/// DDPF_FOURCC — whether the pixel format names a fourcc rather than masks.
constexpr std::uint32_t kPixelFlagFourCc = 0x00000004u;

constexpr int kBlockTexels = 4;

using rm::MapError;

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
    const auto blocks = static_cast<std::size_t>((mipWidth(level) + kBlockTexels - 1)
                                                 / kBlockTexels);
    return blocks * blockBytes(format);
}

std::size_t Texture::mipBytes(int level) const noexcept {
    const auto rows = static_cast<std::size_t>((mipHeight(level) + kBlockTexels - 1)
                                               / kBlockTexels);
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
    if ((pixelFlags & kPixelFlagFourCc) == 0) {
        // Uncompressed DDS. Perfectly legal, just not something this reader
        // handles — and guessing at the channel masks would be worse than saying
        // so, since nothing downstream can consume an uncompressed surface yet.
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "DDS is uncompressed (no FOURCC in its pixel format); only DXT1/3/5 are supported"});
    }

    BlockFormat format{};
    if (std::memcmp(bytes.data() + kOffFourCc, "DXT1", 4) == 0) {
        format = BlockFormat::Bc1;
    } else if (std::memcmp(bytes.data() + kOffFourCc, "DXT3", 4) == 0) {
        format = BlockFormat::Bc2;
    } else if (std::memcmp(bytes.data() + kOffFourCc, "DXT5", 4) == 0) {
        format = BlockFormat::Bc3;
    } else {
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "unsupported DDS FOURCC \"" + fourCcText(bytes, kOffFourCc)
                + "\"; only DXT1, DXT3 and DXT5 are handled"
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
                + (format == BlockFormat::Bc1 ? "DXT1" : "DXT3/5") + " image"});
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
