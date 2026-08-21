#pragma once

// Building a scene: resolving what a `--units` request or a `--skirmish` names, and spawning it.
//
// EXTRACTED FROM `main.mm` FOR PLAN2.md §7 P7.5. The layer above `app/Scene.hpp`: this is what
// turns "a blueprint path" into "a unit standing on the map with a model, a batch, a definition,
// an army and a place in the store".
//
// §10 NAMED THIS AS THE PART TO READ RATHER THAN CUT AND PASTE, and it was right — the spawn
// logic and the extractor ordering are here. Both came across whole, comments included, because
// what they encode is not obvious from the code: `spawnCommanders` reuses one batch per FACTION
// so four models serve eight armies, and `orderFirstExtractors` exists because a commander that
// walks to a deposit before ordering has already lost the race.

#include "app/Scene.hpp"

#include "core/map/ScenarioSave.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/vfs/AssetSearch.hpp"
#include "core/vfs/Vfs.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rm::app {

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

/// One `--units` argument: a model, how many of it, how big, and optionally an
/// animation to play on it.
struct UnitOptions {
    std::filesystem::path modelPath;
    std::size_t count = kDefaultUnitCount;
    float scale = 1.0f;
    std::filesystem::path animationPath;
};

// A unit resolved out of the mounted content: everything `resolveUnits` needs, so
// that the archive path and the filesystem path converge before the code that
// places instances rather than branching all the way down it.
struct VfsUnit {
    rm::unitdef::UnitDef def;
    rm::Model model;
    std::string albedoPath;
    std::string shadingPath;
};

// --- What the build layer does ------------------------------------------------------------

[[nodiscard]] std::optional<rm::unitdef::UnitDef> resolveUnitDef(
    const std::filesystem::path& path, const rm::vfs::AssetSearch& search,
    std::filesystem::path& modelOut);

[[nodiscard]] std::string scmTextureInVfs(const std::string& meshPath, const char* suffix,
                                          const rm::vfs::Vfs& content);

[[nodiscard]] std::optional<VfsUnit> resolveUnitFromContent(const std::string& blueprintPath,
                                                            const rm::vfs::Vfs& content);

/// One commander per start position, each its own army.
///
/// `observer` seats nobody: every army gets a script and `playerArmy` stays `kNoArmy`, so
/// nothing is selectable and no click is authorised. That is `--observer`, and it goes through
/// the same authorisation as everything else rather than through a spectator special case.
/// Sizes the scene's intel grids for this map and this many alliances (ADR-037).
///
/// AFTER the armies exist and after `--alliances` has grouped them, because the grids are
/// per alliance and one built for the wrong count would leave a side with nowhere to see.
/// A scene with no armies is left unconfigured, which means every query answers "seen" —
/// a `--units` crowd has no sides to keep secrets from.
void configureIntel(UnitScene& scene, const rm::HeightField& field, rm::sim::VisionStyle style);

void spawnCommanders(UnitScene& scene, const rm::HeightField& field,
                     std::span<const rm::mapinfo::StartPosition> starts,
                     const rm::vfs::Vfs& content, bool observer = false);

[[nodiscard]] std::optional<rm::sim::UnitId> spawnUnit(UnitScene& scene,
                                                       const rm::vfs::Vfs& content,
                                                       const rm::HeightField& field,
                                                       std::string_view blueprintPath,
                                                       std::array<float, 3> position,
                                                       const rm::sim::Army& army, float yaw);

[[nodiscard]] std::optional<rm::UnitTypeIndex> resolveBuildable(UnitScene& scene,
                                                                const rm::vfs::Vfs& content,
                                                                std::string_view blueprintPath);

void orderFirstExtractors(UnitScene& scene, std::span<const rm::scenario::Marker> markers,
                          const rm::vfs::Vfs& content);

[[nodiscard]] UnitScene resolveUnits(std::span<const UnitOptions> requests,
                                     const rm::HeightField& field,
                                     std::span<const rm::mapinfo::StartPosition> starts,
                                     float landAbove, const rm::vfs::AssetSearch& search,
                                     const rm::vfs::Vfs& content);

} // namespace rm::app
