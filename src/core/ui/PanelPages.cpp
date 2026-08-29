#include "core/ui/PanelPages.hpp"

#include <algorithm>

namespace rm::ui {

std::size_t& PanelPages::build(UnitTypeIndex builderType) {
    const std::size_t index = static_cast<std::size_t>(builderType);
    if (index >= buildByType_.size()) {
        buildByType_.resize(index + 1, 0);
    }
    return buildByType_[index];
}

void PanelPages::showRoster(std::span<const sim::UnitId> selection) {
    if (!std::ranges::equal(selection, rosterIdentity_)) {
        rosterIdentity_.assign(selection.begin(), selection.end());
        rosterPage_ = 0;
    }
}

std::size_t& PanelPages::commands(GameProfile profile) noexcept {
    return commandByProfile_[static_cast<std::size_t>(profile)];
}

} // namespace rm::ui
