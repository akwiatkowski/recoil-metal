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
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
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
        impl_->entries.insert_or_assign(
            key, Impl::Entry{.archive = -1,
                             .indexInArchive = 0,
                             .realPath = item.path(),
                             .vfsName = "/" + relative.generic_string()});
    }
}

bool Vfs::mountArchive(const std::filesystem::path& archive) {
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
        impl_->entries.insert_or_assign(key,
                                        Impl::Entry{.archive = archiveIndex,
                                                    .indexInArchive = i,
                                                    .realPath = {},
                                                    .vfsName = "/" + std::string{stat.m_filename}});
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

} // namespace rm::vfs
