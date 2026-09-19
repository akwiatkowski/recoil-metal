#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::vfs {

/// One active mod, as `__active_mods` carries it (`C-313`): the fields
/// `mods.lua`'s `LoadModInfo` fills from `mod_info.lua`. `location` is where
/// the mod's content lives — a real path for mounting, which `mountMod` turns
/// into the `/mods/<name>` VFS prefix `modMountPoint` computes.
struct ActiveMod {
    std::string uid;
    std::string name;
    std::filesystem::path location;
    /// `hookdir` defaults to `/hook` in `LoadModInfo`; empty means the same.
    std::string hookdir;
    /// `ui_only` mods never reach the sim's `__active_mods` — the user session
    /// appends them separately (`C-313`). Carried so the filter is a decision
    /// the caller makes, not one this struct hides.
    bool uiOnly = false;
    /// Ordering constraints by uid (`mods.lua` `ModComp`). An empty `after`
    /// falls back to `requires` — the documented default.
    std::vector<std::string> before;
    std::vector<std::string> after;
    /// `requires` in `mod_info.lua` — the `after` fallback (mods.lua:82).
    /// Named `requiredUids` because `requires` is a C++20 keyword.
    std::vector<std::string> requiredUids;
};

/// The VFS prefix a mod mounts at: `/mods/<name>` (`C-268`), `<name>` being
/// the location's last component — `/mods/Chess`, not `/mods/<uid>`.
[[nodiscard]] std::string modMountPoint(const ActiveMod& mod);

/// The hook directory a mod contributes: its `hookdir` under the mount point,
/// defaulting to `/hook` (`C-312`).
[[nodiscard]] std::string modHookDirectory(const ActiveMod& mod);

/// `__active_mods` order (`C-313`): the `before`/`after` uid constraints of
/// `mods.lua`'s `ModComp`, topologically sorted with uid-alphabetical
/// tie-breaking — the documented "before/after else uid-alphabetical". A
/// constraint naming a mod outside the set is ignored; a contradictory set
/// (mods.lua warns it "may cause an arbitrary ordering") resolves
/// deterministically by the same uid order rather than sorting garbage.
[[nodiscard]] std::vector<ActiveMod> orderActiveMods(std::vector<ActiveMod> mods);


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
// LAYERS ARE THE MECHANISM — but not the mod mechanism. Mount order decides
// which archive's file a path resolves to (lua.scd over mohodata.scd), so a
// flat extracted directory cannot express the game's own override layers
// however carefully it is unpacked. Retail's MODS work differently (`C-268`):
// they mount under `/mods/<name>` — never at `/`, so they cannot shadow — and
// act through hook concatenation and the blueprint pass instead. `mountMod`
// below is that mount; the layering above is what it deliberately avoids.
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

    /// Mounts a directory under a VFS prefix instead of the root — the form a
    /// mod needs (`C-268`: active mods mount under `/mods/<name>`, NOT at `/`,
    /// so a mod can never shadow a base file).
    void mountDirectoryAt(std::string_view vfsPrefix, std::filesystem::path directory);

    /// Mounts a ZIP archive under a VFS prefix — the same mount as
    /// `mountDirectoryAt`, for a mod shipped packed.
    [[nodiscard]] bool mountArchiveAt(std::string_view vfsPrefix,
                                      const std::filesystem::path& archive);

    /// Mounts a ZIP archive (`.scd`, `.sdz`), which stays open for the lifetime of
    /// this object. False if it is not a readable archive.
    ///
    /// The index is read eagerly and the file data is not: opening all of Supreme
    /// Commander's gamedata is 17 archives and about 30,000 names, which costs
    /// milliseconds, where extracting them is gigabytes.
    [[nodiscard]] bool mountArchive(const std::filesystem::path& archive);

    /// LAST MOUNT WINS — the layering rule the game's own override archives
    /// rely on (lua.scd over mohodata.scd). Mount stock content first and an
    /// override after, and the override's `UEL0201_unit.bp` shadows the stock
    /// one while every file it does not contain still resolves. The opposite
    /// order would make the layer inert, silently.
    ///
    /// This is NOT how retail mods work — `C-268` refutes mod shadowing; see
    /// `mountMod`. It is how the mount list's own ordering works.
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


    /// Registers a hook directory — the `SCR_AddHookDirectory` equivalent
    /// (`C-312`). Retail fills the list from each mount spec's `hook` table:
    /// `bin/SupComDataPath.lua` contributes `hook={'/schook'}` for the stock
    /// content, and each active mod contributes its `hookdir` (default
    /// `/hook`). `doscript`/`import` then concatenates `<hookdir><path>` onto
    /// the base file for every registered directory, in registration order
    /// (`C-311`: original → native hookdirs → `__active_mods` order).
    void addHookDirectory(std::string_view vfsPrefix);

    /// The hook files one VFS path collects, in the order they concatenate:
    /// every registered hook directory's `<dir><path>` that exists. Empty when
    /// nothing hooks the file — the common case.
    [[nodiscard]] std::vector<std::string> hooksFor(std::string_view gamePath) const;

    /// The registered hook directories, in registration order.
    [[nodiscard]] std::span<const std::string> hookDirectories() const noexcept;

    /// Mounts one active mod the way retail does (`C-268`): under
    /// `/mods/<name>` — NEVER at `/`, so a mod cannot shadow a base file —
    /// and registers its hook directory for the concat above (`C-312`).
    /// `location` may be a directory or a `.zip`-family archive; `<name>` is
    /// its last component. Returns false when the location cannot be mounted.
    [[nodiscard]] bool mountMod(const ActiveMod& mod);
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
