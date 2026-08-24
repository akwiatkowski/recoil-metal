#include "core/sim/Terrain.hpp"

#include <algorithm>
#include <cmath>

namespace rm::sim {

Terrain::Terrain(const HeightField& field, bool hasWater, float waterLevelElmos) noexcept
    : field_(&field),
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

    // `raw` is a `uint16` and the scale is kept to 2^-30, so the product is exact to well
    // inside one step of the result. 65,535 times a scale of ~0.01 at 2^30 is about 7e11 —
    // comfortable in 64 bits, and nowhere near what a 32-bit intermediate would survive.
    const FxWide scaled = FxWide{field_->raw[index]} * heightScale_;
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

} // namespace rm::sim
