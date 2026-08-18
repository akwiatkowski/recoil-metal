// Asset search and archive extraction.
//
// The interesting half is isSafeArchivePath: an archive names its own entries,
// and a name is the one part of an archive that can reach outside the directory
// it is being extracted into.
#include <catch2/catch_test_macros.hpp>

#include "core/vfs/AssetSearch.hpp"

#include <filesystem>
#include <fstream>

namespace {

/// A throwaway tree of files to search.
[[nodiscard]] std::filesystem::path makeTree() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "rm_test_assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "objects3d" / "Units");
    std::filesystem::create_directories(root / "unittextures");
    { std::ofstream out{root / "objects3d" / "Units" / "armpw.s3o"}; out << "model"; }
    { std::ofstream out{root / "unittextures" / "Arm_color.dds"}; out << "texture"; }
    return root;
}

} // namespace

TEST_CASE("an archive entry may not escape the directory it is extracted into") {
    // Zip Slip. `path / other` REPLACES the left side when the right is
    // absolute, so a naive join writes wherever the archive says — which is
    // why this is a refusal and not a sanitisation.
    CHECK_FALSE(rm::vfs::isSafeArchivePath("/etc/passwd"));
    CHECK_FALSE(rm::vfs::isSafeArchivePath("../outside.txt"));
    CHECK_FALSE(rm::vfs::isSafeArchivePath("units/../../outside.txt"));
    CHECK_FALSE(rm::vfs::isSafeArchivePath("a/b/../../../c"));
    CHECK_FALSE(rm::vfs::isSafeArchivePath(""));

    // Ordinary names, including ones with dots that are not a parent step.
    CHECK(rm::vfs::isSafeArchivePath("units/armpw.lua"));
    CHECK(rm::vfs::isSafeArchivePath("objects3d/Units/armpw.s3o"));
    CHECK(rm::vfs::isSafeArchivePath("a.b/..c/d..e"));
    CHECK(rm::vfs::isSafeArchivePath("./units/armpw.lua"));
}

TEST_CASE("a search path resolves a relative file under its roots, in order") {
    const std::filesystem::path root = makeTree();

    rm::vfs::AssetSearch search;
    search.addRoot(root);

    CHECK(search.rootCount() == 1);
    CHECK_FALSE(search.empty());

    const std::filesystem::path found = search.resolve("unittextures/Arm_color.dds");
    REQUIRE_FALSE(found.empty());
    CHECK(std::filesystem::exists(found));

    CHECK(search.resolve("unittextures/nosuch.dds").empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("a search path finds a file by name alone, whatever its case or depth") {
    // What model loading needs: a definition says `Units/ARMPW.s3o` and the
    // file is `objects3d/Units/armpw.s3o`.
    const std::filesystem::path root = makeTree();

    rm::vfs::AssetSearch search;
    search.addRoot(root);

    CHECK(search.resolveByName("armpw.s3o").filename() == "armpw.s3o");
    CHECK(search.resolveByName("ARMPW.S3O").filename() == "armpw.s3o");
    CHECK(search.resolveByName("nosuch.s3o").empty());
    CHECK(search.resolveByName("").empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("a root that does not exist is dropped rather than searched") {
    rm::vfs::AssetSearch search;
    search.addRoot("/nonexistent/nowhere");

    CHECK(search.empty());
    CHECK(search.resolve("anything").empty());
    CHECK(search.resolveByName("anything").empty());
}

TEST_CASE("extracting an archive that is not there fails rather than throwing") {
    const std::filesystem::path destination =
        std::filesystem::temp_directory_path() / "rm_test_extract";
    std::filesystem::remove_all(destination);

    CHECK_FALSE(rm::vfs::extractZip("/nonexistent/nowhere.sdz", destination));

    std::filesystem::remove_all(destination);
}
