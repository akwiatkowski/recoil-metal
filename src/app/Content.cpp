#include "app/Content.hpp"

#include "core/Error.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace rm::app {



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
    // The ground's own light. Parsed since the lighting block was first read and
    // dropped on the floor until now — the terrain shader lit every map with two
    // hardcoded floats, which is what made a `.scmap` render darker than the game
    // renders it. See Renderer::Environment for the terms.
    environment.sunColour = map->lighting.sunColour;
    environment.sunAmbience = map->lighting.sunAmbience;
    environment.shadowFill = map->lighting.shadowFill;
    environment.lightingMultiplier = map->lighting.multiplier;
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
    std::printf("  lighting: sun (%.2f %.2f %.2f), ambience (%.2f %.2f %.2f),"
                " shadow fill (%.2f %.2f %.2f), multiplier %.2f\n",
                static_cast<double>(environment.sunColour[0]),
                static_cast<double>(environment.sunColour[1]),
                static_cast<double>(environment.sunColour[2]),
                static_cast<double>(environment.sunAmbience[0]),
                static_cast<double>(environment.sunAmbience[1]),
                static_cast<double>(environment.sunAmbience[2]),
                static_cast<double>(environment.shadowFill[0]),
                static_cast<double>(environment.shadowFill[1]),
                static_cast<double>(environment.shadowFill[2]),
                static_cast<double>(environment.lightingMultiplier));
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

} // namespace rm::app
