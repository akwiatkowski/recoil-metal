#include "core/vfs/Vfs.hpp"

#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <utility>

namespace rm::vfs {
namespace {

[[nodiscard]] char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

std::string normalisedVfsPath(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 1);

    // Split on either separator: the archives use '/', a caller holding a
    // std::filesystem::path on Windows-influenced content may hand over '\\', and
    // one shipped blueprint path does mix them.
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find_first_of("/\\", start), path.size());
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }

    std::vector<std::string_view> kept;
    for (const std::string_view part : parts) {
        if (part.empty() || part == ".") {
            continue;  // `//` and `./` say nothing
        }
        if (part == "..") {
            if (kept.empty()) {
                // Climbing above the root. Inside an archive there is nothing up
                // there, but a mounted DIRECTORY would happily serve
                // `/../../../etc/passwd`, so this is refused rather than clamped:
                // clamping would silently turn a hostile path into a valid one.
                return {};
            }
            kept.pop_back();
            continue;
        }
        kept.push_back(part);
    }

    for (const std::string_view part : kept) {
        out.push_back('/');
        for (const char c : part) {
            out.push_back(lower(c));
        }
    }
    return out.empty() ? "/" : out;
}

// Where one resolved name lives. An archive entry is (archive, index); a
// directory entry is a real path.
struct Vfs::Impl {
    struct Archive {
        std::filesystem::path path;
        // Held by pointer so the vector can grow: miniz keeps internal pointers
        // into its own struct, so moving one after init is undefined.
        std::unique_ptr<mz_zip_archive> zip;
    };

    struct Entry {
        int archive = -1;  ///< index into `archives`, or -1 for a real file
        mz_uint indexInArchive = 0;
        std::filesystem::path realPath;  ///< set when archive == -1
        std::string vfsName;             ///< original case, for `list`
    };

    std::vector<Archive> archives;

    // Keyed by normalised (lower-cased) path. A later mount overwrites an earlier
    // one, which IS the priority rule — insert-or-assign, not insert.
    std::map<std::string, Entry> entries;

    /// Registered hook directories, in registration order (`C-312`): the
    /// `SCR_AddHookDirectory` vector. `/schook` from the stock mount spec
    /// first, then each active mod's hookdir in `__active_mods` order — which
    /// is exactly the concat order `C-311` states.
    std::vector<std::string> hookDirs;

    ~Impl() {
        for (Archive& a : archives) {
            if (a.zip) {
                mz_zip_reader_end(a.zip.get());
            }
        }
    }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

Vfs::Vfs() : impl_{std::make_unique<Impl>()} {}
Vfs::~Vfs() = default;
Vfs::Vfs(Vfs&&) noexcept = default;
Vfs& Vfs::operator=(Vfs&&) noexcept = default;

void Vfs::mountDirectory(std::filesystem::path directory) {
    mountDirectoryAt("/", std::move(directory));
}

void Vfs::mountDirectoryAt(std::string_view vfsPrefix, std::filesystem::path directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return;
    }

    const std::string prefix = normalisedVfsPath(vfsPrefix);
    if (prefix.empty()) {
        return;
    }

    for (const auto& item : std::filesystem::recursive_directory_iterator{directory, ec}) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(item.path(), directory, ec);
        if (ec || relative.empty()) {
            continue;
        }
        const std::string key = normalisedVfsPath(relative.generic_string());
        if (key.empty() || key == "/") {
            continue;
        }
        const std::string mounted = prefix == "/" ? key : prefix + key;
        const std::string shown =
            vfsPrefix == "/" ? "/" + relative.generic_string()
                             : std::string{vfsPrefix} + "/" + relative.generic_string();
        impl_->entries.insert_or_assign(
            mounted, Impl::Entry{.archive = -1,
                                 .indexInArchive = 0,
                                 .realPath = item.path(),
                                 .vfsName = shown});
    }
}

bool Vfs::mountArchive(const std::filesystem::path& archive) {
    return mountArchiveAt("/", archive);
}

bool Vfs::mountArchiveAt(std::string_view vfsPrefix, const std::filesystem::path& archive) {
    const std::string prefix = normalisedVfsPath(vfsPrefix);
    if (prefix.empty()) {
        return false;
    }

    auto zip = std::make_unique<mz_zip_archive>();
    *zip = mz_zip_archive{};
    if (mz_zip_reader_init_file(zip.get(), archive.string().c_str(), 0) == MZ_FALSE) {
        return false;
    }

    const int archiveIndex = static_cast<int>(impl_->archives.size());
    const mz_uint count = mz_zip_reader_get_num_files(zip.get());

    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(zip.get(), i) != MZ_FALSE) {
            continue;  // directories are implied by the names of the files in them
        }
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(zip.get(), i, &stat) == MZ_FALSE) {
            continue;  // one unreadable index should not cost the whole archive
        }
        const std::string key = normalisedVfsPath(stat.m_filename);
        if (key.empty() || key == "/") {
            continue;  // a name that climbs out of the root resolves to nothing
        }
        const std::string mounted = prefix == "/" ? key : prefix + key;
        const std::string shown = vfsPrefix == "/"
                                      ? "/" + std::string{stat.m_filename}
                                      : std::string{vfsPrefix} + "/" + std::string{stat.m_filename};
        impl_->entries.insert_or_assign(mounted,
                                        Impl::Entry{.archive = archiveIndex,
                                                    .indexInArchive = i,
                                                    .realPath = {},
                                                    .vfsName = shown});
    }

    impl_->archives.push_back(Impl::Archive{.path = archive, .zip = std::move(zip)});
    return true;
}


std::optional<std::vector<std::byte>> Vfs::read(std::string_view gamePath) const {
    const std::string key = normalisedVfsPath(gamePath);
    const auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) {
        return std::nullopt;
    }
    const Impl::Entry& entry = it->second;

    if (entry.archive < 0) {
        std::ifstream in{entry.realPath, std::ios::binary | std::ios::ate};
        if (!in) {
            return std::nullopt;
        }
        const std::streamoff size = in.tellg();
        if (size < 0) {
            return std::nullopt;
        }
        std::vector<std::byte> out(static_cast<std::size_t>(size));
        in.seekg(0);
        if (size > 0) {
            in.read(reinterpret_cast<char*>(out.data()), size);
            if (!in) {
                return std::nullopt;
            }
        }
        return out;
    }

    mz_zip_archive* zip = impl_->archives[static_cast<std::size_t>(entry.archive)].zip.get();
    std::size_t size = 0;
    void* raw = mz_zip_reader_extract_to_heap(zip, entry.indexInArchive, &size, 0);
    if (raw == nullptr) {
        return std::nullopt;
    }
    std::vector<std::byte> out(size);
    const auto* first = static_cast<const std::byte*>(raw);
    std::copy(first, first + size, out.begin());
    mz_free(raw);
    return out;
}

bool Vfs::contains(std::string_view gamePath) const {
    const std::string key = normalisedVfsPath(gamePath);
    return !key.empty() && impl_->entries.contains(key);
}

std::vector<std::string> Vfs::list(std::string_view directoryPrefix,
                                   std::string_view extension) const {
    std::string prefix = normalisedVfsPath(directoryPrefix);
    if (prefix.empty()) {
        return {};
    }
    if (prefix != "/") {
        prefix.push_back('/');  // so "/units" does not also match "/unitsextra"
    }

    std::string wantedExtension;
    for (const char c : extension) {
        wantedExtension.push_back(lower(c));
    }

    std::vector<std::string> found;
    // The map is keyed by the normalised path, so a prefix scan is a range rather
    // than a walk of every name.
    for (auto it = impl_->entries.lower_bound(prefix); it != impl_->entries.end(); ++it) {
        if (!it->first.starts_with(prefix)) {
            break;
        }
        if (!wantedExtension.empty() && !it->first.ends_with(wantedExtension)) {
            continue;
        }
        found.push_back(it->second.vfsName);
    }
    std::ranges::sort(found);
    return found;
}

std::size_t Vfs::fileCount() const noexcept { return impl_->entries.size(); }

void Vfs::addHookDirectory(std::string_view vfsPrefix) {
    const std::string prefix = normalisedVfsPath(vfsPrefix);
    if (prefix.empty() || prefix == "/") {
        return;  // a hookdir of "/" would hook every file in the game
    }
    impl_->hookDirs.push_back(prefix);
}

std::vector<std::string> Vfs::hooksFor(std::string_view gamePath) const {
    const std::string key = normalisedVfsPath(gamePath);
    std::vector<std::string> found;
    if (key.empty() || key == "/") {
        return found;
    }
    for (const std::string& dir : impl_->hookDirs) {
        const auto it = impl_->entries.find(dir + key);
        if (it != impl_->entries.end()) {
            found.push_back(it->second.vfsName);
        }
    }
    return found;
}

std::span<const std::string> Vfs::hookDirectories() const noexcept {
    return impl_->hookDirs;
}

bool Vfs::mountMod(const ActiveMod& mod) {
    const std::string mountPoint = modMountPoint(mod);
    std::error_code ec;
    bool mounted = false;
    if (std::filesystem::is_directory(mod.location, ec)) {
        mountDirectoryAt(mountPoint, mod.location);
        mounted = true;
    } else if (std::filesystem::is_regular_file(mod.location, ec)) {
        mounted = mountArchiveAt(mountPoint, mod.location);
    }
    if (mounted) {
        // `C-312`: the mount spec's `hook` list drives SCR_AddHookDirectory —
        // for a mod that is its hookdir under the mount point.
        addHookDirectory(modHookDirectory(mod));
    }
    return mounted;
}

std::string modMountPoint(const ActiveMod& mod) {
    // `/mods/<name>` — the location's last component, which is how the shipped
    // mods name themselves (`/mods/Chess`). A location that cannot name one
    // falls back to the uid rather than mounting at a bare `/mods`.
    std::string name = mod.location.filename().string();
    if (name.empty()) {
        name = mod.location.parent_path().filename().string();
    }
    if (name.empty()) {
        name = mod.uid.empty() ? "mod" : mod.uid;
    }
    return "/mods/" + name;
}

std::string modHookDirectory(const ActiveMod& mod) {
    std::string hookdir = mod.hookdir.empty() ? "/hook" : mod.hookdir;
    while (!hookdir.empty() && hookdir.front() == '/') {
        hookdir.erase(hookdir.begin());
    }
    return modMountPoint(mod) + "/" + hookdir;
}

std::vector<ActiveMod> orderActiveMods(std::vector<ActiveMod> mods) {
    // `C-313` / mods.lua `ModComp`: `before`/`after` uid constraints, else
    // uid-alphabetical. ModComp is a comparator over a sortedpairs walk, which
    // is a topological sort with uid tie-breaking stated as a comparator —
    // Kahn's algorithm is the same order said directly, and stays
    // deterministic where a contradictory set would leave std::sort's result
    // arbitrary (mods.lua warns exactly that).
    const std::size_t n = mods.size();

    // Index mods by uid; constraints naming a uid outside the active set are
    // ignored, as ModComp's comparisons against absent entries never fire.
    std::map<std::string, std::size_t> byUid;
    for (std::size_t i = 0; i < n; ++i) {
        byUid.try_emplace(mods[i].uid, i);
    }

    // Edge i→j means "i sorts before j". `before` lists uids this mod precedes;
    // `after` lists uids it follows — and an empty `after` falls back to
    // `requires`, the documented default.
    std::vector<std::vector<std::size_t>> edges(n);
    std::vector<std::size_t> indegree(n, 0);
    const auto link = [&](std::size_t from, std::size_t to) {
        edges[from].push_back(to);
        ++indegree[to];
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::string& uid : mods[i].before) {
            if (const auto it = byUid.find(uid); it != byUid.end()) {
                link(i, it->second);
            }
        }
        const std::vector<std::string>& after =
            mods[i].after.empty() ? mods[i].requiredUids : mods[i].after;
        for (const std::string& uid : after) {
            if (const auto it = byUid.find(uid); it != byUid.end()) {
                link(it->second, i);
            }
        }
    }

    // Ready set ordered by uid — the "else uid-alphabetical" half, applied at
    // every choice point rather than only at the start.
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }
    const auto uidLess = [&](std::size_t a, std::size_t b) {
        return mods[a].uid > mods[b].uid;  // min-first
    };
    std::ranges::make_heap(ready, uidLess);

    std::vector<ActiveMod> ordered;
    ordered.reserve(n);
    while (!ready.empty()) {
        std::ranges::pop_heap(ready, uidLess);
        const std::size_t next = ready.back();
        ready.pop_back();
        for (const std::size_t to : edges[next]) {
            if (--indegree[to] == 0) {
                ready.push_back(to);
                std::ranges::push_heap(ready, uidLess);
            }
        }
        ordered.push_back(std::move(mods[next]));
    }

    // A cycle left nodes unordered — the "inconsistent ordering" mods.lua
    // warns about. Append them uid-sorted rather than dropping them: a mod
    // that asked for the impossible still runs, in a deterministic place.
    if (ordered.size() < n) {
        std::vector<std::size_t> rest;
        for (std::size_t i = 0; i < n; ++i) {
            if (indegree[i] > 0) {
                rest.push_back(i);
            }
        }
        std::ranges::sort(rest, [&](std::size_t a, std::size_t b) {
            return mods[a].uid < mods[b].uid;
        });
        for (const std::size_t i : rest) {
            ordered.push_back(std::move(mods[i]));
        }
    }
    return ordered;
}

} // namespace rm::vfs
