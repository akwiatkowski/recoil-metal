#include "core/ui/CommandPanel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace rm::ui {

CommandAvailability
commandAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept {
    bool hasSelection = false;
    bool hasMove = false;
    bool hasPatrol = false;
    bool hasStop = false;
    bool hasOrdinaryWeapon = false;
    bool hasAssister = false;
    bool hasReclaimer = false;
    bool hasRepairer = false;
    bool hasManualWeapon = false;

    for (const unitdef::UnitDef* def : selection) {
        if (def == nullptr) {
            continue;
        }

        hasSelection = true;
        const auto permits = [def](std::string_view cap) {
            return !def->commandCapsDeclared || def->hasCommandCap(cap);
        };
        hasMove = hasMove || (def->isMobile() && permits("RULEUCC_Move"));
        hasPatrol = hasPatrol || (def->isMobile() && permits("RULEUCC_Patrol"));
        hasStop = hasStop || permits("RULEUCC_Stop");
        hasOrdinaryWeapon = hasOrdinaryWeapon
                         || (permits("RULEUCC_Attack")
                             && std::ranges::any_of(def->weapons, &unitdef::Weapon::fires));
        hasAssister = hasAssister
                   || (permits("RULEUCC_Guard")
                       && (def->isBuilder() || def->hasCategory("COMMAND")));
        hasReclaimer = hasReclaimer
                    || (def->isBuilder() && permits("RULEUCC_Reclaim"));
        hasRepairer = hasRepairer || (def->isBuilder() && permits("RULEUCC_Repair"));
        hasManualWeapon = hasManualWeapon
                       || (permits("RULEUCC_Overcharge")
                           && std::ranges::any_of(def->weapons, [](const unitdef::Weapon& weapon) {
                              // The current input path enters Overcharge mode only for a charged
                              // manual weapon; a zero-cost manual weapon falls back to Attack.
                              return weapon.manuallyFired()
                                  && weapon.energyRequired > sim::Mag{};
                          }));
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
            available[slot] = hasMove;
            break;
        case sim::CommandKind::Patrol:
            available[slot] = hasPatrol;
            break;
        case sim::CommandKind::Stop:
            available[slot] = hasSelection && hasStop;
            break;
        case sim::CommandKind::Attack:
            available[slot] = hasOrdinaryWeapon;
            break;
        case sim::CommandKind::Assist:
            available[slot] = hasAssister;
            break;
        case sim::CommandKind::Reclaim:
            available[slot] = hasReclaimer;
            break;
        case sim::CommandKind::Repair:
            available[slot] = hasRepairer;
            break;
        case sim::CommandKind::Overcharge:
            available[slot] = hasManualWeapon;
            break;
        case sim::CommandKind::Build:
        case sim::CommandKind::ToggleFactoryRepeat:
        case sim::CommandKind::Script:
            break;  // None has a command-rack descriptor.
        }
    }
    return available;
}

namespace {

constexpr float kRackPadding = 8.0f;
constexpr float kRackHeader = 22.0f;
constexpr float kRackGap = 4.0f;

} // namespace

CommandRackLayout commandRackLayout(const FrameLayout& frame, bool hasSelection) noexcept {
    CommandRackLayout layout;
    if (!hasSelection || frame.commands.width <= kRackPadding * 2.0f
        || frame.commands.height <= kRackPadding * 2.0f + kRackHeader) {
        return layout;
    }

    layout.rect = frame.commands;
    layout.gridX = frame.commands.x + kRackPadding;
    layout.gridY = frame.commands.y + kRackPadding + kRackHeader;
    layout.cellWidth = (frame.commands.width - kRackPadding * 2.0f
                        - kRackGap * static_cast<float>(kCommandColumns - 1))
                     / static_cast<float>(kCommandColumns);
    layout.cellHeight = (frame.commands.height - kRackPadding * 2.0f - kRackHeader
                         - kRackGap * static_cast<float>(kCommandRows - 1))
                      / static_cast<float>(kCommandRows);
    layout.visible = layout.cellWidth > 0.0f && layout.cellHeight > 0.0f;
    return layout;
}

bool insideCommandRack(const CommandRackLayout& layout, float pointX, float pointY) noexcept {
    return layout.visible && layout.rect.contains(pointX, pointY);
}

std::array<float, 2> commandCellOrigin(const CommandRackLayout& layout,
                                       std::size_t slot) noexcept {
    const std::size_t column = slot % kCommandColumns;
    const std::size_t row = slot / kCommandColumns;
    return {{layout.gridX + static_cast<float>(column) * (layout.cellWidth + kRackGap),
             layout.gridY + static_cast<float>(row) * (layout.cellHeight + kRackGap)}};
}

std::optional<std::size_t> commandSlotAt(const CommandRackLayout& layout, float pointX,
                                         float pointY) noexcept {
    if (!layout.visible || pointX < layout.gridX || pointY < layout.gridY) {
        return std::nullopt;
    }
    const float pitchX = layout.cellWidth + kRackGap;
    const float pitchY = layout.cellHeight + kRackGap;
    const auto column = static_cast<int>(std::floor((pointX - layout.gridX) / pitchX));
    const auto row = static_cast<int>(std::floor((pointY - layout.gridY) / pitchY));
    if (column < 0 || column >= static_cast<int>(kCommandColumns) || row < 0
        || row >= static_cast<int>(kCommandRows)) {
        return std::nullopt;
    }
    const float localX = pointX - layout.gridX - static_cast<float>(column) * pitchX;
    const float localY = pointY - layout.gridY - static_cast<float>(row) * pitchY;
    if (localX >= layout.cellWidth || localY >= layout.cellHeight) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row) * kCommandColumns
         + static_cast<std::size_t>(column);
}

InfoCard commandCard(const CommandDescriptor& command, bool available, bool armed) {
    InfoCard card;
    card.title = command.name.empty() ? "UNAVAILABLE COMMAND" : std::string{command.name};
    card.rows.push_back(InfoRow{
        .label = "STATE",
        .value = armed ? "TARGETING" : (available ? "READY" : "UNAVAILABLE"),
        .tint = available ? kGain : kLoss,
    });
    if (command.kind && *command.kind != sim::CommandKind::Stop) {
        card.rows.push_back(InfoRow{.label = "TARGET", .value = "WORLD / UNIT"});
    }
    return card;
}

void appendCommandRack(Geometry& out, const text::Font& labelFont,
                       const text::Font& readoutFont, const Theme& theme,
                       const CommandRackLayout& layout,
                       const CommandAvailability& available,
                       std::optional<std::size_t> hovered,
                       std::optional<sim::CommandKind> armed) {
    if (!layout.visible || !labelFont.usable()) {
        return;
    }
    (void)readoutFont;

    appendPanel(out, labelFont, theme, layout.rect.x, layout.rect.y, layout.rect.width,
                layout.rect.height);
    (void)text::appendText(out.label, labelFont.glyphs, "COMMANDS",
                           layout.rect.x + kRackPadding,
                           layout.rect.y + kRackPadding + 14.0f, theme.label);
    text::appendRect(out.chrome, labelFont, layout.rect.x + kRackPadding,
                     layout.gridY - kBevel, layout.rect.width - kRackPadding * 2.0f,
                     kBevel, fade(theme.edge, 0.9f));

    for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
        const CommandDescriptor& command = kCommandDescriptors[slot];
        const bool enabled = command.kind.has_value() && available[slot];
        const bool active = command.kind && armed == command.kind;
        const auto origin = commandCellOrigin(layout, slot);
        const Colour well = fade(theme.well, enabled ? 1.0f : 0.42f);
        text::appendRectV(out.chrome, labelFont, origin[0], origin[1], layout.cellWidth,
                          layout.cellHeight,
                          Colour{{well[0] * 1.5f, well[1] * 1.5f, well[2] * 1.5f, well[3]}},
                          Colour{{well[0] * 0.7f, well[1] * 0.7f, well[2] * 0.7f, well[3]}});
        if (active || hovered == slot) {
            const Colour edge = active ? kWarn : theme.edgeLit;
            const float weight = kBevel * 2.0f;
            const Colour lit = fade(edge, 0.95f);
            text::appendRect(out.chrome, labelFont, origin[0], origin[1],
                             layout.cellWidth, weight, lit);
            text::appendRect(out.chrome, labelFont, origin[0],
                             origin[1] + layout.cellHeight - weight, layout.cellWidth,
                             weight, lit);
            text::appendRect(out.chrome, labelFont, origin[0], origin[1], weight,
                             layout.cellHeight, lit);
            text::appendRect(out.chrome, labelFont,
                             origin[0] + layout.cellWidth - weight, origin[1], weight,
                             layout.cellHeight, lit);
        }

        const std::string label = command.name.empty() ? "--" : std::string{command.name};
        const std::vector<std::string> lines =
            wrapToWidth(labelFont.glyphs, label, layout.cellWidth - 6.0f, 2, 0.72f);
        const float lineHeight = std::max(9.0f, labelFont.lineHeight * 0.72f);
        const float firstBaseline = origin[1] + layout.cellHeight * 0.5f
                                  - (static_cast<float>(lines.size()) - 1.0f)
                                        * lineHeight * 0.5f
                                  + 3.0f;
        for (std::size_t line = 0; line < lines.size(); ++line) {
            const float width = text::measureText(labelFont.glyphs, lines[line], 0.72f);
            (void)text::appendText(out.label, labelFont.glyphs, lines[line],
                                   origin[0] + (layout.cellWidth - width) * 0.5f,
                                   firstBaseline + static_cast<float>(line) * lineHeight,
                                   fade(enabled ? kInk : theme.label, enabled ? 0.95f : 0.42f),
                                   0.72f);
        }
    }
}

} // namespace rm::ui
