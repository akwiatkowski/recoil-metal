#include "app/Scene.hpp"

#include "core/unit/UnitBlueprint.hpp"
#include "core/log/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <cstdint>

namespace rm::app {

// Defined here, declared `extern` in the header — see the note there.
rm::sim::TickRate gAppTickRate{rm::sim::kDefaultTicksPerSecond};
bool gInterpolate = true;

void UnitScene::logTrialAlignment() {
    for (const auto& [shot, fireTick] : trialAlignmentShots) {
        // The bore the sim posed at fire time against the velocity it gave the
        // shell — read off the EVENT, so the number means the same thing in a
        // rendered frame and in the headless `--play` pre-run, where no
        // instance is ever posed. The read this replaced sampled whatever
        // frame happened to be current at flush, which in the pre-run measured
        // every shot against ONE pose and reported the hull's whole traverse
        // as an aim error.
        const std::array<float, 3> bore{rm::sim::fxToFloat(shot.visualBarrel[0]),
                                        rm::sim::fxToFloat(shot.visualBarrel[1]),
                                        rm::sim::fxToFloat(shot.visualBarrel[2])};
        const std::array<float, 3> launch{rm::sim::fxToFloat(shot.launchVelocity[0]),
                                          rm::sim::fxToFloat(shot.launchVelocity[1]),
                                          rm::sim::fxToFloat(shot.launchVelocity[2])};
        const auto length = [](const std::array<float, 3>& v) {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        };
        const float boreLen = length(bore);
        const float launchLen = length(launch);
        if (boreLen > 1e-6f && launchLen > 1e-6f) {
            const float dot = bore[0] * launch[0] + bore[1] * launch[1]
                              + bore[2] * launch[2];
            // Clamp: acos(NaN) on a 1.0000001 ratio would read as a failure on
            // a perfect alignment.
            const float cosine = std::clamp(dot / (boreLen * launchLen), -1.0f, 1.0f);
            const float degrees = std::acos(cosine) * 180.0f / std::numbers::pi_v<float>;
            rm::log::writef(rm::log::Level::Info, "trial-aim",
                "unit=%u:%u weapon=%s metric=rig_vs_launch error_deg=%.3f draw_tick=%llu",
                shot.unit.index, shot.unit.generation, shot.visualId.c_str(),
                static_cast<double>(degrees), static_cast<unsigned long long>(fireTick));
        } else {
            // Zero bore: an unmuzzled weapon has no barrel to pose (and a beam
            // carries no launch velocity), so there is nothing to measure.
            rm::log::writef(rm::log::Level::Info, "trial-aim",
                "unit=%u:%u weapon=%s error_deg=unavailable (no muzzled bore on the event)",
                shot.unit.index, shot.unit.generation, shot.visualId.c_str());
        }
    }
    trialAlignmentShots.clear();
}

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
/// One impact's scorch: a boot-print for a blast-less direct hit, not a crater —
/// the wrecks own the drama and hundreds of these share the ground. Twelve
/// segments keeps a full cap cheap.
inline constexpr float kImpactMarkRadiusElmos = 2.5f;  // elmos (legacy blast-less mark)
inline constexpr int kImpactMarkSegments = 12;
/// Retail sizes scorch from the explosion scale (`defaultexplosions.lua`
/// C-290): scale above 1.2 lays a decal at x3, otherwise a splat at x4. The
/// mark carries the shot's blast radius in elmos (Combat.hpp C-290), so the
/// scale is recovered by dividing back through 8 elmos per ogrid (Scmap.hpp
/// `kElmosPerOgrid`): radius/8 <= 1.2 draws x4, above draws x3.
inline constexpr float kImpactMarkScaleSplit = 1.2f;  // (C-290 `CreateScorchMark` gate)
inline constexpr float kImpactMarkSplatFactor = 4.0f;  // (C-290 splat x4)
inline constexpr float kImpactMarkDecalFactor = 3.0f;  // (C-290 decal x3)
[[nodiscard]] float impactMarkRadius(float blastRadiusElmos) noexcept {
    if (!(blastRadiusElmos > 0.0f)) {
        return kImpactMarkRadiusElmos;  // elmos (legacy blast-less mark)
    }
    // Elmos per ogrid: 8 (Scmap.hpp `kElmosPerOgrid`), written out so this
    // app-side projection does not include a map header for one divisor.
    const float scale = blastRadiusElmos / 8.0f;  // ogrids (C-290 scale)
    const float factor = scale > kImpactMarkScaleSplit ? kImpactMarkDecalFactor
                                                      : kImpactMarkSplatFactor;
    return blastRadiusElmos * factor;  // elmos (C-290 x3/x4)
}
/// Retail scorch lifetime floor in seconds (`defaultexplosions.lua` C-290:
/// lifetime 300-600); each mark retires at its own point in the spread,
/// derived from the mark's own position bits like the rotation angle below —
/// replay-identical, off the sim RNG (C-297). Presentation only, never hashed.
inline constexpr float kImpactMarkLifetimeSecondsMin = 300.0f;  // s (C-290 floor)
inline constexpr float kImpactMarkLifetimeSecondsWidth = 300.0f;  // s (C-290 600-300 spread)
/// Retail rotates every scorch by 0..2pi (`defaultexplosions.lua` C-290). The
/// engine has no texture to orient — the fan is rotationally symmetric — so the
/// angle buys per-mark variety, not fidelity. Derived from the mark's own bits
/// (splitmix32 on the position hash), never the sim RNG (C-297): the same mark
/// draws the same angle on every replay. Radians (C-290's `2*math.pi` range).
[[nodiscard]] float impactMarkRotation(float x, float z) noexcept {
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x9E3779B1u
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x85EBCA6Bu;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    bits *= 0x846CA68Bu;
    bits ^= bits >> 16;
    return static_cast<float>(static_cast<double>(bits) / 4294967296.0
                              * 6.283185307179586);  // rad, 0..2pi (C-290)
}
/// Each mark's own retirement point inside retail's 300-600 s scorch spread
/// (`defaultexplosions.lua` C-290): floor plus a 0..300 s offset from the
/// mark's own position bits — the same splitmix32 stream as the rotation
/// angle, replay-identical and off the sim RNG (C-297). Ticks, via the tick
/// rate rather than a tick literal. Presentation only, never hashed.
[[nodiscard]] rm::TickIndex impactMarkLifetime(float x, float z) noexcept {
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x9E3779B1u
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x85EBCA6Bu;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    const float unit = static_cast<float>(static_cast<double>(bits) / 4294967296.0);  // 0..1
    const float seconds = kImpactMarkLifetimeSecondsMin  // s (C-290)
                          + unit * kImpactMarkLifetimeSecondsWidth;  // s (C-290 spread)
    return gAppTickRate.ticks(rm::sim::seconds(seconds));  // ticks (C-290)
}

} // namespace

void noteImpactMark(UnitScene& scene, float x, float z, float blastRadiusElmos,
                    rm::TickIndex tick) {
    if (scene.impactMarks.size() >= UnitScene::kMaxImpactMarks) {
        scene.impactMarks.erase(scene.impactMarks.begin(),
                                scene.impactMarks.begin() + 64);
    }
    scene.impactMarks.push_back(UnitScene::ImpactMark{
        .tick = tick, .x = x, .z = z, .blastRadiusElmos = blastRadiusElmos});
    ++scene.impactMarksRevision;
}

void refreshWreckDecals(UnitScene& scene, const rm::HeightField& field,
                        rm::TickIndex now) {
    // Ageing is time, not new marks: sweep expired scorch before the
    // revision early-out, or a quiet battlefield keeps drawing stale marks
    // forever. Single pass — the size change tells whether the revision moved.
    // Each mark retires at its own point in retail's 300-600 s spread (C-290),
    // derived from the mark's own bits like the rotation angle.
    const std::size_t before = scene.impactMarks.size();
    scene.impactMarks.erase(
        std::remove_if(scene.impactMarks.begin(), scene.impactMarks.end(),
                       [now](const UnitScene::ImpactMark& mark) {
                           return now >= mark.tick + impactMarkLifetime(mark.x, mark.z);
                       }),
        scene.impactMarks.end());
    if (scene.impactMarks.size() != before) {
        ++scene.impactMarksRevision;
        // The old projection still holds the retired discs: drop them here
        // rather than appending the survivors beside them below.
        scene.wreckDecals.clear();
    }
    // The REVISION, not the count (unchanged): a removal above bumps it, so
    // a sweep that retired nothing still early-outs on a quiet tick.
    if (scene.features.revision() == scene.wreckDecalsFrom
        && scene.impactMarksRevision == scene.impactDecalsFrom) {
        return;
    }
    scene.wreckDecals.clear();
    // The REVISION, not the count: reclaim removes wrecks now, and "one added, one
    // removed" leaves the count where it was while the ground has changed twice. Dead
    // slots are skipped for the same reason — a reclaimed wreck is clean ground, and
    for (rm::UnitIndex slot = 0; slot < scene.features.size(); ++slot) {
        if (!scene.features.slotAlive(slot)) {
            continue;
        }
        const rm::sim::Feature& wreck = scene.features.all()[slot];
        // `C-292`: retail's `layer == 'Land'` death gate — a crashed gunship or
        // a sunk hull leaves no scorch. The flag was sampled at death; the
        // corpse's slot no longer knows its layer.
        if (!wreck.marksGround) {
            continue;
        }
        rm::appendWreckMark(scene.wreckDecals, field,
                            {rm::sim::fxToFloat(wreck.at[0]), rm::sim::fxToFloat(wreck.at[1]),
                             rm::sim::fxToFloat(wreck.at[2])},
                            rm::sim::fxToFloat(wreck.radiusElmos)
                                * rm::kWreckMarkRadiusFactor);
    }
    // Impact scorch beside the wrecks: dark discs where shots hit ground that
    // killed nothing. Sized by retail's splat/decal split (`defaultexplosions.lua`
    // C-290): the mark's blast radius recovers the explosion scale (radius/8),
    // scale above 1.2 draws x3, otherwise x4 — so an artillery strike reads
    // bigger than a rifle hit. Wreck marks stay permanent: the corpse is the
    // record, and reclaim removes it. Twelve segments, not thirty-two — a
    // bombardment leaves hundreds and nobody inspects one up close.
    for (const UnitScene::ImpactMark& mark : scene.impactMarks) {
        rm::appendWreckMark(scene.wreckDecals, field, {mark.x, 0.0f, mark.z},
                            impactMarkRadius(mark.blastRadiusElmos),
                            kImpactMarkSegments, impactMarkRotation(mark.x, mark.z));
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
