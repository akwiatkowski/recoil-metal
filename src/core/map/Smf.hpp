#pragma once

#include "core/Error.hpp"
#include "core/map/HeightField.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rm::smf {

// The 16-byte magic every .smf opens with. Recoil compares it as a C string
// (SMFMapFile.cpp:26), so the 16th byte is the terminating NUL.
inline constexpr char kMagic[] = "spring map file";

// Byte size of SMFHeader as it appears on disk: char[16] + 15 x 4-byte fields.
// Spelled out rather than sizeof()'d because we never declare a matching
// struct — see load() for why.
inline constexpr std::size_t kHeaderSize = 16 + 15 * 4;

// One texture tile spans 4 heightmap squares on each axis: texelPerSquare = 8
// and tileSize = 32, so a tile is 32 texels = 32 elmos = 4 squares. The tile
// index array is therefore (mapx/4) x (mapy/4) (SMFFormat.h:119-120).
inline constexpr int kSquaresPerTile = 4;

// Which texture tile covers each 4x4-square block of the map.
//
// This is the whole of SMF's texture compression: any tile index may repeat
// arbitrarily, so identical 32x32 blocks are stored once in the .smt
// (SMFFormat.h:114-117).
struct TileIndex {
    int tilesX = 0;  ///< mapx / 4
    int tilesZ = 0;  ///< mapy / 4

    /// Tile file names in declaration order, as embedded in the .smf. May be
    /// overridden by mapinfo.lua's smf.smtFileName0..N.
    std::vector<std::string> smtFileNames;

    /// Total tiles across all files, per MapTileHeader.
    std::int32_t totalTiles = 0;

    /// Row-major, tilesZ rows of tilesX indices into the concatenated tile set.
    std::vector<std::int32_t> indices;

    [[nodiscard]] std::size_t indexCount() const noexcept;
};

// Parses the header and heightmap of a Recoil SMF map.
//
// Returns std::expected because a malformed map is a recoverable input error,
// not a startup fault — see MapError in core/Error.hpp.
[[nodiscard]] std::expected<HeightField, MapError> load(std::span<const std::byte> bytes);

// Parses the tile section: MapTileHeader, the per-file name records, and the
// index array (SMFGroundTextures.cpp:135-172). Independent of load() so the
// geometry path stays usable on maps whose .smt is missing — which the engine
// also tolerates, rendering those tiles flat (SMFGroundTextures.cpp:147-157).
[[nodiscard]] std::expected<TileIndex, MapError> loadTileIndex(std::span<const std::byte> bytes);

// Reads the whole file into memory and parses it. Maps are a few MB (a 20x20 km
// map's heightmap is 2.1 MB) so slurping is fine and keeps the parsers pure
// functions over bytes, which is what makes them testable without disk.
[[nodiscard]] std::expected<HeightField, MapError> loadFile(const std::filesystem::path& path);
[[nodiscard]] std::expected<TileIndex, MapError> loadTileIndexFile(
    const std::filesystem::path& path);

} // namespace rm::smf
