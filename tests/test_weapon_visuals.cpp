#include "core/scene/WeaponVisuals.hpp"
#include "core/scene/ProjectileFx.hpp"
#include "core/scene/CombatEffects.hpp"
#include "app/Scene.hpp"
#include "app/SceneBuild.hpp"
#include "app/FafAi.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>

TEST_CASE("retail weapons resolve distinct textured bolts and beam strips", "[corpus][weapon-visuals]") {
    const std::filesystem::path root = "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata";
    if (!std::filesystem::exists(root / "effects.scd")) SKIP("retail effects unavailable");
    rm::vfs::Vfs content;
    REQUIRE(content.mountArchive(root / "effects.scd"));
    REQUIRE(content.mountArchive(root / "textures.scd"));
    REQUIRE(content.mountArchive(root / "projectiles.scd"));
    REQUIRE(content.mountArchive(root / "mohodata.scd"));
    REQUIRE(content.mountArchive(root / "lua.scd"));
    REQUIRE(content.mountArchive(root / "units.scd"));
    auto visuals = rm::loadWeaponVisuals(content);
    REQUIRE_FALSE(visuals.find("UEL0201:MainGun#FxMuzzleFlash").empty());
    REQUIRE_FALSE(visuals.find("XSL0001:ChronotronCannon#FxMuzzleFlash").empty());
    REQUIRE_FALSE(visuals.find("/projectiles/TDFGauss01/TDFGauss01_proj.bp#FxImpactLand").empty());
    REQUIRE_FALSE(visuals.find("/projectiles/TDFGauss01/TDFGauss01_proj.bp#FxImpactUnit").empty());
    CHECK(visuals.scale("/projectiles/TDFGauss01/TDFGauss01_proj.bp#FxImpactUnderWater") == Catch::Approx(0.25));
    rm::CombatEffectState effects;
    std::vector<rm::Particle> flashes;
    const rm::sim::Event fired{.kind=rm::sim::EventKind::WeaponFired,
        .at2={rm::sim::Fx::fromInt(100),rm::sim::Fx::fromInt(20),{}},
        .visualId="UEL0201:MainGun"};
    rm::emitCombatEffects(flashes, std::array{fired}, &visuals, &effects);
    REQUIRE_FALSE(flashes.empty());
    for (const auto& flash : flashes) {
        CHECK(flash.material != UINT32_MAX);
        CHECK(std::ranges::find(visuals.find(fired.visualId+"#FxMuzzleFlash"), flash.material)
            != visuals.find(fired.visualId+"#FxMuzzleFlash").end());
    }
    const rm::sim::Event impact{.kind=rm::sim::EventKind::ProjectileImpact,
        .at={rm::sim::Fx::fromInt(50),{},{}}, .impactType=rm::sim::ImpactType::Terrain,
        .visualId="/projectiles/TDFGauss01/TDFGauss01_proj.bp"};
    rm::CombatEffectState impactEffects;
    std::vector<rm::Particle> hits;
    rm::emitCombatEffects(hits, std::array{impact}, &visuals, &impactEffects);
    REQUIRE_FALSE(hits.empty());
    const auto before = hits.size();
    rm::emitCombatEffects(hits, {}, &visuals, &impactEffects);
    CHECK(hits.size() >= before);
    for (const auto& hit : hits) CHECK(hit.material != UINT32_MAX);
    const auto emitter = [&](std::string_view path) -> const rm::WeaponMaterial& {
        const auto it = std::ranges::find(visuals.materials, path, &rm::WeaponMaterial::emitter);
        REQUIRE(it != visuals.materials.end());
        return *it;
    };
    const auto& disruptor = emitter("/effects/emitters/adisruptor_cannon_munition_01_emit.bp");
    CHECK(disruptor.blend == rm::EffectBlend::Modulate2xInverse);
    CHECK(disruptor.emitRate.sample(0.5f) == Catch::Approx(30));
    CHECK(disruptor.particleLifetime.sample(0.5f) == Catch::Approx(2));
    CHECK(disruptor.startSize.sample(0.5f) == Catch::Approx(0.162));
    CHECK(disruptor.endSize.sample(0.5f) == Catch::Approx(0.057));
    std::uint32_t seed = 123;
    const auto particle = rm::makeWeaponParticle(disruptor, 0, 0.5f, {10,20,30}, seed);
    CHECK(particle.lifetime == Catch::Approx(2));
    CHECK(particle.size == Catch::Approx(0.162f * 16));
    CHECK(particle.size + particle.growth * particle.lifetime == Catch::Approx(0.057f * 16));
    CHECK(particle.animation[0] == Catch::Approx(1));
    CHECK(particle.velocity == std::array<float,3>{0,0,0});
    CHECK(particle.origin == std::array<float,3>{10,20,30});
    std::vector<rm::Particle> emitted;
    rm::emitWeaponParticles(emitted, disruptor, 0, {0,0,0}, {3,0,0}, 0, 0.1f, 123);
    REQUIRE(emitted.size() == 4);
    CHECK(emitted.front().origin[0] == Catch::Approx(0));
    CHECK(emitted.back().origin[0] == Catch::Approx(3));
    CHECK(emitted.front().age == Catch::Approx(0.1f));
    // Splitting the same interval must preserve emission count and fractional debt.
    std::vector<rm::Particle> split;
    rm::emitWeaponParticles(split, disruptor, 0, {0,0,0}, {1.5f,0,0}, 0, 0.05f, 123);
    rm::emitWeaponParticles(split, disruptor, 0, {1.5f,0,0}, {3,0,0}, 0.05f, 0.1f, 123);
    REQUIRE(split.size() == emitted.size());
    CHECK(split[1].origin[0] == Catch::Approx(emitted[1].origin[0]));
    CHECK(emitter("/effects/emitters/microwave_laser_beam_01_emit.bp").width == Catch::Approx(8.8));
    CHECK(emitter("/effects/emitters/electron_bolter_munition_02_emit.bp").startSize.sample(0.5f) == 0);
    std::fprintf(stderr, "weapon visuals: %zu definitions, %zu materials, %zu unresolved\n",
        visuals.definitions.size(), visuals.materials.size(), visuals.unavailable.size());
    for (const auto& error : visuals.unavailable) INFO(error);
    for (const auto* id : {"TDFGauss01", "ADFDisruptor01", "CDFLaserHeavy01", "SDFOhCannon01"}) {
        const auto key = std::string("/projectiles/") + id + "/" + id + "_proj.bp";
        INFO(key);
        REQUIRE_FALSE(visuals.find(key).empty());
        rm::sim::Projectile shot;
        shot.visualId = key;
        shot.position = {rm::sim::Fx::fromInt(100), rm::sim::Fx::fromInt(10), {}};
        shot.velocity = {rm::sim::Fx::fromInt(8), {}, {}};
        std::vector<rm::Particle> particles;
        rm::appendProjectiles(particles, std::array{shot}, 0.5f, 1.0f, &visuals);
        // Straight strips only: emitters run on the tick path and ribbons over the recorded path.
        const auto straight = static_cast<std::size_t>(std::ranges::count_if(visuals.find(key),
            [&](auto id) { return visuals.materials[id].emitRate.keys.empty()
                                  && !visuals.materials[id].ribbon; }));
        REQUIRE(particles.size() == straight);
        for (const auto& strip : particles) {
            CHECK(strip.origin[0] + strip.length == Catch::Approx(104));
            CHECK(strip.material < visuals.materials.size());
            CHECK(strip.size > 0);
            CHECK(strip.axis[0] == Catch::Approx(1));
        }
        // A PolyTrail resolves as a ribbon with its authored TrailLength, and the gallery's
        // straight preview still draws it.
        const auto ribbon = std::ranges::find_if(visuals.find(key),
            [&](auto id) { return visuals.materials[id].ribbon; });
        if (ribbon != visuals.find(key).end()) {
            CHECK(visuals.materials[*ribbon].length > 0);
            std::vector<rm::Particle> preview;
            rm::appendWeaponVisual(preview, visuals, key, {100, 10, 0}, {108, 10, 0}, 1.0f);
            CHECK(std::ranges::any_of(preview, [&](const auto& p) { return p.material == *ribbon; }));
        }
    }
    CHECK(visuals.find("unknown").empty());

    // Original projectile meshes: `X_lod0.scm` beside `X_proj.bp`, drawn through the unit
    // pipeline pointed along the velocity, with the bolt strip stepping aside.
    {
        rm::app::UnitScene scene;
        scene.weaponVisuals = visuals;
        rm::app::loadProjectileMeshes(scene, content);
        const char* gauss = "/projectiles/TDFGauss01/TDFGauss01_proj.bp";
        REQUIRE(scene.projectileMeshes.contains(rm::foldedVisualKey(gauss)));
        const auto mesh = scene.projectileMeshes.at(rm::foldedVisualKey(gauss));
        CHECK(mesh.scale > 0);
        REQUIRE(mesh.batch < scene.batches.size());
        REQUIRE(scene.batches[mesh.batch].model != nullptr);
        CHECK_FALSE(scene.batches[mesh.batch].model->vertices.empty());
        CHECK(scene.weaponVisuals.hasMesh(gauss));
        // 43 retail projectile blueprints sit beside a lod0 mesh; more resolve through
        // Display.MeshBlueprint, some into archives this test does not mount.
        CHECK(scene.projectileMeshes.size() >= 43);
        // Blueprints naming the same mesh blueprint share one loaded model and batch.
        const auto shell1 = scene.projectileMeshes.find(
            rm::foldedVisualKey("/projectiles/AIFFragmentationSensorShell01/AIFFragmentationSensorShell01_proj.bp"));
        const auto shell2 = scene.projectileMeshes.find(
            rm::foldedVisualKey("/projectiles/AIFFragmentationSensorShell02/AIFFragmentationSensorShell02_proj.bp"));
        REQUIRE(shell1 != scene.projectileMeshes.end());
        REQUIRE(shell2 != scene.projectileMeshes.end());
        CHECK(shell1->second.batch == shell2->second.batch);
        CHECK(shell1->second.batch != mesh.batch);

        rm::sim::Projectile flying;
        flying.visualId = gauss;
        flying.position = {rm::sim::Fx::fromInt(100), rm::sim::Fx::fromInt(10), rm::sim::Fx::fromInt(50)};
        flying.velocity = {rm::sim::Fx::fromInt(10), rm::sim::Fx::fromInt(5), rm::sim::Fx::fromInt(10)};
        scene.projectiles.push_back(flying);
        scene.gatherForDrawing(1.0f);
        REQUIRE(scene.drawScratch.size() > mesh.batch);
        REQUIRE(scene.drawScratch[mesh.batch].size() == 1);
        CHECK(scene.drawSlotOf[mesh.batch].empty()); // no unit slot: picking cannot land on it
        const auto& instance = scene.drawScratch[mesh.batch].front();
        CHECK(instance.position[0] == Catch::Approx(100));
        CHECK(instance.position[2] == Catch::Approx(50));
        CHECK(instance.scale == Catch::Approx(mesh.scale));
        CHECK(instance.rotationY == Catch::Approx(std::atan2(10.0, 10.0)));
        CHECK(instance.rotationX == Catch::Approx(-std::atan2(5.0, std::hypot(10.0, 10.0))));
        // A frame between ticks extrapolates the shot; a delivered shot is not drawn.
        scene.gatherForDrawing(0.5f);
        CHECK(scene.drawScratch[mesh.batch].front().position[0] == Catch::Approx(105));
        scene.projectiles.front().pendingImpact = rm::sim::ImpactType::Terrain;
        scene.gatherForDrawing(1.0f);
        CHECK(scene.drawScratch[mesh.batch].empty());
        std::vector<rm::Particle> strips;
        rm::appendProjectiles(strips, std::array{flying}, 0.0f, 1.0f, &scene.weaponVisuals);
        CHECK(strips.empty());
    }
    rm::sim::Event beam;
    beam.kind = rm::sim::EventKind::BeamFired;
    beam.visualId = "URB2301:MainGun";
    beam.visualDuration = rm::sim::Fx::fromRatio(3, 5);
    beam.at = {rm::sim::Fx::fromInt(100), {}, {}};
    std::vector<rm::Particle> particles;
    rm::CombatEffectState beamEffects;
    rm::emitCombatEffects(particles, std::array{beam}, &visuals, &beamEffects);
    REQUIRE(particles.size() >= visuals.find(beam.visualId).size());
    CHECK_FALSE(beamEffects.bursts.empty()); // Impact emitters may start after the first step.
    for (const auto& drawn : particles) CHECK(drawn.material != UINT32_MAX);
    // The beam itself is live state drawn one tick at a time; the strip spans muzzle to target.
    REQUIRE(beamEffects.beams.size() == 1);
    // 0.6 s authored, one default 0.1 s tick already drawn by this call.
    CHECK(beamEffects.beams.front().remaining == Catch::Approx(0.5).margin(0.0001));
    const auto strip = std::ranges::find_if(particles, [](const auto& p) { return p.length > 0; });
    REQUIRE(strip != particles.end());
    CHECK(strip->length == Catch::Approx(100));
    CHECK(strip->lifetime == Catch::Approx(0.1));
    CHECK(strip->origin[0] == Catch::Approx(0));
    CHECK(strip->material < visuals.materials.size());
    CHECK(visuals.find("urb2301:maingun").size() == visuals.find(beam.visualId).size());

    // A mod changes the original declaration, not a C++ weapon-name lookup table.
    // Reuse retail class imports and emitter assets; only the override is synthetic.
    const auto overlay = std::filesystem::temp_directory_path() / "rm_weapon_visual_override";
    const auto script = overlay / "projectiles/tdfgauss01/tdfgauss01_script.lua";
    std::filesystem::create_directories(script.parent_path());
    {
        std::ofstream file(script);
        file << "# legacy script comment\n"
                "local Parent = import('/lua/terranprojectiles.lua').TDFGaussCannonProjectile\n"
                "TypeClass = Class(Parent) { PolyTrails = {"
                "'/effects/emitters/microwave_laser_beam_01_emit.bp'} }\n";
    }
    content.mountDirectory(overlay);
    const auto modified = rm::loadWeaponVisuals(content);
    std::filesystem::remove_all(overlay);
    const auto replacement = modified.find("/projectiles/tdfgauss01/tdfgauss01_proj.bp");
    REQUIRE(replacement.size() == 1);
    CHECK(modified.materials[replacement.front()].emitter ==
          "/effects/emitters/microwave_laser_beam_01_emit.bp");
    CHECK(modified.materials[replacement.front()].width == Catch::Approx(0.55f * 16));
}

TEST_CASE("effect curves interpolate values and random ranges over their cycle", "[weapon-visuals]") {
    rm::EffectCurve curve{.range = 10, .keys = {{2, 10, 2}, {8, 40, 8}}};
    CHECK(curve.sample(0) == Catch::Approx(10));
    CHECK(curve.sample(0.5f) == Catch::Approx(25));
    CHECK(curve.sample(1) == Catch::Approx(40));
    CHECK(curve.sample(0.5f, 1) == Catch::Approx(27.5f));
    CHECK(curve.sample(0.5f, 0) == Catch::Approx(22.5f));
    CHECK(curve.integral(1) == Catch::Approx(25));
    CHECK(rm::EffectCurve{}.sample(0.5f) == 0);
}

TEST_CASE("legacy Lua permits a numeric literal adjacent to then", "[weapon-visuals]") {
    const auto source = rm::ai::rewriteLegacyLua("if count > 0then\n return '0then' -- 0then\nend");
    CHECK(source.find("0 then") != std::string::npos);
    CHECK(source.find("'0then'") != std::string::npos);
    CHECK(source.find("-- 0then") != std::string::npos);
}

TEST_CASE("retail distortion ring retains its refraction and world-plane geometry", "[weapon-visuals][corpus]") {
    const std::filesystem::path root = "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata";
    if (!std::filesystem::exists(root / "effects.scd")) SKIP("retail effects unavailable");
    rm::vfs::Vfs content;
    REQUIRE(content.mountArchive(root / "effects.scd"));
    REQUIRE(content.mountArchive(root / "textures.scd"));
    rm::WeaponVisuals visuals;
    const std::array<std::string,1> paths{"/effects/emitters/distortion_ring_01_emit.bp"};
    rm::loadWeaponMaterials(visuals, content, "ring", paths);
    REQUIRE(visuals.materials.size() == 1);
    const auto& material = visuals.materials.front();
    CHECK(material.blend == rm::EffectBlend::Refract);
    CHECK(material.flat);
    CHECK(material.emitterLifetime == Catch::Approx(6));
    std::uint32_t seed = 42;
    const auto particle = rm::makeWeaponParticle(material, 0, 0.5f, {}, seed);
    CHECK(particle.flags == 1);
    CHECK(particle.size == Catch::Approx(32));
}

TEST_CASE("a launched shot owns its Lua visual identity", "[weapon-visuals]") {
    rm::unitdef::Weapon weapon;
    weapon.projectileId = "/projectiles/tdfgauss01/tdfgauss01_proj.bp";
    const auto shot = rm::sim::launch({}, {rm::sim::Fx::fromInt(100), {}, {}},
        weapon, 0, rm::sim::TickRate{}, rm::sim::Fx::fromInt(1),
        rm::unitdef::flatDamage(weapon.damage));
    weapon.projectileId.clear();
    CHECK(shot.visualId == "/projectiles/tdfgauss01/tdfgauss01_proj.bp");
}

TEST_CASE("muzzle emitters follow their model bone and preserve owner generations", "[weapon-visuals]") {
    rm::app::UnitScene scene;
    rm::unitdef::UnitDef def;
    def.name = "TEST";
    def.weapons.emplace_back();
    def.weapons.back().label = "MainGun";
    def.weapons.back().visualMuzzleOffset = std::array<float,3>{2,3,5};
    const auto type = scene.catalog.add(&def);
    const auto unit = scene.store.spawn({.type=type,
        .transform={.x=rm::sim::Fx::fromInt(100), .z=rm::sim::Fx::fromInt(100), .heading=16384}});
    const auto muzzle = scene.weaponMuzzle(unit,"TEST:MainGun");
    REQUIRE(muzzle);
    CHECK((*muzzle)[0] == Catch::Approx(105));
    CHECK((*muzzle)[1] == Catch::Approx(3));
    CHECK((*muzzle)[2] == Catch::Approx(98));
    scene.combatEffectState.bursts.push_back({.owner=unit,.weapon="TEST:MainGun"});
    scene.store.transforms()[unit.index].x += rm::sim::Fx::fromInt(10);
    scene.updateCombatAttachments();
    CHECK(scene.combatEffectState.bursts.front().position[0] == Catch::Approx(115));
    scene.store.kill(unit);
    const auto replacement = scene.store.spawn({.type=type});
    REQUIRE(replacement.index == unit.index);
    REQUIRE(replacement.generation != unit.generation);
    scene.updateCombatAttachments();
    CHECK(scene.combatEffectState.bursts.empty());
}

TEST_CASE("event emitters honour finite, unbounded and explicitly empty definitions", "[weapon-visuals]") {
    rm::WeaponVisuals visuals;
    rm::WeaponMaterial material;
    material.emitterLifetime = 0.2f;
    material.emitRate.keys = {{0,10,0}};
    material.particleLifetime.keys = {{0,1,0}};
    material.startSize.keys = {{0,1,0}};
    material.endSize.keys = {{0,2,0}};
    visuals.materials.push_back(material);
    visuals.definitions["gun#fxmuzzleflash"] = {0};
    visuals.scales["gun#fxmuzzleflash"] = 0.5f;
    const rm::sim::Event event{.kind=rm::sim::EventKind::WeaponFired,.visualId="gun"};
    rm::CombatEffectState state;
    std::vector<rm::Particle> particles;
    rm::emitCombatEffects(particles,std::array{event},&visuals,&state);
    REQUIRE(particles.size() == 1);
    CHECK(particles.front().age == 0);
    CHECK(particles.front().size == Catch::Approx(8));
    for (int i=0; i<3; ++i) rm::emitCombatEffects(particles,{},&visuals,&state);
    CHECK(state.bursts.empty());
    CHECK(particles.size() == 3);

    visuals.materials.front().emitterLifetime = -1;
    rm::emitCombatEffects(particles,std::array{event},&visuals,&state);
    for (int i=0; i<5; ++i) rm::emitCombatEffects(particles,{},&visuals,&state,1);
    CHECK_FALSE(state.bursts.empty());

    state.bursts.clear();
    particles.clear();
    visuals.definitions["gun#fxmuzzleflash"].clear();
    rm::emitCombatEffects(particles,std::array{event},&visuals,&state);
    CHECK(particles.empty());
    CHECK(state.bursts.empty());
}

TEST_CASE("beam strips follow their live endpoints for the authored lifetime", "[weapon-visuals]") {
    // One strip material, no emitter curves: a straight textured beam.
    rm::WeaponVisuals visuals;
    rm::WeaponMaterial material;
    material.width = 2;
    visuals.materials.push_back(material);
    visuals.definitions["laser"] = {0};
    // Authored-empty muzzle and impact lists are intentional silence, not a fallback spark.
    // Definition keys are stored case-folded, as the loader writes them.
    visuals.definitions["laser#fxmuzzleflash"] = {};
    visuals.definitions["laser#fximpactunit"] = {};

    const rm::sim::UnitId shooter{.index = 3, .generation = 1};
    const rm::sim::UnitId target{.index = 7, .generation = 1};
    rm::sim::Event event{.kind = rm::sim::EventKind::BeamFired, .unit = shooter, .instigator = target,
        .at = {rm::sim::Fx::fromInt(100), {}, {}}, .at2 = {}, .visualId = "laser",
        .visualDuration = rm::sim::Fx::fromRatio(3, 10)};
    rm::CombatEffectState state;
    std::vector<rm::Particle> particles;

    // Firing registers the beam and draws its first one-tick strip from muzzle to target.
    rm::emitCombatEffects(particles, std::array{event}, &visuals, &state, 0.1f);
    REQUIRE(state.beams.size() == 1);
    CHECK(state.beams.front().owner == shooter);
    CHECK(state.beams.front().target == target);
    REQUIRE(particles.size() == 1);
    CHECK(particles.front().length == Catch::Approx(100));
    CHECK(particles.front().axis[0] == Catch::Approx(1));
    CHECK(particles.front().age == Catch::Approx(0));
    CHECK(particles.front().lifetime == Catch::Approx(0.1));

    // The app moves the endpoints; the next tick's strip follows them and keeps the clock
    // running so a scrolling texture does not restart every tick.
    state.beams.front().to = {0, 0, 50};
    rm::emitCombatEffects(particles, {}, &visuals, &state, 0.1f);
    REQUIRE(particles.size() == 2);
    CHECK(particles.back().length == Catch::Approx(50));
    CHECK(particles.back().axis[2] == Catch::Approx(1));
    CHECK(particles.back().age == Catch::Approx(0.1));
    CHECK(particles.back().lifetime == Catch::Approx(0.2));

    // Three ticks of 0.1 s exhaust a 0.3 s beam; nothing is drawn afterwards.
    rm::emitCombatEffects(particles, {}, &visuals, &state, 0.1f);
    CHECK(particles.size() == 3);
    CHECK(state.beams.empty());
    rm::emitCombatEffects(particles, {}, &visuals, &state, 0.1f);
    CHECK(particles.size() == 3);

    // A weapon fires one beam at a time: refiring replaces its live beam instead of stacking.
    rm::emitCombatEffects(particles, std::array{event, event}, &visuals, &state, 0.1f);
    CHECK(state.beams.size() == 1);
    CHECK(state.beams.front().remaining == Catch::Approx(0.2).margin(0.0001)); // Fx precision

    // Unknown keys keep the procedural bead chain and register no live beam.
    rm::CombatEffectState fallback;
    std::vector<rm::Particle> beads;
    event.visualId = "unknown";
    rm::emitCombatEffects(beads, std::array{event}, &visuals, &fallback, 0.1f);
    CHECK(fallback.beams.empty());
    CHECK(beads.size() > 2);
}

TEST_CASE("projectile ribbons retain turns, clip to the authored length and drain after impact",
          "[weapon-visuals]") {
    rm::WeaponVisuals visuals;
    rm::WeaponMaterial material;
    material.ribbon = true;
    material.length = 15; // TrailLength, elmos
    material.width = 2;
    visuals.materials.push_back(material);
    visuals.definitions["shot"] = {0};

    // Muzzle at the origin; one tick later the shot is at x=10 heading +x, then it turns +z.
    rm::ProjectileTrails trails;
    std::vector<rm::sim::Projectile> shots(1);
    shots[0].visualId = "shot";
    using Fx = rm::sim::Fx;
    shots[0].visualOrigin = std::array<Fx, 3>{};
    shots[0].position = std::array<Fx, 3>{Fx::fromInt(10), Fx{}, Fx{}};
    shots[0].velocity = std::array<Fx, 3>{Fx::fromInt(10), Fx{}, Fx{}};
    trails.update(shots, visuals, 0.1f);
    const auto serial = shots[0].visualSerial;
    REQUIRE(serial != 0);
    shots[0].position = std::array<Fx, 3>{Fx::fromInt(10), Fx{}, Fx::fromInt(10)};
    shots[0].velocity = std::array<Fx, 3>{Fx{}, Fx{}, Fx::fromInt(10)};
    trails.update(shots, visuals, 0.1f);
    CHECK(shots[0].visualSerial == serial);
    CHECK(trails.size() == 1);

    // Head first: the +z leg is whole (u 0..2/3); the +x leg is clipped to the 5 elmos left.
    std::vector<rm::Particle> ribbons;
    trails.append(ribbons, visuals, 0.0f, 0.0f);
    REQUIRE(ribbons.size() == 2);
    CHECK((ribbons[0].flags & 2u) != 0);
    CHECK(ribbons[0].origin[2] == Catch::Approx(0));
    CHECK(ribbons[0].axis[2] == Catch::Approx(1));
    CHECK(ribbons[0].length == Catch::Approx(10));
    CHECK(ribbons[0].trailRange[1] == Catch::Approx(0));
    CHECK(ribbons[0].trailRange[0] == Catch::Approx(10.0 / 15));
    CHECK(ribbons[1].origin[0] == Catch::Approx(5));
    CHECK(ribbons[1].axis[0] == Catch::Approx(1));
    CHECK(ribbons[1].length == Catch::Approx(5));
    CHECK(ribbons[1].trailRange[0] == Catch::Approx(1));
    CHECK(ribbons[1].trailRange[1] == Catch::Approx(ribbons[0].trailRange[0]));
    CHECK(ribbons[0].material == 0);
    CHECK(ribbons[0].size == Catch::Approx(2));

    // Between ticks the live head is extrapolated by the frame's tick fraction.
    ribbons.clear();
    trails.append(ribbons, visuals, 0.5f, 0.0f);
    REQUIRE(ribbons.size() == 2);
    CHECK(ribbons[0].origin[2] + ribbons[0].length == Catch::Approx(15));

    // Impact: the shot is gone, the ribbon keeps moving at its last speed (100 elmos/s) and
    // slides out of the authored window; nothing is left once the whole path has drained.
    shots.clear();
    trails.update(shots, visuals, 0.05f); // drained 5 elmos
    ribbons.clear();
    trails.append(ribbons, visuals, 0.0f, 0.0f);
    REQUIRE(ribbons.size() == 1);
    CHECK(ribbons[0].trailRange[1] == Catch::Approx(5.0 / 15));
    CHECK(ribbons[0].trailRange[0] == Catch::Approx(1));
    trails.update(shots, visuals, 0.2f); // drained 25 > the 20 elmos recorded
    CHECK(trails.size() == 0);
    ribbons.clear();
    trails.append(ribbons, visuals, 0.0f, 0.0f);
    CHECK(ribbons.empty());

    // A shot without ribbon materials is never tracked, and the bolt path skips ribbons.
    std::vector<rm::sim::Projectile> plain(1);
    plain[0].visualId = "unknown";
    trails.update(plain, visuals, 0.1f);
    CHECK(plain[0].visualSerial == 0);
    CHECK(trails.size() == 0);
    std::vector<rm::Particle> bolts;
    rm::appendProjectiles(bolts, shots, 0.0f, 1.0f, &visuals);
    CHECK(bolts.empty());
}
