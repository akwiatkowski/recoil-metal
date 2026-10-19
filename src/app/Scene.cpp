#include "app/Scene.hpp"

#include "core/unit/UnitBlueprint.hpp"
#include "core/log/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <numbers>

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
/// Retail lays one of 10 splat textures (`scorch_001`-`010_albedo`) or one of
/// 10 decal textures (the same `scorch_001`-`010_albedo` names in
/// `ScorchDecalTextures`, `defaultexplosions.lua:234-244`) per scorch, picked
/// at random (`defaultexplosions.lua` C-290). The pick is the mark's own
/// position bits through one more splitmix32 round — replay-identical, off
/// the sim RNG (C-297), presentation only. Blast-less direct hits ride the
/// splat path. Counts, not indices into a texture this engine uploads: the
/// FA-PRESENT projection owns the sampling.
inline constexpr int kImpactMarkSplatTextures = 10;  // (C-290 `scorch_001`-`010`)
inline constexpr int kImpactMarkDecalTextures = 10;  // (C-290 `ScorchDecalTextures:234-244`)
[[nodiscard]] int impactMarkTexture(float x, float z, float blastRadiusElmos) noexcept {
    const float scale = blastRadiusElmos / 8.0f;  // ogrids (C-290 scale)
    const int choices = (blastRadiusElmos > 0.0f && scale > kImpactMarkScaleSplit)
                            ? kImpactMarkDecalTextures
                            : kImpactMarkSplatTextures;
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x85EBCA6Bu
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x9E3779B1u;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    bits *= 0x846CA68Bu;
    bits ^= bits >> 16;
    bits ^= bits >> 13;  // one more round past the rotation stream
    bits *= 0x5BD1E995u;
    bits ^= bits >> 16;
    return static_cast<int>(bits % static_cast<std::uint32_t>(choices));
}
/// Retail's LOD pick for one scorch (`defaultexplosions.lua:211,215` C-290):
/// both `CreateScorchMarkSplat` and `CreateScorchMarkDecal` pass
/// `GetRandomFloat(200,350)`. The pick is the mark's own position bits
/// through one more splitmix32 round past the texture stream —
/// replay-identical, off the sim RNG (C-297), presentation only.
inline constexpr int kImpactMarkLodMin = 200;  // (C-290 `GetRandomFloat(200,350)`)
inline constexpr int kImpactMarkLodMax = 350;  // (C-290 `GetRandomFloat(200,350)`)
[[nodiscard]] int impactMarkLod(float x, float z) noexcept {
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x85EBCA6Bu
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x9E3779B1u;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    bits *= 0x846CA68Bu;
    bits ^= bits >> 16;
    bits ^= bits >> 11;  // one more round past the texture stream
    bits *= 0x27D4EB2Fu;
    bits ^= bits >> 15;
    return kImpactMarkLodMin
           + static_cast<int>(bits
                              % static_cast<std::uint32_t>(kImpactMarkLodMax - kImpactMarkLodMin
                                                           + 1));
}
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
/// A per-weapon `OnImpact` decal's authored draw size in elmos (C-484): the
/// script lays its own texture at its own size, NOT the blast-derived
/// splat/decal split. `BaseScorch` keeps the split; every other value draws
/// what its corpus script states — the sonance crater pair's wider 12-elmo
/// albedo (the 10-elmo normals ride the same mark), proton `scorch_011` at
/// 10, napalm `scorch_001` at 3.75-5 (midpoint 4.375), EMP `nuke_scorch_001`
/// at 28, frag `scorch_004` at 2.25-3.75 (midpoint 3.0), zhanasee
/// `Scorch_012` at 40. The blast radius still sizes the base scorch when a
/// scripted shot ALSO scorches through its base class (all call their parent after).
[[nodiscard]] float impactMarkScriptedRadius(UnitScene::ImpactMarkScript script,
                                             float blastRadiusElmos) noexcept {
    using Script = UnitScene::ImpactMarkScript;
    switch (script) {
        case Script::SonanceCrater: return 12.0f;  // elmos (albedo half of :19-20)
        case Script::ProtonScorch: return 10.0f;  // elmos (`CIFArtilleryProton01:15`)
        case Script::NapalmScorch: return 4.375f;  // elmos (3.75-5 midpoint, `:13`)
        case Script::EmpScorch: return 28.0f;  // elmos (`CIFEMPFluxWarhead02:116`)
        case Script::FragScorch: return 3.0f;  // elmos (2.25-3.75 midpoint, `:20`)
        case Script::ZhanaseeScorch: return 40.0f;  // elmos (`SBOZhanaseeBomb01:29`)
        case Script::QuantumCrater: return 50.0f;  // elmos (`AIFQuantumWarhead02:119-120`)
        case Script::BaseScorch: break;
    }
    return impactMarkRadius(blastRadiusElmos);
}
/// A scripted mark's authored decal duration, in seconds (C-484): each
/// per-weapon `OnImpact` passes its own `duration` to `CreateDecal` — napalm
/// 15 (`TIFNapalmCarpetBomb01_script.lua:13`), frag 15
/// (`AIFFragmentationSensorShell03_script.lua:20`), proton 200
/// (`CIFArtilleryProton01_script.lua:15`), zhanasee 200
/// (`SBOZhanaseeBomb01_script.lua:29`), sonance 300
/// (`AIFSonanceShell02_script.lua:19-20`), EMP 0 = permanent
/// (`CIFEMPFluxWarhead02_script.lua:116`), quantum 0 = permanent
/// (`AIFQuantumWarhead02_script.lua:119-120`). `BaseScorch` keeps the 300-600
/// s spread. Zero reads as permanent. Presentation only, never hashed.
[[nodiscard]] float impactMarkScriptedLifetimeSeconds(UnitScene::ImpactMarkScript script) noexcept {
    using Script = UnitScene::ImpactMarkScript;
    switch (script) {
        case Script::SonanceCrater: return 300.0f;  // s (`:19-20` LOD arg)
        case Script::ProtonScorch: return 200.0f;  // s (`:15`)
        case Script::NapalmScorch: return 15.0f;  // s (`:13`)
        case Script::EmpScorch: return 0.0f;  // s (`:116` permanent)
        case Script::FragScorch: return 15.0f;  // s (`:20`)
        case Script::ZhanaseeScorch: return 200.0f;  // s (`:29`)
        case Script::QuantumCrater: return 0.0f;  // s (`:119-120` permanent)
        case Script::BaseScorch: break;
    }
    return -1.0f;  // (base spread, not a fixed duration)
}
/// A scripted mark's texture stream (C-484): past the base ten
/// (`scorch_001`-`010`), so the FA-PRESENT projection samples the weapon's
/// own texture rather than the splat/decal stream. Base marks keep 0..9.
[[nodiscard]] int impactMarkScriptedTexture(UnitScene::ImpactMarkScript script, int base) noexcept {
    using Script = UnitScene::ImpactMarkScript;
    switch (script) {
        case Script::SonanceCrater: return 10;  // `crater_radial01_*` pair
        case Script::ProtonScorch: return 11;  // `scorch_011_albedo`
        case Script::NapalmScorch: return 12;  // `scorch_001_albedo` scripted draw
        case Script::EmpScorch: return 13;  // `nuke_scorch_001_albedo`
        case Script::FragScorch: return 14;  // `scorch_004_albedo` scripted draw
        case Script::ZhanaseeScorch: return 15;  // `Scorch_012_albedo` scripted draw
        case Script::QuantumCrater: return 16;  // `Crater01_albedo` scripted draw
        case Script::BaseScorch: break;
    }
    return base;
}
/// Retail scorch lifetime floor in seconds (`defaultexplosions.lua` C-290:
/// lifetime 300-600); each mark retires at its own point in the spread,
/// derived from the mark's own position bits like the rotation angle below —
/// replay-identical, off the sim RNG (C-297). Presentation only, never hashed.
inline constexpr float kImpactMarkLifetimeSecondsMin = 300.0f;  // s (C-290 floor)
inline constexpr float kImpactMarkLifetimeSecondsWidth = 300.0f;  // s (C-290 600-300 spread)
/// Retail's `ScorchThread` fixed lifetimes (`defaultcollisionbeams.lua` C-290):
/// the thread lays `CreateSplat` directly, so the lifetime arg is the class's
/// own — 100 s for the seven 100-life thread bodies, 250 s for the quantum
/// thread, 15 s for the orbital death laser. Per-mark, from
/// `beamClassLifetimeSeconds`; this engine never floors one class to another.
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
/// Each mark's own retirement point (`defaultexplosions.lua` C-290 for blast
/// marks: floor plus a 0..300 s offset from the mark's own position bits —
/// the same splitmix32 stream as the rotation angle, replay-identical and off
/// the sim RNG (C-297); beam-thread marks retire at their own thread's
/// `CreateSplat` lifetime instead). Ticks, via the tick rate rather than a
/// tick literal. Presentation only, never hashed.
[[nodiscard]] rm::TickIndex impactMarkLifetime(const UnitScene::ImpactMark& mark) noexcept {
    if (mark.beamThread) {
        return gAppTickRate.ticks(rm::sim::seconds(mark.beamLifetimeSeconds));  // ticks (C-290)
    }
    if (mark.script != UnitScene::ImpactMarkScript::BaseScorch) {
        const float authored = impactMarkScriptedLifetimeSeconds(mark.script);  // s (C-484)
        if (authored <= 0.0f) {
            return std::numeric_limits<rm::TickIndex>::max();  // ticks (retail duration 0 = permanent)
        }
        return gAppTickRate.ticks(rm::sim::seconds(authored));  // ticks (C-484)
    }
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(mark.x) * 4096.0))  // 1/4096 elmo
                         * 0x9E3779B1u
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(mark.z) * 4096.0))  // 1/4096 elmo
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
                    rm::TickIndex tick, UnitScene::ImpactMarkScript script, bool beamThread,
                    float beamLifetimeSeconds) {
    if (scene.impactMarks.size() >= UnitScene::kMaxImpactMarks) {
        scene.impactMarks.erase(scene.impactMarks.begin(),
                                scene.impactMarks.begin() + 64);
    }
    scene.impactMarks.push_back(UnitScene::ImpactMark{
        .tick = tick,
        .x = x,
        .z = z,
        .blastRadiusElmos = blastRadiusElmos,
        .textureIndex = impactMarkScriptedTexture(script, impactMarkTexture(x, z, blastRadiusElmos)),
        .lod = impactMarkLod(x, z),
        .script = script,
        .beamLifetimeSeconds = beamLifetimeSeconds,
        .beamThread = beamThread});
    ++scene.impactMarksRevision;
}
UnitScene::ImpactMarkScript scriptTagForProjectile(std::string_view projectileId) noexcept {
    using Script = UnitScene::ImpactMarkScript;
    // The filename stem names the script family (`.../NAME/NAME_proj.bp`):
    // match the live corpus decal writers (C-484) — trail-thread splats
    // (`ShellTankTerran01`) lay nothing on impact, so those ride base scorch.
    const auto stem = projectileId.substr(projectileId.find_last_of('/') + 1);
    if (stem.starts_with("AIFQuantumWarhead02")) return Script::QuantumCrater;
    if (stem.starts_with("AIFSonanceShell02")) return Script::SonanceCrater;
    if (stem.starts_with("SIFSuthanusArtilleryShell02")) return Script::SonanceCrater;
    if (stem.starts_with("CIFArtilleryProton01")) return Script::ProtonScorch;
    if (stem.starts_with("TIFNapalmCarpetBomb01")) return Script::NapalmScorch;
    if (stem.starts_with("TIFNapalmCarpetBomb02")) return Script::NapalmScorch;
    if (stem.starts_with("CIFEMPFluxWarhead02")) return Script::EmpScorch;
    if (stem.starts_with("AIFFragmentationSensorShell03")) return Script::FragScorch;
    if (stem.starts_with("SBOZhanaseeBomb01")) return Script::ZhanaseeScorch;
    return Script::BaseScorch;
}
/// Whether a collision-beam class forks `ScorchThread` (C-290): the 8 classes
/// in `defaultcollisionbeams.lua` whose `OnImpact` forks the thread on
/// `impactType == 'Terrain'`, plus the two subclasses that inherit it
/// (`MicrowaveLaserCollisionBeam02` from `...01`, `Ultra...02` from `Ultra`).
/// The `SCCollisionBeam` base and the 5 remaining leaves state no thread.
/// Unknown names return true — an unmapped class is a content gap, not a
/// proved opt-out. Presentation only, never hashed.
[[nodiscard]] bool beamClassScorches(std::string_view beamClass) noexcept {
    // The 5 scorch-less leaves (`defaultcollisionbeams.lua` C-290): no
    // `ScorchThread` in their class body and no scorching parent.
    return beamClass != "GinsuCollisionBeam"
           && beamClass != "ParticleCannonCollisionBeam"
           && beamClass != "ZapperCollisionBeam"
           && beamClass != "DisruptorBeamCollisionBeam"
           && beamClass != "TractorClawCollisionBeam";
}
/// A collision-beam class's `ScorchThread` `CreateSplat` lifetime, in seconds
/// (`defaultcollisionbeams.lua` C-290, the 7th arg): the quantum thread passes
/// 250, the orbital death laser passes 15, the other seven thread bodies pass
/// 100. Unknown/empty names read as the 100 s common case — the same default
/// the scorch gate applies. Presentation only, never hashed.
[[nodiscard]] float beamClassLifetimeSeconds(std::string_view beamClass) noexcept {
    if (beamClass == "QuantumBeamGeneratorCollisionBeam") {
        return 250.0f;  // s (`defaultcollisionbeams.lua:120`)
    }
    if (beamClass == "OrbitalDeathLaserCollisionBeam") {
        return 15.0f;  // s (`defaultcollisionbeams.lua:555`)
    }
    return 100.0f;  // s (the seven 100-life thread bodies)
}
/// A collision-beam class's `ScorchThread` `CreateSplat` LOD (C-290, the 6th
/// arg): the quantum thread passes 250, the orbital death laser passes 250,
/// the other seven thread bodies pass 100. Unknown/empty names read as the
/// 100 common case — the same default the scorch gate applies. Presentation
/// only, never hashed.
[[nodiscard]] int beamClassLod(std::string_view beamClass) noexcept {
    if (beamClass == "QuantumBeamGeneratorCollisionBeam") {
        return 250;  // (`defaultcollisionbeams.lua:120`)
    }
    if (beamClass == "OrbitalDeathLaserCollisionBeam") {
        return 250;  // (`defaultcollisionbeams.lua:555`)
    }
    return 100;  // (the seven 100-LOD thread bodies)
}
/// A collision-beam class's `ScorchThread` opening `CreateSplat` size, in
/// elmos (`defaultcollisionbeams.lua` C-290, the 4th/5th args): the `size`
/// local's first-pass band — quantum/orbital 3.5+Random*3.5, experimental
/// phason 4+Random*1, photon-chromatic bodies 1.5+Random*1.5, hiro/ultra
/// 0.75+Random*0.75. The pick is the mark's own position bits through one
/// more splitmix32 round — replay-identical, off the sim RNG (C-297),
/// presentation only, never hashed.
[[nodiscard]] float beamClassSize(float x, float z, std::string_view beamClass) noexcept {
    float base = 1.5f;  // elmos (the photon-chromatic common case)
    float width = 1.5f;  // elmos (1.5+Random*1.5, `:181`)
    if (beamClass == "QuantumBeamGeneratorCollisionBeam"
        || beamClass == "OrbitalDeathLaserCollisionBeam") {
        base = 3.5f;  // elmos (3.5+Random*3.5, `:114`)
        width = 3.5f;  // elmos (`:114`)
    } else if (beamClass == "ExperimentalPhasonLaserCollisionBeam") {
        base = 4.0f;  // elmos (4+Random*1, `:303`)
        width = 1.0f;  // elmos (`:303`)
    } else if (beamClass == "TDFHiroCollisionBeam"
               || beamClass == "UltraChromaticBeamGeneratorCollisionBeam"
               || beamClass == "UltraChromaticBeamGeneratorCollisionBeam02") {
        base = 0.75f;  // elmos (0.75+Random*0.75, `:419`)
        width = 0.75f;  // elmos (`:419`)
    }
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x85EBCA6Bu
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x9E3779B1u;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    bits ^= bits >> 9;  // one more round past the LOD stream
    bits *= 0x1B873593u;
    bits ^= bits >> 15;
    const float unit = static_cast<float>(static_cast<double>(bits) / 4294967296.0);  // 0..1
    return base + unit * width;  // elmos (C-290 `size` band)
}
/// A collision-beam class's `ScorchThread` redrawn `CreateSplat` size, in
/// elmos (`defaultcollisionbeams.lua` C-290): the `size` reassignment after
/// every pass — quantum/orbital 3.2+Random*3.5 (`:128`), experimental phason
/// 4.0+Random*1.0 (`:317`, unchanged), every other thread body
/// 1.2+Random*1.5 (`:195`). Same position-bit splitmix32 pick as the opening
/// band — replay-identical, off the sim RNG (C-297), presentation only.
[[nodiscard]] float beamClassRedrawSize(float x, float z, std::string_view beamClass) noexcept {
    if (beamClass == "ExperimentalPhasonLaserCollisionBeam") {
        return beamClassSize(x, z, beamClass);  // elmos (redraw == opening, `:317`)
    }
    const bool heavy = beamClass == "QuantumBeamGeneratorCollisionBeam"
                    || beamClass == "OrbitalDeathLaserCollisionBeam";
    const float base = heavy ? 3.2f : 1.2f;  // elmos (C-290 redraw bands)
    const float width = heavy ? 3.5f : 1.5f;  // elmos (C-290 redraw bands)
    std::uint32_t bits = static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(x) * 4096.0))  // 1/4096 elmo
                         * 0x85EBCA6Bu
                       ^ static_cast<std::uint32_t>(std::llround(
                             static_cast<double>(z) * 4096.0))  // 1/4096 elmo
                             * 0x9E3779B1u;
    bits ^= bits >> 16;  // splitmix32 finaliser
    bits *= 0x7FEB352Du;
    bits ^= bits >> 15;
    bits ^= bits >> 9;  // one more round past the LOD stream
    bits *= 0x1B873593u;
    bits ^= bits >> 15;
    const float unit = static_cast<float>(static_cast<double>(bits) / 4294967296.0);  // 0..1
    return base + unit * width;  // elmos (C-290 `size` redraw band)
}

/// A collision-beam class's `ScorchThread` splat texture, as a mark texture
/// index (`defaultcollisionbeams.lua` C-290): seven thread bodies lay
/// `czar_mark01_albedo` (the base stream's pick), the experimental phason
/// lays `scorch_004_albedo` (past the C-484 scripted stream, like the
/// quantum `Crater01` pair at 16). Presentation only, never hashed.
[[nodiscard]] int beamClassTextureIndex(int base, std::string_view beamClass) noexcept {
    if (beamClass == "ExperimentalPhasonLaserCollisionBeam") {
        return 17;  // `scorch_004_albedo` (`:235`)
    }
    return base;
}

/// One beam strike's scorch (C-290 `ScorchThread` half). Retail forks the
/// thread when the beam's `OnImpact` sees `impactType == 'Terrain'`
/// (`defaultcollisionbeams.lua` — 8 of 16 classes; the rest scorch nothing),
/// and the thread lays `CreateSplat` marks at the beam's tracked end while it
/// dwells. A beam is hitscan — no projectile, no `ProjectileImpact` — so the
/// `BeamFired` event's `at` is the strike record. With water present, a strike
/// at or under the level leaves no mark (C-292); a waterless field (level 0,
/// dry ground) never refuses — the gate is water, not height. Blast-less, so
/// the mark rides the base splat path at the event's own tick —
/// replay-identical, off the sim RNG (C-297), presentation only, never hashed.
[[nodiscard]] bool noteBeamScorch(UnitScene& scene, const rm::sim::Event& event,
                                 float strikeHeightElmos, float waterLevelElmos,
                                 rm::TickIndex tick, std::string_view beamClass) {
    if (!beamClassScorches(beamClass)) {
        return false;
    }
    if (waterLevelElmos > 0.0f && strikeHeightElmos <= waterLevelElmos) {
        return false;
    }
    const float x = rm::sim::fxToFloat(event.at[0]);
    const float z = rm::sim::fxToFloat(event.at[2]);
    const float lifeSeconds = beamClassLifetimeSeconds(beamClass);  // s (C-290 7th arg)
    const int lod = beamClassLod(beamClass);  // (C-290 6th arg)
    noteImpactMark(scene, x, z, 0.0f, tick, UnitScene::ImpactMarkScript::BaseScorch, true, lifeSeconds);
    scene.impactMarks.back().lod = lod;
    scene.impactMarks.back().textureIndex = beamClassTextureIndex(scene.impactMarks.back().textureIndex, beamClass);  // (`SplatTexture` C-290)
    scene.impactMarks.back().beamSizeElmos = beamClassSize(x, z, beamClass);  // elmos (C-290 `size`)
    // Retail's `ScorchThread` lays its pass splat only when the tracked end
    // moved more than 0.25 (`defaultcollisionbeams.lua:119` C-290) — positions
    // retail measures in ogrids, 8 elmos each (`Scmap.hpp kElmosPerOgrid`), so
    // the gate is 2 elmos. A trembling end inside it lays nothing between
    // strikes; past it the thread draws a continuous line, not one point per
    // shot. Bridge from this shooter's last noted end to the current one with
    // midpoint splats spaced one blast-less mark diameter apart (2.5 elmos,
    // `kImpactMarkRadiusElmos` x2 — the same discs the projection draws, so
    // the trail reads unbroken). Same-shooter only, matched on the firing
    // handle's generation: a recycled slot's first strike notes but bridges
    // nothing. Presentation only, never hashed.
    const auto prior = scene.beamLastEnd.find(event.unit.index);
    const bool sameShooter = prior != scene.beamLastEnd.end()
                             && prior->second.first == event.unit.generation;
    const float prevX = sameShooter ? prior->second.second[0] : x;
    const float prevZ = sameShooter ? prior->second.second[1] : z;
    scene.beamLastEnd[event.unit.index] = {event.unit.generation, {x, z}};
    if (sameShooter) {
        // Elmos per ogrid: 8 (Scmap.hpp `kElmosPerOgrid`), written out so this
        // app-side projection does not include a map header for one product.
        constexpr float kBeamScorchMoveGateElmos = 0.25f * 8.0f;  // elmos (C-290 `:119`)
        const float dx = x - prevX;
        const float dz = z - prevZ;
        const float span = std::sqrt(dx * dx + dz * dz);
        const float gated = span - kBeamScorchMoveGateElmos;  // elmos past the gate
        if (!(gated > 0.0f)) {
            return true;  // inside retail's move gate: endpoints only, no filler
        }
        const float step = kImpactMarkRadiusElmos * 2.0f;  // elmos (one splat diameter)
        const auto middles = static_cast<std::size_t>(gated / step);
        for (std::size_t i = 1; i <= middles; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(middles + 1);
            const float fillX = prevX + dx * t;
            const float fillZ = prevZ + dz * t;
            noteImpactMark(scene, fillX, fillZ, 0.0f, tick, UnitScene::ImpactMarkScript::BaseScorch, true, lifeSeconds);
            scene.impactMarks.back().lod = lod;
            scene.impactMarks.back().textureIndex = beamClassTextureIndex(scene.impactMarks.back().textureIndex, beamClass);  // (`SplatTexture` C-290)
            scene.impactMarks.back().beamSizeElmos = beamClassRedrawSize(fillX, fillZ, beamClass);  // elmos (C-290 redrawn `size`)
        }
    }
    return true;
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
                           return now >= mark.tick + impactMarkLifetime(mark);
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
        const float radius = mark.beamThread ? mark.beamSizeElmos
                                             : impactMarkScriptedRadius(mark.script, mark.blastRadiusElmos);
        rm::appendWreckMark(scene.wreckDecals, field, {mark.x, 0.0f, mark.z}, radius,
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
