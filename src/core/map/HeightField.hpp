#pragma once

#include <cstdint>
#include <vector>

namespace rm {

// Recoil's world unit is the "elmo". One heightmap square spans kSquareSize
// elmos on each horizontal axis. The engine hard-codes this to 8 and the SMF
// loader *rejects* any other value (SMFFormat.h:56, SMFMapFile.cpp:16-28), so
// it is a constant here rather than a per-map field.
inline constexpr int kSquareSize = 8;

// Heights are quantised across the full uint16 domain, so the divisor is 2^16
// and NOT 65535 — a raw word of 0xFFFF therefore never quite reaches the
// declared maximum (SMFMapFile.cpp:132 with mod from SMFReadMap.cpp:133-158).
inline constexpr float kHeightQuantisationSteps = 65536.0f;

// A decoded terrain heightmap, deliberately format-agnostic.
//
// This is the seam that keeps a future FAF/.scmap path cheap. Both formats
// store the *same shape* — a corner-sampled uint16 grid plus an affine decode:
//
//   SMF     h = minHeight + raw * (maxHeight - minHeight) / 65536
//             (rts/Map/SMF/SMFReadMap.cpp:133-158, SMFMapFile.cpp:132)
//   .scmap  h = raw * heightScale * K,  heightScale = 1/128, K = 8 elmos/ogrid
//             (verified at offset 0x400AE of FAF's perftest.scmap)
//
// Both are `h = baseHeight + raw * heightScale`, so storing the raw grid plus
// those two floats loses nothing and privileges neither format. Adding FAF
// later is one more free function filling this struct — the grid layout is
// byte-identical, so it is a memcpy plus two floats. No format interface, no
// virtual dispatch: this is simply the natural output of the SMF loader, and
// the FAF-readiness is a consequence rather than a cost.
//
// "Corner-sampled" is why the grid is (squaresX+1) x (squaresZ+1): heights
// live at square *corners*, so an N-square axis has N+1 samples.
struct HeightField {
    int squaresX = 0;   ///< SMFHeader::mapx — squares along +X
    int squaresZ = 0;   ///< SMFHeader::mapy — squares along +Z
    float baseHeight = 0.0f;    ///< elmos at raw == 0
    float heightScale = 0.0f;   ///< elmos per raw unit

    /// Row-major, verticesZ() rows of verticesX() samples.
    std::vector<std::uint16_t> raw;

    [[nodiscard]] int verticesX() const noexcept { return squaresX + 1; }
    [[nodiscard]] int verticesZ() const noexcept { return squaresZ + 1; }

    /// Total samples the raw grid must hold for this geometry.
    [[nodiscard]] std::size_t sampleCount() const noexcept;

    /// Re-derives baseHeight/heightScale from a declared vertical range.
    ///
    /// Needed because the .smf binary header is not the authority on vertical
    /// scale — mapinfo.lua overrides it entirely when present, and real maps
    /// depend on that (see core/map/MapInfo.hpp). Note min > max is legal: it
    /// simply means the raw domain runs downhill.
    void setVerticalRange(float minHeight, float maxHeight) noexcept;

    /// Decoded height in elmos at grid corner (x, z). Out-of-range indices are
    /// clamped, which is what the mesh builder's central-difference normals
    /// want at the map edges.
    [[nodiscard]] float heightAt(int x, int z) const noexcept;

    /// World extent in elmos. Note this spans *squares*, not vertices: a
    /// 128-square map is 1024 elmos wide and has 129 height samples.
    [[nodiscard]] float widthElmos() const noexcept;
    [[nodiscard]] float depthElmos() const noexcept;
};

} // namespace rm
