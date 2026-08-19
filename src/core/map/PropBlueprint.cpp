#include "core/map/PropBlueprint.hpp"

#include "core/lua/LuaTable.hpp"

#include <string>
#include <utility>

namespace rm::prop {
namespace {

[[nodiscard]] std::unexpected<MapError> fail(MapError::Code code, std::string message) {
    return std::unexpected{MapError{code, std::move(message)}};
}

/// The path a `.scmap` states, made relative so it can be joined onto a root.
///
/// A blueprint path is absolute inside the game's virtual filesystem, so joining
/// it as-is onto a real directory resolves to the filesystem root and silently
/// discards the root — one of the few filesystem mistakes that produces a
/// plausible path rather than an error.
[[nodiscard]] std::string withoutLeadingSlash(std::string_view gameRelativePath) {
    std::string relative{gameRelativePath};
    if (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.erase(0, 1);
    }
    return relative;
}

/// `X_prop.bp` -> `X_lod0.scm`, beside it.
///
/// The convention IS the lookup: a blueprint's LOD table gives textures and
/// cutoff distances and never names the geometry, so the engine finds the mesh
/// by the file name. Verified against the corpus — 199 of the 207 blueprints the
/// stock maps name resolve this way and the other eight have no mesh at all.
///
/// The suffix is matched case-insensitively because the two spellings disagree
/// in the shipped content: the maps say `/env/tropical/props/trees/palm02_s4_prop.bp`
/// in lower case while the archive holds `env/Tropical/Props/Trees/Palm02_s4_lod0.scm`.
/// macOS's default filesystem is case-insensitive so the JOIN survives that, but
/// a string comparison on the suffix would not.
[[nodiscard]] std::filesystem::path meshBeside(const std::filesystem::path& blueprintPath) {
    const std::string stem = blueprintPath.stem().string();  // drops ".bp"
    constexpr std::string_view kSuffix = "_prop";

    if (stem.size() <= kSuffix.size()) {
        return {};
    }
    const std::string tail = stem.substr(stem.size() - kSuffix.size());
    for (std::size_t i = 0; i < kSuffix.size(); ++i) {
        const char lowered =
            (tail[i] >= 'A' && tail[i] <= 'Z') ? static_cast<char>(tail[i] + ('a' - 'A')) : tail[i];
        if (lowered != kSuffix[i]) {
            return {};
        }
    }

    const std::string base = stem.substr(0, stem.size() - kSuffix.size());
    return blueprintPath.parent_path() / (base + "_lod0.scm");
}

} // namespace

std::expected<Blueprint, MapError> loadFile(const std::filesystem::path& root,
                                            std::string_view gameRelativePath) {
    const std::filesystem::path path = root / withoutLeadingSlash(gameRelativePath);

    const auto table = lua::parseTableFile(path.string());
    if (!table) {
        // Covers both a file that is not there and one that is not data — the
        // reader reports the second with a line number, which is the part worth
        // keeping.
        return fail(MapError::Code::BadHeader,
                    "prop blueprint \"" + std::string{gameRelativePath}
                        + "\" not read: " + table.error().message);
    }

    Blueprint blueprint;

    // A mesh is what makes a prop drawable, so its absence decides the outcome
    // before anything else is read.
    const std::filesystem::path mesh = meshBeside(path);
    std::error_code ec;
    if (mesh.empty() || !std::filesystem::is_regular_file(mesh, ec)) {
        return fail(MapError::Code::MissingMesh,
                    "prop blueprint \"" + std::string{gameRelativePath}
                        + "\" names no mesh (an emitter, or content not extracted)");
    }
    blueprint.mesh = mesh;

    if (const std::optional<double> scale = table->path("Display")
                                                ? table->path("Display")->numberAt("UniformScale")
                                                : std::nullopt) {
        blueprint.uniformScale = static_cast<float>(*scale);
    }

    // The FIRST LOD's albedo. `LODs` is a Lua array, so its entries are
    // positional and land in `items`; they run finest-first, and only the mesh's
    // LOD 0 is ever drawn here.
    if (const lua::Value* lods = table->path("Display", "Mesh", "LODs");
        lods != nullptr && !lods->items.empty()) {
        if (const std::optional<std::string_view> albedo = lods->items.front().stringAt("AlbedoName")) {
            // Named relative to the blueprint's own directory, not to the game
            // root — the LOD table gives a bare file name.
            const std::filesystem::path texture = path.parent_path() / *albedo;
            if (std::filesystem::is_regular_file(texture, ec)) {
                blueprint.albedo = texture;
            }
        }
    }

    return blueprint;
}

} // namespace rm::prop
