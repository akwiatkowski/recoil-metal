#include "core/map/HeightField.hpp"

#include <algorithm>

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

float HeightField::widthElmos() const noexcept {
    return static_cast<float>(squaresX) * static_cast<float>(kSquareSize);
}

float HeightField::depthElmos() const noexcept {
    return static_cast<float>(squaresZ) * static_cast<float>(kSquareSize);
}

} // namespace rm
