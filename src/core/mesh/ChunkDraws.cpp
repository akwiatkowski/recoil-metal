#include "core/mesh/ChunkDraws.hpp"

namespace rm {

int lodForDistance(float distance, float chunkWidth) noexcept {
    // A degenerate chunk has no width to measure distance in. Nothing sensible
    // to divide by, and the finest level is always safe.
    if (!(chunkWidth > 0.0f)) {
        return 0;
    }

    const float inChunks = distance / chunkWidth;
    if (inChunks < kLodNearChunks) {
        return 0;
    }
    return inChunks < kLodFarChunks ? 1 : 2;
}

} // namespace rm
