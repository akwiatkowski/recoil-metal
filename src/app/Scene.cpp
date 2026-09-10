#include "app/Scene.hpp"

#include "core/unit/UnitBlueprint.hpp"

#include <algorithm>
#include <cstdio>

namespace rm::app {

// Defined here, declared `extern` in the header — see the note there.
rm::sim::TickRate gAppTickRate{rm::sim::kDefaultTicksPerSecond};
bool gInterpolate = true;

/// Builds the roster from the whole blueprint corpus.
///
/// EVERY unit, once, at scene setup — because "which is the T1 extractor for this faction" is a
/// question about the whole set and cannot be answered from one file (`core/data/Roster.hpp`).
/// The 568 parses cost a few tens of milliseconds against a scene load that already reads
/// hundreds of meshes and textures.
[[nodiscard]] rm::data::Roster buildRoster(const rm::vfs::Vfs& content) {
    std::vector<rm::unitdef::UnitDef> defs;
    std::vector<std::string> ids;
    for (const std::string& path : content.list("/units", ".bp")) {
        if (!path.ends_with("_unit.bp")) {
            continue;
        }
        const std::optional<std::vector<std::byte>> bytes = content.read(path);
        if (!bytes) {
            continue;
        }
        const std::string_view source{reinterpret_cast<const char*>(bytes->data()),
                                      bytes->size()};
        auto def = rm::unitbp::load(source, path);
        if (!def) {
            continue;  // a blueprint this engine cannot read is not in the roster, and that is
                       // reported by the roster being short rather than by failing the load
        }
        for (rm::unitdef::Weapon& weapon : def->weapons) {
            if (!weapon.countedProjectile || weapon.projectileId.empty()) {
                continue;
            }
            const auto projectile = content.read(weapon.projectileId);
            if (!projectile) {
                continue;
            }
            const std::string_view projectileSource{
                reinterpret_cast<const char*>(projectile->data()), projectile->size()};
            if (auto traits = rm::unitbp::loadProjectileTraits(projectileSource)) {
                weapon.projectileTraits = *traits;
            }
        }
        ids.push_back(def->name);
        defs.push_back(std::move(*def));
    }
    return rm::data::Roster::build(defs, ids);
}

/// A float world position as the fixed-point triple the geometry functions take.
///
/// The other half of the boundary: map markers, mouse picks and construction sites are all
/// authored or produced in floats, and this is where they become sim values. Named rather than
/// written out, because a three-element brace list of conversions at a dozen call sites is
/// where a transposed axis hides.
[[nodiscard]] std::array<rm::sim::Fx, 3> fxPoint(const std::array<float, 3>& position) {
    return {rm::sim::fxFromFloat(position[0]), rm::sim::fxFromFloat(position[1]),
            rm::sim::fxFromFloat(position[2])};
}

/// A `Transform` from the float position and yaw a placement helper produced.
///
/// The boundary between the placement code — which works in floats because it reads the map
/// and the blueprints — and the store, which is fixed point. One function so the conversion
/// reads the same at every spawn site.
[[nodiscard]] rm::sim::Transform transformAt(const std::array<float, 3>& position,
                                             float yaw) {
    rm::sim::Transform transform;
    transform.x = rm::sim::fxFromFloat(position[0]);
    transform.y = rm::sim::fxFromFloat(position[1]);
    transform.z = rm::sim::fxFromFloat(position[2]);
    transform.heading = rm::sim::bradFromRadians(yaw);
    return transform;
}



/// The one writer. Throws through `TickRate`'s constructor if the rate is outside 5–50 Hz,
/// which is what makes an out-of-range `--tick-rate` a startup error rather than a clamp.
void setAppTickRate(std::uint32_t ticksPerSecond) {
    gAppTickRate = rm::sim::TickRate{ticksPerSecond};
}


/// Rebuilds the wreck decals from the features, if any have been added since the last time.
///
/// THE PROJECTION (§7 P6.2). `sim::Feature` is the fact — a place, a size, what died there —
/// and this is the geometry for it, which is the renderer's business and crosses back into
/// floats here. Rebuilt from scratch rather than appended to, because "derive the drawing from
/// the state" is the property worth having: an append-only buffer and an append-only store are
/// two records of the same thing that can disagree, and the whole point of P6.2 is that there
/// is one.
///
/// Guarded on the count so a match that kills nothing this tick does no work. Cheap even when
/// it does fire: a wreck is a handful of triangles and a long match leaves tens of them.
namespace {
/// One impact's scorch: a boot-print, not a crater — the wrecks own the drama and
/// hundreds of these share the ground. Twelve segments keeps a full cap cheap.
inline constexpr float kImpactMarkRadiusElmos = 2.5f;
inline constexpr int kImpactMarkSegments = 12;
} // namespace

void noteImpactMark(UnitScene& scene, float x, float z) {
    if (scene.impactMarks.size() >= UnitScene::kMaxImpactMarks) {
        scene.impactMarks.erase(scene.impactMarks.begin(),
                                scene.impactMarks.begin() + 64);
    }
    scene.impactMarks.push_back(UnitScene::ImpactMark{.x = x, .z = z});
    ++scene.impactMarksRevision;
}

void refreshWreckDecals(UnitScene& scene, const rm::HeightField& field) {
    // The REVISION, not the count: reclaim removes wrecks now, and "one added, one
    // removed" leaves the count where it was while the ground has changed twice. Dead
    // slots are skipped for the same reason — a reclaimed wreck is clean ground, and
    // drawing it would advertise mass that is not there.
    if (scene.features.revision() == scene.wreckDecalsFrom
        && scene.impactMarksRevision == scene.impactDecalsFrom) {
        return;
    }
    scene.wreckDecals.clear();
    for (rm::UnitIndex slot = 0; slot < scene.features.size(); ++slot) {
        if (!scene.features.slotAlive(slot)) {
            continue;
        }
        const rm::sim::Feature& wreck = scene.features.all()[slot];
        rm::appendWreckMark(scene.wreckDecals, field,
                            {rm::sim::fxToFloat(wreck.at[0]), rm::sim::fxToFloat(wreck.at[1]),
                             rm::sim::fxToFloat(wreck.at[2])},
                            rm::sim::fxToFloat(wreck.radiusElmos)
                                * rm::kWreckMarkRadiusFactor);
    }
    // Impact scorch beside the wrecks: small dark discs where shots hit ground that
    // killed nothing. Twelve segments, not thirty-two — a bombardment leaves hundreds
    // and nobody inspects one up close.
    for (const UnitScene::ImpactMark& mark : scene.impactMarks) {
        rm::appendWreckMark(scene.wreckDecals, field, {mark.x, 0.0f, mark.z},
                            kImpactMarkRadiusElmos, kImpactMarkSegments);
    }
    scene.wreckDecalsFrom = scene.features.revision();
    scene.impactDecalsFrom = scene.impactMarksRevision;
}

/// The opening's step for a role, or a bare default when the plan names none.
///
/// Three named lookups rather than an index, because `StructureOrder` is an enum of three
/// specific things and the plan is a list: matching them by position would silently reorder the
/// opening if a step were inserted. When the order itself becomes data (P6), both sides become
/// the list and these go away.
[[nodiscard]] rm::data::OpeningStep stepForRole(const rm::data::Opening& opening,
                                                rm::unitdef::Role role) {
    for (const rm::data::OpeningStep& step : opening.structures) {
        if (step.role == role) {
            return step;
        }
    }
    return rm::data::OpeningStep{role, 0, {}, {}};
}

[[nodiscard]] rm::data::OpeningStep energyStep(const rm::data::Opening& o) {
    return stepForRole(o, rm::unitdef::Role::Energy);
}
[[nodiscard]] rm::data::OpeningStep factoryStep(const rm::data::Opening& o) {
    return stepForRole(o, rm::unitdef::Role::Factory);
}
[[nodiscard]] rm::data::OpeningStep extractorStep(const rm::data::Opening& o) {
    return stepForRole(o, rm::unitdef::Role::Extractor);
}

/// The blueprint path for a role, for one army's faction. Empty when the faction fields none.
///
/// THE FUNCTION THAT REPLACED FOUR CONSTANTS. `kExtractorBlueprint` was a UEF path in a header;
/// this asks the roster, so a Cybran army builds Cybran structures from the same plan.
[[nodiscard]] std::string blueprintFor(const UnitScene& scene, const rm::sim::Army& army,
                                       const rm::data::OpeningStep& step) {
    const std::optional<rm::data::RosterEntry> entry = rm::data::resolveStep(
        scene.roster, army.faction, step.role, step.tech, step.requires_, step.fallback);
    return entry ? entry->path() : std::string{};
}

// `paceAnimationByDistance` and `paceSceneAnimations` used to live here.
//
// Both are gone, and not merely moved: the walk-cycle phase is now computed inside
// `UnitScene::instanceFor`, the draw projection. It was always a DERIVED value — ground
// covered divided by the stride the animation implies — and once `UnitInstance` stopped being
// sim state there was nowhere for a separate pass to write it to. Two functions and two call
// sites became four lines in the one place that builds an instance.





} // namespace rm::app
