#include "core/map/MapInfo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

/// ASCII lower-case copy. mapinfo.lua keys are conventionally lower-case, but
/// the engine's own docs spell them minHeight/maxHeight, so matching is done
/// case-insensitively rather than betting on one convention.
[[nodiscard]] std::string toLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    std::transform(text.begin(), text.end(), std::back_inserter(lowered),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

/// Reads `<key> = <number>` and returns the number.
///
/// Deliberately literal: it finds the key, steps over whitespace and a single
/// '=', and parses a float. Anything else — a computed expression, a variable,
/// a string — fails and yields nullopt, which is the honest answer for a
/// scanner that is not a Lua interpreter.
[[nodiscard]] std::optional<float> findNumericAssignment(const std::string& haystack,
                                                         std::string_view key) {
    std::size_t searchFrom = 0;

    while (true) {
        const std::size_t keyPos = haystack.find(key, searchFrom);
        if (keyPos == std::string::npos) {
            return std::nullopt;
        }
        searchFrom = keyPos + key.size();

        // Reject a match that is part of a longer identifier, so looking for
        // "height" cannot latch onto "maxheight" (and vice versa).
        const bool precededByIdent =
            keyPos > 0 && (std::isalnum(static_cast<unsigned char>(haystack[keyPos - 1])) != 0
                           || haystack[keyPos - 1] == '_');
        if (precededByIdent) {
            continue;
        }

        std::size_t cursor = searchFrom;
        while (cursor < haystack.size()
               && std::isspace(static_cast<unsigned char>(haystack[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= haystack.size() || haystack[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < haystack.size()
               && std::isspace(static_cast<unsigned char>(haystack[cursor])) != 0) {
            ++cursor;
        }

        // std::from_chars for floating point is still unimplemented in Apple's
        // libc++ (the overload is explicitly deleted), so strtof it is. Safe
        // here because haystack is a std::string and therefore NUL-terminated.
        const char* first = haystack.c_str() + cursor;
        char* parseEnd = nullptr;
        const float value = std::strtof(first, &parseEnd);
        if (parseEnd != first) {
            return value;
        }
        // Not a plain number (an expression, a string, ...) — keep looking in
        // case a later occurrence is usable.
    }
}

} // namespace

namespace rm::mapinfo {

std::optional<VerticalRange> findVerticalRange(std::string_view lua) {
    const std::string lowered = toLower(lua);

    const std::optional<float> minHeight = findNumericAssignment(lowered, "minheight");
    const std::optional<float> maxHeight = findNumericAssignment(lowered, "maxheight");

    // Both or neither: a half-override would silently mix a Lua value with a
    // header value, and the engine treats the two keys independently only
    // because it has real defaults to fall back on. We do not.
    if (!minHeight.has_value() || !maxHeight.has_value()) {
        return std::nullopt;
    }

    return VerticalRange{*minHeight, *maxHeight};
}

std::optional<VerticalRange> findVerticalRangeInFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }

    const std::string contents{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
    return findVerticalRange(contents);
}

std::optional<std::filesystem::path> findBesideMap(const std::filesystem::path& smfPath) {
    const std::filesystem::path directory = smfPath.parent_path();

    const std::filesystem::path candidates[] = {
        directory / "mapinfo.lua",
        directory.parent_path() / "mapinfo.lua",
    };

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return std::nullopt;
}

} // namespace rm::mapinfo
