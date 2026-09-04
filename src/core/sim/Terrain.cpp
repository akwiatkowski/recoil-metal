#include "core/sim/Terrain.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace rm::sim {

Terrain::Terrain(const HeightField& field, bool hasWater, float waterLevelElmos,
                 const MaxHeightPyramid* lookAhead) noexcept
    : field_(&field),
      lookAhead_(lookAhead),
      baseHeight_(fxFromFloat(field.baseHeight)),
      hasWater_(hasWater),
      waterLevel_(fxFromFloat(waterLevelElmos)),
      // Construction is content load, so the float is allowed here — it is the boundary, and
      // it is crossed once per map rather than once per sample.
      heightScale_(static_cast<FxWide>(
          std::llround(static_cast<double>(field.heightScale)
                       * static_cast<double>(FxWide{1} << kScaleBits)))) {}

Fx Terrain::cornerHeight(std::int32_t x, std::int32_t z) const noexcept {
    const std::int32_t cx = std::clamp(x, 0, field_->squaresX);
    const std::int32_t cz = std::clamp(z, 0, field_->squaresZ);

    const auto index = static_cast<std::size_t>(cz)
                           * static_cast<std::size_t>(field_->verticesX())
                       + static_cast<std::size_t>(cx);
    if (index >= field_->raw.size()) {
        return baseHeight_;
    }

    return decodeRaw(field_->raw[index]);
}

Fx Terrain::decodeRaw(std::uint16_t raw) const noexcept {
    // `raw` is a `uint16` and the scale is kept to 2^-30, so the product is exact to well
    // inside one step of the result. 65,535 times a scale of ~0.01 at 2^30 is about 7e11 —
    // comfortable in 64 bits, and nowhere near what a 32-bit intermediate would survive.
    const FxWide scaled = FxWide{raw} * heightScale_;
    return baseHeight_ + Fx::fromRaw(saturate(roundShift(scaled, kScaleBits
                                                                     - kFxFractionalBits)));
}

Fx Terrain::heightAt(Fx x, Fx z) const noexcept {
    if (field_->squaresX <= 0 || field_->squaresZ <= 0) {
        return baseHeight_;
    }

    // World elmos to grid coordinates. A square is 8 elmos and 8 is a power of two, so this
    // division is exact — no rounding enters the sample position at all, which is a small
    // piece of luck worth noting: on a grid of any other pitch the fraction below would carry
    // a rounding error into every height.
    const Fx gridX = x / Fx::fromInt(kSquareSize);
    const Fx gridZ = z / Fx::fromInt(kSquareSize);

    // Clamped BEFORE the floor, so a position far off the map cannot overflow the cast — the
    // same order `heightAtWorld` uses and for the same reason.
    const Fx clampedX = std::clamp(gridX, Fx{}, Fx::fromInt(field_->squaresX));
    const Fx clampedZ = std::clamp(gridZ, Fx{}, Fx::fromInt(field_->squaresZ));

    // `floorToInt`, not a truncation: truncation rounds toward zero and would mirror the
    // interpolation for negative coordinates instead of clamping it.
    const std::int32_t x0 = clampedX.floorToInt();
    const std::int32_t z0 = clampedZ.floorToInt();
    const Fx fx = clampedX - Fx::fromInt(x0);
    const Fx fz = clampedZ - Fx::fromInt(z0);

    const Fx h00 = cornerHeight(x0, z0);
    const Fx h10 = cornerHeight(x0 + 1, z0);
    const Fx h01 = cornerHeight(x0, z0 + 1);
    const Fx h11 = cornerHeight(x0 + 1, z0 + 1);

    // Interpolating decoded heights rather than raw words is equivalent — the decode is
    // affine — and keeps everything in one type.
    const Fx alongZ0 = h00 + (h10 - h00) * fx;
    const Fx alongZ1 = h01 + (h11 - h01) * fx;
    return alongZ0 + (alongZ1 - alongZ0) * fz;
}

Fx Terrain::surfaceHeightAt(Fx x, Fx z) const noexcept {
    const Fx ground = heightAt(x, z);
    return hasWater_ ? std::max(ground, waterLevel_) : ground;
}

Fx Terrain::maxSurfaceHeightNear(Fx x, Fx z, Fx reachElmos) const noexcept {
    // Retail works in ogrids — one heightmap sample per ogrid, one square here — and
    // takes the point sample under a reach of one (`0x006340f0`).
    const Fx reachSquares = reachElmos / Fx::fromInt(kSquareSize);
    if (reachSquares < Fx::fromInt(1) || field_->squaresX <= 0 || field_->squaresZ <= 0) {
        return surfaceHeightAt(x, z);
    }

    // The pyramid level: the highest set bit of half the reach, plus one — so the cell is
    // at least half the reach wide and less than the whole of it — capped by the level
    // whose cell is the map. `bsr` of a positive integer is `bit_width − 1`.
    const auto bsr = [](std::int32_t value) noexcept -> int {
        return value > 0 ? std::bit_width(static_cast<std::uint32_t>(value)) - 1 : -1;
    };
    const int mapCap = bsr(std::min(field_->squaresX, field_->squaresZ) - 2) + 1;
    int level = bsr((reachSquares * Fx::fromRatio(1, 2)).floorToInt()) + 1;
    level = std::max(1, std::min(level, mapCap));

    const Fx gridX = std::clamp(x / Fx::fromInt(kSquareSize), Fx{}, Fx::fromInt(field_->squaresX));
    const Fx gridZ = std::clamp(z / Fx::fromInt(kSquareSize), Fx{}, Fx::fromInt(field_->squaresZ));
    const std::int32_t cellX = gridX.floorToInt() >> level;
    const std::int32_t cellZ = gridZ.floorToInt() >> level;

    // The pyramid answers in one load. It holds raw maxima, which are height maxima only
    // while the scale is positive; a downhill scale takes the scan below instead.
    if (lookAhead_ != nullptr && heightScale_ > 0 && level <= lookAhead_->levelCount()) {
        const Fx highest = decodeRaw(lookAhead_->maxRaw(level, cellX, cellZ));
        return hasWater_ ? std::max(highest, waterLevel_) : highest;
    }

    const std::int32_t first = 1 << level;

    // Every corner of the cell, inclusive of its far edge: a square's height is decided by
    // its four corners, so the last corner row belongs to the cell as much as the first.
    Fx highest = baseHeight_;
    bool any = false;
    for (std::int32_t cz = cellZ * first; cz <= (cellZ + 1) * first; ++cz) {
        for (std::int32_t cx = cellX * first; cx <= (cellX + 1) * first; ++cx) {
            const Fx h = cornerHeight(cx, cz);
            highest = any ? std::max(highest, h) : h;
            any = true;
        }
    }
    return hasWater_ ? std::max(highest, waterLevel_) : highest;
}

} // namespace rm::sim
