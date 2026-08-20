#include "core/sim/UnitCatalog.hpp"

namespace rm::sim {

UnitTypeIndex UnitCatalog::add(const unitdef::UnitDef* def) {
    const auto index = static_cast<UnitTypeIndex>(defs_.size());
    defs_.push_back(def);
    return index;
}

} // namespace rm::sim
