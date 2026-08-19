// The layered content view: directories and archives under one set of paths.
//
// Archives are BUILT here rather than taken from the install, which is the one
// place in this repo where a synthetic fixture beats the real thing: the question
// is whether layering, case folding and path traversal behave, and a stock
// archive cannot be made to contain a hostile name or a deliberate conflict.
// test_real_vfs.cpp does the corpus half.
#include <catch2/catch_test_macros.hpp>

#include "core/vfs/Vfs.hpp"

#include "miniz.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path scratch() {
    return std::filesystem::temp_directory_path() / "rm_vfs_test";
}

/// Writes a ZIP with the given (name, contents) pairs. Deleted by the fixture.
void writeArchive(const std::filesystem::path& path,
                  const std::vector<std::pair<std::string, std::string>>& files) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::remove(path);
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0) != MZ_FALSE);
    for (const auto& [name, contents] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION))
                != MZ_FALSE);
    }
    REQUIRE(mz_zip_writer_finalize_archive(&zip) != MZ_FALSE);
    mz_zip_writer_end(&zip);
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << contents;
}

[[nodiscard]] std::string asText(const std::vector<std::byte>& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

// Cleans the scratch directory around each test, so one test's mount cannot be
// another's surprise.
struct Scratch {
    Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(scratch(), ec);
        std::filesystem::create_directories(scratch());
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(scratch(), ec);
    }
};

} // namespace

TEST_CASE("a path is normalised the way the archives name things") {
    using rm::vfs::normalisedVfsPath;

    CHECK(normalisedVfsPath("units/UEL0201/UEL0201_unit.bp")
          == "/units/uel0201/uel0201_unit.bp");
    CHECK(normalisedVfsPath("/units/UEL0201/UEL0201_unit.bp")
          == "/units/uel0201/uel0201_unit.bp");

    // Both separators, because the content mixes them.
    CHECK(normalisedVfsPath("env\\Evergreen\\props") == "/env/evergreen/props");

    // Redundant components say nothing.
    CHECK(normalisedVfsPath("//units///x//") == "/units/x");
    CHECK(normalisedVfsPath("./units/./x") == "/units/x");

    // `..` resolves where it can...
    CHECK(normalisedVfsPath("/units/UEL0201/../UEC1101/x.scm") == "/units/uec1101/x.scm");

    // ...and is REFUSED when it would climb out, rather than clamped to the root.
    // Clamping would turn a hostile path into a valid one, and a mounted directory
    // serves real files.
    CHECK(normalisedVfsPath("../../etc/passwd").empty());
    CHECK(normalisedVfsPath("/units/../../etc/passwd").empty());
}

TEST_CASE("a mounted directory's files resolve by VFS path") {
    const Scratch guard;
    writeFile(scratch() / "stock/units/UEL0201/UEL0201_unit.bp", "stock tank");

    rm::vfs::Vfs vfs;
    vfs.mountDirectory(scratch() / "stock");

    REQUIRE(vfs.fileCount() == 1);
    REQUIRE(vfs.contains("/units/UEL0201/UEL0201_unit.bp"));
    const auto bytes = vfs.read("/units/UEL0201/UEL0201_unit.bp");
    REQUIRE(bytes.has_value());
    CHECK(asText(*bytes) == "stock tank");
}

TEST_CASE("a mounted archive's files resolve without being extracted") {
    const Scratch guard;
    writeArchive(scratch() / "units.scd", {{"units/UEL0201/UEL0201_unit.bp", "tank in a zip"},
                                          {"units/UEL0201/UEL0201_lod0.scm", "mesh bytes"}});

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(scratch() / "units.scd"));

    CHECK(vfs.fileCount() == 2);
    const auto bytes = vfs.read("/units/UEL0201/UEL0201_unit.bp");
    REQUIRE(bytes.has_value());
    CHECK(asText(*bytes) == "tank in a zip");
}

TEST_CASE("lookup ignores case, because the content disagrees with itself") {
    const Scratch guard;
    // The real disagreement: `UEL0201_LOD0.scm` in capitals beside
    // `UEL0201_lod1.scm` in lower, in one directory.
    writeArchive(scratch() / "units.scd", {{"units/UEL0201/UEL0201_LOD0.scm", "level zero"}});

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(scratch() / "units.scd"));

    CHECK(vfs.contains("/units/UEL0201/UEL0201_lod0.scm"));
    CHECK(vfs.contains("/UNITS/uel0201/uel0201_LOD0.SCM"));
    REQUIRE(vfs.read("/units/uel0201/uel0201_lod0.scm").has_value());
}

TEST_CASE("the last mount wins, which is what makes a mod a mod") {
    const Scratch guard;
    writeArchive(scratch() / "units.scd", {{"units/UEL0201/UEL0201_unit.bp", "stock"},
                                          {"units/UEL0105/UEL0105_unit.bp", "untouched"}});
    writeArchive(scratch() / "mod.scd", {{"units/UEL0201/UEL0201_unit.bp", "modded"}});

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(scratch() / "units.scd"));
    REQUIRE(vfs.mountArchive(scratch() / "mod.scd"));

    // The mod's file shadows the stock one...
    CHECK(asText(*vfs.read("/units/UEL0201/UEL0201_unit.bp")) == "modded");
    // ...and everything it does not contain still resolves. This is the half that
    // makes layering useful and the half a flat extraction cannot express.
    CHECK(asText(*vfs.read("/units/UEL0105/UEL0105_unit.bp")) == "untouched");
    CHECK(vfs.fileCount() == 2);
}

TEST_CASE("a directory can shadow an archive and an archive a directory") {
    const Scratch guard;
    writeArchive(scratch() / "units.scd", {{"units/x.bp", "from the archive"}});
    writeFile(scratch() / "loose/units/x.bp", "from the directory");

    SECTION("directory mounted last") {
        rm::vfs::Vfs vfs;
        REQUIRE(vfs.mountArchive(scratch() / "units.scd"));
        vfs.mountDirectory(scratch() / "loose");
        CHECK(asText(*vfs.read("/units/x.bp")) == "from the directory");
    }
    SECTION("archive mounted last") {
        rm::vfs::Vfs vfs;
        vfs.mountDirectory(scratch() / "loose");
        REQUIRE(vfs.mountArchive(scratch() / "units.scd"));
        CHECK(asText(*vfs.read("/units/x.bp")) == "from the archive");
    }
}

TEST_CASE("an archive entry that climbs out of the root resolves to nothing") {
    const Scratch guard;
    // A ZIP stores whatever name it likes. Nothing here writes to disk, so this
    // cannot overwrite anything — but a mounted directory serves real files, and
    // one lookup rule covers both, so the name is refused where it is normalised.
    writeArchive(scratch() / "hostile.scd",
                 {{"../../etc/passwd", "not yours"}, {"units/fine.bp", "fine"}});

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(scratch() / "hostile.scd"));

    CHECK(vfs.fileCount() == 1);
    CHECK(vfs.contains("/units/fine.bp"));
    CHECK_FALSE(vfs.read("../../etc/passwd").has_value());
}

TEST_CASE("listing finds content by directory and extension") {
    const Scratch guard;
    writeArchive(scratch() / "units.scd", {
                                              {"units/UEL0201/UEL0201_unit.bp", ""},
                                              {"units/UEL0201/UEL0201_lod0.scm", ""},
                                              {"units/UEL0105/UEL0105_unit.bp", ""},
                                              {"unitsextra/UEL9999_unit.bp", ""},
                                              {"env/Evergreen/props/Tree01_prop.bp", ""},
                                          });

    rm::vfs::Vfs vfs;
    REQUIRE(vfs.mountArchive(scratch() / "units.scd"));

    const auto blueprints = vfs.list("/units", ".bp");
    REQUIRE(blueprints.size() == 2);
    // Original case is preserved: the VFS folds case for LOOKUP, not for reporting,
    // and a caller printing a path should print what the archive says.
    CHECK(blueprints[0] == "/units/UEL0105/UEL0105_unit.bp");
    CHECK(blueprints[1] == "/units/UEL0201/UEL0201_unit.bp");

    // `/units` must not also match `/unitsextra`, which is what a plain string
    // prefix would do.
    CHECK(vfs.list("/units", ".bp").size() == 2);
    CHECK(vfs.list("/unitsextra", ".bp").size() == 1);

    // Everything, when no extension is given.
    CHECK(vfs.list("/units", "").size() == 3);
    CHECK(vfs.list("/", "").size() == 5);

    // Case-insensitive on both arguments.
    CHECK(vfs.list("/UNITS", ".BP").size() == 2);
}

TEST_CASE("a missing file and a bad archive are answers, not failures") {
    const Scratch guard;
    rm::vfs::Vfs vfs;

    CHECK_FALSE(vfs.mountArchive(scratch() / "not-there.scd"));

    writeFile(scratch() / "notazip.scd", "this is not a zip file");
    CHECK_FALSE(vfs.mountArchive(scratch() / "notazip.scd"));

    vfs.mountDirectory(scratch() / "also-not-there");

    CHECK(vfs.empty());
    CHECK_FALSE(vfs.contains("/anything"));
    CHECK_FALSE(vfs.read("/anything").has_value());
    CHECK(vfs.list("/", "").empty());
}
