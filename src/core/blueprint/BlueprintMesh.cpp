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

ScriptBinding scriptBindingFor(std::string_view blueprintPath,
                               std::string_view authoredModule,
                               std::string_view authoredClass,
                               std::string_view fallbackModule,
                               std::string_view fallbackClass) {
    ScriptBinding out;

    // `C-271`: `ScriptModule` wins when authored; else the blueprint's own
    // suffix convention — `X_unit.bp`/`X_prop.bp`/`X_proj.bp` → `X_script.lua`
    // beside it. A stem without the suffix has no conventional script, so the
    // module falls straight to the caller's `/lua/sim/*.lua` default.
    if (!authoredModule.empty()) {
        out.module = authoredModule;
    } else {
        const std::string_view stem = blueprintPath.substr(
            blueprintPath.find_last_of('/') == std::string_view::npos
                ? 0
                : blueprintPath.find_last_of('/') + 1);
        const std::string_view dir = blueprintPath.substr(
            0, blueprintPath.size() - stem.size());
        for (std::string_view suffix : {kUnitSuffix, kPropSuffix, kProjectileSuffix}) {
            const std::string_view ext = ".bp";
            if (stem.size() > suffix.size() + ext.size()
                && stem.substr(stem.size() - ext.size()) == ext
                && endsWithNoCase(stem.substr(0, stem.size() - ext.size()), suffix)) {
                const std::string_view base =
                    stem.substr(0, stem.size() - ext.size() - suffix.size());
                out.module = std::string{dir} + std::string{base} + "_script.lua";
                break;
            }
        }
        if (out.module.empty()) {
            out.module = fallbackModule;
        }
    }

    // `ScriptClass` wins when authored; else `TypeClass`. A module that lacks
    // the class falls back to the class NAME inside the module — which is the
    // same name the caller already has, so the binding itself is unchanged.
    out.className = authoredClass.empty() ? "TypeClass" : std::string{authoredClass};
    if (out.className.empty()) {
        out.className = fallbackClass;
    }
    return out;
}

} // namespace rm::blueprint
