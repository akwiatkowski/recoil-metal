// COMMAND AUTHORITY — what the sim accepts, applied at the mutation boundary.
//
// `applyCommand` is THE SINGLE PATH: a human's click and a script's decision both arrive here,
// which is what makes "they replay through the same path" true rather than aspirational. The
// work inside is one chain — canonicalize the set, validate per kind, then mutate per member —
// and a refusal is ordinary and deterministic, not an error: a replay rejects exactly what
// the original did.
//
// The validity predicates it asks twice (an unqueued order is checked now and again when the
// queue would start it) live in `Command.cpp`'s vocabulary and are declared in
// `CommandInternal.hpp`; the apply-only ones stay file-local below.
#include "core/sim/Command.hpp"
#include "core/sim/CommandInternal.hpp"

#include "core/sim/Combat.hpp"
#include "core/sim/Enhancement.hpp"
#include "core/sim/Capture.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Reclaim.hpp"
#include "core/sim/ScriptTask.hpp"
#include "core/sim/Transport.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <memory>
#include <optional>

namespace rm::sim {
namespace {

/// The player who issued a command, or null.
[[nodiscard]] const Player* playerFor(PlayerIndex index,
                                      std::span<const Player> players) noexcept {
    for (const Player& player : players) {
        if (player.index == index) {
            return &player;
        }
    }
    return nullptr;
}
/// Whether this player may order this unit.
///
/// TWO conditions, and both matter. The player must command the unit's army — that is the
/// authorisation §7 P2.5 makes possible. And the army must not be defeated: a side that is out
/// of the match does not keep taking orders, which is the same rule `hostile` applies to
/// targeting.
[[nodiscard]] bool authorised(const Player& player, const UnitStore& store, UnitId unit,
                             std::span<const Army> armies) noexcept {
    const int owner = store.motion()[unit.index].armyIndex;
    if (!commands(player, owner)) {
        return false;
    }
    const auto army = static_cast<std::size_t>(owner);
    return army >= armies.size() || !armies[army].defeated;
}
/// Whether `builder` is running an unfinished FACTORY construction. Production queued "on a
/// factory still under construction" parks on the founder's queue (the factory unit does not
/// exist until the work completes), and this is the state that makes such an order legal —
/// retail lets a player queue units on a rising factory and have them start the moment it
/// comes online.
[[nodiscard]] bool constructingFactory(std::span<const Construction> building, UnitId builder,
                                       const UnitCatalog& catalog) noexcept {
    return std::ranges::any_of(building, [&](const Construction& work) {
        if (work.finished() || work.builder != builder) {
            return false;
        }
        const unitdef::UnitDef* def =
            catalog.def(static_cast<UnitTypeIndex>(work.blueprintIndex));
        return def != nullptr && def->hasCategory("FACTORY") && !def->isMobile();
    });
}
void cancelActiveConstruction(std::vector<Construction>* building, UnitId builder) {
    if (building == nullptr) {
        return;
    }
    std::erase_if(*building, [builder](const Construction& work) {
        return !work.finished() && work.builder == builder;
    });
}
/// What an interrupt does to the interrupted order's work — and it is per ROW KIND, because
/// the two kinds of construction are different things. Pad-bound work (a factory's product,
/// an upgrade) is the task's own: the order dies and takes it, exactly as it always did. A
/// scaffold standing on the map is world state instead — the player's half-built factory —
/// so interrupting only takes the builder OFF it. Marking it `paused` is the bookkeeping
/// that stops its bill; the next `applyAssistance` sweep keeps the flag honest and a Build
/// order back onto the site adopts it. The row the incoming command re-founds — same site
/// and blueprint — is skipped: that order is a resume, not an abandonment.
void releaseInterruptedConstruction(std::vector<Construction>* building,
                                    const Command& incoming, const UnitStore& store,
                                    const UnitCatalog& catalog) {
    if (building == nullptr) {
        return;
    }
    const UnitId me = incoming.unit;
    // The row the incoming order founded (or is resuming) is not interrupted work — on a
    // pad the two share one position, so identity is site AND blueprint, not place alone.
    const std::optional<std::pair<Fx, Fx>> site =
        incoming.kind == CommandKind::Build
            ? buildSiteFor(incoming.buildType, incoming.targetX, incoming.targetZ, me.index,
                           store, catalog)
            : std::nullopt;
    const auto isIncomingOwn = [&](const Construction& work) {
        return site.has_value() && work.blueprintIndex == incoming.buildType
            && work.position[0] == site->first && work.position[2] == site->second;
    };
    // Pad-bound rows — upgrades and mobile products — anchor to the builder, not the
    // world: the order that stops being current leaves nothing resumable behind. A placed
    // structure's scaffold does anchor to the world even where it happens to coincide
    // with the pad, so position alone cannot stand in for the anchor test.
    const auto padBound = [&](const Construction& work) {
        if (work.isUpgrade()) {
            return true;
        }
        const unitdef::UnitDef* product =
            catalog.def(static_cast<UnitTypeIndex>(work.blueprintIndex));
        return product != nullptr && product->isMobile();
    };
    std::erase_if(*building, [&](const Construction& work) {
        return !work.finished() && work.builder == me && !isIncomingOwn(work)
            && padBound(work);
    });
    for (Construction& work : *building) {
        if (work.finished() || !(work.builder == me) || isIncomingOwn(work)) {
            continue;
        }
        work.paused = true;
    }
}
/// The row belonging to `command` itself — what the re-issue cancel gesture removes. A
/// Build's own unfinished work on the site it names; every other kind owns no row, and an
/// abandoned scaffold elsewhere on the map is not this gesture's business.
void cancelConstructionFor(std::vector<Construction>* building, const Command& command,
                           const UnitStore& store, const UnitCatalog& catalog) {
    if (building == nullptr || command.kind != CommandKind::Build) {
        return;
    }
    const auto site = buildSiteFor(command.buildType, command.targetX, command.targetZ,
                                   command.unit.index, store, catalog);
    if (!site) {
        return;
    }
    std::erase_if(*building, [&](const Construction& work) {
        return !work.finished() && work.builder == command.unit
            && work.blueprintIndex == command.buildType
            && work.position[0] == site->first && work.position[2] == site->second;
    });
}
[[nodiscard]] bool validRepair(const Command& command, const UnitStore& store,
                               const UnitCatalog& catalog, std::span<const Army> armies) noexcept {
    if (!store.alive(command.target) || !store.health()[command.target.index].alive()
        || command.target == command.unit) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
    if (builder == nullptr || target == nullptr || !builder->isBuilder()
        || !needsRepair(store.health()[command.target.index])
        || target->buildTime <= Mag{}) {
        return false;
    }
    const int owner = store.motion()[command.unit.index].armyIndex;
    const int targetOwner = store.motion()[command.target.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [targetOwner](const Army& army) {
        return army.index == targetOwner;
    });
    if (mine == armies.end() || theirs == armies.end() || !allied(*mine, *theirs)) {
        return false;
    }
    return true;
}
/// Issue-time gate for Capture: a live captor with the authored CAPTURE category and
/// a live hostile completed target the admission predicate accepts. Own and allied
/// units are refused, like unit reclaim; commanders, aircraft, the submerged and
/// the attached never enter the queue at all.
[[nodiscard]] bool validCapture(const Command& command, const UnitStore& store,
                                const UnitCatalog& catalog,
                                std::span<const Army> armies) noexcept {
    if (!store.alive(command.unit) || !store.health()[command.unit.index].alive()
        || !capturableTarget(command.unit.index, command.target, store, catalog, armies)
        || effectiveBuildPerTick(store, catalog, command.unit.index) <= Mag{}) {
        return false;
    }
    return true;
}
/// Issue-time gate for ReclaimUnit: a builder, a living target that is not
/// itself, and a target with a build cost to give back. `Unit.lua:2699-2712`
/// (`GetReclaimCosts`, unit branch) carries no alliance gate, so own and
/// allied units are admitted — the C-157 exemption presumes exactly this task.
[[nodiscard]] bool validReclaimUnit(const Command& command, const UnitStore& store,
                                    const UnitCatalog& catalog,
                                    std::span<const Army> armies) noexcept {
    if (!store.alive(command.target) || !store.health()[command.target.index].alive()
        || command.target == command.unit) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* target = catalog.def(store.typeAt(command.target.index));
    if (builder == nullptr || target == nullptr || !builder->isBuilder()
        || std::max(target->buildCostMass, target->buildCostEnergy) <= Mag{}) {
        return false;
    }
    if (armies.empty()) {
        return true;  // the direct-dispatch compatibility seam has no alliance state to judge
    }
    const int owner = store.motion()[command.unit.index].armyIndex;
    const int targetOwner = store.motion()[command.target.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [targetOwner](const Army& army) {
        return army.index == targetOwner;
    });
    return mine != armies.end() && theirs != armies.end();
}
/// Issue-time gate for Sacrifice (`C-192`, `0x00601c50`): a sacrificer whose
/// blueprint carries a nonzero `SacrificeMassMult`/`SacrificeEnergyMult`, and
/// an ALLIED work in progress to feed — a scaffold at the clicked site, or a
/// unit being upgraded or enhanced. Reach is deliberately NOT checked: like
/// repair and capture, the order walks there first.
[[nodiscard]] bool validSacrifice(const Command& command, const UnitStore& store,
                                  const UnitCatalog& catalog,
                                  std::span<const Army> armies,
                                  std::span<const Construction> building,
                                  std::span<const EnhancementWork> enhancements) noexcept {
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    if (builder == nullptr
        || (builder->sacrificeMassMult <= 0.0f && builder->sacrificeEnergyMult <= 0.0f)) {
        return false;
    }
    const std::optional<SacrificeWork> work =
        sacrificeWork(command, store, building, enhancements);
    if (!work.has_value()) {
        return false;
    }
    if (armies.empty()) {
        return true;  // the direct-dispatch compatibility seam has no alliance state to judge
    }
    const int owner = store.motion()[command.unit.index].armyIndex;
    const auto mine = std::ranges::find_if(armies, [owner](const Army& army) {
        return army.index == owner;
    });
    const auto theirs = std::ranges::find_if(armies, [work](const Army& army) {
        return army.index == work->armyIndex;
    });
    return mine != armies.end() && theirs != armies.end() && allied(*mine, *theirs);
}
/// Issue-time gate for MissileLaunch: a unit whose counted manual weapon is live FOR THIS
/// UNIT (`weaponSiloLaunchedFor`, `C-255`) — the silo's round — and, when a unit is named,
/// one that is alive enough to aim at. Ammunition is deliberately NOT checked: the
/// stockpile may fill while the order sits, so an empty silo holds the order rather than
/// refusing the click (`fireMissiles` gates the trigger). An `EnabledByEnhancement` silo
/// weapon before its install (the ACU's `TacticalNukeMissile`) refuses the click the way
/// `fireMissiles` refuses the trigger — overcharge's intake already asks its own per-unit
/// question (`weaponManuallyFiredFor`, `CommandAdvance.cpp`).
[[nodiscard]] bool validMissileLaunch(const Command& command, const UnitStore& store,
                                      const UnitCatalog& catalog) noexcept {
    if (command.target.generation != 0 && !store.alive(command.target)) {
        return false;
    }
    const unitdef::UnitDef* def = catalog.def(store.typeAt(command.unit.index));
    return def != nullptr
           && std::ranges::any_of(def->weapons, [&](const unitdef::Weapon& weapon) {
                  return weaponSiloLaunchedFor(store, catalog, command.unit.index, weapon);
              });
}
/// Issue-time gate for LoadTransport: a live transportable unit boarding a live
/// same-army carrier whose attach table could ever take its class. A FULL
/// carrier is still a legal order — the cargo waits for a slot — but a class
/// the hull cannot hold is refused at the door rather than parked forever.
[[nodiscard]] bool validLoadTransport(const Command& command, const UnitStore& store,
                                      const UnitCatalog& catalog) noexcept {
    if (!store.alive(command.target) || command.target == command.unit
        || store.motion()[command.unit.index].attached
        || store.motion()[command.target.index].attached) {
        return false;
    }
    if (store.motion()[command.unit.index].armyIndex
        != store.motion()[command.target.index].armyIndex) {
        return false;  // cargo rides its own army's decks only
    }
    const unitdef::UnitDef* cargo = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* carrier = catalog.def(store.typeAt(command.target.index));
    return cargo != nullptr && carrier != nullptr && cargo->transportable()
           && carrier->isTransport() && canEverCarry(*carrier, *cargo);
}
/// Issue-time gate for the transport's own orders (UnloadTransport, Ferry):
/// the unit on the order is a live carrier. `isCarrier()` counts too —
/// retail's carriers (UES0401, UAA0310, URS0303) declare `RULEUCC_Transport`
/// and `TransportClass` but NOT the `TRANSPORTATION` category, so
/// `isTransport()` alone would refuse the launch order that empties a
/// carrier's storage pool (`C-225`'s deploy half, `C-264`'s release).
[[nodiscard]] bool validTransportCarrier(const Command& command, const UnitStore& store,
                                         const UnitCatalog& catalog) noexcept {
    const unitdef::UnitDef* def = catalog.def(store.typeAt(command.unit.index));
    return def != nullptr && (def->isTransport() || def->isCarrier())
           && !store.motion()[command.unit.index].attached;
}
/// ART-S007 `GrowthFormation` selects these repeating land-block widths by total unit count.
/// Members fill front rows first in category order (see the intake below); the widths are
/// retail's ThreeWide through EightWide block thresholds.
[[nodiscard]] std::size_t growthFormationWidth(std::size_t units) noexcept {
    if (units <= 3) {
        return 3;
    }
    if (units <= 12) {
        return 4;
    }
    if (units <= 20) {
        return 5;
    }
    if (units <= 30) {
        return 6;
    }
    if (units <= 42) {
        return 7;
    }
    return 8;
}

/// ART-S007 `AttackFormation` selects these repeating land-block widths by total unit count
/// (formations.lua:623-640: `TwoRow` through `NineRow` blocks, 8 to 20 columns). Like
/// `growthFormationWidth` above, members fill front rows first in category order; the block
/// rows carry per-slot category ranks (formations.lua:192-310), which stay deferred.
[[nodiscard]] std::size_t attackFormationWidth(std::size_t units) noexcept {
    if (units <= 16) {
        return 8;
    }
    if (units <= 30) {
        return 10;
    }
    if (units <= 48) {
        return 12;
    }
    if (units <= 70) {
        return 14;
    }
    if (units <= 96) {
        return 16;
    }
    if (units <= 126) {
        return 18;
    }
    return 20;
}

/// ART-S007 `AttackFormation`'s land half fills per-slot category ranks, not one
/// filler per family (formations.lua:192-310). Row 0 is all direct fire; the
/// two-row block's second row reads Util, AA, Shield, Art, Art, Shield, AA,
/// Util (formations.lua:192-197). Wider blocks follow their own rows: row 1
/// mixes shields into direct fire, row 2 mixes AA, and deeper rows alternate
/// shield/AA rhythms until the artillery back rows. `BlockBuilderLand` visits
/// columns centre-outward (`GetColSpot`, formations.lua:915-930), so physical
/// column 1 takes the row's leftmost rank.
[[nodiscard]] int attackLandSlotRank(std::size_t row, std::size_t column, std::size_t width) noexcept {
    // Family IDs match formationClass below. Retail's complete fallback lists
    // (formations.lua:91-97) matter: a slot accepts the next family when its
    // preferred family is absent, rather than reserving holes in the row.
    constexpr int kDirect = 1;
    constexpr int kArtillery = 2;
    constexpr int kAntiAir = 3;
    constexpr int kShield = 4;
    constexpr int kUtility = 6;
    // The two-row block is row-special: its second row is the Util/AA/Shield
    // pair rhythm, not the wider blocks' shield-mix second row.
    constexpr int kTwoRowSecond[] = {kUtility, kAntiAir, kShield, kArtillery,
                                     kArtillery, kShield, kAntiAir, kUtility};
    if (width == 8 && row == 1) {
        return column >= 1 && column <= std::size(kTwoRowSecond)
                   ? kTwoRowSecond[column - 1]
                   : kDirect;
    }
    // Wider-block rows, physical columns 1-based left to right. Row 0 is all
    // direct fire in every block; the tables below transcribe rows 1+ from
    // formations.lua:200-310 (columns past the row's authored width read as
    // direct fire, matching the intake's unranked fallback).
    constexpr int kWideRow1[] = {kUtility, kShield, kDirect, kDirect, kShield, kDirect,
                                 kShield, kDirect, kDirect, kShield, kDirect, kDirect,
                                 kShield, kDirect, kDirect, kShield, kDirect, kDirect,
                                 kShield, kUtility};
    constexpr int kWideRow2[] = {kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                 kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                 kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                 kDirect, kAntiAir};
    constexpr int kWideRowShield[] = {kAntiAir, kShield, kDirect, kAntiAir, kShield, kDirect,
                                      kAntiAir, kShield, kDirect, kShield, kAntiAir, kDirect,
                                      kShield, kAntiAir, kDirect, kShield, kAntiAir, kDirect,
                                      kShield, kAntiAir};
    constexpr int kWideRowMixed[] = {kUtility, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                     kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                     kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect,
                                     kAntiAir, kUtility};
    constexpr int kWideRowArt[] = {kArtillery, kAntiAir, kArtillery, kArtillery, kAntiAir,
                                   kArtillery, kArtillery, kAntiAir, kArtillery, kArtillery,
                                   kAntiAir, kArtillery, kArtillery, kAntiAir, kArtillery,
                                   kArtillery, kAntiAir, kArtillery, kArtillery, kArtillery};
    if (row == 0 || column < 1 || column > 20) {
        return kDirect;
    }
    const int* table = kWideRow1;
    if (row == 1) {
        table = kWideRow1;
    } else if (row == 2) {
        table = kWideRow2;
    } else if (row >= 7) {
        table = kWideRowArt;
    } else if (row % 2 == 1) {
        table = kWideRowMixed;
    } else {
        table = kWideRowShield;
    }
    return table[column - 1];
}

/// Family-level projection of retail slot tables (formations.lua:91-97).
/// IDs are formationClass's experimental/direct/artillery/AA/shield/build/
/// utility/remaining families. Within-family tech order stays stable.
[[nodiscard]] std::size_t attackLandFamilyPriority(int slotType, int family) noexcept {
    constexpr int kDirect[] = {0, 1, 2, 3, 4, 5, 6, 7};
    constexpr int kArtillery[] = {2, 3, 1, 4, 5, 6, 7, 0};
    constexpr int kAntiAir[] = {3, 1, 2, 4, 5, 6, 7, 0};
    constexpr int kShield[] = {4, 3, 2, 1, 5, 6, 7, 0};
    constexpr int kUtility[] = {6, 3, 2, 1, 4, 5, 7, 0};
    const auto& priorities = slotType == 2 ? kArtillery
                             : slotType == 3 ? kAntiAir
                             : slotType == 4 ? kShield
                             : slotType == 6 ? kUtility : kDirect;
    return static_cast<std::size_t>(
        std::ranges::find(priorities, family) - std::begin(priorities));
}

/// ART-S007 `GrowthFormation`'s land half reuses the small attack blocks as
/// travel blocks by count (formations.lua:681-695): row 0 is all direct fire;
/// deeper rows name their own Shield/Util/AA/Art rhythms per width below.
/// Family IDs match formationClass, as for the attack helper above. T1Art slots
/// project to the artillery family, matching the attack tables' projection.
[[nodiscard]] int growthLandSlotRank(std::size_t row, std::size_t column, std::size_t width) noexcept {
    // The 3-wide block is one all-direct row; ranks begin at width 4.
    constexpr int kDirect = 1;
    constexpr int kArtillery = 2;
    constexpr int kAntiAir = 3;
    constexpr int kShield = 4;
    constexpr int kUtility = 6;
    constexpr int kFourRow1[] = {kUtility, kShield, kShield, kUtility};
    constexpr int kFourRow2[] = {kAntiAir, kArtillery, kArtillery, kAntiAir};
    constexpr int kFiveRow1[] = {kDirect, kShield, kDirect, kShield, kDirect};
    constexpr int kFiveRow2[] = {kUtility, kShield, kDirect, kShield, kUtility};
    constexpr int kFiveRow3[] = {kAntiAir, kDirect, kArtillery, kDirect, kAntiAir};
    // Six-wide travel block (formations.lua:131-142): row 1 DF/Shield rhythm,
    // row 2 Util/AA flanks, row 3 AA/Shield/Art, row 4 DF/AA/Art.
    constexpr int kSixRow1[] = {kDirect, kShield, kDirect, kDirect, kShield, kDirect};
    constexpr int kSixRow2[] = {kUtility, kAntiAir, kDirect, kDirect, kAntiAir, kUtility};
    constexpr int kSixRow3[] = {kAntiAir, kShield, kArtillery, kArtillery, kShield, kAntiAir};
    constexpr int kSixRow4[] = {kDirect, kAntiAir, kArtillery, kArtillery, kAntiAir, kDirect};
    // Seven-wide travel block (formations.lua:145-158).
    constexpr int kSevenRow1[] = {kDirect, kShield, kDirect, kDirect, kDirect, kShield, kDirect};
    constexpr int kSevenRow2[] = {kUtility, kAntiAir, kDirect, kDirect, kAntiAir, kDirect, kUtility};
    constexpr int kSevenRow3[] = {kAntiAir, kShield, kAntiAir, kArtillery, kShield, kAntiAir, kDirect};
    constexpr int kSevenRow4[] = {kDirect, kAntiAir, kArtillery, kArtillery, kAntiAir, kArtillery, kDirect};
    constexpr int kSevenRow5[] = {kUtility, kArtillery, kArtillery, kAntiAir, kArtillery, kArtillery, kUtility};
    // Eight-wide travel block (formations.lua:161-176).
    constexpr int kEightRow1[] = {kDirect, kDirect, kShield, kDirect, kDirect, kShield, kDirect, kDirect};
    constexpr int kEightRow2[] = {kUtility, kDirect, kAntiAir, kDirect, kDirect, kAntiAir, kDirect, kUtility};
    constexpr int kEightRow3[] = {kDirect, kAntiAir, kShield, kDirect, kDirect, kShield, kAntiAir, kDirect};
    constexpr int kEightRow4[] = {kDirect, kArtillery, kAntiAir, kArtillery, kArtillery, kAntiAir, kArtillery, kDirect};
    constexpr int kEightRow5[] = {kUtility, kAntiAir, kShield, kArtillery, kArtillery, kShield, kAntiAir, kUtility};
    constexpr int kEightRow6[] = {kDirect, kArtillery, kAntiAir, kArtillery, kArtillery, kAntiAir, kArtillery, kDirect};
    if (column < 1) {
        return kDirect;
    }
    const std::size_t col = column - 1;
    if (row == 0) {
        return kDirect;
    }
    if (width == 4) {
        if (row == 1) {
            return col < std::size(kFourRow1) ? kFourRow1[col] : kDirect;
        }
        return col < std::size(kFourRow2) ? kFourRow2[col] : kDirect;
    }
    if (width == 5) {
        if (row == 1) {
            return col < std::size(kFiveRow1) ? kFiveRow1[col] : kDirect;
        }
        if (row == 2) {
            return col < std::size(kFiveRow2) ? kFiveRow2[col] : kDirect;
        }
        return col < std::size(kFiveRow3) ? kFiveRow3[col] : kDirect;
    }
    if (width == 6) {
        if (row == 1) {
            return col < std::size(kSixRow1) ? kSixRow1[col] : kDirect;
        }
        if (row == 2) {
            return col < std::size(kSixRow2) ? kSixRow2[col] : kDirect;
        }
        if (row == 3) {
            return col < std::size(kSixRow3) ? kSixRow3[col] : kDirect;
        }
        return col < std::size(kSixRow4) ? kSixRow4[col] : kDirect;
    }
    if (width == 7) {
        if (row == 1) {
            return col < std::size(kSevenRow1) ? kSevenRow1[col] : kDirect;
        }
        if (row == 2) {
            return col < std::size(kSevenRow2) ? kSevenRow2[col] : kDirect;
        }
        if (row == 3) {
            return col < std::size(kSevenRow3) ? kSevenRow3[col] : kDirect;
        }
        if (row == 4) {
            return col < std::size(kSevenRow4) ? kSevenRow4[col] : kDirect;
        }
        return col < std::size(kSevenRow5) ? kSevenRow5[col] : kDirect;
    }
    if (width == 8) {
        if (row == 1) {
            return col < std::size(kEightRow1) ? kEightRow1[col] : kDirect;
        }
        if (row == 2) {
            return col < std::size(kEightRow2) ? kEightRow2[col] : kDirect;
        }
        if (row == 3) {
            return col < std::size(kEightRow3) ? kEightRow3[col] : kDirect;
        }
        if (row == 4) {
            return col < std::size(kEightRow4) ? kEightRow4[col] : kDirect;
        }
        if (row == 5) {
            return col < std::size(kEightRow5) ? kEightRow5[col] : kDirect;
        }
        return col < std::size(kEightRow6) ? kEightRow6[col] : kDirect;
    }
    return kDirect;
}

/// ART-S007 naval attack slot ranks (formations.lua:499-528). Each slot names a
/// `*First` fallback list (formations.lua:439-445); the helpers below project
/// each list to its first-pick family. IDs are formationSeaClass's
/// frigate/destroyer/battleship/cruiser/carrier/sonar/nukesub/remaining families.
/// Sub blocks are uniform `Subs` (formations.lua:562-584) and need no ranking.
/// ART-S007 naval Growth slot ranks (formations.lua:450-487), projected to
/// first-pick families like the attack helper below. Row 0 is all
/// `FrigatesFirst` in every block; deeper rows name their own rhythms.
/// Columns past the authored row read frigate.
[[nodiscard]] int growthNavalSlotRank(std::size_t row, std::size_t column, std::size_t width) noexcept {
    constexpr int kFrigate = 0;
    constexpr int kDestroyer = 1;
    constexpr int kBattleship = 2;
    constexpr int kCruiser = 3;
    constexpr int kCarrier = 4;
    constexpr int kSonar = 5;
    constexpr int kThreeRow1[] = {kDestroyer, kSonar, kDestroyer};
    constexpr int kThreeRow2[] = {kDestroyer, kCruiser, kDestroyer};
    constexpr int kFiveRow1[] = {kFrigate, kSonar, kDestroyer, kSonar, kFrigate};
    constexpr int kFiveRow2[] = {kDestroyer, kSonar, kBattleship, kSonar, kDestroyer};
    constexpr int kFiveRow3[] = {kDestroyer, kSonar, kCarrier, kSonar, kDestroyer};
    constexpr int kSevenRow1[] = {kFrigate, kFrigate, kSonar, kDestroyer, kSonar, kFrigate, kFrigate};
    constexpr int kSevenRow2[] = {kDestroyer, kDestroyer, kSonar, kBattleship, kSonar, kDestroyer, kDestroyer};
    constexpr int kSevenRow3[] = {kDestroyer, kBattleship, kSonar, kCarrier, kSonar, kBattleship, kDestroyer};
    constexpr int kSevenRow4[] = {kDestroyer, kCruiser, kSonar, kBattleship, kSonar, kCruiser, kDestroyer};
    constexpr int kSevenRow5[] = {kDestroyer, kCruiser, kSonar, kCarrier, kSonar, kCruiser, kDestroyer};
    if (column < 1) {
        return kFrigate;
    }
    const std::size_t col = column - 1;
    if (row == 0) {
        return kFrigate;
    }
    if (width == 3) {
        if (row == 1) {
            return col < std::size(kThreeRow1) ? kThreeRow1[col] : kFrigate;
        }
        return col < std::size(kThreeRow2) ? kThreeRow2[col] : kFrigate;
    }
    if (width == 5) {
        if (row == 1) {
            return col < std::size(kFiveRow1) ? kFiveRow1[col] : kFrigate;
        }
        if (row == 2) {
            return col < std::size(kFiveRow2) ? kFiveRow2[col] : kFrigate;
        }
        return col < std::size(kFiveRow3) ? kFiveRow3[col] : kFrigate;
    }
    if (width == 7) {
        if (row == 1) {
            return col < std::size(kSevenRow1) ? kSevenRow1[col] : kFrigate;
        }
        if (row == 2) {
            return col < std::size(kSevenRow2) ? kSevenRow2[col] : kFrigate;
        }
        if (row == 3) {
            return col < std::size(kSevenRow3) ? kSevenRow3[col] : kFrigate;
        }
        if (row == 4) {
            return col < std::size(kSevenRow4) ? kSevenRow4[col] : kFrigate;
        }
        return col < std::size(kSevenRow5) ? kSevenRow5[col] : kFrigate;
    }
    return kFrigate;
}

/// ART-S007 naval attack slot ranks (formations.lua:499-528). Each slot names a
/// `*First` fallback list (formations.lua:439-445); the helpers below project
/// each list to its first-pick family. IDs are formationSeaClass's
/// frigate/destroyer/battleship/cruiser/carrier/sonar/nukesub/remaining families.
/// Sub blocks are uniform `Subs` (formations.lua:562-584) and need no ranking.
[[nodiscard]] int attackNavalSlotRank(std::size_t row, std::size_t column, std::size_t width) noexcept {
    constexpr int kFrigate = 0;
    constexpr int kDestroyer = 1;
    constexpr int kBattleship = 2;
    constexpr int kCruiser = 3;
    constexpr int kCarrier = 4;
    constexpr int kSonar = 5;
    // Five-wide block rows (formations.lua:500-506). Row 0 is all frigates;
    // row 1 reads Destroyer, Sonar, Carrier, Sonar, Destroyer.
    constexpr int kFiveRow1[] = {kDestroyer, kSonar, kCarrier, kSonar, kDestroyer};
    // Seven-wide block rows (formations.lua:509-517).
    constexpr int kSevenRow1[] = {kDestroyer, kBattleship, kSonar, kBattleship,
                                  kSonar, kBattleship, kDestroyer};
    constexpr int kSevenRow2[] = {kDestroyer, kCruiser, kCarrier, kCarrier,
                                  kCruiser, kCruiser, kDestroyer};
    // Nine-wide block rows (formations.lua:520-528).
    constexpr int kNineRow1[] = {kDestroyer, kDestroyer, kSonar, kBattleship,
                                 kBattleship, kBattleship, kSonar, kDestroyer, kDestroyer};
    constexpr int kNineRow2[] = {kDestroyer, kCruiser, kSonar, kCarrier,
                                 kCarrier, kCarrier, kSonar, kCruiser, kDestroyer};
    if (column < 1) {
        return kFrigate;
    }
    const std::size_t col = column - 1;
    if (width == 5) {
        return row == 0 ? kFrigate
             : col < std::size(kFiveRow1) ? kFiveRow1[col] : kFrigate;
    }
    if (width == 7) {
        if (row == 0) {
            return kFrigate;
        }
        if (row == 1) {
            return col < std::size(kSevenRow1) ? kSevenRow1[col] : kFrigate;
        }
        return col < std::size(kSevenRow2) ? kSevenRow2[col] : kFrigate;
    }
    if (width == 9) {
        if (row == 0) {
            return kFrigate;
        }
        if (row == 1) {
            return col < std::size(kNineRow1) ? kNineRow1[col] : kFrigate;
        }
        return col < std::size(kNineRow2) ? kNineRow2[col] : kFrigate;
    }
    return kFrigate;
}
[[nodiscard]] std::size_t attackNavalFamilyPriority(int slotType, int family) noexcept {
    constexpr int kFrigatesFirst[] = {0, 1, 2, 3, 4, 6, 5, 7};
    constexpr int kDestroyersFirst[] = {1, 0, 2, 3, 4, 6, 5, 7};
    constexpr int kBattleshipsFirst[] = {2, 1, 0, 3, 4, 6, 5, 7};
    constexpr int kCruisersFirst[] = {3, 4, 2, 1, 0, 6, 5, 7};
    constexpr int kCarriersFirst[] = {4, 3, 2, 1, 0, 6, 5, 7};
    constexpr int kSonarFirst[] = {5, 4, 3, 2, 1, 0, 6, 7};
    const int* priorities = kFrigatesFirst;
    std::size_t count = std::size(kFrigatesFirst);
    switch (slotType) {
        case 1: priorities = kDestroyersFirst; count = std::size(kDestroyersFirst); break;
        case 2: priorities = kBattleshipsFirst; count = std::size(kBattleshipsFirst); break;
        case 3: priorities = kCruisersFirst; count = std::size(kCruisersFirst); break;
        case 4: priorities = kCarriersFirst; count = std::size(kCarriersFirst); break;
        case 5: priorities = kSonarFirst; count = std::size(kSonarFirst); break;
        default: break;
    }
    const int* found = std::find(priorities, priorities + count, family);
    return static_cast<std::size_t>(found - priorities);
}

/// ART-S007 `GrowthFormation`'s naval half (formations.lua:709-721) selects
/// these land-independent surface widths by the shared sea counter: three-wide
/// up to nine, five-wide up to twenty-five, seven-wide past that
/// (`Three`/`Five`/`SevenNavalGrowthFormation`). AttackMove's naval half
/// (formations.lua:654-666) is the 5/7/9-wide split at ten and twenty-five.
/// `seaCounter` picks BOTH the naval and the sub `AttackFormation`/`GrowthFormation`
/// blocks (`AttackFormation` formations.lua:650-666, `GrowthFormation` 705-721):
/// `seaCounter = max(sea surface hulls, submerged hulls)` after the sub count is
/// backed out of the sea list. Each domain still fills only its own count of
/// slots — the shared beat only selects the block both domains lay.
[[nodiscard]] std::size_t seaCounter(std::size_t sea, std::size_t sub) noexcept {
    return sea >= sub ? sea : sub;
}
[[nodiscard]] std::size_t navalFormationWidth(std::size_t counter, bool attack) noexcept {
    if (attack) {
        if (counter <= 10) {
            return 5;
        }
        if (counter <= 25) {
            return 7;
        }
        return 9;
    }
    if (counter <= 9) {
        return 3;
    }
    if (counter <= 25) {
        return 5;
    }
    return 7;
}
/// ART-S007 `GrowthFormation`'s sub half (formations.lua:709-721) sails submerged
/// hulls in their own blocks on the shared sea-counter beats: three-wide up to nine
/// (`ThreeWideSubGrowthFormation`, 2 rows of 3, formations.lua:534-539), five-wide
/// past that — both `FiveWideSubGrowthFormation` (3 rows of 5, formations.lua:541-547)
/// and `SevenWideSubGrowthFormation` (4 rows of 5, formations.lua:549-557) lay five
/// columns, so the 25/49 beats re-fill rather than widen. AttackMove's sub half
/// (formations.lua:654-666) is the 5/7/9-wide split at ten and twenty-five
/// (`FiveWideSubGrowthFormation` reused at 656, `SevenWideSubAttackFormation` 3 rows
/// of 7 at 659, `NineWideSubAttackFormation` 4 rows of 9 at 662-665). Widths feed the
/// same `BlockBuilderLand` slot geometry, anchor, facing and retry path as land.
[[nodiscard]] std::size_t subFormationWidth(std::size_t counter, bool attack) noexcept {
    if (attack) {
        if (counter <= 10) {
            return 5;
        }
        if (counter <= 25) {
            return 7;
        }
        return 9;
    }
    if (counter <= 9) {
        return 3;
    }
    return 5;
}

[[nodiscard]] const std::shared_ptr<SharedCommand>& ensureSharedCommand(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    Fx formationAnchorX, Fx formationAnchorZ, std::string_view scriptTask,
    std::span<const std::uint8_t> scriptData, UnitStore& store,
    std::shared_ptr<SharedCommand>& shared) {
    if (shared == nullptr) {
        shared = std::make_shared<SharedCommand>(SharedCommand{
            .tick = command.tick,
            .source = source,
            .id = id,
            .player = command.player,
            .kind = command.kind,
            .queued = command.queued,
            // Finalized to the accepted subset after every requested member has been tried.
            .units = {},
            .targetX = formationAnchorX,
            .targetZ = formationAnchorZ,
            .target = command.target,
            .buildType = command.buildType,
            .creationSerial = store.allocateCommandSerial(),
            .originalCount = count,
            .remainingCount = count,
            .scriptTask = std::string{scriptTask},
            .scriptData = {scriptData.begin(), scriptData.end()},
        });
        (void)store.registerCommand(shared);
    }
    return shared;
}
/// Whether retail's outgoing move task preserves motion while this replacement clears it.
/// Of the command kinds implemented here, C-212's keep set contains Stop, Reclaim,
/// BuildMobile, and Upgrade. Factory production is BuildFactory and deliberately not included.
[[nodiscard]] bool replacementKeepsMotion(const Command& command, const UnitStore& store,
                                           const UnitCatalog& catalog) noexcept {
    if (command.kind == CommandKind::Stop || command.kind == CommandKind::Reclaim) {
        return true;
    }
    if (command.kind != CommandKind::Build) {
        return false;
    }
    const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
    const unitdef::UnitDef* product = catalog.def(command.buildType);
    if (builder == nullptr || product == nullptr) {
        return false;
    }
    const bool upgrade = !builder->upgradesTo.empty() && builder->upgradesTo == product->name;
    const bool factoryProduction = builder->hasCategory("FACTORY") && product->isMobile();
    return upgrade || !factoryProduction;
}
[[nodiscard]] bool applyCommandMember(
    const Command& command, CommandSource source, CommandId id, std::uint32_t count,
    Fx formationAnchorX, Fx formationAnchorZ, std::shared_ptr<SharedCommand>& shared,
    UnitStore& store,
    const UnitCatalog& catalog, std::span<const Player> players, std::span<const Army> armies,
    const Terrain& terrain, const PassabilityGrid* grid, TickRate rate,
    std::vector<Construction>* building, EventQueue* events, FeatureStore* features,
    PathService* pathService, std::string_view scriptTask,
    std::span<const std::uint8_t> scriptData, ScriptTaskHost* scriptTasks,
    const PassabilityGrid* approachGrid,
    std::vector<EnhancementWork>* enhancements) {
    // A stale handle first, before anything else looks at the slot. A player may click a unit
    // that died on the tick their order was issued, and a replay of an old log may name a unit
    // that no longer exists — in both cases the generation has moved on, so this must not
    if (!store.alive(command.unit)) {
        return false;
    }

    // `C-231` (`0x006f6460`): `Sim::IssueCommand`'s per-unit gate refuses a
    // unit still being built unless it is a FACTORY. Retail's `IsBeingBuilt`
    // is the scaffold state — a unit that exists but is not yet complete. This
    // sim has no such thing: a rising structure is a `Construction` row, not a
    // live unit, and an upgrade's `upgradeOf` target is already complete —
    // retail accepts a queued next tier on it (the extractor test proves it).
    // The gate is therefore vacuous here and stays a comment, not code.

    const Player* player = playerFor(command.player, players);
    if (player == nullptr || !authorised(*player, store, command.unit, armies)) {
        return false;
    }
    if (command.kind == CommandKind::Assist && !validAssist(command, store, catalog)) {
        return false;
    }
    if (command.kind == CommandKind::Guard
        && (!validGuard(command, store, catalog)
            || !repairStillAllied(command.unit.index, command.target, store, armies))) {
        return false;
    }
    if (command.kind == CommandKind::Repair && !validRepair(command, store, catalog, armies)) {
        return false;
    }
    if (command.kind == CommandKind::ReclaimUnit
        && !validReclaimUnit(command, store, catalog, armies)) {
        return false;
    }
    if (command.kind == CommandKind::Capture
        && !validCapture(command, store, catalog, armies)) {
        return false;
    }
    if (command.kind == CommandKind::Sacrifice
        && !validSacrifice(command, store, catalog, armies,
                           building != nullptr ? std::span<const Construction>{*building}
                                               : std::span<const Construction>{},
                           enhancements != nullptr
                               ? std::span<const EnhancementWork>{*enhancements}
                               : std::span<const EnhancementWork>{})) {
        return false;
    }
    if (command.kind == CommandKind::MissileLaunch
        && !validMissileLaunch(command, store, catalog)) {
        return false;
    }
    if (command.kind == CommandKind::LoadTransport
        && !validLoadTransport(command, store, catalog)) {
        return false;
    }
    if ((command.kind == CommandKind::UnloadTransport
         || command.kind == CommandKind::Ferry)
        && !validTransportCarrier(command, store, catalog)) {
        return false;
    }

    CommandQueue& orders = store.orders()[command.unit.index];
    if (scriptTasks != nullptr) {
        orders.bindScriptTaskHost(scriptTasks);
    }

    // Retail enforces the cap before dispatching the command kind, so an opaque script task is
    // bounded exactly like a move or build promise.
    if (command.queued && command.kind != CommandKind::Stop && orders.atCapacity()) {
        return false;
    }

    if (command.kind == CommandKind::Script) {
        if (scriptTasks == nullptr || scriptTask.empty()
            || scriptTask.size() > kMaxScriptTaskNameBytes
            || scriptData.size() > kMaxScriptTaskDataBytes) {
            return false;
        }
        const std::shared_ptr<const SharedCommand> payload = ensureSharedCommand(
            command, source, id, count, formationAnchorX, formationAnchorZ, scriptTask,
            scriptData, store, shared);
        QueuedCommand entry{command.unit, payload};
        if (command.queued) {
            (void)orders.give(std::move(entry), true);
            return true;
        }
        if (pathService != nullptr) {
            pathService->cancel(command.unit);
        }
        releaseInterruptedConstruction(building, command, store, catalog);
        teardownMovement(store.motion()[command.unit.index]);
        orders.clear();
        orders.append(std::move(entry));
        return true;
    }
    if (grid == nullptr) {
        return false;
    }
    const PassabilityGrid& movementGrid = *grid;

    // THE CAP, retail's, at retail's boundary (`kCommandQueueCap`). It is checked here rather
    // than inside `CommandQueue::give` because retail checks it in `Sim::IssueCommand` — on
    // the way in from a command source, before anything is appended — so it bounds what a
    // PLAYER can pile up and leaves the engine's own inserts (a patrol's synthetic origin)
    // alone. An order that clears the queue is exempt, so a unit at the cap is still
    // commandable.
    // A STOP IS NOT A QUEUED ORDER HERE, shift or no shift. Recoil allows one — its comment at
    // `CommandAI.cpp:996` says as much, with an exclamation mark — and it needs to, because it
    // has a wait command that a queued stop interacts with. We have none, so a queued stop
    // would be an order to stand still at some future point in a route, which is what deleting
    // the rest of the route already means. It clears and stops.
    if (command.kind == CommandKind::Stop) {
        MoveState& motion = store.motion()[command.unit.index];
        const MoveState previous = motion;
        if (!startCommand(command, store, catalog, terrain, movementGrid, rate, building, events,
                          features, armies, approachGrid, enhancements)) {
            motion = previous;
            return false;
        }
        MoveState stopped = std::move(motion);
        motion = previous;
        if (pathService != nullptr) {
            pathService->cancel(command.unit);
        }
        orders.clear();
        releaseInterruptedConstruction(building, command, store, catalog);
        motion = std::move(stopped);
        (void)ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                  {}, {}, store, shared);
        return true;
    }

    if (command.queued) {
        const std::shared_ptr<const SharedCommand> payload =
            ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                {}, {}, store, shared);
        // Factory production is repeatable: Shift-clicking the same tank twice means two tanks,
        // unlike placing the same structure twice, which retains the ordinary cancel gesture.
        // The same repeatable stacking applies to production queued on a factory STILL UNDER
        // CONSTRUCTION — the order parks on the founder's queue and `advanceMatch`'s completion
        // hand-over moves it to the factory the moment the factory unit exists.
        if (command.kind == CommandKind::Build) {
            const unitdef::UnitDef* builder = catalog.def(store.typeAt(command.unit.index));
            const unitdef::UnitDef* product = catalog.def(command.buildType);
            if (builder != nullptr && product != nullptr && product->isMobile()
                && (builder->hasCategory("FACTORY")
                    || (building != nullptr
                        && constructingFactory(*building, command.unit, catalog)))) {
                orders.append(QueuedCommand{command.unit, payload});
                return true;
            }
        }
        const bool currentWasPatrol =
            orders.current() != nullptr && orders.current()->kind() == CommandKind::Patrol;
        const bool alreadyPatrolling = std::any_of(
            orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                return queued.kind() == CommandKind::Patrol;
        });
        QueuedCommand entry{command.unit, payload};
        if (command.kind == CommandKind::Move) {
            entry.setTargetPosition(command.targetX, command.targetZ);
        }
        if (command.kind == CommandKind::Patrol && !alreadyPatrolling) {
            entry.setPatrolOrigin({store.transforms()[command.unit.index].x,
                                   store.transforms()[command.unit.index].z});
        }
        const CommandQueue::Result result = orders.give(std::move(entry), true, &catalog);
        if (command.kind == CommandKind::Patrol
            && (result == CommandQueue::Result::Cancelled
                || result == CommandQueue::Result::CancelledCurrent)) {
            const std::size_t points = static_cast<std::size_t>(std::count_if(
                orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                    return queued.kind() == CommandKind::Patrol;
                }));
            const bool hasHiddenOrigin = std::any_of(
                orders.entries().begin(), orders.entries().end(), [](const QueuedCommand& queued) {
                    return queued.kind() == CommandKind::Patrol && queued.patrolOrigin().has_value();
                });
            if (points + static_cast<std::size_t>(hasHiddenOrigin) < 2) {
                orders.remove(CommandKind::Patrol);
                if (currentWasPatrol) {
                    MoveState& motion = store.motion()[command.unit.index];
                    motion.moving = false;
                    motion.path.clear();
                    motion.pathIndex = 0;
                    if (const QueuedCommand* next = orders.current()) {
                        if (startCommand(next->asCommand(), store, catalog, terrain, movementGrid, rate,
                                         building, events, features, armies, approachGrid,
                                         enhancements)) {
                        }
                    }
                    return true;
                }
            }
        }
        switch (result) {
        case CommandQueue::Result::CancelledCurrent: {
            // The order the unit was carrying out has been taken away, so it has to be
            // interrupted as well as forgotten. Recoil pushes a stop to the front and lets its
            // own slow update pick it up (`:1044-1049`); stopping here and starting whatever
            // is next reaches the same state one tick sooner.
            MoveState& motion = store.motion()[command.unit.index];
            motion.moving = false;
            motion.path.clear();
            motion.pathIndex = 0;
            cancelConstructionFor(building, command, store, catalog);
            if (const QueuedCommand* next = orders.current()) {
                if (startCommand(next->asCommand(), store, catalog, terrain, movementGrid, rate, building,
                                 events, features, armies, approachGrid, enhancements)) {
                }
            }
            return true;
        }
        case CommandQueue::Result::Appended:
        case CommandQueue::Result::Cancelled:
        case CommandQueue::Result::Replaced:
            // Appended or cancelled, and either way the order took. `Replaced` cannot happen
            // on this branch — `give` only returns it for an unqueued order — and is listed so
            // that adding a `Result` is a compile error rather than a silent fall-through.
            return true;
        }
        return true;
    }

    // A plain ground move is accepted as intent, then its owning army's FIFO service performs
    // the existing A* over later beats. Keeping the entry active prevents ordinary dispatch from
    // treating an unpublished path as an arrived move.
    if (command.kind == CommandKind::Move && pathService != nullptr
        && !store.motion()[command.unit.index].airborne
        && !store.motion()[command.unit.index].canFly) {
        const std::shared_ptr<const SharedCommand> payload =
            ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                                {}, {}, store, shared);
        MoveState& motion = store.motion()[command.unit.index];
        pathService->cancel(command.unit);
        orders.clear();
        teardownMovement(motion);
        QueuedCommand entry{command.unit, payload};
        entry.setTargetPosition(command.targetX, command.targetZ);
        orders.append(std::move(entry));
        orders.markCurrentActive();
        const Transform& at = store.transforms()[command.unit.index];
        motion.pathPhaseStartX = movementGrid.cellAtWorld(at.x);
        motion.pathPhaseStartZ = movementGrid.cellAtWorld(at.z);
        motion.pathPhaseCellsX = movementGrid.cellsX;
        pathService->enqueue(PathRequest{.unit = command.unit,
                                         .command = id,
                                         .army = motion.armyIndex,
                                         .fromX = at.x,
                                         .fromZ = at.z,
                                         .targetX = command.targetX,
                                         .targetZ = command.targetZ,
                                         .grid = std::make_shared<PassabilityGrid>(movementGrid)});
        return true;
    }

    // Stage the replacement before clearing so a refused order changes nothing. Once accepted,
    // restore the outgoing task for the clear notifications, apply C-212's replacement-kind
    // teardown, insert the replacement, then publish the staged motion from its successful
    // start. Observers therefore see no replacement during `Cleared` and never see the old task
    // destroyed according to the wrong incoming kind.
    MoveState& motion = store.motion()[command.unit.index];
    const MoveState previous = motion;
    const bool keepMotion = replacementKeepsMotion(command, store, catalog);
    if (!keepMotion) {
        teardownMovement(motion);
    }
    if (!startCommand(command, store, catalog, terrain, movementGrid, rate, building, events,
                      features, armies, approachGrid, enhancements)) {
        // A refused move may still be served — by air (#15800). The offer
        // rewrites both queues itself, so a true return skips the staging
        // path entirely; the click is carried inside the rewrite.
        if (command.kind == CommandKind::Move
            && offerAutoEmbark(store, catalog, command.unit.index, command)) {
            if (pathService != nullptr) {
                pathService->cancel(command.unit);
            }
            return true;
        }
        // A build refused because its site is OCCUPIED still means "build there": an
        // allied colleague's scaffold or an abandoned one is joined, not routed around.
        // Admission is all intake owes it — head dispatch decides lend vs takeover.
        // A factory build refused by the UNIT CAP is admitted the same way: retail's
        // `CFactoryBuildTask` retries the create every beat, so the order waits at
        // the head rather than being dropped (`0x0074fda0`).
        if (command.kind != CommandKind::Build || building == nullptr
            || !(joinableConstructionAt(*building, command, store, catalog, armies)
                 || factoryProductionCapped(command, store, catalog, armies, *building))) {
            return false;
        }
    }
    MoveState replacementMotion = std::move(motion);
    motion = previous;
    releaseInterruptedConstruction(building, command, store, catalog);
    const std::shared_ptr<const SharedCommand> payload =
        ensureSharedCommand(command, source, id, count, formationAnchorX, formationAnchorZ,
                            {}, {}, store, shared);
    QueuedCommand entry{command.unit, payload};
    if (command.kind == CommandKind::Move || command.kind == CommandKind::AttackMove) {
        entry.setTargetPosition(command.targetX, command.targetZ);
    }
    if (command.kind == CommandKind::Patrol) {
        entry.setPatrolOrigin({store.transforms()[command.unit.index].x,
                               store.transforms()[command.unit.index].z});
    }
    if (pathService != nullptr) {
        pathService->cancel(command.unit);
    }
    orders.clear();
    if (!keepMotion) {
        teardownMovement(motion);
    }
    orders.append(std::move(entry));
    orders.markCurrentActive();
    motion = std::move(replacementMotion);
    return true;
}

/// A structure order's site on the build grid, applied ONCE at intake so the queue, the
/// replayed log, the finished-work match and every downstream consumer agree on the site the
/// order claims. Mobile products site at their factory and are left alone; deposit-bound
/// structures keep the deposit centre (`Terrain::buildSite`).
template <typename Order>
[[nodiscard]] Order onBuildGrid(const Order& order, const UnitCatalog& catalog,
                                const Terrain& terrain) {
    if (order.kind != CommandKind::Build) {
        return order;
    }
    const unitdef::UnitDef* def = catalog.def(order.buildType);
    if (def == nullptr || def->isMobile()) {
        return order;
    }
    Order snapped = order;
    const std::array<Fx, 2> site = terrain.buildSite(*def, order.targetX, order.targetZ);
    snapped.targetX = site[0];
    snapped.targetZ = site[1];
    return snapped;
}
} // namespace

bool ApplyCommandResult::acceptedUnit(UnitId unit) const noexcept {
    return std::ranges::find(accepted, unit) != accepted.end();
}

ApplyCommandResult applyCommand(const CommandIssue& issued, UnitStore& store,
                                const UnitCatalog& catalog,
                                std::span<const Player> players,
                                std::span<Army> armies, const Terrain& terrain,
                                 const CommandGridForUnit& gridForUnit, TickRate rate,
                                 std::vector<Construction>* building, EventQueue* events,
                                 FeatureStore* features, PathService* pathService,
                                 ScriptTaskHost* scriptTasks,
                                 const CommandGridForUnit& approachGridForUnit,
                                 std::vector<SiloAmmo>* siloAmmo,
                                 std::vector<SiloBuild>* siloQueue,
                                 std::vector<SelfDestructWork>* selfDestructs,
                                 std::vector<EnhancementWork>* enhancements) {
    const CommandIssue issue = onBuildGrid(issued, catalog, terrain);
    ApplyCommandResult result;
    if (!validCancellation(issue) || issue.source == kInvalidCommandSource || issue.id == kInvalidCommandId
        || issue.player != static_cast<PlayerIndex>(issue.source)
        || commandSource(issue.id) != issue.source || issue.count == 0
        || playerFor(issue.player, players) == nullptr || store.commandIdLive(issue.id)
        || (issue.kind == CommandKind::Script
            && (issue.scriptTask.empty()
                || issue.scriptTask.size() > kMaxScriptTaskNameBytes
                || issue.scriptData.size() > kMaxScriptTaskDataBytes))
        || (issue.kind != CommandKind::Script
            && (!issue.scriptTask.empty() || !issue.scriptData.empty()))) {
        return result;
    }
    if (issue.kind == CommandKind::OfferDraw) {
        // Army-level, not a unit order: `SimUtils.SetOfferDraw` flips
        // `brain.OfferingDraw` on the issuing army, and `victory.lua` ends the
        // match in a draw the moment every surviving brain offers. `scriptBit`
        // carries the flag so an offer can be withdrawn. The issuer's army is
        // already validated above.
        for (Army& army : armies) {
            if (army.index == playerFor(issue.player, players)->army) {
                army.offeringDraw = issue.scriptBit != 0;
            }
        }
        return result;
    }

    std::vector<UnitId> canonical = issue.units;
    canonicalizeUnits(canonical);

    // `GrowthFormation` is the travel formation for non-air grouped `Move`, `AttackFormation`
    // for grouped `AttackMove` (C-178; ART-S007 lua/formations.lua:619-641). Members fill
    // front rows first in category order — experimentals, direct fire by tech,
    // artillery, anti-air, shields, engineers, then the rest — which is retail
    // `lua/formations.lua`'s DFFirst block order reduced to one rank per family.
    // Rejected handles must not consume a formation slot.
    const Player* issuer = playerFor(issue.player, players);
    std::vector<UnitId> formationMembers;
    const bool isAttackOrder = issue.kind == CommandKind::AttackMove;
    bool useGrowthFormation = (issue.kind == CommandKind::Move || isAttackOrder)
                              && canonical.size() > 1;
    // Retail flies air groups through `GrowthFormation` too (ART-S007
    // lua/formations.lua:676-728): `BlockBuilderAir` lays a staggered chevron
    // (`StaggeredChevronBlock`, formations.lua:371-375) with its own
    // `GetChevronPosition` offsets (formations.lua:1013-1030), not the land
    // block — so flyable members join the formation set and fan out below.
    std::vector<UnitId> airMembers;
    for (const UnitId unit : canonical) {
        if (!useGrowthFormation) {
            break;
        }
        if (!store.alive(unit) || issuer == nullptr || !authorised(*issuer, store, unit, armies)
            || !store.motion()[unit.index].canFly) {
            continue;
        }
        airMembers.push_back(unit);
    }
    for (const UnitId unit : canonical) {
        if (!useGrowthFormation) {
            break;
        }
        if (!store.alive(unit) || issuer == nullptr || !authorised(*issuer, store, unit, armies)
            || store.motion()[unit.index].canFly || gridForUnit == nullptr
            || gridForUnit(unit) == nullptr) {
            continue;
        }
        formationMembers.push_back(unit);
    }
    useGrowthFormation = useGrowthFormation
                         && (formationMembers.size() + airMembers.size()) > 1;
    const auto formationClass = [&](UnitId unit) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(unit.index));
        if (def == nullptr) {
            return std::pair{7, 3};
        }
        const int tech = def->hasCategory("TECH3") ? 0
                       : def->hasCategory("TECH2") ? 1
                       : def->hasCategory("TECH1") ? 2
                                                   : 3;
        if (def->hasCategory("EXPERIMENTAL")) {
            return std::pair{0, tech};
        }
        // Retail `DirectFire` (formations.lua:37) subtracts both `CONSTRUCTION`
        // and `COMMAND`: an armed commander is construction, not a front-rank
        // gun — `Construction` (formations.lua:38) claims it instead.
        if ((def->hasCategory("DIRECTFIRE") || def->hasCategory("INDIRECTFIRE"))
            && !def->hasCategory("CONSTRUCTION") && !def->hasCategory("ENGINEER")
            && !def->hasCategory("COMMAND")) {
            return std::pair{1, tech};
        }
        if (def->hasCategory("ARTILLERY") || def->hasCategory("INDIRECTFIRE")) {
            return std::pair{2, tech};
        }
        if (def->hasCategory("ANTIAIR")) {
            return std::pair{3, tech};
        }
        if (def->hasCategory("SHIELD")) {
            return std::pair{4, tech};
        }
        if (def->hasCategory("CONSTRUCTION") || def->hasCategory("ENGINEER")
            || def->hasCategory("COMMAND")) {
            return std::pair{5, tech};
        }
        return std::pair{6, tech};
    };
    // Retail `NavalCategories` (formations.lua:408-422): light boats count as
    // frigate family, defensive boats and mobile sonar as sonar family, and
    // anything naval-but-unmatched (e.g. T1/T2 subs counted above the sea
    // list) as the remaining family. Nuclear subs keep their own family.
    const auto formationSeaClass = [&](UnitId unit) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(unit.index));
        if (def == nullptr) {
            return 7;
        }
        if (def->hasCategory("FRIGATE") || def->hasCategory("LIGHTBOAT")) {
            return 0;
        }
        if (def->hasCategory("DESTROYER")) {
            return 1;
        }
        if (def->hasCategory("BATTLESHIP")) {
            return 2;
        }
        if (def->hasCategory("CRUISER")) {
            return 3;
        }
        if (def->hasCategory("NAVALCARRIER")) {
            return 4;
        }
        if (def->hasCategory("MOBILESONAR") || def->hasCategory("DEFENSIVEBOAT")) {
            return 5;
        }
        if (def->hasCategory("NUKESUB")) {
            return 6;
        }
        return 7;
    };
    // Retail `ChevronSlot` (formations.lua:359): each chevron slot fills from
    // AntiAir, then Experimental, AntiNavy, GroundAttack, Bombers, Intel,
    // Transports, and what misses every family. Tech ranks high-to-low inside
    // each family (`AirCategories`, formations.lua:322-344).
    const auto formationAirClass = [&](UnitId unit) {
        const unitdef::UnitDef* def = catalog.def(store.typeAt(unit.index));
        if (def == nullptr) {
            return std::pair{7, 3};
        }
        const int tech = def->hasCategory("TECH3") ? 0
                       : def->hasCategory("TECH2") ? 1
                       : def->hasCategory("TECH1") ? 2
                                                   : 3;
        const bool air = def->hasCategory("AIR");
        if (air && def->hasCategory("ANTIAIR") && !def->hasCategory("GROUNDATTACK")) {
            return std::pair{0, tech};
        }
        if (air && def->hasCategory("EXPERIMENTAL")) {
            return std::pair{1, tech};
        }
        if (air && def->hasCategory("ANTINAVY")) {
            return std::pair{2, tech};
        }
        if (air && def->hasCategory("GROUNDATTACK") && !def->hasCategory("ANTIAIR")) {
            return std::pair{3, tech};
        }
        if (air && def->hasCategory("BOMBER")) {
            return std::pair{4, tech};
        }
        if (air && (def->hasCategory("SCOUT") || def->hasCategory("RADAR"))) {
            return std::pair{5, tech};
        }
        if (air && def->hasCategory("TRANSPORTATION")
            && !def->hasCategory("GROUNDATTACK")) {
            return std::pair{6, tech};
        }
        return std::pair{7, tech};
    };
    // Stable: ties keep canonical order, so a homogeneous group fills exactly as
    // before and selection order stays presentation-only.
    std::ranges::stable_sort(airMembers, {}, formationAirClass);
    // Stable: ties keep canonical order, so a homogeneous group fills exactly as
    // before and selection order stays presentation-only.
    std::ranges::stable_sort(formationMembers, {}, formationClass);
    // Retail splits `formationUnits` into land/sea/air lists BEFORE block choice
    // (`GrowthFormation` formations.lua:679-723, `AttackFormation` 622-668): each
    // domain fills its own blocks on the same anchor/facing. Submerged hulls sail
    // apart from surface hulls (the `submersible` flag `SceneBuild` derives from
    // `MotionType::SurfacingSub` takes precedence over `surfaceWater`).
    std::vector<UnitId> landMembers;
    std::vector<UnitId> seaMembers;
    std::vector<UnitId> subMembers;
    landMembers.reserve(formationMembers.size());
    seaMembers.reserve(formationMembers.size());
    subMembers.reserve(formationMembers.size());
    for (const UnitId unit : formationMembers) {
        if (store.motion()[unit.index].submersible) {
            subMembers.push_back(unit);
        } else if (store.motion()[unit.index].surfaceWater) {
            seaMembers.push_back(unit);
        } else {
            landMembers.push_back(unit);
        }
    }
    // Retail `AttackFormation` land blocks fill per-slot category ranks inside
    // each row (`BlockBuilderLand`, formations.lua:838-913): the <=16-member
    // two-row block reads Util, AA, Shield, Art, Art, Shield, AA, Util across
    // its second row (formations.lua:192-197), and wider blocks transcribe
    // their own rows (formations.lua:200-310), so support hulls take the
    // flanks there instead of a second combat line. Replay that walk by
    // ordering land members into slot order — row-major over the centre-out
    // column walk — keyed by each slot's rank, so an engineer owns the Util
    // flank before any combat spillover reaches row two. Other domains keep
    // intake order.
    const std::size_t landWidthForRanks =
        isAttackOrder ? attackFormationWidth(landMembers.size())
                      : growthFormationWidth(landMembers.size());
    const bool rankGrowthLand = !isAttackOrder && landMembers.size() >= 5
                                && (landWidthForRanks >= 4 && landWidthForRanks <= 8);
    if ((isAttackOrder && landMembers.size() >= 5
         && (landWidthForRanks == 8 || landWidthForRanks == 10 || landWidthForRanks == 12
             || landWidthForRanks == 14 || landWidthForRanks == 16
             || landWidthForRanks == 18 || landWidthForRanks == 20))
        || rankGrowthLand) {
        // `GetColSpot` (formations.lua:915-930) maps the visit order to physical
        // slots: visit c takes physical column (half - floor(c/2)) on odd visits
        // and (half + c/2) on even visits — for width 8: 4,5,3,6,2,7,1,8.
        // Wider widths walk the same centre-out rhythm over their own column
        // count, so build the walk per width instead of pinning width 8.
        const std::size_t width = landWidthForRanks;
        std::vector<std::size_t> walk;
        walk.reserve(width);
        const std::size_t half = width / 2;
        for (std::size_t visit = 0; visit < width; ++visit) {
            const std::size_t step = visit / 2;
            walk.push_back(visit % 2 == 0 ? half - step : half + 1 + step);
        }
        std::vector<UnitId> ranked;
        ranked.reserve(landMembers.size());
        std::vector<UnitId> pool = landMembers;
        for (std::size_t slot = 0; slot < landMembers.size(); ++slot) {
            const std::size_t row = slot / width;
            const std::size_t column = walk[slot % width];
            const int want = isAttackOrder ? attackLandSlotRank(row, column, width)
                                           : growthLandSlotRank(row, column, width);
            // min_element keeps the first equal-priority member, preserving
            // canonical and tech ordering within the chosen family.
            const auto pick = std::ranges::min_element(pool, {}, [&](UnitId member) {
                return attackLandFamilyPriority(want, formationClass(member).first);
            });
            ranked.push_back(*pick);
            pool.erase(pick);
        }
        landMembers = std::move(ranked);
    }
    // Retail naval blocks rank hulls per slot on both scripts: the Growth
    // `Three`/`Five`/`SevenNavalGrowthFormation` blocks (formations.lua:450-487)
    // read all-`FrigatesFirst` first rows with `DestroyersFirst`/`SonarFirst`
    // rhythms behind, and the `AttackFormation` 5/7/9-wide blocks
    // (formations.lua:499-528) every row of their own `*First` lists. Sub
    // blocks are uniform `Subs` (formations.lua:562-584), so only surface
    // hulls rank here on either script.
    const std::size_t seaCounterForRanks = seaCounter(seaMembers.size(), subMembers.size());
    const std::size_t seaWidthForRanks =
        isAttackOrder ? navalFormationWidth(seaCounterForRanks, true)
                      : navalFormationWidth(seaCounterForRanks, false);
    const bool rankSea = !seaMembers.empty()
                         && ((isAttackOrder
                              && (seaWidthForRanks == 5 || seaWidthForRanks == 7
                                  || seaWidthForRanks == 9))
                             || (!isAttackOrder
                                 && (seaWidthForRanks == 3 || seaWidthForRanks == 5
                                     || seaWidthForRanks == 7)));
    if (rankSea) {
        const std::size_t width = seaWidthForRanks;
        std::vector<std::size_t> walk;
        walk.reserve(width);
        for (std::size_t visit = 0; visit < width; ++visit) {
            // The geometry lays rank r (row-major over this same visit order)
            // left-to-right without a `GetColSpot` un-walk for non-8 widths,
            // so rank slots into the geometric column directly: row 0's
            // frigate slots are visits 0-4 regardless of their physical side.
            walk.push_back(visit + 1);
        }
        std::vector<UnitId> ranked;
        std::vector<UnitId> pool = seaMembers;
        for (std::size_t slot = 0; slot < seaMembers.size(); ++slot) {
            const std::size_t row = slot / width;
            const std::size_t column = walk[slot % width];
            const int want = isAttackOrder ? attackNavalSlotRank(row, column, width)
                                          : growthNavalSlotRank(row, column, width);
            const auto pick = std::ranges::min_element(pool, {}, [&](UnitId member) {
                return attackNavalFamilyPriority(want, formationSeaClass(member));
            });
            ranked.push_back(*pick);
            pool.erase(pick);
        }
        seaMembers = std::move(ranked);
    }
    // Retail `BlockBuilderLand` multiplies every sea/sub slot by the call-site
    // `spacing` argument: `GrowthFormation` passes 1.5 for both sea and sub
    // lists (formations.lua:722-723) and `AttackFormation` passes 1.5 too
    // (formations.lua:667-668); land passes nothing, so 1. The naval blocks
    // additionally set `LineBreak = 0.5` (formations.lua:451-587), adding half
    // a row step per row transition — columns spread 1.5 diameters and rows
    // advance 1.5 diameters. Fixed-point `3/2` throughout (C-178, ART-S007).
    const Fx landSpacing = Fx::fromInt(1);
    const Fx seaSpacing = Fx::fromRatio(3, 2);
    const std::size_t landWidth = isAttackOrder ? attackFormationWidth(landMembers.size())
                                                : growthFormationWidth(landMembers.size());
    const std::size_t counter = seaCounter(seaMembers.size(), subMembers.size());
    const std::size_t seaWidth = navalFormationWidth(counter, isAttackOrder);
    const std::size_t subWidth = subFormationWidth(counter, isAttackOrder);
    // The formation's facing. Retail carries it on the wire command as an orientation
    // quaternion (`C-151`, `CDecoder::DecodeCommandData` +0x3c..+0x48) that the engine
    // applies to the Lua `FormationPos` offsets — the script itself returns unrotated
    // geometry. With no UI to supply that quaternion, the deterministic stand-in is the
    // bearing from the issued group's centroid to the click: the formation faces its
    // destination, so the front row lands on the anchor and later rows trail back toward
    // where the group stood. The centroid is over every live unit the issue names —
    // refused members included — because the facing belongs to the selection as issued,
    // the same way retail's UI computes the quaternion before the sim validates anyone.
    // A click exactly on the centroid has no bearing — `fxBearing` answers zero there,
    // which is the unrotated layout this replaces.
    FxWide centroidX = 0;
    FxWide centroidZ = 0;
    std::size_t centroidCount = 0;
    for (const UnitId unit : canonical) {
        if (!store.alive(unit)) {
            continue;
        }
        centroidX += store.transforms()[unit.index].x.raw();
        centroidZ += store.transforms()[unit.index].z.raw();
        ++centroidCount;
    }
    Brad formationFacing = 0;
    if (centroidCount > 0) {
        const Fx centreX = Fx::fromRaw(saturate(centroidX
                                                / static_cast<FxWide>(centroidCount)));
        const Fx centreZ = Fx::fromRaw(saturate(centroidZ
                                                / static_cast<FxWide>(centroidCount)));
        formationFacing = fxBearing(issue.targetX - centreX, issue.targetZ - centreZ);
    }
    const Fx facingSin = fxSin(formationFacing);
    const Fx facingCos = fxCos(formationFacing);

    result.accepted.reserve(canonical.size());
    if (issue.kind == CommandKind::CancelFactoryBuild) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit) || !authorised(*issuer, store, unit, armies)) continue;
            const auto* factory = catalog.def(store.typeAt(unit.index));
            if (factory == nullptr || !factory->isBuilder() || factory->isMobile()) continue;
            auto& queue = store.orders()[unit.index];
            const auto found = std::ranges::find_if(queue.entries(), [&](const QueuedCommand& entry) {
                return entry.kind() == CommandKind::Build
                    && entry.payload().id == issue.cancelCommandId;
            });
            if (found == queue.entries().end()) continue;
            const auto* product = catalog.def(found->buildType());
            if (product == nullptr) continue;
            // Upgrade orders form a dependency chain, including successors of the active tier.
            const auto* tier = factory;
            bool upgrade = false;
            for (auto it = queue.entries().begin(); it != std::next(found); ++it) {
                if (it->kind() != CommandKind::Build) continue;
                const auto* next = catalog.def(it->buildType());
                if (next && tier->upgradesTo == next->name) {
                    tier = next;
                    if (it == found) upgrade = true;
                }
            }
            if (!upgrade && !(factory->hasCategory("FACTORY") && product->isMobile())) continue;
            std::vector<const SharedCommand*> dependents;
            if (upgrade) {
                for (auto it = std::next(found); it != queue.entries().end(); ++it) {
                    if (it->kind() != CommandKind::Build) continue;
                    const auto* next = catalog.def(it->buildType());
                    if (next && tier->upgradesTo == next->name) {
                        dependents.push_back(&it->payload());
                        tier = next;
                    }
                }
            }
            const auto* payload = &found->payload();
            const bool active = queue.active() != nullptr && &queue.active()->payload() == payload;
            const auto* work = building != nullptr ? activeConstruction(*building, unit) : nullptr;
            if (active || (work != nullptr && work->retainedCommandId == issue.cancelCommandId)) {
                cancelActiveConstruction(building, unit);
                if (pathService != nullptr) pathService->cancel(unit);
                teardownMovement(store.motion()[unit.index]);
            }
            (void)queue.removeExact(payload);
            for (const auto* dependent : dependents) (void)queue.removeExact(dependent);
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::Dive) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) continue;
            const Player* player = playerFor(issue.player, players);
            const auto* def = catalog.def(store.typeAt(unit.index));
            auto& motion = store.motion()[unit.index];
            if (!player || !authorised(*player, store, unit, armies) || !def
                || !motion.submersible
                || !unitHasCommandCap(store, catalog, unit.index, "RULEUCC_Dive")) continue;
            // C-198: choose from the committed layer, even while already transitioning.
            motion.diveTargetSubmerged = !motion.submerged;
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::ToggleFactoryRepeat) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !definition->hasCategory("FACTORY")) {
                continue;
            }
            (void)store.setFactoryRepeat(unit, !store.factoryRepeat(unit));
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::ToggleProduction) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !canPauseProduction(*definition)) {
                continue;
            }
            (void)store.setProductionPaused(unit, !store.productionPaused(unit));
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::ToggleScriptBit) {
        // Retail's `ToggleScriptBit` named command (`C-350`): the UI's RULEUTC_*
        // toggles reach `Unit:ToggleScriptBit` and `Unit.lua` flips the feature.
        // Only the bits with real effects are accepted; the cap gate is the same
        // `ToggleCaps` table the rack reads, so a unit cannot be asked to toggle
        // a feature its blueprint never declared.
        static constexpr std::pair<std::uint8_t, std::string_view> kBits[] = {
            {0, "RULEUTC_ShieldToggle"},
            {2, "RULEUTC_JammingToggle"},
            {3, "RULEUTC_IntelToggle"},
            {5, "RULEUTC_StealthToggle"},
            {8, "RULEUTC_CloakToggle"},
        };
        const auto* rule = std::ranges::find_if(
            kBits, [&](const auto& entry) { return entry.first == issue.scriptBit; });
        if (rule == std::end(kBits)) {
            return result;
        }
        for (const UnitId unit : canonical) {
            // `C-349`: toggles are blocked while attached to a transport — the
            // cargo's script bits are the carrier's business until it lands.
            if (!store.alive(unit) || store.motion()[unit.index].attached) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            // `C-255`/`C-379`: the cap gate reads the unit's LIVE caps, not the
            // blueprint's alone — `CreateEnhancement`'s `AddToggleCap` grants
            // survive intake, and `RemoveToggleCap` closes the gate again. A
            // blueprint-declared cap stays accepted with no install; the reader
            // answers false when the unit has no blueprint at all.
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || !unitHasToggleCap(store, catalog, unit.index, rule->second)) {
                continue;
            }
            const bool disabled = !store.scriptBitDisabled(unit, issue.scriptBit);
            (void)store.setScriptBitDisabled(unit, issue.scriptBit, disabled);
            // `SetMaintenanceConsumption{Active,Inactive}` — last writer wins
            // (`Unit.lua:309-380`): every implemented bit calls it, so the unit's
            // upkeep follows whichever toggle it touched last, not the count of
            // features still on.
            (void)store.setMaintenanceActive(unit, !disabled);
            if (issue.scriptBit == 0 && !disabled) {
                // Re-enabling a shield is retail's `OnState`: the bubble gates
                // absorption for `ShieldEnergyDrainRechargeTime` and resumes at
                // the health it kept (`OffHealth`), not at maximum — that refill
                // belongs to damage collapse alone.
                Health& health = store.health()[unit.index];
                const UnitCatalog::ShieldInfo& shield =
                    shieldFor(store, catalog, unit.index);
                if (shield.exists() && health.shield.maximum > Mag{}
                    && health.shield.rechargeRemaining == 0) {
                    health.shield.rechargeRemaining = shield.recharge;
                    health.shield.rechargeRestoresFull = false;
                }
            }
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::Gift) {
        // `C-238`: gifting is `ChangeUnitArmy` reached directly — no capture
        // task, no captor, no cost. `Sim::TransferUnit` (`0x0074dc40`) rejects
        // dead/invalid units; the recipient must be a live ALLIED army other
        // than the unit's own, and `scriptBit` names it. The transfer itself
        // is the same replacement-entity swap capture uses, so kills, health,
        // fuel, silo ammo and shield state all follow the unit.
        const int recipient = issue.scriptBit;
        const auto recipientArmy = std::ranges::find_if(
            armies, [recipient](const Army& army) { return army.index == recipient; });
        for (const UnitId unit : canonical) {
            if (!store.alive(unit) || recipientArmy == armies.end()
                || recipientArmy->defeated) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            if (player == nullptr || !authorised(*player, store, unit, armies)) {
                continue;
            }
            const int owner = store.motion()[unit.index].armyIndex;
            const auto ownerArmy = std::ranges::find_if(
                armies, [owner](const Army& army) { return army.index == owner; });
            if (owner == recipient || ownerArmy == armies.end()
                || !allied(*ownerArmy, *recipientArmy)) {
                continue;
            }
            // The recipient side is a creation (`C-383`): retail's gate at
            // `0x0074fda0` refuses `costTotal + CapCost > unitCap` at the moment
            // the entity would exist. A rising build already reserves its cost
            // there, so the gate's own reserved row is the right denominator.
            // `C-361`: the refused creation is the `OnUnitCapLimitReached`
            // trigger — the recipient's brain hears it, like the scaffold and
            // factory refusals, which is what retail's cap callback reports.
            const unitdef::UnitDef* giftDef = catalog.def(store.typeAt(unit.index));
            if (giftDef != nullptr
                && unitCapBlocks(armies, recipient, catalog,
                                 building != nullptr
                                     ? std::span<const Construction>{*building}
                                     : std::span<const Construction>{},
                                 *giftDef)) {
                emit(events, Event{.kind = EventKind::UnitCapLimitReached,
                                   .unit = unit,
                                   .army = recipient});
                continue;
            }
            const UnitId replacement = transferUnitArmy(
                store, catalog, unit, recipient, events,
                siloAmmo != nullptr ? std::span<SiloAmmo>{*siloAmmo} : std::span<SiloAmmo>{},
                enhancements != nullptr ? std::span<EnhancementWork>{*enhancements}
                                        : std::span<EnhancementWork>{},
                siloQueue != nullptr ? std::span<SiloBuild>{*siloQueue}
                                     : std::span<SiloBuild>{});
            if (replacement.generation != 0) {
                result.accepted.push_back(unit);
            }
        }
        return result;
    }
    if (issue.kind == CommandKind::CycleBuildPriority) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            // Producers only: a unit with no tierable demand funds nothing, so a tier on
            // it would be a flag that never moves a resource — refused rather than stored.
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !canSetBuildPriority(*definition)) {
                continue;
            }
            (void)store.setBuildPriority(
                unit, nextBuildPriority(store.buildPriority(unit)));
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::SetBuildPriority) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            // The cycle's gate plus a valid tier: a value outside the enum is a malformed
            // issue, not a tier — refused outright rather than clamped into a wrong one.
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !canSetBuildPriority(*definition)
                || issue.priority > BuildPriority::High) {
                continue;
            }
            (void)store.setBuildPriority(unit, issue.priority);
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::CycleRetreatThreshold) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            // Mobile only: a threshold on a building is a flag nothing can act on,
            // the same reason a combat unit refuses the production pause.
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || definition == nullptr || !definition->isMobile()) {
                continue;
            }
            (void)store.setRetreatThreshold(
                unit, nextRetreatThreshold(store.retreatThreshold(unit)));
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::CycleTargetFocus) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            const unitdef::UnitDef* definition = catalog.def(store.typeAt(unit.index));
            // Armed only: a focus on a unit with no acquiring weapon is a flag
            // nothing can read — `nearestTarget` early-outs on exactly this
            // weapon shape, so the gate asks the same question it does.
            const bool canAcquire = definition != nullptr
                && std::ranges::any_of(definition->weapons, [&](const unitdef::Weapon& w) {
                       return weaponFiresFor(store, catalog, unit.index, w)
                              && !weaponManuallyFiredFor(store, catalog, unit.index, w)
                              && !w.targetPriorities.empty();
                   });
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || !canAcquire) {
                continue;
            }
            (void)store.setTargetFocus(
                unit, nextTargetFocus(store.targetFocus(unit)));
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::SiloBuildTactical
        || issue.kind == CommandKind::SiloBuildNuke) {
        // `IssueSiloBuildTactical/Nuke` reach the same `SiloAddBuild` the idle refill
        // uses (`C-241`): the slot exists, its round is buildable, and stored plus
        // queued is below capacity — anything else refuses.
        const std::uint8_t slot =
            issue.kind == CommandKind::SiloBuildTactical ? 0 : 1;
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || siloAmmo == nullptr || siloQueue == nullptr
                || !queueSiloBuild(*siloQueue, *siloAmmo, unit, slot)) {
                continue;
            }
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::ToggleSiloAuto) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || siloAmmo == nullptr) {
                continue;
            }
            // Retail's AutoMode is one flag on the unit; the records are this model's
            // unit, so the toggle sets every record the owner carries at once.
            bool found = false;
            bool enable = true;
            for (SiloAmmo& ammo : *siloAmmo) {
                if (ammo.owner != unit) {
                    continue;
                }
                if (!found) {
                    enable = !ammo.autoBuild;
                }
                found = true;
                ammo.autoBuild = enable;
            }
            if (!found) {
                continue;
            }
            result.accepted.push_back(unit);
        }
        return result;
    }
    if (issue.kind == CommandKind::SelfDestruct) {
        for (const UnitId unit : canonical) {
            if (!store.alive(unit)) {
                continue;
            }
            const Player* player = playerFor(issue.player, players);
            if (player == nullptr || !authorised(*player, store, unit, armies)
                || selfDestructs == nullptr) {
                continue;
            }
            // The toggle (`C-345`): an entry already counting down is cancelled, a unit
            // with none starts `selfdestruct.lua`'s five-second `StartCountdown`.
            const auto found = std::ranges::find_if(*selfDestructs,
                [unit](const SelfDestructWork& work) { return work.unit == unit; });
            if (found != selfDestructs->end()) {
                selfDestructs->erase(found);
            } else {
                selfDestructs->push_back(SelfDestructWork{
                    .unit = unit,
                    .remainingTicks = rate.ticks(seconds(5.0f))});
            }
            result.accepted.push_back(unit);
        }
        return result;
    }
    std::shared_ptr<SharedCommand> shared;
    for (std::size_t rank = 0; rank < canonical.size(); ++rank) {
        const UnitId unit = canonical[rank];
        // Validate the handle and authority before asking a resolver that may index by the
        // handle. `applyCommandMember` repeats these checks at the mutation boundary.
        if (!store.alive(unit)) {
            continue;
        }
        const Player* player = playerFor(issue.player, players);
        if (player == nullptr || !authorised(*player, store, unit, armies)) {
            continue;
        }
        const PassabilityGrid* grid = gridForUnit != nullptr ? gridForUnit(unit) : nullptr;
        if (grid == nullptr && issue.kind != CommandKind::Script) {
            continue;
        }
        Command member{
            .tick = issue.tick,
            .player = issue.player,
            .kind = issue.kind,
            .queued = issue.queued,
            .unit = unit,
            .targetX = issue.targetX,
            .targetZ = issue.targetZ,
            .target = issue.target,
            .buildType = issue.buildType,
        };

        Fx localX;
        Fx localZ;
        if (useGrowthFormation) {
            // Retail `GrowthFormation` (lua/formations.lua:676-728) and `AttackFormation`
            // (formations.lua:619-641) both route land units through `BlockBuilderLand`
            // offsets computed here: the engine rotates them by the command's formation
            // quaternion before adding the anchor. `xPos` enumerates columns centre-outward —
            // odd widths 0,+1,-1,+2,-2 and even widths -.5,+.5,-1.5,+1.5 (the `math.mod`
            // branches at formations.lua:880-893) — and `zPos` is `-formationLength`, one row
            // per step behind the anchor (formations.lua:896). A collision diameter is the
            // existing local-target spacing, so its half supplies the even-row half
            // positions; successive rows are one diameter behind the anchor.
            const auto airRank = std::ranges::find(airMembers, unit);
            const bool isAirMember = airRank != airMembers.end();
            // Both formation scripts lay the same chevron for air groups:
            // `AttackFormation` (formations.lua:669-671) and `GrowthFormation`
            // (formations.lua:724-726) each close with `BlockBuilderAir`
            // over the same `StaggeredChevronBlock`.
            if (isAirMember) {
                // Retail `BlockBuilderAir` chevron (ART-S007 lua/formations.lua:1013-1030):
                // `GetChevronPosition` trails each slot one 0.375-elmo step per
                // chevron rank behind the anchor (`yPos = -offset`, so the leader
                // keeps the anchor line) and fans it sideways the same amount,
                // alternating flanks (`xPos = +/-offset`); the two-row
                // `StaggeredChevronBlock` (formations.lua:371-375) adds a
                // per-column block shift (`blockOff`) plus its -2/-1 row
                // stagger (`firstBlockOffset`). Spacing doubles to 2 elmos when
                // an experimental flies along (`BlockBuilderAir`, formations.lua:958).
                const bool hasExperimental = std::ranges::any_of(airMembers, [&](UnitId member) {
                    const unitdef::UnitDef* memberDef =
                        catalog.def(store.typeAt(member.index));
                    return memberDef != nullptr && memberDef->hasCategory("EXPERIMENTAL");
                });
                const std::size_t chevronRank = static_cast<std::size_t>(airRank - airMembers.begin());
                // `BlockBuilderAir` numbers chevron positions from 1
                // (formations.lua:943-1010: `chevronPos` starts at 1 and the
                // `> chevronSize` reset fires before the first slot).
                const std::int32_t chevronPos = static_cast<std::int32_t>(chevronRank % 5) + 1;
                const std::int32_t column = static_cast<std::int32_t>(chevronRank % 3) + 1;
                const Fx spacing = hasExperimental ? Fx::fromInt(2) : Fx::fromInt(1);
                const Fx step = Fx::fromRatio(3, 8) * spacing;
                const std::int32_t whole = chevronPos / 2;
                Fx chevronX = step * whole;
                if (chevronPos % 2 == 0) {
                    chevronX = -chevronX;
                }
                const Fx chevronZ = -(step * whole);
                const std::int32_t rowLen = 3;
                const std::int32_t firstBlockOffset = (rowLen % 2 == 1) ? -1 : -2;
                const std::int32_t blockOff = (column % 2 == 1) ? -(column / 2) * 2 : (column / 2) * 2;
                localX = chevronX + spacing * (blockOff + firstBlockOffset);
                localZ = chevronZ;
            } else {
                const bool isSubMember = std::ranges::find(subMembers, unit) != subMembers.end();
                const bool isSeaMember = std::ranges::find(seaMembers, unit) != seaMembers.end();
                const std::span<const UnitId> domainList = isSubMember
                                                               ? std::span<const UnitId>{subMembers}
                                                               : (isSeaMember ? std::span<const UnitId>{seaMembers}
                                                                              : std::span<const UnitId>{landMembers});
                const std::size_t domainWidth = isSubMember ? subWidth : (isSeaMember ? seaWidth : landWidth);
                const Fx slotSpacing = (isSubMember || isSeaMember) ? seaSpacing : landSpacing;
                const std::size_t formationRank = static_cast<std::size_t>(
                    std::ranges::find(domainList, unit) - domainList.begin());
                // Retail visits slots centre-outward (`GetColSpot`), but the
                // engine lays them left-to-right: visit c of an 8-wide row
                // lands on physical column 4,5,3,6,2,7,1,8. Un-walk the rank
                // so the offset math below keeps its left-to-right columns.
                constexpr std::size_t kUnwalk[8] = {6, 4, 2, 0, 1, 3, 5, 7};
                const std::size_t visit = formationRank % domainWidth;
                const std::size_t column = domainWidth == 8
                                               ? kUnwalk[visit]
                                               : visit;
                const std::size_t row = formationRank / domainWidth;
                const Fx radius = store.motion()[unit.index].radiusElmos;
                const Fx diameter = Fx::fromRaw(saturate(FxWide{radius.raw()} * 2));
                if (domainWidth % 2 == 0) {
                    const FxWide halfDiameterOffsets = column % 2 == 0
                                                           ? -static_cast<FxWide>(column + 1)
                                                           : static_cast<FxWide>(column);
                    localX = Fx::fromRaw(
                        saturate(FxWide{(radius * slotSpacing).raw()} * halfDiameterOffsets));
                } else {
                    const FxWide diameterOffsets = column % 2 == 0
                                                       ? -static_cast<FxWide>(column / 2)
                                                       : static_cast<FxWide>((column + 1) / 2);
                    localX = Fx::fromRaw(
                        saturate(FxWide{(diameter * slotSpacing).raw()} * diameterOffsets));
                }
                localZ = Fx::fromRaw(saturate(-FxWide{(diameter * slotSpacing).raw()}
                                              * static_cast<FxWide>(row)));
            }
        } else if ((issue.kind == CommandKind::Move || issue.kind == CommandKind::AttackMove)
                   && canonical.size() > 1 && !store.motion()[unit.index].canFly) {
            // Degenerate fan-out for a group that cannot fill a formation (fewer than two
            // valid members): centre a line on the clicked anchor, adjacent ranks one
            // collision radius apart, rotated by the same facing so the intake stays
            // consistent whichever path serves it.
            const FxWide offsetRanks = static_cast<FxWide>(rank) * 2
                                       - static_cast<FxWide>(canonical.size() - 1);
            const Fx spacing = store.motion()[unit.index].radiusElmos;
            localX = Fx::fromRaw(saturate(FxWide{spacing.raw()} * offsetRanks));
        }
        // Local → world by the facing yaw, the same convention `slopeAlignment` uses:
        // local +z is the direction the formation faces, local +x its right flank.
        member.targetX += localX * facingCos + localZ * facingSin;
        member.targetZ += localZ * facingCos - localX * facingSin;

        if (applyCommandMember(member, issue.source, issue.id, issue.count, issue.targetX,
                               issue.targetZ, shared, store, catalog, players, armies, terrain,
                               grid, rate, building, events, features, pathService,
                               issue.scriptTask, issue.scriptData, scriptTasks,
                               approachGridForUnit ? approachGridForUnit(unit) : grid,
                               enhancements)) {
            result.accepted.push_back(unit);
        }
    }
    if (shared != nullptr) {
        shared->units = result.accepted;
    }
    return result;
}
bool applyCommand(const Command& ordered, UnitStore& store, const UnitCatalog& catalog,
                  std::span<const Player> players, std::span<Army> armies,
                   const Terrain& terrain, const PassabilityGrid& grid, TickRate rate,
                   std::vector<Construction>* building, EventQueue* events,
                   FeatureStore* features, PathService* pathService,
                   ScriptTaskHost* scriptTasks,
                   std::vector<SiloAmmo>* siloAmmo,
                   std::vector<SiloBuild>* siloQueue,
                   std::vector<SelfDestructWork>* selfDestructs,
                   std::vector<EnhancementWork>* enhancements) {
    const Command command = onBuildGrid(ordered, catalog, terrain);
    if (command.player >= static_cast<PlayerIndex>(kInvalidCommandSource)
        || command.kind == CommandKind::Script) {
        return false;
    }
    const CommandSource source = static_cast<CommandSource>(command.player);
    const std::optional<CommandId> id = store.allocateCommandId(source);
    if (!id) {
        return false;
    }
    const CommandIssue issue{
        .tick = command.tick,
        .source = source,
        .id = *id,
        .player = command.player,
        .kind = command.kind,
        .queued = command.queued,
        .units = {command.unit},
        .targetX = command.targetX,
        .targetZ = command.targetZ,
        .target = command.target,
        .buildType = command.buildType,
    };
    const ApplyCommandResult result = applyCommand(
        issue, store, catalog, players, armies, terrain,
        [&grid](UnitId) { return &grid; }, rate, building, events, features, pathService,
        scriptTasks, {}, siloAmmo, siloQueue, selfDestructs, enhancements);
    return result.acceptedUnit(command.unit);
}

} // namespace rm::sim
