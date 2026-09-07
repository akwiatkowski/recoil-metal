#pragma once

#include "core/sim/Command.hpp"
#include "core/ui/Hud.hpp"
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
/// A rack button that is the APP's standing order rather than a sim command: it changes what
/// the app does with the selection every tick, and every order it produces is an ordinary
/// logged command. One so far.
enum class RackAction : std::uint8_t {
    AutoExpand,  ///< idle engineers keep claiming the nearest free deposit (ADR-109)
};

struct CommandDescriptor {
    std::optional<sim::CommandKind> kind;
    std::string_view name;
    std::string_view icon;
    std::optional<RackAction> action{};

    /// A cell that does something when pressed: a sim command or an app action.
    [[nodiscard]] bool implemented() const noexcept { return kind || action; }
};

inline constexpr std::size_t kCommandColumns = 4;
inline constexpr std::size_t kCommandRows = 3;
inline constexpr std::size_t kCommandSlots = kCommandColumns * kCommandRows;

using CommandDescriptors = std::array<CommandDescriptor, kCommandSlots>;
using CommandAvailability = std::array<bool, kCommandSlots>;

/// One immutable 4x3 fitting inside the universal frame's command rectangle.
struct CommandRackLayout {
    Rect rect;
    float gridX = 0.0f;
    float gridY = 0.0f;
    float cellWidth = 0.0f;
    float cellHeight = 0.0f;
    bool visible = false;
};

/// The fixed FA order positions, truncated to the requested 4x3 rack.
///
/// Slots are zero-based here and one-based in FA's `preferredSlot`: AttackMove 1 through Assist
/// 6, fire-state 7, Overcharge 8, Repair 9, unit-specific actions 10-11, and Reclaim 12. Build is absent on
/// purpose: choosing a blueprint belongs to the construction panel, not the order rack.
///
/// FA folds Assist into the Guard button (one `RULEUCC_Guard` order reads the target). This
/// simulation keeps them distinct — Guard follows a unit, Assist lends a build arm — so Assist
/// takes the first unit-specific slot, next to Repair, where a builder's rack has room for it.
inline constexpr CommandDescriptors kCommandDescriptors{{
    {{sim::CommandKind::AttackMove}, "ATTACK MOVE", "attack_move"},
    {{sim::CommandKind::Move}, "MOVE", "move"},
    {{sim::CommandKind::Attack}, "ATTACK", "attack"},
    {{sim::CommandKind::Patrol}, "PATROL", "patrol"},
    {{sim::CommandKind::Stop}, "STOP", "stop"},
    {{sim::CommandKind::Guard}, "GUARD", "guard"},
    {std::nullopt, {}, {}},  // FA fire-state: not implemented by the simulation.
    {{sim::CommandKind::Overcharge}, "OVERCHARGE", "overcharge"},
    {{sim::CommandKind::Repair}, "REPAIR", "repair"},
    {{sim::CommandKind::Assist}, "ASSIST", "assist"},
    {std::nullopt, "AUTO MEX", "auto_expand", RackAction::AutoExpand},
    {{sim::CommandKind::Reclaim}, "RECLAIM", "reclaim"},
}};

/// The rack slot an app action sits in.
[[nodiscard]] constexpr std::size_t rackSlotFor(RackAction action) noexcept {
    for (std::size_t slot = 0; slot < kCommandDescriptors.size(); ++slot) {
        if (kCommandDescriptors[slot].action == action) return slot;
    }
    return kCommandDescriptors.size();  // unreachable while every action has a cell
}

/// Enabled state parallel to `kCommandDescriptors` for one selection.
///
/// Mixed selections use ANY semantics: a command is enabled when at least one selected unit can
/// execute it, matching the existing order path which attempts the command per selected handle.
/// Null definitions are ignored; an empty or wholly unknown selection disables every slot.
[[nodiscard]] CommandAvailability
commandAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept;

[[nodiscard]] CommandRackLayout commandRackLayout(const FrameLayout& frame,
                                                   bool hasSelection) noexcept;

[[nodiscard]] bool insideCommandRack(const CommandRackLayout& layout, float pointX,
                                     float pointY) noexcept;

[[nodiscard]] std::array<float, 2> commandCellOrigin(const CommandRackLayout& layout,
                                                     std::size_t slot) noexcept;

/// The stable slot under a HUD point. Gutters and the header are deliberate misses.
[[nodiscard]] std::optional<std::size_t> commandSlotAt(const CommandRackLayout& layout,
                                                       float pointX, float pointY) noexcept;

[[nodiscard]] InfoCard commandCard(const CommandDescriptor& command,
                                   std::span<const unitdef::UnitDef* const> selection,
                                   bool armed = false);

/// `engaged` lights the cells whose standing order is ON for the whole selection, the way
/// `armed` lights the command being targeted.
void appendCommandRack(Geometry& out, const text::Font& labelFont,
                       const text::Font& readoutFont, const Theme& theme,
                       const CommandRackLayout& layout,
                       const CommandAvailability& available,
                       std::optional<std::size_t> hovered = std::nullopt,
                       std::optional<sim::CommandKind> armed = std::nullopt,
                       const CommandAvailability& engaged = CommandAvailability{});

} // namespace rm::ui
