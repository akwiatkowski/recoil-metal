#pragma once

#include "core/Error.hpp"
#include "core/map/HeightField.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
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

// Largest map this loads. The three 4096-square stock maps (Setons-scale and
// up) would need 16.8M vertices at 24 B each plus uint32 indices — about 806 MB
// on the GPU and the same again CPU-side while the mesh is built — and there is
// no LOD or streaming yet. 57 of the 60 stock maps are 2048 or under and fit in
// ≤201 MB. Refusing loudly beats an allocation failure three layers down.
inline constexpr int kMaxSquares = 2048;

// A decoded Supreme Commander map.
//
// The heightmap lands in the same format-agnostic HeightField the SMF loader
// fills — that seam is why this milestone was cheap (HeightField.hpp:19-37).
// Everything else here is what SMF has no counterpart for.
struct Map {
    HeightField field;

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
