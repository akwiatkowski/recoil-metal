#include "core/ui/CommandPanel.hpp"

#include "core/unit/Role.hpp"

#include <algorithm>

namespace rm::ui {

CommandAvailability
commandAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept {
    bool hasSelection = false;
    bool hasMobile = false;
    bool hasOrdinaryWeapon = false;
    bool hasAssister = false;
    bool hasBuilder = false;
    bool hasManualWeapon = false;

    for (const unitdef::UnitDef* def : selection) {
        if (def == nullptr) {
            continue;
        }

        hasSelection = true;
        hasMobile = hasMobile || def->isMobile();
        hasOrdinaryWeapon = hasOrdinaryWeapon
                         || std::ranges::any_of(def->weapons, &unitdef::Weapon::fires);
        const unitdef::Role role = unitdef::roleOf(*def);
        hasAssister = hasAssister || role == unitdef::Role::Builder
                   || role == unitdef::Role::Commander;
        hasBuilder = hasBuilder || def->isBuilder();
        hasManualWeapon = hasManualWeapon
                       || std::ranges::any_of(def->weapons, [](const unitdef::Weapon& weapon) {
                              // The current input path enters Overcharge mode only for a charged
                              // manual weapon; a zero-cost manual weapon falls back to Attack.
                              return weapon.manuallyFired()
                                  && weapon.energyRequired > sim::Mag{};
                          });
    }

    CommandAvailability available{};
    for (std::size_t slot = 0; slot < kCommandDescriptors.size(); ++slot) {
        const std::optional<sim::CommandKind> kind = kCommandDescriptors[slot].kind;
        if (!kind) {
            continue;
        }

        switch (*kind) {
        case sim::CommandKind::Move:
        case sim::CommandKind::AttackMove:
        case sim::CommandKind::Patrol:
            available[slot] = hasMobile;
            break;
        case sim::CommandKind::Stop:
            available[slot] = hasSelection;
            break;
        case sim::CommandKind::Attack:
            available[slot] = hasOrdinaryWeapon;
            break;
        case sim::CommandKind::Assist:
            available[slot] = hasAssister;
            break;
        case sim::CommandKind::Reclaim:
        case sim::CommandKind::Repair:
            available[slot] = hasBuilder;
            break;
        case sim::CommandKind::Overcharge:
            available[slot] = hasManualWeapon;
            break;
        case sim::CommandKind::Build:
        case sim::CommandKind::ToggleFactoryRepeat:
            break;  // None has a command-rack descriptor.
        }
    }
    return available;
}

} // namespace rm::ui
