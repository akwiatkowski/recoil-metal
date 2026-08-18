#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rm::vfs {

// A list of filesystem roots searched in order when resolving content paths.
//
// Recoil's VFS is sectioned and archive-based; this is the smallest useful
// subset for recoil-metal: multiple roots, case-insensitive filename matching,
// and recursive descent under each root. It lets a user point the app at one or
// more extracted mod/map directories (or temporary extractions of .sdz files)
// and have model/texture lookups find them without hard-coding a single layout.
class AssetSearch {
public:
    /// Adds a directory root at the back of the search order.
    void addRoot(std::filesystem::path directory);

    /// Tries to resolve `relativePath` under each root in order.
    ///
    /// If the path is already absolute and exists, it is returned as-is.
    /// Otherwise each root is tried; the first regular file that exists wins.
    /// Returns an empty path if nothing resolves.
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& relativePath) const;

    /// Tries to resolve a file by its filename alone, case-insensitively,
    /// searching recursively under each root.
    ///
    /// This is what model loading needs: definitions name `Units/ARMPW.s3o`
    /// but the actual file may live several levels deep with different casing.
    [[nodiscard]] std::filesystem::path resolveByName(std::string_view filename) const;

    [[nodiscard]] std::size_t rootCount() const noexcept { return roots_.size(); }
    [[nodiscard]] bool empty() const noexcept { return roots_.empty(); }

private:
    std::vector<std::filesystem::path> roots_;
};

/// Whether an archive entry's name may be joined onto a destination directory.
///
/// A ZIP stores whatever name it likes, including `../../etc/passwd` or an
/// absolute path — and `std::filesystem::path`'s `/` REPLACES the left side
/// when the right is absolute, so a naive join writes wherever the archive
/// says. That is the Zip Slip vulnerability. Refusing absolute paths and any
/// `..` component keeps an extraction inside the directory it was given.
///
/// Exposed because it is the security-relevant half of extractZip and the only
/// half that can be tested without building an archive.
[[nodiscard]] bool isSafeArchivePath(std::string_view entryName) noexcept;

/// Extracts a ZIP archive (`*.sdz`) into `destination`, creating the directory
/// if necessary. Returns true on success. Existing files are overwritten so the
/// extraction is idempotent for a given archive.
///
/// Entries that would escape `destination` are refused — see isSafeArchivePath.
[[nodiscard]] bool extractZip(const std::filesystem::path& archive,
                              const std::filesystem::path& destination);

} // namespace rm::vfs
