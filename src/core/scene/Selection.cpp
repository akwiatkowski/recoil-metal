#include "core/scene/Selection.hpp"

#include <algorithm>

namespace rm {

std::vector<SelectionEntry> applyClick(std::span<const SelectionEntry> current,
                                       std::optional<SelectionEntry> hit, bool addToSet) {
    if (!hit) {
        // A miss while adding is a near-miss, not an instruction to throw the
        // selection away.
        return addToSet ? std::vector<SelectionEntry>{current.begin(), current.end()}
                        : std::vector<SelectionEntry>{};
    }

    const auto existing = std::find(current.begin(), current.end(), *hit);
    if (existing != current.end()) {
        std::vector<SelectionEntry> without;
        without.reserve(current.size() - 1);
        std::copy_if(current.begin(), current.end(), std::back_inserter(without),
                     [&hit](const SelectionEntry& entry) { return !(entry == *hit); });
        return without;
    }

    if (!addToSet) {
        return {*hit};
    }

    std::vector<SelectionEntry> extended{current.begin(), current.end()};
    extended.push_back(*hit);
    return extended;
}

} // namespace rm
