// The VFS against the retail install's own archives, unextracted.
//
// This is the test the extraction step existed to avoid needing. It reads
// `units.scd` and `env.scd` where they lie — 1.06 GiB and 1.29 GiB — and asks for
// content by the paths the blueprints themselves use. If it passes, nothing in
// this repo needs a Python one-liner to see the game's content.
#include <catch2/catch_test_macros.hpp>

#include "core/vfs/Vfs.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

/// The retail install's gamedata directory. An external drive, so absence is
/// ordinary and these SKIP rather than fail.
[[nodiscard]] std::filesystem::path gamedata() {
    return "/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata";
}

} // namespace

TEST_CASE("the retail archives mount and answer by VFS path", "[corpus]") {
    const std::filesystem::path units = gamedata() / "units.scd";
    if (!std::filesystem::exists(units)) {
        SKIP("no retail install at " + gamedata().string());
    }

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(units));

    // 6726 entries in the archive, 680 of which are DIRECTORIES and so not files:
    // one per unit and then some. The VFS keeps only the files, because a directory
    // is implied by the names of the things in it and mounting one as a readable
    // entry would hand a caller zero bytes that looked like content.
    CHECK(vfs.fileCount() == 6046);

    // The blueprint corpus, found by listing rather than by walking a disk.
    const auto blueprints = vfs.list("/units", "_unit.bp");
    CHECK(blueprints.size() == 568);

    // A specific unit, by the path its own content names it with — and note the
    // case, which is the point: the archive holds `UEL0201_LOD0.scm` in capitals
    // and this asks in lower.
    const auto blueprint = vfs.read("/units/UEL0201/UEL0201_unit.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->size() > 1000);

    const auto mesh = vfs.read("/units/uel0201/uel0201_lod0.scm");
    REQUIRE(mesh.has_value());
    // "MODL", the .scm magic, so this is the real mesh rather than a name that
    // happened to resolve.
    REQUIRE(mesh->size() > 4);
    CHECK(static_cast<char>((*mesh)[0]) == 'M');
    CHECK(static_cast<char>((*mesh)[1]) == 'O');
    CHECK(static_cast<char>((*mesh)[2]) == 'D');
    CHECK(static_cast<char>((*mesh)[3]) == 'L');

    // The textures too, which is what makes a unit look like itself.
    CHECK(vfs.contains("/units/UEL0201/UEL0201_Albedo.dds"));
    CHECK(vfs.contains("/units/UEL0201/UEL0201_SpecTeam.dds"));
}

TEST_CASE("mounting the whole gamedata directory gives one namespace", "[corpus]") {
    if (!std::filesystem::exists(gamedata())) {
        SKIP("no retail install at " + gamedata().string());
    }

    rm::vfs::Vfs vfs;
    std::size_t mounted = 0;
    // Mounted in name order, which is what the game does absent a mod list, and
    // what makes the count below reproducible.
    for (const auto& item : std::filesystem::directory_iterator{gamedata()}) {
        if (item.path().extension() == ".scd" && vfs.mountArchive(item.path())) {
            ++mounted;
        }
    }

    CHECK(mounted == 17);

    // Content from four different archives, reached through one set of paths —
    // which is the thing an extracted directory can imitate and a modded install
    // cannot.
    CHECK(vfs.contains("/units/UEL0201/UEL0201_unit.bp"));         // units.scd
    CHECK(vfs.contains("/env/Evergreen/Props/Trees/Pine06_prop.bp"));  // env.scd
    CHECK(vfs.contains("/lua/sim/Unit.lua"));                      // lua.scd
    CHECK(vfs.contains("/effects/Emitters/beam_default_emit.bp"));  // effects.scd

    // The prop blueprints the map reader already resolves, now without extracting
    // 135 MiB of layer textures to get at them. 334 under `/env`, which is where
    // the maps look — the corpus figure of 335 quoted elsewhere in this repo counts
    // across archives, and the two stragglers live outside `/env` entirely.
    CHECK(vfs.list("/env", "_prop.bp").size() == 334);
    CHECK(vfs.contains("/meshes/explosions/oblivion_explosion01_prop.bp"));
    CHECK(vfs.contains("/props/DefaultWreckage/DefaultWreckage_prop.bp"));

    // No `.scmap` in gamedata: maps live in their own directory beside the install,
    // not in the archives. The single exception is an AI test fixture, and finding
    // it is how that was established rather than assumed.
    const auto maps = vfs.list("/", ".scmap");
    CHECK(maps.size() == 1);
    CHECK(maps.front() == "/lua/AI/OpAI/OpAIMap.scmap");
}
