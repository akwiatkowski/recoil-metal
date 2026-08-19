#include "core/blueprint/BlueprintMesh.hpp"

#include <string>

namespace rm::blueprint {
namespace {

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// Whether `text` ends in `suffix`, ignoring case.
[[nodiscard]] bool endsWithNoCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() <= suffix.size()) {
        // Not `<`: a stem that is ONLY the suffix leaves nothing to name a mesh
        // after, so it is no more usable than a stem too short to hold it.
        return false;
    }
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (lower(tail[i]) != lower(suffix[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

std::filesystem::path meshBeside(const std::filesystem::path& blueprintPath,
                                 std::string_view expectedSuffix, std::size_t level) {
    const std::string stem = blueprintPath.stem().string();  // drops ".bp"

    if (!endsWithNoCase(stem, expectedSuffix)) {
        return {};
    }

    const std::string base = stem.substr(0, stem.size() - expectedSuffix.size());
    return blueprintPath.parent_path() / (base + "_lod" + std::to_string(level) + ".scm");
}

} // namespace rm::blueprint
