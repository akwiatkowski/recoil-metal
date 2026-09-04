#pragma once

#include "core/map/HeightField.hpp"

#include <cstdint>
#include <vector>

namespace rm {

// The highest raw height in every power-of-two cell of a heightfield, at every level.
//
// WHY. A flyer's terrain look-ahead (`C-246`) asks "what is the highest ground within my
// reach?" and retail answers it in O(1) from exactly this structure: a pyramid of maxima
// indexed by `(level, x >> level, z >> level)`. Scanning the cell's corners instead costs
// `(2^L + 1)^2` reads per question — five thousand for a five-second reach — twice per
// aircraft per beat. Built once per map, this makes the question a single load.
//
// SEMANTICS, chosen to be indistinguishable from the corner scan it replaces. Level `L`
// cell `i` covers corners `[i·2^L, (i+1)·2^L]` INCLUSIVE — a square is decided by both its
// corners — with every corner index clamped to the field's edge, as `Terrain::cornerHeight`
// clamps. Each axis therefore has `(squares >> L) + 1` cells, the last of which may lie
// wholly past the edge and hold the edge value. Level 0 (one square per cell) is not stored:
// no caller asks below level 1, and it would cost as much as the field itself.
//
// RAW, not decoded. The maximum raw word is the maximum height only when the vertical scale
// is positive; a field whose scale runs downhill (legal, see `setVerticalRange`) inverts
// the order and gets the corner scan instead. The pyramid stays a pure function of the
// raw grid, so it never needs to know the scale.
class MaxHeightPyramid {
public:
    MaxHeightPyramid() = default;

    /// Builds every level from 1 up to the level whose cell spans the whole field.
    explicit MaxHeightPyramid(const HeightField& field);

    /// The number of levels stored above level 0; `maxRaw` accepts `1..levelCount()`.
    [[nodiscard]] int levelCount() const noexcept { return static_cast<int>(levels_.size()); }

    /// The highest raw word in level `level`'s cell `(cellX, cellZ)`; cell indices past the
    /// last cell clamp to it, mirroring the edge clamp of the corners they would cover.
    [[nodiscard]] std::uint16_t maxRaw(int level, int cellX, int cellZ) const noexcept;

private:
    struct Level {
        int cellsX = 0;
        int cellsZ = 0;
        std::vector<std::uint16_t> cells;  ///< row-major, cellsZ rows of cellsX
    };
    std::vector<Level> levels_;  ///< levels_[L - 1] is level L
};

} // namespace rm
