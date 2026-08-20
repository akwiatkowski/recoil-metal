#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "core/map/MapInfo.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/map/Scmap.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/map/TerrainType.hpp"
#include "core/model/S3o.hpp"
#include "core/model/Sca.hpp"
#include "core/model/Scm.hpp"
#include "core/scene/Picking.hpp"
#include "core/map/PropBlueprint.hpp"
#include "core/scene/Particles.hpp"
#include "core/scene/PropBatch.hpp"
#include "core/scene/Selection.hpp"
#include "core/scene/UnitIcons.hpp"
#include "core/text/TextLayout.hpp"
#include "core/ui/Hud.hpp"

#include <cassert>
#include "core/settings/Settings.hpp"
#include "core/scene/GroundDecals.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Army.hpp"
#include "core/sim/BuildOrder.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"
#include "core/vfs/AssetSearch.hpp"
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

    // .scmap: the ground splat — nine tiled layers plus the two masks that
    // weight them. Empty when the layer textures are not on disk, in which case
    // `colours` above still carries the terrain-type placeholder.
    std::vector<rm::SplatLayer> splat;
    rm::dds::Texture splatMaskA;
    rm::dds::Texture splatMaskB;

    std::vector<rm::mapinfo::StartPosition> starts;

    /// Everything the map annotates itself with — 19310 markers across the 60 stock maps.
    /// The mass deposits are what an economy stands on; the rest is read because it is
    /// nearly free once the table is being walked at all.
    std::vector<rm::scenario::Marker> markers;

    // .scmap: the props the map places — trees, rocks, wrecks. Kept as the map
    // stated them; resolving each blueprint to a mesh is a separate step, because
    // it reads files the map only names.
    std::vector<rm::scmap::Prop> props;

    // Recoil's water is a fixed plane at y = 0 that every map shares; Supreme
    // Commander stores a level per map, and 17 of the 60 stock maps are dry.
    bool hasWater = true;
    float waterLevel = 0.0f;

    // The map's own sky and water settings. Empty for an SMF map, which carries
    // no such block — the renderer then keeps the engine's shader defaults.
    std::optional<rm::Renderer::Environment> environment;
};

// Where Supreme Commander's ground layer textures live when they have been
// extracted rather than mounted.
//
// A `.scmap` names its strata as game-relative paths ("/env/Evergreen/Layers/...")
// into env.scd, and the FIRST answer to that was to extract the 184 textures the
// stock maps reference and read them off a disk. Mounting the archive is the right
// answer and is what `--gamedata` does now; this remains the fallback for a command
// line that names no content, so the extraction the README documents keeps working.
constexpr const char* kFaEnvDir = "projects/llm/input/faf";

/// The decoded ground splat: layers in blend order, plus the two weight masks.
struct LoadedSplat {
    std::vector<rm::SplatLayer> layers;
    rm::dds::Texture maskA;
    rm::dds::Texture maskB;
};

/// Resolves a .scmap's stratum table into uploaded-ready textures.
///
/// Returns nullopt when the base layer or either mask is missing — a splat
/// without those would render as something plausible rather than as something
/// absent, and the terrain-type placeholder is the honest fallback.
[[nodiscard]] std::optional<LoadedSplat> resolveSplat(const rm::scmap::Map& map,
                                                     const rm::vfs::Vfs& content) {
    const char* home = std::getenv("HOME");
    const std::filesystem::path root =
        std::filesystem::path{home == nullptr ? "" : home} / kFaEnvDir;

    LoadedSplat splat;

    // Slots 0..8 are the base plus the eight strata, in the order the masks
    // weight them; slot 9 is the macrotexture, laid over everything and keyed
    // on its own alpha rather than on a mask channel.
    // Paths are absolute within the game's virtual filesystem, so the leading
    // slash has to go before joining, or the concatenation resolves to the real
    // filesystem root.
    // The MOUNTED CONTENT first, since a `.scmap`'s stratum path is a VFS path and
    // the VFS is the thing that speaks that language. Falling back to an extracted
    // tree keeps every command line that predates `--gamedata` working.
    const auto resolve = [&root, &content](const rm::scmap::TextureRef& ref, const char* what,
                                           std::size_t slot) -> std::optional<rm::dds::Texture> {
        if (ref.empty()) {
            return std::nullopt;
        }

        if (const auto bytes = content.read(ref.path)) {
            auto texture = rm::dds::load(*bytes);
            if (texture) {
                return std::move(*texture);
            }
            std::fprintf(stderr, "  splat %s %zu (%s) will not decode: %s\n", what, slot,
                         ref.path.c_str(), texture.error().message.c_str());
            return std::nullopt;
        }

        std::string relative = ref.path;
        if (!relative.empty() && relative.front() == '/') {
            relative.erase(0, 1);
        }

        auto texture = rm::dds::loadFile(root / relative);
        if (!texture) {
            std::fprintf(stderr, "  splat %s %zu (%s) unavailable: %s\n", what, slot,
                         ref.path.c_str(), texture.error().message.c_str());
            return std::nullopt;
        }
        return std::move(*texture);
    };

    for (std::size_t i = 0; i < rm::kSplatLayers; ++i) {
        rm::SplatLayer layer;

        const rm::scmap::TextureRef& albedo = map.albedo[i];
        if (auto texture = resolve(albedo, "layer", i)) {
            layer.texture = std::move(*texture);
            // The file stores ogrids; the renderer works in elmos.
            layer.tileElmos = albedo.scale * rm::scmap::kElmosPerOgrid;
        }

        // The macrotexture has no normal entry — nine normals against ten
        // albedos — so the loop stops one short of the array rather than
        // indexing past it.
        if (i < rm::kSplatNormalLayers) {
            const rm::scmap::TextureRef& normal = map.normals[i];
            if (auto texture = resolve(normal, "normal", i)) {
                layer.normal = std::move(*texture);
                layer.normalTileElmos = normal.scale * rm::scmap::kElmosPerOgrid;
            }
        }

        splat.layers.push_back(std::move(layer));
    }

    if (!splat.layers.front().present()) {
        return std::nullopt;
    }

    const auto maskA = rm::dds::load(map.maskA);
    const auto maskB = rm::dds::load(map.maskB);
    if (!maskA || !maskB) {
        std::fprintf(stderr, "  splat masks did not decode; falling back to terrain-type "
                             "colouring\n");
        return std::nullopt;
    }
    splat.maskA = *maskA;
    splat.maskB = *maskB;

    std::size_t used = 0;
    std::size_t normals = 0;
    for (const rm::SplatLayer& layer : splat.layers) {
        used += layer.present() ? 1 : 0;
        normals += layer.hasNormal() ? 1 : 0;
    }
    std::printf("  splat: %zu of %zu layers, %zu normal maps, masks %dx%d\n", used,
                splat.layers.size(), normals, splat.maskA.width, splat.maskA.height);

    return splat;
}

/// Loads a Supreme Commander `.scmap`.
///
/// No mapinfo.lua equivalent and no start positions: those live in the map's
/// `_scenario.lua`, which is a later milestone. Units therefore scatter rather
/// than spawning at bases on these maps.
[[nodiscard]] std::optional<LoadedMap> resolveScmap(const std::string& path,
                                                    const rm::vfs::Vfs& content) {
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

    // The map states its own horizon and sea, so use them rather than the
    // shader's stock values — that is the difference between every map looking
    // the same and each looking like itself.
    rm::Renderer::Environment environment;
    environment.fogColour = map->lighting.fogColour;
    environment.waterSurfaceColour = map->water.surfaceColour;
    environment.waterSunColour = map->water.sunColour;
    // The engine clamps the water depth into [min, max] and the retail maps set
    // both to the same value, which makes it a constant rather than a ramp.
    environment.waterColourLerp = map->water.colourLerp[0];
    environment.waterFresnelBias = map->water.fresnelBias;
    environment.waterFresnelPower = map->water.fresnelPower;
    environment.waterSkyReflection = map->water.skyReflection;
    environment.waterSunShininess = map->water.sunShininess;
    environment.waterRefractionScale = map->water.refractionScale;
    // The map's own cirrus colour, tinting the high sky. See Renderer::Environment
    // for why this rather than the skybox block's own mid colour, which is black on
    // every stock map.
    environment.skyZenithTint = map->sky.cirrusColour;
    loaded.environment = environment;

    std::printf("  sky/water: fog (%.2f %.2f %.2f), surface (%.2f %.2f %.2f),"
                " fresnel %.2f^%.2f\n",
                static_cast<double>(environment.fogColour[0]),
                static_cast<double>(environment.fogColour[1]),
                static_cast<double>(environment.fogColour[2]),
                static_cast<double>(environment.waterSurfaceColour[0]),
                static_cast<double>(environment.waterSurfaceColour[1]),
                static_cast<double>(environment.waterSurfaceColour[2]),
                static_cast<double>(environment.waterFresnelBias),
                static_cast<double>(environment.waterFresnelPower));
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

    // The ground splat. SupCom bakes no ground image: the map names nine tiled
    // layers that live in env.scd and embeds only the two masks that weight
    // them, so this assembles a recipe rather than loading a picture.
    if (auto splat = resolveSplat(*map, content)) {
        loaded.splat = std::move(splat->layers);
        loaded.splatMaskA = std::move(splat->maskA);
        loaded.splatMaskB = std::move(splat->maskB);
    }

    // Start positions, from the _save.lua beside the map. The .scmap itself
    // carries none — unlike .smf, whose mapinfo.lua holds them — so this is the
    // only source. Campaign maps legitimately have none at all, which leaves
    // `starts` empty and falls back to the scatter, exactly as before.
    if (const auto savePath = rm::scenario::findSaveBesideMap(path)) {
        const auto starts = rm::scenario::loadStartPositionsFile(*savePath);
        if (starts) {
            loaded.starts = *starts;

            std::size_t massSites = 0;
            std::ifstream save{*savePath, std::ios::binary};
            const std::string lua{std::istreambuf_iterator<char>(save),
                                  std::istreambuf_iterator<char>()};
            if (auto markers = rm::scenario::loadMarkers(lua)) {
                loaded.markers = std::move(*markers);
                for (const rm::scenario::Marker& marker : loaded.markers) {
                    if (marker.isType("Mass")) {
                        ++massSites;
                    }
                }
            }

            std::printf("  starts: %zu, markers: %zu (%zu mass), from %s\n",
                        loaded.starts.size(), loaded.markers.size(), massSites,
                        savePath->filename().string().c_str());
        } else {
            // Worth a word: a map whose starts fail to read still draws, but
            // its units land in a scatter and it is not obvious why.
            std::fprintf(stderr, "  warning: could not read start positions from %s: %s\n",
                         savePath->filename().string().c_str(),
                         starts.error().message.c_str());
        }
    }

    loaded.props = std::move(map->props);
    if (!loaded.props.empty()) {
        std::printf("  props: %zu placed\n", loaded.props.size());
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
[[nodiscard]] std::optional<LoadedMap> resolveMap(int argc, const char* argv[],
                                                 const rm::vfs::Vfs& content) {
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
        return resolveScmap(path, content);
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

// How long a WASD pan takes to cross the visible width, in seconds.
//
// One and a half, which is brisk without overshooting: an RTS camera is used in short
// corrections, and a pan that takes four seconds to cross the screen gets held down and
// then overshot. Expressed as a duration rather than a speed so it means the same thing at
// every zoom level.
constexpr float kSecondsToCrossTheView = 1.5f;

// The window height, in points, that the pan step is measured against.
//
// `elmosPerPoint` needs a viewport height and the frame callback does not have one; the
// value only has to be CONSISTENT, since it is multiplied straight back out to recover the
// frustum width. 1000 points is a plausible window and cancels exactly.
constexpr float kPanReferenceHeightPoints = 1000.0f;

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

    /// The same, for a texture inside the mounted content.
    ///
    /// Keyed on the VFS path, which cannot collide with a filesystem key: one begins
    /// with a slash and names the game's namespace, the other names this disk. A
    /// texture reached both ways would upload twice, which is a waste rather than a
    /// bug, and no scene reaches one both ways.
    [[nodiscard]] int resolve(const rm::vfs::Vfs& content, const std::string& vfsPath,
                              const char* slot) {
        if (vfsPath.empty()) {
            return -1;
        }
        const auto known = indexByPath_.find(vfsPath);
        if (known != indexByPath_.end()) {
            return known->second;
        }

        const auto bytes = content.read(vfsPath);
        if (!bytes) {
            std::fprintf(stderr, "  no %s texture (%s): not in the mounted content\n", slot,
                         vfsPath.c_str());
            indexByPath_.emplace(vfsPath, -1);
            return -1;
        }

        auto texture = rm::dds::load(*bytes);
        if (!texture) {
            std::fprintf(stderr, "  no %s texture (%s): %s\n", slot, vfsPath.c_str(),
                         texture.error().message.c_str());
            indexByPath_.emplace(vfsPath, -1);
            return -1;
        }

        std::printf("  %s %s: %dx%d, %d mips\n", slot,
                    std::filesystem::path{vfsPath}.filename().string().c_str(), texture->width,
                    texture->height, texture->mipLevels);

        const auto index = static_cast<int>(textures_.size());
        textures_.push_back(std::move(*texture));
        indexByPath_.emplace(vfsPath, index);
        return index;
    }

    [[nodiscard]] std::span<const rm::dds::Texture> all() const noexcept { return textures_; }
    [[nodiscard]] std::size_t size() const noexcept { return textures_.size(); }

private:
    std::vector<rm::dds::Texture> textures_;
    std::map<std::string, int> indexByPath_;
};

// Everything the map's scenery contributes to the scene: one mesh per distinct
// blueprint, and the instances to draw each at.
//
// Held in deques for the same reason the units are: PropBatch keeps a pointer to
// the model and a span over the instances, so neither may be reallocated once a
// batch has been built.
struct PropScene {
    std::deque<rm::Model> models;
    std::deque<std::vector<rm::UnitInstance>> instances;
    std::deque<std::vector<rm::PropLevel>> levels;
    std::vector<rm::PropBatch> batches;
    TextureRegistry textures;

    /// Where the map's ambient effects happen — the props that draw nothing and
    /// mark a place for steam, mist, bubbles, sand or snow instead.
    std::vector<rm::AmbientEmitter> ambient;
};

/// The yaw of a prop's stored rotation basis.
///
/// The basis is 3x3 and the instance format carries Euler angles, so something
/// has to give. 98.4% of the 418 942 props in the stock corpus are rotations
/// about +Y alone — the map editor spins a tree, it does not tip one over — so
/// yaw is very nearly the whole of it. The 1.6% that are genuinely tilted are
/// tree GROUPS, and losing their tilt leaves a clump of trees standing straight
/// instead of leaning; the alternative, decomposing an arbitrary basis into the
/// shader's roll-pitch-yaw order, is real work for a sixth of one percent of the
/// scenery leaning the wrong way.
///
/// HANDEDNESS IS NOT VERIFIED, and this comment is the honest place to say so.
/// The file states a basis and nothing states whether its columns or its rows are
/// the axes, and the corpus cannot settle it: getting it backwards mirrors each
/// prop about its own vertical axis, and a mirrored pine is a pine. Should a
/// distinctive asymmetric prop ever turn up, this is the line to check.
[[nodiscard]] float propYaw(const rm::scmap::Prop& prop) noexcept {
    return std::atan2(prop.rotationX[2], prop.rotationX[0]);
}

/// Loads every prop mesh a map names, and the instances to draw them at.
///
/// Grouped by blueprint, so a map placing 14 000 pines costs one mesh, one
/// texture and one instanced draw. That grouping is the whole reason this is
/// affordable: the busiest stock map places 46 971 props, and one draw each would
/// be more draw calls than everything else in the frame put together.
[[nodiscard]] PropScene loadProps(const LoadedMap& map, const std::filesystem::path& root,
                                  const rm::vfs::Vfs& content) {
    PropScene scene;
    if (map.props.empty()) {
        return scene;
    }

    // Blueprint -> the instances placed with it, in first-seen order so that a
    // screenshot does not depend on how a hash table happened to bucket.
    std::map<std::string, std::vector<rm::UnitInstance>> byBlueprint;
    for (const rm::scmap::Prop& prop : map.props) {
        byBlueprint[prop.blueprint].push_back(rm::UnitInstance{
            .position = prop.position,
            .rotationY = propYaw(prop),
            // The MAP's scale, which is 1.0 for every prop in the retail corpus —
            // the size that matters comes from the blueprint and is applied below,
            // multiplied rather than substituted so a map that does set one is
            // still honoured.
            .scale = prop.scale[0],
        });
    }

    std::size_t emitters = 0;
    std::size_t effects = 0;
    std::size_t unreadable = 0;
    std::size_t drawn = 0;
    std::size_t levelCount = 0;

    for (auto& [blueprint, instances] : byBlueprint) {
        // The MOUNTED CONTENT first, and the extracted tree only when nothing is
        // mounted — the same order the splat uses, and for the same reason: a
        // blueprint path is a VFS path, so the VFS is what natively speaks it.
        const bool fromContent = content.contains(blueprint);
        const auto info = fromContent ? rm::prop::loadFromContent(content, blueprint)
                                      : rm::prop::loadFile(root, blueprint);
        if (!info) {
            if (info.error().code == rm::MapError::Code::MissingMesh) {
                ++emitters;
            } else {
                ++unreadable;
                std::fprintf(stderr, "  prop %s: %s\n", blueprint.c_str(),
                             info.error().message.c_str());
            }
            continue;
        }

        // A blueprint that draws nothing marks an ambient effect instead — steam
        // over lava, mist on water, bubbles under it. Every placement becomes an
        // emitter; the appearance is ours (Particles.cpp), since the map states only
        // which effect and where.
        if (info->effect != rm::prop::Effect::None) {
            for (const rm::UnitInstance& instance : instances) {
                scene.ambient.push_back(rm::AmbientEmitter{
                    .position = instance.position,
                    .effect = info->effect,
                });
            }
            ++effects;
            continue;
        }

        // Every level the blueprint declares, finest first. Which one an instance is
        // drawn at is decided per frame from the camera, so all of them are loaded.
        std::vector<rm::PropLevel> levels;
        for (const rm::prop::BlueprintLod& lod : info->lods) {
            // A path from `loadFromContent` is a VFS path and must be read back
            // through the VFS; one from `loadFile` is a real file. The blueprint
            // reader that produced it is what says which.
            auto model = [&] {
                if (!fromContent) {
                    return rm::scm::loadFile(lod.mesh);
                }
                const auto bytes = content.read(lod.mesh.generic_string());
                if (!bytes) {
                    return std::expected<rm::Model, rm::MapError>{std::unexpect,
                        rm::MapError{rm::MapError::Code::Truncated,
                                     "not in the mounted content"}};
                }
                return rm::scm::load(*bytes);
            }();
            if (!model) {
                std::fprintf(stderr, "  prop mesh %s: %s\n",
                             lod.mesh.filename().string().c_str(),
                             model.error().message.c_str());
                break;  // and the finer levels already loaded still stand
            }
            scene.models.push_back(std::move(*model));
            levels.push_back(rm::PropLevel{
                .model = &scene.models.back(),
                .albedo = fromContent ? scene.textures.resolve(content,
                                                               lod.albedo.generic_string(),
                                                               "prop albedo")
                                      : scene.textures.resolve(lod.albedo, "prop albedo"),
                .normals = fromContent ? scene.textures.resolve(content,
                                                                lod.normals.generic_string(),
                                                                "prop normals")
                                       : scene.textures.resolve(lod.normals, "prop normals"),
                // The blueprint's own cutoff: how far out this level is the right
                // one, and for the coarsest, how far out the prop is drawn at all.
                .cutoffElmos = lod.cutoffElmos,
            });
        }

        if (levels.empty()) {
            ++unreadable;
            continue;
        }

        // Two conversions, and both are needed.
        //
        // UniformScale takes the mesh to OGRIDS — Supreme Commander's world unit,
        // not this renderer's. Cross-checked against the blueprints themselves:
        // the palm and the pine both declare SizeY = 1, the collision height in
        // ogrids, and their meshes measure 0.96 and 1.18 once scaled. Then an
        // ogrid is 8 elmos, the same factor the map's prop POSITIONS take when
        // they are read (Scmap.cpp), so the two agree about the world they land
        // in. Applying only the first leaves a pine 1.2 elmos tall — visible as a
        // scatter of dark specks, which reads as a texture problem rather than a
        // units one.
        for (rm::UnitInstance& instance : instances) {
            instance.scale *= info->uniformScale * rm::scmap::kElmosPerOgrid;
        }

        levelCount += levels.size();
        scene.instances.push_back(std::move(instances));
        scene.levels.push_back(std::move(levels));
        scene.batches.push_back(rm::PropBatch{
            .levels = scene.levels.back(),
            .instances = scene.instances.back(),
        });
        drawn += scene.instances.back().size();
    }

    // The draw distances, since they decide what a zoomed-out frame costs. Reported
    // as a range because they are graded per prop rather than one number.
    float nearest = std::numeric_limits<float>::infinity();
    float furthest = 0.0f;
    for (const rm::PropBatch& batch : scene.batches) {
        const float distance = batch.levels.back().cutoffElmos;
        nearest = std::min(nearest, distance);
        furthest = std::max(furthest, distance);
    }

    std::printf("  props: %zu blueprints, %zu meshes, %zu instances, %zu textures",
                scene.batches.size(), levelCount, drawn, scene.textures.size());
    if (!scene.batches.empty()) {
        std::printf(", drawn within %.0f..%.0f elmos", static_cast<double>(nearest),
                    static_cast<double>(furthest));
    }
    if (effects > 0) {
        std::printf(", %zu ambient effects at %zu places", effects, scene.ambient.size());
    }
    if (emitters > 0) {
        std::printf(", %zu unrecognised markers skipped", emitters);
    }
    if (unreadable > 0) {
        std::printf(", %zu unreadable", unreadable);
    }
    std::printf("\n");
    return scene;
}

/// The quality switches in force, for a benchmark to state alongside its number.
///
/// Not decoration. The three switches are worth 0.21, 0.50 and up to 6.2 ms of a
/// frame between them, so two figures from this renderer are not comparable
/// unless both say what was on — and the whole point of the benchmark harness is
/// comparing figures, against Recoil and against this renderer's own past.
[[nodiscard]] std::string describeQuality(const rm::Settings& settings, std::size_t propCount,
                                          std::size_t particleCount) {
    std::string text = "quality: reflections ";
    text += settings.reflections ? "on" : "off";
    text += ", stratum normals ";
    text += settings.stratumNormals ? "on" : "off";
    text += ", refraction ";
    text += settings.refraction ? "on" : "off";
    text += ", particles ";
    text += std::to_string(particleCount);
    text += ", props ";
    if (!settings.props) {
        text += "off";
    } else {
        text += std::to_string(propCount);
    }
    return text;
}

/// Where a Recoil model's named texture lives, resolved against the search roots.
[[nodiscard]] std::filesystem::path barTexturePath(const rm::vfs::AssetSearch& search,
                                                   const std::string& name) {
    if (name.empty()) {
        return {};
    }
    const std::filesystem::path relative = std::filesystem::path{"unittextures"} / name;
    std::filesystem::path found = search.resolve(relative);
    if (!found.empty()) {
        return found;
    }
    // Fallback to the historical hard-coded BAR reference layout for backwards
    // compatibility with existing command lines.
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

/// The same sniff, on bytes already read — the form the VFS path uses.
///
/// Both loaders have taken a byte span since they were written, which is why
/// content arriving from an archive instead of a disk costs one function here and
/// nothing at all in `core/`.
[[nodiscard]] std::expected<rm::Model, rm::MapError> loadModelBytes(
    std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(rm::scm::kMagic)) {
        return std::unexpected(
            rm::MapError{rm::MapError::Code::Truncated, "model is too short to identify"});
    }
    if (std::memcmp(bytes.data(), rm::scm::kMagic, sizeof(rm::scm::kMagic)) == 0) {
        return rm::scm::load(bytes);
    }
    return rm::s3o::load(bytes);
}

/// A float world position as the fixed-point triple the geometry functions take.
///
/// The other half of the boundary: map markers, mouse picks and construction sites are all
/// authored or produced in floats, and this is where they become sim values. Named rather than
/// written out, because a three-element brace list of conversions at a dozen call sites is
/// where a transposed axis hides.
[[nodiscard]] std::array<rm::sim::Fx, 3> fxPoint(const std::array<float, 3>& position) {
    return {rm::sim::fxFromFloat(position[0]), rm::sim::fxFromFloat(position[1]),
            rm::sim::fxFromFloat(position[2])};
}

/// A `Transform` from the float position and yaw a placement helper produced.
///
/// The boundary between the placement code — which works in floats because it reads the map
/// and the blueprints — and the store, which is fixed point. One function so the conversion
/// reads the same at every spawn site.
[[nodiscard]] rm::sim::Transform transformAt(const std::array<float, 3>& position,
                                             float yaw) {
    rm::sim::Transform transform;
    transform.x = rm::sim::fxFromFloat(position[0]);
    transform.y = rm::sim::fxFromFloat(position[1]);
    transform.z = rm::sim::fxFromFloat(position[2]);
    transform.heading = rm::sim::bradFromRadians(yaw);
    return transform;
}

/// The rate this build of the app runs its sim at.
///
/// A constant HERE, in the app, rather than in the sim: `core/sim` may not hold file-scope
/// mutable state (PLAN2 §5.4) and does not hold this at all — a `TickRate` is a value passed
/// to the passes that need it. What is missing is the configuration that would set it, which
/// is the rest of P2.3; until then the app has one rate and this is where it is written down,
/// once, instead of at every site that converts a content rate.
inline const rm::sim::TickRate kAppTickRate{rm::sim::kDefaultTicksPerSecond};

// Everything the renderer needs to draw units, owned in one place.
//
// models and instances are held in deques, NOT vectors: the batches point into
// them, and a vector that reallocates while later models load would leave every
// batch built so far dangling. A deque never moves what it already holds.
struct UnitScene {
    std::deque<rm::Model> models;
    std::deque<rm::sca::Animation> animations;
    std::vector<rm::UnitBatch> batches;
    TextureRegistry textures;

    // EVERY UNIT, flat, addressed by handle. Was three parallel deques of per-batch
    // vectors — `instances[batch][i]`, `motion[batch][i]`, `health[batch][i]` — which made
    // a unit's identity its position in a draw call (PLAN2.md §1.1).
    rm::sim::UnitStore store;

    // What each unit TYPE is. A batch is exactly one unit type, so a type index and a batch
    // index are THE SAME NUMBER and deliberately so: it keeps the render batching and the
    // sim's type numbering in step without a mapping table, and `batches[type]` is how a
    // unit reaches its model.
    rm::sim::UnitCatalog catalog;

    // Per TYPE now rather than per batch — the same numbers, since the two indices coincide.
    // Used to pick which passability grid routes a unit: a slope one climbs is a wall to
    // another.
    std::vector<float> maxSlopeDegrees;
    std::vector<float> maxWaterDepthElmos;

    /// How much to scale each type's mesh by, from its blueprint's `meshToElmos`.
    ///
    /// Per TYPE, and here rather than in the store, because a scale is presentation: the sim
    /// has a collision radius and does not care how big the model that represents it is. It
    /// moved out of `UnitInstance` when the store stopped holding one (P2.2), and this is
    /// where the draw projection reads it.
    std::vector<float> typeScale;

    // Scratch for drawing: one contiguous instance array per batch, refilled from the store
    // each frame. The store is flat and the GPU wants a run per model, so somebody has to
    // gather — this is P7's snapshot in embryo, arriving here because the renderer's upload
    // has to stay a memcpy.
    std::vector<std::vector<rm::UnitInstance>> drawScratch;

    // Where each live unit ended up in `drawScratch`, by slot: which batch, and which index
    // within it. Empty for a slot that is dead or was never filled. This is what turns a
    // handle back into something the renderer can outline.
    std::vector<rm::SelectionEntry> drawIndexOf;

    // The reverse: which slot each drawn instance came from, parallel to `drawScratch`.
    // Picking happens against what is DRAWN — that is what the ray can see — and this is
    // what turns the hit back into a unit the sim knows about.
    std::vector<std::vector<rm::UnitIndex>> drawSlotOf;

    // The sides in the match, empty outside a skirmish. Held with the scene rather
    // than beside it because every question that needs an army — may I select this,
    // may I shoot that, who banks the mass — starts from a unit.
    std::vector<rm::sim::Army> armies;

    /// The army the mouse belongs to. Only its units may be selected.
    int playerArmy = rm::sim::kNoArmy;

    /// The definitions themselves, owned here so the catalog's pointers stay valid.
    std::deque<rm::unitdef::UnitDef> definitions;

    /// Shots in flight.
    std::vector<rm::sim::Projectile> projectiles;

    /// One economy per army, indexed by army. Empty outside a skirmish.
    std::vector<rm::sim::Economy> economies;

    /// Everything under construction, all armies together. Partitioned per army each tick
    /// rather than held per army, because a build is a thing in the world and belongs with
    /// the others rather than filed under its owner.
    std::vector<rm::sim::Construction> building;

    /// The blueprint each Construction becomes, by its `blueprintIndex`.
    std::vector<rm::unitdef::UnitDef> buildable;

    /// The VFS path behind each `buildable` entry, parallel to it. What a FINISHED
    /// construction spawns from: the def alone cannot resolve a model, and milestone
    /// 20 is where a finished build becomes a unit on the map rather than a number
    /// in the economy.
    std::vector<std::string> buildablePaths;

    /// The batch each spawned blueprint reuses, so twenty tanks are one batch and
    /// one draw call rather than twenty. Keyed by VFS path, same as the cache in
    /// spawnCommanders is keyed by faction.
    std::map<std::string, std::size_t, std::less<>> batchForBlueprint;

    /// Set when a spawn added instances mid-simulation, so the tick loop knows its
    /// collision spans point at moved storage and rebuilds them.
    bool grewThisTick = false;

    /// The scorch marks the dead have left, in decal vertices ready to upload.
    ///
    /// Accumulated rather than rebuilt, unlike the selection rings: a wreck is permanent, so
    /// there is nothing to recompute each frame and a growing buffer is the honest shape.
    std::vector<rm::DecalVertex> wreckDecals;

    /// Death explosions set off, and the damage they dealt. BOTH, because they answer
    /// different questions: a blast that goes off and hurts nothing is the ordinary case when
    /// two commanders kill each other in the same tick, and reporting only the damage would
    /// make that look like the explosions never happened.
    std::size_t deathBlasts = 0;
    rm::sim::Mag deathBlastDamage{};

    /// How many commanders each army STARTED with, indexed by army. The win condition
    /// needs it to tell "lost its commander" from "never had one" — a `--units` crowd must
    /// not be declared a draw on the first tick.
    std::vector<int> commandersEver;

    /// Living commanders per army, recounted each tick.
    ///
    /// Delegates now: the sim owns the definition of "a living commander of army N", and this
    /// used to be a second copy of it walking a different layout. `UnitCensus` (P1.3) makes
    /// the same answer O(1), and wiring it here is P3's job — the scan is still cheap next to
    /// a tick, and swapping it now would be a second change riding on this one.
    [[nodiscard]] std::vector<int> countCommanders() const {
        return rm::sim::countCommanders(store, catalog, armies.size());
    }

    /// Refills `drawScratch` and `drawIndexOf` from the store.
    ///
    /// One contiguous run per batch, because that is what the GPU is uploaded: the store is
    /// flat and a draw call wants one model's instances together. Dead units are left out —
    /// a corpse's scale is already zero so drawing it costs a collapsed mesh, but leaving it
    /// out costs nothing at all.
    ///
    /// The scratch vectors are cleared rather than freed, so a steady-state frame does no
    /// allocation after the first few.
    ///
    /// It also re-points each batch's `instances` span at its scratch vector, which is not
    /// tidiness — it is a correctness requirement with a sharp edge. `Renderer::setUnits`
    /// takes each batch's instance capacity from `batch.instances.size()`, and a batch built
    /// mid-match starts with an empty span. Before the flat store, `spawnUnit` re-pointed the
    /// span on every spawn, so a new batch was never empty by the time `setUnits` saw it;
    /// there is no per-batch vector to re-point any more. Doing it here means the invariant
    /// holds wherever the gather is called, rather than at four call sites that must
    /// remember. (The span also goes stale on its own: a scratch vector that grows past its
    /// capacity reallocates.)
    void gatherForDrawing() {
        drawScratch.resize(batches.size());
        drawSlotOf.resize(batches.size());
        for (std::vector<rm::UnitInstance>& batch : drawScratch) {
            batch.clear();
        }
        for (std::vector<rm::UnitIndex>& batch : drawSlotOf) {
            batch.clear();
        }
        drawIndexOf.assign(store.slotCount(), rm::SelectionEntry{});

        const std::span<const rm::sim::Transform> transforms = store.transforms();
        const std::span<const rm::sim::MoveState> motion = store.motion();

        for (rm::UnitIndex slot = 0; slot < store.slotCount(); ++slot) {
            if (!store.slotAlive(slot)) {
                continue;
            }
            const auto batch = static_cast<std::size_t>(store.typeAt(slot));
            if (batch >= drawScratch.size()) {
                continue;  // a type with no batch: nothing to draw it with
            }
            drawIndexOf[slot] =
                rm::SelectionEntry{.batch = batch, .instance = drawScratch[batch].size()};
            drawScratch[batch].push_back(instanceFor(slot, transforms[slot], motion[slot]));
            drawSlotOf[batch].push_back(slot);
        }

        for (std::size_t batch = 0; batch < batches.size(); ++batch) {
            batches[batch].instances = drawScratch[batch];
        }
    }

    /// Builds the GPU's view of one unit from the sim's.
    ///
    /// THE PROJECTION, and the reason `UnitInstance` is no longer sim state (P2.2). Everything
    /// here is derived: the position and angles convert from fixed point, the scale is a
    /// property of the TYPE, the colour is a property of the ARMY, and the walk-cycle phase is
    /// the ground the unit has covered divided by the stride its animation implies.
    ///
    /// One way only. Nothing reads a `UnitInstance` back into the store — that would be a
    /// float round-trip through the middle of a match, which is exactly what fixed point is
    /// for avoiding.
    [[nodiscard]] rm::UnitInstance instanceFor(rm::UnitIndex slot,
                                               const rm::sim::Transform& transform,
                                               const rm::sim::MoveState& motion) const {
        rm::UnitInstance instance{};
        instance.position = {rm::sim::fxToFloat(transform.x), rm::sim::fxToFloat(transform.y),
                             rm::sim::fxToFloat(transform.z)};
        instance.rotationY = rm::sim::radiansFromBrad(transform.heading);
        instance.rotationX = rm::sim::radiansFromBrad(transform.pitch);
        instance.rotationZ = rm::sim::radiansFromBrad(transform.roll);

        const auto type = static_cast<std::size_t>(store.typeAt(slot));
        instance.scale = type < typeScale.size() ? typeScale[type] : 1.0f;

        // The army's colour. `kNoArmy` and an out-of-range owner both get the first palette
        // entry, which is what a decorative crowd should look like — the alternative, treating
        // an unowned unit as army zero's, is the bug `kNoArmy = -1` exists to prevent, and it
        // is prevented in the SIM rather than here.
        const int owner = motion.armyIndex;
        instance.teamColour =
            owner >= 0 && static_cast<std::size_t>(owner) < armies.size()
                ? armies[static_cast<std::size_t>(owner)].colour
                : rm::kTeamColours[0];

        // The walk cycle, paced by ground covered rather than by wall time — a unit pivoting
        // on the spot or standing still must not keep striding. Zero for a type with no
        // animation, which is every structure.
        const float duration = type < batches.size() && batches[type].animation != nullptr
                                   ? batches[type].animation->duration
                                   : 0.0f;
        const float speed = rm::sim::fxToFloat(motion.speedPerTick)
                            * static_cast<float>(kAppTickRate.ticksPerSecond());
        const float strideElmos = speed * duration;
        if (strideElmos > 0.0f) {
            instance.animationPhase =
                rm::sim::fxToFloat(motion.distanceTravelledElmos) / strideElmos;
        }

        return instance;
    }

    /// The unit behind a drawn instance, or nothing when the pair names nothing drawn.
    [[nodiscard]] std::optional<rm::sim::UnitId> unitDrawnAt(std::size_t batch,
                                                             std::size_t index) const {
        if (batch >= drawSlotOf.size() || index >= drawSlotOf[batch].size()) {
            return std::nullopt;
        }
        return store.idAt(drawSlotOf[batch][index]);
    }

    /// Who owns the unit in a slot, or kNoArmy.
    [[nodiscard]] int armyOf(rm::UnitIndex slot) const noexcept {
        const std::span<const rm::sim::MoveState> motion = store.motion();
        if (slot >= motion.size()) {
            return rm::sim::kNoArmy;
        }
        return motion[slot].armyIndex;
    }

    /// Where a live unit is being drawn, or nothing when it is dead or undrawable.
    ///
    /// The bridge between a handle, which is how the sim and the UI name a unit, and a
    /// (batch, index) pair, which is the only thing the renderer can outline.
    [[nodiscard]] std::optional<rm::SelectionEntry> drawnAt(rm::sim::UnitId id) const {
        if (!store.alive(id) || id.index >= drawIndexOf.size()) {
            return std::nullopt;
        }
        const rm::SelectionEntry& where = drawIndexOf[id.index];
        if (where.batch >= drawScratch.size()
            || where.instance >= drawScratch[where.batch].size()) {
            return std::nullopt;
        }
        return where;
    }

};

// The passability grids a scene needs, one per distinct pair of limits.
//
// Keyed on the LIMITS rather than on the unit type, because the grid depends on
// nothing else — every unit that climbs 17 degrees and wades 12 elmos sees the
// same map, whatever model it wears. On a scene of a dozen unit types that is
// usually two or three grids rather than a dozen.
class PassabilitySet {
public:
    PassabilitySet(const rm::HeightField& field, float waterLevel)
        : field_{&field}, waterLevel_{waterLevel} {}

    [[nodiscard]] const rm::sim::PassabilityGrid& gridFor(float slopeDegrees, float depthElmos) {
        const auto key = std::make_pair(slopeDegrees, depthElmos);
        const auto existing = grids_.find(key);
        if (existing != grids_.end()) {
            return existing->second;
        }

        rm::sim::PassabilityGrid grid =
            rm::sim::buildPassability(*field_, waterLevel_, slopeDegrees, depthElmos);
        std::printf("passability: %d x %d cells of %.0f elmos, %zu%% walkable"
                    " (maxslope %.0f deg, maxwaterdepth %.0f)\n",
                    grid.cellsX, grid.cellsZ,
                    static_cast<double>(rm::sim::fxToFloat(grid.elmosPerCell)),
                    grid.passable.empty()
                        ? 0u
                        : 100u * static_cast<std::size_t>(std::count(grid.passable.begin(),
                                                                     grid.passable.end(),
                                                                     std::uint8_t{1}))
                              / grid.passable.size(),
                    static_cast<double>(slopeDegrees), static_cast<double>(depthElmos));

        return grids_.emplace(key, std::move(grid)).first->second;
    }

private:
    const rm::HeightField* field_;
    float waterLevel_;
    std::map<std::pair<float, float>, rm::sim::PassabilityGrid> grids_;
};
// `paceAnimationByDistance` and `paceSceneAnimations` used to live here.
//
// Both are gone, and not merely moved: the walk-cycle phase is now computed inside
// `UnitScene::instanceFor`, the draw projection. It was always a DERIVED value — ground
// covered divided by the stride the animation implies — and once `UnitInstance` stopped being
// sim state there was nowhere for a separate pass to write it to. Two functions and two call
// sites became four lines in the one place that builds an instance.


/// The colour of the ring drawn on the ground under a selected unit.
///
/// The ONLY selection feedback. Milestone 13 also tinted the selected unit
/// white through its team-colour slot, which was free but misleading: in an RTS
/// a unit's own colours mean allegiance, and overwriting them to mean "selected"
/// makes a unit change sides for as long as it is in the set. A marker on the
/// ground says the same thing without lying about the unit (ADR-021).
///
/// One colour for every unit rather than the unit's own team colour: a
/// selection is "mine", and the question a ring answers is which units an order
/// will reach — not which army they belong to, which the model already says.
///
/// Bright green because it is the one hue no stratum, sea or sky in either
/// game's palette occupies at this saturation, and translucent so the ground it
/// marks still reads through it.
inline constexpr std::array<float, 4> kSelectionRingColour{{0.35f, 1.0f, 0.45f, 0.55f}};

/// How much wider than the unit's collision radius the ring is drawn.
///
/// A ring exactly on the radius touches the model's feet and reads as part of
/// it. A little outside reads as a marker on the ground, which is what it is.
inline constexpr float kSelectionRingMargin = 1.35f;

/// The colour of the marker where a move order was given.
///
/// Amber against the rings' green, and a cross rather than a plain ring, because
/// the two appear on the same ground within a second of each other — a right-click
/// follows a left-click — and one distinction would not be enough in the one place
/// the player is looking. Two, so it survives being small or being colour-blind.
inline constexpr std::array<float, 4> kOrderMarkerColour{{1.0f, 0.72f, 0.20f, 0.85f}};

/// One `--units` argument: a model, how many of it, how big, and optionally an
/// animation to play on it.
struct UnitOptions {
    std::filesystem::path modelPath;
    std::size_t count = kDefaultUnitCount;
    float scale = 1.0f;
    std::filesystem::path animationPath;
};

/// Resolves a `--units` argument that names a unit DEFINITION rather than a
/// model, returning the model to load and the stats to move it with.
///
/// A `.lua` argument is the interesting path: the definition names its own
/// model and carries the speed and turn rate the game authored for it, which is
/// how a scene stops moving every unit at the one speed this engine used to
/// hardcode.
[[nodiscard]] std::optional<rm::unitdef::UnitDef> resolveUnitDef(
    const std::filesystem::path& path, const rm::vfs::AssetSearch& search,
    std::filesystem::path& modelOut) {
    // A Supreme Commander blueprint. Its mesh is not named inside it, so the
    // reader that finds one by convention is the same one that reads the stats.
    if (path.extension() == ".bp") {
        const auto def = rm::unitbp::loadFile(path);
        if (!def) {
            std::fprintf(stderr, "unit blueprint \"%s\" not read: %s\n",
                         path.filename().string().c_str(), def.error().message.c_str());
            return std::nullopt;
        }

        // The game root a `MeshName` path is relative to, when the blueprint uses
        // one: two directories up from `<root>/units/<ID>/<ID>_unit.bp`.
        const std::filesystem::path root = path.parent_path().parent_path().parent_path();
        modelOut = rm::unitbp::resolveMesh(*def, path, root);
        if (modelOut.empty()) {
            std::fprintf(stderr,
                         "unit \"%s\" has no mesh beside it%s\n", def->name.c_str(),
                         def->modelPath.empty() ? "" : " and its MeshName did not resolve");
            return std::nullopt;
        }

        std::printf("unit %s: %.0f elmos/s, %.2f rad/s, radius %.1f elmos, %.0f hp, %s\n",
                    def->name.c_str(), static_cast<double>(def->speedElmosPerSecond),
                    static_cast<double>(def->turnRateRadiansPerSecond),
                    static_cast<double>(def->collisionRadiusElmos),
                    static_cast<double>(rm::sim::magToFloat(def->health)),
                    rm::unitdef::travelsOnGround(def->motion) ? "on the ground"
                    : def->canFly                            ? "flying"
                                                             : "not a ground mover");
        return *def;
    }

    if (path.extension() != ".lua") {
        return std::nullopt;
    }

    const auto def = rm::unitdef::loadFile(path);
    if (!def) {
        std::fprintf(stderr, "unit definition \"%s\" not read: %s\n",
                     path.filename().string().c_str(), def.error().message.c_str());
        return std::nullopt;
    }

    modelOut = rm::unitdef::resolveModel(search, def->modelPath);

    if (modelOut.empty()) {
        // Nothing in the search path — which is the usual case, since pointing
        // at a definition by its full path is how this is used and needs no
        // --data-dir. Walk up from the definition to a sibling objects3d:
        // `units/ArmBots/armpw.lua` and `objects3d/Units/armpw.s3o` are a few
        // levels apart, so the layout answers the question.
        rm::vfs::AssetSearch beside;
        for (std::filesystem::path dir = path.parent_path();
             !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
            const std::filesystem::path candidate = dir / "objects3d";
            if (std::filesystem::is_directory(candidate)) {
                beside.addRoot(candidate);
                break;
            }
        }
        modelOut = rm::unitdef::resolveModel(beside, def->modelPath);
    }

    if (modelOut.empty()) {
        std::fprintf(stderr, "unit \"%s\" names model \"%s\", which was not found"
                             " (try --data-dir)\n",
                     def->name.c_str(), def->modelPath.c_str());
        return std::nullopt;
    }

    std::printf("unit %s: %.0f elmos/s, %.2f rad/s, footprint %d x %d squares, %.0f hp%s\n",
                def->name.c_str(), static_cast<double>(def->speedElmosPerSecond),
                static_cast<double>(def->turnRateRadiansPerSecond), def->footprintSquaresX,
                def->footprintSquaresZ,
                static_cast<double>(rm::sim::magToFloat(def->health)),
                def->canFly ? " (flies)" : "");
    return *def;
}

// A unit resolved out of the mounted content: everything `resolveUnits` needs, so
// that the archive path and the filesystem path converge before the code that
// places instances rather than branching all the way down it.
struct VfsUnit {
    rm::unitdef::UnitDef def;
    rm::Model model;
    std::string albedoPath;
    std::string shadingPath;
};

/// A Supreme Commander texture beside a mesh, inside the mounted content.
///
/// `scmTexturePath` cannot serve this: it decides whether to strip a `_lod0` by
/// asking the real filesystem whether the stripped file exists, which is a
/// filesystem assumption living inside what is otherwise string work. Given a VFS
/// path nothing exists on disk, so it kept the `_lod0` and looked for
/// `UEL0201_lod0_Albedo.dds` — a name the archive does not contain.
///
/// The strip is right and the check is what has to move. Level 0 shares the unit's
/// own textures (`UEL0201_Albedo.dds`) while every coarser level has its OWN
/// (`UEL0201_lod1_Albedo.dds`), both of which the archive really carries — so the
/// stripped spelling is tried first and the literal one second, and the content
/// decides rather than the disk.
[[nodiscard]] std::string scmTextureInVfs(const std::string& meshPath, const char* suffix,
                                          const rm::vfs::Vfs& content) {
    const std::filesystem::path path{meshPath};
    const std::string stem = path.stem().string();
    const std::string dir = path.parent_path().generic_string();

    static constexpr std::string_view kLod0 = "_lod0";
    if (stem.size() > kLod0.size()) {
        std::string tail = stem.substr(stem.size() - kLod0.size());
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == kLod0) {
            const std::string stripped =
                dir + "/" + stem.substr(0, stem.size() - kLod0.size()) + suffix;
            if (content.contains(stripped)) {
                return stripped;
            }
        }
    }

    const std::string literal = dir + "/" + stem + suffix;
    return content.contains(literal) ? literal : std::string{};
}

/// Reads a unit entirely out of the mounted content: blueprint, then the mesh it
/// does not name, then the textures neither of them names.
///
/// This is the path a real game takes and the reason the VFS exists. Nothing is
/// extracted, and the paths are the ones the content uses about itself.
[[nodiscard]] std::optional<VfsUnit> resolveUnitFromContent(const std::string& blueprintPath,
                                                            const rm::vfs::Vfs& content) {
    const auto source = content.read(blueprintPath);
    if (!source) {
        return std::nullopt;
    }

    const std::string text{reinterpret_cast<const char*>(source->data()), source->size()};
    const auto def = rm::unitbp::load(text, blueprintPath);
    if (!def) {
        std::fprintf(stderr, "unit blueprint \"%s\" not read: %s\n", blueprintPath.c_str(),
                     def.error().message.c_str());
        return std::nullopt;
    }

    const std::string meshPath = rm::unitbp::resolveMeshInVfs(*def, blueprintPath, content);
    if (meshPath.empty()) {
        std::fprintf(stderr, "unit \"%s\" has no mesh in the mounted content\n",
                     def->name.c_str());
        return std::nullopt;
    }

    const auto meshBytes = content.read(meshPath);
    if (!meshBytes) {
        return std::nullopt;
    }
    auto model = loadModelBytes(*meshBytes);
    if (!model) {
        std::fprintf(stderr, "failed to load mesh \"%s\": %s\n", meshPath.c_str(),
                     model.error().message.c_str());
        return std::nullopt;
    }

    std::printf("unit %s: %.0f elmos/s, %.2f rad/s, radius %.1f elmos, %.0f hp, %s\n",
                def->name.c_str(), static_cast<double>(def->speedElmosPerSecond),
                static_cast<double>(def->turnRateRadiansPerSecond),
                static_cast<double>(def->collisionRadiusElmos),
                static_cast<double>(rm::sim::magToFloat(def->health)),
                rm::unitdef::travelsOnGround(def->motion) ? "on the ground"
                : def->canFly                            ? "flying"
                                                         : "not a ground mover");

    // A mesh read from bytes has no file to take its name from, so it arrives
    // nameless — and the name is what every log line and the batch report identify
    // it by. The blueprint's id is the better answer anyway: it is what the rest of
    // the content calls this unit.
    model->name = def->name;

    return VfsUnit{
        .def = *def,
        .model = std::move(*model),
        .albedoPath = scmTextureInVfs(meshPath, kScmDiffuseSuffix, content),
        .shadingPath = scmTextureInVfs(meshPath, kScmShadingSuffix, content),
    };
}

/// Spawns one commander per start position: a skirmish's opening position.
///
/// The map decides how many sides there are — its `ARMY_<n>` markers, which
/// `scenario::loadStartPositions` already reads — and the factions are dealt
/// round-robin so the same map yields the same match every run.
///
/// Commanders of the same faction SHARE A BATCH, because a batch is one model and two
/// UEF armies field the same one. What distinguishes them is the instance's colour,
/// which is exactly the split UnitInstance was built for: the GPU gets the army's
/// colour and never its index.
/// What an army starts with, and the baseline its storage is recomputed from every
/// tick: OUR constant, not a blueprint's — enough to afford the first extractor and
/// see the bars move, per milestone 19.
inline const rm::sim::Resources kStartingStorage{.mass = rm::sim::Mag::fromInt(650),
                                                .energy = rm::sim::Mag::fromInt(5000)};

void spawnCommanders(UnitScene& scene, const rm::HeightField& field,
                     std::span<const rm::mapinfo::StartPosition> starts,
                     const rm::vfs::Vfs& content) {
    if (starts.empty()) {
        std::fprintf(stderr, "skirmish: the map declares no start positions\n");
        return;
    }

    scene.armies = rm::sim::freeForAll(starts.size());
    scene.playerArmy = 0;

    // Faction -> the batch already holding that faction's commander, so four models
    // serve eight armies, and the scale that model needs.
    std::map<rm::sim::Faction, std::size_t> batchForFaction;
    std::map<rm::sim::Faction, float> scaleForFaction;

    for (const rm::sim::Army& army : scene.armies) {
        const std::string path = rm::sim::commanderBlueprintPath(army.faction);

        const auto existing = batchForFaction.find(army.faction);
        if (existing == batchForFaction.end()) {
            const auto unit = resolveUnitFromContent(path, content);
            if (!unit) {
                std::fprintf(stderr, "skirmish: no commander for %s\n",
                             std::string{rm::sim::factionName(army.faction)}.c_str());
                continue;
            }

            scene.models.push_back(unit->model);
            scene.maxSlopeDegrees.push_back(unit->def.maxSlopeDegrees > 0.0f
                                                ? unit->def.maxSlopeDegrees
                                                : rm::sim::kDefaultMaxSlopeDegrees);
            scene.maxWaterDepthElmos.push_back(unit->def.maxWaterDepthElmos);
            scene.typeScale.push_back(unit->def.meshToElmos);

            scene.batches.push_back(rm::UnitBatch{
                .model = &scene.models.back(),
                .instances = {},
                .textures = rm::TexturePair{
                    .diffuse = scene.textures.resolve(content, unit->albedoPath, "albedo"),
                    .shading = scene.textures.resolve(content, unit->shadingPath, "specTeam"),
                },
            });
            batchForFaction.emplace(army.faction, scene.batches.size() - 1);
            scaleForFaction.emplace(army.faction, unit->def.meshToElmos);

            scene.definitions.push_back(unit->def);
            const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back());
            // A type index and a batch index are the same number, by construction. Asserted
            // rather than assumed: everything from the passability grid to the draw gather
            // reads one as the other, and a drift here is a silent mismatch rather than a
            // crash. (The old per-batch parallel arrays asserted the same thing and read
            // `+ 1` until milestone 20 — never caught, because Release compiles asserts out,
            // which is exactly one assert's worth of irony.)
            assert(static_cast<std::size_t>(type) + 1 == scene.batches.size());
            assert(scene.catalog.size() == scene.batches.size());
            (void)type;

            const auto armed = static_cast<std::size_t>(std::ranges::count_if(
                unit->def.weapons, [](const rm::unitdef::Weapon& w) { return w.fires(); }));
            std::printf("army %d: %s commander %s, %.0f hp, %zu weapon(s)\n", army.index,
                        std::string{rm::sim::factionName(army.faction)}.c_str(),
                        unit->def.name.c_str(),
                        static_cast<double>(rm::sim::magToFloat(unit->def.health)), armed);

        }

        const std::size_t batch = batchForFaction.at(army.faction);
        const rm::mapinfo::StartPosition& start = starts[static_cast<std::size_t>(army.index)];

        // One commander, on the ground, in its army's colour. `atStartPositions` would
        // place one per start in one call, but each needs a DIFFERENT colour and army,
        // so they are placed one at a time.
        // The blueprint's own scale, NOT 1: a `.scm`'s vertices are not in elmos, and
        // taking them for elmos draws a commander an order of magnitude too big. Same
        // trap as the tanks, one commit earlier (AGENT.md).
        std::vector<rm::UnitInstance> one =
            rm::atStartPositions(field, std::span{&start, 1}, scaleForFaction.at(army.faction));
        if (one.empty()) {
            continue;
        }
        // `atStartPositions` places a `UnitInstance` because that is what the placement
        // helper has always produced; only its position is wanted here, and the colour and
        // scale it also sets are now the draw projection's business.
        rm::sim::MoveState motion;
        motion.armyIndex = army.index;

        const auto type = static_cast<rm::UnitTypeIndex>(batch);
        const rm::unitdef::UnitDef* def = scene.catalog.def(type);
        const rm::sim::Mag hp = def != nullptr ? def->health : rm::sim::Mag{};
        (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
            .type = type,
            .transform = transformAt(one.front().position, one.front().rotationY),
            .motion = motion,
            .health = rm::sim::Health{.current = hp, .maximum = hp},
        });
    }

    // The batches' spans are filled from the store, once, here. They used to be re-pointed
    // after every push_back because a vector that grew had moved its storage; the store owns
    // that storage now and the gather hands each batch a contiguous run.
    scene.gatherForDrawing();

    // Each army starts with the commander's trickle and one extractor's worth of storage,
    // which is what makes the first build affordable — see kCommanderTrickle.
    scene.commandersEver = scene.countCommanders();
    scene.economies.assign(scene.armies.size(), rm::sim::Economy{});
    for (rm::sim::Economy& economy : scene.economies) {
        // The commander's trickle, per tick. `recomputeIncome` recomputes this every tick
        // from what is standing; seeding it here is what makes the first tick's earnings
        // spendable before anything has been counted.
        economy.incomePerTick = rm::sim::Resources{
            .mass = kAppTickRate.magPerTick(rm::sim::kCommanderTrickleMassPerSecond),
            .energy = kAppTickRate.magPerTick(rm::sim::kCommanderTrickleEnergyPerSecond),
        };
        economy.storage = kStartingStorage;
        // FULL at spawn, which is what the game does — a match opens with the
        // starting storage banked, and that bank is what pays for the first base.
        // Milestone 19 started empty, which worked only because the extractor was
        // the sole build: a power generator costs 750 energy against a 5-a-second
        // trickle, and an empty start parks the whole build order for four minutes.
        economy.stored = kStartingStorage;
    }

    std::printf("skirmish: %zu armies, %zu commander model(s)\n", scene.armies.size(),
                batchForFaction.size());
}

/// Spawns one unit of `blueprintPath` for `army` at `position`, creating its batch on
/// first use so twenty tanks stay one draw call.
///
/// The milestone-20 counterpart of spawnCommanders, and the moment a finished
/// Construction stops being a number in the economy and becomes a thing on the map:
/// grounded on the height field, tilted onto its slope, in its army's colour, with its
/// definition's own health, radius and speed. Returns the batch and instance it landed
/// in, or nothing when the blueprint or its model is not in the mounted content.
[[nodiscard]] std::optional<rm::sim::UnitId> spawnUnit(UnitScene& scene,
                                                          const rm::vfs::Vfs& content,
                                                          const rm::HeightField& field,
                                                          std::string_view blueprintPath,
                                                          std::array<float, 3> position,
                                                          const rm::sim::Army& army,
                                                          float yaw) {
    auto found = scene.batchForBlueprint.find(blueprintPath);
    if (found == scene.batchForBlueprint.end()) {
        const auto unit = resolveUnitFromContent(std::string{blueprintPath}, content);
        if (!unit) {
            std::fprintf(stderr, "spawn: no unit at %s in the mounted content\n",
                         std::string{blueprintPath}.c_str());
            return std::nullopt;
        }

        scene.models.push_back(unit->model);
        // The same fallback rules as resolveUnits: a structure's zero slope means
        // the default, and a ground mover's zero depth means "does not wade" —
        // see ADR-027.
        const bool grounded = rm::unitdef::travelsOnGround(unit->def.motion);
        scene.maxSlopeDegrees.push_back(unit->def.maxSlopeDegrees > 0.0f
                                            ? unit->def.maxSlopeDegrees
                                            : rm::sim::kDefaultMaxSlopeDegrees);
        scene.maxWaterDepthElmos.push_back(grounded ? unit->def.maxWaterDepthElmos
                                                    : rm::sim::kDefaultMaxWaterDepthElmos);
        scene.typeScale.push_back(unit->def.meshToElmos);
        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = {},
            .textures = rm::TexturePair{
                .diffuse = scene.textures.resolve(content, unit->albedoPath, "albedo"),
                .shading = scene.textures.resolve(content, unit->shadingPath, "specTeam"),
            },
        });
        scene.definitions.push_back(unit->def);
        (void)scene.catalog.add(&scene.definitions.back());
        assert(scene.catalog.size() == scene.batches.size());

        found = scene.batchForBlueprint.emplace(std::string{blueprintPath},
                                                scene.batches.size() - 1).first;
    }

    const auto type = static_cast<rm::UnitTypeIndex>(found->second);
    const rm::unitdef::UnitDef& def = *scene.catalog.def(type);

    const rm::sim::Terrain terrain{field};
    rm::sim::Transform transform;
    transform.x = rm::sim::fxFromFloat(position[0]);
    transform.z = rm::sim::fxFromFloat(position[2]);
    transform.y = terrain.heightAt(transform.x, transform.z);
    transform.heading = rm::sim::bradFromRadians(yaw);
    const std::array<rm::Brad, 2> align =
        rm::sim::slopeAlignment(terrain, transform.x, transform.z, transform.heading);
    transform.pitch = align[0];
    transform.roll = align[1];

    rm::sim::MoveState motion;
    motion.armyIndex = army.index;
    motion.radiusElmos = rm::sim::fxFromFloat(def.collisionRadiusElmos);
    if (def.isMobile()) {
        // Per second in the blueprint, per tick in the sim — converted here because this is
        // where a unit is built from its definition (§5.1). A structure gets zero, which is
        // what makes it a structure as far as movement is concerned.
        motion.speedPerTick = kAppTickRate.perTick(def.speedElmosPerSecond);
        if (def.turnRateRadiansPerSecond > 0.0f) {
            motion.turnPerTick = kAppTickRate.bradPerTick(def.turnRateRadiansPerSecond);
        } else {
            motion.turnPerTick =
                kAppTickRate.bradPerTick(rm::sim::kDefaultTurnRateRadiansPerSecond);
        }
    }

    const rm::sim::UnitId id = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = transform,
        .motion = motion,
        .health = rm::sim::Health{.current = def.health, .maximum = def.health},
    });

    // The store may have grown its arrays, so any span into them is stale — which for the
    // sim means the caller-side tick has to re-read them, and for the renderer means the
    // draw gather has to run again before the batch spans are trusted.
    scene.grewThisTick = true;

    return id;
}

/// Finds (or loads and registers) the buildable entry for `blueprintPath`, so every
/// Construction of the same blueprint shares one definition and one index.
[[nodiscard]] std::optional<std::size_t> resolveBuildable(UnitScene& scene,
                                                          const rm::vfs::Vfs& content,
                                                          std::string_view blueprintPath) {
    for (std::size_t i = 0; i < scene.buildablePaths.size(); ++i) {
        if (scene.buildablePaths[i] == blueprintPath) {
            return i;
        }
    }
    const auto bytes = content.read(std::string{blueprintPath});
    if (!bytes) {
        std::fprintf(stderr, "economy: no blueprint at %s in the mounted content\n",
                     std::string{blueprintPath}.c_str());
        return std::nullopt;
    }
    const auto def = rm::unitbp::load(
        std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()},
        std::string{blueprintPath});
    if (!def) {
        return std::nullopt;
    }
    scene.buildable.push_back(*def);
    scene.buildablePaths.emplace_back(blueprintPath);
    return scene.buildable.size() - 1;
}

/// Orders every commander to build a mass extractor on its nearest deposit.
///
/// The whole of milestone 19's build order, and deliberately not an AI: one structure,
/// on the site the MAP names, paid for out of the trickle a commander produces. What it
/// demonstrates is the chain — a marker becomes a site, a blueprint becomes a cost, and
/// the cost is met over time rather than at once. Milestone 20's scripted opponent
/// (core/sim/BuildOrder.hpp) continues from exactly this point.
///
/// The extractor is the right first thing for the same reason it is in the game: it is the
/// cheapest structure that pays for the next one.
void orderFirstExtractors(UnitScene& scene, std::span<const rm::scenario::Marker> markers,
                          const rm::vfs::Vfs& content) {
    if (scene.armies.empty()) {
        return;
    }

    // The extractor blueprint, once. Read per FACTION in the game — each has its own — but
    // the UEF one serves here: this milestone is about the economy rather than about
    // faction-specific structures, and pretending otherwise would mean four blueprints
    // where one demonstrates the same thing.
    const std::optional<std::size_t> registered =
        resolveBuildable(scene, content, rm::sim::kExtractorBlueprint);
    if (!registered) {
        return;
    }
    const std::size_t blueprintIndex = *registered;
    const rm::unitdef::UnitDef* extractor = &scene.buildable[blueprintIndex];

    // Which commander is where, so each can be sent to its OWN nearest deposit rather than
    // all of them to one.
    float firstBuilderRate = 1.0f;
    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !def->isBuilder()) {
            continue;
        }
        const int army = scene.store.motion()[slot].armyIndex;
        if (army == rm::sim::kNoArmy) {
            continue;
        }
        firstBuilderRate = std::max(1.0f, def->buildRate);

        const std::array<rm::sim::Fx, 3> from =
            rm::sim::positionOf(scene.store.transforms()[slot]);
        const rm::scenario::Marker* nearest = nullptr;
        rm::sim::Fx nearestDistance{};
        for (const rm::scenario::Marker& marker : markers) {
            if (!marker.isType("Mass")) {
                continue;
            }
            // The marker's position is float — it comes from a map file — so it crosses into
            // fixed point here, at the boundary, rather than the distance being computed in
            // floats and compared against sim values.
            const rm::sim::Fx distance = rm::sim::groundDistanceElmos(
                from, {rm::sim::fxFromFloat(marker.position[0]),
                       rm::sim::fxFromFloat(marker.position[1]),
                       rm::sim::fxFromFloat(marker.position[2])});
            if (nearest == nullptr || distance < nearestDistance) {
                nearest = &marker;
                nearestDistance = distance;
            }
        }
        if (nearest == nullptr) {
            continue;  // a map with no mass on it: nothing to extract
        }

        scene.building.push_back(rm::sim::Construction{
            .armyIndex = army,
            .position = nearest->position,
            .cost = {.mass = extractor->buildCostMass,
                     .energy = extractor->buildCostEnergy},
            .buildTimeRemaining = extractor->buildTime,
            .totalBuildTime = extractor->buildTime,
            .buildPerTick = kAppTickRate.magPerTick(def->buildRate),
            .blueprintIndex = blueprintIndex,
        });
    }

    std::printf("economy: %zu extractor(s) ordered on the map's own deposits,"
                " %.0f mass / %.0f energy each over %.0fs\n",
                scene.building.size(),
                static_cast<double>(rm::sim::magToFloat(extractor->buildCostMass)),
                static_cast<double>(rm::sim::magToFloat(extractor->buildCostEnergy)),
                static_cast<double>(rm::sim::magToFloat(extractor->buildTime)
                                    / firstBuilderRate));
}

/// Loads every requested model, resolves its textures, and places instances.
///
/// The first model takes the map's start positions and fills the rest of its
/// count by scatter; every later model is scattered alone. Spawn points read as
/// meaningful, and a second model landing on top of the first at every spawn
/// would not.
[[nodiscard]] UnitScene resolveUnits(std::span<const UnitOptions> requests,
                                     const rm::HeightField& field,
                                     std::span<const rm::mapinfo::StartPosition> starts,
                                     float landAbove, const rm::vfs::AssetSearch& search,
                                     const rm::vfs::Vfs& content) {
    UnitScene scene;

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const UnitOptions& request = requests[i];

        // `--units` takes a model, a unit DEFINITION, or a blueprint inside the
        // mounted content. A definition brings the stats the game authored for it,
        // which is the whole point: otherwise every unit moves at the one speed this
        // engine used to hardcode.
        //
        // THE CONTENT IS TRIED FIRST, because a path that resolves in the game's own
        // namespace means the caller named game content and not a file that happens
        // to sit at the same place on this disk. Nothing else distinguishes them: a
        // VFS path is spelled like a path, deliberately.
        const std::string requestedPath = request.modelPath.generic_string();
        std::optional<VfsUnit> fromContent;
        if (request.modelPath.extension() == ".bp" && content.contains(requestedPath)) {
            fromContent = resolveUnitFromContent(requestedPath, content);
            if (!fromContent) {
                continue;  // the blueprint said something that could not be honoured
            }
        }

        std::filesystem::path modelPath = request.modelPath;
        std::optional<rm::unitdef::UnitDef> def;
        std::expected<rm::Model, rm::MapError> model{rm::Model{}};

        if (fromContent) {
            def = fromContent->def;
            model = std::move(fromContent->model);
        } else {
            def = resolveUnitDef(request.modelPath, search, modelPath);
            if ((request.modelPath.extension() == ".lua"
                 || request.modelPath.extension() == ".bp")
                && !def) {
                continue;  // the definition said something that could not be honoured
            }

            model = loadModel(modelPath);
            if (!model) {
                std::fprintf(stderr, "failed to load model \"%s\": %s\n",
                             modelPath.string().c_str(), model.error().message.c_str());
                continue;  // one bad model should not cost the whole scene
            }
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

        // Textures are named after the MESH, not after whatever the command line
        // pointed at — `UEL0201_Albedo.dds` beside `UEL0201_lod0.scm`. It matters
        // now that a blueprint can be the argument: taking the request's path
        // would look for `UEL0201_unit_Albedo.dds`, find nothing, and draw the
        // unit in the fallback white, which reads as a missing texture in the
        // content rather than a wrong path here.
        const rm::TexturePair pair =
            fromContent
                ? rm::TexturePair{
                      .diffuse = scene.textures.resolve(content, fromContent->albedoPath,
                                                        "albedo"),
                      .shading = scene.textures.resolve(content, fromContent->shadingPath,
                                                        "specTeam"),
                  }
            : supCom ? rm::TexturePair{
                         .diffuse = scene.textures.resolve(
                             scmTexturePath(modelPath, kScmDiffuseSuffix), "albedo"),
                         .shading = scene.textures.resolve(
                             scmTexturePath(modelPath, kScmShadingSuffix), "specTeam"),
                     }
                   : rm::TexturePair{
                         .diffuse = scene.textures.resolve(
                             barTexturePath(search, model->textures[0]), "diffuse"),
                         .shading = scene.textures.resolve(
                             barTexturePath(search, model->textures[1]), "shading"),
                     };

        // How big the model is, and the answer depends on whether a blueprint
        // said.
        //
        // WITH ONE, it is `meshToElmos` — `Display.UniformScale` times the eight
        // elmos in an ogrid — and that is the file's own answer rather than ours.
        //
        // WITHOUT ONE, a bare `.scm` gets the x8 that has stood here since
        // milestone 7, on the assumption that its vertices are in ogrids. THEY ARE
        // NOT: raw mesh extents across the corpus run from 10 to 262 units, which
        // as ogrids would make one experimental 2096 elmos long — a quarter of the
        // map it stands on — and UEL0201 a 65-elmo tank instead of a 4.5-elmo one.
        // The blueprint is what closes that gap, so the old factor is left only
        // where there is no blueprint to consult and nothing better to guess.
        const float scale = request.scale
                          * (def && def->meshToElmos > 0.0f ? def->meshToElmos
                             : supCom                       ? kOgridScale
                                                            : 1.0f);

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

        // A definition that omits these — or a building, whose maxslope is 0 —
        // falls back to the defaults rather than to a grid nothing can cross.
        //
        // A GROUND MOVER'S ZERO IS NOT A MISSING VALUE, which is why the depth
        // test asks the motion class rather than the number. Supreme Commander's
        // land units state no wading depth because they do not wade, and reading
        // that 0 as "unstated" would hand them BAR's 12 elmos and walk them into
        // the sea. Slope keeps the numeric guard: no unit means 0 to mean "cannot
        // move at all".
        const bool grounded = def && rm::unitdef::travelsOnGround(def->motion);
        const float slope = def && def->maxSlopeDegrees > 0.0f
                                ? def->maxSlopeDegrees
                                : rm::sim::kDefaultMaxSlopeDegrees;
        const float depth = grounded ? def->maxWaterDepthElmos
                            : def.has_value() && def->maxWaterDepthElmos > 0.0f
                                ? def->maxWaterDepthElmos
                                : rm::sim::kDefaultMaxWaterDepthElmos;
        scene.maxSlopeDegrees.push_back(slope);
        scene.maxWaterDepthElmos.push_back(depth);
        scene.typeScale.push_back(def.has_value() ? def->meshToElmos : 1.0f);

        // The TYPE for these units. One per batch, and the two indices are the same number
        // by construction — which is the whole reason the old parallel-array hazard here is
        // gone: there is one array to append to, not four that had to be appended to
        // together or the next batch wrote its entries at the wrong index and read health
        // off the end. (That was a segfault the moment `--units` and `--skirmish` were given
        // together.)
        rm::UnitTypeIndex type = 0;
        if (def) {
            scene.definitions.push_back(*def);
            type = scene.catalog.add(&scene.definitions.back());
        } else {
            type = scene.catalog.add(nullptr);  // a bare model: it neither fires nor dies
        }
        assert(static_cast<std::size_t>(type) == scene.batches.size());

        // A definition's speed and turn rate reach every unit of it. Slope and depth limits
        // do NOT yet: passability is one grid for the whole scene, so honouring them per
        // unit type would mean a grid per type.
        const rm::sim::Mag hp = def.has_value() ? def->health : rm::sim::Mag{};
        for (const rm::UnitInstance& instance : placed) {
            rm::sim::MoveState state;
            if (def) {
                // The footprint is what a unit takes up, whether or not it moves — a
                // building is still something to be pushed out of.
                state.radiusElmos = rm::sim::fxFromFloat(def->collisionRadiusElmos);
                if (def->isMobile()) {
                    state.speedPerTick = kAppTickRate.perTick(def->speedElmosPerSecond);
                    state.turnPerTick = kAppTickRate.bradPerTick(
                        def->turnRateRadiansPerSecond > 0.0f
                            ? def->turnRateRadiansPerSecond
                            : rm::sim::kDefaultTurnRateRadiansPerSecond);
                }
            }
            (void)scene.store.spawn(rm::sim::UnitStore::Spawn{
                .type = type,
                .transform = transformAt(instance.position, instance.rotationY),
                .motion = state,
                .health = rm::sim::Health{.current = hp, .maximum = hp},
            });
        }

        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = {},
            .textures = pair,
            .animation = animation,
        });
    }

    // The batches' instance spans, filled from the store now that every unit is in it.
    scene.gatherForDrawing();

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

/// Builds the asset search path from `--data-dir <dir>`.
///
/// The BAR path, which resolves by filename under real directories because that is
/// how a `.s3o` names its textures. The Supreme Commander path goes through the VFS
/// instead — see parseContent.
[[nodiscard]] rm::vfs::AssetSearch parseAssetSearch(int argc, const char* argv[]) {
    rm::vfs::AssetSearch search;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string{argv[i]} == "--data-dir") {
            search.addRoot(argv[i + 1]);
        }
    }

    return search;
}

/// Mounts the game's content: `--gamedata <dir>` for a whole install, `--archive
/// <scd>` for one archive, `--data-dir <dir>` for a loose directory.
///
/// MOUNT ORDER IS PRIORITY, last wins (core/vfs/Vfs.hpp), and the command line's
/// order is honoured verbatim so a mod can be layered over stock content by naming
/// it second. `--gamedata` mounts every `.scd` it finds in name order, which is what
/// the game does absent a mod list.
///
/// This replaced extracting archives to a temporary directory. That worked, and it
/// is not what a game does: it duplicated 2.4 GiB for the two big archives, went
/// stale whenever the install was patched, and could not express a mod at all, since
/// a mod IS a layer rather than a set of files to merge.
[[nodiscard]] rm::vfs::Vfs parseContent(int argc, const char* argv[]) {
    rm::vfs::Vfs content;
    std::size_t archives = 0;

    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gamedata") {
            std::vector<std::filesystem::path> found;
            std::error_code ec;
            for (const auto& item :
                 std::filesystem::directory_iterator{std::filesystem::path{argv[i + 1]}, ec}) {
                if (item.path().extension() == ".scd" || item.path().extension() == ".sdz") {
                    found.push_back(item.path());
                }
            }
            std::ranges::sort(found);  // the iterator promises no order; a mount is priority
            for (const std::filesystem::path& archive : found) {
                if (content.mountArchive(archive)) {
                    ++archives;
                } else {
                    std::fprintf(stderr, "failed to mount %s\n", archive.string().c_str());
                }
            }
        } else if (arg == "--archive") {
            if (content.mountArchive(argv[i + 1])) {
                ++archives;
            } else {
                std::fprintf(stderr, "failed to mount archive %s\n", argv[i + 1]);
            }
        } else if (arg == "--data-dir") {
            content.mountDirectory(argv[i + 1]);
        }
    }

    if (!content.empty()) {
        std::printf("content: %zu archives, %zu files\n", archives, content.fileCount());
    }
    return content;
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

/// `--march <x> <z> <seconds>`: order every unit to a world position and run the
/// sim that long before the first frame.
///
/// Click-to-move cannot be screenshotted — there is no click in a headless
/// capture, and no way to script one here (AppleScript has no Accessibility
/// permission on this machine). This gives the same path a reproducible
/// entry point: the same orders and the same number of fixed ticks produce the
/// same scene every run, which is what makes a screenshot or a benchmark of
/// moving units worth comparing. Same rationale as `--time` for animation.
struct MarchOptions {
    /// `--march` orders every unit to (x, z) before simulating; `--play` simulates the
    /// skirmish without the blanket order, which is what a MATCH wants — the scripted
    /// armies decide their own movement, and sending both commanders to one point is a
    /// demolition derby rather than a game.
    bool orderAll = true;
    bool enabled = false;
    float x = 0.0f;
    float z = 0.0f;
    float seconds = 0.0f;

    /// Where to write this run's per-tick state hashes, or empty for nowhere.
    ///
    /// The determinism artifact. Two runs of the same invocation must produce identical
    /// files, and — once P8's Linux build exists — so must two ARCHITECTURES, which is the
    /// project's stated success criterion (PLAN2.md §1.3).
    std::string hashLogPath;

    /// A hash log to compare this run against. The check that makes the artifact useful:
    /// it reports the FIRST tick that disagrees, which is a breakpoint rather than a mood.
    std::string checkHashLogPath;
};

[[nodiscard]] MarchOptions parseMarch(int argc, const char* argv[]) {
    MarchOptions options;
    for (int i = 2; i + 3 < argc; ++i) {
        if (std::string{argv[i]} != "--march") {
            continue;
        }
        options.enabled = true;
        options.x = static_cast<float>(std::atof(argv[i + 1]));
        options.z = static_cast<float>(std::atof(argv[i + 2]));
        options.seconds = static_cast<float>(std::atof(argv[i + 3]));
        return options;
    }
    // `--play SECONDS`: the same pre-run sim, minus the blanket move order.
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string{argv[i]} != "--play") {
            continue;
        }
        options.enabled = true;
        options.orderAll = false;
        options.seconds = static_cast<float>(std::atof(argv[i + 1]));
        break;
    }

    // Both take a path and are independent of which pre-run mode is in use, so they are
    // parsed after it rather than inside either branch.
    for (int i = 2; i + 1 < argc; ++i) {
        const std::string flag{argv[i]};
        if (flag == "--hash-log") {
            options.hashLogPath = argv[i + 1];
        } else if (flag == "--check-hash-log") {
            options.checkHashLogPath = argv[i + 1];
        }
    }
    return options;
}

/// Sends one unit to a world position, routed around whatever is in the way.
///
/// Falls back to nothing rather than to a straight line when no route exists:
/// walking into a cliff because the search failed is worse than standing still,
/// and standing still is at least legible as "it cannot get there".
[[nodiscard]] bool orderRouted(rm::sim::MoveState& state, const rm::sim::Transform& unit,
                               const rm::sim::PassabilityGrid& grid, rm::sim::Fx toX,
                               rm::sim::Fx toZ) {
    // No conversion either way: the pathfinder takes and returns the same fixed point the
    // store holds. `fxPath` existed to bridge them and is gone.
    const auto path = rm::sim::findPath(grid, unit.x, unit.z, toX, toZ);
    if (path.empty()) {
        return false;
    }
    rm::sim::orderAlongPath(state, path);
    return true;
}

/// What one army has ON THE MAP, gathered for the scripted opponent's decisions —
/// and for the economy, which recomputes income from this rather than accumulating
/// it, so a structure that dies takes its production with it.
struct Standing {
    bool commanderAlive = false;
    std::array<rm::sim::Fx, 3> commanderPosition{};
    float commanderBuildRate = 0.0f;
    std::size_t extractors = 0;
    std::size_t powerGenerators = 0;
    std::size_t factories = 0;
    std::array<rm::sim::Fx, 3> factoryPosition{};
    float factoryBuildRate = 0.0f;
    std::vector<rm::sim::UnitId> tanks;
};

/// Gathers what `army` has standing, by walking the store once.
///
/// A scan, still: `UnitCensus` (P1.3) answers this in O(1) and wiring it in is P3's, where
/// the roles it keys on stop being hardcoded blueprint ids. One pass a second over a few
/// hundred units is not what this costs.
[[nodiscard]] Standing standingFor(UnitScene& scene, int army) {
    Standing standing;
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }
        if (motion[slot].armyIndex != army || !health[slot].alive()) {
            continue;
        }
        if (rm::sim::isCommanderId(def->name)) {
            standing.commanderAlive = true;
            standing.commanderPosition = rm::sim::positionOf(transforms[slot]);
            standing.commanderBuildRate = def->buildRate;
        } else if (def->name == rm::sim::kExtractorId) {
            ++standing.extractors;
        } else if (def->name == rm::sim::kPowerGeneratorId) {
            ++standing.powerGenerators;
        } else if (def->name == rm::sim::kFactoryId) {
            ++standing.factories;
            standing.factoryPosition = rm::sim::positionOf(transforms[slot]);
            standing.factoryBuildRate = def->buildRate;
        } else if (def->name == rm::sim::kTankId) {
            standing.tanks.push_back(scene.store.idAt(slot));
        }
    }
    return standing;
}

/// The nearest living enemy commander to `from`, or nothing when the war is over.
/// Where the attack wave walks: kill it and its army is defeated, which is the
/// whole win condition.
[[nodiscard]] std::optional<std::array<rm::sim::Fx, 3>> nearestEnemyCommander(
    UnitScene& scene, int army, const std::array<rm::sim::Fx, 3>& from) {
    if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) {
        return std::nullopt;
    }
    std::optional<std::array<rm::sim::Fx, 3>> best;
    rm::sim::Fx bestDistance{};
    const std::span<const rm::sim::Transform> transforms = scene.store.transforms();
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
            continue;
        }
        const int theirs = motion[slot].armyIndex;
        if (theirs < 0 || static_cast<std::size_t>(theirs) >= scene.armies.size()
            || !health[slot].alive()
            || !rm::sim::hostile(scene.armies[static_cast<std::size_t>(army)],
                                 scene.armies[static_cast<std::size_t>(theirs)])) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, rm::sim::positionOf(transforms[slot]));
        if (!best || distance < bestDistance) {
            best = rm::sim::positionOf(transforms[slot]);
            bestDistance = distance;
        }
    }
    return best;
}

/// The nearest Mass deposit to `from` that nothing has claimed — no construction
/// (finished ones stay in the list, so a standing extractor counts) within a
/// footprint of it.
[[nodiscard]] const rm::scenario::Marker* nearestFreeDeposit(
    const UnitScene& scene, std::span<const rm::scenario::Marker> markers,
    const std::array<rm::sim::Fx, 3>& from) {
    /// A deposit within this of an existing build site is the SAME deposit —
    /// half an extractor footprint. The old comment said "generous against float drift";
    /// there is no float drift here any more, and the generosity is now purely about the
    /// deposit being a point and the extractor a footprint.
    constexpr rm::sim::Fx kClaimedRadius = rm::sim::Fx::fromInt(8);

    const rm::scenario::Marker* nearest = nullptr;
    rm::sim::Fx nearestDistance{};
    for (const rm::scenario::Marker& marker : markers) {
        if (!marker.isType("Mass")) {
            continue;
        }
        bool claimed = false;
        for (const rm::sim::Construction& work : scene.building) {
            if (rm::sim::groundDistanceElmos(fxPoint(work.position),
                                             fxPoint(marker.position))
                < kClaimedRadius) {
                claimed = true;
                break;
            }
        }
        if (claimed) {
            continue;
        }
        const rm::sim::Fx distance =
            rm::sim::groundDistanceElmos(from, fxPoint(marker.position));
        if (nearest == nullptr || distance < nearestDistance) {
            nearest = &marker;
            nearestDistance = distance;
        }
    }
    return nearest;
}

/// One decision pass of the scripted opponent, for every army but the player's.
///
/// This is milestone 20's "not an AI", enacted: the pure decisions live in
/// core/sim/BuildOrder.hpp and are tested there; this function only translates them
/// into the scene — a Construction pushed, an attack order routed. Run once a
/// second rather than every tick, because nothing here changes faster than a build
/// finishes and the decisions read the whole scene.
void runOpponents(UnitScene& scene, const rm::vfs::Vfs& content, const rm::HeightField& field,
                  PassabilitySet& passability,
                  std::span<const rm::mapinfo::StartPosition> starts,
                  std::span<const rm::scenario::Marker> markers,
                  std::vector<rm::sim::Opponent>& scripts, float elapsedSeconds) {
    // The middle of the map, in fixed point: `structureSite` and `rolloffPoint` place things
    // relative to it, and both are sim geometry now.
    const rm::sim::Fx centreX = rm::sim::Fx::fromInt(field.squaresX * rm::kSquareSize / 2);
    const rm::sim::Fx centreZ = rm::sim::Fx::fromInt(field.squaresZ * rm::kSquareSize / 2);

    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy || army.defeated
            || static_cast<std::size_t>(army.index) >= scripts.size()) {
            continue;
        }
        rm::sim::Opponent& script = scripts[static_cast<std::size_t>(army.index)];
        const Standing standing = standingFor(scene, army.index);

        // What is being paid for right now, split by who builds it: the commander
        // owns structures, the factory owns tanks.
        bool structureUnderway = false;
        bool tankUnderway = false;
        for (const rm::sim::Construction& work : scene.building) {
            if (work.armyIndex != army.index || work.finished()) {
                continue;
            }
            (scene.buildable[work.blueprintIndex].isMobile() ? tankUnderway
                                                             : structureUnderway) = true;
        }

        const rm::sim::ArmyView view{
            .commanderAlive = standing.commanderAlive,
            .commanderBusy = structureUnderway,
            .factoryBusy = tankUnderway,
            .extractorsStanding = standing.extractors,
            .powerGeneratorsStanding = standing.powerGenerators,
            .factoriesStanding = standing.factories,
            .tanksAlive = standing.tanks.size(),
        };

        // The commander's next structure, placed around ITS OWN start position —
        // the base grows where the map put the army, not where the commander wandered.
        const rm::sim::StructureOrder structure = rm::sim::nextStructure(view);
        if (structure != rm::sim::StructureOrder::None) {
            std::string_view blueprint;
            std::optional<std::array<rm::sim::Fx, 3>> site;
            const std::size_t slot = standing.powerGenerators + standing.factories;
            const rm::mapinfo::StartPosition& start =
                starts[static_cast<std::size_t>(army.index)];
            const std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x),
                                                  rm::sim::Fx{},
                                                  rm::sim::fxFromFloat(start.z)};
            switch (structure) {
            case rm::sim::StructureOrder::PowerGenerator:
                blueprint = rm::sim::kPowerGeneratorBlueprint;
                site = rm::sim::structureSite(home, centreX, centreZ,
                                              static_cast<int>(slot));
                break;
            case rm::sim::StructureOrder::Factory:
                blueprint = rm::sim::kFactoryBlueprint;
                site = rm::sim::structureSite(home, centreX, centreZ,
                                              static_cast<int>(slot));
                break;
            case rm::sim::StructureOrder::Extractor: {
                blueprint = rm::sim::kExtractorBlueprint;
                const rm::scenario::Marker* deposit =
                    nearestFreeDeposit(scene, markers, standing.commanderPosition);
                if (deposit != nullptr) {
                    site = fxPoint(deposit->position);
                }
                break;
            }
            case rm::sim::StructureOrder::None:
                break;
            }
            if (site) {
                const std::optional<std::size_t> blueprintIndex =
                    resolveBuildable(scene, content, blueprint);
                if (blueprintIndex) {
                    const rm::unitdef::UnitDef& def = scene.buildable[*blueprintIndex];
                    scene.building.push_back(rm::sim::Construction{
                        .armyIndex = army.index,
                        // `Construction::position` is the caller's float triple still — see
                        // the note at the factory's construction below.
                        .position = {rm::sim::fxToFloat((*site)[0]),
                                     rm::sim::fxToFloat((*site)[1]),
                                     rm::sim::fxToFloat((*site)[2])},
                        .cost = {.mass = def.buildCostMass, .energy = def.buildCostEnergy},
                        .buildTimeRemaining = def.buildTime,
                        .totalBuildTime = def.buildTime,
                        .buildPerTick = kAppTickRate.magPerTick(standing.commanderBuildRate),
                        .blueprintIndex = *blueprintIndex,
                    });
                    std::printf("  [%6.1fs] army %d starts %.*s\n",
                                static_cast<double>(elapsedSeconds), army.index,
                                static_cast<int>(blueprint.size()), blueprint.data());
                }
            }
        }

        // The factory's next tank, built where the factory stands and rolled off it
        // once finished (the spawn handles the rolloff).
        if (rm::sim::wantsTank(view)) {
            const std::optional<std::size_t> blueprintIndex =
                resolveBuildable(scene, content, rm::sim::kTankBlueprint);
            if (blueprintIndex) {
                const rm::unitdef::UnitDef& def = scene.buildable[*blueprintIndex];
                scene.building.push_back(rm::sim::Construction{
                    .armyIndex = army.index,
                    // `Construction::position` is still a float triple: it is where the
                    // CALLER wants a thing put, and it migrates with the order system in
                    // P2.5. Converted back here rather than the field changing type, so the
                    // two migrations stay separable.
                    .position = {rm::sim::fxToFloat(standing.factoryPosition[0]),
                                 rm::sim::fxToFloat(standing.factoryPosition[1]),
                                 rm::sim::fxToFloat(standing.factoryPosition[2])},
                    .cost = {.mass = def.buildCostMass, .energy = def.buildCostEnergy},
                    .buildTimeRemaining = def.buildTime,
                    .totalBuildTime = def.buildTime,
                    .buildPerTick = kAppTickRate.magPerTick(standing.factoryBuildRate),
                    .blueprintIndex = *blueprintIndex,
                });
            }
        }

        // The one attack wave: at strength, every tank walks at the nearest enemy
        // commander. After this, reinforcements are sent as they roll off.
        if (rm::sim::launchesAttack(script, view)) {
            const std::optional<std::array<rm::sim::Fx, 3>> target =
                nearestEnemyCommander(scene, army.index, standing.commanderPosition);
            if (target) {
                script.attackLaunched = true;
                std::size_t marching = 0;
                for (const rm::sim::UnitId tank : standing.tanks) {
                    if (!scene.store.alive(tank)) {
                        continue;  // died between the census and the order
                    }
                    const auto type =
                        static_cast<std::size_t>(scene.store.typeAt(tank.index));
                    const rm::sim::PassabilityGrid& grid =
                        passability.gridFor(scene.maxSlopeDegrees[type],
                                            scene.maxWaterDepthElmos[type]);
                    if (orderRouted(scene.store.motion()[tank.index],
                                    scene.store.transforms()[tank.index], grid,
                                    (*target)[0], (*target)[2])) {
                        ++marching;
                    }
                }
                std::printf("  [%6.1fs] army %d ATTACKS with %zu of %zu tanks\n",
                            static_cast<double>(elapsedSeconds), army.index, marching,
                            standing.tanks.size());
            }
        }
    }
}

/// Orders every unit in the scene to a point, and runs the sim for a while.
///
/// `dust` collects the particles the march raised, so that a headless capture can
/// show a trail. Emitted DURING the ticks rather than at the end, which is the only
/// way to get one: dust marks where a unit has been, and a scene sampled after the
/// walk knows only where everything ended up.
// Everything one tick of a match is, from the CALLER's side.
//
// WHY THIS EXISTS. `rm::sim::tickSkirmish` owns the order the sim's own passes run in.
// This owns the order the caller's work runs in around them: the opponents decide before
// the tick, and afterwards come the two things the sim cannot do for itself — mark a
// wreck, and turn a finished construction into a unit on the map, which needs a model out
// of the VFS.
//
// It is ONE type used by both callers — the headless `--march`/`--play` pre-run and the
// windowed frame loop — because the alternative was two hand-rolled loops and they
// diverged. The windowed one ticked movement and collisions only, so a unit in the
// interactive game moved perfectly and never fired a shot, and the whole test suite stayed
// green because every rule was tested and the ASSEMBLY was not. Skirmish.cpp fixed that for
// the sim's passes; this fixes it for the caller's.
//
// What deliberately stays with each caller, because it is presentation rather than rules:
// PRINTING (the pre-run narrates the match, the frame loop draws it) and PARTICLE timing
// (the pre-run ages dust by the fixed tick, the frame loop by the frame it just drew).
// Neither changes what the match does.
//
// References rather than values, the same shape and for the same reason as
// `rm::sim::Match`: a runner is built at a call site from storage that outlives it.
struct MatchRunner {
    UnitScene& scene;
    const rm::HeightField& field;
    PassabilitySet& passability;
    const rm::vfs::Vfs& content;
    std::span<const rm::mapinfo::StartPosition> starts;
    std::span<const rm::scenario::Marker> markers;

    /// The scripted opponents' memory, one per army; a human player's slot stays unused.
    std::vector<rm::sim::Opponent> scripts;

    /// Built once and kept, because `over` has to survive between ticks — a match is
    /// decided on one tick and stays decided.
    rm::sim::Match match;

    /// Running totals, for the callers that report them at the end.
    std::size_t shotsFired = 0;
    std::size_t completedBuilds = 0;

    /// Units destroyed, accumulated from the tick's death reports.
    ///
    /// NOT counted by scanning the store at the end, which is what this used to do and what
    /// the flat store quietly broke: a corpse's slot is reused by the next spawn, so a scan
    /// for "slots whose health is zero" undercounts every death whose slot got recycled. The
    /// golden match reported 21 that way against 24 scorch marks — and the marks were right,
    /// because they are accumulated from the same reports as this. A total that only ever
    /// goes up cannot be undone by storage reusing a slot.
    std::size_t unitsDestroyed = 0;
    bool matchOver = false;
};

/// How often the scripted opponents get to think.
///
/// Once a second: nothing an opponent reacts to changes faster than a build finishes, and
/// each pass walks the whole scene. Derived from the tick rate rather than written as a
/// number of ticks, so changing the rate does not silently change how often they decide.
inline constexpr int kDecisionTicks = rm::sim::kTicksPerSecond;

[[nodiscard]] MatchRunner makeMatchRunner(UnitScene& scene, const rm::HeightField& field,
                                          PassabilitySet& passability,
                                          const rm::vfs::Vfs& content,
                                          std::span<const rm::mapinfo::StartPosition> starts,
                                          std::span<const rm::scenario::Marker> markers) {
    MatchRunner runner{
        .scene = scene,
        .field = field,
        .passability = passability,
        .content = content,
        .starts = starts,
        .markers = markers,
        .scripts = std::vector<rm::sim::Opponent>(scene.armies.size()),
        .match =
            rm::sim::Match{
                .armies = scene.armies,
                .economies = scene.economies,
                .projectiles = &scene.projectiles,
                .building = &scene.building,
                .commandersEver = scene.commandersEver,
                .baseStorage = kStartingStorage,
                // Seeded from the scene rather than defaulted to false, using the same
                // predicate the sim decides on (Skirmish.cpp): a match with one side left
                // is already over. Two cases need it, and both are announcements that
                // would otherwise be wrong:
                //
                //   `--play` pre-runs the match headless and THEN opens the window. A
                //   runner built fresh over those armies would re-detect the end on its
                //   first tick and announce a winner the pre-run already announced.
                //
                //   A `--units` crowd has no armies at all, so `survivorCount` is zero,
                //   which is also `<= 1` — without this the interactive window would
                //   declare a DRAW on tick one of every decorative scene.
                .over = scene.armies.empty()
                        || rm::sim::survivorCount(scene.armies) <= 1,
            },
    };
    scene.grewThisTick = false;
    return runner;
}

/// Advances the match by one fixed tick, and does the work the sim reports back.
///
/// `tickIndex` paces the opponents' decisions; `now` is passed through to them for their
/// own logging and reaches nothing in the sim, which counts in ticks and not in seconds.
rm::sim::TickReport advanceMatch(MatchRunner& runner, int tickIndex, float now) {
    UnitScene& scene = runner.scene;

    // The opponents decide FIRST, so an order given this tick moves this tick.
    if (!scene.armies.empty() && !runner.matchOver && tickIndex % kDecisionTicks == 0) {
        runOpponents(scene, runner.content, runner.field, runner.passability, runner.starts,
                     runner.markers, runner.scripts, now);
    }

    // ONE call, and the same one both callers make. What used to be here — the order of
    // movement, collision, aiming, firing, death, defeat and economy — is a fact about
    // core/sim/Skirmish.cpp rather than about whichever loop you are reading.
    const rm::sim::TickReport report =
        rm::sim::tickSkirmish(scene.store, scene.catalog, runner.match,
                              rm::sim::Terrain{runner.field}, kAppTickRate);

    runner.shotsFired += report.shotsFired;
    scene.deathBlasts += report.deathBlasts;
    scene.deathBlastDamage += report.deathBlastDamage;
    if (report.matchEnded) {
        runner.matchOver = true;
    }

    // The scorch each death leaves, sized from what died: a commander marks more ground
    // than a tank. Permanent, because a wreck IS the record of what happened here and a
    // battlefield that tidied itself up would lose it.
    //
    // The caller's work rather than the sim's: a decal buffer is a thing the renderer
    // uploads, and the sim has no business owning one.
    runner.unitsDestroyed += report.died.size();
    for (const rm::sim::Death& death : report.died) {
        // The decal buffer is the renderer's, so the wreck's place and size cross back into
        // floats here — the sim-to-renderer half of the boundary.
        rm::appendWreckMark(scene.wreckDecals, runner.field,
                            {rm::sim::fxToFloat(death.at[0]), rm::sim::fxToFloat(death.at[1]),
                             rm::sim::fxToFloat(death.at[2])},
                            rm::sim::fxToFloat(death.radiusElmos)
                                * rm::kWreckMarkRadiusFactor);
    }

    // What finished this tick BECOMES A UNIT: an extractor that is done stands on its
    // deposit, a factory stands by the base, and a tank rolls off the factory floor.
    //
    // The caller's work, and it has to be: a finished construction turns into a model out
    // of the VFS, which is the one thing the sim cannot do for itself.
    for (const rm::sim::Construction& work : report.finished) {
        const auto army = static_cast<std::size_t>(work.armyIndex);
        if (army >= scene.armies.size()) {
            continue;
        }
        ++runner.completedBuilds;
        // Face the map centre — a base laid out toward the fight reads as one.
        const float yaw = std::atan2(runner.field.widthElmos() * 0.5f - work.position[0],
                                     runner.field.depthElmos() * 0.5f - work.position[2]);
        const auto spawned = spawnUnit(scene, runner.content, runner.field,
                                       scene.buildablePaths[work.blueprintIndex],
                                       work.position, scene.armies[army], yaw);
        if (spawned && scene.buildable[work.blueprintIndex].isMobile()) {
            // Off the factory floor: straight to the fight once the wave has gone, to the
            // rally point outside the base while it forms.
            const auto target = runner.scripts[army].attackLaunched
                                    ? nearestEnemyCommander(scene, work.armyIndex,
                                                            fxPoint(work.position))
                                    : std::nullopt;
            const std::array<rm::sim::Fx, 2> to =
                target ? std::array<rm::sim::Fx, 2>{(*target)[0], (*target)[2]}
                       : rm::sim::rolloffPoint(
                             fxPoint(work.position),
                             rm::sim::Fx::fromInt(runner.field.squaresX * rm::kSquareSize / 2),
                             rm::sim::Fx::fromInt(runner.field.squaresZ * rm::kSquareSize
                                                  / 2));
            const auto type = static_cast<std::size_t>(scene.store.typeAt(spawned->index));
            const rm::sim::PassabilityGrid& grid =
                runner.passability.gridFor(scene.maxSlopeDegrees[type],
                                           scene.maxWaterDepthElmos[type]);
            (void)orderRouted(scene.store.motion()[spawned->index],
                              scene.store.transforms()[spawned->index], grid, to[0], to[1]);
        }
    }

    // Nothing to rebuild any more. This used to re-point a vector of per-batch spans,
    // because a spawn into an existing batch reallocated the vector it landed in and left
    // every span into it dangling — a segfault for the first reader that looked straight
    // after the call. The store hands out spans on demand instead, so a grown array is
    // simply a longer span next time somebody asks.
    scene.grewThisTick = false;

    return report;
}

void march(UnitScene& scene, const rm::HeightField& field, PassabilitySet& passability,
           const MarchOptions& options, std::span<const rm::AmbientEmitter> ambient,
           std::vector<rm::Particle>& dust, const rm::vfs::Vfs& content,
           std::span<const rm::mapinfo::StartPosition> starts,
           std::span<const rm::scenario::Marker> markers) {
    std::size_t routed = 0;
    std::size_t total = 0;
    std::vector<bool> announced(scene.armies.size(), false);
    // `--march` sends everything at one point; `--play` lets the match decide.
    if (options.orderAll) {
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            {
                ++total;
                const auto type = static_cast<std::size_t>(scene.store.typeAt(slot));
                const rm::sim::PassabilityGrid& grid = passability.gridFor(
                    scene.maxSlopeDegrees[type], scene.maxWaterDepthElmos[type]);
                if (orderRouted(scene.store.motion()[slot], scene.store.transforms()[slot],
                                grid, rm::sim::fxFromFloat(options.x),
                                rm::sim::fxFromFloat(options.z))) {
                    ++routed;
                }
            }
        }
    }

    // Whole ticks from a duration, rather than feeding a wall clock: this has
    // to land on exactly the same state every run.
    const auto ticks = static_cast<int>(options.seconds
                                        * static_cast<float>(rm::sim::kTicksPerSecond));

    // The caller-side tick, shared with the windowed frame loop. See MatchRunner.
    MatchRunner runner =
        makeMatchRunner(scene, field, passability, content, starts, markers);

    // Per-tick state hashes, kept when this run has been asked to record or check them.
    // Reserved up front so the recording cannot itself perturb what it measures by
    // reallocating mid-match.
    const bool hashing = !options.hashLogPath.empty() || !options.checkHashLogPath.empty();
    std::vector<rm::StateHash> hashes;
    if (hashing) {
        hashes.reserve(static_cast<std::size_t>(ticks));
    }

    const float kTickSeconds = kAppTickRate.secondsPerTick();
    float dustDebt = 0.0f;
    float ambientDebt = 0.0f;
    std::uint32_t dustSeed = 0x51ED27u;
    std::vector<rm::DustEmitter> emitters;

    for (int i = 0; i < ticks; ++i) {
        const float now = static_cast<float>(i) * kTickSeconds;

        // ONE call, and the same one the windowed frame loop makes: the opponents'
        // decisions, the sim's tick, the wrecks, and the finished builds that become
        // units. See MatchRunner for what is deliberately left to each caller.
        const rm::sim::TickReport report = advanceMatch(runner, i, now);

        // Fingerprinted after the WHOLE caller-side tick, which deliberately includes the
        // units a finished construction spawned: what got built is part of what the match
        // did, and a hash that stopped at the sim's own passes would agree about two runs
        // that built different things. The renderer's side stays out on its own — the decal
        // buffer is not reachable from a group or a match, so `hashMatch` cannot see it.
        if (hashing) {
            hashes.push_back(rm::sim::hashMatch(scene.store, runner.match));
        }

        if (!scene.armies.empty()) {

            // What the tick decided, announced. The decisions themselves are the sim's;
            // saying them out loud is this run's, which is why only the printing is left.
            for (const rm::sim::Army& army : scene.armies) {
                if (army.defeated && !announced[static_cast<std::size_t>(army.index)]) {
                    announced[static_cast<std::size_t>(army.index)] = true;
                    std::printf("  [%6.1fs] army %d (%s) has lost its commander\n",
                                static_cast<double>(now), army.index,
                                std::string{rm::sim::factionName(army.faction)}.c_str());
                }
            }
            if (report.matchEnded) {
                if (report.winner) {
                    std::printf("  [%6.1fs] team %d WINS\n", static_cast<double>(now),
                                *report.winner);
                } else {
                    std::printf("  [%6.1fs] a DRAW: every army lost its commander\n",
                                static_cast<double>(now));
                }
            }
        }

        // What finished this tick, narrated. The spawning itself is `advanceMatch`'s, so
        // that the windowed loop gets the buildings too; only saying so is this run's.
        for (const rm::sim::Construction& work : report.finished) {
            const auto army = static_cast<std::size_t>(work.armyIndex);
            if (army >= scene.armies.size()) {
                continue;
            }
            std::printf("  [%6.1fs] army %zu completes %s\n", static_cast<double>(now), army,
                        scene.buildable[work.blueprintIndex].name.c_str());
        }

        // Dust as the walk happens, aged as the walk continues, so what a capture
        // shows is a trail rather than a puff at everyone's feet.
        emitters.clear();
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::sim::MoveState& motion = scene.store.motion()[slot];
            emitters.push_back(rm::DustEmitter{
                .position = {rm::sim::fxToFloat(scene.store.transforms()[slot].x),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].y),
                             rm::sim::fxToFloat(scene.store.transforms()[slot].z)},
                .moving = motion.moving,
                .topSpeedElmosPerSecond = rm::sim::fxToFloat(motion.speedPerTick)
                                          * static_cast<float>(
                                              kAppTickRate.ticksPerSecond()),
                .radiusElmos = rm::sim::fxToFloat(motion.radiusElmos),
            });
        }
        rm::advanceParticles(dust, kTickSeconds);
        rm::emitDust(dust, emitters, field, kTickSeconds, dustDebt, dustSeed);
        rm::emitAmbient(dust, ambient, kTickSeconds, ambientDebt, dustSeed);
    }

    // A marched scene has been walked, so its walk cycles are paced by the
    // ground covered — the same rule the windowed path follows. This is also
    // what makes such a screenshot independent of `--time`: the sim decided
    // where the legs are, not the clock.
    // The walk cycles, paced from the store, then gathered for drawing. Per TYPE because the
    // animation belongs to the model: every unit of one type shares its clock length, and the
    // pass writes each unit's own phase from the ground it has covered.
    scene.gatherForDrawing();

    // --- Determinism ---------------------------------------------------------
    //
    // Written before the summaries below, so that a run whose whole purpose was the
    // artifact says whether it got one before saying anything else.
    if (hashing) {
        rm::sim::ReplayHeader header;
        header.ticksPerSecond = rm::sim::kTicksPerSecond;
        header.widthsFingerprint = rm::sim::widthsFingerprint();
        header.tickCount = hashes.size();

        if (!options.checkHashLogPath.empty()) {
            const auto recorded = rm::sim::readHashLog(options.checkHashLogPath);
            if (!recorded) {
                std::printf("determinism: cannot read %s — %s\n",
                            options.checkHashLogPath.c_str(),
                            recorded.error().message.c_str());
            } else {
                const rm::sim::Divergence heads =
                    rm::sim::compareHeaders(recorded->header, header);
                if (heads.incomparable) {
                    std::printf("determinism: NOT COMPARABLE — %s\n", heads.why.c_str());
                } else {
                    const rm::sim::Divergence d =
                        rm::sim::compareHashes(recorded->hashes, hashes);
                    if (!d.diverged) {
                        std::printf("determinism: MATCH — %zu ticks identical to %s\n",
                                    hashes.size(), options.checkHashLogPath.c_str());
                    } else {
                        // The first divergent tick, which is the only one with diagnostic
                        // value: every later tick is downstream of it.
                        std::printf("determinism: DIVERGED at tick %llu —"
                                    " recorded %016llx, this run %016llx\n",
                                    static_cast<unsigned long long>(d.tick),
                                    static_cast<unsigned long long>(d.recorded),
                                    static_cast<unsigned long long>(d.replayed));
                        if (!d.why.empty()) {
                            std::printf("            %s\n", d.why.c_str());
                        }
                        std::printf("            re-run with --hash-log to capture this"
                                    " run, then diff the two files at that line\n");
                    }
                }
            }
        }

        if (!options.hashLogPath.empty()) {
            const auto written =
                rm::sim::writeHashLog(options.hashLogPath, header, hashes);
            if (written) {
                std::printf("determinism: %zu tick hashes written to %s\n", hashes.size(),
                            options.hashLogPath.c_str());
            } else {
                std::printf("determinism: cannot write %s — %s\n",
                            options.hashLogPath.c_str(), written.error().message.c_str());
            }
        }
    }

    // Saying how many found a route matters on a map like aw04, where most
    // units are on islands: a unit that cannot walk there stays put, and
    // without this line that reads as the sim being broken.
    if (options.orderAll) {
        std::printf("march: %zu of %zu units routed to (%.0f, %.0f), %d ticks simulated,"
                    " %zu dust particles still in the air\n",
                    routed, total, static_cast<double>(options.x),
                    static_cast<double>(options.z), ticks, dust.size());
    } else {
        std::printf("play: %d ticks simulated, %zu dust particles still in the air\n", ticks,
                    dust.size());
    }

    if (!scene.armies.empty()) {
        // What the fight came to. Reported rather than inferred from a screenshot,
        // because a unit that died is a unit that is no longer in the frame and its
        // absence looks the same as its never having been there.
        rm::sim::Mag remaining{};
        rm::sim::Mag maximum{};
        for (const rm::sim::Health& one : scene.store.health()) {
            remaining += one.current;
            maximum += one.maximum;
        }

        std::printf("combat: %zu shots fired, %zu in flight, %zu unit(s) destroyed,"
                    " %.0f of %.0f hp left\n",
                    runner.shotsFired, scene.projectiles.size(), runner.unitsDestroyed,
                    static_cast<double>(rm::sim::magToFloat(remaining)),
                    static_cast<double>(rm::sim::magToFloat(maximum)));

        // Each commander's state, because the match hangs on exactly these numbers
        // and "the fight is still on" and "the fight never reached anyone" read the
        // same from the aggregate.
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
            if (def == nullptr || !rm::sim::isCommanderId(def->name)) {
                continue;
            }
            std::printf("  army %d commander: %.0f of %.0f hp\n",
                        scene.store.motion()[slot].armyIndex,
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].current)),
                        static_cast<double>(
                            rm::sim::magToFloat(scene.store.health()[slot].maximum)));
        }
        if (scene.deathBlasts > 0 || !scene.wreckDecals.empty()) {
            std::printf("wreckage: %zu scorch mark(s), %zu death explosion(s) dealing"
                        " %.0f damage\n",
                        scene.wreckDecals.size() / rm::wreckVertexCount(), scene.deathBlasts,
                        static_cast<double>(rm::sim::magToFloat(scene.deathBlastDamage)));
        }
    }

    if (!scene.economies.empty()) {
        const rm::sim::Economy& first = scene.economies.front();
        std::printf("economy: %zu of %zu build(s) complete; army 0 holds %.0f mass /"
                    " %.0f energy, earning %.1f / %.1f a second against %.1f energy of"
                    " upkeep, %.0f%% funded\n",
                    runner.completedBuilds, scene.building.size(),
                    static_cast<double>(rm::sim::magToFloat(first.stored.mass)),
                    static_cast<double>(rm::sim::magToFloat(first.stored.energy)),
                    // Reported PER SECOND, which is what a reader wants, converted back from
                    // the per-tick figure the sim keeps.
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.mass))
                        * kAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.incomePerTick.energy))
                        * kAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::magToFloat(first.upkeepPerTick.energy))
                        * kAppTickRate.ticksPerSecond(),
                    static_cast<double>(rm::sim::fxToFloat(first.fundedFraction)) * 100.0);
    }
}

/// How far back `--focus` should sit, in model radii, or 0 when it was not
/// given at all.
///
/// 2.5 radii fills the frame with one unit, which is what the flag was for —
/// checking a model. A squad, or a ring around each of several units, needs to
/// see further out, so the distance takes an optional argument rather than
/// forcing a choice between one unit and the whole map.
[[nodiscard]] float parseFocus(int argc, const char* argv[]) {
    constexpr float kDefaultRadiiBack = 2.5f;

    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} != "--focus") {
            continue;
        }
        if (i + 1 >= argc) {
            return kDefaultRadiiBack;
        }
        char* end = nullptr;
        const double radii = std::strtod(argv[i + 1], &end);
        // Only a bare number counts as the argument; the next flag does not.
        if (end != argv[i + 1] && *end == '\0' && radii > 0.0) {
            return static_cast<float>(radii);
        }
        return kDefaultRadiiBack;
    }
    return 0.0f;
}

/// The unsigned count following a flag, or 0 when the flag is absent or its
/// argument is not a number.
[[nodiscard]] std::size_t parseCount(int argc, const char* argv[], std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != flag) {
            continue;
        }
        char* end = nullptr;
        const unsigned long value = std::strtoul(argv[i + 1], &end, 10);
        if (end != argv[i + 1] && *end == '\0') {
            return static_cast<std::size_t>(value);
        }
    }
    return 0;
}

/// Whether a bare flag appears anywhere in the arguments.
[[nodiscard]] bool hasFlag(int argc, const char* argv[], std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) {
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
    // Set after the colour map, not instead of it: the terrain-type bands stay
    // loaded as the fallback the shader uses whenever the splat is off, so a map
    // whose layer textures are missing still draws something map-shaped.
    if (!map.splat.empty()) {
        target.setSplat(map.splat, map.splatMaskA, map.splatMaskB);
    }
    target.setWater(map.hasWater, map.waterLevel);
    if (map.environment) {
        target.setEnvironment(*map.environment);
    }
}

/// Points the camera at the first instance of the first model.
///
/// Framing the whole map makes a unit about two pixels across, which says
/// nothing about whether it is textured correctly. Templated on the target
/// because Window and Renderer both offer focusOn and neither shares a base
/// class — a one-method interface would be ceremony for two call sites.
template <typename Target>
void focusOnFirstUnit(Target& target, const UnitScene& scene, float radiiBack) {
    if (scene.batches.empty() || scene.batches.front().instances.empty()
        || scene.batches.front().model == nullptr) {
        return;
    }

    const rm::UnitBatch& batch = scene.batches.front();
    const rm::UnitInstance& first = batch.instances.front();
    target.focusOn(first.position,
                   std::max(batch.model->radius, 1.0f) * radiiBack * first.scale);
}

/// `--look X Z RADIUS`: aim a capture at a world point, framed to show RADIUS elmos
/// around it. A match's stages happen at one base or the other, and neither is the
/// first unit `--focus` knows how to find.
struct LookOptions {
    bool enabled = false;
    float x = 0.0f;
    float z = 0.0f;
    float radiusElmos = 300.0f;
};

[[nodiscard]] LookOptions parseLook(int argc, const char* argv[]) {
    LookOptions options;
    for (int i = 2; i + 3 < argc; ++i) {
        if (std::string{argv[i]} != "--look") {
            continue;
        }
        options.enabled = true;
        options.x = static_cast<float>(std::atof(argv[i + 1]));
        options.z = static_cast<float>(std::atof(argv[i + 2]));
        options.radiusElmos = static_cast<float>(std::atof(argv[i + 3]));
        break;
    }
    return options;
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

namespace {

/// Reads the scene into the state the interface reports.
///
/// A translation step rather than the interface reaching into the scene, so `ui::build` takes
/// a small struct it can be tested against and knows nothing about batches, health arrays or
/// deques.
[[nodiscard]] rm::ui::MatchState hudStateFrom(const UnitScene& scene, float elapsedSeconds) {
    rm::ui::MatchState state;
    state.elapsedSeconds = elapsedSeconds;
    state.armiesTotal = scene.armies.size();
    state.armiesLeft = rm::sim::survivorCount(scene.armies);

    for (const rm::sim::Health& one : scene.store.health()) {
        if (one.alive()) {
            ++state.unitsAlive;
        }
    }

    if (scene.playerArmy != rm::sim::kNoArmy
        && static_cast<std::size_t>(scene.playerArmy) < scene.economies.size()) {
        const rm::sim::Economy& mine =
            scene.economies[static_cast<std::size_t>(scene.playerArmy)];

        // The HUD reads in floats and per SECOND — it is a display, and a player thinks in
        // seconds. This is the sim-to-renderer half of the float boundary (`Fx.hpp`).
        const auto perSecond = [](rm::sim::Mag perTick) {
            return rm::sim::magToFloat(perTick)
                   * static_cast<float>(kAppTickRate.ticksPerSecond());
        };
        state.mass = rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.mass),
                                   .capacity = rm::sim::magToFloat(mine.storage.mass),
                                   .incomePerSecond = perSecond(mine.incomePerTick.mass),
                                   .drainPerSecond = perSecond(mine.upkeepPerTick.mass)};
        state.energy = rm::ui::Gauge{.stored = rm::sim::magToFloat(mine.stored.energy),
                                     .capacity = rm::sim::magToFloat(mine.storage.energy),
                                     .incomePerSecond = perSecond(mine.incomePerTick.energy),
                                     .drainPerSecond = perSecond(mine.upkeepPerTick.energy)};
        state.fundedFraction = rm::sim::fxToFloat(mine.fundedFraction);
    }

    if (!scene.armies.empty() && state.armiesLeft <= 1) {
        const std::optional<int> winner = rm::sim::winningTeam(scene.armies);
        state.outcome = winner ? rm::ui::MatchState::Outcome::Win
                               : rm::ui::MatchState::Outcome::Draw;
        state.winningTeam = winner.value_or(0);
    }
    return state;
}

/// The livery the interface wears: the player's own faction, or the neutral cyan when the scene
/// has no armies in it.
[[nodiscard]] rm::ui::Theme hudThemeFor(const UnitScene& scene) {
    for (const rm::sim::Army& army : scene.armies) {
        if (army.index == scene.playerArmy) {
            return rm::ui::themeFor(army.faction);
        }
    }
    return rm::ui::neutralTheme();
}

/// Appends an icon for every unit in the scene too small to read, at the camera's current
/// zoom. See core/scene/UnitIcons.hpp.
///
/// Shared by the windowed loop and the capture paths, because a screenshot that did not show
/// the icons would make the feature unverifiable — and AGENT.md asks for a screenshot or it
/// did not happen.
void appendSceneIcons(std::vector<rm::Particle>& into, const UnitScene& scene,
                      const rm::OrbitCamera& camera) {
    const float elmosPerPoint = camera.elmosPerPoint(rm::kIconReferenceHeightPoints);
    std::vector<float> radii;
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        radii.clear();
        radii.reserve(scene.drawSlotOf[batch].size());
        for (const rm::UnitIndex slot : scene.drawSlotOf[batch]) {
            radii.push_back(rm::sim::fxToFloat(scene.store.motion()[slot].radiusElmos));
        }
        (void)rm::appendUnitIcons(into, scene.drawScratch[batch], radii, elmosPerPoint);
    }
}

/// The unit nearest the ray across every batch, IGNORING who owns it.
///
/// What a right-click wants: an order aimed at an enemy has to be able to find one, and
/// the selection pick deliberately cannot.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAnyBatch(const rm::Ray& ray,
                                                          const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    // Against what is DRAWN, because that is what the ray can see — a dead unit is not in
    // the gather and so cannot be clicked. The hit comes back as a (batch, index) pair and
    // is turned into a handle, which is what everything downstream now speaks.
    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }
        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = scene.unitDrawnAt(batch, *hit);
    }
    return best;
}

/// Whether `army` may shoot what `entry` points at.
[[nodiscard]] bool hostileTo(const UnitScene& scene, int army, rm::sim::UnitId id) {
    const int theirs = scene.armyOf(id.index);
    if (army == rm::sim::kNoArmy || theirs == rm::sim::kNoArmy) {
        return false;
    }
    const auto find = [&scene](int index) -> const rm::sim::Army* {
        for (const rm::sim::Army& candidate : scene.armies) {
            if (candidate.index == index) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const rm::sim::Army* mine = find(army);
    const rm::sim::Army* target = find(theirs);
    return mine != nullptr && target != nullptr && rm::sim::hostile(*mine, *target);
}

/// The unit nearest the ray across every batch, or nothing.
///
/// pickUnit searches one array at a time because that is how the instances are
/// held — one per model. Comparing its winners across batches is what makes the
/// nearest unit on SCREEN win, rather than the nearest one in whichever model
/// happened to load first.
[[nodiscard]] std::optional<rm::sim::UnitId> pickAcrossBatches(const rm::Ray& ray,
                                                              const UnitScene& scene) {
    std::optional<rm::sim::UnitId> best;
    float bestDistance = rm::kDefaultPickRadiusElmos;

    for (std::size_t batch = 0; batch < scene.drawScratch.size(); ++batch) {
        const std::vector<rm::UnitInstance>& instances = scene.drawScratch[batch];
        const std::optional<std::size_t> hit = rm::pickUnit(ray, instances, bestDistance);
        if (!hit) {
            continue;
        }

        // A unit belongs to whoever the sim says, and only the player's own may be
        // selected. Filtered HERE rather than inside pickUnit, which is pure over
        // instances and has no business knowing about armies.
        //
        // Note what this deliberately does NOT do: it does not skip an enemy and keep
        // looking behind it. Clicking an enemy selects nothing, because that is what
        // was clicked — the alternative reaches through the thing under the cursor to
        // grab something else, which is worse than a null answer.
        const std::optional<rm::sim::UnitId> id = scene.unitDrawnAt(batch, *hit);
        if (!id) {
            continue;
        }
        if (scene.playerArmy != rm::sim::kNoArmy
            && scene.armyOf(id->index) != scene.playerArmy) {
            continue;
        }

        const rm::UnitInstance& unit = instances[*hit];
        bestDistance = rm::distanceToRay(
            ray, simd_make_float3(unit.position[0], unit.position[1], unit.position[2]));
        best = id;
    }

    return best;
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        // The content is mounted BEFORE the map, because the map's ground layers
        // and props are content too — a `.scmap` names its strata as VFS paths and
        // has always needed somewhere to look them up.
        const rm::vfs::Vfs content = parseContent(argc, argv);

        const auto map = resolveMap(argc, argv, content);
        if (!map) {
            return 1;
        }

        const std::vector<UnitOptions> unitRequests = parseUnits(argc, argv);
        const rm::vfs::AssetSearch assetSearch = parseAssetSearch(argc, argv);
        const float focus = parseFocus(argc, argv);
        const float animationTime = parseAnimationTime(argc, argv);
        // Land above the water, whatever the map calls water: Recoil's plane is
        // always y = 0, Supreme Commander's is per map and 140 elmos on most.
        // Scattering above 0 on a FA map drowns most of the units.
        // Not const: the windowed path steps this scene every frame.
        UnitScene units = resolveUnits(unitRequests, map->field, map->starts,
                                       map->hasWater ? map->waterLevel : 0.0f, assetSearch,
                                       content);

        // `--skirmish`: one commander per start position, each its own army. Appended
        // to whatever `--units` asked for rather than replacing it, so a scene can
        // hold both a match and a crowd of test units. `--armies N` takes the first N
        // start positions instead of all of them — a stock map declares up to eight,
        // and the match milestone 20 describes is a duel.
        std::span<const rm::mapinfo::StartPosition> starts{map->starts};
        const std::size_t armiesCap = parseCount(argc, argv, "--armies");
        if (armiesCap > 0 && armiesCap < starts.size()) {
            starts = starts.first(armiesCap);
        }
        if (hasFlag(argc, argv, "--skirmish")) {
            spawnCommanders(units, map->field, starts, content);
            orderFirstExtractors(units, map->markers, content);
        }

        // Tilt every unit onto its slope once, here, because the headless paths
        // never tick the sim: a screenshot of a scattered scene would otherwise
        // show every unit standing horizontally on its hillside.
        const rm::sim::Terrain terrain{map->field};
        for (rm::sim::Transform& unit : units.store.transforms()) {
            const std::array<rm::Brad, 2> align =
                rm::sim::slopeAlignment(terrain, unit.x, unit.z, unit.heading);
            unit.pitch = align[0];
            unit.roll = align[1];
        }
        units.gatherForDrawing();

        // Before anything is uploaded: setUnits seeds every ring slot from the
        // instances as they stand, so a marched scene is correct even on the
        // paths that never push an instance update.
        // Where a ground unit may stand. One grid per distinct pair of limits,
        // built on first use: the terrain and water level never change, and a
        // unit's own maxslope/maxwaterdepth decide which map it sees.
        PassabilitySet passability{map->field, map->hasWater ? map->waterLevel : 0.0f};

        // --- Quality settings ----------------------------------------------
        // The file first, then the command line over the top: a flag is somebody
        // asking for this run, a file is somebody stating a preference, and the
        // more specific of the two wins. There is no --reflections to turn one
        // back ON, deliberately — the defaults are already on, so the only thing
        // a flag has ever needed to express is "not this time".
        std::vector<std::string> settingsProblems;
        rm::Settings settings = rm::loadSettings(rm::settingsPath(), settingsProblems);
        for (const std::string& problem : settingsProblems) {
            std::fprintf(stderr, "%s\n", problem.c_str());
        }
        if (hasFlag(argc, argv, "--no-reflections")) {
            settings.reflections = false;
        }
        if (hasFlag(argc, argv, "--no-stratum-normals")) {
            settings.stratumNormals = false;
        }
        if (hasFlag(argc, argv, "--no-props")) {
            settings.props = false;
        }
        if (hasFlag(argc, argv, "--no-refraction")) {
            settings.refraction = false;
        }
        // The only switch with a positive flag, because it is the only one whose
        // default is off — "not this time" is not the useful thing to say about it.
        if (hasFlag(argc, argv, "--refraction")) {
            settings.refraction = true;
        }

        // The map's own scenery: trees, rocks, wrecks. Loaded once here and
        // shared by all three modes below, because it is the same scene however
        // it gets to the screen.
        //
        // Only a .scmap has any — a Recoil .smf carries a feature list too, but
        // BAR's own maps declare it empty and place their objects through a
        // runtime Lua gadget, so there is nothing there to read yet.
        const char* propHome = std::getenv("HOME");
        // Not loaded at all when switched off, rather than loaded and hidden: the
        // meshes and textures are the cost, and a run that has decided against
        // scenery should not pay it. The live `p` toggle therefore only reaches
        // what a run did load — which is the honest behaviour for a switch whose
        // purpose is comparing two frames.
        const PropScene props =
            settings.props ? loadProps(*map,
                                       std::filesystem::path{propHome == nullptr ? "" : propHome}
                                           / kFaEnvDir,
                                       content)
                           : PropScene{};

        // Held here rather than inside march(): the capture path below uploads
        // them, and the windowed path raises its own as the sim runs.
        std::vector<rm::Particle> marchDust;

        const MarchOptions marchOptions = parseMarch(argc, argv);
        if (marchOptions.enabled) {
            march(units, map->field, passability, marchOptions, props.ambient, marchDust,
                  content, starts, map->markers);
        }

        // Full detail up to the vertex budget, halved per doubling beyond it —
        // which is what lets a 4096-square map load at all.
        const int stride = rm::chooseStride(map->field);
        const rm::TerrainMesh mesh = rm::buildTerrainMesh(map->field, stride);
        std::printf("terrain: %zu vertices, %zu triangles, height %.1f..%.1f elmos",
                    mesh.vertices.size(), mesh.triangleCount(),
                    static_cast<double>(mesh.minY), static_cast<double>(mesh.maxY));
        if (stride > 1) {
            std::printf(" (every %dth sample: %d squares exceeds the %d-vertex budget)",
                        stride, map->field.squaresX, rm::kMaxVerticesPerSide);
        }
        std::printf("\n");

        std::size_t propInstances = 0;
        for (const rm::PropBatch& batch : props.batches) {
            propInstances += batch.instances.size();
        }

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
            renderer.setProps(props.textures.all(), props.batches);
            renderer.setAnimationTime(animationTime);
            renderer.setReflections(settings.reflections);
            renderer.setStratumNormals(settings.stratumNormals);
            renderer.setRefraction(settings.refraction);
            // The march's dust, so a benchmark measures the same scene a capture
            // shows rather than one without particles in it — plus the icons, for the
            // units the camera has left too small to read.
            std::vector<rm::Particle> captureParticles;
            captureParticles.assign(marchDust.begin(), marchDust.end());
            appendSceneIcons(captureParticles, units, renderer.camera());
            renderer.setParticles(captureParticles);
            // --focus works here too, so the benchmark can measure a close
            // camera as well as a whole-map one. They are different workloads:
            // anything that culls to what the camera sees is invisible at full
            // zoom and everything up close.
            if (focus > 0.0f) {
                focusOnFirstUnit(renderer, units, focus);
            }

            std::printf("offscreen benchmark: %ux%u, %zu frames (discarding %zu warmup),"
                        " %zu frames in flight, no vsync\n",
                        bench.width, bench.height, bench.frames, bench.warmup,
                        rm::Renderer::kMaxFramesInFlight);
            std::printf("  %s\n", describeQuality(settings, propInstances, marchDust.size()).c_str());

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
            renderer.setProps(props.textures.all(), props.batches);
            renderer.setAnimationTime(animationTime);
            renderer.setReflections(settings.reflections);
            renderer.setStratumNormals(settings.stratumNormals);
            renderer.setRefraction(settings.refraction);
            if (focus > 0.0f) {
                focusOnFirstUnit(renderer, units, focus);
            }
            const LookOptions look = parseLook(argc, argv);
            if (look.enabled) {
                renderer.focusOn({look.x, map->field.heightAtWorld(look.x, look.z), look.z},
                                 look.radiusElmos);
            }

            // Selection rings need a selection, and a headless run has no
            // clicks. `--select N` rings the first N units so that what a
            // click produces can be captured and compared between builds —
            // otherwise the one piece of interface this renderer draws is the
            // one thing no screenshot can show.
            // The ground decals for a capture. WRECKS FIRST and unconditionally, because
            // they are a fact about the battlefield rather than a piece of interface — the
            // rest of this block used to be gated on `--select`, which meant a scorch mark
            // only appeared in a screenshot that also happened to be ringing units.
            std::vector<rm::DecalVertex> vertices{units.wreckDecals.begin(),
                                                  units.wreckDecals.end()};
            {
                const std::size_t rings = parseCount(argc, argv, "--select");
                std::vector<rm::SelectionEntry> captured;
                std::size_t made = 0;
                for (std::size_t batch = 0; batch < units.drawScratch.size() && made < rings;
                     ++batch) {
                    for (std::size_t i = 0; i < units.drawScratch[batch].size() && made < rings;
                         ++i, ++made) {
                        const rm::UnitIndex slot = units.drawSlotOf[batch][i];
                        const rm::sim::Transform& at = units.store.transforms()[slot];
                        rm::appendSelectionRing(
                            vertices, map->field,
                            {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                             rm::sim::fxToFloat(at.z)},
                            rm::sim::fxToFloat(units.store.motion()[slot].radiusElmos)
                                * kSelectionRingMargin,
                            kSelectionRingColour);
                        captured.push_back(rm::SelectionEntry{batch, i});
                    }
                }
                // No beginFrame: that acquires a frames-in-flight slot which
                // only drawFrame releases, and a capture goes through
                // renderToImage instead. Without it the slot stays 0, which is
                // the same slot encodeScene will read — consistent, because
                // nothing here is pipelined.
                // An order marker too, where --march sent them. A right-click
                // cannot be screenshotted any more than a left-click can, and
                // --march already names the destination, so the flag that orders
                // the move is also the one that marks it.
                if (marchOptions.enabled && marchOptions.orderAll) {
                    rm::appendOrderMarker(vertices, map->field,
                                          {{marchOptions.x, 0.0f, marchOptions.z}},
                                          kOrderMarkerColour, /*age=*/0.0f);
                }

                renderer.setGroundDecals(vertices);
                renderer.setSelection(captured);
                std::printf("  selected %zu units for the capture\n", made);
            }

            // The dust the march raised, if there was one. Nothing else in a
            // headless capture can produce a particle: dust records movement, and
            // only --march moves anything without a window.
            // The march's dust, plus icons for whatever the camera has left too small to
            // read. The SCREENSHOT path, which is the one that has to show them: a feature
            // only visible in a live window cannot be verified, and AGENT.md asks for a
            // screenshot or it did not happen.
            std::vector<rm::Particle> shotParticles{marchDust.begin(), marchDust.end()};
            appendSceneIcons(shotParticles, units, renderer.camera());
            renderer.setParticles(shotParticles);

            // The HUD in a capture too. A screenshot is how this project verifies anything,
            // and an interface only visible in a live window cannot be checked at all.
            rm::ui::Geometry hud;
            rm::ui::build(hud, renderer.labelFont(), renderer.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, marchOptions.seconds),
                          static_cast<float>(shot.width),
                          static_cast<float>(shot.height));
            renderer.setHud(hud.label, hud.readout);

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
        rm::Window window{1280, 720, "recoil-metal — m8: movable units"};
        window.setTerrain(mesh);
        applyGround(window, *map);
        window.setProps(props.textures.all(), props.batches);

        // Only the windowed path steps the sim, so only it can pace a walk
        // cycle by distance. The headless paths — screenshots and benchmarks —
        // stay on the renderer's clock, which is what makes `--time` mean
        // something there and keeps a captured frame reproducible.
        for (rm::UnitBatch& batch : units.batches) {
            batch.animationDrivenByInstance = true;
        }
        window.setUnits(units.textures.all(), units.batches);
        if (focus > 0.0f) {
            focusOnFirstUnit(window, units, focus);
        }

        // The planar reflection is half the frame for something Fresnel largely
        // hides at this camera angle, so it is a setting rather than a fact.
        // `r` flips it live, which is the only way to judge whether it is worth
        // its cost — a side-by-side of two runs cannot show the difference
        // moving.
        window.setReflections(settings.reflections);
        window.setStratumNormals(settings.stratumNormals);
        window.setRefraction(settings.refraction);
        window.onKey([&window](char key) {
            if (key == 'r') {
                const bool enabled = !window.reflectionsEnabled();
                window.setReflections(enabled);
                std::printf("reflections %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'n') {
                const bool enabled = !window.stratumNormalsEnabled();
                window.setStratumNormals(enabled);
                std::printf("stratum normals %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'f') {
                const bool enabled = !window.refractionEnabled();
                window.setRefraction(enabled);
                std::printf("water refraction %s\n", enabled ? "on" : "off");
                std::fflush(stdout);
            } else if (key == 'p') {
                const bool visible = !window.propsVisible();
                window.setPropsVisible(visible);
                std::printf("props %s\n", visible ? "on" : "off");
                std::fflush(stdout);
            }
        });

        // --- Click to move -------------------------------------------------
        // Left selects the unit under the cursor, right orders the selection to
        // the ground under it. Both arrive as a world ray; what it hit is
        // decided here, where the map and the units are.
        //
        // Captured by reference: everything named outlives the window, which is
        // destroyed at the end of this scope before any of them.
        // Which units the user has selected. Shift- or Command-click adds or
        // removes one; a plain click replaces the set with the unit under the
        // cursor. Right-click orders every selected unit to the ground under it.
        //
        // Identity only — nothing about a selected unit is drawn differently, so
        // there is nothing per-unit to remember and put back. The rings are
        // rebuilt from this list every frame.
        // Handles, not (batch, instance) pairs: a selection has to survive a unit dying
        // and the gather renumbering what is drawn. Converted to `SelectionEntry` at draw
        // time, which is the only place the pair means anything.
        std::vector<rm::sim::UnitId> selected;
        std::vector<rm::SelectionEntry> selectionScratch;
        rm::sim::TickClock clock;

        // The caller-side tick, the same one `march()` drives. Built here rather than in
        // the frame callback because a match is decided on one tick and stays decided, and
        // the opponents remember what they have already started.
        MatchRunner runner =
            makeMatchRunner(units, map->field, passability, content, starts, map->markers);
        // Match time in TICKS, for pacing the opponents' decisions. Counted rather than
        // read off `matchSeconds`, so a dropped frame cannot skip a decision or run one
        // twice.
        //
        // Seeded from the pre-run, because `--play` simulates N seconds headless before
        // the window opens and the window then continues the SAME match: an opponent
        // announcing its attack at 0.0s in a match already 500 seconds old reads as a bug
        // in the opponent. Only the logging sees this number — the sim counts its own
        // ticks.
        int matchTicks =
            marchOptions.enabled
                ? static_cast<int>(marchOptions.seconds
                                   * static_cast<float>(rm::sim::kTicksPerSecond))
                : 0;

        // Where the last few orders landed, and when. Markers expire on their own
        // (GroundDecals.hpp), so this only ever grows to the number of orders
        // given inside kOrderMarkerSecondsToLive — a handful at a click a second,
        // and bounded below by nothing needing to be cleaned up on a schedule.
        struct OrderMark {
            std::array<float, 3> position{};
            float age = 0.0f;
        };
        std::vector<OrderMark> orderMarks;

        // Dust. Particles live across frames — that is the point of them — so
        // this list persists and is aged rather than rebuilt, unlike the decals.
        // The emitter list IS rebuilt each frame, because it is a view of where
        // the units are now.
        std::vector<rm::Particle> particles;
        std::vector<rm::DustEmitter> dustEmitters;
        float dustDebt = 0.0f;
        float ambientDebt = 0.0f;
        // A fixed seed, so a scene is the same every run: the same reason the unit
        // scatter takes one.
        std::uint32_t dustSeed = 0x51ED27u;

        // Reused across frames so that rebuilding the rings costs no
        // allocation — the capacity settles after the first large selection.
        std::vector<rm::DecalVertex> decalVertices;

        window.onClick([&](const rm::Ray& ray, rm::MouseButton button,
                           rm::MouseModifiers mods) {
            if (button == rm::MouseButton::Left) {
                // What the click MEANS is decided in core/scene/Selection.hpp,
                // where it can be tested. Nothing is left to do here: the units
                // themselves are not repainted, and the rings are rebuilt from
                // this list by the frame callback.
                const bool addToSet = mods.shift || mods.command || mods.control;
                selected = rm::applyClick<rm::sim::UnitId>(
                    selected, pickAcrossBatches(ray, units), addToSet);
                return;
            }

            if (selected.empty()) {
                return;  // an order with nothing selected is not an error
            }

            // AN ATTACK OR A MOVE, decided by what the right button landed on. Picking is
            // done WITHOUT the army filter that selection uses — the point here is to find
            // an enemy, which is exactly what selection excludes.
            //
            // What this is not: a tracked attack order. The selection is sent to where the
            // target IS, and the automatic targeting does the shooting once in range. A
            // target that walks away is therefore not chased, which is the honest
            // difference between "attack move" and "attack that unit", and the next thing
            // here.
            std::optional<simd_float3> ground;
            const std::optional<rm::sim::UnitId> hit = pickAnyBatch(ray, units);
            const bool isAttack = hit && units.playerArmy != rm::sim::kNoArmy
                               && hostileTo(units, units.playerArmy, *hit);

            if (isAttack) {
                const rm::sim::Transform& at = units.store.transforms()[hit->index];
                ground = simd_make_float3(rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                          rm::sim::fxToFloat(at.z));
            } else {
                ground = rm::pickGround(ray, map->field);
            }
            if (!ground) {
                return;  // clicked the sky, or past the edge of the map
            }

            // Marked before the routing is attempted, and deliberately: the mark
            // answers "did that click land, and where", which is true even if
            // every unit then reports no route. A marker that appeared only on
            // success would leave the player unsure whether the click registered.
            orderMarks.push_back(OrderMark{
                .position = {ground->x, ground->y, ground->z},
                .age = 0.0f,
            });

            std::size_t failed = 0;
            for (const rm::sim::UnitId sel : selected) {
                if (!units.store.alive(sel)) {
                    continue;  // selected, then killed before the order was given
                }
                // Each unit routes on the map ITS limits see. Two units given
                // the same order can legitimately get different answers, and
                // one of them can be "no route" while the other walks off.
                const auto type = static_cast<std::size_t>(units.store.typeAt(sel.index));
                const rm::sim::PassabilityGrid& grid =
                    passability.gridFor(units.maxSlopeDegrees[type],
                                        units.maxWaterDepthElmos[type]);
                if (!orderRouted(units.store.motion()[sel.index],
                                 units.store.transforms()[sel.index], grid,
                                 rm::sim::fxFromFloat(ground->x),
                                 rm::sim::fxFromFloat(ground->z))) {
                    ++failed;
                }
            }
            if (failed > 0) {
                std::printf("no route there for %zu of %zu units\n", failed, selected.size());
            }
        });

        // The overhead view the camera returns to when space is released. Captured at the
        // start, which is the whole-map fit `OrbitCamera::frame` computed on load, so
        // "normal view" means the view the app opened with rather than a hardcoded angle.
        const float restPitch = window.camera().pitch;
        const float restYaw = window.camera().yaw;

        window.onKeyState([&window, restPitch, restYaw](char key, bool pressed) {
            // Releasing space snaps the camera back overhead. Holding it is what allows a
            // drag to rotate at all (Window.mm), so this is the other half of that mode:
            // look around while you hold, and you are put back when you let go, which
            // means a glance never costs you your bearings.
            if (key == ' ' && !pressed) {
                rm::OrbitCamera& camera = window.camera();
                camera.pitch = restPitch;
                camera.yaw = restYaw;
            }
        });

        // Scratch for the icon pass, held outside the frame callback so a frame costs no
        // allocation — the same reason the dust emitters are.
        std::vector<rm::Particle> iconScratch;
        rm::ui::Geometry hudScratch;
        float matchSeconds = 0.0f;

        window.onFrame([&](float elapsed) {
            // WASD pans the map, every frame rather than per keypress: a pan driven by
            // key EVENTS moves in jerks the length of the auto-repeat interval, and stops
            // dead the moment the repeat lapses.
            //
            // The step is scaled by the frustum's width at the target, the same quantity a
            // mouse pan uses, so the speed feels the same zoomed in as out — there is no
            // sensitivity constant here to get wrong. A second and a half to cross the
            // visible width.
            {
                const float across =
                    window.camera().elmosPerPoint(kPanReferenceHeightPoints)
                    * kPanReferenceHeightPoints;
                const float step = across * elapsed / kSecondsToCrossTheView;

                float right = 0.0f;
                float forward = 0.0f;
                if (window.keyHeld('a')) { right -= step; }
                if (window.keyHeld('d')) { right += step; }
                if (window.keyHeld('w')) { forward += step; }
                if (window.keyHeld('s')) { forward -= step; }
                if (right != 0.0f || forward != 0.0f) {
                    window.camera().pan(right, forward);
                }
            }

            matchSeconds += elapsed;
            const int ticks = clock.advance(elapsed);

            // How many batches the renderer currently knows about. A finished
            // construction spawns its unit into a NEW batch when nothing of that
            // blueprint stands yet, and `setInstances` silently drops a batch index it
            // was never given (Renderer.hpp) — so without noticing the growth here, a
            // building would exist in the sim, fight, earn, and never be drawn.
            const std::size_t batchesBefore = units.batches.size();

            // THE SAME TICK the headless pre-run makes. This loop used to call
            // `sim::tick` and `resolveCollisions` and nothing else, so a unit in the
            // interactive game moved perfectly and never fired a shot — while the whole
            // suite stayed green, because every rule was tested and the assembly of them
            // was not. Both callers now go through `advanceMatch`, which is what makes
            // "the same game" checkable rather than remembered.
            for (int i = 0; i < ticks; ++i) {
                const rm::sim::TickReport report =
                    advanceMatch(runner, matchTicks, static_cast<float>(matchTicks)
                                                         * kAppTickRate.secondsPerTick());
                ++matchTicks;

                // The match, announced once. The frame loop draws the fight rather than
                // narrating it, so this is the one thing worth saying out loud — and only
                // when there is a match to decide, the same guard the pre-run uses.
                if (report.matchEnded && !units.armies.empty()) {
                    if (report.winner) {
                        std::printf("team %d WINS\n", *report.winner);
                    } else {
                        std::printf("a DRAW: every army lost its commander\n");
                    }
                    std::fflush(stdout);
                }
            }

            // The gather builds every instance from the store, walk-cycle phase included —
            // see `UnitScene::instanceFor`. There is no separate pacing pass to run first.
            units.gatherForDrawing();

            // Re-upload when the match built something new. Only on growth, which is a
            // handful of times in a whole match — this walks every model and texture, so
            // doing it per frame would cost what it costs to load the scene.
            //
            // AFTER the gather, and that ordering is load-bearing: `setUnits` sizes each
            // batch's instance buffer from what its span holds, and a batch created this
            // tick holds nothing until the gather fills it. Uploading first gives the new
            // model a capacity of zero, and a unit type the player just built never draws.
            if (units.batches.size() != batchesBefore) {
                for (std::size_t b = batchesBefore; b < units.batches.size(); ++b) {
                    units.batches[b].animationDrivenByInstance = true;
                }
                window.setUnits(units.textures.all(), units.batches);
            }

            for (std::size_t batch = 0; batch < units.drawScratch.size(); ++batch) {
                window.setInstances(batch, units.drawScratch[batch]);
            }

            // Dust behind whatever is moving. After the sim, so a puff is born
            // where the unit has got to rather than where it started the frame.
            //
            // The emitters are gathered from the motion state the sim just wrote,
            // which is also where the speed threshold gets its answer — a unit
            // being jostled by a crowd is moving in position but not in speed, and
            // should not smoke.
            dustEmitters.clear();
            for (rm::UnitIndex slot = 0; slot < units.store.slotCount(); ++slot) {
                if (!units.store.slotAlive(slot)) {
                    continue;  // a wreck does not kick up dust
                }
                const rm::sim::MoveState& motion = units.store.motion()[slot];
                const rm::sim::Transform& at = units.store.transforms()[slot];
                dustEmitters.push_back(rm::DustEmitter{
                    .position = {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                                 rm::sim::fxToFloat(at.z)},
                    .moving = motion.moving,
                    // The dust threshold is stated per second, so the per-tick speed converts
                    // back — a display concern reading a sim value, which is what
                    // `secondsPerTick` exists for.
                    .topSpeedElmosPerSecond =
                        rm::sim::fxToFloat(motion.speedPerTick)
                        * static_cast<float>(kAppTickRate.ticksPerSecond()),
                    .radiusElmos = rm::sim::fxToFloat(motion.radiusElmos),
                });
            }
            rm::advanceParticles(particles, elapsed);
            rm::emitDust(particles, dustEmitters, map->field, elapsed, dustDebt, dustSeed);
            // ...and the map's own ambient effects, which run whether anything moves
            // or not: a map's lava does not stop steaming because nobody is looking.
            rm::emitAmbient(particles, props.ambient, elapsed, ambientDebt, dustSeed);

            // ICONS for whatever has shrunk past reading. Appended to the particle list
            // rather than drawn by a pass of their own, because an icon IS a stationary
            // camera-facing quad and that is what the particle pipeline already draws
            // (core/scene/UnitIcons.hpp). Built into a scratch copy so the icons do not
            // accumulate in the list the dust ages through.
            iconScratch.assign(particles.begin(), particles.end());
            appendSceneIcons(iconScratch, units, window.camera());
            window.setParticles(iconScratch);

            hudScratch.clear();
            rm::ui::build(hudScratch, window.labelFont(), window.readoutFont(),
                          hudThemeFor(units), hudStateFrom(units, matchSeconds),
                          static_cast<float>(window.width()),
                          static_cast<float>(window.height()));
            window.setHud(hudScratch.label, hudScratch.readout);

            // Rings under whatever is selected, rebuilt from scratch every
            // frame. Cheap — a selection is tens of units and each ring is 192
            // vertices of arithmetic — and it is the only way a ring can follow
            // a unit that is walking, which is the whole point of drawing one.
            //
            // The buffer is reused rather than reallocated so that a frame
            // costs no heap traffic; it is declared outside this lambda for
            // exactly that reason.
            // WRECKS FIRST, so the rings and markers a player is reading sit on top of the
            // scorch rather than under it. They are copied in rather than rebuilt: a wreck
            // is permanent and there is nothing to recompute.
            decalVertices.assign(units.wreckDecals.begin(), units.wreckDecals.end());
            // Dead selections draw nothing rather than being pruned here: a frame is not
            // where a selection changes, and a ring under a wreck is the bug this avoids.
            for (const rm::sim::UnitId sel : selected) {
                if (!units.store.alive(sel)) {
                    continue;
                }
                const rm::sim::Transform& at = units.store.transforms()[sel.index];
                rm::appendSelectionRing(
                    decalVertices, map->field,
                    {rm::sim::fxToFloat(at.x), rm::sim::fxToFloat(at.y),
                     rm::sim::fxToFloat(at.z)},
                    rm::sim::fxToFloat(units.store.motion()[sel.index].radiusElmos)
                        * kSelectionRingMargin,
                    kSelectionRingColour);
            }

            // ...and a marker wherever an order was given recently. Aged by the
            // frame's own elapsed time rather than a wall clock, so a marker
            // fades over the same span whatever the frame rate — and by the same
            // reasoning that paces the walk cycles by distance covered.
            for (OrderMark& mark : orderMarks) {
                mark.age += elapsed;
                rm::appendOrderMarker(decalVertices, map->field, mark.position,
                                      kOrderMarkerColour, mark.age);
            }
            // Dropped once they have nothing left to draw. The append above is
            // silent past its lifetime, so this is housekeeping rather than
            // correctness — without it the list grows for as long as the app runs.
            std::erase_if(orderMarks, [](const OrderMark& mark) {
                return mark.age >= rm::kOrderMarkerSecondsToLive;
            });

            window.setGroundDecals(decalVertices);
            // ...and an outline around each selected unit, which is what a ring
            // cannot do at a low camera angle where the units hide their own rings.
            // The renderer wants (batch, instance) — where the unit ended up in THIS frame's
            // gather. That is a projection of the selection, not the selection itself, which
            // is why it is built here and not stored.
            selectionScratch.clear();
            for (const rm::sim::UnitId sel : selected) {
                if (const std::optional<rm::SelectionEntry> where = units.drawnAt(sel)) {
                    selectionScratch.push_back(*where);
                }
            }
            window.setSelection(selectionScratch);
        });

        window.show();

        RMBenchWatcher* watcher = nil;
        if (bench.enabled) {
            std::printf("benchmarking %zu frames (discarding %zu warmup)\n  %s\n",
                        bench.frames, bench.warmup,
                        describeQuality(settings, propInstances, marchDust.size()).c_str());
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
