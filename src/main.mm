#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "core/map/MapInfo.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/Scmap.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/map/TerrainType.hpp"
#include "core/model/S3o.hpp"
#include "core/model/Sca.hpp"
#include "core/model/Scm.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/texture/Dds.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/mesh/TerrainMesh.hpp"
#include "platform/Window.hpp"
#include "render/Renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Quits the process when the last window closes. Without a delegate the app
// lingers windowless in the Dock, which is endlessly confusing in a terminal
// workflow. (Objective-C declarations must be at global scope — the RM
// prefix is the namespacing mechanism, per Cocoa convention.)
@interface RMAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation RMAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}
@end

namespace {

// Fallback map size when no .smf is supplied: 256 squares is 2048 elmos across,
// big enough to look like terrain and small enough to build instantly.
constexpr int kDemoSquares = 256;

// Vertical range for the demo, chosen to match the relief ratio real content
// actually has. FAR measured typical SupCom relief at 3-6% of map width (report
// 06 section 8.2) and BAR maps are somewhat steeper; 400 elmos over a 2048-elmo
// map is about 20%, which reads as proper hills.
//
// The temptation is to span the format's full 4096-elmo range because the
// heightmap is 16-bit — but real maps use only a slice of that range, and using
// all of it produces vertical needles rather than terrain. Learned by looking.
constexpr float kDemoMinHeight = -40.0f;
constexpr float kDemoMaxHeight = 360.0f;

/// Loads a Recoil `.smf` heightmap, honouring mapinfo.lua's vertical range.
/// Returns std::nullopt when the map failed to load — falling back silently
/// there would hide the error the user needs to see.
[[nodiscard]] std::optional<rm::HeightField> resolveSmfHeightField(const std::string& path) {
    auto field = rm::smf::loadFile(path);
    if (!field) {
        std::fprintf(stderr, "failed to load \"%s\": %s\n",
                     path.c_str(), field.error().message.c_str());
        return std::nullopt;
    }

    std::printf("loaded %s: %d x %d squares (%.0f x %.0f elmos)\n",
                path.c_str(), field->squaresX, field->squaresZ,
                static_cast<double>(field->widthElmos()),
                static_cast<double>(field->depthElmos()));

    // mapinfo.lua overrides the binary header's vertical range. Skipping this
    // renders maps with an inverted header upside down — BAR's Angel Crossing
    // is one such map. See core/map/MapInfo.hpp.
    const auto infoPath = rm::mapinfo::findBesideMap(path);
    if (!infoPath) {
        std::printf("  no mapinfo.lua alongside; using the binary header range\n");
        return std::move(*field);
    }

    const auto info = rm::mapinfo::parseFile(*infoPath);
    if (!info) {
        // A mapinfo.lua we cannot read is worth saying out loud rather than
        // ignoring: the header values we fall back to may well be wrong.
        std::fprintf(stderr, "  warning: could not read %s (%s); keeping header range\n",
                     infoPath->filename().string().c_str(), info.error().message.c_str());
        return std::move(*field);
    }

    if (info->verticalRange) {
        const float headerMin = field->baseHeight;
        const float headerMax =
            field->baseHeight + field->heightScale * rm::kHeightQuantisationSteps;
        std::printf("  %s overrides header range %.1f..%.1f -> %.1f..%.1f elmos\n",
                    infoPath->filename().string().c_str(),
                    static_cast<double>(headerMin), static_cast<double>(headerMax),
                    static_cast<double>(info->verticalRange->minHeight),
                    static_cast<double>(info->verticalRange->maxHeight));
        field->setVerticalRange(info->verticalRange->minHeight,
                                info->verticalRange->maxHeight);
    }

    return std::move(*field);
}

/// Loads and assembles the ground texture for a map, if its .smt is on disk.
///
/// A missing .smt is not fatal — the engine tolerates it too, rendering those
/// tiles flat (SMFGroundTextures.cpp:147-157) — so this reports and returns
/// nullopt rather than failing the launch.
[[nodiscard]] std::optional<rm::TileAtlas> resolveAtlas(const std::filesystem::path& smfPath) {
    const auto index = rm::smf::loadTileIndexFile(smfPath);
    if (!index) {
        std::fprintf(stderr, "  no tile index: %s\n", index.error().message.c_str());
        return std::nullopt;
    }

    // mapinfo.lua may rename the tile files; the engine only honours the
    // override when the count matches (SMFGroundTextures.cpp:122-127).
    std::vector<std::string> names = index->smtFileNames;
    if (const auto infoPath = rm::mapinfo::findBesideMap(smfPath)) {
        if (const auto info = rm::mapinfo::parseFile(*infoPath)) {
            if (info->smtFileNames.size() == names.size()) {
                names = info->smtFileNames;
            }
        }
    }

    if (names.empty()) {
        std::fprintf(stderr, "  map declares no tile files\n");
        return std::nullopt;
    }
    if (names.size() > 1) {
        // Multi-file tile sets concatenate their index spaces. Nothing here
        // stops that working, but no test covers it, so say so rather than
        // quietly rendering the first file's tiles for every index.
        std::fprintf(stderr, "  warning: %zu tile files; only the first is loaded\n",
                     names.size());
    }

    // Names are relative to the map file's directory (SMFGroundTextures.cpp:138-145).
    const std::filesystem::path smtPath =
        smfPath.parent_path() / std::filesystem::path{names[0]}.filename();

    const auto tiles = rm::smt::loadFile(smtPath);
    if (!tiles) {
        std::fprintf(stderr, "  no ground texture (%s): %s\n",
                     smtPath.filename().string().c_str(), tiles.error().message.c_str());
        return std::nullopt;
    }

    auto atlas = rm::buildTileAtlas(*index, *tiles);
    if (!atlas) {
        std::fprintf(stderr, "  could not build atlas: %s\n", atlas.error().message.c_str());
        return std::nullopt;
    }

    std::printf("texture: %d x %d texels, %d tiles, %zu MiB BC1\n",
                atlas->widthTexels, atlas->heightTexels, tiles->tileCount,
                atlas->data.size() / (1024 * 1024));
    return std::move(*atlas);
}

// Everything a loaded map contributes to the scene, whichever family it came
// from. The renderer takes the same calls either way — the point of the whole
// HeightField seam.
struct LoadedMap {
    rm::HeightField field;

    std::optional<rm::TileAtlas> atlas;       ///< SMF: a baked BC1 ground texture
    std::optional<rm::ColourImage> colours;   ///< .scmap: terrain-type bands

    std::vector<rm::mapinfo::StartPosition> starts;

    // Recoil's water is a fixed plane at y = 0 that every map shares; Supreme
    // Commander stores a level per map, and 17 of the 60 stock maps are dry.
    bool hasWater = true;
    float waterLevel = 0.0f;
};

/// Loads a Supreme Commander `.scmap`.
///
/// No mapinfo.lua equivalent and no start positions: those live in the map's
/// `_scenario.lua`, which is a later milestone. Units therefore scatter rather
/// than spawning at bases on these maps.
[[nodiscard]] std::optional<LoadedMap> resolveScmap(const std::string& path) {
    auto map = rm::scmap::loadFile(path);
    if (!map) {
        std::fprintf(stderr, "failed to load \"%s\": %s\n", path.c_str(),
                     map.error().message.c_str());
        return std::nullopt;
    }

    std::printf("loaded %s: %d x %d squares (%.0f x %.0f elmos), Supreme Commander v%d\n",
                path.c_str(), map->field.squaresX, map->field.squaresZ,
                static_cast<double>(map->field.widthElmos()),
                static_cast<double>(map->field.depthElmos()), rm::scmap::kVersionMinor);

    if (!map->endsExactlyAtEof) {
        // Not fatal — everything drawn is read before the last two sections —
        // but it means a field width above them is wrong, and saying so is the
        // whole value of parsing them.
        std::fprintf(stderr, "  warning: the sequential parse did not land on EOF; a "
                             "section width is wrong\n");
    }

    LoadedMap loaded;
    loaded.hasWater = map->hasWater;
    loaded.waterLevel = map->waterElevation;
    std::printf("  water: %s at %.1f elmos\n", map->hasWater ? "yes" : "none (dry map)",
                static_cast<double>(map->waterElevation));

    rm::ColourImage colours =
        rm::colourTerrainTypes(map->terrainType, map->typesX, map->typesZ);
    if (colours.empty()) {
        std::fprintf(stderr, "  no terrain-type colouring; shading by elevation instead\n");
    } else {
        std::printf("  ground: %d x %d terrain-type bands (%zu MiB RGBA8)\n", colours.width,
                    colours.height, colours.rgba.size() / (1024 * 1024));
        loaded.colours = std::move(colours);
    }

    loaded.field = std::move(map->field);
    return loaded;
}

/// Reads a file's first bytes, for telling the two map families apart.
[[nodiscard]] std::vector<std::byte> readMagic(const std::filesystem::path& path,
                                               std::size_t count) {
    std::vector<std::byte> magic(count);
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    file.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(count));
    magic.resize(static_cast<std::size_t>(file.gcount()));
    return magic;
}

/// The map named on the command line, or a synthesised one if none is given.
///
/// Which family a file belongs to is decided by its MAGIC, not its extension:
/// `.smf` opens with "spring map file" and `.scmap` with "Map\x1a". Sniffing
/// costs four bytes and means neither needs its own flag — and everything
/// downstream, renderer included, cannot tell which one it got. That is the
/// whole return on having built HeightField format-agnostic at milestone 2.
[[nodiscard]] std::optional<LoadedMap> resolveMap(int argc, const char* argv[]) {
    if (argc < 2) {
        std::printf("no map given, rendering a procedural field\n"
                    "  usage: recoil-metal <path/to/map.smf | path/to/map.scmap>\n");
        LoadedMap loaded;
        loaded.field = rm::makeSineHills(kDemoSquares, kDemoSquares, kDemoMinHeight,
                                         kDemoMaxHeight);
        return loaded;
    }

    const std::string path = argv[1];

    if (rm::scmap::looksLikeScmap(readMagic(path, sizeof(rm::scmap::kMagic)))) {
        return resolveScmap(path);
    }

    auto field = resolveSmfHeightField(path);
    if (!field) {
        return std::nullopt;
    }

    LoadedMap loaded;
    loaded.field = std::move(*field);
    loaded.atlas = resolveAtlas(path);

    // Start positions come from the same mapinfo.lua already consulted for the
    // height range, so this costs one extra parse and no new plumbing.
    if (const auto infoPath = rm::mapinfo::findBesideMap(path)) {
        if (const auto info = rm::mapinfo::parseFile(*infoPath)) {
            loaded.starts = info->startPositions;
        }
    }

    return loaded;
}

// Where BAR's reference content lives. Used only to resolve a model's texture
// names, which S3O carries but does not contain.
constexpr const char* kBarTextureDir =
    "projects/llm/games/forged-alliance-reborn/reference/BAR/unittextures";

// Supreme Commander's world unit is the ogrid, eight elmos across, exactly as on
// its maps (core/map/Scmap.hpp). A .scm's vertices are in ogrids, so a model
// drawn at scale 1 next to Recoil content would be a eighth of its proper size.
constexpr float kOgridScale = 8.0f;

// A .scm names no textures at all — Supreme Commander resolves them by
// convention from the model's own file name, which is what these reproduce.
// `_normalsTS` exists too and is not read: there is no normal-map path yet.
constexpr const char* kScmDiffuseSuffix = "_Albedo.dds";
constexpr const char* kScmShadingSuffix = "_SpecTeam.dds";

/// How many scattered instances by default. A single unit is well under 1% of an
/// 8192-elmo map's width and effectively invisible when framed, so seeing units
/// on a map means seeing many of them.
constexpr std::size_t kDefaultUnitCount = 800;

/// Seed for the scatter. Fixed, because a benchmark whose scene changes between
/// runs is not a benchmark; each extra model offsets it so two models do not
/// land in exactly the same places.
constexpr std::uint32_t kScatterSeed = 20260817u;

// Loads each texture once, however many models name it.
//
// This is the half of batching that actually saves something: BAR unit textures
// are 4096x4096, and a faction's models overwhelmingly share one pair, so a
// dozen models cost one upload rather than a dozen. The renderer's job is only
// to avoid rebinding them.
class TextureRegistry {
public:
    /// Index for a texture file, loading it on first sight. -1 when the model
    /// named none or the file could not be read.
    ///
    /// Keyed by full path rather than by the name a model used, because the two
    /// families name textures completely differently — .s3o carries a file name
    /// resolved under BAR's unittextures/, .scm carries nothing and its textures
    /// are found by convention beside it — and only the path is comparable.
    [[nodiscard]] int resolve(const std::filesystem::path& path, const char* slot) {
        if (path.empty()) {
            return -1;
        }

        const std::string key = path.string();
        const auto known = indexByPath_.find(key);
        if (known != indexByPath_.end()) {
            return known->second;
        }

        auto texture = rm::dds::loadFile(path);
        if (!texture) {
            // Not fatal: the shader has a defined fallback for each slot, and
            // half a model's shading beats no model. Remembered as -1 so a
            // second model naming the same missing file does not retry it.
            std::fprintf(stderr, "  no %s texture (%s): %s\n", slot,
                         path.filename().string().c_str(), texture.error().message.c_str());
            indexByPath_.emplace(key, -1);
            return -1;
        }

        std::printf("  %s %s: %dx%d, %d mips\n", slot, path.filename().string().c_str(),
                    texture->width, texture->height, texture->mipLevels);

        const auto index = static_cast<int>(textures_.size());
        textures_.push_back(std::move(*texture));
        indexByPath_.emplace(key, index);
        return index;
    }

    [[nodiscard]] std::span<const rm::dds::Texture> all() const noexcept { return textures_; }
    [[nodiscard]] std::size_t size() const noexcept { return textures_.size(); }

private:
    std::vector<rm::dds::Texture> textures_;
    std::map<std::string, int> indexByPath_;
};

/// Where a Recoil model's named texture lives: under BAR's unittextures/.
[[nodiscard]] std::filesystem::path barTexturePath(const std::string& name) {
    if (name.empty()) {
        return {};
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home == nullptr ? "" : home} / kBarTextureDir / name;
}

/// Where a Supreme Commander model's texture lives: beside the model, named
/// after it.
///
/// The LOD suffix is part of the texture name for every level EXCEPT lod0, whose
/// textures drop it — `DAA0206_lod0.scm` uses `DAA0206_Albedo.dds` while
/// `DAA0206_lod1.scm` uses `DAA0206_lod1_Albedo.dds`. Both spellings are tried
/// rather than assuming, since the exception is the common case.
[[nodiscard]] std::filesystem::path scmTexturePath(const std::filesystem::path& modelPath,
                                                   const char* suffix) {
    const std::string stem = modelPath.stem().string();
    const std::filesystem::path dir = modelPath.parent_path();

    static constexpr std::string_view kLod0 = "_lod0";
    if (stem.size() > kLod0.size()) {
        const std::string tail = stem.substr(stem.size() - kLod0.size());
        // Case-insensitive: the corpus spells it "_lod0" but nothing guarantees
        // that for content from elsewhere.
        std::string lowered = tail;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == kLod0) {
            const std::filesystem::path base =
                dir / (stem.substr(0, stem.size() - kLod0.size()) + suffix);
            std::error_code ec;
            if (std::filesystem::is_regular_file(base, ec)) {
                return base;
            }
        }
    }

    return dir / (stem + suffix);
}

/// Loads a model of either family, chosen by the file's magic rather than its
/// extension — the same rule the map path uses.
[[nodiscard]] std::expected<rm::Model, rm::MapError> loadModel(
    const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::unexpected(
            rm::MapError{rm::MapError::Code::Truncated,
                         "could not open \"" + path.string() + "\""});
    }

    char magic[4] = {};
    file.read(magic, sizeof(magic));
    file.close();

    if (std::memcmp(magic, rm::scm::kMagic, sizeof(magic)) == 0) {
        return rm::scm::loadFile(path);
    }
    return rm::s3o::loadFile(path);
}

// Everything the renderer needs to draw units, owned in one place.
//
// models and instances are held in deques, NOT vectors: the batches point into
// them, and a vector that reallocates while later models load would leave every
// batch built so far dangling. A deque never moves what it already holds.
struct UnitScene {
    std::deque<rm::Model> models;
    std::deque<std::vector<rm::UnitInstance>> instances;
    std::deque<rm::sca::Animation> animations;
    std::vector<rm::UnitBatch> batches;
    TextureRegistry textures;
};

/// One `--units` argument: a model, how many of it, how big, and optionally an
/// animation to play on it.
struct UnitOptions {
    std::filesystem::path modelPath;
    std::size_t count = kDefaultUnitCount;
    float scale = 1.0f;
    std::filesystem::path animationPath;
};

/// Loads every requested model, resolves its textures, and places instances.
///
/// The first model takes the map's start positions and fills the rest of its
/// count by scatter; every later model is scattered alone. Spawn points read as
/// meaningful, and a second model landing on top of the first at every spawn
/// would not.
[[nodiscard]] UnitScene resolveUnits(std::span<const UnitOptions> requests,
                                     const rm::HeightField& field,
                                     std::span<const rm::mapinfo::StartPosition> starts,
                                     float landAbove) {
    UnitScene scene;

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const UnitOptions& request = requests[i];

        auto model = loadModel(request.modelPath);
        if (!model) {
            std::fprintf(stderr, "failed to load model \"%s\": %s\n",
                         request.modelPath.string().c_str(), model.error().message.c_str());
            continue;  // one bad model should not cost the whole scene
        }

        const bool supCom = model->family == rm::Family::SupremeCommander;
        std::printf("model %s (%s): %zu bones, %zu vertices, %zu triangles, radius %.1f\n",
                    model->name.c_str(), supCom ? "Supreme Commander" : "Recoil",
                    model->bones.size(), model->vertices.size(), model->triangleCount(),
                    static_cast<double>(model->radius));

        // Both textures, not just the diffuse. The two families disagree on
        // where they live and on what the second one's channels mean, and that
        // is all they disagree on — see Family in Model.hpp.
        // An animation is optional and, when present, must outlive the upload.
        // It lives in the scene's deque for the same reason the models do.
        const rm::sca::Animation* animation = nullptr;
        if (!request.animationPath.empty()) {
            auto loaded = rm::sca::loadFile(request.animationPath);
            if (!loaded) {
                std::fprintf(stderr, "  no animation (%s): %s\n",
                             request.animationPath.filename().string().c_str(),
                             loaded.error().message.c_str());
            } else {
                const auto map = rm::mapBonesToAnimation(*model, *loaded);
                const auto driven = static_cast<std::size_t>(
                    std::count_if(map.begin(), map.end(), [](int b) { return b >= 0; }));
                std::printf("  animation %s: %.2fs, %zu keyframes, %zu of %zu bones driven\n",
                            loaded->name.c_str(), static_cast<double>(loaded->duration),
                            loaded->frames.size(), driven, model->bones.size());
                scene.animations.push_back(std::move(*loaded));
                animation = &scene.animations.back();
            }
        }

        const rm::TexturePair pair =
            supCom ? rm::TexturePair{
                         .diffuse = scene.textures.resolve(
                             scmTexturePath(request.modelPath, kScmDiffuseSuffix), "albedo"),
                         .shading = scene.textures.resolve(
                             scmTexturePath(request.modelPath, kScmShadingSuffix), "specTeam"),
                     }
                   : rm::TexturePair{
                         .diffuse = scene.textures.resolve(barTexturePath(model->textures[0]),
                                                           "diffuse"),
                         .shading = scene.textures.resolve(barTexturePath(model->textures[1]),
                                                           "shading"),
                     };

        // A .scm's vertices are in ogrids, the same unit its maps use, so it
        // takes the same x8 the terrain does. Applying it to the instance rather
        // than to the vertices keeps the loader's output faithful to the file.
        const float scale = request.scale * (supCom ? kOgridScale : 1.0f);

        std::vector<rm::UnitInstance> placed;
        const bool takesStarts = (i == 0);
        if (takesStarts) {
            placed = rm::atStartPositions(field, starts, scale);
        }
        const std::size_t remaining =
            request.count > placed.size() ? request.count - placed.size() : 0;
        const auto scattered =
            rm::scatterOnLand(field, remaining, kScatterSeed + static_cast<std::uint32_t>(i),
                              scale, landAbove);
        placed.insert(placed.end(), scattered.begin(), scattered.end());

        std::printf("  %zu instances (%zu at start positions, %zu scattered)\n", placed.size(),
                    takesStarts ? starts.size() : 0u, scattered.size());

        scene.models.push_back(std::move(*model));
        scene.instances.push_back(std::move(placed));
        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = scene.instances.back(),
            .textures = pair,
            .animation = animation,
        });
    }

    if (scene.batches.size() > 1) {
        // Reports what the batching bought. The renderer orders the draws
        // itself; this recomputes the same thing purely to say it out loud,
        // which is the only way the saving is visible at all — a renderer that
        // rebound per model would produce an identical image.
        std::vector<rm::TexturePair> pairs;
        pairs.reserve(scene.batches.size());
        for (const rm::UnitBatch& batch : scene.batches) {
            pairs.push_back(batch.textures);
        }

        std::printf("scene: %zu models, %zu textures uploaded, %zu texture binds per frame\n",
                    scene.batches.size(), scene.textures.size(),
                    rm::textureBindCount(pairs, rm::orderByTexturePair(pairs)));
    }

    return scene;
}

/// Parsed --bench / --bench-offscreen arguments.
/// Parsed --units arguments.
/// Parsed --screenshot arguments.
struct ShotOptions {
    bool enabled = false;
    std::string path;
    unsigned int width = 1920;
    unsigned int height = 1080;
};

[[nodiscard]] ShotOptions parseShot(int argc, const char* argv[]) {
    ShotOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} != "--screenshot") {
            continue;
        }
        options.enabled = true;
        if (i + 1 < argc) {
            options.path = argv[i + 1];
        }
        if (i + 3 < argc) {
            const int w = std::atoi(argv[i + 2]);
            const int h = std::atoi(argv[i + 3]);
            if (w > 0 && h > 0) {
                options.width = static_cast<unsigned int>(w);
                options.height = static_cast<unsigned int>(h);
            }
        }
        break;
    }
    return options;
}

/// Every `--units <model> [count] [scale]` on the command line, in order.
///
/// Repeatable: one flag per model, which is how a scene with several models is
/// described. `--focus` is a scene-wide flag and is read separately, since it
/// may appear before, between or after the positional arguments.
[[nodiscard]] std::vector<UnitOptions> parseUnits(int argc, const char* argv[]) {
    std::vector<UnitOptions> requests;

    // ONE pass, because `--animate` attaches to the `--units` it follows and two
    // passes can disagree about which that is: a bare `--units` with no path is
    // dropped from the list but would still be counted while attaching, landing
    // the animation on the previous model.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--animate") {
            if (i + 1 < argc && !requests.empty()) {
                requests.back().animationPath = argv[i + 1];
            } else if (requests.empty()) {
                std::fprintf(stderr, "--animate before any --units; ignored\n");
            }
            continue;
        }

        if (arg != "--units") {
            continue;
        }

        UnitOptions options;
        if (i + 1 < argc) {
            options.modelPath = argv[i + 1];
        }
        // Positional arguments stop at the next flag, so `--units a.s3o --units
        // b.s3o` does not read "--units" as a count.
        if (i + 2 < argc && argv[i + 2][0] != '-') {
            const int parsed = std::atoi(argv[i + 2]);
            if (parsed > 0) {
                options.count = static_cast<std::size_t>(parsed);
            }
        }
        if (i + 3 < argc && argv[i + 3][0] != '-') {
            const double parsed = std::atof(argv[i + 3]);
            if (parsed > 0.0) {
                options.scale = static_cast<float>(parsed);
            }
        }

        if (options.modelPath.empty()) {
            std::fprintf(stderr, "--units with no model path; ignored\n");
            continue;
        }
        requests.push_back(std::move(options));
    }

    return requests;
}

/// `--time <seconds>`: where in their animations to freeze the units.
///
/// Only meaningful for a screenshot or a benchmark. The windowed app advances
/// its own clock, because an animation that needs a flag to move is not one
/// anybody would notice working.
[[nodiscard]] float parseAnimationTime(int argc, const char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string{argv[i]} == "--time") {
            return static_cast<float>(std::atof(argv[i + 1]));
        }
    }
    return 0.0f;
}

/// Whether `--focus` was given: frame the first instance instead of the map.
[[nodiscard]] bool parseFocus(int argc, const char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} == "--focus") {
            return true;
        }
    }
    return false;
}

/// Applies whichever ground the map brought, plus its water plane.
///
/// Templated on the target for the same reason focusOnFirstUnit is: Window and
/// Renderer both offer these and share no base class.
template <typename Target>
void applyGround(Target& target, const LoadedMap& map) {
    if (map.atlas) {
        target.setGroundTexture(*map.atlas);   // SMF: a baked BC1 atlas
    } else if (map.colours) {
        target.setGroundColourMap(*map.colours);  // .scmap: terrain-type bands
    }
    target.setWater(map.hasWater, map.waterLevel);
}

/// Points the camera at the first instance of the first model.
///
/// Framing the whole map makes a unit about two pixels across, which says
/// nothing about whether it is textured correctly. Templated on the target
/// because Window and Renderer both offer focusOn and neither shares a base
/// class — a one-method interface would be ceremony for two call sites.
template <typename Target>
void focusOnFirstUnit(Target& target, const UnitScene& scene) {
    if (scene.batches.empty() || scene.batches.front().instances.empty()
        || scene.batches.front().model == nullptr) {
        return;
    }

    /// How far back to sit, in model radii.
    constexpr float kRadiiBack = 2.5f;

    const rm::UnitBatch& batch = scene.batches.front();
    const rm::UnitInstance& first = batch.instances.front();
    target.focusOn(first.position,
                   std::max(batch.model->radius, 1.0f) * kRadiiBack * first.scale);
}

struct BenchOptions {
    bool enabled = false;
    bool offscreen = false;  ///< no window, no vsync
    std::size_t frames = 600;
    std::size_t warmup = 60;
    unsigned int width = 1920;
    unsigned int height = 1080;
    std::string csvPath;
};

/// Recognises `--bench <frames> <out.csv>` anywhere after the map path.
[[nodiscard]] BenchOptions parseBench(int argc, const char* argv[]) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg != "--bench" && arg != "--bench-offscreen") {
            continue;
        }
        options.enabled = true;
        options.offscreen = (arg == "--bench-offscreen");
        if (i + 1 < argc) {
            options.frames = static_cast<std::size_t>(std::max(1, std::atoi(argv[i + 1])));
        }
        if (i + 2 < argc) {
            options.csvPath = argv[i + 2];
        }
        break;
    }
    return options;
}

} // namespace

// Polls the renderer and quits once the benchmark has collected enough frames.
// A timer rather than a counter inside drawFrame: the display link owns the
// frame loop, and terminating from inside its callback is asking for trouble.
@interface RMBenchWatcher : NSObject
@property(nonatomic, assign) rm::Window* window;
@property(nonatomic, assign) NSUInteger targetFrames;
@property(nonatomic, copy) NSString* csvPath;
@end

@implementation RMBenchWatcher

- (void)tick:(NSTimer*)timer {
    if (self.window->recordedFrames() < self.targetFrames) {
        return;
    }
    [timer invalidate];

    const rm::bench::FrameRecorder recorder = self.window->benchmarkSnapshot();
    std::printf("%s\n", recorder.summaryLine("recoil-metal").c_str());
    std::printf("  note: cpu ms is display-paced by CAMetalDisplayLink; gpu ms is the\n"
                "        renderer's own cost and is the number worth comparing.\n");

    if (self.csvPath.length > 0) {
        const std::string csv = recorder.toCsv();
        std::ofstream out{self.csvPath.UTF8String, std::ios::binary};
        if (out) {
            out << csv;
            std::printf("  wrote %s (%zu frames)\n", self.csvPath.UTF8String,
                        recorder.recorded());
        } else {
            std::fprintf(stderr, "  could not write %s\n", self.csvPath.UTF8String);
        }
    }

    [NSApp terminate:nil];
}

@end

/// Writes BGRA8 pixels to a PNG via ImageIO — part of the OS, so no new
/// dependency. Reports rather than throwing on failure.
bool writePng(const std::string& path, const rm::Renderer::CapturedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.bgra.empty()) {
        std::fprintf(stderr, "nothing to write to %s\n", path.c_str());
        return false;
    }

    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, image.bgra.data(),
                                 static_cast<CFIndex>(image.bgra.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);

    // The drawable is BGRA8; premultiplied-first + little-endian is how
    // CoreGraphics spells that.
    CGImageRef cgImage = CGImageCreate(
        static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height), 8, 32,
        static_cast<std::size_t>(image.width) * 4, space,
        // Cast to the bitmap-info type before OR-ing: the two constants come from
        // different enums and mixing them directly is deprecated.
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst)
            | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little),
        provider, nullptr, false,
        kCGRenderingIntentDefault);

    bool ok = false;
    if (cgImage != nullptr) {
        CFStringRef cfPath = CFStringCreateWithCString(nullptr, path.c_str(),
                                                       kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfPath, kCFURLPOSIXPathStyle,
                                                     false);
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
        if (destination != nullptr) {
            CGImageDestinationAddImage(destination, cgImage, nullptr);
            ok = CGImageDestinationFinalize(destination);
            CFRelease(destination);
        }
        CFRelease(url);
        CFRelease(cfPath);
        CGImageRelease(cgImage);
    }

    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);

    if (ok) {
        std::printf("wrote %s (%dx%d)\n", path.c_str(), image.width, image.height);
    } else {
        std::fprintf(stderr, "failed to encode %s\n", path.c_str());
    }
    return ok;
}

/// Writes a recorder's per-frame CSV, reporting rather than throwing on failure.
void writeCsv(const std::string& path, const rm::bench::FrameRecorder& recorder) {
    if (path.empty()) {
        return;
    }
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "  could not write %s\n", path.c_str());
        return;
    }
    out << recorder.toCsv();
    std::printf("  wrote %s (%zu frames)\n", path.c_str(), recorder.recorded());
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const auto map = resolveMap(argc, argv);
        if (!map) {
            return 1;
        }

        const std::vector<UnitOptions> unitRequests = parseUnits(argc, argv);
        const bool focus = parseFocus(argc, argv);
        const float animationTime = parseAnimationTime(argc, argv);
        // Land above the water, whatever the map calls water: Recoil's plane is
        // always y = 0, Supreme Commander's is per map and 140 elmos on most.
        // Scattering above 0 on a FA map drowns most of the units.
        const UnitScene units = resolveUnits(unitRequests, map->field, map->starts,
                                             map->hasWater ? map->waterLevel : 0.0f);

        const rm::TerrainMesh mesh = rm::buildTerrainMesh(map->field);
        std::printf("terrain: %zu vertices, %zu triangles, height %.1f..%.1f elmos\n",
                    mesh.vertices.size(), mesh.triangleCount(),
                    static_cast<double>(mesh.minY), static_cast<double>(mesh.maxY));

        const BenchOptions bench = parseBench(argc, argv);

        // --- Headless offscreen benchmark ----------------------------------
        // No NSApplication, no window, no display link, hence no vsync. This is
        // the only mode whose CPU numbers describe the renderer instead of the
        // display, so it is the one comparable against another engine.
        if (bench.enabled && bench.offscreen) {
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            applyGround(renderer, *map);
            renderer.setUnits(units.textures.all(), units.batches);
            renderer.setAnimationTime(animationTime);

            std::printf("offscreen benchmark: %ux%u, %zu frames (discarding %zu warmup),"
                        " %zu frames in flight, no vsync\n",
                        bench.width, bench.height, bench.frames, bench.warmup,
                        rm::Renderer::kMaxFramesInFlight);

            const rm::bench::FrameRecorder recorder =
                renderer.runOffscreenBenchmark(bench.width, bench.height, bench.frames,
                                               bench.warmup);

            std::printf("%s\n", recorder.summaryLine("recoil-metal offscreen").c_str());
            writeCsv(bench.csvPath, recorder);
            return 0;
        }

        // --- Headless screenshot -------------------------------------------
        // No window, so this works regardless of which Space is active — the
        // reason it exists.
        const ShotOptions shot = parseShot(argc, argv);
        if (shot.enabled) {
            rm::Renderer renderer{nullptr};
            renderer.setTerrain(mesh);
            applyGround(renderer, *map);
            renderer.setUnits(units.textures.all(), units.batches);
            renderer.setAnimationTime(animationTime);
            if (focus) {
                focusOnFirstUnit(renderer, units);
            }

            const auto image = renderer.renderToImage(shot.width, shot.height);
            return writePng(shot.path, image) ? 0 : 1;
        }

        NSApplication* app = [NSApplication sharedApplication];
        // Regular = real Dock icon and keyboard focus; without this a
        // terminal-launched app is a background agent that can't take focus.
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app setDelegate:[[RMAppDelegate alloc] init]];
        [app finishLaunching];

        // 1280x720 points: comfortable debug size on a laptop screen.
        rm::Window window{1280, 720, "recoil-metal — m5: units on SMF terrain"};
        window.setTerrain(mesh);
        applyGround(window, *map);
        window.setUnits(units.textures.all(), units.batches);
        if (focus) {
            focusOnFirstUnit(window, units);
        }
        window.show();

        RMBenchWatcher* watcher = nil;
        if (bench.enabled) {
            std::printf("benchmarking %zu frames (discarding %zu warmup)\n",
                        bench.frames, bench.warmup);
            window.beginBenchmark(bench.warmup);

            watcher = [[RMBenchWatcher alloc] init];
            watcher.window = &window;
            watcher.targetFrames = bench.frames;
            watcher.csvPath = bench.csvPath.empty()
                                  ? @""
                                  : [NSString stringWithUTF8String:bench.csvPath.c_str()];
            [NSTimer scheduledTimerWithTimeInterval:0.1
                                             target:watcher
                                           selector:@selector(tick:)
                                           userInfo:nil
                                            repeats:YES];
        }

        [app activateIgnoringOtherApps:YES];
        [app run]; // never returns until the app quits
    }
    return 0;
}
