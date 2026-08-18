#include "core/vfs/AssetSearch.hpp"

// miniz is a single-file MIT-licensed zlib replacement with ZIP support.
// The implementation is compiled separately from third_party/miniz/miniz.c so
// the header is included here without MINIZ_IMPLEMENTATION.
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace rm::vfs {

namespace {

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

void AssetSearch::addRoot(std::filesystem::path directory) {
    std::error_code error;
    directory = std::filesystem::canonical(directory, error);
    if (!error) {
        roots_.push_back(std::move(directory));
    }
}

std::filesystem::path AssetSearch::resolve(const std::filesystem::path& relativePath) const {
    if (relativePath.is_absolute()) {
        std::error_code error;
        if (std::filesystem::is_regular_file(relativePath, error) && !error) {
            return relativePath;
        }
    }

    std::error_code error;
    for (const std::filesystem::path& root : roots_) {
        const std::filesystem::path candidate = root / relativePath;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }

    return {};
}

std::filesystem::path AssetSearch::resolveByName(std::string_view filename) const {
    if (filename.empty()) {
        return {};
    }

    const std::string wanted = lowercased(std::string{filename});
    std::error_code error;

    for (const std::filesystem::path& root : roots_) {
        std::filesystem::recursive_directory_iterator walk{
            root, std::filesystem::directory_options::skip_permission_denied, error};
        if (error) {
            continue;
        }

        for (const std::filesystem::directory_entry& entry : walk) {
            if (!entry.is_regular_file(error) || error) {
                continue;
            }
            if (lowercased(entry.path().filename().string()) == wanted) {
                return entry.path();
            }
        }
    }

    return {};
}

bool isSafeArchivePath(std::string_view entryName) noexcept {
    if (entryName.empty()) {
        return false;
    }

    const std::filesystem::path entry{entryName};
    if (entry.is_absolute()) {
        return false;
    }

    // Also catches a leading "/" spelled with backslashes on an archive written
    // by a Windows tool, since path splits on either separator here.
    for (const std::filesystem::path& part : entry) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}

bool extractZip(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        return false;
    }

    const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
    bool ok = true;

    for (mz_uint i = 0; i < fileCount && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            continue;
        }

        // An entry that would land outside the destination is skipped rather
        // than failing the whole extraction: one hostile or malformed name in a
        // game archive should not stop the rest of it loading.
        if (!isSafeArchivePath(stat.m_filename)) {
            continue;
        }

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::filesystem::path dir = destination / stat.m_filename;
            std::filesystem::create_directories(dir, error);
            ok = !error;
            continue;
        }

        const std::filesystem::path outPath = destination / stat.m_filename;
        std::filesystem::create_directories(outPath.parent_path(), error);
        if (error) {
            ok = false;
            continue;
        }

        ok = mz_zip_reader_extract_to_file(&zip, i, outPath.string().c_str(), 0) != 0;
    }

    mz_zip_reader_end(&zip);
    return ok;
}

} // namespace rm::vfs
