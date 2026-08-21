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
#include "core/ui/Minimap.hpp"

#include <cassert>
#include "core/settings/Settings.hpp"
#include "core/scene/GroundDecals.hpp"
#include "core/scene/UnitDraw.hpp"
#include "core/scene/UnitPlacement.hpp"
#include "core/sim/Army.hpp"
#include "core/data/MoveDef.hpp"
#include "core/data/Opening.hpp"
#include "core/data/Roster.hpp"
#include "core/sim/Command.hpp"
#include "app/FafAi.hpp"
#include "core/sim/BuildOrder.hpp"
#include "core/sim/Combat.hpp"
#include "core/sim/Economy.hpp"
#include "core/sim/Movement.hpp"
#include "core/sim/Pathfinding.hpp"
#include "core/sim/Replay.hpp"
#include "core/sim/StateHash.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/SlowUpdate.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/unit/UnitDef.hpp"
#include "core/vfs/AssetSearch.hpp"
#include "core/texture/Dds.hpp"
#include "core/map/TileAtlas.hpp"
#include "core/mesh/TerrainMesh.hpp"
#include "app/Content.hpp"
#include "app/Scene.hpp"
#include "app/Cli.hpp"
#include "app/Interface.hpp"
#include "app/Run.hpp"
#include "app/View.hpp"
#include "app/Match.hpp"
#include "app/SceneBuild.hpp"

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


namespace {

// The content loaders moved to `app/Content.hpp` (§7 P7.5). Brought in wholesale rather than
// qualified at a hundred call sites: they were file-local a commit ago and their meaning has
// not changed, and this file is what P7.5 is emptying.
using namespace rm::app;  // NOLINT(google-build-using-namespace)






} // namespace




namespace {


} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        // The content is mounted BEFORE the map, because the map's ground layers
        // and props are content too — a `.scmap` names its strata as VFS paths and
        // has always needed somewhere to look them up.
        const rm::vfs::Vfs content = parseContent(argc, argv);

        // `--tick-rate N`: the sim's rate for this run, 5–50 Hz. FIRST, because a unit's speed
        // and a weapon's reload are derived from it at spawn and at catalog registration, and
        // this used to sit after `resolveUnits` — so a `--units` crowd was built at 10 Hz
        // whatever the flag said. The fourth instance of P2.3's bug shape: a rate applied later
        // than the thing that derives from it.
        if (const std::size_t requested = parseCount(argc, argv, "--tick-rate");
            requested > 0) {
            setAppTickRate(static_cast<std::uint32_t>(requested));
            std::printf("sim: %u ticks a second\n", gAppTickRate.ticksPerSecond());
        }

        // `--no-interpolate`: draw the newest snapshot rather than blending two (§7 P7.2).
        //
        // What a capture wants. A screenshot of tick N should BE tick N; with interpolation on,
        // what it shows depends on how far the frame clock's banked time had got, which is a
        // function of when the process happened to be scheduled. Every golden image in
        // `docs/images/` is taken with this.
        gInterpolate = !hasFlag(argc, argv, "--no-interpolate");

        // `--print-events`: narrate the sim's own event queue (§7 P6.1's manual check).
        gPrintEvents = hasFlag(argc, argv, "--print-events");

        // `--ai-debug`: boot the FAF AI sandbox and report on it, then play the match (ADR-039).
        //
        // Printed BEFORE the match rather than after, and traced module by module rather than
        // summarised, because the failure mode being debugged is a HANG: a summary at the end
        // never arrives, while a trace that stops mid-line names the file that hung. The whole
        // output is meant to be pasted somewhere and read by someone else.
        if (hasFlag(argc, argv, "--ai-debug")) {
            rm::ai::reportFafSandbox();
        }

        // `--dump-weapon <ID>`: print and exit. Before the map, because a weapon's timings have
        // nothing to do with terrain and requiring a `.scmap` to read a blueprint would make
        // the check harder to run than the thing it checks.
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string{argv[i]} == "--dump-weapon") {
                return dumpWeapons(content, argv[i + 1]) ? 0 : 1;
            }
        }

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

        // `--alliances N`: deal the armies into N sides that win together, round-robin, so
        // `--armies 4 --alliances 2` is a 2v2. Free-for-all — every army its own alliance —
        // remains the default, which is what `freeForAll` builds.
        //
        // Read before `--skirmish` spawns anything: an alliance decides who shoots whom, and
        // the scripted opponents pick targets on their first decision.
        //
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
            spawnCommanders(units, map->field, starts, content,
                            hasFlag(argc, argv, "--observer"));
            if (const std::size_t alliances = parseCount(argc, argv, "--alliances");
                alliances > 1 && alliances < units.armies.size()) {
                for (rm::sim::Army& army : units.armies) {
                    army.alliance = army.index % static_cast<int>(alliances);
                }
                std::printf("skirmish: %zu armies in %zu alliances\n", units.armies.size(),
                            alliances);
            }
            // AFTER the alliances are grouped: the grids are per alliance, and one sized
            // for the wrong count would leave a side with nowhere to see.
            configureIntel(units, map->field, parseVisionStyle(argc, argv));

            // A `--units` crowd spawned before there were armies to spawn into, so it belongs
            // to nobody and nothing can select it. Hand it to the seated player now that there
            // is one — see `adoptOwnerlessUnits`. This is also how an engineer reaches a match:
            // `--units /units/UEL0105/UEL0105_unit.bp 4 --skirmish` puts four of them in the
            // player's hands, which is what `make shot-engineer` captures.
            if (const std::size_t adopted = adoptOwnerlessUnits(units); adopted > 0) {
                std::printf("skirmish: %zu ownerless unit(s) adopted by army %d\n", adopted,
                            units.playerArmy);
            }
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
        units.publish(0);
        units.publish(0);
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

        const ShotOptions shot = parseShot(argc, argv);
        const BenchOptions bench = parseBench(argc, argv);

        // --- Which way this scene reaches a screen -------------------------
        //
        // Three modes, each in `app/Run.hpp` (§7 P7.5), sharing one `Session` — which is the
        // set of locals they used to share by all being inside this function. Dispatch, in
        // order of headlessness: a benchmark wants no window at all, a screenshot wants one
        // frame, and the game wants the loop.
        const Session session{
            .argc = argc,
            .argv = argv,
            .map = *map,
            .units = units,
            .props = props,
            .passability = passability,
            .content = content,
            .settings = settings,
            .mesh = mesh,
            .starts = starts,
            .marchDust = marchDust,
            .shot = shot,
            .bench = bench,
            .march = marchOptions,
            .look = parseLook(argc, argv),
            .focus = focus,
            .animationTime = animationTime,
            .propInstances = propInstances,
        };

        if (bench.enabled && bench.offscreen) {
            return runOffscreenBenchmark(session);
        }
        if (shot.enabled) {
            return runScreenshot(session);
        }
        return runWindowed(session);
    }
}
