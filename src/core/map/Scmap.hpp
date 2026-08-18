#pragma once

#include "core/Error.hpp"
#include "core/map/HeightField.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rm::scmap {

// The 4-byte magic every .scmap opens with. NOT NUL-terminated, unlike SMF's —
// hence a char array with an explicit size rather than a string literal.
inline constexpr char kMagic[4] = {'M', 'a', 'p', '\x1a'};

// Retail Forged Alliance ships version 60 in the minor field. The reader accepts
// what it can validate: the section layout below was derived against v60 and
// checked to EOF on all 60 stock maps, so anything else is reported rather than
// parsed on the assumption that the layout held.
inline constexpr std::int32_t kVersionMinor = 60;

// Supreme Commander's world unit is the "ogrid": one heightmap square on each
// horizontal axis. Recoil's is the "elmo" and its square is 8 of them
// (HeightField.hpp:12), so an ogrid IS 8 elmos here and heights must scale by
// the same 8 or the terrain comes out flat by a factor of eight.
inline constexpr float kElmosPerOgrid = 8.0f;

// Largest map this loads.
//
// This used to be 2048, because the three 4096-square stock maps would have
// needed 16.8M vertices — about 806 MB on the GPU and the same again while the
// mesh was built — and refusing loudly beat an allocation failure three layers
// down. The mesh builder now decimates instead (core/mesh/TerrainMesh.hpp), so
// a 4096-square map costs exactly what a 1024-square one does and the limit is
// no longer about the mesh at all.
//
// What it still bounds is the HEIGHTFIELD, which is not decimated: 8192 squares
// is 8193^2 samples at 2 bytes, or 134 MB, plus a passability grid over the
// same ground. Beyond that a map wants streaming rather than a bigger number.
inline constexpr int kMaxSquares = 8192;

// One entry of the stratum table: where the texture lives and how often it
// repeats across the map.
//
// Unused slots hold an empty path and a scale, which is why the table must be
// read in full rather than stopped at the first blank.
struct TextureRef {
    std::string path;      ///< game-relative, e.g. "/env/Evergreen/Layers/Sand01_albedo.dds"
    float scale = 0.0f;

    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

// Slot counts, fixed by the format rather than counted in the file.
//
// Ten albedo entries: [0] is the base layer that covers the whole map, [1..8]
// are the strata blended over it by the two weight masks, and [9] is the
// macrotexture laid over everything. Nine normal entries, one per layer bar the
// macrotexture.
inline constexpr std::size_t kAlbedoSlots = 10;
inline constexpr std::size_t kNormalSlots = 9;
inline constexpr std::size_t kStrataSlots = 8;

// A decoded Supreme Commander map.
//
// The heightmap lands in the same format-agnostic HeightField the SMF loader
// fills — that seam is why this milestone was cheap (HeightField.hpp:19-37).
// Everything else here is what SMF has no counterpart for.
struct Map {
    HeightField field;

    // --- ground shading ----------------------------------------------------
    //
    // SMF bakes its ground into a tile atlas; Supreme Commander bakes nothing
    // and blends nine tiled layers at runtime through two weight masks. So
    // where the SMF path gets a finished image, this path gets a recipe: the
    // layer textures live outside the map file entirely (in env.scd), and only
    // the masks are embedded.
    std::array<TextureRef, kAlbedoSlots> albedo;
    std::array<TextureRef, kNormalSlots> normals;

    // Embedded DDS blobs, kept as raw file bytes — header included — so the DDS
    // reader parses them exactly as it parses one off disk.
    //
    // The normal map is TILED: one tile for every map up to 2048 squares, and
    // four for the three 4096-square maps. That single count is what a naive
    // parser trips over on the big maps.
    std::vector<std::vector<std::byte>> normalMapTiles;  ///< DXT5
    std::vector<std::byte> maskA;  ///< BGRA8, strata 1-4 in r,g,b,a
    std::vector<std::byte> maskB;  ///< BGRA8, strata 5-8 in r,g,b,a

    // Terrain type, one byte per square at full map resolution. Undocumented
    // semantically — the engine uses it for movement and effects — but it is
    // banded in a way that correlates with the map's strata, which is what makes
    // it a usable ground colour before the real splat shader exists.
    int typesX = 0;
    int typesZ = 0;
    std::vector<std::uint8_t> terrainType;

    // Unlike Recoil, which hard-codes water at y = 0 (rts/Map/Ground.h:32),
    // SupCom stores a water level per map — and 17 of the 60 stock maps have no
    // water at all, so honouring this is the difference between a dry map and a
    // dry map under a blue sheet.
    bool hasWater = false;
    float waterElevation = 0.0f;  ///< elmos

    // Whether the sequential parse landed exactly on the last byte.
    //
    // This is the format work's acid test: nothing is seeked to, so a wrong
    // field width anywhere upstream shifts everything after it and the final
    // array cannot end on EOF. It is reported rather than enforced because
    // everything this renderer needs is read before the last two sections — a
    // map whose skybox block we misread is still a map we can draw.
    bool endsExactlyAtEof = false;
};

/// Whether a buffer opens with the .scmap magic. Used to tell the two map
/// families apart from one file name, since neither extension is load-bearing.
[[nodiscard]] bool looksLikeScmap(std::span<const std::byte> bytes) noexcept;

// Parses a retail v60 .scmap.
//
// Strictly sequential: every section is read in order and nothing is seeked to,
// which is what makes endsExactlyAtEof meaningful. Returns std::expected because
// a malformed map is a recoverable input error — see MapError in core/Error.hpp.
[[nodiscard]] std::expected<Map, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<Map, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::scmap
