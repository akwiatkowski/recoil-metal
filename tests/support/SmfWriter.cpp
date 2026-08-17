#include "support/SmfWriter.hpp"

#include <bit>

namespace {

// --- Little-endian appenders ------------------------------------------------

void appendU32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
}

void appendI32(std::vector<std::byte>& out, std::int32_t v) {
    appendU32(out, std::bit_cast<std::uint32_t>(v));
}

void appendF32(std::vector<std::byte>& out, float v) {
    appendU32(out, std::bit_cast<std::uint32_t>(v));
}

void appendU16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
}

void appendZeros(std::vector<std::byte>& out, std::size_t count) {
    out.insert(out.end(), count, std::byte{0});
}

/// Clamp a possibly-hostile header dimension into a safe count. Negative-size
/// tests must not turn into gigabyte allocations.
[[nodiscard]] std::size_t safeCount(std::int32_t v) {
    return v <= 0 ? 0u : static_cast<std::size_t>(v);
}

// Layout constants, all from BlankMapGenerator.cpp:134-232.
constexpr std::size_t kHeaderBytes = 80;       ///< SMFHeader on disk
constexpr std::size_t kExtraHeaderBytes = 8;   ///< ExtraHeader{size,type}
constexpr std::int32_t kMehVegetation = 1;     ///< SMFFormat.h:103
constexpr char kSmtFileName[] = "generated.smt";

// Recoil's generator writes vegHeader.size = sizeof(int) = 4, but its own
// reader skips an unrecognised extra header by `size - 8` (SMFMapFile.cpp:265),
// which implies size counts the 8-byte {size,type} prefix too — i.e. 12 here.
// The engine gets away with 4 only because ReadGrassMap special-cases
// MEH_Vegetation and returns before it ever performs that skip. We emit the
// self-consistent value so this fixture stays valid for any conforming reader.
constexpr std::int32_t kVegExtraHeaderSize = 12;

} // namespace

namespace rmtest {

std::vector<std::byte> writeSmf(const SmfSpec& spec) {
    const std::size_t mapx = safeCount(spec.mapx);
    const std::size_t mapy = safeCount(spec.mapy);

    const std::size_t heightmapCount = (mapx + 1) * (mapy + 1);
    const std::size_t typemapCount   = (mapx / 2) * (mapy / 2);
    const std::size_t metalmapCount  = (mapx / 2) * (mapy / 2);
    const std::size_t tilemapCount   = (mapx * mapy) / 16;
    const std::size_t vegmapCount    = (mapx / 4) * (mapy / 4);

    const std::size_t heightmapSize = heightmapCount * sizeof(std::uint16_t);
    const std::size_t tilemapSize   = tilemapCount * sizeof(std::int32_t);

    // MapTileHeader{numTileFiles,numTiles} + numTilesInThisFile + name + indices
    const std::size_t tilemapTotalSize =
        8 + 4 + sizeof(kSmtFileName) + tilemapSize;

    // The vegetation grass map sits between the header block and the heightmap,
    // which is precisely why the parser must honour heightmapPtr instead of
    // assuming the heightmap follows the header.
    const std::size_t vegmapOffset = kHeaderBytes + kExtraHeaderBytes + sizeof(std::int32_t);

    const std::size_t heightmapPtr = vegmapOffset + vegmapCount;
    const std::size_t typeMapPtr   = heightmapPtr + heightmapSize;
    const std::size_t tilesPtr     = typeMapPtr + typemapCount;
    const std::size_t metalmapPtr  = tilesPtr + tilemapTotalSize;
    const std::size_t featurePtr   = metalmapPtr + metalmapCount;

    std::vector<std::byte> out;
    out.reserve(featurePtr + 8);

    // --- SMFHeader ---------------------------------------------------------
    if (spec.validMagic) {
        // 15 characters plus the terminating NUL: exactly the char[16] field.
        const char* magic = "spring map file";
        for (std::size_t i = 0; i < 16; ++i) {
            out.push_back(static_cast<std::byte>(magic[i]));
        }
    } else {
        appendZeros(out, 16);
    }

    appendI32(out, spec.version);
    appendI32(out, 0);  // mapid: "just set to a random value" (SMFFormat.h:52)
    appendI32(out, spec.mapx);
    appendI32(out, spec.mapy);
    appendI32(out, spec.squareSize);
    appendI32(out, spec.texelPerSquare);
    appendI32(out, spec.tilesize);
    appendF32(out, spec.minHeight);
    appendF32(out, spec.maxHeight);
    appendI32(out, static_cast<std::int32_t>(heightmapPtr));
    appendI32(out, static_cast<std::int32_t>(typeMapPtr));
    appendI32(out, static_cast<std::int32_t>(tilesPtr));
    appendI32(out, 0);  // minimapPtr: the generator leaves this 0 too (:191)
    appendI32(out, static_cast<std::int32_t>(metalmapPtr));
    appendI32(out, static_cast<std::int32_t>(featurePtr));
    appendI32(out, 1);  // numExtraHeaders

    // --- Extra header: vegetation ------------------------------------------
    appendI32(out, kVegExtraHeaderSize);
    appendI32(out, kMehVegetation);
    appendI32(out, static_cast<std::int32_t>(vegmapOffset));
    appendZeros(out, vegmapCount);

    // --- Heightmap ---------------------------------------------------------
    for (std::size_t i = 0; i < heightmapCount; ++i) {
        appendU16(out, i < spec.heights.size() ? spec.heights[i] : std::uint16_t{0});
    }

    // --- Type map ----------------------------------------------------------
    appendZeros(out, typemapCount);

    // --- Tiles -------------------------------------------------------------
    appendI32(out, 1);                               // numTileFiles
    appendI32(out, 1);                               // numTiles
    appendI32(out, 1);                               // tiles in this file
    for (const char c : kSmtFileName) {              // includes the NUL
        out.push_back(static_cast<std::byte>(c));
    }
    appendZeros(out, tilemapSize);

    // --- Metal map ---------------------------------------------------------
    appendZeros(out, metalmapCount);

    // --- Feature header ----------------------------------------------------
    appendI32(out, 0);  // numFeatureType
    appendI32(out, 0);  // numFeatures

    return out;
}

} // namespace rmtest
