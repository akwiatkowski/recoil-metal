#include "core/map/MaxHeightPyramid.hpp"

#include <algorithm>
#include <bit>

namespace rm {

namespace {

/// The raw word at a corner, clamped to the field's edge like `HeightField::heightAt`.
[[nodiscard]] std::uint16_t rawAt(const HeightField& field, int x, int z) noexcept {
    const int cx = std::clamp(x, 0, field.squaresX);
    const int cz = std::clamp(z, 0, field.squaresZ);
    const auto index = static_cast<std::size_t>(cz) * static_cast<std::size_t>(field.verticesX())
                       + static_cast<std::size_t>(cx);
    return index < field.raw.size() ? field.raw[index] : std::uint16_t{0};
}

}  // namespace

MaxHeightPyramid::MaxHeightPyramid(const HeightField& field) {
    if (field.squaresX <= 0 || field.squaresZ <= 0 || field.raw.size() < field.sampleCount()) {
        return;
    }
    // The top level is the one whose cell covers the field: `bsr(min − 2) + 1` is the
    // ceiling retail's look-ahead clamps to (`C-246`), and one more level would never be
    // asked for.
    const int smaller = std::min(field.squaresX, field.squaresZ);
    const int top = smaller > 2 ? std::bit_width(static_cast<std::uint32_t>(smaller - 2)) : 1;
    levels_.reserve(static_cast<std::size_t>(top));

    // Level 1 straight from the corners: cell `i` is the corners `2i, 2i+1, 2i+2`.
    {
        Level level;
        level.cellsX = (field.squaresX >> 1) + 1;
        level.cellsZ = (field.squaresZ >> 1) + 1;
        level.cells.resize(static_cast<std::size_t>(level.cellsX) * static_cast<std::size_t>(level.cellsZ));
        for (int cz = 0; cz < level.cellsZ; ++cz) {
            for (int cx = 0; cx < level.cellsX; ++cx) {
                std::uint16_t highest = 0;
                for (int z = 2 * cz; z <= 2 * cz + 2; ++z) {
                    for (int x = 2 * cx; x <= 2 * cx + 2; ++x) {
                        highest = std::max(highest, rawAt(field, x, z));
                    }
                }
                level.cells[static_cast<std::size_t>(cz) * static_cast<std::size_t>(level.cellsX)
                            + static_cast<std::size_t>(cx)] = highest;
            }
        }
        levels_.push_back(std::move(level));
    }

    // Every further level from the one below: cell `i` is children `2i` and `2i+1`, and a
    // child past the last one clamps to it — which is exactly the corner clamp one level up.
    for (int L = 2; L <= top; ++L) {
        const Level& below = levels_.back();
        Level level;
        level.cellsX = (field.squaresX >> L) + 1;
        level.cellsZ = (field.squaresZ >> L) + 1;
        level.cells.resize(static_cast<std::size_t>(level.cellsX) * static_cast<std::size_t>(level.cellsZ));
        const auto childAt = [&below](int cx, int cz) noexcept -> std::uint16_t {
            const int x = std::min(cx, below.cellsX - 1);
            const int z = std::min(cz, below.cellsZ - 1);
            return below.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(below.cellsX)
                               + static_cast<std::size_t>(x)];
        };
        for (int cz = 0; cz < level.cellsZ; ++cz) {
            for (int cx = 0; cx < level.cellsX; ++cx) {
                const std::uint16_t highest = std::max(
                    std::max(childAt(2 * cx, 2 * cz), childAt(2 * cx + 1, 2 * cz)),
                    std::max(childAt(2 * cx, 2 * cz + 1), childAt(2 * cx + 1, 2 * cz + 1)));
                level.cells[static_cast<std::size_t>(cz) * static_cast<std::size_t>(level.cellsX)
                            + static_cast<std::size_t>(cx)] = highest;
            }
        }
        levels_.push_back(std::move(level));
    }
}

std::uint16_t MaxHeightPyramid::maxRaw(int level, int cellX, int cellZ) const noexcept {
    if (level < 1 || level > levelCount()) {
        return 0;
    }
    const Level& l = levels_[static_cast<std::size_t>(level - 1)];
    const int x = std::clamp(cellX, 0, l.cellsX - 1);
    const int z = std::clamp(cellZ, 0, l.cellsZ - 1);
    return l.cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(l.cellsX)
                   + static_cast<std::size_t>(x)];
}

} // namespace rm
