#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::vfs {

// The game's content as the game sees it: one namespace of paths, assembled from
// directories and archives, read in place.
//
// WHY THIS EXISTS AND EXTRACTION DOES NOT SUFFICE. Both games ship their content
// in ZIPs — `.scd` for Supreme Commander, `.sdz` for Recoil — and a `.bp` inside
// one names its neighbours by VFS path (`/env/Evergreen/props/Tree01_prop.bp`), not
// by anything on a real disk. Unpacking the archives first works, and is what the
// tests have done, but it is not what a game does: it costs 1.15 GiB of duplicate
// DDS for env.scd alone, it goes stale the moment the install is patched, and it
// has no answer at all to the thing that makes this content worth reading —
//
// MODS ARE LAYERS. A Supreme Commander mod is an archive mounted OVER the stock
// one, replacing the blueprints it happens to contain and leaving the rest.
// Layering is the whole mechanism, so a flat extracted directory cannot express a
// modded game however carefully it is unpacked, and the priority rule below is the
// feature rather than a detail of it.
//
// Read-only by design. Nothing here writes, so there is no question of what a mod
// does to the install.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;
    Vfs(Vfs&&) noexcept;
    Vfs& operator=(Vfs&&) noexcept;

    /// Mounts a directory. Its contents appear at the VFS root.
    void mountDirectory(std::filesystem::path directory);

    /// Mounts a ZIP archive (`.scd`, `.sdz`), which stays open for the lifetime of
    /// this object. False if it is not a readable archive.
    ///
    /// The index is read eagerly and the file data is not: opening all of Supreme
    /// Commander's gamedata is 17 archives and about 30,000 names, which costs
    /// milliseconds, where extracting them is gigabytes.
    [[nodiscard]] bool mountArchive(const std::filesystem::path& archive);

    /// LAST MOUNT WINS, which is what makes a mod a mod.
    ///
    /// Mount stock content first and mods after, and a mod's `UEL0201_unit.bp`
    /// shadows the stock one while every file it does not contain still resolves.
    /// The opposite order would make mods inert, silently — the game would run,
    /// with none of the changes.
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view gamePath) const;

    [[nodiscard]] bool contains(std::string_view gamePath) const;

    /// Every path under `directoryPrefix` ending in `extension`, in the form the
    /// VFS names them (leading slash, original case). Both arguments are matched
    /// case-insensitively; an empty extension matches everything.
    ///
    /// What a corpus test wants, and what "find every map" wants. Sorted, so two
    /// runs enumerate in the same order — the archives do not promise one, and a
    /// test that iterates content should not depend on which archive was written
    /// first.
    [[nodiscard]] std::vector<std::string> list(std::string_view directoryPrefix,
                                                std::string_view extension) const;

    /// How many names resolve. Cheap; it is a map size.
    [[nodiscard]] std::size_t fileCount() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return fileCount() == 0; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The VFS spelling of a path: forward slashes, one leading slash, no `.` or `..`
/// components, lower-cased for comparison.
///
/// Exposed because it is the whole of the lookup rule and worth testing directly.
/// Returns an empty string for a path that tries to climb out of the VFS with
/// `..`, which a mounted directory must refuse — inside an archive there is
/// nothing above the root to reach, but a directory mount would otherwise hand out
/// files from anywhere on the disk.
[[nodiscard]] std::string normalisedVfsPath(std::string_view path);

} // namespace rm::vfs
