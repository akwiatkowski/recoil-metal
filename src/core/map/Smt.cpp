#include "core/map/Smt.hpp"

#include "core/map/ByteReader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Field offsets within TileFileHeader (SMFFormat.h:175-183).
constexpr std::size_t kOffVersion = 16;
constexpr std::size_t kOffNumTiles = 20;
constexpr std::size_t kOffTileSize = 24;
constexpr std::size_t kOffCompression = 28;

constexpr std::int32_t kRequiredVersion = 1;
constexpr std::int32_t kRequiredTileSize = 32;
constexpr std::int32_t kCompressionDxt1 = 1;

} // namespace

namespace rm::smt {

std::span<const std::byte> TileSet::mip(std::int32_t tile, int level) const noexcept {
    if (tile < 0 || tile >= tileCount || level < 0 || level >= kMipLevels) {
        return {};
    }
    const std::size_t base = static_cast<std::size_t>(tile) * kTileBytes + kMipOffsets[level];
    if (base + kMipBytes[level] > tiles.size()) {
        return {};
    }
    return std::span{tiles}.subspan(base, kMipBytes[level]);
}

std::expected<TileSet, MapError> load(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "file is " + std::to_string(bytes.size()) + " bytes, shorter than the "
                + std::to_string(kHeaderSize) + "-byte SMT header"});
    }

    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return std::unexpected(MapError{
            MapError::Code::NotSmf,
            "not a Recoil SMT tile file: expected magic \"spring tilefile\""});
    }

    const std::int32_t version = readI32(bytes, kOffVersion);
    const std::int32_t tileCount = readI32(bytes, kOffNumTiles);
    const std::int32_t tileSize = readI32(bytes, kOffTileSize);
    const std::int32_t compression = readI32(bytes, kOffCompression);

    if (version != kRequiredVersion || tileSize != kRequiredTileSize
        || compression != kCompressionDxt1) {
        return std::unexpected(MapError{
            MapError::Code::BadHeader,
            "corrupt SMT header (v=" + std::to_string(version)
                + " ts=" + std::to_string(tileSize)
                + " compression=" + std::to_string(compression)
                + "), expected (v=1 ts=32 compression=1/DXT1)"});
    }

    if (tileCount < 0) {
        return std::unexpected(MapError{MapError::Code::BadGeometry,
                                        "negative tile count " + std::to_string(tileCount)});
    }

    const std::size_t payload = static_cast<std::size_t>(tileCount) * kTileBytes;
    if (payload > bytes.size() - kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "header declares " + std::to_string(tileCount) + " tiles ("
                + std::to_string(payload) + " bytes) but only "
                + std::to_string(bytes.size() - kHeaderSize) + " bytes follow the header"});
    }

    TileSet set;
    set.tileCount = tileCount;
    // One bulk copy: the DXT1 payload is opaque to us and goes to the GPU as-is.
    set.tiles.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                     bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + payload));
    return set;
}

std::expected<TileSet, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(MapError{MapError::Code::Truncated,
                                        "could not open \"" + path.string() + "\""});
    }

    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    return load(std::as_bytes(std::span{data}));
}

} // namespace rm::smt
