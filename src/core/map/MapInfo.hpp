#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace rm::mapinfo {

// The vertical range a map's terrain spans, in elmos.
struct VerticalRange {
    float minHeight;
    float maxHeight;
};

// Extracts smf.minheight / smf.maxheight from a map's mapinfo.lua.
//
// WHY THIS EXISTS, because it looks like scope creep and is not:
// the .smf binary header carries minHeight/maxHeight, but mapinfo.lua
// *overrides them entirely* when present (rts/Map/MapInfo.cpp:405-418,
// consumed at SMFReadMap.cpp:133-158). The header is therefore NOT the
// authority on vertical scale, and real maps rely on that.
//
// This is not academic. BAR's "Angel Crossing 1.4" (maps/aw04.smf) ships a
// header saying minHeight=850, maxHeight=-150 — inverted — and corrects it in
// mapinfo.lua to -150/850. Honouring only the header renders that map upside
// down: every hill becomes a pit, silently and plausibly.
//
// SCOPE: this is a targeted two-key extractor, NOT a Lua parser. mapinfo.lua is
// a real Lua program that can compute these values, and a map that does so will
// not be understood here — findVerticalRange returns nullopt and the caller
// falls back to the header. A proper mapinfo reader belongs with the rest of
// the map metadata in milestone 3.
[[nodiscard]] std::optional<VerticalRange> findVerticalRange(std::string_view lua);

/// Same, reading from a file. Returns nullopt if the file is absent or has no
/// usable override — both are ordinary, not errors.
[[nodiscard]] std::optional<VerticalRange> findVerticalRangeInFile(
    const std::filesystem::path& path);

/// Locates the mapinfo.lua that belongs to an .smf, if one is on disk.
/// Checks beside the map file and one directory up, which covers both a flat
/// extraction and the `maps/foo.smf` + `mapinfo.lua` layout used inside .sd7
/// archives.
[[nodiscard]] std::optional<std::filesystem::path> findBesideMap(
    const std::filesystem::path& smfPath);

} // namespace rm::mapinfo
