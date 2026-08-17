#include "core/scene/UnitBatch.hpp"

#include <algorithm>
#include <numeric>

namespace rm {

std::vector<std::size_t> orderByTexturePair(std::span<const TexturePair> batches) {
    std::vector<std::size_t> order(batches.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    // stable_sort, not sort: two batches with the same pair must stay in the
    // order the caller gave them. The comparison is on the pair only — sorting
    // on the index too would be a needless total order that hides accidental
    // reordering from the test.
    std::stable_sort(order.begin(), order.end(), [batches](std::size_t a, std::size_t b) {
        if (batches[a].diffuse != batches[b].diffuse) {
            return batches[a].diffuse < batches[b].diffuse;
        }
        return batches[a].shading < batches[b].shading;
    });

    return order;
}

std::size_t textureBindCount(std::span<const TexturePair> batches,
                             std::span<const std::size_t> order) {
    std::size_t binds = 0;
    bool first = true;
    TexturePair bound;

    for (const std::size_t index : order) {
        if (index >= batches.size()) {
            continue;
        }
        if (first || !(batches[index] == bound)) {
            ++binds;
            bound = batches[index];
            first = false;
        }
    }

    return binds;
}

} // namespace rm
