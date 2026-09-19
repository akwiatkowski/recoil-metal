#include "app/SceneBuild.hpp"

#include "core/blueprint/BlueprintMesh.hpp"
#include "core/data/ArmorDefs.hpp"
#include "core/data/MoveDef.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/log/Log.hpp"
#include "core/model/BuilderAim.hpp"
#include "core/model/Scm.hpp"
#include "core/model/Sca.hpp"
#include "core/unit/BarNames.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/sim/BuildOrder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>

namespace rm::app {

[[nodiscard]] bool floatsOnWater(const rm::unitdef::UnitDef& def) noexcept {
    // `C-324` (`0x631800`): the spawn layer's Water branch is
    // `caps&Water || FERRYBEACON` — the beacon marker rides the waterline even
    // though its motion type is None and it owns no water grid.
    return rm::data::moveDefFor(def).usesSurfaceWaterGrid || def.isFerryBeacon();
}

/// The primary turret's aim spec from the first turreted weapon, or nothing when the
/// type has no turret to pose. The traverse is the turret's OWN authored one —
/// `TurretYaw`/`TurretPitch` are the rest angles and the ranges are degrees either
/// side of them (02 §9.4) — while the firing arc the spec used to derive them from
/// is the hull's. A blueprint that states no range keeps the arc's limits, the
/// historical answer. Speeds arrive in radians already. The muzzle doubles as the
/// aim point — it is what the barrel is pointing at the target with.
[[nodiscard]] std::optional<std::pair<rm::TurretAimSpec, std::size_t>>
turretSpecFor(const rm::unitdef::UnitDef& def) {
    constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
    for (std::size_t w = 0; w < def.weapons.size(); ++w) {
        const rm::unitdef::Weapon& weapon = def.weapons[w];
        if (!weapon.turreted || weapon.turretYawBone.empty() || weapon.turretPitchBone.empty()
            || weapon.muzzleBone.empty()) {
            continue;
        }
        rm::TurretAimSpec spec{
            .yawBone = {.name = weapon.turretYawBone},
            .pitchBone = {.name = weapon.turretPitchBone},
            .muzzleBone = {.name = weapon.muzzleBone},
            .yawSlew = weapon.turretYawSpeedRadPerSecond,
            .pitchSlew = weapon.turretPitchSpeedRadPerSecond,
        };
        spec.yawMin = (weapon.turretYawRangeDegrees > 0.0f
                           ? weapon.turretYawDegrees - weapon.turretYawRangeDegrees
                           : weapon.arcCentreDegrees - weapon.arcRangeDegrees)
                      * kDegToRad;
        spec.yawMax = (weapon.turretYawRangeDegrees > 0.0f
                           ? weapon.turretYawDegrees + weapon.turretYawRangeDegrees
                           : weapon.arcCentreDegrees + weapon.arcRangeDegrees)
                      * kDegToRad;
        if (weapon.turretPitchRangeDegrees > 0.0f) {
            spec.pitchMin =
                (weapon.turretPitchDegrees - weapon.turretPitchRangeDegrees) * kDegToRad;
            spec.pitchMax =
                (weapon.turretPitchDegrees + weapon.turretPitchRangeDegrees) * kDegToRad;
        }
        // `TurretDualManipulators`: the second arm's trunnion and muzzle ride the
        // same ring under the same two angles — only their bones are their own.
        if (weapon.turretDualManipulators) {
            spec.pitch2Bone = {.name = weapon.turretDualPitchBone};
            spec.muzzle2Bone = {.name = weapon.turretDualMuzzleBone};
        }
        return std::make_pair(std::move(spec), w);
    }
    return std::nullopt;
}

[[nodiscard]] TurretRig resolveTurretRig(const rm::Model& model,
                                         const rm::unitdef::UnitDef* def) {
    if (def == nullptr) {
        return {};
    }
    const auto spec = turretSpecFor(*def);
    if (!spec) {
        return {};
    }
    rm::BuilderAimRig rig = rm::resolveTurretAim(model, spec->first);
    if (!rig.exists()) {
        // Silent otherwise: the applier skips the unit outright, which reads
        // as "the turret never moves" with no error anywhere.
        const rm::unitdef::Weapon& gun = def->weapons[spec->second];
        rm::log::writef(rm::log::Level::Warn, "animation",
                        "no turret bones on %s (%s) yaw '%s' pitch '%s' muzzle '%s'",
                        def->name.c_str(), gun.label.c_str(),
                        gun.turretYawBone.c_str(), gun.turretPitchBone.c_str(),
                        gun.muzzleBone.c_str());
        return {};
    }
    TurretRig turret{.rig = std::move(rig), .weapon = spec->second};
    const rm::unitdef::Weapon& weapon = def->weapons[spec->second];
    // The recoil slide: rack subtree flags, then the telescope's own channel
    // OR'd in beside them — retail gives each its own CSlideManipulator with
    // its own goal (defaultweapons.lua:270-287). The distances stay SIGNED:
    // negative slides backwards along the barrel (the corpus's convention —
    // the Titan's is -0.2), positive forwards (UEL0203's +0.1).
    turret.recoilFlags = rm::resolveRecoilFlags(model, weapon.recoilBone);
    const std::vector<std::uint32_t> telescopeFlags =
        rm::resolveTelescopeFlags(model, weapon.telescopeBone);
    if (!telescopeFlags.empty()) {
        if (turret.recoilFlags.size() < telescopeFlags.size()) {
            turret.recoilFlags.resize(telescopeFlags.size(), 0U);
        }
        for (std::size_t bone = 0; bone < telescopeFlags.size(); ++bone) {
            turret.recoilFlags[bone] |= telescopeFlags[bone];
        }
    }
    const rm::RecoilSpec recoil =
        rm::resolveRecoilSpec(weapon, def->meshToElmos,
                              static_cast<float>(rm::app::gAppTickRate.ticksPerSecond()));
    turret.recoilDistanceElmos = recoil.rackDistanceElmos;
    turret.recoilReturnPerTick = recoil.rackReturnPerTick;
    turret.telescopeDistanceElmos = recoil.telescopeDistanceElmos;
    turret.telescopeReturnPerTick = recoil.telescopeReturnPerTick;
    return turret;
}

/// A type's deploy animation: the first weapon's WeaponUnpackAnimation, loaded
/// into the scene's store. Only 17 blueprints name one, so the scan is free and
/// the VFS read happens only then. Null when nothing names one or the file is
/// missing — a gun without authored deploy simply appears deployed.
[[nodiscard]] const rm::sca::Animation*
loadUnpackAnimation(UnitScene& scene, const rm::vfs::Vfs& content,
                    const rm::unitdef::UnitDef& def) {
    for (const rm::unitdef::Weapon& weapon : def.weapons) {
        if (weapon.weaponUnpackAnimation.empty()) {
            continue;
        }
        const auto bytes = content.read(weapon.weaponUnpackAnimation);
        if (!bytes) {
            rm::log::writef(rm::log::Level::Warn, "animation", "no unpack animation (%s)",
                            weapon.weaponUnpackAnimation.c_str());
            return nullptr;
        }
        auto loaded = rm::sca::load(std::span<const std::byte>{bytes->data(), bytes->size()});
        if (!loaded) {
            rm::log::writef(rm::log::Level::Warn, "animation", "bad unpack animation (%s): %s",
                            weapon.weaponUnpackAnimation.c_str(),
                            loaded.error().message.c_str());
            return nullptr;
        }
        scene.animations.push_back(std::move(*loaded));
        return &scene.animations.back();
    }
    return nullptr;
}

/// The sim's copy of the resolved ring, for the type whose batch was just
/// pushed: pivots and muzzle for slew, gating and the fire origin, converted
/// once here so the tick runs no float math. Fine mesh only — the coarse
/// batch reuses this mount, and a second call would overwrite it with
/// merged-bone approximations.
void publishTurretMount(UnitScene& scene, rm::UnitTypeIndex type, float meshToElmos) {
    const rm::UnitBatch& batch = scene.batches.back();
    const rm::unitdef::UnitDef& resolved = scene.definitions.back();
    if (!batch.turretAim.exists() || batch.turretWeapon >= resolved.weapons.size()
        || !resolved.weapons[batch.turretWeapon].visualMuzzleOffset) {
        return;
    }
    const auto& gun = resolved.weapons[batch.turretWeapon];
    const auto& rig = batch.turretAim;
    // Rest barrel direction in mesh units (ratios only — scale cancels), for
    // the sim's rest angles. Same trunnion-to-muzzle line the aim solver uses.
    const auto& pivot = rig.pitchPivot;
    rm::sim::UnitCatalog::TurretMountSpec spec;
    spec.weapon = batch.turretWeapon;
    spec.muzzle = {(*gun.visualMuzzleOffset)[0], (*gun.visualMuzzleOffset)[1],
                   (*gun.visualMuzzleOffset)[2]};
    spec.yawPivot = {rig.yawPivot[0] * meshToElmos, rig.yawPivot[1] * meshToElmos,
                     rig.yawPivot[2] * meshToElmos};
    spec.pitchPivot = {rig.pitchPivot[0] * meshToElmos, rig.pitchPivot[1] * meshToElmos,
                       rig.pitchPivot[2] * meshToElmos};
    spec.yawAxis = {rig.yawAxis[0], rig.yawAxis[1], rig.yawAxis[2]};
    spec.pitchAxis = {rig.pitchAxis[0], rig.pitchAxis[1], rig.pitchAxis[2]};
    spec.restDir = {(*gun.visualMuzzleOffset)[0] / meshToElmos - pivot[0],
                    (*gun.visualMuzzleOffset)[1] / meshToElmos - pivot[1],
                    (*gun.visualMuzzleOffset)[2] / meshToElmos - pivot[2]};
    spec.yawMinRadians = rig.yawMin;
    spec.yawMaxRadians = rig.yawMax;
    spec.pitchMinRadians = rig.pitchMin;
    spec.pitchMaxRadians = rig.pitchMax;
    // The second arm comes along only with BOTH ends resolved: a pivot with no
    // muzzle poses nothing the sim can fire from.
    if (rig.hasPitch2 && gun.visualMuzzle2Offset) {
        spec.dual = true;
        spec.muzzle2 = *gun.visualMuzzle2Offset;
        spec.pitchPivot2 = {rig.pitch2Pivot[0] * meshToElmos,
                            rig.pitch2Pivot[1] * meshToElmos,
                            rig.pitch2Pivot[2] * meshToElmos};
        spec.pitchAxis2 = {rig.pitch2Axis[0], rig.pitch2Axis[1], rig.pitch2Axis[2]};
    }
    scene.catalog.setTurretMount(type, spec);
}

/// A type's walk cycle. The blueprint's `Display.AnimationWalk` is the
/// authority — it names clips a directory scan cannot see (the UEF ACU's is
/// `uel0001_a001.sca`, no "walk" anywhere in the name). Units that state none
/// fall back to the first `*walk*.sca` beside the mesh, which is where the
/// convention still pays for itself. Null when nothing walks there — every
/// structure, and every vehicle that rolls rather than strides.
[[nodiscard]] const rm::sca::Animation* loadWalkAnimation(
    UnitScene& scene, const rm::vfs::Vfs& content,
    const rm::unitdef::UnitDef& def, std::string_view meshPath) {
    const auto loadAt = [&scene](std::span<const std::byte> bytes,
                                 const char* path) -> const rm::sca::Animation* {
        auto loaded = rm::sca::load(bytes);
        if (!loaded) {
            rm::log::writef(rm::log::Level::Warn, "animation",
                            "bad walk animation (%s): %s", path,
                            loaded.error().message.c_str());
            return nullptr;
        }
        scene.animations.push_back(std::move(*loaded));
        return &scene.animations.back();
    };
    if (!def.animationWalk.empty()) {
        if (const auto bytes = content.read(def.animationWalk)) {
            return loadAt({bytes->data(), bytes->size()}, def.animationWalk.c_str());
        }
        rm::log::writef(rm::log::Level::Warn, "animation",
                        "declared walk animation not found (%s)",
                        def.animationWalk.c_str());
        return nullptr;
    }
    const std::size_t slash = meshPath.rfind('/');
    if (slash == std::string_view::npos) {
        return nullptr;
    }
    const std::string dir{meshPath.substr(0, slash)};
    for (const std::string& candidate : content.list(dir, ".sca")) {
        std::string folded = candidate;
        for (char& c : folded) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (folded.find("walk") == std::string::npos) {
            continue;
        }
        const auto bytes = content.read(candidate);
        if (!bytes) {
            continue;
        }
        return loadAt({bytes->data(), bytes->size()}, candidate.c_str());
    }
    return nullptr;
}

/// The movement state a unit of this definition is born with.
///
/// ONE DERIVATION, because there are two spawn paths and they drifted. `spawnUnit` built this by
/// hand and `spawnCommanders` set only the army index — so every commander in the game started
/// with `speedPerTick` and `turnPerTick` at zero, which are `MoveState`'s deliberate defaults.
///
/// That is the whole of the "I right click and the commander is standing still" report, and it
/// is invisible from every angle an assertion was looking from: the route is FOUND, the order is
/// ACCEPTED, the queue line is drawn to the destination, and no refusal is printed — the unit
/// simply multiplies its step by a speed of zero forever. A headless `--march` reports "2 of 2
/// units routed" and is telling the truth about routing while saying nothing about motion.
///
/// Rates are per second in the blueprint and per tick in the sim (§5.1). A structure keeps the
/// zeroes, which is what makes it a structure as far as movement is concerned.
rm::sim::MoveState motionFor(const rm::unitdef::UnitDef& def, int armyIndex) {
    rm::sim::MoveState motion;
    motion.armyIndex = armyIndex;
    // `C-324` (`0x631800`): the spawn layer is derived, never authored — an
    // air-capable unit spawns Air UNLESS it is EXPERIMENTAL, which spawns Land
    // (the CZAR/Ahwassa roll off the pad grounded and take off on their first
    // order). `airborne` is the current layer, not the capability — `canFly`
    // below carries that — so a grounded experimental reads as a land target.
    motion.airborne = def.motion == rm::unitdef::MotionType::Air
                      && !def.hasCategory("EXPERIMENTAL");
    motion.surfaceWater = floatsOnWater(def);
    motion.submersible = def.motion == rm::unitdef::MotionType::SurfacingSub;
    if (motion.submersible) {
        // Retail's default wet spawn prefers Sub except for EXPERIMENTAL units
        // (ART-E001 0x006319c7). Explicit layer-bearing spawns remain outside this seam.
        motion.submerged = !def.hasCategory("EXPERIMENTAL");
        motion.diveTargetSubmerged = motion.submerged;
        motion.submarineElevation = std::min(rm::sim::Fx{}, rm::sim::fxFromFloat(def.elevationElmos));
        motion.submarineOffset = motion.submerged ? motion.submarineElevation : rm::sim::Fx{};
        motion.divePerTick = gAppTickRate.perTick(def.diveSurfaceSpeedElmosPerSecond);
    }
    // `AutoSurfaceMode` (`C-203`): the Dive toggle's second state, from the
    // root-level `AutoSurfaceToAttack` key. Only a submersible can act on it —
    // the attack task reads it to pick the Water layer — but the flag itself
    // is authored state, so it is copied unconditionally.
    motion.autoSurface = def.autoSurfaceToAttack;
    motion.hovering = def.motion == rm::unitdef::MotionType::Hover;
    motion.hoverElevation = motion.hovering ? rm::sim::fxFromFloat(def.elevationElmos)
                                           : rm::sim::Fx{};
    motion.radiusElmos = rm::sim::fxFromFloat(def.collisionRadiusElmos);
    if (def.isMobile()) {
        motion.speedPerTick = gAppTickRate.perTick(def.speedElmosPerSecond);
        motion.turnPerTick =
            def.turnRateRadiansPerSecond > 0.0f
                ? gAppTickRate.bradPerTick(def.turnRateRadiansPerSecond)
                : gAppTickRate.bradPerTick(rm::sim::kDefaultTurnRateRadiansPerSecond);
    }
    if (def.motion == rm::unitdef::MotionType::Air) {
        // Flyers spawn cruising (the status quo ante — spawn changes nothing observable);
        // the winged mover (`C-221`) takes them from there. Gains stay per second as
        // authored; the integrator applies the retail 0.1 step itself.
        motion.canFly = true;
        motion.airState = motion.airborne ? rm::sim::MoveState::AirState::Top
                                          : rm::sim::MoveState::AirState::Bottom;
        motion.airMaxSpeedElmosPerSec = rm::sim::fxFromFloat(def.speedElmosPerSecond);
        motion.airMinSpeedElmosPerSec = rm::sim::fxFromFloat(def.airMinSpeedElmosPerSecond);
        motion.airAttackElevation = rm::sim::fxFromFloat(def.airAttackElevationElmos);
        motion.airWinged = def.airWinged;
        motion.airTurnSpeed = rm::sim::fxFromFloat(def.airTurnSpeed);
        motion.airCombatTurnSpeed = rm::sim::fxFromFloat(def.airCombatTurnSpeed);
        motion.airKTurn = rm::sim::fxFromFloat(def.airKTurn);
        motion.airKTurnDamping = rm::sim::fxFromFloat(def.airKTurnDamping);
        motion.airKRoll = rm::sim::fxFromFloat(def.airKRoll);
        motion.airBankFactor = rm::sim::fxFromFloat(def.airBankFactor);
        motion.airTightTurnMultiplier = rm::sim::fxFromFloat(def.airTightTurnMultiplier);
        motion.airBreakOffTrigger = rm::sim::fxFromFloat(def.airBreakOffTrigger);
        motion.airBreakOffDistance = rm::sim::fxFromFloat(def.airBreakOffDistance);
        motion.airRandomBreakOffMultiplier = rm::sim::fxFromFloat(def.airRandomBreakOffMultiplier);
        motion.airSustainedThreshold = static_cast<rm::TickCount>(def.airSustainedThresholdSec * 10);
        motion.airMinChangeTicks = static_cast<rm::TickCount>(def.airMinChangeSec * 10);
        motion.airMaxChangeTicks = static_cast<rm::TickCount>(def.airMaxChangeSec * 10);
        motion.airBreakOffNearTarget = def.airBreakOffNearTarget;
        motion.airKMove = rm::sim::fxFromFloat(def.airKMove);
        motion.airKMoveDamping = rm::sim::fxFromFloat(def.airKMoveDamping);
        motion.airKLift = rm::sim::fxFromFloat(def.airKLift);
        motion.airKLiftDamping = rm::sim::fxFromFloat(def.airKLiftDamping);
        motion.airLiftFactor = rm::sim::fxFromFloat(def.airLiftFactor);
        motion.airTransportation = def.hasCategory("TRANSPORTATION");
        motion.airElevation = def.elevationElmos > 0.0f
            ? rm::sim::fxFromFloat(def.elevationElmos)
            : rm::sim::kAirClearanceElmos;
        motion.idleLandThreshold = def.airAutoLandTimeSec > 0.0f
            ? static_cast<std::uint32_t>(def.airAutoLandTimeSec
                                         * static_cast<float>(gAppTickRate.ticksPerSecond()))
            : std::numeric_limits<std::uint32_t>::max();
        motion.fuelDrainPerTick = def.airFuelUseTimeSec > 0.0f
            ? rm::sim::fxFromFloat(1.0f / (def.airFuelUseTimeSec * 10.0f))
            : rm::sim::Fx{};
        motion.fuelRatio = rm::sim::Fx::fromInt(1);
    }
    // `C-244`: own mass for the air controller's cargo ratio — `carriedMass`
    // accumulates on the children as `UnitStore::attach` runs. Set for every
    // unit, not just flyers: the CHILD's mass is what the carrier's ratio reads.
    motion.unitMass = rm::sim::fxFromFloat(def.unitMass);
    return motion;
}

/// Resolves each weapon's muzzle bone against the model's skeleton — the one moment both
/// the blueprint's bone NAME and the model's bone POSITIONS are in hand. The height is the
/// bone's rest-pose global Y scaled into elmos; anything at ground level or below keeps the
/// sim's fallback constant, because a muzzle in the floor is a modelling accident and not a
/// firing solution. Case-insensitive, as the corpus's bone spelling is not reliable.
void resolveMuzzleBones(rm::unitdef::UnitDef& def, const rm::Model& model) {
    const auto sameName = [](std::string_view a, std::string_view b) {
        return a.size() == b.size()
               && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                      return std::tolower(x) == std::tolower(y);
                  });
    };
    for (rm::unitdef::Weapon& weapon : def.weapons) {
        if (weapon.muzzleBone.empty()) {
            continue;
        }
        bool resolved = false;
        for (const rm::ModelBone& bone : model.bones) {
            if (sameName(bone.name, weapon.muzzleBone)) {
                weapon.visualMuzzleOffset = std::array<float,3>{bone.globalOffset[0]*def.meshToElmos,
                    bone.globalOffset[1]*def.meshToElmos, bone.globalOffset[2]*def.meshToElmos};
                const float heightElmos = bone.globalOffset[1] * def.meshToElmos;
                if (heightElmos > 0.05f) {
                    weapon.muzzleHeight = rm::sim::fxFromFloat(heightElmos);
                }
                resolved = true;
            }
            if (weapon.turretDualManipulators && !weapon.turretDualMuzzleBone.empty()
                && sameName(bone.name, weapon.turretDualMuzzleBone)) {
                // The second arm's tip, for the dual-manipulator mount — same
                // bone lookup, its own slot so the primary's offset survives.
                weapon.visualMuzzle2Offset =
                    std::array<float,3>{bone.globalOffset[0]*def.meshToElmos,
                                        bone.globalOffset[1]*def.meshToElmos,
                                        bone.globalOffset[2]*def.meshToElmos};
            }
        }
        // Silent otherwise: flashes and trails fall back to the hull centre,
        // which reads as "shots come from the middle" with no error anywhere.
        if (!resolved) {
            rm::log::writef(rm::log::Level::Warn, "animation",
                            "no muzzle bone '%s' on %s (%s) — flashes use hull centre",
                            weapon.muzzleBone.c_str(), def.name.c_str(),
                            weapon.label.c_str());
        }
    }
}

/// Files a model's `Attachpoint*` bones into the catalog's per-class lists
/// (`C-198`): `Attachpoint_Spr` is class 4, `_Lrg` 3, `_Med` 2, and every other
/// `Attachpoint` 1 — matched as a SUBSTRING because shipped meshes prefix them
/// (`Left_Attachpoint_sml_01` on the C-6 Courier). `Launchpoint` and
/// `AttachSpecial` are staging/special machinery, not cargo points, and are
/// skipped. Rest offsets scale to elmos here, at the content boundary.
[[nodiscard]] std::vector<rm::sim::UnitCatalog::AttachBoneSpec>
resolveAttachBones(const rm::Model& model, const rm::unitdef::UnitDef& def) {
    std::vector<rm::sim::UnitCatalog::AttachBoneSpec> bones;
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const rm::ModelBone& bone = model.bones[i];
        const std::string_view name{bone.name};
        int cargoClass = 0;
        if (name.find("Attachpoint_Spr") != std::string_view::npos) {
            cargoClass = 4;
        } else if (name.find("Attachpoint_Lrg") != std::string_view::npos) {
            cargoClass = 3;
        } else if (name.find("Attachpoint_Med") != std::string_view::npos) {
            cargoClass = 2;
        } else if (name.find("Attachpoint") != std::string_view::npos) {
            cargoClass = 1;
        }
        if (cargoClass == 0) {
            continue;
        }
        bones.push_back(rm::sim::UnitCatalog::AttachBoneSpec{
            .bone = static_cast<std::int32_t>(i),
            .cargoClass = cargoClass,
            .rest = {bone.globalOffset[0] * def.meshToElmos,
                     bone.globalOffset[1] * def.meshToElmos,
                     bone.globalOffset[2] * def.meshToElmos}});
    }
    return bones;
}



/// Resolves a `--units` argument that names a unit DEFINITION rather than a
/// model, returning the model to load and the stats to move it with.
///
/// A `.lua` argument is the interesting path: the definition names its own
/// model and carries the speed and turn rate the game authored for it, which is
/// how a scene stops moving every unit at the one speed this engine used to
/// hardcode.
[[nodiscard]] std::optional<rm::unitdef::UnitDef> resolveUnitDef(
    const std::filesystem::path& path, const rm::vfs::AssetSearch& search,
    std::filesystem::path& modelOut) {
    // A Supreme Commander blueprint. Its mesh is not named inside it, so the
    // reader that finds one by convention is the same one that reads the stats.
    if (path.extension() == ".bp") {
        const auto def = rm::unitbp::loadFile(path);
        if (!def) {
            rm::log::writef(rm::log::Level::Error, "unit",
                            "blueprint \"%s\" not read: %s",
                            path.filename().string().c_str(),
                            def.error().message.c_str());
            return std::nullopt;
        }

        // The game root a `MeshName` path is relative to, when the blueprint uses
        // one: two directories up from `<root>/units/<ID>/<ID>_unit.bp`.
        const std::filesystem::path root = path.parent_path().parent_path().parent_path();
        modelOut = rm::unitbp::resolveMesh(*def, path, root);
        if (modelOut.empty()) {
            rm::log::writef(rm::log::Level::Error, "unit",
                            "\"%s\" has no mesh beside it%s", def->name.c_str(),
                            def->modelPath.empty() ? ""
                                                   : " and its MeshName did not resolve");
            return std::nullopt;
        }

        std::printf("unit %s: %.0f elmos/s, %.2f rad/s, radius %.1f elmos, %.0f hp, %s\n",
                    def->name.c_str(), static_cast<double>(def->speedElmosPerSecond),
                    static_cast<double>(def->turnRateRadiansPerSecond),
                    static_cast<double>(def->collisionRadiusElmos),
                    static_cast<double>(rm::sim::magToFloat(def->health)),
                    rm::unitdef::travelsOnGround(def->motion) ? "on the ground"
                    : def->canFly                            ? "flying"
                                                             : "not a ground mover");
        return *def;
    }

    if (path.extension() != ".lua") {
        return std::nullopt;
    }

    auto def = rm::unitdef::loadFile(path);
    if (!def) {
        rm::log::writef(rm::log::Level::Error, "unit", "definition \"%s\" not read: %s",
                        path.filename().string().c_str(), def.error().message.c_str());
        return std::nullopt;
    }

    // The display name, from the tree's own language files. A BAR unit `.lua` carries no
    // player-facing text; `language/en/units.json` does, and it sits a few levels above the
    // unit — the same walk that finds `objects3d` below. Nothing found leaves the name
    // empty, and the interface falls back to the id as it always has.
    if (def->description.empty()) {
        for (std::filesystem::path dir = path.parent_path();
             !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
            const std::filesystem::path names = dir / "language" / "en" / "units.json";
            if (!std::filesystem::is_regular_file(names)) {
                continue;
            }
            std::ifstream in{names, std::ios::binary};
            if (in) {
                const std::string text{std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()};
                def->description = rm::unitdef::barUnitName(text, def->name);
            }
            break;
        }
    }

    modelOut = rm::unitdef::resolveModel(search, def->modelPath);

    if (modelOut.empty()) {
        // Nothing in the search path — which is the usual case, since pointing
        // at a definition by its full path is how this is used and needs no
        // --data-dir. Walk up from the definition to a sibling objects3d:
        // `units/ArmBots/armpw.lua` and `objects3d/Units/armpw.s3o` are a few
        // levels apart, so the layout answers the question.
        rm::vfs::AssetSearch beside;
        for (std::filesystem::path dir = path.parent_path();
             !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
            const std::filesystem::path candidate = dir / "objects3d";
            if (std::filesystem::is_directory(candidate)) {
                beside.addRoot(candidate);
                break;
            }
        }
        modelOut = rm::unitdef::resolveModel(beside, def->modelPath);
    }

    if (modelOut.empty()) {
        rm::log::writef(rm::log::Level::Error, "unit",
                        "\"%s\" names model \"%s\", which was not found (try --data-dir)",
                        def->name.c_str(), def->modelPath.c_str());
        return std::nullopt;
    }

    std::printf("unit %s: %.0f elmos/s, %.2f rad/s, footprint %d x %d squares, %.0f hp%s\n",
                def->name.c_str(), static_cast<double>(def->speedElmosPerSecond),
                static_cast<double>(def->turnRateRadiansPerSecond), def->footprintSquaresX,
                def->footprintSquaresZ,
                static_cast<double>(rm::sim::magToFloat(def->health)),
                def->canFly ? " (flies)" : "");
    return *def;
}


/// A Supreme Commander texture beside a mesh, inside the mounted content.
///
/// `scmTexturePath` cannot serve this: it decides whether to strip a `_lod0` by
/// asking the real filesystem whether the stripped file exists, which is a
/// filesystem assumption living inside what is otherwise string work. Given a VFS
/// path nothing exists on disk, so it kept the `_lod0` and looked for
/// `UEL0201_lod0_Albedo.dds` — a name the archive does not contain.
///
/// The strip is right and the check is what has to move. Level 0 shares the unit's
/// own textures (`UEL0201_Albedo.dds`) while every coarser level has its OWN
/// (`UEL0201_lod1_Albedo.dds`), both of which the archive really carries — so the
/// stripped spelling is tried first and the literal one second, and the content
/// decides rather than the disk.
[[nodiscard]] std::string scmTextureInVfs(const std::string& meshPath, const char* suffix,
                                          const rm::vfs::Vfs& content) {
    const std::filesystem::path path{meshPath};
    const std::string stem = path.stem().string();
    const std::string dir = path.parent_path().generic_string();

    static constexpr std::string_view kLod0 = "_lod0";
    if (stem.size() > kLod0.size()) {
        std::string tail = stem.substr(stem.size() - kLod0.size());
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == kLod0) {
            const std::string stripped =
                dir + "/" + stem.substr(0, stem.size() - kLod0.size()) + suffix;
            if (content.contains(stripped)) {
                return stripped;
            }
        }
    }

    const std::string literal = dir + "/" + stem + suffix;
    return content.contains(literal) ? literal : std::string{};
}

/// Reads a unit entirely out of the mounted content: blueprint, then the mesh it
/// does not name, then the textures neither of them names.
///
/// This is the path a real game takes and the reason the VFS exists. Nothing is
/// extracted, and the paths are the ones the content uses about itself.
[[nodiscard]] std::optional<VfsUnit> resolveUnitFromContent(const std::string& blueprintPath,
                                                            const rm::vfs::Vfs& content) {
    const auto source = content.read(blueprintPath);
    if (!source) {
        return std::nullopt;
    }

    const std::string text{reinterpret_cast<const char*>(source->data()), source->size()};
    auto def = rm::unitbp::load(text, blueprintPath);
    if (!def) {
        rm::log::writef(rm::log::Level::Error, "unit", "blueprint \"%s\" not read: %s",
                        blueprintPath.c_str(), def.error().message.c_str());
        return std::nullopt;
    }

    VfsUnit resolved;
    resolved.def = std::move(*def);
    // `Display.Tarmacs` (`C-289`): the gate `CreateTarmac` checks before it
    // would stamp the footprint — `bp and table.getn(bp) > 0`
    // (defaultunits.lua:80-81). `unitbp::load` does not carry decal-only
    // fields, so the source table is read once more for this one key; the
    // parse is per-type, at registration, not per spawn.
    if (const auto parsed = rm::lua::parseTable(text)) {
        if (const rm::lua::Value* tarmacs = parsed->path("Display", "Tarmacs");
            tarmacs != nullptr && !tarmacs->items.empty()) {
            resolved.hasTarmac = true;
        }
    }

    for (rm::unitdef::Weapon& gun : resolved.def.weapons) {
        if (gun.projectileId.empty()) continue;
        const auto projectile = content.read(gun.projectileId);
        if (!projectile) continue;
        const std::string_view projectileSource{reinterpret_cast<const char*>(projectile->data()),
                                      projectile->size()};
        if (auto traits = rm::unitbp::loadProjectileTraits(projectileSource)) {
            gun.projectileTraits = *traits;
        }
    }

    const std::string meshPath =
        rm::unitbp::resolveMeshInVfs(resolved.def, blueprintPath, content);
    if (meshPath.empty()) {
        rm::log::writef(rm::log::Level::Error, "unit",
                        "\"%s\" has no mesh in the mounted content", resolved.def.name.c_str());
        return std::nullopt;
    }

    const auto meshBytes = content.read(meshPath);
    if (!meshBytes) {
        return std::nullopt;
    }
    auto model = loadModelBytes(*meshBytes);
    if (!model) {
        rm::log::writef(rm::log::Level::Error, "unit", "failed to load mesh \"%s\": %s",
                        meshPath.c_str(), model.error().message.c_str());
        return std::nullopt;
    }
    std::printf("unit %s: %.0f elmos/s, %.2f rad/s, radius %.1f elmos, %.0f hp, %s\n",
                resolved.def.name.c_str(), static_cast<double>(resolved.def.speedElmosPerSecond),
                static_cast<double>(resolved.def.turnRateRadiansPerSecond),
                static_cast<double>(resolved.def.collisionRadiusElmos),
                static_cast<double>(rm::sim::magToFloat(resolved.def.health)),
                rm::unitdef::travelsOnGround(resolved.def.motion) ? "on the ground"
                : resolved.def.canFly                            ? "flying"
                                                                 : "not a ground mover");

    // A mesh read from bytes has no file to take its name from, so it arrives
    // nameless — and the name is what every log line and the batch report identify
    // it by. The blueprint's id is the better answer anyway: it is what the rest of
    // the content calls this unit.
    model->name = resolved.def.name;

    resolved.model = std::move(*model);
    resolved.albedoPath = scmTextureInVfs(meshPath, kScmDiffuseSuffix, content);
    resolved.shadingPath = scmTextureInVfs(meshPath, kScmShadingSuffix, content);
    resolved.normalsPath = scmTextureInVfs(meshPath, kScmNormalsSuffix, content);
    return resolved;
}


void configureIntel(UnitScene& scene, const rm::HeightField& field,
                    rm::sim::VisionStyle style) {
    if (scene.armies.empty()) {
        return;
    }

    // One more than the highest alliance index in use, not the army count: `--alliances 2`
    // on eight armies needs two grids, and sizing by armies would allocate six that nothing
    // ever writes to — on an 8192-elmo map that is megabytes of zeroes per sense.
    int highest = 0;
    for (const rm::sim::Army& army : scene.armies) {
        highest = std::max(highest, army.alliance);
    }

    scene.intel.configure(static_cast<std::size_t>(highest) + 1,
                          rm::sim::fxFromFloat(field.widthElmos()),
                          rm::sim::fxFromFloat(field.depthElmos()), style);

    const rm::sim::IntelGrid& sight = scene.intel.grid(0, rm::sim::IntelKind::Vision);
    std::printf("intel: %s style, %d alliance(s), sight grid %dx%d at %d elmos a square\n",
                style == rm::sim::VisionStyle::Recoil ? "recoil (terrain blocks sight)"
                                                      : "forged alliance (flat discs)",
                highest + 1, sight.squaresX(), sight.squaresZ(),
                sight.squareElmos().floorToInt());
}

void spawnCommanders(UnitScene& scene, const rm::HeightField& field,
                     std::span<const rm::mapinfo::StartPosition> starts,
                     const rm::vfs::Vfs& content, bool observer,
                     std::span<const rm::sim::Faction> factions) {
    if (starts.empty()) {
        rm::log::write(rm::log::Level::Error, "skirmish",
                       "the map declares no start positions");
        return;
    }

    // THE ARMOUR TABLE FIRST, before anything else touches the catalog (PLAN2.md §7 P10.1).
    //
    // `UnitCatalog::setArmor` resolves names when a TYPE IS REGISTERED, so a type added before
    // this call keeps the flat profile it was given and silently ignores armour for the rest of
    // the match. Nothing registers a type before `spawnCommanders`, and this being the first
    // statement in it is what keeps that true.
    //
    // READ FROM THE MOUNTED CONTENT rather than compiled in, because the two installs disagree:
    // retail authors 6 classes with 5 non-1.0 entries, FAF authors 9 with 10, and they differ on
    // the numbers too — a structure takes 0.066666 of an Overcharge under retail and 0.25 under
    // FAF. The importer adds one engine-owned Shield pseudo-class to either registry. See
    // `core/data/ArmorDefs.hpp`.
    //
    // Content with no armour definition gets the `default`-only table, which is exactly the
    // pre-P10.1 engine: every unit ordinary, every weapon a flat scalar. That is why this is
    // safe to run unconditionally and why `make verify` still MATCHes for a scene without it.
    if (const auto armorSource = content.read(std::string{rm::data::kArmorDefinitionPath})) {
        const std::string text{reinterpret_cast<const char*>(armorSource->data()),
                               armorSource->size()};
        rm::data::ArmorTable armor = rm::data::armorTableFromSource(text);
        std::printf("skirmish: armour table holds %zu class(es) and %zu multiplier(s)\n",
                    armor.registry.size(), armor.multipliers.size());
        scene.catalog.setArmor(std::move(armor.registry), std::move(armor.multipliers));
    } else {
        std::printf("skirmish: no %.*s in the mounted content — every unit is ordinary armour\n",
                    static_cast<int>(rm::data::kArmorDefinitionPath.size()),
                    rm::data::kArmorDefinitionPath.data());
    }

    // The roster and the opening, before anything is ordered: both decide WHAT gets built, and
    // the first extractor is ordered during this same setup.
    scene.roster = buildRoster(content);
    if (const auto fromFile = rm::data::loadOpening("data/opening.lua")) {
        scene.opening = *fromFile;
    } else {
        // Not an error. A build run from outside the repo has no `data/` beside it, and
        // `defaultOpening()` states the same plan in C++ precisely so that still plays.
        std::printf("skirmish: no data/opening.lua — using the built-in opening\n");
    }
    std::printf("skirmish: roster holds %zu units; opening builds %zu structure(s),"
                " wave of %zu\n",
                scene.roster.size(), scene.opening.structures.size(),
                scene.opening.waveSize);

    scene.armies = rm::sim::freeForAll(starts.size());

    // `--factions` overrides the round-robin, in seat order and cycled — the mirror match
    // and the chosen matchup. Applied BEFORE any commander loads, since which model a seat
    // needs is exactly what this decides.
    if (!factions.empty()) {
        for (rm::sim::Army& army : scene.armies) {
            army.faction =
                factions[static_cast<std::size_t>(army.index) % factions.size()];
        }
    }

    // One participant per army, the human driving the first. `--armies 4` with two alliances
    // is the 2v2 §7 P2.4 asks to be checked by hand, and `--alliances N` below sets it up.
    //
    // `--observer` seats nobody. Every army gets a script and `playerArmy` stays `kNoArmy`, so
    // nothing is selectable and no click is authorised — which is what watching the scripted
    // opponents play each other means, and it goes through the same authorisation as everything
    // else rather than through a "spectator" special case.
    const int human = observer ? rm::sim::kNoArmy : 0;
    scene.players = rm::sim::onePlayerPerArmy(starts.size(), human);
    scene.playerArmy = human;

    // Faction -> the batch already holding that faction's commander, so four models
    // serve eight armies, and the scale that model needs.
    std::map<rm::sim::Faction, std::size_t> batchForFaction;
    std::map<rm::sim::Faction, float> scaleForFaction;

    for (const rm::sim::Army& army : scene.armies) {
        const std::string path = rm::sim::commanderBlueprintPath(army.faction);

        const auto existing = batchForFaction.find(army.faction);
        if (existing == batchForFaction.end()) {
            const auto unit = resolveUnitFromContent(path, content);
            if (!unit) {
                rm::log::writef(rm::log::Level::Error, "skirmish", "no commander for %s",
                                std::string{rm::sim::factionName(army.faction)}.c_str());
                continue;
            }

            scene.models.push_back(unit->model);
            // FROM THE MOTION CLASS, not from the blueprint's own slope and depth — those
            // govern building placement (P3.4, `core/data/MoveDef.hpp`).
            const rm::data::MoveDef move = rm::data::moveDefFor(unit->def);

            const TurretRig turretRig = resolveTurretRig(scene.models.back(), &unit->def);
            const rm::sca::Animation* unpack = loadUnpackAnimation(scene, content, unit->def);
            const rm::sca::Animation* walk = unit->def.isMobile()
                ? loadWalkAnimation(
                      scene, content, unit->def,
                      rm::unitbp::resolveMeshInVfs(unit->def, path, content))
                : nullptr;
            scene.batches.push_back(rm::UnitBatch{
                .model = &scene.models.back(),
                .instances = {},
                .textures = rm::TexturePair{
                    .diffuse = scene.textures.resolve(content, unit->albedoPath, "albedo"),
                    .shading = scene.textures.resolve(content, unit->shadingPath, "specTeam"),
                },
            .normals = scene.textures.resolve(content, unit->normalsPath, "normalsTS"),
            .animation = walk,
            .animationWalkRate = unit->def.animationWalkRate,
            .builderAim = rm::resolveBuilderAim(scene.models.back(), unit->def.builderArm,
                                                unit->def.buildEffectBones),
                .turretAim = std::move(turretRig.rig),
                .turretWeapon = turretRig.weapon,
                .recoilFlags = std::move(turretRig.recoilFlags),
                .recoilDistanceElmos = turretRig.recoilDistanceElmos,
                .recoilReturnPerTick = turretRig.recoilReturnPerTick,
                .telescopeDistanceElmos = turretRig.telescopeDistanceElmos,
                .telescopeReturnPerTick = turretRig.telescopeReturnPerTick,
                .unpackAnimation = unpack,
                .unpackOneshot = unpack != nullptr,
                .animationDrivenByInstance = walk != nullptr,
            });
            batchForFaction.emplace(army.faction, scene.batches.size() - 1);
            scaleForFaction.emplace(army.faction, unit->def.meshToElmos);

            scene.definitions.push_back(unit->def);
            resolveMuzzleBones(scene.definitions.back(), scene.models.back());
            const rm::UnitTypeIndex type =
                scene.catalog.add(&scene.definitions.back(), gAppTickRate);
            scene.catalog.setAttachBones(
                type, resolveAttachBones(scene.models.back(), unit->def));
            // The type draws with the batch just pushed. A MAP now rather than an identity —
            // see `Scene::batchForType` for why the two numbers had to come apart (`#3090`).
            scene.setBatchForType(type, scene.batches.size() - 1);
            scene.setPathForType(type, path);
            scene.setTypeTraits(type, move, unit->def.meshToElmos);
            publishTurretMount(scene, type, unit->def.meshToElmos);

            const auto armed = static_cast<std::size_t>(std::ranges::count_if(
                unit->def.weapons, [](const rm::unitdef::Weapon& w) { return w.fires(); }));
            std::printf("army %d: %s commander %s, %.0f hp, %zu weapon(s)\n", army.index,
                        std::string{rm::sim::factionName(army.faction)}.c_str(),
                        unit->def.name.c_str(),
                        static_cast<double>(rm::sim::magToFloat(unit->def.health)), armed);

        }

        const std::size_t batch = batchForFaction.at(army.faction);
        const rm::mapinfo::StartPosition& start = starts[static_cast<std::size_t>(army.index)];

        // One commander, on the ground, in its army's colour. `atStartPositions` would
        // place one per start in one call, but each needs a DIFFERENT colour and army,
        // so they are placed one at a time.
        // The blueprint's own scale, NOT 1: a `.scm`'s vertices are not in elmos, and
        // taking them for elmos draws a commander an order of magnitude too big. Same
        // trap as the tanks, one commit earlier (AGENT.md).
        std::vector<rm::UnitInstance> one =
            rm::atStartPositions(field, std::span{&start, 1}, scaleForFaction.at(army.faction));
        if (one.empty()) {
            continue;
        }
        // `atStartPositions` places a `UnitInstance` because that is what the placement
        // helper has always produced; only its position is wanted here, and the colour and
        // scale it also sets are now the draw projection's business.
        const auto type = static_cast<rm::UnitTypeIndex>(batch);
        const rm::unitdef::UnitDef* def = scene.catalog.def(type);
        // FROM THE DEFINITION, like every other spawn. This used to set the army index and
        // nothing else, which left the first unit of every match — the one the player drives —
        // unable to move at any speed. See `motionFor`.
        const rm::sim::MoveState motion =
            def != nullptr ? motionFor(*def, army.index)
                           : rm::sim::MoveState{.armyIndex = army.index};
        const rm::sim::Mag hp = def != nullptr ? def->health : rm::sim::Mag{};
        const rm::sim::UnitId id = scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = transformAt(one.front().position, one.front().rotationY),
            .motion = motion,
            .health = rm::sim::initialHealth(hp, scene.catalog.shield(type).maximum),
        });
        if (def != nullptr) {
            const rm::sim::Mag minimumRate = gAppTickRate.magPerTick(0.1f);
            const rm::sim::Mag buildPerTick =
                std::max(scene.catalog.rates(type).buildPerTick, minimumRate);
            for (std::size_t weapon = 0; weapon < def->weapons.size(); ++weapon) {
                const rm::unitdef::Weapon& gun = def->weapons[weapon];
                if (!gun.countedProjectile || gun.enabledByEnhancement
                    || gun.projectileTraits.buildTime <= rm::sim::Mag{}) {
                    continue;
                }
                // C-241 names an adjacency build modifier. It is fixed at 1 in this slice:
                // per-unit adjacency is not threaded into silo-event demand yet.
                // One record per retail silo SLOT (C-081): same-slot duplicates keep the
                // FIRST weapon, the selection C-085 records for `GetCountedProjectileWeapon`.
                const bool slotTaken = std::any_of(
                    scene.siloAmmo.begin(), scene.siloAmmo.end(),
                    [&](const rm::sim::SiloAmmo& existing) {
                        return existing.owner == id
                               && existing.slot == static_cast<std::uint8_t>(gun.nukeWeapon);
                    });
                if (slotTaken) continue;
                scene.siloAmmo.push_back(rm::sim::makeSiloAmmo(
                    id, weapon, gun.nukeWeapon, gun.maxProjectileStorage,
                    {.mass = gun.projectileTraits.buildCostMass,
                     .energy = gun.projectileTraits.buildCostEnergy},
                    gun.projectileTraits.buildTime, buildPerTick));
            }
            // A missile redirector (`Defense.AntiMissile`, URL0303, `C-088`) — one per unit,
            // created alongside the silo records above. The rate-to-ticks conversion is
            // content-side work done once here, at load (PLAN2.md §5.1).
            if (def->antiMissileRadiusElmos > rm::sim::Fx{}
                && def->antiMissileRatePerSecond > 0.0f) {
                const int cooldown = static_cast<int>(gAppTickRate.ticks(
                    rm::sim::Seconds{1.0f / def->antiMissileRatePerSecond}));
                scene.redirects.push_back(
                    rm::sim::makeMissileRedirect(id, def->antiMissileRadiusElmos, cooldown));
            }
        }
    }

    // The batches' spans are filled from the store, once, here. They used to be re-pointed
    // after every push_back because a vector that grew had moved its storage; the store owns
    // that storage now and the gather hands each batch a contiguous run.
    // Published before the gather because the gather reads snapshots now, and outside a tick
    // there is no tick to have published one. Twice, so `previous` and `current` are the same
    // state — a scene that has only just been built has nothing to interpolate.
    scene.publish(0);
    scene.publish(0);
    scene.gatherForDrawing();

    // Each army starts with the commander's trickle and one extractor's worth of storage,
    // which is what makes the first build affordable — see kCommanderTrickle.
    scene.commandersEver = scene.countCommanders();
    scene.economies.assign(scene.armies.size(), rm::sim::Economy{});
    for (rm::sim::Economy& economy : scene.economies) {
        // The commander's trickle, per tick. `recomputeIncome` recomputes this every tick
        // from what is standing; seeding it here is what makes the first tick's earnings
        // spendable before anything has been counted.
        economy.incomePerTick = rm::sim::Resources{
            .mass = gAppTickRate.magPerTick(rm::sim::kCommanderTrickleMassPerSecond),
            .energy = gAppTickRate.magPerTick(rm::sim::kCommanderTrickleEnergyPerSecond),
        };
        economy.storage = {};
        // EMPTY at spawn, which is what retail does — `SetArmyEconomy` seeds from
        // the map's `_save.lua` `Economy` record (nothing for a stock skirmish),
        // and the bank a match opens with is the ACU's `GiveInitialResources`
        // grant landing on the fifth beat (`kInitialResourceGrantTick`), not a
        // starting balance. Milestone 19's empty start stalled only because the
        // grant did not exist yet.
        economy.stored = {};
    }

    std::printf("skirmish: %zu armies, %zu commander model(s)\n", scene.armies.size(),
                batchForFaction.size());
}

/// Spawns one unit of `blueprintPath` for `army` at `position`, creating its batch on
/// first use so twenty tanks stay one draw call.
///
/// The milestone-20 counterpart of spawnCommanders, and the moment a finished
/// Construction stops being a number in the economy and becomes a thing on the map:
/// grounded on the height field, tilted onto its slope, in its army's colour, with its
/// definition's own health, radius and speed. Returns the batch and instance it landed
/// in, or nothing when the blueprint or its model is not in the mounted content.
[[nodiscard]] std::optional<rm::UnitTypeIndex> ensureDrawableType(UnitScene& scene,
                                                                  const rm::vfs::Vfs& content,
                                                                  std::string_view blueprintPath) {
    auto found = scene.typeForBlueprint.find(blueprintPath);
    if (found == scene.typeForBlueprint.end()) {
        const auto unit = resolveUnitFromContent(std::string{blueprintPath}, content);
        if (!unit) {
            rm::log::writef(rm::log::Level::Error, "spawn",
                            "no unit at %s in the mounted content",
                            std::string{blueprintPath}.c_str());
            return std::nullopt;
        }

        scene.models.push_back(unit->model);
        // From the MOTION CLASS. The fallback rules that used to be here — "a structure's zero
        // slope means the default, and a ground mover's zero depth means does not wade" — were
        // guesses standing in for a fact the blueprint had all along: `RULEUMT_*` says what this
        // unit crosses (P3.4). ADR-027 said passability comes from motion class; now it does.
        const rm::data::MoveDef move = rm::data::moveDefFor(unit->def);
        const TurretRig turretRig = resolveTurretRig(scene.models.back(), &unit->def);
        const rm::sca::Animation* unpack = loadUnpackAnimation(scene, content, unit->def);
        // The walk cycle the blueprint declares, else the one beside the mesh.
        // Only mobile types look: a structure's directory holds no walk clip,
        // and the list call that proves it costs a scan per type.
        const rm::sca::Animation* walk = nullptr;
        if (unit->def.isMobile()) {
            walk = loadWalkAnimation(
                scene, content, unit->def,
                rm::unitbp::resolveMeshInVfs(unit->def, blueprintPath, content));
        }
        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = {},
            .textures = rm::TexturePair{
                .diffuse = scene.textures.resolve(content, unit->albedoPath, "albedo"),
                .shading = scene.textures.resolve(content, unit->shadingPath, "specTeam"),
            },
            .normals = scene.textures.resolve(content, unit->normalsPath, "normalsTS"),
            .animation = walk,
            .animationWalkRate = unit->def.animationWalkRate,
            .builderAim = rm::resolveBuilderAim(scene.models.back(), unit->def.builderArm,
                                                unit->def.buildEffectBones),
            .turretAim = std::move(turretRig.rig),
            .turretWeapon = turretRig.weapon,
            .recoilFlags = std::move(turretRig.recoilFlags),
            .recoilDistanceElmos = turretRig.recoilDistanceElmos,
            .recoilReturnPerTick = turretRig.recoilReturnPerTick,
            .telescopeDistanceElmos = turretRig.telescopeDistanceElmos,
            .telescopeReturnPerTick = turretRig.telescopeReturnPerTick,
            .unpackAnimation = unpack,
            .unpackOneshot = unpack != nullptr,
            .animationDrivenByInstance = walk != nullptr,
        });
        scene.definitions.push_back(unit->def);
        resolveMuzzleBones(scene.definitions.back(), scene.models.back());
        const rm::UnitTypeIndex type =
            scene.catalog.add(&scene.definitions.back(), gAppTickRate);
        scene.catalog.setAttachBones(
            type, resolveAttachBones(scene.models.back(), unit->def));
        scene.setBatchForType(type, scene.batches.size() - 1);
        scene.setPathForType(type, blueprintPath);
        scene.setTypeTraits(type, move, unit->def.meshToElmos);
        // `Display.Tarmacs` (`C-289`): the flag `stampTarmacAt` gates on,
        // resolved once at registration like the rest of the type's traits.
        scene.tarmacForType[static_cast<std::size_t>(type)] =
            unit->hasTarmac ? std::uint8_t{1} : std::uint8_t{0};
        // The sim's copy of the ring (see publishTurretMount): fine mesh only.
        // The coarse batch reuses this mount; resolving it again would
        // overwrite it with merged-bone approximations.
        publishTurretMount(scene, type, unit->def.meshToElmos);

        // The coarse mesh, when the blueprint declares one: its own batch, drawn instead
        // of the fine one past the cutoff (Scene::lodOfType; the gather routes). Found by
        // convention beside the blueprint, with its own albedo when it names one — a far
        // unit with the fine texture on coarse UVs would smear, and the LOD art ships its
        // own exactly because of that.
        if (unit->def.hasLod1 && unit->def.lodCutoff > 0.0f) {
            const std::string coarsePath =
                rm::unitbp::resolveMeshInVfs(unit->def, blueprintPath, content, 1);
            if (!coarsePath.empty()) {
                if (const auto bytes = content.read(coarsePath)) {
                    if (auto coarse = rm::scm::load(*bytes)) {
                        scene.models.push_back(std::move(*coarse));
                        const std::string dir =
                            std::string{blueprintPath.substr(0, blueprintPath.rfind('/') + 1)};
                        const std::string albedo = unit->def.lod1Albedo.empty()
                                                       ? unit->albedoPath
                                                       : dir + unit->def.lod1Albedo;
                        const std::string shading = unit->def.lod1Spec.empty()
                                                        ? unit->shadingPath
                                                        : dir + unit->def.lod1Spec;
                        const TurretRig turretLod =
                            resolveTurretRig(scene.models.back(), &unit->def);
                        scene.batches.push_back(rm::UnitBatch{
                            .model = &scene.models.back(),
                            .instances = {},
                            .textures = rm::TexturePair{
                                .diffuse = scene.textures.resolve(content, albedo, "albedo"),
                                .shading =
                                    scene.textures.resolve(content, shading, "specTeam"),
                            },
                            .normals = scene.textures.resolve(
                                content,
                                scmTextureInVfs(coarsePath, kScmNormalsSuffix, content),
                                "normalsTS"),
                            .builderAim = rm::resolveBuilderAim(
                                scene.models.back(), unit->def.builderArm,
                                unit->def.buildEffectBones),
                            .turretAim = turretLod.rig,
                            .turretWeapon = turretLod.weapon,
                            .recoilFlags = std::move(turretLod.recoilFlags),
                            .recoilDistanceElmos = turretLod.recoilDistanceElmos,
                            .recoilReturnPerTick = turretLod.recoilReturnPerTick,
                            .telescopeDistanceElmos = turretLod.telescopeDistanceElmos,
                            .telescopeReturnPerTick = turretLod.telescopeReturnPerTick,
                        });
                        scene.lodOfType[type] = UnitScene::LodLevel{
                            .batch = scene.batches.size() - 1,
                            .cutoffElmos =
                                unit->def.lodCutoff * UnitScene::kLodElmosPerCutoff,
                        };
                        std::printf("  lod: %s coarse mesh beyond %.0f elmos\n",
                                    unit->def.name.c_str(),
                                    static_cast<double>(
                                        scene.lodOfType[type].cutoffElmos));
                    }
                }
            }
        }

        found = scene.typeForBlueprint.emplace(std::string{blueprintPath}, type).first;
    }
    return found->second;
}

/// `StructureUnit.FlattenSkirt` (C-286, defaultunits.lua:68-73): a structure on the
/// LAND layer flattens its skirt rect to its own Y the moment it exists — retail runs
/// it from `OnCreate`, and our unit entity materialises at spawn. The skirt is the gate:
/// `Physics.FlattenSkirt` is unparsed, and a skirtless structure's rect is empty, which
/// is retail's no-op anyway. Floating, seabed and airborne structures are not Land —
/// the same exclusion retail's `GetCurrentLayer() == 'Land'` makes.
///
/// Shared by `spawnUnit` and the `resolveUnits` crowd path: both are "the unit now
/// exists" seams, and retail flattens scenario-placed structures the same way
/// (`CreateUnitHPR` → `OnCreate`).
static void flattenSkirtAt(UnitScene& scene, const rm::HeightField& field,
                           rm::UnitTypeIndex type, const rm::sim::Transform& transform,
                           const rm::sim::MoveState& motion) {
    rm::sim::Terrain terrain = scene.terrain(field);
    const rm::sim::UnitCatalog::AdjacencyInfo& skirt = scene.catalog.adjacency(type);
    if (skirt.participates() && !motion.surfaceWater && !motion.airborne
        && !motion.submersible
        && (!terrain.hasWater() || transform.y >= terrain.waterLevel())) {
        const rm::sim::Fx cx = transform.x + skirt.skirtCentreOffsetXElmos;
        const rm::sim::Fx cz = transform.z + skirt.skirtCentreOffsetZElmos;
        terrain.flattenRect(cx - skirt.skirtHalfXElmos, cz - skirt.skirtHalfZElmos,
                            cx + skirt.skirtHalfXElmos, cz + skirt.skirtHalfZElmos,
                            transform.y);
    }
}

/// `StructureUnit.CreateTarmac`'s terrain half (`C-289`): retail's
/// `SetTerrainTypeRect(self.tarmacRect, {TypeCode = factionIndex + 189})` —
/// commented out in shipped Lua (`defaultunits.lua:99`, "disabling this for
/// now") because the write never dirtied retail's path tables. Ours is live:
/// a structure whose blueprint carries `Display.Tarmacs` stamps its
/// footprint with the owner's faction tarmac code (UEF 190, Aeon 191, Cybran
/// 192 — TerrainTypes.lua:1714-1746; Seraphim's 193 is unauthored but the
/// formula's answer), and the tick's owner sweep lifts it when the structure
/// is gone — `DestroyTarmac`'s counterpart.
///
/// The gates are retail's own: `GetCurrentLayer() == 'Land'` (the same
/// exclusion `flattenSkirtAt` makes), a non-empty `Display.Tarmacs`, and a
/// footprint to stamp — `self.tarmacRect` is never assigned in shipped Lua,
/// so the footprint rect is the only rect the dead call could have meant.
/// The stamp is keyed to the unit so the sweep knows whose it is.
static void stampTarmacAt(UnitScene& scene, rm::UnitTypeIndex type,
                          rm::sim::UnitId id, const rm::sim::Transform& transform,
                          const rm::sim::MoveState& motion,
                          const rm::sim::Army& army) {
    const auto index = static_cast<std::size_t>(type);
    const rm::unitdef::UnitDef* def = scene.catalog.def(type);
    if (def == nullptr || index >= scene.tarmacForType.size()
        || scene.tarmacForType[index] == 0 || motion.surfaceWater
        || motion.airborne || motion.submersible
        || (scene.hasWater && transform.y < rm::sim::fxFromFloat(scene.waterLevelElmos))) {
        return;
    }
    // `factionIndex + 189` on retail's 1-based index; `Faction` is 0-based.
    const auto type8 = static_cast<std::uint8_t>(
        std::min(255, static_cast<int>(army.faction) + 1 + 189));
    const rm::sim::Fx halfX =
        rm::sim::Fx::fromInt(std::max(1, def->footprintSquaresX) * rm::kSquareSize / 2);
    const rm::sim::Fx halfZ =
        rm::sim::Fx::fromInt(std::max(1, def->footprintSquaresZ) * rm::kSquareSize / 2);
    scene.terrainTypeGrid.stamp(transform.x - halfX, transform.z - halfZ,
                                transform.x + halfX, transform.z + halfZ, type8, id);
}

[[nodiscard]] std::optional<rm::sim::UnitId> spawnUnit(UnitScene& scene,
                                                          const rm::vfs::Vfs& content,
                                                          const rm::HeightField& field,
                                                          std::string_view blueprintPath,
                                                          std::array<float, 3> position,
                                                          const rm::sim::Army& army,
                                                          rm::Brad yaw) {
    const std::optional<rm::UnitTypeIndex> ensured =
        ensureDrawableType(scene, content, blueprintPath);
    if (!ensured) {
        return std::nullopt;
    }

    const rm::UnitTypeIndex type = *ensured;
    const rm::unitdef::UnitDef& def = *scene.catalog.def(type);

    rm::sim::Transform transform;
    transform.x = rm::sim::fxFromFloat(position[0]);
    transform.z = rm::sim::fxFromFloat(position[2]);
    // The heading arrives already in brads: it enters hashed sim state here, and the
    // conversion from radians runs in whoever produced it — which is fixed point for a
    // bearing derived from positions (see the factory roll-off call in Match.cpp), never
    // libm's atan2.
    transform.heading = yaw;

    const rm::sim::MoveState motion = motionFor(def, army.index);
    rm::sim::Terrain terrain = scene.terrain(field);
    rm::sim::placeOnMotionLayer(transform, motion, terrain);

    flattenSkirtAt(scene, field, type, transform, motion);

    const rm::sim::UnitId id = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = transform,
        .motion = motion,
        .health = rm::sim::initialHealth(def.health, scene.catalog.shield(type).maximum),
    });
    // `CreateTarmac`'s terrain half (`C-289`): the pad stamps the moment the
    // unit exists — retail runs it from `OnCreate` like `FlattenSkirt`, and
    // the stamp is keyed to this id so the tick's sweep lifts it on death.
    stampTarmacAt(scene, type, id, transform, motion, army);

    // A flyer spawns cruising at its placed altitude: seed the reference the lift law
    // chases from the same ground-plus-clearance the align pass would assign (`C-221`).
    if (motion.canFly) {
        scene.store.motion()[id.index].altitudeRef =
            scene.terrain(field).heightAt(transform.x, transform.z)
            + rm::sim::kAirClearanceElmos;
    }

    const rm::sim::Mag minimumRate = gAppTickRate.magPerTick(0.1f);
    const rm::sim::Mag buildPerTick =
        std::max(scene.catalog.rates(type).buildPerTick, minimumRate);
    for (std::size_t weapon = 0; weapon < def.weapons.size(); ++weapon) {
        const rm::unitdef::Weapon& gun = def.weapons[weapon];
        // Enhancement weapons are disabled at spawn, including their automatic ammo bill.
        if (!gun.countedProjectile || gun.enabledByEnhancement
            || gun.projectileTraits.buildTime <= rm::sim::Mag{}) continue;
        // C-241's queued builds count toward capacity; queue issuance is deliberately absent.
        // One record per retail silo SLOT (C-081): a unit with two counted weapons on the
        // same slot keeps the FIRST, the order-dependent selection C-085 records for
        // `Unit::GetCountedProjectileWeapon` (0x006b1fb0).
        const bool slotTaken = std::any_of(
            scene.siloAmmo.begin(), scene.siloAmmo.end(), [&](const rm::sim::SiloAmmo& existing) {
                return existing.owner == id
                       && existing.slot == static_cast<std::uint8_t>(gun.nukeWeapon);
            });
        if (slotTaken) continue;
        scene.siloAmmo.push_back(rm::sim::makeSiloAmmo(
            id, weapon, gun.nukeWeapon, gun.maxProjectileStorage,
            {.mass = gun.projectileTraits.buildCostMass,
             .energy = gun.projectileTraits.buildCostEnergy},
            gun.projectileTraits.buildTime, buildPerTick));
    }
    // A missile redirector (`Defense.AntiMissile`, URL0303, `C-088`) — see above.
    if (def.antiMissileRadiusElmos > rm::sim::Fx{}
        && def.antiMissileRatePerSecond > 0.0f) {
        const int cooldown = static_cast<int>(
            gAppTickRate.ticks(rm::sim::Seconds{1.0f / def.antiMissileRatePerSecond}));
        scene.redirects.push_back(
            rm::sim::makeMissileRedirect(id, def.antiMissileRadiusElmos, cooldown));
    }

    // `UnitCreated` is THE CALLER'S to raise (§7 P6.1). The sim never spawns a unit — a spawn
    // needs a model out of the VFS, which is exactly the line the sim does not cross — so this
    // is the one event kind that cannot come from a pass. Raised here rather than at each of
    // the three spawn sites, because this is the one that loads content.
    scene.events.emit(rm::sim::Event{
        .kind = rm::sim::EventKind::UnitCreated,
        .unit = id,
        .army = motion.armyIndex,
        .at = {transform.x, transform.y, transform.z},
    });

    // The store may have grown its arrays, so any span into them is stale — which for the
    // sim means the caller-side tick has to re-read them, and for the renderer means the
    // draw gather has to run again before the batch spans are trusted.
    scene.grewThisTick = true;

    return id;
}

/// A unit trial: the named blueprint for the first army at the middle of the
/// starts, and a crowd of enemy T1 scouts and engineers a gap away facing it.
/// in the middle. Engineers are the bulk of the crowd with a couple of scouts
/// that find the trial unit fast; the engineers stand around being shot at.
void stageTrial(UnitScene& scene, const rm::HeightField& field,
                std::span<const rm::mapinfo::StartPosition> starts,
                const rm::vfs::Vfs& content, std::string_view unitId, std::size_t count,
                std::size_t foes, float gapElmos) {
    scene.trialAlignmentDebug = true;
    if (starts.empty() || scene.armies.size() < 2 || count == 0) {
        rm::log::write(rm::log::Level::Warn, "trial",
                        "needs starts and two armies; ignored");
        return;
    }
    const rm::sim::Army& home = scene.armies.front();
    const rm::sim::Army* away = nullptr;
    for (const rm::sim::Army& army : scene.armies) {
        if (army.alliance != home.alliance) {
            away = &army;
            break;
        }
    }
    if (away == nullptr) {
        away = &scene.armies[1];
    }
    float middleX = 0.0f;
    float middleZ = 0.0f;
    for (const rm::mapinfo::StartPosition& start : starts) {
        middleX += start.x;
        middleZ += start.z;
    }
    middleX /= static_cast<float>(starts.size());
    middleZ /= static_cast<float>(starts.size());
    // Brads from radians: the full turn is the identity by unsigned wraparound.
    constexpr float kTurn = static_cast<float>(rm::sim::kBradFullTurn)
                          / (2.0f * std::numbers::pi_v<float>);
    // The crowd sits off the trial unit's nose, not dead ahead: a turret that
    // never leaves rest is indistinguishable from a frozen one, and the point
    // of the stage is watching it track.
    constexpr float kLateral = 0.6f;
    const rm::Brad faceAway =
        static_cast<rm::Brad>(std::atan2(gapElmos, gapElmos * kLateral) * kTurn);
    const rm::Brad faceHome =
        static_cast<rm::Brad>(std::atan2(-gapElmos, -gapElmos * kLateral) * kTurn);
    const std::string trialPath =
        "/units/" + std::string{unitId} + "/" + std::string{unitId} + "_unit.bp";
    std::size_t stood = 0;
    std::optional<rm::sim::UnitId> firstTrial;
    for (std::size_t i = 0; i < count; ++i) {
        if (const auto id = spawnUnit(scene, content, field, trialPath,
                                      {middleX + static_cast<float>(i) * 25.0f, 0.0f, middleZ},
                                      home, faceAway)) {
            if (!firstTrial) firstTrial = *id;
            ++stood;
        }
    }
    // A TRANSPORT TRIAL STAGES ITS CARGO TOO: a line of same-army engineers beside the
    // carriers, each issued the real LoadTransport order, so a headless capture shows
    // boarding instead of parked hardware. The order goes through submitCommand like a
    // click would — the command log records it and a replay re-runs the lift.
    if (firstTrial && scene.store.alive(*firstTrial)) {
        const rm::unitdef::UnitDef* trialDef =
            scene.catalog.def(scene.store.typeAt(firstTrial->index));
        if (trialDef != nullptr && trialDef->isTransport()) {
            const std::string homeCommander = rm::sim::commanderBlueprintId(home.faction);
            const std::string homePrefix =
                homeCommander.size() >= 3 ? homeCommander.substr(0, 3) : "UEL";
            const std::string cargoPath =
                "/units/" + homePrefix + "0105/" + homePrefix + "0105_unit.bp";
            const rm::sim::Transform& carrierAt =
                scene.store.transforms()[firstTrial->index];
            const rm::PlayerIndex player = playerDriving(scene, home.index);
            std::size_t boarded = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                const auto cargo = spawnUnit(scene, content, field, cargoPath,
                    {middleX + 25.0f + static_cast<float>(i) * 8.0f, 0.0f,
                     middleZ - 40.0f},
                    home, faceAway);
                if (!cargo) continue;
                boarded += submitCommand(scene, rm::sim::CommandIssue{
                    .phase = rm::sim::CommandPhase::PreTick,
                    .source = static_cast<rm::CommandSource>(player),
                    .player = player,
                    .kind = rm::sim::CommandKind::LoadTransport,
                    .units = {*cargo},
                    .targetX = carrierAt.x,
                    .targetZ = carrierAt.z,
                    .target = *firstTrial,
                }).has_value() ? 1 : 0;
            }
            std::printf("trial cargo: %zu engineer(s) ordered aboard\n", boarded);
        }
    }
    // The foe prefix from its commander's id: UEL0001 fields UEL scouts.
    const std::string foeCommander = rm::sim::commanderBlueprintId(away->faction);
    const std::string prefix =
        foeCommander.size() >= 3 ? foeCommander.substr(0, 3) : "UEL";
    const std::string scoutPath =
        "/units/" + prefix + "0101/" + prefix + "0101_unit.bp";
    const std::string engineerPath =
        "/units/" + prefix + "0105/" + prefix + "0105_unit.bp";
    // Engineers are the bulk: a crowd to chew through, spread wide enough that
    // one shot rarely catches two. A couple of scouts still find the trial
    // unit fast.
    const std::size_t scouts = std::max<std::size_t>(2, foes / 4);
    std::size_t crowded = 0;
    for (std::size_t i = 0; i < scouts + foes; ++i) {
        const std::string& path = i < scouts ? scoutPath : engineerPath;
        const float lane = static_cast<float>(i % 4) * 26.0f;
        const float rank = static_cast<float>(i / 4) * 26.0f;
        if (spawnUnit(scene, content, field, path,
                      {middleX + gapElmos + lane, 0.0f,
                       middleZ + gapElmos * kLateral + rank},
                      *away, faceHome)) {
            ++crowded;
        }
    }
    std::printf("trial: %zu %s for army %d, %zu foes for army %d at (%.0f, %.0f)\n", stood,
                std::string{unitId}.c_str(), home.index, crowded, away->index,
                static_cast<double>(middleX), static_cast<double>(middleZ));
}

/// Finds (or loads and registers) the buildable entry for `blueprintPath`, so every
/// Construction of the same blueprint shares one definition and one index.
/// Registers a blueprint as buildable and returns its TYPE INDEX.
///
/// A `UnitTypeIndex`, not an index into a private list — which is what closes the `Build` hole
/// in `sim::applyCommand`. There used to be two registries: `scene.buildable` for things under
/// construction and `scene.definitions`/`scene.catalog` for things standing on the map, with a
/// `Construction::blueprintIndex` meaning "an index into whatever list the caller is building
/// from". The sim could not create a construction because it had no way to name a blueprint the
/// caller would recognise.
///
/// One registry fixes that: a type index means the same thing to the sim, the catalog and the
/// draw gather, and `buildablePaths` is now indexed BY type index so a finished construction can
/// still find its model.
[[nodiscard]] std::optional<rm::UnitTypeIndex> resolveBuildable(UnitScene& scene,
                                                                const rm::vfs::Vfs& content,
                                                                std::string_view blueprintPath) {
    // ONE REGISTRY (`#3090`). This used to keep its own `scene.buildable` list and return an
    // index into it, typed `rm::UnitTypeIndex` — a different number from the catalog's under the
    // same type name, which is why routing a build order through `sim::applyCommand` turned a
    // 36-mass extractor into an 18,000-mass experimental.
    //
    // Now a buildable type is a type: registered with the catalog like anything else, with NO
    // BATCH until something of it is actually built. That is what `Scene::batchForType` bought —
    // the gather no longer assumes a type index is a batch index, so a type may exist with
    // nothing to draw it, which is precisely what "buildable but not yet built" means.
    for (std::size_t type = 0; type < scene.pathForType.size(); ++type) {
        if (scene.pathForType[type] == blueprintPath) {
            return static_cast<rm::UnitTypeIndex>(type);
        }
    }

    const auto bytes = content.read(std::string{blueprintPath});
    if (!bytes) {
        rm::log::writef(rm::log::Level::Error, "economy",
                        "no blueprint at %s in the mounted content",
                        std::string{blueprintPath}.c_str());
        return std::nullopt;
    }
    auto def = rm::unitbp::load(
        std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()},
        std::string{blueprintPath});
    if (!def) {
        return std::nullopt;
    }
    for (rm::unitdef::Weapon& gun : def->weapons) {
        if (gun.projectileId.empty()) continue;
        const auto projectile = content.read(gun.projectileId);
        if (!projectile) continue;
        const std::string_view projectileSource{reinterpret_cast<const char*>(projectile->data()),
                                      projectile->size()};
        if (auto traits = rm::unitbp::loadProjectileTraits(projectileSource)) {
            gun.projectileTraits = *traits;
        }
    }

    scene.definitions.push_back(*def);
    const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back(), gAppTickRate);
    scene.setPathForType(type, blueprintPath);
    // The traits too, even though nothing of this type is standing yet: they are indexed by
    // type, so leaving a hole would make every LATER type read the wrong slope limit and the
    // wrong model scale. That is the bug this refactor introduced and then caught — see
    // `Scene::setTypeTraits`.
    const rm::data::MoveDef move = rm::data::moveDefFor(*def);
    scene.setTypeTraits(type, move, def->meshToElmos);
    // Deliberately NO `setBatchForType`: nothing of this type exists yet. `spawnUnit` records
    // the batch when the first one is built.
    return type;
}

/// Orders every commander to build a mass extractor on its nearest deposit.
///
/// The whole of milestone 19's build order, and deliberately not an AI: one structure,
/// on the site the MAP names, paid for out of the trickle a commander produces. What it
/// demonstrates is the chain — a marker becomes a site, a blueprint becomes a cost, and
/// the cost is met over time rather than at once. Milestone 20's scripted opponent
/// (core/sim/BuildOrder.hpp) continues from exactly this point.
///
/// The extractor is the right first thing for the same reason it is in the game: it is the
/// cheapest structure that pays for the next one.
/// The player driving an army, or none. What an issued order is attributed to.
rm::PlayerIndex playerDriving(const UnitScene& scene, int army) {
    for (const rm::sim::Player& player : scene.players) {
        if (rm::sim::commands(player, army)) {
            return player.index;
        }
    }
    return 0;
}

std::optional<rm::CommandId> submitCommand(UnitScene& scene, rm::sim::CommandIssue issue) {
    const bool tracing = rm::log::enabled(rm::log::Level::Debug);
    const auto recipients = tracing ? issue.units : std::vector<rm::sim::UnitId>{};
    const auto id = scene.commandInput.submit(std::move(issue), scene.store);
    for (const auto unit : recipients) {
        rm::log::writef(rm::log::Level::Debug, "order",
            "tick=%llu command=%u unit=%u:%u stage=%s kind=%s player=%u",
            static_cast<unsigned long long>(issue.tick), id.value_or(rm::kInvalidCommandId),
            unit.index, unit.generation, id ? "submitted" : "intake-rejected",
            rm::sim::commandKindName(issue.kind), static_cast<unsigned>(issue.player));
    }
    return id;
}

std::vector<DispatchedCommand> dispatchCommands(UnitScene& scene,
                                                   const rm::HeightField& field,
                                                   PassabilitySet& passability,
                                                   rm::TickIndex tick,
                                                   rm::sim::CommandPhase phase,
                                                   rm::sim::PathService* pathService,
                                                   rm::sim::ScriptTaskHost* scriptTasks) {
    std::vector<DispatchedCommand> dispatched;
    std::vector<rm::sim::CommandIssue> due = scene.commandInput.take(tick, phase);
    dispatched.reserve(due.size());
    const rm::sim::Terrain terrain = scene.terrain(field);
    for (rm::sim::CommandIssue& issue : due) {
        // The LAYER grid, not just the type grid: a submerged submarine is
        // answered by its own (usually shallower) water grid at intake too —
        // C-205's `Sub` admission — while a surfaced hull or anything that
        // never dives keeps the one it had.
        const auto layerGridForUnit =
            [&](rm::sim::UnitId unit) -> const rm::sim::PassabilityGrid* {
            if (!scene.store.alive(unit)) {
                return nullptr;
            }
            const auto unitType = static_cast<std::size_t>(scene.store.typeAt(unit.index));
            const rm::sim::MoveState& motion = scene.store.motion()[unit.index];
            if (motion.submersible && motion.submerged) {
                return &passability.gridForSubmerged(scene, unitType);
            }
            return &passability.gridFor(scene, unitType);
        };
        const auto gridForUnit = [&](rm::sim::UnitId unit) -> const rm::sim::PassabilityGrid* {
            if (!scene.store.alive(unit)) {
                return nullptr;
            }
            const auto unitType = static_cast<std::size_t>(scene.store.typeAt(unit.index));
            if (issue.kind == rm::sim::CommandKind::Build) {
                return &passability.gridForBuild(
                    scene, static_cast<std::size_t>(issue.buildType), unitType);
            }
            return layerGridForUnit(unit);
        };
        rm::sim::ApplyCommandResult result = rm::sim::applyCommand(
            issue, scene.store, scene.catalog, scene.players, scene.armies, terrain, gridForUnit,
            gAppTickRate, &scene.building, &scene.events, &scene.features, pathService,
            scriptTasks, layerGridForUnit, &scene.siloAmmo, &scene.siloQueue);
        if (rm::log::enabled(rm::log::Level::Debug)) {
            for (const auto unit : issue.units) {
                rm::log::writef(rm::log::Level::Debug, "order",
                    "tick=%llu command=%u unit=%u:%u stage=%s kind=%s player=%u",
                    static_cast<unsigned long long>(tick), issue.id, unit.index, unit.generation,
                    result.acceptedUnit(unit) ? "accepted" : "rejected",
                    rm::sim::commandKindName(issue.kind), static_cast<unsigned>(issue.player));
            }
        }
        issue.units = result.accepted;
        if (!scene.commands.record(issue)) {
            throw std::logic_error{"command dispatcher produced an invalid semantic log order"};
        }
        dispatched.push_back(DispatchedCommand{.recorded = std::move(issue),
                                                .result = std::move(result)});
    }
    return dispatched;
}

bool issueBuild(UnitScene& scene, rm::sim::UnitId builder,
                                rm::PlayerIndex player, rm::TickIndex tick,
                                rm::UnitTypeIndex type, rm::sim::Fx atX, rm::sim::Fx atZ,
                                bool queued) {
    return submitCommand(scene, rm::sim::CommandIssue{
        .tick = tick,
        .phase = rm::sim::CommandPhase::PreTick,
        .source = static_cast<rm::CommandSource>(player),
        .player = player,
        .kind = rm::sim::CommandKind::Build,
        .queued = queued,
        .units = {builder},
        .targetX = atX,
        .targetZ = atZ,
        .buildType = type,
    }).has_value();
}

std::size_t adoptOwnerlessUnits(UnitScene& scene) {
    if (scene.playerArmy == rm::sim::kNoArmy) {
        return 0;  // an observer owns nothing, which is the point of `--observer`
    }

    std::size_t adopted = 0;
    for (rm::sim::MoveState& motion : scene.store.motion()) {
        if (motion.armyIndex == rm::sim::kNoArmy) {
            motion.armyIndex = scene.playerArmy;
            ++adopted;
        }
    }
    return adopted;
}

void orderFirstExtractors(UnitScene& scene, std::span<const rm::scenario::Marker> markers,
                          const rm::vfs::Vfs& content, const rm::HeightField& field,
                          PassabilitySet& passability) {
    if (scene.armies.empty()) {
        return;
    }

    // The extractor blueprint, PER FACTION, from the opening's first step — which is what
    // P3.3 bought. This used to register one UEF path for everybody, with a comment saying
    // that four blueprints "would demonstrate nothing the one does not". Four blueprints is
    // now zero blueprints: the plan says `role = 'extractor'` and the roster says which.
    //
    // Resolved per army below rather than once here, because the answer differs by faction.
    if (scene.opening.structures.empty()) {
        return;

    }

    // Which commander is where, so each can be sent to its OWN nearest deposit rather than
    // all of them to one.
    float firstBuilderRate = 1.0f;
    // The last extractor's figures, for the summary line. Reported rather than assumed
    // identical across factions, because they are not: each faction's mex costs its own.
    rm::sim::Mag lastCost{};
    rm::sim::Mag lastEnergy{};
    rm::sim::Mag lastTime{};
    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !def->isBuilder()) {
            continue;
        }
        const int army = scene.store.motion()[slot].armyIndex;
        if (army == rm::sim::kNoArmy
            || static_cast<std::size_t>(army) >= scene.armies.size()) {
            continue;
        }
        firstBuilderRate = std::max(1.0f, def->buildRate);

        // PER FACTION. The extractor this commander builds is its own faction's, resolved from
        // the opening's first step through the roster — which is the whole of what P3.3 bought.
        const std::string blueprint = blueprintFor(
            scene, scene.armies[static_cast<std::size_t>(army)],
            extractorStep(scene.opening));
        const std::optional<rm::UnitTypeIndex> registered =
            blueprint.empty() ? std::nullopt : resolveBuildable(scene, content, blueprint);
        if (!registered) {
            continue;  // this faction fields no extractor this engine can read
        }
        const rm::unitdef::UnitDef* extractor = scene.catalog.def(*registered);

        const std::array<rm::sim::Fx, 3> from =
            rm::sim::positionOf(scene.store.transforms()[slot]);
        const rm::scenario::Marker* nearest = nullptr;
        rm::sim::Fx nearestDistance{};
        for (const rm::scenario::Marker& marker : markers) {
            if (!marker.isType("Mass")) {
                continue;
            }
            // The marker's position is float — it comes from a map file — so it crosses into
            // fixed point here, at the boundary, rather than the distance being computed in
            // floats and compared against sim values.
            const rm::sim::Fx distance = rm::sim::groundDistanceElmos(
                from, {rm::sim::fxFromFloat(marker.position[0]),
                       rm::sim::fxFromFloat(marker.position[1]),
                       rm::sim::fxFromFloat(marker.position[2])});
            if (nearest == nullptr || distance < nearestDistance) {
                nearest = &marker;
                nearestDistance = distance;
            }
        }
        if (nearest == nullptr) {
            continue;  // a map with no mass on it: nothing to extract
        }

        // THROUGH `applyCommand`, like every other order — the first extractor of a match is
        // a build like any other. It was the second bypass, and `check_one_order_path.sh`
        // found it the moment that guard learned to watch construction rather than only
        // movement: routing the scripted opponent's builds alone would have left the property
        // half true for a second time, which is the exact shape of the bug being closed.
        //
        // The marker's `y` is dropped rather than converted, and that is the point of the
        // field's note: a build order names a place on the map and the ground decides the
        // height. Carrying it made two runs of one match differ in the state hash over a
        // number nothing ever read.
        if (!issueBuild(scene, scene.store.idAt(slot), playerDriving(scene, army),
                         0, *registered, rm::sim::fxFromFloat(nearest->position[0]),
                         rm::sim::fxFromFloat(nearest->position[2]))) {
            continue;  // refused deterministically
        }

        lastCost = extractor->buildCostMass;
        lastEnergy = extractor->buildCostEnergy;
        lastTime = extractor->buildTime;
    }

    (void)dispatchCommands(scene, field, passability, 0, rm::sim::CommandPhase::PreTick);

    std::printf("economy: %zu extractor(s) ordered on the map's own deposits,"
                " %.0f mass / %.0f energy each over %.0fs\n",
                scene.building.size(),
                static_cast<double>(rm::sim::magToFloat(lastCost)),
                static_cast<double>(rm::sim::magToFloat(lastEnergy)),
                static_cast<double>(rm::sim::magToFloat(lastTime) / firstBuilderRate));
}

/// Places a requested surface fleet at deterministic navigable-cell centres.
[[nodiscard]] std::vector<rm::UnitInstance> scatterOnWater(
    const rm::HeightField& field, float waterLevelElmos, std::size_t count,
    std::uint32_t seed, float scale) {
    const rm::sim::PassabilityGrid grid =
        rm::sim::buildSurfaceWaterPassability(field, waterLevelElmos);
    std::vector<std::size_t> cells;
    for (std::size_t cell = 0; cell < grid.divisor.size(); ++cell) {
        const int x = static_cast<int>(cell % static_cast<std::size_t>(grid.cellsX));
        const int z = static_cast<int>(cell / static_cast<std::size_t>(grid.cellsX));
        if (grid.divisor[cell] != 0
            && (grid.passableAt(x - 1, z) || grid.passableAt(x + 1, z)
                || grid.passableAt(x, z - 1) || grid.passableAt(x, z + 1))) {
            cells.push_back(cell);  // never seed a ship in a one-cell puddle
        }
    }
    std::vector<rm::UnitInstance> placed;
    if (cells.empty()) {
        return placed;
    }

    placed.reserve(count);
    const std::size_t first = static_cast<std::size_t>(seed) % cells.size();
    const std::size_t stride =
        std::max<std::size_t>(1, cells.size() / std::max<std::size_t>(1, count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t cell = cells[(first + i * stride) % cells.size()];
        const int x = static_cast<int>(cell % static_cast<std::size_t>(grid.cellsX));
        const int z = static_cast<int>(cell / static_cast<std::size_t>(grid.cellsX));
        const float phase = count > 0 ? static_cast<float>(i) / static_cast<float>(count) : 0.0f;
        placed.push_back(rm::UnitInstance{
            .position = {{rm::sim::fxToFloat(grid.worldAtCellCentre(x)), waterLevelElmos,
                          rm::sim::fxToFloat(grid.worldAtCellCentre(z))}},
            .rotationY = 2.0f * std::numbers::pi_v<float> * phase,
            .scale = scale,
            .teamColour = rm::teamColour(i),
            .animationPhase = phase,
        });
    }
    return placed;
}

/// Loads every requested model, resolves its textures, and places instances.
///
/// The first model takes the map's start positions and fills the rest of its
/// count by scatter; every later model is scattered alone. Spawn points read as
/// meaningful, and a second model landing on top of the first at every spawn
/// would not.
[[nodiscard]] UnitScene resolveUnits(std::span<const UnitOptions> requests,
                                     const rm::HeightField& field,
                                     std::span<const rm::mapinfo::StartPosition> starts,
                                     bool hasWater, float waterLevelElmos,
                                     const rm::vfs::AssetSearch& search,
                                     const rm::vfs::Vfs& content,
                                     std::span<const std::uint8_t> terrainTypes,
                                     float deepLevelElmos, float abyssLevelElmos) {
    UnitScene scene;
    scene.hasWater = hasWater;
    scene.waterLevelElmos = waterLevelElmos;
    scene.deepLevelElmos = deepLevelElmos;
    scene.abyssLevelElmos = abyssLevelElmos;
    scene.terrainTypeGrid =
        rm::sim::TerrainTypeGrid{terrainTypes, field.squaresX, field.squaresZ};
    scene.lookAhead = std::make_shared<const rm::MaxHeightPyramid>(field);

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const UnitOptions& request = requests[i];

        // `--units` takes a model, a unit DEFINITION, or a blueprint inside the
        // mounted content. A definition brings the stats the game authored for it,
        // which is the whole point: otherwise every unit moves at the one speed this
        // engine used to hardcode.
        //
        // THE CONTENT IS TRIED FIRST, because a path that resolves in the game's own
        // namespace means the caller named game content and not a file that happens
        // to sit at the same place on this disk. Nothing else distinguishes them: a
        // VFS path is spelled like a path, deliberately.
        const std::string requestedPath = request.modelPath.generic_string();
        std::optional<VfsUnit> fromContent;
        if (request.modelPath.extension() == ".bp" && content.contains(requestedPath)) {
            fromContent = resolveUnitFromContent(requestedPath, content);
            if (!fromContent) {
                continue;  // the blueprint said something that could not be honoured
            }
        }

        std::filesystem::path modelPath = request.modelPath;
        std::optional<rm::unitdef::UnitDef> def;
        std::expected<rm::Model, rm::MapError> model{rm::Model{}};

        if (fromContent) {
            def = fromContent->def;
            model = std::move(fromContent->model);
        } else {
            def = resolveUnitDef(request.modelPath, search, modelPath);
            if ((request.modelPath.extension() == ".lua"
                 || request.modelPath.extension() == ".bp")
                && !def) {
                continue;  // the definition said something that could not be honoured
            }

            model = loadModel(modelPath);
            if (!model) {
                rm::log::writef(rm::log::Level::Error, "unit",
                                "failed to load model \"%s\": %s",
                                modelPath.string().c_str(), model.error().message.c_str());
                continue;  // one bad model should not cost the whole scene
            }
        }

        const bool supCom = model->family == rm::Family::SupremeCommander;
        std::printf("model %s (%s): %zu bones, %zu vertices, %zu triangles, radius %.1f\n",
                    model->name.c_str(), supCom ? "Supreme Commander" : "Recoil",
                    model->bones.size(), model->vertices.size(), model->triangleCount(),
                    static_cast<double>(model->radius));

        // Both textures, not just the diffuse. The two families disagree on
        // where they live and on what the second one's channels mean, and that
        // is all they disagree on — see Family in Model.hpp.
        // An animation is optional and, when present, must outlive the upload.
        // It lives in the scene's deque for the same reason the models do.
        const rm::sca::Animation* animation = nullptr;
        if (!request.animationPath.empty()) {
            auto loaded = rm::sca::loadFile(request.animationPath);
            if (!loaded) {
                rm::log::writef(rm::log::Level::Warn, "animation", "no animation (%s): %s",
                                request.animationPath.filename().string().c_str(),
                                loaded.error().message.c_str());
            } else {
                const auto map = rm::mapBonesToAnimation(*model, *loaded);
                const auto driven = static_cast<std::size_t>(
                    std::count_if(map.begin(), map.end(), [](int b) { return b >= 0; }));
                std::printf("  animation %s: %.2fs, %zu keyframes, %zu of %zu bones driven\n",
                            loaded->name.c_str(), static_cast<double>(loaded->duration),
                            loaded->frames.size(), driven, model->bones.size());
                scene.animations.push_back(std::move(*loaded));
                animation = &scene.animations.back();
            }
        }

        // Textures are named after the MESH, not after whatever the command line
        // pointed at — `UEL0201_Albedo.dds` beside `UEL0201_lod0.scm`. It matters
        // now that a blueprint can be the argument: taking the request's path
        // would look for `UEL0201_unit_Albedo.dds`, find nothing, and draw the
        // unit in the fallback white, which reads as a missing texture in the
        // content rather than a wrong path here.
        const rm::TexturePair pair =
            fromContent
                ? rm::TexturePair{
                      .diffuse = scene.textures.resolve(content, fromContent->albedoPath,
                                                        "albedo"),
                      .shading = scene.textures.resolve(content, fromContent->shadingPath,
                                                        "specTeam"),
                  }
            : supCom ? rm::TexturePair{
                         .diffuse = scene.textures.resolve(
                             scmTexturePath(modelPath, kScmDiffuseSuffix), "albedo"),
                         .shading = scene.textures.resolve(
                             scmTexturePath(modelPath, kScmShadingSuffix), "specTeam"),
                     }
                   : rm::TexturePair{
                         .diffuse = scene.textures.resolve(
                             barTexturePath(search, model->textures[0]), "diffuse"),
                         .shading = scene.textures.resolve(
                             barTexturePath(search, model->textures[1]), "shading"),
                   };

        const int normals =
            fromContent
                ? scene.textures.resolve(content, fromContent->normalsPath, "normalsTS")
                : supCom ? scene.textures.resolve(
                               scmTexturePath(modelPath, kScmNormalsSuffix), "normalsTS")
                         : -1;

        // How big the model is, and the answer depends on whether a blueprint
        // said.
        //
        // WITH ONE, it is `meshToElmos` — `Display.UniformScale` times the eight
        // elmos in an ogrid — and that is the file's own answer rather than ours.
        //
        // WITHOUT ONE, a bare `.scm` gets the x8 that has stood here since
        // milestone 7, on the assumption that its vertices are in ogrids. THEY ARE
        // NOT: raw mesh extents across the corpus run from 10 to 262 units, which
        // as ogrids would make one experimental 2096 elmos long — a quarter of the
        // map it stands on — and UEL0201 a 65-elmo tank instead of a 4.5-elmo one.
        // The blueprint is what closes that gap, so the old factor is left only
        // where there is no blueprint to consult and nothing better to guess.
        const float scale = request.scale
                          * (def && def->meshToElmos > 0.0f ? def->meshToElmos
                             : supCom                       ? kOgridScale
                                                            : 1.0f);

        std::vector<rm::UnitInstance> placed;
        const bool surfaceWater = def && floatsOnWater(*def);
        const bool takesStarts = i == 0 && !surfaceWater;
        if (surfaceWater && hasWater) {
            placed = scatterOnWater(field, waterLevelElmos, request.count,
                                     kScatterSeed + static_cast<std::uint32_t>(i), scale);
            if (!placed.empty()) {
                std::printf("  water placement starts at %.0f, %.0f\n",
                            static_cast<double>(placed.front().position[0]),
                            static_cast<double>(placed.front().position[2]));
            }
        } else if (takesStarts) {
            placed = rm::atStartPositions(field, starts, scale);
        }
        const std::size_t remaining =
            request.count > placed.size() ? request.count - placed.size() : 0;
        const auto scattered = surfaceWater
                                 ? std::vector<rm::UnitInstance>{}
                                 : rm::scatterOnLand(
                                       field, remaining,
                                       kScatterSeed + static_cast<std::uint32_t>(i), scale,
                                       hasWater ? waterLevelElmos : 0.0f);
        placed.insert(placed.end(), scattered.begin(), scattered.end());

        std::printf("  %zu instances (%zu at start positions, %zu scattered)\n", placed.size(),
                    takesStarts ? starts.size() : 0u, scattered.size());

        scene.models.push_back(std::move(*model));

        // A definition that omits these — or a building, whose maxslope is 0 —
        // falls back to the defaults rather than to a grid nothing can cross.
        //
        // A GROUND MOVER'S ZERO IS NOT A MISSING VALUE, which is why the depth
        // test asks the motion class rather than the number. Supreme Commander's
        // land units state no wading depth because they do not wade, and reading
        // From the MOTION CLASS (P3.4). A decorative instance with no definition gets the
        // immobile MoveDef, which uses no ground grid — correct, since nothing routes it.
        const rm::data::MoveDef move =
            def.has_value() ? rm::data::moveDefFor(*def)
                            : rm::data::moveDefFor(rm::unitdef::MotionType::None);

        // The TYPE for these units. One per batch, and the two indices are the same number
        // by construction — which is the whole reason the old parallel-array hazard here is
        // gone: there is one array to append to, not four that had to be appended to
        // together or the next batch wrote its entries at the wrong index and read health
        // off the end. (That was a segfault the moment `--units` and `--skirmish` were given
        // together.)
        rm::UnitTypeIndex type = 0;
        if (def) {
            scene.definitions.push_back(*def);
            type = scene.catalog.add(&scene.definitions.back(), gAppTickRate);
            scene.catalog.setAttachBones(
                type, resolveAttachBones(scene.models.back(), *def));
        } else {
            type = scene.catalog.add(nullptr);  // a bare model: it neither fires nor dies
        }
        // The batch for this type is the one pushed below, so the mapping is recorded there
        // rather than asserted here. The assertion this replaces — `type == batches.size()` —
        // is the one that made a buildable type impossible to register early (`#3090`).
        scene.setTypeTraits(type, move, def.has_value() ? def->meshToElmos : 1.0f);
        // `Display.Tarmacs` (`C-289`), resolved with the blueprint — a crowd
        // unit has no army to stamp for, but the flag is the type's fact and
        // costs nothing to keep honest.
        if (fromContent && fromContent->hasTarmac) {
            scene.tarmacForType[static_cast<std::size_t>(type)] = 1;
        }

        // A definition's speed and turn rate reach every unit of it. Slope and depth limits
        // do NOT yet: passability is one grid for the whole scene, so honouring them per
        // unit type would mean a grid per type.
        const rm::sim::Mag hp = def.has_value() ? def->health : rm::sim::Mag{};
        for (const rm::UnitInstance& instance : placed) {
            rm::sim::MoveState state;
            if (def) {
                state = motionFor(*def, rm::sim::kNoArmy);
            }
            rm::sim::Transform transform = transformAt(instance.position, instance.rotationY);
            rm::sim::placeOnMotionLayer(transform, state, scene.terrain(field));
            // Scenario-placed structures flatten their pads too — retail's
            // `CreateUnitHPR` runs the same `OnCreate` → `FlattenSkirt` a built one does.
            if (def) {
                flattenSkirtAt(scene, field, type, transform, state);
            }
            // The lift law chases this from the first tick: seed it from the same
            // ground-plus-clearance the align pass just assigned (`C-221`).
            if (state.canFly) {
                state.altitudeRef = transform.y;
            }
            (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
                .type = type,
                .transform = transform,
                .motion = state,
                .health = rm::sim::initialHealth(hp, scene.catalog.shield(type).maximum),
            });
        }

        const TurretRig turretRig =
            resolveTurretRig(scene.models.back(), def ? &*def : nullptr);
        const rm::sca::Animation* unpack =
            def ? loadUnpackAnimation(scene, content, *def) : nullptr;
        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = {},
            .textures = pair,
            .normals = normals,
            .animation = animation,
            .builderAim = def ? rm::resolveBuilderAim(scene.models.back(), def->builderArm,
                                                      def->buildEffectBones)
                              : rm::BuilderAimRig{},
            .turretAim = std::move(turretRig.rig),
            .turretWeapon = turretRig.weapon,
            .recoilFlags = std::move(turretRig.recoilFlags),
            .recoilDistanceElmos = turretRig.recoilDistanceElmos,
            .recoilReturnPerTick = turretRig.recoilReturnPerTick,
            .telescopeDistanceElmos = turretRig.telescopeDistanceElmos,
            .telescopeReturnPerTick = turretRig.telescopeReturnPerTick,
            .unpackAnimation = unpack,
            .unpackOneshot = unpack != nullptr,
        });
        scene.setBatchForType(type, scene.batches.size() - 1);
    }

    scene.interpolate = gInterpolate;

    // The batches' instance spans, filled from a snapshot now that every unit is in the store.
    // Published twice so `previous` and `current` agree: nothing has moved yet.
    scene.publish(0);
    scene.publish(0);
    scene.gatherForDrawing();

    if (scene.batches.size() > 1) {
        // Reports what the batching bought. The renderer orders the draws
        // itself; this recomputes the same thing purely to say it out loud,
        // which is the only way the saving is visible at all — a renderer that
        // rebound per model would produce an identical image.
        std::vector<rm::TexturePair> pairs;
        pairs.reserve(scene.batches.size());
        for (const rm::UnitBatch& batch : scene.batches) {
            pairs.push_back(batch.textures);
        }

        std::printf("scene: %zu models, %zu textures uploaded, %zu texture binds per frame\n",
                    scene.batches.size(), scene.textures.size(),
                    rm::textureBindCount(pairs, rm::orderByTexturePair(pairs)));
    }

    return scene;
}

void loadProjectileMeshes(UnitScene& scene, const rm::vfs::Vfs& content) {
    // Shared geometry is loaded once: 65 retail projectiles name a mesh blueprint, and many
    // of them name the same one (`/meshes/projectiles/missile_default_mesh.bp`).
    std::map<std::string, std::size_t> batchByMesh;
    for (const std::string& path : content.list("/projectiles", "_proj.bp")) {
        const auto bytes = content.read(path);
        if (!bytes) continue;
        const auto table = rm::lua::parseTable(
            std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        if (!table) continue;
        const rm::lua::Value* display = table->find("Display");

        // Where the geometry is. `Display.MeshBlueprint` names a mesh blueprint
        // (`X_mesh.bp`) whose LOD 0 sits beside IT; otherwise the mesh sits beside the
        // projectile blueprint. Both are the one file-name rule of BlueprintMesh.hpp. A
        // blueprint with neither is an effect-only projectile and keeps its strip.
        std::optional<std::string_view> declared;
        if (display != nullptr) declared = display->stringAt("MeshBlueprint");
        std::string meshPath =
            declared ? rm::blueprint::meshBeside(std::string{*declared}, "_mesh", 0).generic_string()
                     : rm::blueprint::meshBeside(path, rm::blueprint::kProjectileSuffix, 0).generic_string();
        // A mesh blueprint's LOD table may point elsewhere: retail's shared
        // `missile_default_mesh.bp` names `TAAMissileFlayer01_lod0.scm` and its textures
        // outright. Its LOD 0 wins over the file-name rule when it says so.
        std::string albedoPath;
        std::string normalsPath;
        if (declared) {
            if (const auto meshBytes = content.read(*declared)) {
                const auto meshTable = rm::lua::parseTable(std::string_view{
                    reinterpret_cast<const char*>(meshBytes->data()), meshBytes->size()});
                const rm::lua::Value* lods = meshTable ? meshTable->find("LODs") : nullptr;
                if (lods != nullptr && !lods->items.empty()) {
                    const rm::lua::Value& lod0 = lods->items.front();
                    if (const auto name = lod0.stringAt("MeshName")) meshPath = std::string{*name};
                    if (const auto name = lod0.stringAt("AlbedoName")) albedoPath = std::string{*name};
                    if (const auto name = lod0.stringAt("NormalsName")) normalsPath = std::string{*name};
                }
            }
        }
        const std::string meshKey = rm::foldedVisualKey(meshPath);
        auto batch = batchByMesh.find(meshKey);
        if (batch == batchByMesh.end()) {
            const auto scm = content.read(meshPath);
            if (!scm) continue;
            auto model = loadModelBytes(*scm);
            if (!model) {
                rm::log::writef(rm::log::Level::Warn, "content", "projectile mesh %s: %s",
                                meshPath.c_str(), model.error().message.c_str());
                continue;
            }
            scene.models.push_back(std::move(*model));
            const rm::Model& stored = scene.models.back();
            if (albedoPath.empty()) albedoPath = scmTextureInVfs(meshPath, kScmDiffuseSuffix, content);
            if (normalsPath.empty()) normalsPath = scmTextureInVfs(meshPath, kScmNormalsSuffix, content);
            const rm::TexturePair pair{
                .diffuse = scene.textures.resolve(content, albedoPath, "albedo"),
                .shading = scene.textures.resolve(content, scmTextureInVfs(meshPath, kScmShadingSuffix, content), "specTeam"),
            };
            const int normals = scene.textures.resolve(content, normalsPath, "normalsTS");
            scene.batches.push_back(rm::UnitBatch{
                .model = &stored, .instances = {}, .textures = pair, .normals = normals});
            batch = batchByMesh.emplace(meshKey, scene.batches.size() - 1).first;
        }
        // `Display.UniformScale` times the eight elmos in an ogrid — the same answer a unit's
        // `meshToElmos` gives (SceneBuild's model registration above).
        const double uniform = display != nullptr ? display->numberAt("UniformScale").value_or(1.0) : 1.0;
        const std::string key = rm::foldedVisualKey(path);
        scene.projectileMeshes[key] = UnitScene::ProjectileMesh{
            .batch = batch->second, .scale = static_cast<float>(uniform) * kOgridScale};
        scene.weaponVisuals.meshed.insert(key);
    }
}


std::size_t spawnSavedUnits(UnitScene& scene, const rm::HeightField& field,
                            std::span<const rm::scenario::SavedUnit> units,
                            const rm::vfs::Vfs& content) {
    std::size_t spawned = 0;
    for (const rm::scenario::SavedUnit& unit : units) {
        // ARMY_<n> -> seat n-1, the same mapping `loadStartPositions` uses for
        // its markers. A unit whose army the session did not seat is skipped
        // rather than rehomed — retail's `ArmyInitializePrebuiltUnits` walks
        // the armies it created, not every army the save names.
        const auto armyNumber = [&]() -> std::optional<int> {
            if (unit.army.size() <= 5 || unit.army.rfind("ARMY_", 0) != 0) {
                return std::nullopt;
            }
            try {
                return std::stoi(unit.army.substr(5));
            } catch (...) {
                return std::nullopt;
            }
        }();
        if (!armyNumber || *armyNumber < 1
            || static_cast<std::size_t>(*armyNumber) > scene.armies.size()) {
            continue;
        }
        const rm::sim::Army& army = scene.armies[static_cast<std::size_t>(*armyNumber - 1)];

        // The saved `type` is a blueprint id ('ueb5101'); the drawable path is
        // the same convention `resolveUnits` uses for --units arguments.
        const std::string path = "/units/" + unit.type + "/" + unit.type + "_unit.bp";
        const rm::Brad yaw = rm::sim::bradFromRadians(unit.orientation[1]);
        if (spawnUnit(scene, content, field, path, unit.position, army, yaw)) {
            ++spawned;
        }
    }
    return spawned;
}
} // namespace rm::app
