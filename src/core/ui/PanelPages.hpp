#pragma once

#include "core/Types.hpp"
#include "core/sim/IdPool.hpp"
#include "core/ui/GameProfile.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rm::ui {

/// Mutable page ownership for the three paged lower-deck instruments.
///
/// This stores intent only. BuildPanel and Roster remain the authorities that clamp a requested
/// page to current content and viewport capacity; the caller writes their answer back here.
class PanelPages {
public:
    /// Page remembered by builder TYPE, so another unit of the same type presents the same menu.
    [[nodiscard]] std::size_t& build(UnitTypeIndex builderType);

    /// Records the exact ordered identity whose roster was just rebuilt. A changed identity
    /// resets the page before that roster is laid out. UnitId includes generation, so a new unit
    /// reusing a dead unit's slot is a new selection.
    void showRoster(std::span<const sim::UnitId> selection);

    /// Page of the roster last recorded by showRoster. Input reads this without a selection so
    /// it keeps addressing the tiles actually on screen until the next frame rebuilds them.
    [[nodiscard]] std::size_t& roster() noexcept { return rosterPage_; }

    /// Page remembered independently for each concrete game-interface profile.
    [[nodiscard]] std::size_t& commands(GameProfile profile) noexcept;

private:
    std::vector<std::size_t> buildByType_;
    std::vector<sim::UnitId> rosterIdentity_;
    std::size_t rosterPage_ = 0;
    std::array<std::size_t, 4> commandByProfile_{};
};

} // namespace rm::ui
