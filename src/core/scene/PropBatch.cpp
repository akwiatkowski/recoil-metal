#include "core/scene/PropBatch.hpp"

#include <algorithm>

namespace rm {

void cullPropsByLevel(std::span<const UnitInstance> instances, std::array<float, 3> eye,
                      std::span<const float> cutoffsElmos, std::span<UnitInstance> out,
                      std::span<std::size_t> countsOut) noexcept {
    std::fill(countsOut.begin(), countsOut.end(), std::size_t{0});

    // A caller mistake either way, and both would otherwise overrun.
    if (out.size() < instances.size() || countsOut.size() != cutoffsElmos.size()
        || cutoffsElmos.empty()) {
        return;
    }

    // TWO passes, because the runs have to be contiguous and a level's offset is not
    // known until every instance has been placed. Counting first and writing second
    // costs one extra walk of the positions and saves a per-level scratch buffer —
    // and on the busiest stock map this runs 47 000 times a frame, so the thing worth
    // avoiding is the allocation, not the arithmetic.
    const auto levelOf = [&](const UnitInstance& instance) -> std::size_t {
        const float dx = instance.position[0] - eye[0];
        const float dy = instance.position[1] - eye[1];
        const float dz = instance.position[2] - eye[2];
        const float distanceSquared = dx * dx + dy * dy + dz * dz;

        for (std::size_t level = 0; level < cutoffsElmos.size(); ++level) {
            const float cutoff = cutoffsElmos[level];
            if (!(cutoff > 0.0f)) {
                continue;  // a level that is never the right one
            }
            // Compared squared, so the inner loop has no square root in it. An
            // infinite cutoff would square to infinity and still compare correctly,
            // but it is worth short-circuiting since it is the common case for the
            // DevTest props and for anything the blueprint left unstated.
            if (cutoff == std::numeric_limits<float>::infinity()
                || distanceSquared <= cutoff * cutoff) {
                return level;
            }
        }
        return cutoffsElmos.size();  // past every cutoff: not drawn
    };

    for (const UnitInstance& instance : instances) {
        const std::size_t level = levelOf(instance);
        if (level < countsOut.size()) {
            ++countsOut[level];
        }
    }

    // Where each level's run begins: the sum of the counts before it.
    std::array<std::size_t, 8> cursor{};
    const std::size_t levels = std::min(countsOut.size(), cursor.size());
    std::size_t at = 0;
    for (std::size_t level = 0; level < levels; ++level) {
        cursor[level] = at;
        at += countsOut[level];
    }

    for (const UnitInstance& instance : instances) {
        const std::size_t level = levelOf(instance);
        if (level >= levels) {
            continue;
        }
        out[cursor[level]] = instance;
        ++cursor[level];
    }
}

} // namespace rm
