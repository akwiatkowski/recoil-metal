#include "core/map/Smf.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// --- Little-endian scalar reads --------------------------------------------
//
// SMF is little-endian on disk; Recoil reads it field-by-field through
// swab*() helpers that are no-ops on little-endian hosts (SMFMapFile.cpp:295-312).
// We do the same rather than memcpy'ing into a struct, for three reasons:
// the on-disk layout then needs no packing pragma or static_assert to stay
// honest, alignment is irrelevant because we only ever touch bytes, and the
// reads are explicit about width and signedness (which -Wconversion enforces).

[[nodiscard]] std::uint32_t readU32(std::span<const std::byte> b, std::size_t off) noexcept {
    return std::to_integer<std::uint32_t>(b[off])
         | (std::to_integer<std::uint32_t>(b[off + 1]) << 8)
         | (std::to_integer<std::uint32_t>(b[off + 2]) << 16)
         | (std::to_integer<std::uint32_t>(b[off + 3]) << 24);
}

[[nodiscard]] std::int32_t readI32(std::span<const std::byte> b, std::size_t off) noexcept {
    return std::bit_cast<std::int32_t>(readU32(b, off));
}

[[nodiscard]] float readF32(std::span<const std::byte> b, std::size_t off) noexcept {
    return std::bit_cast<float>(readU32(b, off));
}

[[nodiscard]] std::uint16_t readU16(std::span<const std::byte> b, std::size_t off) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(b[off])
                                    | (std::to_integer<std::uint32_t>(b[off + 1]) << 8));
}

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

// Values the engine demands. Anything else is rejected outright, exactly as
// CheckHeader does (SMFMapFile.cpp:16-28).
constexpr std::int32_t kRequiredVersion        = 1;
constexpr std::int32_t kRequiredSquareSize     = 8;
constexpr std::int32_t kRequiredTexelPerSquare = 8;
constexpr std::int32_t kRequiredTileSize       = 32;

// The engine's big-square granularity: mapx/mapy must be multiples of this.
// CSMFReadMap divides by it to get numBigTexX/Y (SMFReadMap.cpp:113-130).
constexpr std::int32_t kBigSquareSize = 128;

} // namespace

namespace rm::smf {

std::expected<HeightField, MapError> load(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "file is " + std::to_string(bytes.size()) + " bytes, shorter than the "
                + std::to_string(kHeaderSize) + "-byte SMF header"});
    }

    // Recoil compares the magic as a C string, so the field is the 15 characters
    // plus their terminating NUL — a full 16-byte match (SMFMapFile.cpp:26).
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return std::unexpected(MapError{
            MapError::Code::NotSmf,
            "not a Recoil SMF map: expected magic \"spring map file\""});
    }

    const std::int32_t version        = readI32(bytes, kOffVersion);
    const std::int32_t mapx           = readI32(bytes, kOffMapX);
    const std::int32_t mapy           = readI32(bytes, kOffMapY);
    const std::int32_t squareSize     = readI32(bytes, kOffSquareSize);
    const std::int32_t texelPerSquare = readI32(bytes, kOffTexelPerSquare);
    const std::int32_t tilesize       = readI32(bytes, kOffTileSize);

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

    HeightField field;
    field.squaresX = mapx;
    field.squaresZ = mapy;

    // The header's range is reported as-is. It is NOT necessarily the truth:
    // mapinfo.lua overrides it entirely when present, and real maps ship
    // inverted headers relying on exactly that (core/map/MapInfo.hpp). Applying
    // the override is the caller's job because it needs a second file.
    field.setVerticalRange(readF32(bytes, kOffMinHeight), readF32(bytes, kOffMaxHeight));

    // Every section is addressed by an absolute file offset, so the heightmap
    // need not follow the header — Recoil's own generator puts a vegetation
    // extra header and grass map in between (BlankMapGenerator.cpp:180-221).
    const auto heightmapPtr = static_cast<std::size_t>(readI32(bytes, kOffHeightmapPtr));
    const std::size_t samples = field.sampleCount();
    const std::size_t heightmapBytes = samples * sizeof(std::uint16_t);

    if (heightmapPtr > bytes.size() || heightmapBytes > bytes.size() - heightmapPtr) {
        return std::unexpected(MapError{
            MapError::Code::Truncated,
            "heightmap needs " + std::to_string(heightmapBytes) + " bytes at offset "
                + std::to_string(heightmapPtr) + " but the file is only "
                + std::to_string(bytes.size()) + " bytes"});
    }

    field.raw.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        field.raw[i] = readU16(bytes, heightmapPtr + i * sizeof(std::uint16_t));
    }

    return field;
}

std::expected<HeightField, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(MapError{
            MapError::Code::Truncated, "could not open \"" + path.string() + "\""});
    }

    // istreambuf_iterator over the whole file: maps are a few MB, and this keeps
    // load() a pure function of bytes, which is what makes it unit-testable
    // without any file on disk.
    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};

    return load(std::as_bytes(std::span{data}));
}

} // namespace rm::smf
