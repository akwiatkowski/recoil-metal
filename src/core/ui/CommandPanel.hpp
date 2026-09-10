#pragma once

#include "core/sim/Command.hpp"
#include "core/ui/Hud.hpp"
#include "core/unit/UnitDef.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <map>
#include <string>
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
    {{sim::CommandKind::Dive}, "DIVE / SURFACE", "dive"},
    {{sim::CommandKind::Overcharge}, "OVERCHARGE", "overcharge"},
    {{sim::CommandKind::Repair}, "REPAIR", "repair"},
    {{sim::CommandKind::Assist}, "ASSIST", "assist"},
    {std::nullopt, "AUTO MEX", "auto_expand", RackAction::AutoExpand},
    {{sim::CommandKind::Reclaim}, "RECLAIM", "reclaim"},
}};

/// One retail unit-toggle rule (`lua/ui/game/orders.lua`, `# Unit toggle rules`).
/// Slots are 0-based here like the order table above (retail states them 1-based):
/// Shield/Weapon share Dive's slot, Jamming/Intel Overcharge's, Production/Stealth
/// Repair's, Generic Assist's, Special/Cloak the app action's. No shipped unit
/// authors both toggles of any shared slot, so the pair never collides in practice.
struct ToggleDescriptor {
    std::string_view cap;   ///< `RULEUTC_*` key, matching `UnitDef::toggleCaps`
    std::string_view label; ///< English cell label for the retail help key
    std::string_view icon;  ///< retail bitmapId; the atlas falls back when absent
    std::size_t slot;       ///< 0-based into `kCommandDescriptors`
};
inline constexpr std::array<ToggleDescriptor, 9> kToggleDescriptors{{
    {"RULEUTC_ShieldToggle", "SHIELD", "shield", 6},
    {"RULEUTC_WeaponToggle", "WEAPON", "toggle-weapon", 6},
    {"RULEUTC_JammingToggle", "JAMMING", "jamming", 7},
    {"RULEUTC_IntelToggle", "INTEL", "intel", 7},
    {"RULEUTC_ProductionToggle", "PRODUCTION", "production", 8},
    {"RULEUTC_StealthToggle", "STEALTH", "stealth", 8},
    {"RULEUTC_GenericToggle", "GENERIC", "production", 9},
    {"RULEUTC_SpecialToggle", "SPECIAL", "activate-weapon", 10},
    {"RULEUTC_CloakToggle", "CLOAK", "intel-counter", 10},
}};
using ToggleAvailability = std::array<bool, 9>;

/// One resolved rack cell: an order, a toggle filling its dead order slot, or the
/// dead order itself. Name/icon views borrow from the descriptor tables and the
/// selection's override records, so a page must not outlive either.
struct CommandPageCell {
    std::string_view name;
    std::string_view icon;
    bool enabled = false;
    /// `kToggleDescriptors` index when this cell is a toggle; otherwise an order.
    std::optional<std::size_t> toggle;
};
using CommandPage = std::array<CommandPageCell, kCommandSlots>;

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

/// Toggle presence parallel to `kToggleDescriptors`: true when at least one selected
/// unit authors the cap true. Presence is data-driven; nothing enables a toggle yet,
/// because no simulation state backs any of them — the page renders present toggles
/// visibly disabled. Undeclared tables mean no toggles, never all of them.
[[nodiscard]] ToggleAvailability
toggleAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept;

/// Retail's override merge over a selection: an order key survives only when every
/// unit stating it agrees on bitmap and help; any conflict drops the key, and units
/// without the key do not vote. Views borrow from the selection's definitions.
[[nodiscard]] std::map<std::string, unitdef::UnitDef::OrderOverride, std::less<>>
orderOverrides(std::span<const unitdef::UnitDef* const> selection);

/// The resolved page for one selection: order cells where their command is available,
/// toggles filling dead order slots at their retail preferred slot (first table entry
/// wins a shared slot), overrides applied to both. A page must not outlive the
/// selection's definitions.
[[nodiscard]] CommandPage
commandPage(std::span<const unitdef::UnitDef* const> selection) noexcept;

/// The hover card for a toggle cell: present-but-unsupported, with the count it would
/// apply to once its simulation state exists.
[[nodiscard]] InfoCard toggleCard(const ToggleDescriptor& toggle,
                                  std::span<const unitdef::UnitDef* const> selection);

/// The hover inspector for a rack slot: the toggle card on toggle cells, the order
/// card elsewhere. Slot must be a live rack position.
[[nodiscard]] InfoCard commandInspector(const CommandPage& page, std::size_t slot,
                                        std::span<const unitdef::UnitDef* const> selection,
                                        bool armed = false);

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
/// `armed` lights the command being targeted. Cells come from `commandPage`: orders where
/// available, toggles filling dead slots, overrides applied.
void appendCommandRack(Geometry& out, const text::Font& labelFont,
                       const text::Font& readoutFont, const Theme& theme,
                       const CommandRackLayout& layout,
                       const CommandPage& page,
                       std::optional<std::size_t> hovered = std::nullopt,
                       std::optional<sim::CommandKind> armed = std::nullopt,
                       const CommandAvailability& engaged = CommandAvailability{});

} // namespace rm::ui
