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
    bool hasDive = false;
    bool hasOrdinaryWeapon = false;
    bool hasGuard = false;
    bool hasReclaimer = false;
    bool hasRepairer = false;
    bool hasAssister = false;
    bool hasFieldBuilder = false;
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
        hasDive = hasDive || (def->motion == unitdef::MotionType::SurfacingSub
                              && def->hasCommandCap("RULEUCC_Dive"));
        hasOrdinaryWeapon = hasOrdinaryWeapon
                         || (permits("RULEUCC_Attack")
                             && std::ranges::any_of(def->weapons, &unitdef::Weapon::fires));
        hasGuard = hasGuard
                   || (permits("RULEUCC_Guard")
                       && (def->isMobile() || def->isBuilder() || def->hasCategory("COMMAND")));
        hasReclaimer = hasReclaimer
                    || (def->isBuilder() && permits("RULEUCC_Reclaim"));
        hasRepairer = hasRepairer || (def->isBuilder() && permits("RULEUCC_Repair"));
        // Any build arm can be lent — the sim's `validAssist` asks only for a builder, and a
        // factory's assist mirrors compatible production (`Assist.hpp`).
        hasAssister = hasAssister || def->isBuilder();
        // Auto-expand wants a builder that can WALK to the next deposit.
        hasFieldBuilder = hasFieldBuilder || (def->isBuilder() && def->isMobile());
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
        if (kCommandDescriptors[slot].action == RackAction::AutoExpand) {
            available[slot] = hasFieldBuilder;
            continue;
        }
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
        case sim::CommandKind::Dive:
            available[slot] = hasDive;
            break;
        case sim::CommandKind::Stop:
            available[slot] = hasSelection && hasStop;
            break;
        case sim::CommandKind::Attack:
            available[slot] = hasOrdinaryWeapon;
            break;
        case sim::CommandKind::Guard:
            available[slot] = hasGuard;
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
        case sim::CommandKind::Assist:
            available[slot] = hasAssister;
            break;
        case sim::CommandKind::Build:
        case sim::CommandKind::ToggleFactoryRepeat:
        case sim::CommandKind::ToggleProduction:
        case sim::CommandKind::CancelFactoryBuild:
        case sim::CommandKind::Script:
        case sim::CommandKind::ReclaimUnit:  // reached through Reclaim's descriptor, not its own
        case sim::CommandKind::Capture:  // no rack cell yet; issued through its own order path
        case sim::CommandKind::MissileLaunch:  // fills Reclaim's dead cell on a silo, below
        case sim::CommandKind::SiloBuildTactical:  // fills Repair's dead cell on a silo, below
        case sim::CommandKind::SiloBuildNuke:     // spills into the next dead unit-slot
        case sim::CommandKind::ToggleSiloAuto:   // a right-click on either build cell
        case sim::CommandKind::CycleBuildPriority:  // the B key and the production panel's cell
        case sim::CommandKind::SetBuildPriority:  // the economy window's cells, not the rack's
        case sim::CommandKind::CycleRetreatThreshold:  // the V key
        case sim::CommandKind::CycleTargetFocus:       // the X key
        case sim::CommandKind::LoadTransport:   // issued by right-clicking a carrier, below
        case sim::CommandKind::UnloadTransport: // the U key
        case sim::CommandKind::Ferry:          // the Y key
        case sim::CommandKind::SelfDestruct:  // the ctrl-K kill switch; no rack cell
            break;  // None has a command-rack descriptor.
        }
    }
    return available;
}

ToggleAvailability
toggleAvailability(std::span<const unitdef::UnitDef* const> selection) noexcept {
    ToggleAvailability present{};
    for (const unitdef::UnitDef* def : selection) {
        if (def == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < kToggleDescriptors.size(); ++i) {
            // Undeclared tables vote nothing: unlike orders, toggles have no capability
            // fallback to infer them from — except production, whose pause is a
            // capability rather than a cap: only two dozen retail units declare it,
            // while FAF offers it to everything that produces.
            const std::string_view cap = kToggleDescriptors[i].cap;
            if ((def->toggleCapsDeclared && def->hasToggleCap(cap))
                || (cap == "RULEUTC_ProductionToggle" && sim::canPauseProduction(*def))) {
                present[i] = true;
            }
        }
    }
    return present;
}

namespace {

/// Retail's unanimous rule for one key (`orders.lua`, `-- apply overrides`): the
/// first unit stating it sets it, a disagreeing unit drops it, and units without
/// the key do not vote. The winner borrows from its definition.
[[nodiscard]] const unitdef::UnitDef::OrderOverride*
agreedOverride(std::span<const unitdef::UnitDef* const> selection, std::string_view key) {
    const unitdef::UnitDef::OrderOverride* agreed = nullptr;
    for (const unitdef::UnitDef* def : selection) {
        if (def == nullptr) {
            continue;
        }
        const auto stated = def->orderOverrides.find(key);
        if (stated == def->orderOverrides.end()) {
            continue;
        }
        if (agreed == nullptr) {
            agreed = &stated->second;
        } else if (agreed->bitmapId != stated->second.bitmapId
                   || agreed->helpText != stated->second.helpText) {
            return nullptr;
        }
    }
    return agreed;
}

} // namespace

std::map<std::string, unitdef::UnitDef::OrderOverride, std::less<>>
orderOverrides(std::span<const unitdef::UnitDef* const> selection) {
    std::map<std::string, unitdef::UnitDef::OrderOverride, std::less<>> merged;
    for (const unitdef::UnitDef* def : selection) {
        if (def == nullptr) {
            continue;
        }
        for (const auto& [key, _] : def->orderOverrides) {
            if (merged.contains(key)) {
                continue;
            }
            if (const auto* agreed = agreedOverride(selection, key)) {
                merged.emplace(key, *agreed);
            }
        }
    }
    return merged;
}
namespace {

/// The `RULEUCC_*` key an override states for a rack order. Attack-move and Assist
/// have none: retail folds both into other orders, so no override can name them.
[[nodiscard]] std::optional<std::string_view>
orderKeyFor(sim::CommandKind kind) noexcept {
    switch (kind) {
    case sim::CommandKind::Move: return "RULEUCC_Move";
    case sim::CommandKind::Attack: return "RULEUCC_Attack";
    case sim::CommandKind::Patrol: return "RULEUCC_Patrol";
    case sim::CommandKind::Stop: return "RULEUCC_Stop";
    case sim::CommandKind::Guard: return "RULEUCC_Guard";
    case sim::CommandKind::Dive: return "RULEUCC_Dive";
    case sim::CommandKind::Overcharge: return "RULEUCC_Overcharge";
    case sim::CommandKind::Repair: return "RULEUCC_Repair";
    case sim::CommandKind::Reclaim: return "RULEUCC_Reclaim";
    default: return std::nullopt;
    }
}

/// Whether `def` authors a buildable counted projectile on silo `slot` — the same gates
/// `SceneBuild` applies when it makes a `SiloAmmo` record, kept here so an inspector card
/// can count eligibility without the match's records.
[[nodiscard]] bool siloSlotAuthored(const unitdef::UnitDef& def, std::uint8_t slot) {
    return std::ranges::any_of(def.weapons, [&](const unitdef::Weapon& weapon) {
        return weapon.countedProjectile && !weapon.enabledByEnhancement
               && weapon.nukeWeapon == (slot != 0)
               && weapon.projectileTraits.buildTime > sim::Mag{};
    });
}

/// Rounds the owner's `slot` still has room for: capacity minus what is stored and what
/// the queue already promises (`SiloIsFull`'s stored-plus-queued count, `C-241`).
[[nodiscard]] int siloRoom(const sim::SiloAmmo& record,
                           std::span<const sim::SiloBuild> queue) noexcept {
    const auto pending = static_cast<int>(std::count_if(
        queue.begin(), queue.end(), [&](const sim::SiloBuild& entry) {
            return entry.owner == record.owner && entry.slot == record.slot;
        }));
    return record.capacity - record.stored - pending;
}

} // namespace

CommandPage commandPage(std::span<const unitdef::UnitDef* const> selection,
                        std::span<const sim::SiloAmmo> siloAmmo,
                        std::span<const sim::SiloBuild> siloQueue) noexcept {
    const CommandAvailability available = commandAvailability(selection);
    const ToggleAvailability toggles = toggleAvailability(selection);
    // Which silo slots the selection owns: a record existing IS the silo existing, so the
    // build cells key on the records rather than re-deriving the weapon gates.
    bool tacticalWanted =
        std::ranges::any_of(siloAmmo, [](const sim::SiloAmmo& record) {
            return record.slot == 0;
        });
    bool nukeWanted =
        std::ranges::any_of(siloAmmo, [](const sim::SiloAmmo& record) {
            return record.slot == 1;
        });
    // A launcher anywhere in the selection earns the silo's launch button — the same ANY
    // semantics `commandAvailability` applies to the rack's own orders.
    const bool hasLauncher = std::ranges::any_of(selection, [](const unitdef::UnitDef* def) {
        return def != nullptr
               && std::ranges::any_of(def->weapons, &unitdef::Weapon::siloLaunched);
    });
    const auto presentation = [&](std::string_view key, std::string_view name,
                                  std::string_view icon) {
        // Views borrow from the selection's definitions (never the merged copy,
        // which dies with this call); the page must not outlive them.
        const unitdef::UnitDef::OrderOverride* stated = agreedOverride(selection, key);
        if (stated == nullptr) {
            return std::pair{name, icon};
        }
        // Retail swaps the bitmap and help text; an empty half keeps the default.
        // Help keys show raw — no LOC table resolves them out here.
        const std::string_view bitmap =
            stated->bitmapId.empty() ? icon : std::string_view{stated->bitmapId};
        const std::string_view help =
            stated->helpText.empty() ? name : std::string_view{stated->helpText};
        return std::pair{help, bitmap};
    };
    CommandPage page{};
    for (std::size_t slot = 0; slot < kCommandSlots; ++slot) {
        const CommandDescriptor& descriptor = kCommandDescriptors[slot];
        const std::optional<std::string_view> key =
            descriptor.kind ? orderKeyFor(*descriptor.kind) : std::nullopt;
        if (available[slot]) {
            const auto [name, icon] = key ? presentation(*key, descriptor.name, descriptor.icon)
                                         : std::pair{descriptor.name, descriptor.icon};
            page[slot] = {name, icon, true, std::nullopt, std::nullopt};
            continue;
        }
        // Orders first: a toggle only fills its preferred slot when the order there
        // is dead. First table entry wins a shared slot; no shipped unit authors
        // both toggles of any pair, so the order is a formality.
        std::optional<std::size_t> toggle;
        for (std::size_t i = 0; i < kToggleDescriptors.size(); ++i) {
            if (toggles[i] && kToggleDescriptors[i].slot == slot) {
                toggle = i;
                break;
            }
        }
        if (!toggle) {
            // Silo build buttons borrow dead unit-slots from Repair's onward — retail
            // puts both at preferredSlot 9 (0-based 8) and lets the second spill, which is
            // exactly what "tactical first, nuke next, dead cells only" produces. Slot 8
            // is usually the production toggle's — every silo pauses production — so the
            // borrow runs through 9 and 10 too: Repair, Assist and AUTO MEX all sit dead
            // on a silo. A builder silo with live cells simply shows fewer, same as a
            // builder silo in retail spills out of the rack.
            if ((slot >= 8 && slot <= 10) && (tacticalWanted || nukeWanted)) {
                const std::uint8_t siloSlot = tacticalWanted ? 0 : 1;
                if (tacticalWanted) {
                    tacticalWanted = false;
                } else {
                    nukeWanted = false;
                }
                const sim::CommandKind kind = siloSlot == 0
                    ? sim::CommandKind::SiloBuildTactical
                    : sim::CommandKind::SiloBuildNuke;
                // Retail greys a full silo's button — stored plus queued against capacity.
                const bool room = std::ranges::any_of(
                    siloAmmo, [&](const sim::SiloAmmo& record) {
                        return record.slot == siloSlot
                               && siloRoom(record, siloQueue) > 0;
                    });
                const auto [name, icon] = presentation(
                    siloSlot == 0 ? "RULEUCC_SiloBuildTactical" : "RULEUCC_SiloBuildNuke",
                    siloSlot == 0 ? "BUILD TACT" : "BUILD NUKE", "silo-build");
                page[slot] = {name, icon, room, std::nullopt, kind};
                continue;
            }
            // A silo's launch button borrows the last unit-specific cell the way a toggle
            // borrows a dead order's: Reclaim never lights on a launcher, and no toggle
            // claims the slot. The override keys are the retail command caps — a nuke
            // silo states Nuke, a tactical one Tactical; whichever the unit authored wins.
            if (slot == 11 && hasLauncher) {
                auto [name, icon] = presentation("RULEUCC_Nuke", "LAUNCH", "nuke");
                if (name == "LAUNCH" && icon == "nuke") {
                    const auto tactical = presentation("RULEUCC_Tactical", "LAUNCH", "nuke");
                    name = tactical.first;
                    icon = tactical.second;
                }
                page[slot] = {name, icon, true, std::nullopt, sim::CommandKind::MissileLaunch};
                continue;
            }
            page[slot] = {descriptor.name, descriptor.icon, available[slot], std::nullopt,
                          std::nullopt};
            continue;
        }
        const ToggleDescriptor& rule = kToggleDescriptors[*toggle];
        const auto [name, icon] = presentation(rule.cap, rule.label, rule.icon);
        // The production pause is the one backed toggle: `UnitStore::productionPaused`
        // and `CommandKind::ToggleProduction` implement it. The rest remain
        // present-but-disabled until a simulation state backs them.
        const bool implemented = rule.cap == "RULEUTC_ProductionToggle";
        page[slot] = {name, icon, implemented, toggle, std::nullopt};
    }
    return page;
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

InfoCard toggleCard(const ToggleDescriptor& toggle,
    std::span<const unitdef::UnitDef* const> selection,
    std::span<const std::uint8_t> paused) {
    InfoCard card;
    card.title = std::string{toggle.label} + " TOGGLE";
    const bool production = toggle.cap == "RULEUTC_ProductionToggle";
    std::size_t total = 0, eligible = 0, held = 0;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        const auto* def = selection[i];
        if (!def) continue;
        ++total;
        const bool applies = production
            ? sim::canPauseProduction(*def)
            : (def->toggleCapsDeclared && def->hasToggleCap(toggle.cap));
        if (!applies) continue;
        ++eligible;
        if (i < paused.size() && paused[i] != 0) ++held;
    }
    if (total == 0) {
        card.rows.push_back({"STATE", "SELECT A UNIT", kLoss});
        return card;
    }
    if (!production) {
        // Present but never enabled: no simulation state backs the other toggles yet,
        // so the count reads as the audience the toggle will serve once it exists.
        card.rows.push_back({"STATE", "NOT IMPLEMENTED", kLoss});
        card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
            + std::to_string(total) + " UNITS"});
        card.rows.push_back({"", "NO SIMULATION STATE YET"});
        return card;
    }
    card.rows.push_back({"STATE", held > 0 && held == eligible ? "PAUSED"
                            : held > 0 ? "PARTIALLY PAUSED" : "RUNNING",
                         held > 0 ? kLoss : kGain});
    card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
        + std::to_string(total) + " UNITS"});
    card.rows.push_back({"", "HOLDS BUILDS, SILOS, UPGRADES,"});
    card.rows.push_back({"", "REPAIRS AND INCOME"});
    return card;
}

InfoCard commandCard(const CommandDescriptor& command,
    std::span<const unitdef::UnitDef* const> selection, bool armed) {
    InfoCard card;
    card.title = command.name.empty() ? "UNIT ACTION" : std::string{command.name};
    if (command.action == RackAction::AutoExpand) {
        // A standing order, not a targeted one: it is on or off for the selection.
        std::size_t total = 0, eligible = 0;
        for (const auto* def : selection) {
            if (!def) continue;
            ++total;
            if (def->isBuilder() && def->isMobile()) ++eligible;
        }
        if (total == 0) {
            card.rows.push_back({"STATE", "SELECT A UNIT", kLoss});
            return card;
        }
        if (eligible == 0) {
            card.rows.push_back({"STATE", "SELECTION CANNOT DO THIS", kLoss});
            card.rows.push_back({"", "SELECT A FIELD ENGINEER"});
            return card;
        }
        card.rows.push_back({"STATE", armed ? "ON" : "OFF", kGain});
        card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
            + std::to_string(total) + " UNITS"});
        card.rows.push_back({"TARGET", "NEAREST FREE MASS OR HYDRO SPOT"});
        card.rows.push_back({"", "STAYS ON YOUR SIDE OF THE MAP"});
        return card;
    }
    if (!command.kind) {
        card.rows.push_back({"STATE", "NOT IMPLEMENTED", kLoss});
        card.rows.push_back({"", "NO UNIT CAN USE THIS YET"});
        return card;
    }
    const auto descriptor = std::ranges::find(kCommandDescriptors, command.kind,
        &CommandDescriptor::kind);
    const auto slot = static_cast<std::size_t>(descriptor - kCommandDescriptors.begin());
    std::size_t total = 0, eligible = 0;
    for (const auto* def : selection) {
        if (!def) continue;
        ++total;
        const std::array single{def};
        if (slot < kCommandSlots && commandAvailability(single)[slot]) ++eligible;
    }
    if (total == 0) {
        card.rows.push_back({"STATE", "SELECT A UNIT", kLoss});
        return card;
    }
    if (eligible == 0) {
        card.rows.push_back({"STATE", "SELECTION CANNOT DO THIS", kLoss});
        card.rows.push_back({"", "SELECT A UNIT WITH THIS COMMAND"});
        return card;
    }
    card.rows.push_back({"STATE", armed ? "TARGETING" : "READY", kGain});
    card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
        + std::to_string(total) + " UNITS"});
    const auto target = [&]() -> std::string_view {
        switch (*command.kind) {
        case sim::CommandKind::Dive:
        case sim::CommandKind::Stop: return "NO TARGET NEEDED";
        case sim::CommandKind::Assist: return "ALLIED BUILDER";
        case sim::CommandKind::Guard: return "ALLIED UNIT";
        case sim::CommandKind::Repair: return "DAMAGED ALLY";
        case sim::CommandKind::Reclaim: return "WRECK";
        case sim::CommandKind::Attack:
        case sim::CommandKind::Overcharge:
        case sim::CommandKind::Capture: return "ENEMY UNIT";
        default: return "GROUND POSITION";
        }
    }();
    card.rows.push_back({"TARGET", std::string{target}});
    return card;
}

InfoCard commandInspector(const CommandPage& page, std::size_t slot,
    std::span<const unitdef::UnitDef* const> selection, bool armed,
    std::span<const std::uint8_t> paused) {
    const std::optional<std::size_t> toggle = page[slot].toggle;
    if (toggle) {
        return toggleCard(kToggleDescriptors[*toggle], selection, paused);
    }
    if (page[slot].order == sim::CommandKind::MissileLaunch) {
        // A substituted cell inspects as the order it issues — the table under it
        // (Reclaim) cannot answer for a silo, and `commandCard`'s eligibility count
        // keys on descriptor slots MissileLaunch has no row in.
        InfoCard card;
        card.title = std::string{page[slot].name};
        std::size_t total = 0, eligible = 0;
        for (const auto* def : selection) {
            if (!def) continue;
            ++total;
            if (std::ranges::any_of(def->weapons, [](const unitdef::Weapon& weapon) {
                    return weapon.siloLaunched();
                })) ++eligible;
        }
        if (total == 0) {
            card.rows.push_back({"STATE", "SELECT A UNIT", kLoss});
            return card;
        }
        if (eligible == 0) {
            card.rows.push_back({"STATE", "SELECTION CANNOT DO THIS", kLoss});
            card.rows.push_back({"", "SELECT A MISSILE SILO"});
            return card;
        }
        card.rows.push_back({"STATE", armed ? "TARGETING" : "READY", kGain});
        card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
            + std::to_string(total) + " UNITS"});
        card.rows.push_back({"TARGET", "UNIT OR GROUND"});
        return card;
    }
    if (page[slot].order == sim::CommandKind::SiloBuildTactical
        || page[slot].order == sim::CommandKind::SiloBuildNuke) {
        // Same substituted-cell story as LAUNCH: the descriptor under the cell cannot
        // answer for a silo. Eligibility keys on the authored weapon slot so the card
        // works without the match's records.
        const std::uint8_t siloSlot =
            page[slot].order == sim::CommandKind::SiloBuildNuke ? 1 : 0;
        InfoCard card;
        card.title = std::string{page[slot].name};
        std::size_t total = 0, eligible = 0;
        for (const auto* def : selection) {
            if (!def) continue;
            ++total;
            if (siloSlotAuthored(*def, siloSlot)) ++eligible;
        }
        if (total == 0) {
            card.rows.push_back({"STATE", "SELECT A UNIT", kLoss});
            return card;
        }
        if (eligible == 0) {
            card.rows.push_back({"STATE", "SELECTION CANNOT DO THIS", kLoss});
            card.rows.push_back({"", "SELECT A MISSILE SILO"});
            return card;
        }
        card.rows.push_back({"STATE", page[slot].enabled ? "READY" : "FULL",
                             page[slot].enabled ? kGain : kWarn});
        card.rows.push_back({"APPLIES TO", std::to_string(eligible) + " OF "
            + std::to_string(total) + " UNITS"});
        card.rows.push_back({"CLICK", "QUEUES ONE MISSILE"});
        card.rows.push_back({"RIGHT-CLICK", "TOGGLES AUTO-BUILD"});
        return card;
    }
    return commandCard(kCommandDescriptors[slot], selection, armed);
}

void appendCommandRack(Geometry& out, const text::Font& labelFont,
                       const text::Font& readoutFont, const Theme& theme,
                       const CommandRackLayout& layout,
                       const CommandPage& page,
                       std::optional<std::size_t> hovered,
                       std::optional<sim::CommandKind> armed,
                       const CommandAvailability& engaged) {
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
        const CommandPageCell& cell = page[slot];
        const bool enabled = cell.enabled;
        // A toggle borrows a dead order's cell: it must not light as the order
        // the descriptor underneath names, whatever is armed. A substituted cell
        // (the silo's LAUNCH) lights as its own kind instead of the descriptor's.
        const std::optional<sim::CommandKind> cellKind =
            cell.order ? cell.order : command.kind;
        const bool active =
            (!cell.toggle && cellKind && armed == cellKind) || engaged[slot];
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

        const std::string label = cell.name.empty() ? "--" : std::string{cell.name};
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
