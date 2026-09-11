#include "app/Interface.hpp"

#include "app/SceneBuild.hpp"  // ensureDrawableType — a construction is the first time a
                               // blueprint needs to be DRAWN rather than merely simulated
#include "core/model/Pose.hpp"
#include "core/scene/BuildEffects.hpp"
#include "core/ui/IconAtlas.hpp"
#include "core/ui/CommandPanel.hpp"
#include "core/unit/Role.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace rm::app {
namespace {

/// The colour the energy going into a building takes, by faction.
///
/// SEPARATE FROM THE HUD'S LIVERY (`ui::themeFor`), which is chosen for legibility against
/// readouts and deliberately pushed away from the resource colours — Cybran's chrome is rose so
/// it cannot be mistaken for a loss figure. Out on the battlefield there is nothing to be
/// mistaken for, and the effect should be the faction's own: Cybran build lasers are red.
[[nodiscard]] std::array<float, 4> buildEnergyColour(rm::sim::Faction faction) noexcept {
    switch (faction) {
    case rm::sim::Faction::Uef:
        return {{0.42f, 0.68f, 1.00f, 1.0f}};  // the steel blue of a UEF build frame
    case rm::sim::Faction::Aeon:
        return {{0.36f, 1.00f, 0.72f, 1.0f}};  // the green-white a unit rises out of
    case rm::sim::Faction::Cybran:
        return {{1.00f, 0.24f, 0.18f, 1.0f}};  // nano-swarm red
    case rm::sim::Faction::Seraphim:
        return {{1.00f, 0.82f, 0.36f, 1.0f}};  // the gold its rings contract in
    }
    return {{0.42f, 0.68f, 1.00f, 1.0f}};
}

void appendBuildBeam(std::vector<rm::Particle>& particles, std::array<float, 3> from,
                     std::array<float, 3> to, const std::array<float, 4>& colour) {
    constexpr int kMotesPerBeam = 7;
    for (int mote = 0; mote <= kMotesPerBeam; ++mote) {
        const float t = static_cast<float>(mote) / static_cast<float>(kMotesPerBeam);
        particles.push_back(rm::Particle{
            .origin = {{from[0] + (to[0] - from[0]) * t,
                        from[1] + (to[1] - from[1]) * t,
                        from[2] + (to[2] - from[2]) * t}},
            // These motes are rebuilt every frame. Age zero is also zero opacity during the
            // particle shader's fade-in, so start at its visible crest.
            .age = 0.02f,
            .velocity = {{0.0f, 0.0f, 0.0f}},
            .lifetime = 0.12f,
            .colour = {{colour[0] * 0.9f, colour[1] * 0.9f, colour[2] * 0.9f, 0.0f}},
            .size = 1.6f,
            .growth = 0.0f,
        });
    }
}

/// A handle eligible to own the build panel, or null.
[[nodiscard]] const rm::unitdef::UnitDef* buildCandidateDef(const UnitScene& scene,
                                                            rm::sim::UnitId id) noexcept {
    if (!scene.store.alive(id)) {
        return nullptr;
    }
    const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(id.index));
    if (def == nullptr) {
        return nullptr;
    }
    const int army = scene.armyOf(id.index);
    if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
        return nullptr;
    }
    const rm::unitdef::Role role = rm::unitdef::roleOf(*def);
    return role == rm::unitdef::Role::Commander || role == rm::unitdef::Role::Builder
            || role == rm::unitdef::Role::Factory
            || (def->isBuilder() && !def->upgradesTo.empty())
         ? def
         : nullptr;
}

} // namespace

bool constructionInProgress(const rm::sim::Construction& work) noexcept {
    return !work.finished();
}

float constructionProgress(const rm::sim::Construction& work) noexcept {
    const float total = rm::sim::magToFloat(work.totalBuildTime);
    if (!(total > 0.0f)) {
        // No stated build time is not "finished" — it is a blueprint that said nothing, and a
        // site drawn as complete would be a building that never appears to go up at all. Show
        // it as barely started; the work still completes on the economy's own schedule.
        return 0.0f;
    }
    const float remaining = rm::sim::magToFloat(work.buildTimeRemaining);
    return std::clamp(1.0f - remaining / total, 0.0f, 1.0f);
}

std::optional<rm::ui::InfoCard> constructionCard(const UnitScene& scene,
                                               rm::sim::UnitId builder) {
    if (!scene.store.alive(builder)) {
        return std::nullopt;
    }
    for (const auto& work : scene.building) {
        if ((work.builder != builder && work.upgradeOf != builder)
            || !constructionInProgress(work)) {
            continue;
        }
        rm::ui::InfoCard card;
        card.title = work.isUpgrade() ? "UPGRADING" : "BUILDING";
        card.progress = constructionProgress(work);
        card.corner = std::to_string(static_cast<int>(*card.progress * 100.0f)) + "%";
        if (const auto* def = scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex))) {
            card.rows.push_back({"", def->description.empty() ? def->name : def->description});
        }
        const bool stalled = work.fundedLastTick == rm::sim::Fx{};
        const bool limited = work.fundedLastTick < rm::sim::kFxOne;
        std::string label = "FLOW";
        if (limited && work.armyIndex >= 0
            && static_cast<std::size_t>(work.armyIndex) < scene.economies.size()) {
            // This is the army allocator's constraint, not a guess from an empty bank.
            label = scene.economies[static_cast<std::size_t>(work.armyIndex)].massIsBinding
                ? "MASS" : "ENERGY";
        }
        const int percent = static_cast<int>(rm::sim::fxToFloat(work.fundedLastTick) * 100);
        const std::string flow = stalled ? "STALLED" : !limited ? "ACTIVE"
            : (percent == 0 ? "<1" : std::to_string(percent)) + "% FUNDED";
        card.rows.push_back({label, flow, limited ? rm::ui::kLoss : rm::ui::kGain});
        return card;
    }
    return std::nullopt;
}

/// The minimap's pips, from the snapshot the renderer is already drawing.
///
/// FROM THE SNAPSHOT, which is the whole reason §7 calls P7.4 "nearly free once a `Map` object
/// and a snapshot exist": before P7.1 the only list of where units were was the renderer's
/// per-batch instance arrays, and a minimap would have needed a second walk over game state that
/// nothing owned.
///
/// A radar contact's colour: enemy red. Every blip is hostile — allies and own
/// units are Seen exactly, neutrals plot nothing — so red says nothing radar
/// did not already say, and it reads instantly against any stratum.
constexpr rm::ui::Colour kBlipColour{0.95f, 0.2f, 0.15f, 0.9f};

/// The blips: what radar and sonar know and sight does not.
///
/// A RED DOT, not a team-coloured pip: the position is known hostile, the rest
/// (whose, what) is withheld. Dots on the map and the minimap alike — the plus
/// read as interface chrome rather than as an enemy.
void appendMinimapBlips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene) {
    const int viewer = scene.viewingAlliance();
    if (viewer == UnitScene::kNoAlliance) {
        return;  // an observer sees units, not guesses about them
    }

    for (const rm::sim::Contact& contact : scene.contactScratch) {
        if (!contact.isBlip()) {
            continue;  // already plotted, in its own colour, where it really is
        }
        out.push_back(rm::ui::MinimapPip{
            .worldX = rm::sim::fxToFloat(contact.x),
            .worldZ = rm::sim::fxToFloat(contact.z),
            .colour = kBlipColour,
            .size = 2.0f,
        });
    }
}

/// The player's own units are drawn a point bigger. A minimap's job is to say where the
/// important things are, and every pip the same size says only "units".
///

namespace {
std::array<float, 4> depositColour(rm::unitdef::BuildRestriction kind) {
    return kind == rm::unitdef::BuildRestriction::MassDeposit
        ? std::array<float, 4>{0.25f, 1.0f, 0.55f, 1.0f}
        : std::array<float, 4>{1.0f, 0.75f, 0.2f, 1.0f};
}
}

void appendUnitSelection(std::vector<rm::DecalVertex>& out, const rm::HeightField& field,
                         std::array<float, 3> centre, float radius, rm::ui::GameProfile profile) {
    if (profile == rm::ui::GameProfile::Fa || profile == rm::ui::GameProfile::ClassicFaf) {
        rm::appendSelectionSquare(out, field, centre, radius, kSelectionRingColour);
    } else {
        rm::appendSelectionRing(out, field, centre, radius, kSelectionRingColour);
    }
}

void appendIntelRings(std::vector<rm::DecalVertex>& out, const rm::HeightField& field,
                      const UnitScene& scene, rm::UnitIndex slot) {
    if (slot >= scene.store.slotCount()) {
        return;
    }
    const rm::sim::UnitCatalog::IntelRadii radii =
        scene.catalog.intel(scene.store.typeAt(slot));
    const rm::sim::Transform& at = scene.store.transforms()[slot];
    const std::array<float, 3> centre{rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                      rm::sim::fxToFloat(at.z)};
    // One ring per live sense; a jammer's deception radius reads the same way.
    const std::array<rm::sim::Fx, 4> reach{radii.radar, radii.sonar, radii.omni,
                                           radii.jamRadius};
    for (const rm::sim::Fx radius : reach) {
        if (radius > rm::sim::Fx{}) {
            rm::appendSelectionRing(out, field, centre, rm::sim::fxToFloat(radius),
                                    kIntelRingColour, kRangeRingThicknessElmos);
        }
    }
}

void appendRallyLine(std::vector<rm::DecalVertex>& out, const rm::HeightField& field,
 const UnitScene& scene, rm::UnitIndex slot) {
    const auto rally = scene.rallyPoints.find(slot);
    if (rally == scene.rallyPoints.end() || slot >= scene.store.slotCount()) {
        return;
    }
    const rm::sim::Transform& at = scene.store.transforms()[slot];
    rm::appendGroundSegment(out, field,
                            {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.z)},
                            {rally->second[0], rally->second[1]}, kRallyLineColour,
                            kRangeRingThicknessElmos);
}

void appendResourceDeposits(std::vector<rm::DecalVertex>& out, const UnitScene& scene,
                            const rm::HeightField& field) {
    for (const auto& deposit : scene.resourceDeposits) {
        const std::array<float, 3> at{rm::sim::fxToFloat(deposit.x), 0.0f,
                                     rm::sim::fxToFloat(deposit.z)};
        // The official shape for a deposit is a square with its corners cut at 45 degrees,
        // not a circle; two concentric outlines keep a deposit distinct from a selected
        // unit's single outline. A 1-elmo stroke, thinner than a selection's, because a
        // deposit is a mark on the map rather than interface asking to be read.
        constexpr float kDepositChamferFraction = 0.4f;
        for (float halfExtent : {8.0f, 12.0f}) {
            rm::appendSelectionChamferedSquare(out, field, at, halfExtent,
                                               depositColour(deposit.kind), 1.0f,
                                               kDepositChamferFraction);
        }
    }
}

std::array<float, 2> snapResourceSite(const UnitScene& scene, rm::UnitTypeIndex type,
                                     std::array<float, 2> at) {
    const auto* def = scene.catalog.def(type);
    if (!def || def->buildRestriction == rm::unitdef::BuildRestriction::None) return at;
    // Four ogrids of cursor tolerance; only the exact centre is sent to the simulation.
    float bestSquared = 32.0f * 32.0f;
    auto result = at;
    for (const auto& deposit : scene.resourceDeposits) {
        if (deposit.kind != def->buildRestriction) continue;
        const float x = rm::sim::fxToFloat(deposit.x), z = rm::sim::fxToFloat(deposit.z);
        const float dx = x - at[0], dz = z - at[1];
        const float distanceSquared = dx * dx + dz * dz;
        if (distanceSquared <= bestSquared) {
            bestSquared = distanceSquared;
            result = {x, z};
        }
    }
    return result;
}

std::array<float, 2> snapBuildSite(const UnitScene& scene, rm::UnitTypeIndex type,
                                   std::array<float, 2> at) {
    const auto snapped = snapResourceSite(scene, type, at);
    const auto* def = scene.catalog.def(type);
    if (!def || scene.placementMode == rm::sim::PlacementMode::Free
        || def->buildRestriction != rm::unitdef::BuildRestriction::None) {
        return snapped;
    }
    return {rm::sim::fxToFloat(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(snapped[0]),
                                                        def->footprintSquaresX)),
            rm::sim::fxToFloat(rm::sim::snapToBuildGrid(rm::sim::fxFromFloat(snapped[1]),
                                                        def->footprintSquaresZ))};
}

void appendMinimapPips(std::vector<rm::ui::MinimapPip>& out, const UnitScene& scene) {
    out.clear();
    out.reserve(scene.snapshotCurrent.size() + scene.resourceDeposits.size());
    for (const auto& deposit : scene.resourceDeposits) {
        out.push_back({rm::sim::fxToFloat(deposit.x), rm::sim::fxToFloat(deposit.z),
                       depositColour(deposit.kind), 4.0f});
    }

    scene.refreshViewerContacts();

    for (const rm::sim::UnitView& unit : scene.snapshotCurrent.units) {
        const int owner = unit.armyIndex;
        const float worldX = rm::sim::fxToFloat(unit.transform.x);
        const float worldZ = rm::sim::fxToFloat(unit.transform.z);

        // WHAT THE MINIMAP SHOWS IS WHAT THE SCREEN SHOWS. A minimap that plotted every unit
        // would make the fog decorative: a player could read the enemy's whole position off
        // the corner of the display and never look at the map.
        if (!scene.visibleToViewer(unit.id)) {
            continue;
        }

        out.push_back(rm::ui::MinimapPip{
            .worldX = worldX,
            .worldZ = worldZ,
            .colour = owner >= 0 && static_cast<std::size_t>(owner) < scene.armies.size()
                          ? rm::teamColour(static_cast<std::size_t>(owner))
                          : rm::kTeamColours[0],
            .size = owner == scene.playerArmy ? 3.0f : 2.0f,
        });
    }

    appendMinimapBlips(out, scene);

    // Alarms younger than the lifetime, newest brightest. White-yellow, bigger
    // than a blip: an alarm is an order to look, not a contact to interpret.
    const int viewer = scene.viewingAlliance();
    for (auto it = scene.alerts.rbegin(); it != scene.alerts.rend(); ++it) {
        if (it->viewer != viewer && viewer != UnitScene::kNoAlliance) {
            continue;
        }
        const std::uint64_t age =
            scene.snapshotCurrent.tick >= it->tick ? scene.snapshotCurrent.tick - it->tick : 0;
        const std::uint64_t budget = static_cast<std::uint64_t>(
            gAppTickRate.ticks(rm::sim::Seconds{UnitScene::kAlertLifetimeSeconds}));
        if (age > budget) {
            break;  // older still: the ring is newest-last, so everything past is older
        }
        out.push_back(rm::ui::MinimapPip{
            .worldX = it->x,
            .worldZ = it->z,
            .colour = rm::ui::Colour{1.0f, 0.9f, 0.45f, 0.95f},
            .size = 3.0f,
        });
    }
}


/// The four ground points the viewport's corners see, for the minimap's view outline.
///
/// THE SAME RAY-TO-GROUND PICK A RIGHT-CLICK USES, so the outline is exact rather than estimated
/// from the camera's distance and pitch. A corner looking past the map's edge or at the sky
/// yields nothing, and the caller draws no outline rather than a wrong one — which is the honest
/// answer for a camera that is not looking at the ground.
void appendViewFootprint(std::vector<std::array<float, 2>>& out, const rm::OrbitCamera& camera,
                         const rm::HeightField& field, const rm::ui::UiViewport& viewport) {
    out.clear();
    const rm::ui::Extent extent = viewport.hudExtent();
    // Clockwise from the top-left, so consecutive pairs are the edges of the shape.
    const std::array<std::array<float, 2>, 4> corners{
        {{0.0f, extent.height}, {extent.width, extent.height}, {extent.width, 0.0f},
         {0.0f, 0.0f}}};
    for (const std::array<float, 2>& corner : corners) {
        const rm::Ray ray =
            rm::screenRay(camera, corner[0], corner[1], extent.width, extent.height);
        const std::optional<simd_float3> ground = rm::pickGround(ray, field);
        if (!ground) {
            out.clear();  // all four or none: a partial outline is a wrong one
            return;
        }
        out.push_back({ground->x, ground->z});
    }
}

/// Reads the scene into the list the build panel draws.
///
/// A TRANSLATION STEP rather than the panel reaching into the scene, which is what lets
/// `core/ui/BuildPanel.hpp` be tested over a `std::span<const BuildOption>` and know nothing
/// about stores, catalogs, rosters or deques. The two halves are tested separately for the
/// same reason they are separate: `tests/test_build_panel.cpp` asks where a cell is,
/// `tests/test_build_options.cpp` asks what belongs in it.
std::optional<bool> submitAutoExpandControl(MatchRunner& runner,
    std::span<const rm::sim::UnitId> selection, const rm::ui::FrameLayout& frame,
    float x, float y) {
    const auto rack = rm::ui::commandRackLayout(frame, !selection.empty());
    const auto slot = rm::ui::commandSlotAt(rack, x, y);
    if (!slot || rm::ui::kCommandDescriptors[*slot].action != rm::ui::RackAction::AutoExpand)
        return std::nullopt;
    std::vector<const rm::unitdef::UnitDef*> definitions;
    for (const auto id : selection) {
        if (runner.scene.store.alive(id))
            definitions.push_back(runner.scene.catalog.def(runner.scene.store.typeAt(id.index)));
    }
    if (!rm::ui::commandAvailability(definitions)[*slot]) return std::nullopt;
    return toggleAutoExpand(runner, selection);
}

std::optional<bool> submitAutoExpandKey(MatchRunner& runner,
    std::span<const rm::sim::UnitId> selection) {
    constexpr std::size_t slot = rm::ui::rackSlotFor(rm::ui::RackAction::AutoExpand);
    std::vector<const rm::unitdef::UnitDef*> definitions;
    for (const auto id : selection) {
        if (runner.scene.store.alive(id))
            definitions.push_back(runner.scene.catalog.def(runner.scene.store.typeAt(id.index)));
    }
    if (!rm::ui::commandAvailability(definitions)[slot]) return std::nullopt;
    return toggleAutoExpand(runner, selection);
}

bool buildSitePlaceableFor(const UnitScene& scene, const rm::HeightField& field,
    PassabilitySet& passability, rm::sim::UnitId builder, rm::UnitTypeIndex type,
    std::array<float, 2> at, float radiusElmos) {
    if (!scene.store.alive(builder)) {
        return false;
    }
    const rm::unitdef::UnitDef* target = scene.catalog.def(type);
    if (target == nullptr) {
        return false;
    }
    const auto builderType = static_cast<std::size_t>(scene.store.typeAt(builder.index));
    const rm::sim::PassabilityGrid& grid =
        passability.gridForBuild(scene, static_cast<std::size_t>(type), builderType);
    if (!scene.terrain(field).resourceSitePlaceable(target->buildRestriction,
            rm::sim::fxFromFloat(at[0]), rm::sim::fxFromFloat(at[1]))) {
        return false;
    }
    return rm::sim::buildSitePlaceable(grid, rm::sim::fxFromFloat(at[0]),
        rm::sim::fxFromFloat(at[1]), rm::sim::fxFromFloat(radiusElmos), scene.store,
        scene.catalog, scene.building);
}

float arrayBuildSitesInto(const UnitScene& scene, rm::UnitTypeIndex type,
    std::array<float, 2> from, std::array<float, 2> to, float spacingScale,
    std::vector<std::array<float, 2>>& sites) {
    sites.clear();
    const auto* def = scene.catalog.def(type);
    if (!def) return 0.0f;
    from = snapBuildSite(scene, type, from);
    to = snapBuildSite(scene, type, to);
    const float grid = rm::sim::fxToFloat(rm::sim::kBuildGridElmos);
    // Skirts are navigation clearance and may be larger than the visible footprint.
    // Packing by them leaves an unexplained one-tile gap between buildings.
    const float width = grid * static_cast<float>(std::max(1, def->footprintSquaresX));
    const float depth = grid * static_cast<float>(std::max(1, def->footprintSquaresZ));
    const float dx = to[0] - from[0], dz = to[1] - from[1];
    const float distance = std::hypot(dx, dz);
    // Two axis-aligned footprints stop overlapping when either pair of edges separates.
    const float alongX = std::abs(dx) > 0 ? width * distance / std::abs(dx) : INFINITY;
    const float alongZ = std::abs(dz) > 0 ? depth * distance / std::abs(dz) : INFINITY;
    const float touching = distance > 0 ? std::min(alongX, alongZ)
                                        : std::min(width, depth);
    const float spacing = touching * std::clamp(spacingScale,
        rm::ui::kArraySpacingMinScale, rm::ui::arraySpacingMaxScale(touching));
    const auto raw = rm::ui::arrayBuildCells(from, to, spacing);
    for (const auto& point : raw) {
        const auto snapped = snapBuildSite(scene, type, point);
        if (!sites.empty()) {
            const float gapX = std::abs(snapped[0] - sites.back()[0]);
            const float gapZ = std::abs(snapped[1] - sites.back()[1]);
            // Grid/deposit snapping may collapse or pull neighbouring raw sites together.
            if (gapX < width && gapZ < depth) continue;
        }
        sites.push_back(snapped);
    }
    return touching;
}

ArrayBuildResult submitArrayBuilds(UnitScene& scene, const rm::HeightField& field,
    PassabilitySet& passability, rm::sim::UnitId builder, rm::UnitTypeIndex type,
    std::span<const std::array<float, 2>> sites, rm::PlayerIndex player,
    rm::TickIndex tick) {
    ArrayBuildResult result;
    const rm::unitdef::UnitDef* def = scene.catalog.def(type);
    const float radius =
        def != nullptr && def->collisionRadiusElmos > 0.0f ? def->collisionRadiusElmos : 4.0f;
    std::optional<std::array<float, 2>> lastSnapped;
    for (const std::array<float, 2>& raw : sites) {
        const std::array<float, 2> snapped = snapBuildSite(scene, type, raw);
        if (lastSnapped && *lastSnapped == snapped) {
            continue;  // the grid collapsed two raw sites onto one cell
        }
        lastSnapped = snapped;
        if (buildSitePlaceableFor(scene, field, passability, builder, type, snapped, radius)
            && issueBuild(scene, builder, player, tick, type, rm::sim::fxFromFloat(snapped[0]),
                          rm::sim::fxFromFloat(snapped[1]), true)) {
            ++result.placed;
        } else {
            ++result.refused;
        }
    }
    return result;
}

bool submitBuildOption(UnitScene& scene, const rm::vfs::Vfs& content,
    rm::sim::UnitId builder, rm::PlayerIndex player, rm::TickIndex tick,
    const rm::ui::BuildOption& option, bool shift) {
    const auto* def = buildCandidateDef(scene, builder);
    if (!def || rm::ui::buildOptionAction(option, rm::unitdef::roleName(rm::unitdef::roleOf(*def)))
                    != rm::ui::BuildOptionAction::SubmitAtBuilder) return false;
    const auto type = resolveBuildable(scene, content, rm::data::RosterEntry{.id = option.id}.path());
    if (!type) return false;
    const auto& at = scene.store.transforms()[builder.index];
    const auto rollOff = option.upgrade ? rm::sim::Fx{}
        : scene.store.motion()[builder.index].radiusElmos * 2;
    return issueBuild(scene, builder, player, tick, *type, at.x, at.z + rollOff,
                      !option.upgrade || shift || option.queuedUpgrade);
}

void followUpgradeSelection(const UnitScene& scene, std::span<rm::sim::UnitId> selection) {
    for (const auto& event : scene.events.all()) {
        if (event.kind == rm::sim::EventKind::UnitFinished
            && event.instigator != rm::sim::UnitId{} && scene.store.alive(event.unit)) {
            std::replace(selection.begin(), selection.end(), event.instigator, event.unit);
        }
    }
}

void gatherBuilderCandidates(const UnitScene& scene,
                             std::span<const rm::sim::UnitId> selection,
                             std::vector<rm::sim::UnitId>& out) {
    out.clear();
    for (const rm::sim::UnitId id : selection) {
        if (buildCandidateDef(scene, id) != nullptr) {
            out.push_back(id);
        }
    }
}

namespace {

/// The walk behind the idle hotkeys: the player army's alive units whose order queue is
/// empty, filtered by the kind of unit the key asks for. Slot order, so two presses of the
/// key select the same units in the same order.
[[nodiscard]] std::vector<rm::sim::UnitId>
idleUnitsOfKind(const UnitScene& scene, bool (*wanted)(const rm::unitdef::UnitDef&)) {
    std::vector<rm::sim::UnitId> found;
    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        if (!scene.store.slotAlive(slot) || scene.armyOf(slot) != scene.playerArmy
            || scene.store.orders()[slot].active() != nullptr) {
            continue;
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def != nullptr && wanted(*def)) {
            found.push_back(scene.store.idAt(slot));
        }
    }
    return found;
}

} // namespace

std::vector<rm::sim::UnitId> idleMobileCombatUnits(const UnitScene& scene) {
    return idleUnitsOfKind(scene, &rm::unitdef::isMobileCombat);
}

std::vector<rm::sim::UnitId> idleFieldEngineers(const UnitScene& scene) {
    return idleUnitsOfKind(scene, [](const rm::unitdef::UnitDef& def) {
        // A field engineer walks to the work: factories build too, but no hotkey needs to
        // go and FIND one.
        return def.isBuilder() && def.speedElmosPerSecond > 0.0f;
    });
}

rm::sim::UnitId activeBuilderFor(std::span<const rm::sim::UnitId> candidates,
                                 rm::sim::UnitId current) noexcept {
    if (std::ranges::find(candidates, current) != candidates.end()) {
        return current;
    }
    return candidates.empty() ? rm::sim::UnitId{} : candidates.front();
}

void gatherBuildOptions(const UnitScene& scene, rm::sim::UnitId activeBuilder,
                         const rm::ui::Theme& theme, std::vector<rm::ui::BuildOption>& out,
                         BuildSelection& who) {
    out.clear();
    who = BuildSelection{};
    if (scene.roster.size() == 0) {
        return;
    }

    // FROM THE ROSTER, NOT FROM `BuildTree` OVER `scene.definitions`, and the first version got
    // this wrong in a way only a screenshot caught. `scene.definitions` holds the types that
    // have been REGISTERED — one spawns, or `resolveBuildable` resolves one for a build order —
    // so a commander's menu came back with a single entry: the extractor the opening had just
    // asked for. Everything a player has not built yet is, by definition, exactly what a build
    // menu is for.
    //
    // Roster carries the full corpus's categories so every menu can use its builder's
    // authored BuildableCategory, even before products have been registered in the scene.
    float storedMass = 0.0f;
    if (scene.playerArmy != rm::sim::kNoArmy
        && static_cast<std::size_t>(scene.playerArmy) < scene.economies.size()) {
        storedMass = rm::sim::magToFloat(
            scene.economies[static_cast<std::size_t>(scene.playerArmy)].stored.mass);
    }

    const rm::unitdef::UnitDef* def = buildCandidateDef(scene, activeBuilder);
    if (def == nullptr) {
        return;
    }
    const rm::unitdef::Role role = rm::unitdef::roleOf(*def);
    // General-purpose builders share the authored build tree; upgrade-only structures
    // expose just their successor below.
    const bool isFactory = role == rm::unitdef::Role::Factory;

    const int army = scene.armyOf(activeBuilder.index);
    if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
        return;
    }
    const rm::sim::Faction faction = scene.armies[static_cast<std::size_t>(army)].faction;

    who = BuildSelection{.builder = activeBuilder,
                         .name = def->name,
                         .role = std::string{rm::unitdef::roleName(role)}};

    // THE TECH PATH, first in the tray because it is the decision a player returns to.
    //
    // `General.UpgradesTo` is a field of its own and belongs to NO `BuildableCategory`, so
    // walking the build tree could never find it: a T1 land factory's own tier two was in
    // the corpus, understood by `startCommand` (which recognises an upgrade by exactly this
    // field) and offered by nothing. The chain continues on its own — the T2 factory's
    // blueprint names the T3 — so this one lookup gives all three tiers as each is reached.
    const rm::unitdef::UnitDef* upgradeFrom = def;
    for (const auto& work : scene.building) {
        if (work.isUpgrade() && !work.finished() && work.upgradeOf == activeBuilder) {
            if (const auto* product = scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex)))
                upgradeFrom = product;
        }
    }
    // Follow already queued upgrade steps too, so the tray never offers the same tier twice.
    for (const auto& entry : scene.store.orders()[activeBuilder.index].entries()) {
        if (entry.kind() != rm::sim::CommandKind::Build) continue;
        const auto* product = scene.catalog.def(entry.buildType());
        if (product && upgradeFrom->upgradesTo == product->name) upgradeFrom = product;
    }
    if (const std::optional<rm::data::RosterEntry> next =
            upgradeFrom->upgradesTo.empty() ? std::nullopt : scene.roster.byId(upgradeFrom->upgradesTo)) {
        const float mass = rm::sim::magToFloat(next->costMass);
        const float seconds = upgradeFrom->buildRate > 0.0f
                                ? rm::sim::magToFloat(next->buildTime) / upgradeFrom->buildRate
                                : 0.0f;
        // NAMED BY ITS TIER, because all three tiers of a land factory are called "Land
        // Factory" in the shipped corpus and a cell reading the same as its neighbour tells
        // a player nothing at all.
        const std::string name =
            (next->description.empty() ? next->id : next->description) + " T"
            + std::to_string(next->tech);
        out.push_back(rm::ui::BuildOption{
            .id = next->id,
            .name = name,
            .massCost = mass,
            .energyCost = rm::sim::magToFloat(next->costEnergy),
            .buildSeconds = seconds,
            .health = rm::sim::magToFloat(next->health),
            .upgrade = true,
            .queuedUpgrade = upgradeFrom != def,
            .affordable = mass <= storedMass,
            .tint = rm::ui::tierTint(theme, next->tech),
        });
    }

    if (isFactory || role == rm::unitdef::Role::Commander || role == rm::unitdef::Role::Builder) {
        auto products = scene.roster.buildableBy(faction, def->buildableCategory);
        // The authored build tree gates tiers. Put newly unlocked units on the first page.
        std::stable_sort(products.begin(), products.end(), [](const auto& a, const auto& b) {
            return a.tech > b.tech;
        });
        for (const rm::data::RosterEntry& entry : products) {
            // ACU blueprints include T2/T3 engineering enhancement categories even at spawn.
            // Enhancement installation is not modelled yet; keep the initial ACU menu at T1.
            if (role == rm::unitdef::Role::Commander && entry.tech > 1) continue;
            if (entry.id == def->upgradesTo) continue; // Already offered as an upgrade.
            const float mass = rm::sim::magToFloat(entry.costMass);
            const float seconds =
                def->buildRate > 0.0f
                    ? rm::sim::magToFloat(entry.buildTime) / def->buildRate
                    : 0.0f;
            out.push_back(rm::ui::BuildOption{
                .id = entry.id,
                .name = "T" + std::to_string(entry.tech) + " "
                    + (entry.description.empty() ? entry.id : entry.description),
                .massCost = mass,
                .energyCost = rm::sim::magToFloat(entry.costEnergy),
                .buildSeconds = seconds,
                .health = rm::sim::magToFloat(entry.health),
                .affordable = mass <= storedMass,
                .tint = rm::ui::tierTint(theme, entry.tech),
            });
        }
    }

    // PRODUCTION ON A FACTORY STILL RISING. Retail lets a player queue units on a factory
    // that has not finished construction; the factory unit does not exist yet, so its
    // products appear in the FOUNDER's tray here and are issued to the founder's queue.
    // `advanceMatch`'s completion hand-over moves them to the factory the moment it stands.
    // Cost and time come from the rising factory's own build rate, which is the rate that
    // will actually drive the work.
    for (const auto& work : scene.building) {
        if (work.finished() || work.isUpgrade() || work.builder != activeBuilder) {
            continue;
        }
        const rm::unitdef::UnitDef* rising =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex));
        if (rising == nullptr || !rising->hasCategory("FACTORY") || rising->isMobile()) {
            continue;
        }
        auto products = scene.roster.buildableBy(faction, rising->buildableCategory);
        std::stable_sort(products.begin(), products.end(), [](const auto& a, const auto& b) {
            return a.tech > b.tech;
        });
        for (const rm::data::RosterEntry& entry : products) {
            const float mass = rm::sim::magToFloat(entry.costMass);
            const float seconds =
                rising->buildRate > 0.0f
                    ? rm::sim::magToFloat(entry.buildTime) / rising->buildRate
                    : 0.0f;
            out.push_back(rm::ui::BuildOption{
                .id = entry.id,
                .name = "T" + std::to_string(entry.tech) + " "
                    + (entry.description.empty() ? entry.id : entry.description),
                .massCost = mass,
                .energyCost = rm::sim::magToFloat(entry.costEnergy),
                .buildSeconds = seconds,
                .health = rm::sim::magToFloat(entry.health),
                .atBuilder = true,
                .affordable = mass <= storedMass,
                .tint = rm::ui::tierTint(theme, entry.tech),
            });
        }
    }
}

void gatherConstructions(UnitScene& scene, const rm::vfs::Vfs& content,
                         const rm::HeightField& field,
                         std::vector<rm::Renderer::ConstructionDraw>& out) {
    out.clear();
    for (const rm::sim::Construction& work : scene.building) {
        // FINISHED WORK STAYS IN THE LIST. `scene.building` is a ledger, not a queue — the
        // sim reports what newly completed and removes nothing, because the census counts a
        // standing extractor by finding its completed row here (`Match.cpp`'s `standingFor`).
        // Drawing those would put a permanent half-built ghost on top of every building the
        // army ever finished.
        if (!constructionInProgress(work)) {
            continue;
        }
        const float x = rm::sim::fxToFloat(work.position[0]);
        const float z = rm::sim::fxToFloat(work.position[2]);
        if (!scene.visibleToViewer(work.position[0], work.position[2])) {
            continue;  // a building the viewer has not seen; drawing it would be free scouting
        }

        // AN UPGRADE IS NOT A SITE. It replaces a building that is already standing and drawn,
        // so a second model in the same place would z-fight with the thing being upgraded.
        // The pad and the stream still mark it — see `appendConstructionEffects`.
        if (work.upgradeOf != rm::sim::UnitId{}) {
            continue;
        }

        const auto type = static_cast<rm::UnitTypeIndex>(work.blueprintIndex);
        const rm::unitdef::UnitDef* def = scene.catalog.def(type);
        if (def == nullptr) {
            continue;
        }

        // The model, loaded on demand. A blueprint under construction has usually never been
        // drawn — that is the whole point — so this is where its batch comes into existence,
        // and the caller must upload before the frame draws. See the frame loop.
        const std::optional<rm::UnitTypeIndex> drawable =
            ensureDrawableType(scene, content, std::string{scene.pathOf(type)});
        if (!drawable) {
            continue;
        }
        const std::size_t batch = scene.batchOf(*drawable);
        if (batch == UnitScene::kNoBatch) {
            continue;
        }

        const int army = work.armyIndex;
        const rm::sim::Faction faction =
            army >= 0 && static_cast<std::size_t>(army) < scene.armies.size()
                ? scene.armies[static_cast<std::size_t>(army)].faction
                : rm::sim::Faction::Uef;

        // The ground decides the height, exactly as `spawnUnit` will when the work completes —
        // the construction's own `y` is always zero and says so in its own comment.
        const float groundY = field.heightAtWorld(x, z);
        const auto typeIndex = static_cast<std::size_t>(*drawable);

        rm::UnitInstance instance{};
        instance.position = {{x, groundY, z}};
        // The SAME facing the finished spawn will get, from the same authority — this used
        // to be a hardcoded zero under a comment claiming spawns face north, which they had
        // long stopped doing, so every building on a diagonal base snapped ~45° at completion.
        instance.rotationY = rm::sim::radiansFromBrad(
            structureFacing(field, work.position[0], work.position[2]));
        instance.scale =
            typeIndex < scene.typeScale.size() ? scene.typeScale[typeIndex] : 1.0f;
        instance.teamColour = army >= 0 && static_cast<std::size_t>(army) < scene.armies.size()
                                ? rm::teamColour(static_cast<std::size_t>(army))
                                : rm::kTeamColours[0];

        out.push_back(rm::Renderer::ConstructionDraw{
            .batch = batch,
            .instance = instance,
            .progress = constructionProgress(work),
            .baseY = groundY,
            // The MESH's height, not the collision box's — a factory's box is the low apron
            // and its mesh is the gantry above it (`UnitDef::meshHeightElmos`).
            .heightElmos = def->meshHeightElmos > 0.0f ? def->meshHeightElmos
                                                        : def->collisionRadiusElmos * 2.0f,
            .style = static_cast<rm::Renderer::BuildStyle>(faction),
            .tint = buildEnergyColour(faction),
        });
    }
}

void appendConstructionEffects(std::vector<rm::DecalVertex>& decals,
                               std::vector<rm::Particle>& particles, const UnitScene& scene,
                               const rm::HeightField& field, float seconds) {
    for (const rm::sim::Construction& work : scene.building) {
        // Only work still in progress — the list keeps completed rows forever; see
        // `gatherConstructions`.
        if (!constructionInProgress(work) || !scene.visibleToViewer(work.position[0], work.position[2])) {
            continue;
        }
        const int army = work.armyIndex;
        const rm::sim::Faction faction =
            army >= 0 && static_cast<std::size_t>(army) < scene.armies.size()
                ? scene.armies[static_cast<std::size_t>(army)].faction
                : rm::sim::Faction::Uef;
        const std::array<float, 4> energy = buildEnergyColour(faction);
        const float x = rm::sim::fxToFloat(work.position[0]);
        const float z = rm::sim::fxToFloat(work.position[2]);
        const float progress = constructionProgress(work);

        // THE PAD, which is what Aeon's units rise out of and what every faction's site stands
        // on. It closes as the work finishes, so a nearly-complete building has a thin collar
        // rather than a halo — the pad's radius is the progress bar nobody has to read.
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex));
        const float radius = def != nullptr && def->collisionRadiusElmos > 0.0f
                               ? def->collisionRadiusElmos
                               : 8.0f;
        const float glow = 0.35f + 0.25f * std::sin(seconds * 4.0f);
        rm::appendSelectionRing(
            decals, field, {{x, 0.0f, z}}, radius * (1.35f - 0.3f * progress),
            {{energy[0], energy[1], energy[2], 0.30f + 0.25f * glow}},
            /*thicknessElmos=*/2.5f);

        // A site can remain allocated while its economy is stalled. The beam marks work that
        // actually advanced during the completed sim tick, not merely an existing command.
        if (!work.advancedLastTick || !scene.store.alive(work.builder)) {
            continue;
        }
        const rm::sim::Transform& from = scene.store.transforms()[work.builder.index];
        const float fx = rm::sim::fxToFloat(from.x);
        const float fy = rm::sim::fxToFloat(from.y);
        const float fz = rm::sim::fxToFloat(from.z);
        const float toY = field.heightAtWorld(x, z) + radius * 0.5f;

        bool emittedUefPair = false;
        const rm::unitdef::UnitDef* builderDef =
            scene.catalog.def(scene.store.typeAt(work.builder.index));
        if (faction == rm::sim::Faction::Uef && def != nullptr && builderDef != nullptr
            && def->meshExtentsXElmos > 0.0f && def->meshExtentsZElmos > 0.0f
            && !builderDef->buildEffectBones.empty()
            && work.builder.index < scene.drawIndexOf.size()) {
            const rm::SelectionEntry where = scene.drawIndexOf[work.builder.index];
            if (where.batch < scene.batches.size() && where.batch < scene.drawScratch.size()
                && where.batch < scene.drawSlotOf.size()
                && where.instance < scene.drawScratch[where.batch].size()
                && where.instance < scene.drawSlotOf[where.batch].size()
                && scene.drawSlotOf[where.batch][where.instance] == work.builder.index) {
                const rm::UnitBatch& batch = scene.batches[where.batch];
                if (batch.model != nullptr) {
                    const rm::UnitInstance& drawn = scene.drawScratch[where.batch][where.instance];
                    const rm::BuildBeamEnds ends = rm::uefBuildBeamEnds(
                        {{x, field.heightAtWorld(x, z), z}}, drawn.position,
                        def->meshExtentsXElmos, def->meshHeightElmos,
                        def->meshExtentsZElmos, progress, seconds);
                    const rm::InstancePlacement placement{
                        .position = drawn.position,
                        .rotationX = drawn.rotationX,
                        .rotationY = drawn.rotationY,
                        .rotationZ = drawn.rotationZ,
                        .scale = drawn.scale,
                    };
                    for (const std::string& name : builderDef->buildEffectBones) {
                        const auto bone = std::find_if(
                            batch.model->bones.begin(), batch.model->bones.end(),
                            [&name](const rm::ModelBone& candidate) {
                                return candidate.name == name;
                            });
                        if (bone == batch.model->bones.end()) {
                            continue;
                        }
                        const std::size_t boneIndex = static_cast<std::size_t>(
                            std::distance(batch.model->bones.begin(), bone));
                        rm::BoneTransform rest{};
                        rest.translation = bone->globalOffset;
                        if (boneIndex < batch.builderAim.boneFlags.size()) {
                            rest.translation = rm::applyBuilderAim(
                                rest.translation, batch.builderAim.boneFlags[boneIndex],
                                batch.builderAim,
                                rm::BuilderAimAngles{.yaw = drawn.builderYaw,
                                                     .pitch = drawn.builderPitch});
                        }
                        const std::array<float, 3> origin =
                            rm::boneWorldPosition(rest, placement);
                        appendBuildBeam(particles, origin, ends.first, energy);
                        appendBuildBeam(particles, origin, ends.second, energy);
                        emittedUefPair = true;
                    }
                    if (emittedUefPair) {
                        for (const std::array<float, 3>& endpoint : {ends.first, ends.second}) {
                            particles.push_back(rm::Particle{
                                .origin = endpoint,
                                .age = 0.02f,
                                .velocity = {{0.0f, 0.0f, 0.0f}},
                                .lifetime = 0.12f,
                                .colour = {{energy[0], energy[1], energy[2], 0.0f}},
                                .size = 4.0f,
                                .growth = 0.0f,
                            });
                        }
                    }
                }
            }
        }

        if (!emittedUefPair) {
            appendBuildBeam(particles, {{fx, fy + 4.0f, fz}}, {{x, toY, z}}, energy);
        }
    }
}

[[nodiscard]] rm::ui::MatchState hudStateFrom(const UnitScene& scene, float elapsedSeconds,
                                               rm::ui::GameProfile profile) {
    rm::ui::MatchState state;
    state.resources = rm::ui::resourceViews(profile, {}, {});
    state.elapsedSeconds = elapsedSeconds;
    state.armiesTotal = scene.armies.size();
    state.armiesLeft = rm::sim::survivorCount(scene.armies);

    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();
    for (std::size_t slot = 0; slot < health.size(); ++slot) {
        if (health[slot].alive()
            && (scene.playerArmy == rm::sim::kNoArmy
                || (slot < motion.size() && motion[slot].armyIndex == scene.playerArmy))) {
            ++state.unitsAlive;
        }
    }

    if (scene.playerArmy != rm::sim::kNoArmy
        && static_cast<std::size_t>(scene.playerArmy) < scene.economies.size()) {
        const rm::sim::Economy& mine =
            scene.economies[static_cast<std::size_t>(scene.playerArmy)];

        // The HUD reads in floats and per SECOND — it is a display, and a player thinks in
        // seconds. This is the sim-to-renderer half of the float boundary (`Fx.hpp`).
        const auto perSecond = [](rm::sim::Mag perTick) {
            return rm::sim::magToFloat(perTick)
                   * static_cast<float>(gAppTickRate.ticksPerSecond());
        };
        // Net flow includes every funded request (construction, repair, ammo and upkeep).
        // Maintenance alone can show a surplus while construction consumes all income.
        state.resources = rm::ui::resourceViews(
            profile,
            rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.mass),
                          .capacity = rm::sim::magToFloat(mine.storage.mass),
                          .incomePerSecond = perSecond(mine.incomePerTick.mass),
                          .drainPerSecond = perSecond(mine.usageLastTick.mass)},
            rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.energy),
                          .capacity = rm::sim::magToFloat(mine.storage.energy),
                          .incomePerSecond = perSecond(mine.incomePerTick.energy),
                          .drainPerSecond = perSecond(mine.usageLastTick.energy)});
        state.fundedFraction = rm::sim::fxToFloat(mine.fundedFraction);
    }

    if (!scene.armies.empty() && state.armiesLeft <= 1) {
        const std::optional<int> winner = rm::sim::winningAlliance(scene.armies);
        state.outcome = winner ? rm::ui::MatchState::Outcome::Win
                               : rm::ui::MatchState::Outcome::Draw;
        state.winningTeam = winner.value_or(0);
    }
    return state;
}

[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene, rm::ui::GameProfile profile,
                                         const rm::ui::PanelSkin& skin) {
    rm::ui::Theme theme = rm::ui::neutralTheme();
    const rm::ui::GameProfileDescriptor descriptor = rm::ui::gameProfile(profile);
    if (descriptor.factionOwned) {
        for (const rm::sim::Army& army : scene.armies) {
            if (army.index == scene.playerArmy) {
                theme = rm::ui::themeFor(army.faction);
                break;
            }
        }
    }
    if (profile == rm::ui::GameProfile::Bar) {
        theme = rm::ui::barTheme();
    }
    if (descriptor.classicChrome) {
        if (skin.active) {
            // Complete classic chrome rides the faction livery; partial/missing art never does.
            theme.skin = skin;
        } else {
            theme = rm::ui::neutralTheme();
        }
    }
    return theme;
}

/// Appends an icon for every unit in the scene too small to read, at the camera's current
/// zoom. See core/scene/UnitIcons.hpp.
///
/// Shared by the windowed loop and the capture paths, because a screenshot that did not show
/// the icons would make the feature unverifiable — and AGENT.md asks for a screenshot or it
/// did not happen.
void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera,
                      std::span<const std::optional<StrategicIconRef>> refs) {
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    std::vector<float> radii;
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        radii.clear();
        radii.reserve(scene.drawSlotOf[batch].size());
        for (const rm::UnitIndex slot : scene.drawSlotOf[batch]) {
            // A type the strategic layer drew is passed a zero radius, which
            // `appendUnitIcons` reads as "skip" — the square is the fallback for the
            // glyphless, not a backing plate for everyone.
            const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
            const bool drawnAsGlyph = type < refs.size() && refs[type].has_value();
            radii.push_back(drawnAsGlyph
                                ? 0.0f
                                : rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos));
        }
        (void)rm::appendUnitIcons(into, scene.drawScratch[batch], radii, elmosPerPoint);
    }
}

void appendStrategicIcons(rm::ui::Geometry& out, const UnitScene& scene,
                          const rm::OrbitCamera& camera, const rm::ui::UiViewport& viewport,
                          std::span<const std::optional<StrategicIconRef>> refs) {
    const rm::ui::Extent extent = viewport.hudExtent();
    if (refs.empty() || !(extent.width > 0.0f) || !(extent.height > 0.0f)) {
        return;
    }
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    if (!(elmosPerPoint > 0.0f)) {
        return;
    }

    // The glyphs ship at their reading size for a ~1000-point view; scaling by the actual
    // viewport keeps them the same fraction of the screen on every window, exactly as the
    // square fallback's point size behaves.
    const float scale = extent.height / rm::kIconReferenceHeightPoints;

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        for (std::size_t i = 0; i < scene.drawScratch[batch].size()
                                && i < scene.drawSlotOf[batch].size(); ++i) {
            const rm::UnitIndex slot = scene.drawSlotOf[batch][i];
            const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
            if (type >= refs.size() || !refs[type]) {
                continue;  // no glyph: the square fallback owns this one
            }
            const float radius = rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos);
            if (radius <= 0.0f
                || rm::apparentPoints(radius, elmosPerPoint) >= rm::kIconThresholdPoints) {
                continue;  // the mesh still reads; a glyph over it would be noise on a tank
            }

            const rm::UnitInstance& unit = scene.drawScratch[batch][i];
            const auto screen = rm::worldToScreen(
                camera,
                simd_make_float3(unit.position[0], unit.position[1], unit.position[2]),
                extent.width, extent.height);
            if (!screen) {
                continue;
            }

            const StrategicIconRef& ref = *refs[type];
            const float w = static_cast<float>(ref.width) * scale;
            const float h = static_cast<float>(ref.height) * scale;
            const float x0 = (*screen)[0] - w * 0.5f;
            const float y0 = (*screen)[1] - h * 0.5f;
            const float x1 = x0 + w;
            const float y1 = y0 + h;

            // The glyph is white artwork; the army's colour arrives as the tint the image
            // shader multiplies in — the same identity story the pips and the old squares
            // tell, now wearing a shape.
            const rm::ui::IconUv uv = rm::ui::iconUvSized(ref.slot, ref.width, ref.height);
            const std::array<float, 4> tint{unit.teamColour[0], unit.teamColour[1],
                                            unit.teamColour[2], 0.95f};
            out.worldOverlay.image.push_back({{x0, y0}, {uv.u0, uv.v0}, tint});
            out.worldOverlay.image.push_back({{x1, y0}, {uv.u1, uv.v0}, tint});
            out.worldOverlay.image.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.worldOverlay.image.push_back({{x0, y0}, {uv.u0, uv.v0}, tint});
            out.worldOverlay.image.push_back({{x1, y1}, {uv.u1, uv.v1}, tint});
            out.worldOverlay.image.push_back({{x0, y1}, {uv.u0, uv.v1}, tint});
        }
    }
}

void appendContactBlips(rm::ui::Geometry& out, const UnitScene& scene,
                         const rm::OrbitCamera& camera, const rm::HeightField& field,
                         const rm::text::Font& font, const rm::ui::UiViewport& viewport) {
    const rm::ui::Extent extent = viewport.hudExtent();
    if (!font.usable() || !(extent.width > 0.0f) || !(extent.height > 0.0f)) {
        return;
    }
    scene.refreshViewerContacts();
    // A filled dot, five points across: an enemy position, nothing more. The plus
    // read as interface chrome; a red dot reads as an enemy.
    constexpr float kDot = 5.0f;
    for (const rm::sim::Contact& contact : scene.contactScratch) {
        if (!contact.isBlip()) {
            continue;
        }
        const float x = rm::sim::fxToFloat(contact.x);
        const float z = rm::sim::fxToFloat(contact.z);
        const auto screen = rm::worldToScreen(
            camera, simd_make_float3(x, field.heightAtWorld(x, z) + 2.0f, z), extent.width,
            extent.height);
        if (!screen) {
            continue;
        }
        rm::text::appendRect(out.worldOverlay.solid, font, (*screen)[0] - kDot * 0.5f,
                             (*screen)[1] - kDot * 0.5f, kDot, kDot, kBlipColour);
    }
}

std::size_t appendConstructionBars(rm::ui::Geometry& out, const UnitScene& scene,
    const rm::OrbitCamera& camera, const rm::HeightField& field, const rm::text::Font& font,
    const rm::ui::UiViewport& viewport, std::span<const rm::sim::UnitId> selected,
    std::optional<std::array<float, 2>> cursor) {
    const auto extent = viewport.hudExtent();
    if (!font.usable() || extent.width <= 0 || extent.height <= 0) return 0;
    const float elmosPerPoint = camera.elmosPerPoint(extent.height);
    if (elmosPerPoint <= 0) return 0;
    const auto battlefield = rm::ui::frameLayout(viewport).battlefield;
    std::size_t drawn = 0;
    for (const auto& work : scene.building) {
        if (!constructionInProgress(work) || !scene.store.alive(work.builder)
            || !scene.store.health()[work.builder.index].alive()
            || !scene.visibleToViewer(work.position[0], work.position[2])) continue;
        const auto* def = scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex));
        if (!def) continue;
        const float x = rm::sim::fxToFloat(work.position[0]);
        const float z = rm::sim::fxToFloat(work.position[2]);
        const float ground = field.heightAtWorld(x, z);
        const auto base = rm::worldToScreen(camera, simd_make_float3(x, ground, z),
            extent.width, extent.height);
        if (!base) continue;
        const float radius = std::max(12.0f, def->collisionRadiusElmos / elmosPerPoint);
        const bool hovered = cursor && battlefield.contains((*cursor)[0], (*cursor)[1])
            && std::hypot((*cursor)[0] - (*base)[0], (*cursor)[1] - (*base)[1]) <= radius;
        if (!hovered && std::ranges::find(selected, work.builder) == selected.end()
            && std::ranges::find(selected, work.upgradeOf) == selected.end()) continue;
        const auto top = rm::worldToScreen(camera,
            simd_make_float3(x, ground + std::max(def->meshHeightElmos,
                def->collisionRadiusElmos * 2), z), extent.width, extent.height);
        if (!top || !battlefield.contains((*top)[0], (*top)[1])) continue;
        // A short cyan work bar stays distinct from green health and survives inspector hover.
        constexpr float width = 40.0f, height = 4.0f;
        const float left = (*top)[0] - width / 2;
        const float y = (*top)[1] - 12.0f;
        rm::text::appendRect(out.worldOverlay.solid, font, left, y, width, height,
            rm::ui::Colour{{0, 0, 0, 0.8f}});
        rm::text::appendRect(out.worldOverlay.solid, font, left, y,
            width * constructionProgress(work), height, rm::ui::Colour{{0.2f, 0.75f, 1, 1}});
        ++drawn;
    }
    return drawn;
}

void appendHealthBars(rm::ui::Geometry& out, const UnitScene& scene,
                      const rm::OrbitCamera& camera, const rm::text::Font& font,
                      const rm::ui::UiViewport& viewport) {
    const rm::ui::Extent extent = viewport.hudExtent();
    if (!font.usable() || !(extent.width > 0.0f) || !(extent.height > 0.0f)) {
        return;
    }

    // The icon threshold, inverted: icons take over when a unit shrinks past reading
    // (UnitIcons.hpp), and the bar stops where they start — one regime per zoom, never both.
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    if (!(elmosPerPoint > 0.0f)) {
        return;
    }

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        for (std::size_t i = 0; i < scene.drawScratch[batch].size()
                                && i < scene.drawSlotOf[batch].size(); ++i) {
            const rm::UnitIndex slot = scene.drawSlotOf[batch][i];
            const rm::sim::Health& hp = scene.store.health()[slot];
            const bool hasShield = hp.shield.maximum > rm::sim::Mag{};
            if (!hp.alive() || (hp.current >= hp.maximum && !hasShield)) {
                continue;
            }

            const float radiusElmos =
                rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos);
            const float radiusPoints = radiusElmos / elmosPerPoint;
            if (radiusPoints * 2.0f < rm::kIconThresholdPoints) {
                continue;  // icon territory: the strategic layer owns this zoom
            }

            const rm::UnitInstance& unit = scene.drawScratch[batch][i];
            const auto screen = rm::worldToScreen(
                camera,
                simd_make_float3(unit.position[0], unit.position[1], unit.position[2]),
                extent.width, extent.height);
            if (!screen) {
                continue;
            }

            // Sized with the unit but clamped: a bar narrower than ~16 points is unreadable
            // and one wider than ~48 reads as interface chrome rather than a unit's.
            const float barWidth = std::clamp(radiusPoints * 2.2f, 16.0f, 48.0f);
            const float barHeight = 3.0f;
            const float x = (*screen)[0] - barWidth * 0.5f;
            const float y = (*screen)[1] - radiusPoints - 8.0f;

            if (hasShield) {
                const float shieldFill = rm::sim::magToFloat(hp.shield.current)
                                         / rm::sim::magToFloat(hp.shield.maximum);
                constexpr rm::ui::Colour kShield{{0.2f, 0.75f, 1.0f, 0.95f}};
                rm::text::appendRect(out.worldOverlay.solid, font, x, y - 4.0f, barWidth,
                                     barHeight,
                                     rm::ui::Colour{{0.0f, 0.0f, 0.0f, 0.55f}});
                rm::text::appendRect(out.worldOverlay.solid, font, x, y - 4.0f,
                                     barWidth * std::clamp(shieldFill, 0.0f, 1.0f), barHeight,
                                     kShield);
            }

            if (hp.current < hp.maximum) {
                const float fill =
                    rm::sim::magToFloat(hp.current) / rm::sim::magToFloat(hp.maximum);
                const rm::ui::Colour bar = fill > 0.6f ? rm::ui::kGain
                                           : (fill > 0.3f ? rm::ui::kWarn : rm::ui::kLoss);
                rm::text::appendRect(out.worldOverlay.solid, font, x, y, barWidth, barHeight,
                                     rm::ui::Colour{{0.0f, 0.0f, 0.0f, 0.55f}});
                rm::text::appendRect(out.worldOverlay.solid, font, x, y,
                                     barWidth * std::clamp(fill, 0.0f, 1.0f), barHeight, bar);
            }
        }
    }
}

/// The unit nearest the ray across every batch, IGNORING who owns it.
///
/// What a right-click wants: an order aimed at an enemy has to be able to find one, and
/// the selection pick deliberately cannot.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAnyBatch(const rm::Ray& ray,
                                                          const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    // Against what is DRAWN, because that is what the ray can see — a dead unit is not in
    // the gather and so cannot be clicked. The hit comes back as a (batch, index) pair and
    // is turned into a handle, which is what everything downstream now speaks.
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }
        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = scene.unitDrawnAt(batch, *hit);
    }
    return best;
}

[[nodiscard]] bool orderHitConfirmed(const rm::Ray& ray, std::array<float, 3> at,
                                     float radiusElmos) noexcept {
    const float gap = rm::distanceToRay(ray, simd_make_float3(at[0], at[1], at[2]));
    return gap <= std::max(kOrderPickFloorElmos, radiusElmos);
}

[[nodiscard]] std::optional<rm::sim::UnitId> siteAssistFounder(const UnitScene& scene, float x,
                                                               float z, int army) noexcept {
    std::optional<rm::sim::UnitId> best;
    float bestGap = std::numeric_limits<float>::max();
    for (const rm::sim::Construction& work : scene.building) {
        if (work.finished() || work.armyIndex != army
            || !scene.store.alive(work.builder)) {
            continue;
        }
        // The scaffold's own size: the rising structure's footprint, with a floor
        // for small buildings so a mine is still clickable.
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(work.blueprintIndex));
        const float radius =
            std::max(12.0f, def != nullptr ? def->collisionRadiusElmos : 0.0f);
        const float dx = x - rm::sim::fxToFloat(work.position[0]);
        const float dz = z - rm::sim::fxToFloat(work.position[2]);
        const float gap = std::sqrt(dx * dx + dz * dz);
        if (gap <= radius && gap < bestGap) {
            best = work.builder;
            bestGap = gap;
        }
    }
    return best;
}

/// Whether `army` may shoot what `entry` points at.
[[nodiscard]] bool hostileTo(const UnitScene& scene, int army, rm::sim::UnitId id) {
    const int theirs = scene.armyOf(id.index);
    if (army == rm::sim::kNoArmy || theirs == rm::sim::kNoArmy) {
        return false;
    }
    const auto find = [&scene](int index) -> const rm::sim::Army* {
        for (const rm::sim::Army& candidate : scene.armies) {
            if (candidate.index == index) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const rm::sim::Army* mine = find(army);
    const rm::sim::Army* target = find(theirs);
    return mine != nullptr && target != nullptr && rm::sim::hostile(*mine, *target);
}

[[nodiscard]] bool alliedTo(const UnitScene& scene, int army, rm::sim::UnitId id) {
    const int theirs = scene.armyOf(id.index);
    if (army == rm::sim::kNoArmy || theirs == rm::sim::kNoArmy) {
        return false;
    }
    const auto find = [&scene](int index) -> const rm::sim::Army* {
        for (const rm::sim::Army& candidate : scene.armies) {
            if (candidate.index == index) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const rm::sim::Army* mine = find(army);
    const rm::sim::Army* target = find(theirs);
    return mine != nullptr && target != nullptr && rm::sim::allied(*mine, *target);
}

/// The unit nearest the ray across every batch, or nothing.
///
/// pickUnit searches one array at a time because that is how the instances are
/// held — one per model. Comparing its winners across batches is what makes the
/// nearest unit on SCREEN win, rather than the nearest one in whichever model
/// happened to load first.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAcrossBatches(const rm::Ray& ray,
                                                              const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }

        // A unit belongs to whoever the sim says, and only the player's own may be
        // selected. Filtered HERE rather than inside pickUnit, which is pure over
        // instances and has no business knowing about armies.
        //
        // Note what this deliberately does NOT do: it does not skip an enemy and keep
        // looking behind it. Clicking an enemy selects nothing, because that is what
        // was clicked — the alternative reaches through the thing under the cursor to
        // grab something else, which is worse than a null answer.
        const std::optional<rm::sim::UnitId> id = scene.unitDrawnAt(batch, *hit);
        if (!id) {
            continue;
        }
        if (scene.playerArmy != rm::sim::kNoArmy
            && scene.armyOf(id->index) != scene.playerArmy) {
            continue;
        }

        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = id;
    }

    return best;
}


/// The archive path for a unit's icon, derived from its id.
///
/// ONE PLACE, because two panels need it and a path spelled twice is a path that can be spelled
/// differently. The corpus's own layout, and the same reasoning `RosterEntry::path` gives for
/// deriving rather than storing.
[[nodiscard]] std::string iconPathFor(const std::string& id) {
    return "/textures/ui/common/icons/units/" + id + "_icon.dds";
}

/// One unit's icon out of the archives, or an empty texture.
///
/// EMPTY IS THE ORDINARY ANSWER and it is not an error: `UEB5208` ships no icon at all, measured
/// over `textures.scd`. `packIcons` skips an empty input and leaves its slot blank, which is
/// what keeps the packing positional.
[[nodiscard]] rm::dds::Texture iconFor(const rm::vfs::Vfs& content, const std::string& id) {
    const auto bytes = content.read(iconPathFor(id));
    if (!bytes) {
        return {};
    }
    auto icon = rm::dds::load(*bytes);
    return icon ? std::move(*icon) : rm::dds::Texture{};
}

void ensureStrategicIconArt(UnitScene& scene, const rm::vfs::Vfs& content) {
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(type));
        if (def == nullptr || def->strategicIcon.empty()
            || scene.strategicIconMissing.contains(def->strategicIcon)) {
            continue;
        }
        const auto cached = std::find_if(
            scene.strategicIconArt.begin(), scene.strategicIconArt.end(),
            [&](const auto& entry) { return entry.first == def->strategicIcon; });
        if (cached != scene.strategicIconArt.end()) {
            continue;
        }

        // The `_rest` state: the glyph at rest is the map's own layer; `_over` and
        // `_selected` belong to a hover-and-selection story the strategic layer does not
        // tell yet.
        const std::string path = "/textures/ui/common/game/strategicicons/"
                                 + def->strategicIcon + "_rest.dds";
        const auto bytes = content.read(path);
        bool loaded = false;
        if (bytes) {
            if (auto icon = rm::dds::load(*bytes); icon && !icon->data.empty()) {
                scene.strategicIconArt.emplace_back(def->strategicIcon, std::move(*icon));
                loaded = true;
            }
        }
        if (!loaded) {
            scene.strategicIconMissing.insert(def->strategicIcon);
        }
    }
}

void buildStrategicIconRefs(const UnitScene& scene, std::size_t base,
                            std::vector<std::optional<StrategicIconRef>>& out) {
    out.assign(scene.catalog.size(), std::nullopt);
    for (std::size_t type = 0; type < scene.catalog.size(); ++type) {
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(type));
        if (def == nullptr || def->strategicIcon.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < scene.strategicIconArt.size(); ++i) {
            const auto& [name, art] = scene.strategicIconArt[i];
            if (name == def->strategicIcon) {
                out[type] = StrategicIconRef{
                    .slot = base + i, .width = art.width, .height = art.height};
                break;
            }
        }
    }
}

PackedInterfaceAtlas packInterfaceIcons(
    const rm::vfs::Vfs& content, std::vector<rm::ui::BuildOption>& options,
    std::vector<rm::ui::RosterTile>& tiles, VisibleIconRange visibleBuild,
    VisibleIconRange visibleRoster, rm::ui::GameProfile profile,
    std::span<const std::pair<std::string, rm::dds::Texture>> strategic,
    std::size_t* strategicBase) {
    std::vector<rm::dds::Texture> icons;
    icons.reserve(visibleBuild.count + visibleRoster.count + strategic.size() + 9);

    // ONE NUMBERING ACROSS BOTH PANELS. A slot is an index into `icons`, and `packIcons` places
    // by that index, so appending the roster's after the tray's is all "one atlas" needs to be
    // true — there is no second base to add and get wrong.
    for (rm::ui::BuildOption& option : options) {
        option.iconSlot.reset();
    }
    const std::size_t buildEnd =
        std::min(options.size(), visibleBuild.first + visibleBuild.count);
    for (std::size_t index = std::min(visibleBuild.first, options.size()); index < buildEnd;
         ++index) {
        rm::ui::BuildOption& option = options[index];
        rm::dds::Texture icon = iconFor(content, option.id);
        if (!icon.data.empty()) {
            option.iconSlot = icons.size();
        }
        icons.push_back(std::move(icon));
    }
    for (rm::ui::RosterTile& tile : tiles) {
        tile.iconSlot.reset();
    }
    const std::size_t rosterEnd =
        std::min(tiles.size(), visibleRoster.first + visibleRoster.count);
    for (std::size_t index = std::min(visibleRoster.first, tiles.size()); index < rosterEnd;
         ++index) {
        rm::ui::RosterTile& tile = tiles[index];
        rm::dds::Texture icon = iconFor(content, tile.id);
        if (!icon.data.empty()) {
            tile.iconSlot = icons.size();
        }
        icons.push_back(std::move(icon));
    }

    // The strategic glyphs after both panels' icons, in the cache's own order — so `base +
    // cache index` is the slot, and the caller needs nothing but the base to map them.
    if (strategicBase != nullptr) {
        *strategicBase = icons.size();
    }
    for (const auto& [name, art] : strategic) {
        icons.push_back(art);  // a copy per pack; a glyph is ~100 bytes of BC3 blocks
    }

    // The skin's nine slices, last, when `--ui faf` asked for the game's own chrome. The
    // atlas is the ride every icon already takes; nine more squares cost nothing and spare
    // the renderer a second texture bind it has no slot for.
    rm::ui::PanelSkin skin;
    std::array<std::size_t, 9> skinSlots{};
    std::array<std::array<int, 2>, 9> skinSizes{};
    bool skinComplete = profile == rm::ui::GameProfile::ClassicFaf;
    if (skinComplete) {
        constexpr std::array<const char*, 9> kPieces{
            "generic_brd_ul",      "generic_brd_horz_um", "generic_brd_ur",
            "generic_brd_vert_l",  "generic_brd_m",       "generic_brd_vert_r",
            "generic_brd_ll",      "generic_brd_horz_lm", "generic_brd_lr"};
        for (std::size_t piece = 0; piece < kPieces.size(); ++piece) {
            const std::string path = std::string{"/textures/ui/common/game/generic_brd/"}
                                     + kPieces[piece] + ".dds";
            const std::optional<std::vector<std::byte>> bytes = content.read(path);
            const auto art = bytes ? rm::dds::load(*bytes)
                                   : std::expected<rm::dds::Texture, rm::MapError>{
                                         std::unexpect, rm::MapError{}};
            if (!art || art->width == 0) {
                skinComplete = false;
                break;
            }
            skinSlots[piece] = icons.size();
            skinSizes[piece] = {static_cast<int>(art->width), static_cast<int>(art->height)};
            icons.push_back(*art);
        }
    }


    assert(icons.size() <= rm::ui::kAtlasCapacity
           && "visible interface icons exceed the fixed atlas");

    rm::dds::Texture atlas = rm::ui::packIcons(icons);
    if (skinComplete && !atlas.data.empty()) {
        for (std::size_t piece = 0; piece < 9; ++piece) {
            skin.uv[piece] = rm::ui::iconUvSized(skinSlots[piece], skinSizes[piece][0],
                                                 skinSizes[piece][1]);
            skin.size[piece] = {static_cast<float>(skinSizes[piece][0]),
                                static_cast<float>(skinSizes[piece][1])};
        }
        skin.active = true;
        std::printf("  ui: FAF chrome packed (generic_brd, nine slices)\n");
    }
    if (atlas.data.empty()) {
        // Nothing packed: no cell may point into an empty atlas, or it draws whatever the
        // sampler makes of a texture that is not bound.
        for (rm::ui::BuildOption& option : options) {
            option.iconSlot.reset();
        }
        for (rm::ui::RosterTile& tile : tiles) {
            tile.iconSlot.reset();
        }
    }
    return {.texture = std::move(atlas), .skin = skin};
}

void gatherRoster(const UnitScene& scene, std::span<const rm::sim::UnitId> selection,
                  std::vector<rm::ui::RosterTile>& out) {
    out.clear();

    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::vector<float> health;
    std::vector<float> maxHealth;
    ids.reserve(selection.size());
    names.reserve(selection.size());
    health.reserve(selection.size());
    maxHealth.reserve(selection.size());

    for (const rm::sim::UnitId id : selection) {
        if (!scene.store.alive(id)) {
            continue;  // a selection outlives the units in it
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(id.index));
        if (def == nullptr) {
            continue;
        }
        const rm::sim::Health& hp = scene.store.health()[id.index];
        ids.push_back(def->name);
        names.push_back(def->description);
        health.push_back(rm::sim::magToFloat(hp.current));
        maxHealth.push_back(rm::sim::magToFloat(hp.maximum));
    }

    out = rm::ui::groupSelection(ids, health, maxHealth, names);
    const float hz = static_cast<float>(gAppTickRate.ticksPerSecond());
    for (const auto id : selection) {
        if (!scene.store.alive(id) || id.index >= scene.resourceFlows.size()) continue;
        const auto& flow = scene.resourceFlows[id.index];
        const auto* def = scene.catalog.def(scene.store.typeAt(id.index));
        if (flow.unit != id || def == nullptr) continue;
        auto tile = std::ranges::find(out, def->name, &rm::ui::RosterTile::id);
        if (tile == out.end()) continue;
        if (!tile->resourceRates) tile->resourceRates.emplace();
        auto& rates = *tile->resourceRates;
        rates[0].incomePerSecond += rm::sim::magToFloat(flow.incomePerTick.mass) * hz;
        rates[1].incomePerSecond += rm::sim::magToFloat(flow.incomePerTick.energy) * hz;
        rates[0].drainPerSecond += rm::sim::magToFloat(flow.usageLastTick.mass) * hz;
        rates[1].drainPerSecond += rm::sim::magToFloat(flow.usageLastTick.energy) * hz;
    }
}

rm::ui::InfoCard selectedUnitCard(const UnitScene& scene, const rm::ui::RosterTile& tile,
                                 rm::sim::UnitId activeBuilder) {
    auto card = rm::ui::rosterTileCard(tile);
    const auto* def = scene.store.alive(activeBuilder)
        ? scene.catalog.def(scene.store.typeAt(activeBuilder.index)) : nullptr;
    if (def && def->name == tile.id) {
        if (const auto work = constructionCard(scene, activeBuilder)) {
            card.progress = work->progress;
            card.corner = work->rows.back().value + " " + work->corner;
        }
    }
    return card;
}

std::optional<rm::ui::ProductionView> gatherProduction(const UnitScene& scene,
                                                       rm::sim::UnitId builder) {
    if (!scene.store.alive(builder)) {
        return std::nullopt;
    }
    const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(builder.index));
    if (def == nullptr || !def->isBuilder() || def->isMobile()) {
        return std::nullopt;
    }

    rm::ui::ProductionView view;
    view.factoryId = def->name;
    view.factoryName = def->description.empty() ? def->name : def->description;
    view.repeat = scene.store.factoryRepeat(builder);
    view.canRepeat = def->hasCategory("FACTORY");

    // The queue as the sim holds it, current first; only Build orders are production. A
    // shared order's remaining count is the stack a player shift-clicked.
    for (const rm::sim::QueuedCommand& entry : scene.store.orders()[builder.index].entries()) {
        if (entry.kind() != rm::sim::CommandKind::Build) {
            continue;
        }
        const rm::sim::SharedCommand& order = entry.payload();
        const rm::unitdef::UnitDef* product = scene.catalog.def(order.buildType);
        rm::ui::ProductionEntry row;
        row.id = product != nullptr ? product->name : std::to_string(order.buildType);
        row.name = product != nullptr && !product->description.empty() ? product->description
                                                                        : row.id;
        if (!view.canRepeat && product)
            row.name = "T" + std::to_string(rm::unitdef::techOf(*product)) + " " + row.name;
        row.count = std::max<std::uint32_t>(1, order.remainingCount);
        row.commandId = order.id;
        view.queue.push_back(std::move(row));
    }

    // The construction this factory is running, if any — matched by builder, the same link
    // an Assist resolves through.
    for (const rm::sim::Construction& work : scene.building) {
        if (work.builder == builder && constructionInProgress(work)) {
            view.building = true;
            view.progress = constructionProgress(work);
            // Per second, as the HUD reads every rate (`Fx.hpp`'s float boundary).
            view.assistRate = rm::sim::magToFloat(work.assistPerTick)
                            * static_cast<float>(gAppTickRate.ticksPerSecond());
            break;
        }
    }
    if (!view.canRepeat && view.queue.empty() && !view.building) return std::nullopt;
    return view;
}

bool submitProductionControl(UnitScene& scene, rm::sim::UnitId builder,
    rm::PlayerIndex player, rm::TickIndex tick, const rm::ui::FrameLayout& frame,
    float x, float y, std::size_t page) {
    const auto view = gatherProduction(scene, builder);
    if (!view) return false;
    const auto rect = rm::ui::productionPanelRect(frame);
    if (const auto command = rm::ui::productionCancelAt(rect, *view, x, y, page)) {
        return issueCancelFactoryBuild(scene, builder, player, tick, *command);
    }
    const auto kind = rm::ui::productionCommandAt(rect, *view, x, y);
    if (!kind) return false;
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = *kind,
        .units = {builder},
    }).has_value();
}

} // namespace rm::app
