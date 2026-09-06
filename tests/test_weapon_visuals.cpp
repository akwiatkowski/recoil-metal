#include "core/scene/WeaponVisuals.hpp"
#include "core/scene/ProjectileFx.hpp"
#include "core/scene/CombatEffects.hpp"
#include "app/Scene.hpp"
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
        REQUIRE(particles.size() == static_cast<std::size_t>(std::ranges::count_if(visuals.find(key),
            [&](auto id) { return visuals.materials[id].emitRate.keys.empty(); })));
        CHECK(particles.front().origin[0] + particles.front().length == Catch::Approx(104));
        CHECK(particles.front().material < visuals.materials.size());
        CHECK(particles.front().size > 0);
        CHECK(particles.front().axis[0] == Catch::Approx(1));
    }
    CHECK(visuals.find("unknown").empty());
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
    CHECK(particles.front().length == Catch::Approx(100));
    CHECK(particles.front().lifetime == Catch::Approx(0.6).margin(0.0001));
    CHECK(particles.front().origin[0] == Catch::Approx(0));
    CHECK(particles.front().material < visuals.materials.size());
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
