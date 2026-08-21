#include "core/sim/FeatureStore.hpp"

namespace rm::sim {

FeatureId FeatureStore::add(const Feature& feature) {
    const FeatureId id = ids_.acquire();
    const auto slot = static_cast<std::size_t>(id.index);
    if (slot >= features_.size()) {
        // The pool only ever grows by one, so this is an append. Same contract as
        // `UnitStore::spawn`, and asserted by construction rather than checked.
        features_.emplace_back();
    }
    features_[slot] = feature;
    return id;
}

const Feature* FeatureStore::find(FeatureId id) const noexcept {
    if (!ids_.alive(id) || id.index >= features_.size()) {
        return nullptr;
    }
    return &features_[id.index];
}

void FeatureStore::clear() noexcept {
    features_.clear();
    ids_ = IdPool{};
}

} // namespace rm::sim
