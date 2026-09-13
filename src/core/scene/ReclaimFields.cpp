#include "core/scene/ReclaimFields.hpp"

#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace rm {

std::vector<ReclaimField> reclaimFields(const sim::FeatureStore& features) {
    // Cell key → accumulator. std::map, not unordered_map: the iteration order IS the
    // label order, and a hash map's order is allowed to differ between runs.
    struct Accum {
        double weightX = 0.0;
        double weightZ = 0.0;
        double weight = 0.0;
        sim::Mag mass{};
        sim::Mag energy{};
        int wrecks = 0;
    };
    std::map<std::pair<std::int64_t, std::int64_t>, Accum> cells;

    for (UnitIndex slot = 0; slot < features.size(); ++slot) {
        if (!features.slotAlive(slot)) {
            continue;
        }
        const sim::Feature& wreck = features.all()[slot];
        if (wreck.massRemaining <= sim::Mag{} && wreck.energyRemaining <= sim::Mag{}) {
            continue;  // a bare scorch is no field
        }
        // The centroid is weighted by what the wreck is WORTH, not by count — the
        // label sits where the mass is, so a fat wreck pulls the number toward itself
        // and a scatter of debris only leans on it.
        const double weight =
            static_cast<double>(sim::magToFloat(wreck.massRemaining))
            + static_cast<double>(sim::magToFloat(wreck.energyRemaining));
        const std::pair key{
            static_cast<std::int64_t>(std::floor(
                sim::fxToFloat(wreck.at[0]) / kReclaimFieldCellElmos)),
            static_cast<std::int64_t>(std::floor(
                sim::fxToFloat(wreck.at[2]) / kReclaimFieldCellElmos))};
        Accum& cell = cells[key];
        cell.weightX += sim::fxToFloat(wreck.at[0]) * weight;
        cell.weightZ += sim::fxToFloat(wreck.at[2]) * weight;
        cell.weight += weight;
        cell.mass += wreck.massRemaining;
        cell.energy += wreck.energyRemaining;
        ++cell.wrecks;
    }

    std::vector<ReclaimField> out;
    out.reserve(cells.size());
    for (const auto& [key, cell] : cells) {
        out.push_back(ReclaimField{
            .x = sim::fxFromFloat(static_cast<float>(cell.weightX / cell.weight)),
            .z = sim::fxFromFloat(static_cast<float>(cell.weightZ / cell.weight)),
            .mass = cell.mass,
            .energy = cell.energy,
            .wrecks = cell.wrecks,
        });
    }
    return out;
}

} // namespace rm
