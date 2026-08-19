#include "core/map/PropBlueprint.hpp"

#include "core/blueprint/BlueprintMesh.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/map/Scmap.hpp"

#include <algorithm>
#include <cctype>

#include <fstream>
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

/// Which ambient effect a blueprint marks, from its file name.
///
/// The name, because nothing else identifies it. `HelpText` says the same thing in
/// prose and is display text — localised in principle, and "Small lava steam steam"
/// in practice, which is not something to parse. `ScriptClass` and `ScriptModule`
/// name Lua this project does not run. The asset's own name is the stable handle.
[[nodiscard]] Effect effectFromName(std::string_view path) {
    std::string lowered{path};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lowered.find("lavasteam") != std::string::npos) return Effect::Steam;
    if (lowered.find("surfacemist") != std::string::npos) return Effect::Mist;
    if (lowered.find("bubbles") != std::string::npos) return Effect::Bubbles;
    if (lowered.find("blowingsand") != std::string::npos) return Effect::BlowingSand;
    if (lowered.find("blowingsnow") != std::string::npos) return Effect::BlowingSnow;
    return Effect::None;
}

// The reader proper, over two things that differ between a real disk and mounted
// content and NOTHING else: whether a path names something readable, and how a
// game-relative path becomes one this caller can use.
//
// A template rather than std::function because this runs 207 times per map load and
// the two lambdas are known at each call site — and because the alternative was two
// copies of eighty lines that resolve LOD meshes, albedos, normals and cutoffs, which
// is precisely the duplication that lets one copy quietly grow a bug the other does
// not have.
template <typename Exists, typename FromGamePath>
[[nodiscard]] std::expected<Blueprint, MapError> parseBlueprint(
    std::string_view source, std::string_view gameRelativePath,
    const std::filesystem::path& path, Exists exists, FromGamePath fromGamePath) {
    const auto table = lua::parseTable(source);
    if (!table) {
        // Covers both a file that is not there and one that is not data — the
        // reader reports the second with a line number, which is the part worth
        // keeping.
        return fail(MapError::Code::BadHeader,
                    "prop blueprint \"" + std::string{gameRelativePath}
                        + "\" not read: " + table.error().message);
    }

    Blueprint blueprint;

    if (const std::optional<double> scale = table->path("Display")
                                                ? table->path("Display")->numberAt("UniformScale")
                                                : std::nullopt) {
        blueprint.uniformScale = static_cast<float>(*scale);
    }

    // The LOD table decides how many levels there are, what each is painted with,
    // and how far out each is the right one to draw. `LODs` is a Lua array, so its
    // entries are positional and land in `items`, and they run finest-first.
    const lua::Value* lods = table->path("Display", "Mesh", "LODs");

    for (std::size_t level = 0; lods != nullptr && level < lods->items.size(); ++level) {
        // MeshName, when the blueprint states one: a path inside the game's virtual
        // filesystem rather than a name beside the blueprint. Twelve of the shipped
        // blueprints use it, mostly the effect markers pointing at an editor marker
        // — so it rarely names anything worth drawing, but it IS the format's own
        // answer and the convention below is only what the engine falls back to.
        std::filesystem::path mesh;
        if (const std::optional<std::string_view> named =
                lods->items[level].stringAt("MeshName")) {
            mesh = fromGamePath(*named);
        } else {
            mesh = blueprint::meshBeside(path, blueprint::kPropSuffix, level);
        }
        if (mesh.empty() || !exists(mesh)) {
            // Ends the list rather than failing: the table describes more levels
            // than the archive ships in a few cases, and a prop with a fine level
            // and no coarse one is still a prop. Level 0 missing is a different
            // matter and is caught below.
            break;
        }

        BlueprintLod lod;
        lod.mesh = mesh;

        // Each level names its own albedo — a distant tree gets a smaller one — so
        // this is read per level rather than taken from the first. Pairing level 2's
        // geometry with level 0's texture is the wrong way round of the mistake that
        // used to be here, and just as invisible.
        if (const std::optional<std::string_view> albedo =
                lods->items[level].stringAt("AlbedoName")) {
            // Named relative to the blueprint's own directory, not to the game
            // root — the LOD table gives a bare file name.
            const std::filesystem::path texture = path.parent_path() / *albedo;
            if (exists(texture)) {
                lod.albedo = texture;
            }
        }

        // ...and its normal map, when the blueprint names one. 244 of the shipped
        // levels do.
        if (const std::optional<std::string_view> normals =
                lods->items[level].stringAt("NormalsName")) {
            const std::filesystem::path texture = path.parent_path() / *normals;
            if (exists(texture)) {
                lod.normals = texture;
            }
        }

        // A non-positive cutoff means no limit — only the four DevTest blueprints
        // do that, and no stock map references them, but reading -1 as "stop
        // drawing at -8 elmos" would make them permanently invisible instead of
        // permanently visible.
        const std::optional<double> cutoff = lods->items[level].numberAt("LODCutoff");
        if (cutoff && *cutoff > 0.0) {
            lod.cutoffElmos = static_cast<float>(*cutoff) * scmap::kElmosPerOgrid;
        }

        blueprint.lods.push_back(std::move(lod));
    }

    // A SCALE OF ZERO means the blueprint draws nothing, whatever mesh it names —
    // and every one of the eight effect markers in the corpus says exactly that,
    // while pointing MeshName at an editor marker. So it is the structural signal
    // for "this is a place, not a thing", and the name says which effect.
    if (blueprint.uniformScale == 0.0f) {
        blueprint.effect = effectFromName(gameRelativePath);
        blueprint.lods.clear();
        if (blueprint.effect != Effect::None) {
            return blueprint;
        }
    }

    // No geometry and no effect anyone recognises: either content that was not
    // extracted, or a marker whose name this reader does not know.
    if (blueprint.lods.empty()) {
        return fail(MapError::Code::MissingMesh,
                    "prop blueprint \"" + std::string{gameRelativePath}
                        + "\" names no mesh and marks no effect this reader knows");
    }

    // Cutoffs must increase outwards, or the level chosen for a distance is
    // whichever the search happened to reach first. A blueprint stating them out of
    // order is one whose LOD table this reader has misunderstood, so it is fixed
    // here by taking the running maximum rather than trusted or refused: the effect
    // is that a level never draws nearer than the one before it.
    for (std::size_t i = 1; i < blueprint.lods.size(); ++i) {
        blueprint.lods[i].cutoffElmos =
            std::max(blueprint.lods[i].cutoffElmos, blueprint.lods[i - 1].cutoffElmos);
    }

    return blueprint;
}

} // namespace

std::expected<Blueprint, MapError> loadFile(const std::filesystem::path& root,
                                            std::string_view gameRelativePath) {
    const std::filesystem::path path = root / withoutLeadingSlash(gameRelativePath);

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return fail(MapError::Code::BadHeader,
                    "prop blueprint \"" + std::string{gameRelativePath} + "\" not read: cannot"
                        " open " + path.string());
    }
    const std::string source{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    std::error_code ec;
    return parseBlueprint(
        source, gameRelativePath, path,
        [&ec](const std::filesystem::path& candidate) {
            return std::filesystem::is_regular_file(candidate, ec);
        },
        [&root](std::string_view gamePath) {
            return root / withoutLeadingSlash(gamePath);
        });
}

std::expected<Blueprint, MapError> loadFromContent(const vfs::Vfs& content,
                                                   std::string_view gameRelativePath) {
    const auto bytes = content.read(gameRelativePath);
    if (!bytes) {
        return fail(MapError::Code::BadHeader,
                    "prop blueprint \"" + std::string{gameRelativePath}
                        + "\" is not in the mounted content");
    }
    const std::string source{reinterpret_cast<const char*>(bytes->data()), bytes->size()};

    // The VFS speaks game paths natively, so the join is the identity and the
    // blueprint's own path needs no rooting — which is what a game path was for.
    return parseBlueprint(
        source, gameRelativePath, std::filesystem::path{gameRelativePath},
        [&content](const std::filesystem::path& candidate) {
            return content.contains(candidate.generic_string());
        },
        [](std::string_view gamePath) { return std::filesystem::path{gamePath}; });
}

} // namespace rm::prop
