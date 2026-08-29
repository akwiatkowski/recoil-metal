#pragma once

#include "core/sim/Command.hpp"
#include "core/unit/UnitDef.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace rm::ui {

/// One stable position in the order rack.
///
/// The positions follow Forged Alliance's horizontal order panel. Empty positions are kept
/// rather than compacted so selecting a unit with fewer abilities never moves the common orders.
struct CommandDescriptor {
    std::optional<sim::CommandKind> kind;
    std::string_view name;
    std::string_view icon;
};

inline constexpr std::size_t kCommandColumns = 4;
inline constexpr std::size_t kCommandRows = 3;
inline constexpr std::size_t kCommandSlots = kCommandColumns * kCommandRows;

using CommandDescriptors = std::array<CommandDescriptor, kCommandSlots>;
using CommandAvailability = std::array<bool, kCommandSlots>;

/// The fixed FA order positions, truncated to the requested 4x3 rack.
///
/// Slots are zero-based here and one-based in FA's `preferredSlot`: AttackMove 1 through Assist
/// 6, fire-state 7, Overcharge 8, unit-specific actions 9-11, and Reclaim 12. Build is absent on
/// purpose: choosing a blueprint belongs to the construction panel, not the order rack.
inline constexpr CommandDescriptors kCommandDescriptors{{
    {{sim::CommandKind::AttackMove}, "ATTACK MOVE", "attack_move"},
    {{sim::CommandKind::Move}, "MOVE", "move"},
    {{sim::CommandKind::Attack}, "ATTACK", "attack"},
    {{sim::CommandKind::Patrol}, "PATROL", "patrol"},
    {{sim::CommandKind::Stop}, "STOP", "stop"},
    {{sim::CommandKind::Assist}, "ASSIST", "guard"},
    {std::nullopt, {}, {}},  // FA fire-state: not implemented by the simulation.
    {{sim::CommandKind::Overcharge}, "OVERCHARGE", "overcharge"},
    {std::nullopt, {}, {}},  // FA unit-specific action.
    {std::nullopt, {}, {}},  // FA launch, teleport, ferry, or sacrifice action.
    {std::nullopt, {}, {}},  // FA dive or another unit-specific action.
    {{sim::CommandKind::Reclaim}, "RECLAIM", "reclaim"},
}};

/// Enabled state parallel to `kCommandDescriptors` for one selection.
///
/// Mixed selections use ANY semantics: a command is enabled when at least one selected unit can
/// execute it, matching the existing order path which attempts the command per selected handle.
/// Null definitions are ignored; an empty or wholly unknown selection disables every slot.
[[nodiscard]] CommandAvailability
commandAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept;

} // namespace rm::ui
