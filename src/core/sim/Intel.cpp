#include "core/sim/Intel.hpp"

#include <algorithm>
#include <cassert>

namespace rm::sim {

namespace {

/// Calls `row(halfWidth, z)` once for each line of a filled disc of `radius` squares.
///
/// Recoil's `MidpointCircleAlgoPerLine` (`LosMap.cpp:74-103`), transcribed. Integer
/// arithmetic throughout — no square roots, no trigonometry, hence nothing that could
/// differ between two machines.
///
/// THE `x != y` GUARD IS LOad-BEARING, not a micro-optimisation: without it the diagonal
/// where the two octants meet emits its row twice, and every count in that row would be
/// double. `covered` would survive that (removal doubles too) and every reader of the
/// number would not.
template <typename Row>
void midpointCircleRows(std::int32_t radius, const Row& row) {
    std::int32_t x = radius;
    std::int32_t z = 0;
    std::int32_t decisionOver2 = 1 - x;

    while (x >= z) {
        row(x, z);
        if (z != 0) {
            row(x, -z);
        }

        if (decisionOver2 <= 0) {
            ++z;
            decisionOver2 += 2 * z + 1;
        } else {
            if (x != z) {
                row(z, x);
                if (x != 0) {
                    row(z, -x);
                }
            }
            ++z;
            --x;
            decisionOver2 += 2 * (z - x) + 1;
        }
    }
}

} // namespace

IntelGrid::IntelGrid(Fx widthElmos, Fx depthElmos, int mipLevel) {
    assert(mipLevel >= 0 && mipLevel < 16);
    squareElmos_ = kElmosPerSquare << mipLevel;

    // FLOOR, so a map that is not a whole number of squares across loses its last partial
    // one rather than gaining a square that is mostly off the map. At least one either way:
    // a grid with no squares would make every coverage query answer "off the map", which
    // reads as an intel system that is switched off rather than one that is misconfigured.
    squaresX_ = std::max(1, widthElmos.floorToInt() / squareElmos_);
    squaresZ_ = std::max(1, depthElmos.floorToInt() / squareElmos_);

    counts_.assign(static_cast<std::size_t>(squaresX_) * static_cast<std::size_t>(squaresZ_),
                   std::uint16_t{0});
}

std::int32_t IntelGrid::squareAt(Fx x, Fx z) const noexcept {
    // Negative coordinates are off the map, and the check has to come BEFORE the division:
    // `-1 / 8` is 0 in C++, so a unit one elmo past the western border would otherwise
    // land in column zero and see from inside the map.
    if (x < kFxZero || z < kFxZero) {
        return kNoSquare;
    }

    const std::int32_t sx = x.floorToInt() / squareElmos_;
    const std::int32_t sz = z.floorToInt() / squareElmos_;
    if (sx >= squaresX_ || sz >= squaresZ_) {
        return kNoSquare;
    }
    return sz * squaresX_ + sx;
}

void IntelGrid::add(std::span<const std::int32_t> squares) noexcept {
    for (const std::int32_t square : squares) {
        ++counts_[static_cast<std::size_t>(square)];
    }
}

void IntelGrid::remove(std::span<const std::int32_t> squares) noexcept {
    for (const std::int32_t square : squares) {
        // An assert rather than a clamp. A count that reaches zero with a withdrawal still
        // to come means some emitter removed a shape it never added — which is a bookkeeping
        // bug in the caller, and clamping it here would hide the cause and leave the grid
        // quietly wrong for the rest of the match.
        assert(counts_[static_cast<std::size_t>(square)] > 0);
        --counts_[static_cast<std::size_t>(square)];
    }
}

void circleSquares(const IntelGrid& grid, Fx x, Fx z, Fx radius,
                   std::vector<std::int32_t>& squares) {
    squares.clear();

    const std::int32_t centre = grid.squareAt(x, z);
    if (centre == IntelGrid::kNoSquare) {
        return;
    }

    const std::int32_t squaresX = grid.squaresX();
    const std::int32_t squaresZ = grid.squaresZ();
    const std::int32_t centreX = centre % squaresX;
    const std::int32_t centreZ = centre / squaresX;

    // Elmos to squares, truncating — Recoil's `(unit->losRadius / SQUARE_SIZE) >> mipLevel`
    // (`LosHandler.cpp:139`). A radius that rounds to nothing still yields the one square
    // the emitter is standing on, which is what the loop below does at radius zero.
    const std::int32_t radiusSquares =
        std::max(0, radius.floorToInt() / grid.squareElmos().floorToInt());

    midpointCircleRows(radiusSquares, [&](std::int32_t halfWidth, std::int32_t rowZ) {
        const std::int32_t row = centreZ + rowZ;
        if (row < 0 || row >= squaresZ) {
            return;
        }
        // CLAMPED PER ROW, which is what stops a disc at the western border from
        // continuing into the eastern end of the row above it. The indices are one flat
        // array and nothing else would notice.
        const std::int32_t from = std::max(0, centreX - halfWidth);
        const std::int32_t to = std::min(squaresX - 1, centreX + halfWidth);
        for (std::int32_t column = from; column <= to; ++column) {
            squares.push_back(row * squaresX + column);
        }
    });
}

} // namespace rm::sim
