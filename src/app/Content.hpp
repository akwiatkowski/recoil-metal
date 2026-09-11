#pragma once

// Turning paths on disk into things the renderer can be handed: maps, props, models, textures.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5, which asks for that file to become "argv plus
// wiring, under 400 lines from 3,948" — it had reached 5,098. This is the bottom layer of what
// came out: everything that reads content, and nothing that knows what a match is.
//
// WHY MOVING IT MATTERS MORE THAN SHORTENING `main.mm`. §10 says that file "holds real
// knowledge, not just mess", and that is true of the spawn logic and the extractor ordering. It
// is not true of this: three map formats, two model formats and four texture conventions are a
// LIBRARY, and a library inside an executable-only translation unit is one no test can link.
// That is the same argument `check_sim_boundary.sh` exists to make about the tick order.

#include "core/map/GroundSplat.hpp"
#include "core/map/HeightField.hpp"
#include "core/map/MapInfo.hpp"
#include "core/map/PropBlueprint.hpp"
#include "core/map/ProceduralField.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/map/Scmap.hpp"
#include "core/map/Smf.hpp"
#include "core/map/Smt.hpp"
#include "core/map/TerrainType.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/log/Log.hpp"
#include "core/mesh/TerrainMesh.hpp"
#include "core/model/Model.hpp"
#include "core/model/Pose.hpp"
#include "core/model/S3o.hpp"
#include "core/model/Sca.hpp"
#include "core/model/Scm.hpp"
#include "core/scene/PropBatch.hpp"
#include "core/settings/Settings.hpp"
#include "core/texture/Dds.hpp"
#include "core/vfs/AssetSearch.hpp"
#include "core/scene/Particles.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/vfs/Vfs.hpp"
#include "render/Renderer.hpp"

#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::app {

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

    /// The map's own thumbnail, for the minimap to stand on. Decoded here so the renderer
    /// takes a `dds::Texture` like every other image it is handed, rather than a blob.
    ///
    /// EMPTY IS ORDINARY: a `.smf` has no preview, and a procedural map has no file. The
    /// minimap then draws its own filled panel, which is what it did for its whole first life.
    rm::dds::Texture preview;

    /// The map's wave-normal texture (its first named wave layer), decoded like the
    /// preview. Empty when the map names none or the archive lacks it — the water then
    /// keeps its analytic ripple.
    rm::dds::Texture waterWaves;

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

// Where BAR's reference content lives. Used only to resolve a model's texture
// names, which S3O carries but does not contain.
constexpr const char* kBarTextureDir =
    "projects/llm/games/faf/forged-alliance-reborn/reference/BAR/unittextures";

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
constexpr const char* kScmDiffuseSuffix = "_Albedo.dds";

constexpr const char* kScmShadingSuffix = "_SpecTeam.dds";

constexpr const char* kScmNormalsSuffix = "_NormalsTS.dds";

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
            rm::log::writef(rm::log::Level::Warn, "texture", "no %s texture (%s): %s",
                            slot, path.filename().string().c_str(),
                            texture.error().message.c_str());
            indexByPath_.emplace(key, -1);
            return -1;
        }

        std::printf("  %s %s: %dx%d, %d mips\n", slot, path.filename().string().c_str(),
                    texture->width, texture->height, texture->mipLevels);

        const auto index = static_cast<int>(textures_.size());
        textures_.push_back(std::move(*texture));
        srgb_.push_back(std::string_view{slot}.find("albedo") != std::string_view::npos);
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
            rm::log::writef(rm::log::Level::Warn, "texture",
                            "no %s texture (%s): not in the mounted content", slot,
                            vfsPath.c_str());
            indexByPath_.emplace(vfsPath, -1);
            return -1;
        }

        auto texture = rm::dds::load(*bytes);
        if (!texture) {
            rm::log::writef(rm::log::Level::Warn, "texture", "no %s texture (%s): %s",
                            slot, vfsPath.c_str(), texture.error().message.c_str());
            indexByPath_.emplace(vfsPath, -1);
            return -1;
        }

        std::printf("  %s %s: %dx%d, %d mips\n", slot,
                    std::filesystem::path{vfsPath}.filename().string().c_str(), texture->width,
                    texture->height, texture->mipLevels);

        const auto index = static_cast<int>(textures_.size());
        textures_.push_back(std::move(*texture));
        srgb_.push_back(std::string_view{slot}.find("albedo") != std::string_view::npos);
        indexByPath_.emplace(vfsPath, index);
        return index;
    }

    [[nodiscard]] std::span<const rm::dds::Texture> all() const noexcept { return textures_; }
    [[nodiscard]] std::size_t size() const noexcept { return textures_.size(); }
    /// Per texture, whether it is authored colour (sRGB) rather than data. Index-aligned
    /// with `all()`: the slot said so at resolve time ("albedo", "prop albedo").
    [[nodiscard]] std::span<const char> srgbFlags() const noexcept { return srgb_; }

private:
    std::vector<rm::dds::Texture> textures_;
    std::vector<char> srgb_;
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

// --- What the loaders do ------------------------------------------------------------------

[[nodiscard]] std::optional<rm::HeightField> resolveSmfHeightField(const std::string& path);

[[nodiscard]] std::optional<rm::TileAtlas> resolveAtlas(const std::filesystem::path& smfPath);

[[nodiscard]] std::optional<LoadedSplat> resolveSplat(const rm::scmap::Map& map,
                                                     const rm::vfs::Vfs& content);

[[nodiscard]] std::optional<LoadedMap> resolveScmap(const std::string& path,
                                                   const rm::vfs::Vfs& content);

[[nodiscard]] std::vector<std::byte> readMagic(const std::filesystem::path& path,
                                              std::size_t count);

/// The map named on the command line, whichever format it is in.
///
/// Decided by a file's MAGIC rather than its extension, which is the rule the whole loader
/// family follows — see ADR-005.
[[nodiscard]] std::optional<LoadedMap> resolveMap(int argc, const char* argv[],
                                                 const rm::vfs::Vfs& content);

[[nodiscard]] float propYaw(const rm::scmap::Prop& prop) noexcept;

[[nodiscard]] PropScene loadProps(const LoadedMap& map, const std::filesystem::path& root,
                                  const rm::vfs::Vfs& content);

[[nodiscard]] std::string describeQuality(const rm::Settings& settings, std::size_t propCount,
                                          std::size_t particleCount);

[[nodiscard]] std::filesystem::path barTexturePath(const rm::vfs::AssetSearch& search,
                                                   const std::string& name);

[[nodiscard]] std::filesystem::path scmTexturePath(const std::filesystem::path& modelPath,
                                                   const char* suffix);

[[nodiscard]] std::expected<rm::Model, rm::MapError> loadModel(
    const std::filesystem::path& path);

[[nodiscard]] std::expected<rm::Model, rm::MapError> loadModelBytes(
    std::span<const std::byte> bytes);

} // namespace rm::app
