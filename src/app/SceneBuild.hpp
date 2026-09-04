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

#include "core/map/HeightField.hpp"
#include "core/map/ScenarioSave.hpp"
#include "core/sim/Fx.hpp"
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

/// `factions`, when non-empty, seats army i as `factions[i % factions.size()]` in place of
/// the round-robin default — `--factions` is how a mirror match or a chosen matchup exists.
/// Cycling rather than truncating, so two names over eight armies is an even split.
void spawnCommanders(UnitScene& scene, const rm::HeightField& field,
                     std::span<const rm::mapinfo::StartPosition> starts,
                     const rm::vfs::Vfs& content, bool observer = false,
                     std::span<const rm::sim::Faction> factions = {});

/// The movement state a unit of this definition is born with — the ONE derivation both spawn
/// paths use.
///
/// EXPORTED TO BE TESTED, because the two paths drifted and the drift was undetectable from
/// outside. `spawnCommanders` set the army index and nothing else, so every commander started
/// with `speedPerTick` and `turnPerTick` at zero — `MoveState`'s deliberate defaults — and also
/// with no collision radius. The player's own unit could not move at any speed and did not
/// collide with anything.
///
/// Nothing about that is visible to an assertion aimed at the order path: the route is found,
/// the order is accepted, the queue line is drawn, and no refusal is printed. The unit
/// multiplies its step by a speed of zero. A headless `--march` reports "2 of 2 units routed"
/// and is telling the truth about routing while saying nothing about motion.
[[nodiscard]] rm::sim::MoveState motionFor(const rm::unitdef::UnitDef& def, int armyIndex);

/// The DRAWABLE registration half of `spawnUnit`: model, textures, batch, type and traits,
/// without spawning anything into the store. Idempotent per blueprint — the second call is a
/// map lookup.
///
/// FACTORED OUT FOR THE BUILD GHOST, which needs exactly this and nothing more: a silhouette
/// at the cursor is a model with no unit behind it, and before this existed the only way to
/// get a blueprint's model on screen was to spawn one. (`resolveBuildable` is the OTHER half
/// of the story — a type with no batch — and the two remain distinct type entries per the
/// loose end documented in `Match.cpp`.)
[[nodiscard]] std::optional<rm::UnitTypeIndex> ensureDrawableType(UnitScene& scene,
                                                                  const rm::vfs::Vfs& content,
                                                                  std::string_view blueprintPath);

[[nodiscard]] std::optional<rm::sim::UnitId> spawnUnit(UnitScene& scene,
                                                       const rm::vfs::Vfs& content,
                                                       const rm::HeightField& field,
                                                       std::string_view blueprintPath,
                                                       std::array<float, 3> position,
                                                       const rm::sim::Army& army, rm::Brad yaw);

/// Which way a structure at (x, z) faces: toward the map centre, so a base laid out
/// toward the fight reads as one.
///
/// THE ONE AUTHORITY on that bearing, and it exists because there used to be two.
/// `Match.cpp` computed it at completion while the in-progress site drew at yaw zero
/// (with a comment claiming the spawn would face north — once true, then not), so
/// every diagonal base watched its buildings snap ~45° the moment they finished. The
/// site, the completion spawn, and the placement ghost all ask here now; a future
/// player-chosen facing (retail stores one of four cardinals per order — Recoil's
/// `buildFacing`) replaces this body and every caller follows.
///
/// FIXED POINT, not `std::atan2`: the completion path feeds this into
/// `Transform.heading`, which the state hash covers, and libm is where two
/// architectures disagree in the last ulp. Presentation callers convert with
/// `radiansFromBrad` at their own edge.
[[nodiscard]] inline rm::Brad structureFacing(const rm::HeightField& field, rm::sim::Fx x,
                                              rm::sim::Fx z) noexcept {
    return rm::sim::fxBearing(rm::sim::fxFromFloat(field.widthElmos() * 0.5f) - x,
                              rm::sim::fxFromFloat(field.depthElmos() * 0.5f) - z);
}

[[nodiscard]] std::optional<rm::UnitTypeIndex> resolveBuildable(UnitScene& scene,
                                                                const rm::vfs::Vfs& content,
                                                                std::string_view blueprintPath);

/// Gives every unit that belongs to nobody to the seated player. Returns how many changed hands.
///
/// WHY THIS EXISTS. `--units` spawns before there are any armies to spawn into — `resolveUnits`
/// runs first, because `--skirmish` is documented as APPENDING to whatever `--units` asked for
/// rather than replacing it — so a `--units` crowd carries `kNoArmy`. On its own that is
/// correct and harmless: a march with no skirmish has no armies, `playerArmy` is `kNoArmy` too,
/// and the selection filter that compares them is skipped entirely, so every unit is clickable.
///
/// Put the two together and the crowd becomes furniture. `pickAcrossBatches` refuses anything
/// whose army is not the player's, so in a skirmish a `--units` unit cannot be selected, cannot
/// be ordered, and cannot show a build panel — which makes `--units` useless for exercising any
/// of the interface, the one job a crowd of test units is for. It is also the only way to get
/// an ENGINEER into a match today: `data/opening.lua` builds four structures and then tanks.
///
/// ADOPTION RATHER THAN A SPAWN-TIME ARMY, because the ownerless window is real and short: the
/// units exist before the armies do. Giving them an owner the moment there is one to give is
/// the smallest thing that closes it, and "every unit nobody owns" is exactly the set — the
/// skirmish's own spawns all carry an army by the time this runs.
///
/// An observer adopts nothing and the count is zero, which is what `--observer` means.
std::size_t adoptOwnerlessUnits(UnitScene& scene);

/// Submits a build issue for deterministic application in its pre-tick phase.
///
/// **THE HOLE THIS CLOSES, kept because it was documented at length as open.** Builds used to
/// push a `Construction` straight onto `scene.building` and raise `ConstructionStarted`
/// themselves, so a build order skipped the authorisation check and left no entry in the command
/// log. `check_one_order_path.sh` never caught it — that guard watched the MOVEMENT primitives —
/// so P4.2's "every order goes through applyCommand" was true of movement and not of
/// construction. It is true of both now, and the guard watches both.
///
/// What had blocked it was `#3090`: `resolveBuildable` returned an index into a private
/// `scene.buildable` list typed `rm::UnitTypeIndex` while `applyCommand` read
/// `catalog.def(command.buildType)` — two numbers wearing one type name, and routing through the
/// sim turned a 36-mass extractor into an 18,000-mass experimental. That is gone: an index is a
/// catalog type index everywhere, so the sim reads the definition the caller meant.
///
/// THE RATE IS NO LONGER PASSED IN, and that is the one visible difference. `Decision::buildRate`
/// was the builder's own `def->buildRate`, read by the caller and handed back; `applyCommand`
/// reads it off the builder itself. Same number, one fewer way to disagree — and a decision that
/// named a rate could name a rate the builder does not have, which is a whole class of desync a
/// value carried across an interface invites.
///
/// LIVES HERE rather than beside `issueMove` in `Match.cpp` because `orderFirstExtractors` below
/// needs it too, and `Match.hpp` includes this header rather than the other way round. The first
/// extractor of every match is a build like any other and goes the same way.
[[nodiscard]] bool issueBuild(UnitScene& scene, rm::sim::UnitId builder,
                               rm::PlayerIndex player, rm::TickIndex tick, rm::UnitTypeIndex type,
                               rm::sim::Fx atX, rm::sim::Fx atZ, bool queued = false);

struct DispatchedCommand {
    rm::sim::CommandIssue recorded;
    rm::sim::ApplyCommandResult result;
};

/// Queues one semantic issue, assigning its source-local identity.
[[nodiscard]] std::optional<rm::CommandId> submitCommand(UnitScene& scene,
                                                          rm::sim::CommandIssue issue);

/// Applies and records one deterministic tick/phase batch. The recorded issue contains the
/// canonical accepted set, including an empty set when submission consumed an ID but no unit
/// accepted the command.
[[nodiscard]] std::vector<DispatchedCommand> dispatchCommands(
    UnitScene& scene, const rm::HeightField& field, PassabilitySet& passability,
    rm::TickIndex tick, rm::sim::CommandPhase phase,
    rm::sim::PathService* pathService = nullptr,
    rm::sim::ScriptTaskHost* scriptTasks = nullptr);

/// The player driving an army, or none. What an issued order is attributed to.
[[nodiscard]] rm::PlayerIndex playerDriving(const UnitScene& scene, int army);

void orderFirstExtractors(UnitScene& scene, std::span<const rm::scenario::Marker> markers,
                          const rm::vfs::Vfs& content, const rm::HeightField& field,
                          PassabilitySet& passability);

[[nodiscard]] UnitScene resolveUnits(std::span<const UnitOptions> requests,
                                     const rm::HeightField& field,
                                     std::span<const rm::mapinfo::StartPosition> starts,
                                     bool hasWater, float waterLevelElmos,
                                     const rm::vfs::AssetSearch& search,
                                     const rm::vfs::Vfs& content);

} // namespace rm::app
