#include "core/sim/FeatureStore.hpp"

#include <stdexcept>
#include <utility>

namespace rm::sim {

FeatureStore::FeatureStore(const Snapshot& snapshot)
    : features_(snapshot.features),
      ids_(snapshot.ids),
      generations_(snapshot.generations),
      revision_(snapshot.revision) {
    if (generations_.size() != features_.size()) {
        throw std::invalid_argument("feature snapshot generations do not match slots");
    }
}

FeatureStore::Snapshot FeatureStore::snapshot() const {
    return Snapshot{.ids = ids_.snapshot(),
                    .generations = generations_,
                    .features = features_,
                    .revision = revision_};
}

FeatureId FeatureStore::add(const Feature& feature) {
    const FeatureId id = ids_.acquire();
    const auto slot = static_cast<std::size_t>(id.index);
    if (slot >= features_.size()) {
        // The pool only ever grows by one, so this is an append. Same contract as
        // `UnitStore::spawn`, and asserted by construction rather than checked.
        features_.emplace_back();
        generations_.emplace_back();
    }
    features_[slot] = feature;
    generations_[slot] = id.generation;
    ++revision_;
    return id;
}

void FeatureStore::remove(FeatureId id) {
    if (!ids_.alive(id)) {
        return;  // a stale handle removes nothing — least of all whoever moved in
    }
    ids_.release(id);
    ++revision_;
}

const Feature* FeatureStore::find(FeatureId id) const noexcept {
    if (!ids_.alive(id) || id.index >= features_.size()) {
        return nullptr;
    }
    return &features_[id.index];
}

Feature* FeatureStore::findMutable(FeatureId id) noexcept {
    // The const lookup does the checking; casting its answer back is safe because the
    // storage is ours and non-const. One implementation, not two that can drift.
    return const_cast<Feature*>(std::as_const(*this).find(id));
}

void FeatureStore::clear() noexcept {
    features_.clear();
    generations_.clear();
    ids_ = IdPool{};
    ++revision_;
}

} // namespace rm::sim
