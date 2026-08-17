#pragma once

#include "core/Error.hpp"
#include "core/map/HeightField.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>

namespace rm::smf {

// The 16-byte magic every .smf opens with. Recoil compares it as a C string
// (SMFMapFile.cpp:26), so the 16th byte is the terminating NUL.
inline constexpr char kMagic[] = "spring map file";

// Byte size of SMFHeader as it appears on disk: char[16] + 15 x 4-byte fields.
// Spelled out rather than sizeof()'d because we never declare a matching
// struct — see load() for why.
inline constexpr std::size_t kHeaderSize = 16 + 15 * 4;

// Parses the header and heightmap of a Recoil SMF map.
//
// Only the geometry is decoded. Type map, metal map, tile indices, minimap and
// features are all located by their own header pointers and are milestone 3+
// concerns; ignoring them now costs nothing because every section is addressed
// absolutely, not sequentially.
//
// Returns std::expected because a malformed map is recoverable input error,
// not a startup fault — see MapError in core/Error.hpp.
[[nodiscard]] std::expected<HeightField, MapError> load(std::span<const std::byte> bytes);

// Reads the whole file into memory and parses it. Maps are a few MB (a 20x20 km
// map's heightmap is 2.1 MB) so slurping is fine and keeps load() a pure
// function over bytes, which is what makes it testable without touching disk.
[[nodiscard]] std::expected<HeightField, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::smf
