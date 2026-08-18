#include "core/map/HeightField.hpp"

#include <algorithm>
#include <cmath>

namespace rm {

std::size_t HeightField::sampleCount() const noexcept {
    if (squaresX <= 0 || squaresZ <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(verticesX()) * static_cast<std::size_t>(verticesZ());
}

void HeightField::setVerticalRange(float minHeight, float maxHeight) noexcept {
    baseHeight = minHeight;
    heightScale = (maxHeight - minHeight) / kHeightQuantisationSteps;
}

float HeightField::heightAt(int x, int z) const noexcept {
    // Clamp rather than wrap or assert: the mesh builder samples (x±1, z±1) for
    // central-difference normals and would otherwise need an edge special case
    // at every border vertex. Clamping mirrors the height of the edge sample,
    // which yields the correct "flat continuation" normal at map borders.
    const int cx = std::clamp(x, 0, squaresX);
    const int cz = std::clamp(z, 0, squaresZ);

    const auto index = static_cast<std::size_t>(cz) * static_cast<std::size_t>(verticesX())
                     + static_cast<std::size_t>(cx);
    if (index >= raw.size()) {
        return baseHeight;
    }

    return baseHeight + static_cast<float>(raw[index]) * heightScale;
}

float HeightField::heightAtWorld(float x, float z) const noexcept {
    if (squaresX <= 0 || squaresZ <= 0) {
        return baseHeight;
    }

    // World elmos to grid coordinates. Heights live at square corners, so the
    // integer part names the corner at or before the position and the fraction
    // is how far across the square it sits.
    const float gridX = x / static_cast<float>(kSquareSize);
    const float gridZ = z / static_cast<float>(kSquareSize);

    // Clamped before the floor rather than after, so that a position far off
    // the map cannot overflow the cast. floor, not truncation: truncation
    // rounds toward zero and would mirror the interpolation for negative
    // coordinates instead of clamping it.
    const float clampedX = std::clamp(gridX, 0.0f, static_cast<float>(squaresX));
    const float clampedZ = std::clamp(gridZ, 0.0f, static_cast<float>(squaresZ));

    const auto x0 = static_cast<int>(std::floor(clampedX));
    const auto z0 = static_cast<int>(std::floor(clampedZ));
    const float fx = clampedX - static_cast<float>(x0);
    const float fz = clampedZ - static_cast<float>(z0);

    // heightAt clamps its own indices, so x0 + 1 on the far edge simply reads
    // the edge corner again — which is what makes the last row and column
    // interpolate to a flat continuation rather than wrapping.
    const float h00 = heightAt(x0, z0);
    const float h10 = heightAt(x0 + 1, z0);
    const float h01 = heightAt(x0, z0 + 1);
    const float h11 = heightAt(x0 + 1, z0 + 1);

    // Interpolating decoded heights rather than raw words is equivalent — the
    // decode is affine — and keeps the arithmetic in float throughout.
    const float alongZ0 = std::lerp(h00, h10, fx);
    const float alongZ1 = std::lerp(h01, h11, fx);
    return std::lerp(alongZ0, alongZ1, fz);
}

float HeightField::widthElmos() const noexcept {
    return static_cast<float>(squaresX) * static_cast<float>(kSquareSize);
}

float HeightField::depthElmos() const noexcept {
    return static_cast<float>(squaresZ) * static_cast<float>(kSquareSize);
}

} // namespace rm
