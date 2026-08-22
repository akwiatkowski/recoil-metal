#include "core/unit/BarNames.hpp"

namespace rm::unitdef {
namespace {

/// The next JSON string starting at or after `from`, unescaped, or nothing.
/// Advances `from` past the closing quote when a string is found.
[[nodiscard]] bool readString(std::string_view text, std::size_t& from, std::string& out) {
    const std::size_t open = text.find('"', from);
    if (open == std::string_view::npos) {
        return false;
    }
    out.clear();
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            from = i + 1;
            return true;
        }
        if (c == '\\' && i + 1 < text.size()) {
            // The escapes a name actually uses. \uXXXX is skipped whole rather than
            // half-read: four hex digits of garbage in a label is worse than a gap.
            const char next = text[++i];
            switch (next) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'u': i += 4; break;
                default: break;  // \n, \t and friends have no place in a unit name
            }
            continue;
        }
        out.push_back(c);
    }
    return false;  // ran off the end inside a string: malformed
}

} // namespace

std::string barUnitName(std::string_view unitsJson, std::string_view key) {
    // The descriptions object. Found by name rather than by walking the whole document —
    // the file's shape is fixed, and "descriptions" appears exactly once in the corpus.
    const std::size_t section = unitsJson.find("\"descriptions\"");
    if (section == std::string_view::npos) {
        return {};
    }
    const std::size_t open = unitsJson.find('{', section);
    if (open == std::string_view::npos) {
        return {};
    }

    // Key-value pairs until the object closes. Depth stays flat in the shipped file, so the
    // first unmatched '}' ends the section; a string containing '}' cannot confuse this,
    // because both key and value are consumed as whole strings.
    std::size_t cursor = open + 1;
    std::string candidate;
    std::string value;
    while (cursor < unitsJson.size()) {
        const std::size_t brace = unitsJson.find('}', cursor);
        const std::size_t quote = unitsJson.find('"', cursor);
        if (quote == std::string_view::npos || (brace != std::string_view::npos && brace < quote)) {
            return {};  // the object closed (or the text ran out) without the key
        }
        if (!readString(unitsJson, cursor, candidate)) {
            return {};
        }
        if (!readString(unitsJson, cursor, value)) {
            return {};
        }
        if (candidate == key) {
            return value;
        }
    }
    return {};
}

} // namespace rm::unitdef
