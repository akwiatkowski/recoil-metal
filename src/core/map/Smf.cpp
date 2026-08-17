#include "core/map/Smf.hpp"

#include "core/map/ByteReader.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

// Field byte offsets within SMFHeader, in declaration order (SMFFormat.h:49-70).
// Named constants beat arithmetic-in-place: a wrong offset here is the single
// easiest way to produce a parser that "works" on one map and garbage on another.
constexpr std::size_t kOffVersion        = 16;
constexpr std::size_t kOffMapX           = 24;
constexpr std::size_t kOffMapY           = 28;
constexpr std::size_t kOffSquareSize     = 32;
constexpr std::size_t kOffTexelPerSquare = 36;
constexpr std::size_t kOffTileSize       = 40;
constexpr std::size_t kOffMinHeight      = 44;
constexpr std::size_t kOffMaxHeight      = 48;
constexpr std::size_t kOffHeightmapPtr   = 52;
constexpr std::size_t kOffTilesPtr       = 60;

// Values the engine demands. Anything else is rejected outright, exactly as
// CheckHeader does (SMFMapFile.cpp:16-28).
constexpr std::int32_t kRequiredVersion        = 1;
constexpr std::int32_t kRequiredSquareSize     = 8;
constexpr std::int32_t kRequiredTexelPerSquare = 8;
constexpr std::int32_t kRequiredTileSize       = 32;

// The engine's big-square granularity: mapx/mapy must be multiples of this.
// CSMFReadMap divides by it to get numBigTexX/Y (SMFReadMap.cpp:113-130).
constexpr std::int32_t kBigSquareSize = 128;

// A tile-file name is a NUL-terminated string of unbounded length on paper. Cap
// it so a corrupt file cannot make us walk the whole map looking for a NUL.
constexpr std::size_t kMaxTileFileNameLength = 260;

/// The fields of SMFHeader both loaders need, already validated.
struct Header {
    std::int32_t mapx = 0;
    std::int32_t mapy = 0;
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    std::size_t heightmapPtr = 0;
    std::size_t tilesPtr = 0;
};

[[nodiscard]] std::expected<Header, rm::MapError> parseHeader(std::span<const std::byte> bytes) {
    using rm::MapError;

    if (bytes.size() < rm::smf::kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "file is " + std::to_string(bytes.size()) + " bytes, shorter than the "
                + std::to_string(rm::smf::kHeaderSize) + "-byte SMF header"});
    }

    // Recoil compares the magic as a C string, so the field is the 15 characters
    // plus their terminating NUL — a full 16-byte match (SMFMapFile.cpp:26).
    if (std::memcmp(bytes.data(), rm::smf::kMagic, sizeof(rm::smf::kMagic)) != 0) {
        return std::unexpected(MapError{
            MapError::Code::NotSmf,
            "not a Recoil SMF map: expected magic \"spring map file\""});
    }

    const std::int32_t version        = rm::readI32(bytes, kOffVersion);
    const std::int32_t mapx           = rm::readI32(bytes, kOffMapX);
    const std::int32_t mapy           = rm::readI32(bytes, kOffMapY);
    const std::int32_t squareSize     = rm::readI32(bytes, kOffSquareSize);
    const std::int32_t texelPerSquare = rm::readI32(bytes, kOffTexelPerSquare);
    const std::int32_t tilesize       = rm::readI32(bytes, kOffTileSize);

    if (version != kRequiredVersion || tilesize != kRequiredTileSize
        || texelPerSquare != kRequiredTexelPerSquare || squareSize != kRequiredSquareSize) {
        // Mirrors the engine's own diagnostic so a rejected map can be compared
        // against a Recoil log line directly.
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "corrupt SMF header (v=%d ts=%d tps=%d ss=%d), expected (v=1 ts=32 tps=8 ss=8)",
                      version, tilesize, texelPerSquare, squareSize);
        return std::unexpected(MapError{MapError::Code::BadHeader, buf});
    }

    // The engine never validates this and fails *silently* downstream: ROAM
    // computes numPatchesX = mapx / PATCH_SIZE independently and the equality
    // assert against numBigTexX is commented out (RoamMeshDrawer.cpp:58-60), so
    // a non-conforming map simply loses its last strip of terrain. We reject.
    if (mapx <= 0 || mapy <= 0 || mapx % kBigSquareSize != 0 || mapy % kBigSquareSize != 0) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "map is " + std::to_string(mapx) + "x" + std::to_string(mapy)
                + " squares; both axes must be positive multiples of "
                + std::to_string(kBigSquareSize)});
    }

    return Header{
        .mapx = mapx,
        .mapy = mapy,
        .minHeight = rm::readF32(bytes, kOffMinHeight),
        .maxHeight = rm::readF32(bytes, kOffMaxHeight),
        .heightmapPtr = static_cast<std::size_t>(rm::readI32(bytes, kOffHeightmapPtr)),
        .tilesPtr = static_cast<std::size_t>(rm::readI32(bytes, kOffTilesPtr)),
    };
}

/// Bounds check for a section: does `length` bytes fit at `offset`?
[[nodiscard]] bool sectionFits(std::span<const std::byte> bytes, std::size_t offset,
                               std::size_t length) noexcept {
    return offset <= bytes.size() && length <= bytes.size() - offset;
}

[[nodiscard]] std::vector<char> slurp(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    return std::vector<char>{std::istreambuf_iterator<char>{in},
                             std::istreambuf_iterator<char>{}};
}

} // namespace

namespace rm::smf {

std::size_t TileIndex::indexCount() const noexcept {
    if (tilesX <= 0 || tilesZ <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesZ);
}

std::expected<HeightField, MapError> load(std::span<const std::byte> bytes) {
    const auto header = parseHeader(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }

    HeightField field;
    field.squaresX = header->mapx;
    field.squaresZ = header->mapy;

    // The header's range is reported as-is. It is NOT necessarily the truth:
    // mapinfo.lua overrides it entirely when present, and real maps ship
    // inverted headers relying on exactly that (core/map/MapInfo.hpp). Applying
    // the override is the caller's job because it needs a second file.
    field.setVerticalRange(header->minHeight, header->maxHeight);

    // Every section is addressed by an absolute file offset, so the heightmap
    // need not follow the header — Recoil's own generator puts a vegetation
    // extra header and grass map in between (BlankMapGenerator.cpp:180-221).
    const std::size_t samples = field.sampleCount();
    const std::size_t heightmapBytes = samples * sizeof(std::uint16_t);

    if (!sectionFits(bytes, header->heightmapPtr, heightmapBytes)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "heightmap needs " + std::to_string(heightmapBytes) + " bytes at offset "
                + std::to_string(header->heightmapPtr) + " but the file is only "
                + std::to_string(bytes.size()) + " bytes"});
    }

    field.raw.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        field.raw[i] = readU16(bytes, header->heightmapPtr + i * sizeof(std::uint16_t));
    }

    return field;
}

std::expected<TileIndex, MapError> loadTileIndex(std::span<const std::byte> bytes) {
    const auto header = parseHeader(bytes);
    if (!header) {
        return std::unexpected(header.error());
    }

    // MapTileHeader { int numTileFiles; int numTiles; } (SMFFormat.h:123-127).
    constexpr std::size_t kMapTileHeaderSize = 8;
    if (!sectionFits(bytes, header->tilesPtr, kMapTileHeaderSize)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "tile section header at offset " + std::to_string(header->tilesPtr)
                + " runs past the end of a " + std::to_string(bytes.size()) + "-byte file"});
    }

    const std::int32_t numTileFiles = readI32(bytes, header->tilesPtr);
    const std::int32_t numTiles = readI32(bytes, header->tilesPtr + 4);

    if (numTileFiles < 0 || numTiles < 0) {
        return std::unexpected(MapError{
            MapError::Code::BadGeometry,
            "tile section declares " + std::to_string(numTileFiles) + " files and "
                + std::to_string(numTiles) + " tiles"});
    }

    TileIndex index;
    index.tilesX = header->mapx / kSquaresPerTile;
    index.tilesZ = header->mapy / kSquaresPerTile;
    index.totalTiles = numTiles;

    // Then numTileFiles records of { int32 tilesInThisFile; char name[] '\0' }
    // (SMFGroundTextures.cpp:135-137).
    std::size_t cursor = header->tilesPtr + kMapTileHeaderSize;
    index.smtFileNames.reserve(static_cast<std::size_t>(numTileFiles));

    for (std::int32_t file = 0; file < numTileFiles; ++file) {
        if (!sectionFits(bytes, cursor, 4)) {
            return std::unexpected(MapError{
                MapError::Code::Truncated,
                "tile file record " + std::to_string(file) + " runs past end of file"});
        }
        cursor += 4;  // tilesInThisFile — the running total is implied by order

        std::string name;
        while (true) {
            if (cursor >= bytes.size()) {
                return std::unexpected(MapError{
                    MapError::Code::Truncated,
                    "unterminated tile file name in record " + std::to_string(file)});
            }
            const auto c = static_cast<char>(bytes[cursor]);
            ++cursor;
            if (c == '\0') {
                break;
            }
            if (name.size() >= kMaxTileFileNameLength) {
                return std::unexpected(MapError{
                    MapError::Code::BadHeader,
                    "tile file name in record " + std::to_string(file) + " exceeds "
                        + std::to_string(kMaxTileFileNameLength) + " characters"});
            }
            name.push_back(c);
        }
        index.smtFileNames.push_back(std::move(name));
    }

    // Then int32[mapx/4 * mapy/4] global tile indices (SMFGroundTextures.cpp:172).
    const std::size_t count = index.indexCount();
    const std::size_t indexBytes = count * sizeof(std::int32_t);
    if (!sectionFits(bytes, cursor, indexBytes)) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "tile index array needs " + std::to_string(indexBytes) + " bytes at offset "
                + std::to_string(cursor) + " but the file is only "
                + std::to_string(bytes.size()) + " bytes"});
    }

    index.indices.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        index.indices[i] = readI32(bytes, cursor + i * sizeof(std::int32_t));
    }

    return index;
}

std::expected<HeightField, MapError> loadFile(const std::filesystem::path& path) {
    const std::vector<char> data = slurp(path);
    if (data.empty()) {
        return std::unexpected(MapError{
            MapError::Code::Truncated, "could not open \"" + path.string() + "\""});
    }
    return load(std::as_bytes(std::span{data}));
}

std::expected<TileIndex, MapError> loadTileIndexFile(const std::filesystem::path& path) {
    const std::vector<char> data = slurp(path);
    if (data.empty()) {
        return std::unexpected(MapError{
            MapError::Code::Truncated, "could not open \"" + path.string() + "\""});
    }
    return loadTileIndex(std::as_bytes(std::span{data}));
}

} // namespace rm::smf
