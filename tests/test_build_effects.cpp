#include "core/scene/BuildEffects.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE(
    "UEF build beams sweep between the two cube corners nearest the builder") {
    const std::array<float, 3> site{100.0f, 10.0f, 200.0f};
    const std::array<float, 3> builder{80.0f, 10.0f, 180.0f};

    const rm::BuildBeamEnds start =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.0f);
    CHECK(start.first[0] == Approx(90.0f));
    CHECK(start.first[1] == Approx(40.0f));
    CHECK(start.first[2] == Approx(194.0f));
    CHECK(start.second[0] == Approx(90.0f));
    CHECK(start.second[2] == Approx(206.0f));

    const rm::BuildBeamEnds halfway =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.3f);
    CHECK(halfway.first[2] == Approx(200.0f));
    CHECK(halfway.second[2] == Approx(200.0f));

    const rm::BuildBeamEnds swapped =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 0.6f);
    CHECK(swapped.first == start.second);
    CHECK(swapped.second == start.first);

    const rm::BuildBeamEnds returned =
        rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 12.0f, 0.25f, 1.2f);
    CHECK(returned.first == start.first);
    CHECK(returned.second == start.second);
}

TEST_CASE("UEF build beam height descends with construction progress") {
    const std::array<float, 3> site{0.0f, 5.0f, 0.0f};
    const std::array<float, 3> builder{-20.0f, 5.0f, 0.0f};

    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 0.0f, 0.0f)
              .first[1] == Approx(45.0f));
    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 0.5f, 0.0f)
              .first[1] == Approx(25.0f));
    CHECK(rm::uefBuildBeamEnds(site, builder, 20.0f, 40.0f, 20.0f, 1.0f, 0.0f)
              .first[1] == Approx(5.0f));
}

TEST_CASE("Cybran build beams land at a heading-rotated random offset inside "
          "the target's size box") {
    // `CreateCybranBuildBeams` (`lua/EffectUtilities.lua`, retail `lua.scd`)
    // warps each bone's beam end to `unitBeingBuilt:GetPosition()` plus
    // `builder.GetRandomOffset(unitBeingBuilt, 1)` every 0.2 s, where
    // `Unit.lua:GetRandomOffset` scales the target's `SizeX/SizeY/SizeZ` by
    // the scalar, draws three uniforms, and rotates by the builder heading.
    // The draw is presentation Lua `Random()`, never the sim stream.
    constexpr float kPi = 3.141592653589793f;
    // Zero heading: the box passes through unrotated. Size 8 x 16 x 4 elmos,
    // draws 0.75/0.5/0.25 -> rx = 0.75*8-4 = 2, y = 0.5*16 = 8,
    // rz = 0.25*4-2 = -1.
    const std::array<float, 3> plain =
        rm::cybranBuildBeamTarget(8.0f, 16.0f, 4.0f, 0.0f, 0.0f, 0.75f, 0.5f, 0.25f);
    CHECK(plain[0] == Approx(2.0f));
    CHECK(plain[1] == Approx(8.0f));
    CHECK(plain[2] == Approx(1.0f));
    // A quarter turn: cos90*rx - sin90*rz = -rz, sin90*rx - cos90*rz = rx.
    // (2, -1) -> (1, 2): retail's `z` arm keeps the source's minus sign,
    // not the textbook plus.
    const std::array<float, 3> turned =
        rm::cybranBuildBeamTarget(8.0f, 16.0f, 4.0f, kPi / 2.0f, 0.0f, 0.75f, 0.5f, 0.25f);
    CHECK(turned[0] == Approx(1.0f));
    CHECK(turned[1] == Approx(8.0f));
    CHECK(turned[2] == Approx(2.0f));
    // The collision offset lifts the box: `Random()*sy + CollisionOffsetY`.
    const std::array<float, 3> lifted =
        rm::cybranBuildBeamTarget(8.0f, 16.0f, 4.0f, 0.0f, 3.0f, 0.75f, 0.5f, 0.25f);
    CHECK(lifted[1] == Approx(11.0f));
}

TEST_CASE("Aeon engineer beams end at the target pad, one beam per bone") {
    // `CreateAeonCommanderBuildingEffects` (retail `lua/EffectUtilities.lua`
    // `lua.scd`): the `beamEnd` entity is warped to
    // `unitBeingBuilt:GetPosition()` and each `BuildEffectBones` bone streams
    // one `AeonBuildBeams01` beam to it — no sweep, no random offset. The
    // beam lands at the pad, unlike UEF's corner sweep or Cybran's
    // `GetRandomOffset` box.
    const std::array<float, 3> end =
        rm::aeonBuildBeamEnd(100.0f, 12.0f, 200.0f);
    CHECK(end[0] == Approx(100.0f));
    CHECK(end[1] == Approx(12.0f));
    CHECK(end[2] == Approx(200.0f));
}

TEST_CASE("Seraphim engineer beams end at the target pad, one beam per bone") {
    // `CreateSeraphimUnitEngineerBuildingEffects` (retail EffectUtilities.lua
    // :600-611): each `BuildEffectBones` bone streams one
    // `SeraphimBuildBeams01` beam builder-bone -> target — no sweep, no offset.
    const std::array<float, 3> end =
        rm::seraphimBuildBeamEnd(100.0f, 12.0f, 200.0f);
    CHECK(end[0] == Approx(100.0f));
    CHECK(end[1] == Approx(12.0f));
    CHECK(end[2] == Approx(200.0f));
}

TEST_CASE("Seraphim/Aeon factory build-base scale follows authored mesh "
          "extents with footprint fallback") {
    // `CreateSeraphimFactoryBuildingEffects` (`EffectUtilities.lua:613-628`)
    // and its Aeon twin (:537-552): `mul = 1`, `sx = MeshExtentsX or
    // Footprint.SizeX * mul` (same for Z), `sy = MeshExtentsY or sx + sz`,
    // base projectile `SetScale(sx, 1, sz)` warped to `(x, y - 0.05, z)`.
    // Zero MeshExtents means unauthored (UnitDef keeps zero, never a
    // default), so each axis falls back independently.
    const rm::FactoryBaseScale authored = rm::factoryBaseScale(5.0f, 9.0f, 7.0f, 0.0f, 0.0f);
    CHECK(authored.sx == Approx(5.0f));
    CHECK(authored.sy == Approx(9.0f));
    CHECK(authored.sz == Approx(7.0f));
    const rm::FactoryBaseScale fallback =
        rm::factoryBaseScale(0.0f, 0.0f, 0.0f, 3.0f, 4.0f);
    CHECK(fallback.sx == Approx(3.0f));
    CHECK(fallback.sy == Approx(3.0f + 4.0f));
    CHECK(fallback.sz == Approx(4.0f));
    CHECK(rm::factoryBaseWarpYOffset == Approx(-0.05f));
}

TEST_CASE("factory build-base pool resolves placement from the product def") {
    // `CreateSeraphimFactoryBuildingEffects` (`EffectUtilities.lua:613-628`)
    // and its Aeon twin (:537-552) warp the base projectile to
    // `(x, y - 0.05, z)` with `SetScale(sx, 1, sz)` where `sx/sz` come
    // from mesh extents with footprint fallback (`mul = 1`). The render
    // path must resolve the same placement from the product def: a unit
    // with no authored mesh extents and a 2x2-ogrid footprint covers
    // 16x16 elmos (`scmap::kElmosPerOgrid` = 8) at the builder pad
    // minus 0.05.
    const rm::FactoryBasePlacement pool = rm::factoryBasePool(
        0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 100.0f, 12.0f, 200.0f);
    CHECK(pool.scaleX == Approx(16.0f));
    CHECK(pool.scaleZ == Approx(16.0f));
    CHECK(pool.scaleY == Approx(1.0f));
    CHECK(pool.position[0] == Approx(100.0f));
    CHECK(pool.position[1] == Approx(12.0f - 0.05f));
    CHECK(pool.position[2] == Approx(200.0f));
}
