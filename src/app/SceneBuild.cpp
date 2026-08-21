#include "app/SceneBuild.hpp"

#include "core/data/ArmorDefs.hpp"
#include "core/data/MoveDef.hpp"
#include "core/model/Sca.hpp"
#include "core/unit/UnitBlueprint.hpp"
#include "core/sim/BuildOrder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>

namespace rm::app {


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


void configureIntel(UnitScene& scene, const rm::HeightField& field,
                    rm::sim::VisionStyle style) {
    if (scene.armies.empty()) {
        return;
    }

    // One more than the highest alliance index in use, not the army count: `--alliances 2`
    // on eight armies needs two grids, and sizing by armies would allocate six that nothing
    // ever writes to — on an 8192-elmo map that is megabytes of zeroes per sense.
    int highest = 0;
    for (const rm::sim::Army& army : scene.armies) {
        highest = std::max(highest, army.alliance);
    }

    scene.intel.configure(static_cast<std::size_t>(highest) + 1,
                          rm::sim::fxFromFloat(field.widthElmos()),
                          rm::sim::fxFromFloat(field.depthElmos()), style);

    const rm::sim::IntelGrid& sight = scene.intel.grid(0, rm::sim::IntelKind::Vision);
    std::printf("intel: %s style, %d alliance(s), sight grid %dx%d at %d elmos a square\n",
                style == rm::sim::VisionStyle::Recoil ? "recoil (terrain blocks sight)"
                                                      : "forged alliance (flat discs)",
                highest + 1, sight.squaresX(), sight.squaresZ(),
                sight.squareElmos().floorToInt());
}

void spawnCommanders(UnitScene& scene, const rm::HeightField& field,
                     std::span<const rm::mapinfo::StartPosition> starts,
                     const rm::vfs::Vfs& content, bool observer) {
    if (starts.empty()) {
        std::fprintf(stderr, "skirmish: the map declares no start positions\n");
        return;
    }

    // THE ARMOUR TABLE FIRST, before anything else touches the catalog (PLAN2.md §7 P10.1).
    //
    // `UnitCatalog::setArmor` resolves names when a TYPE IS REGISTERED, so a type added before
    // this call keeps the flat profile it was given and silently ignores armour for the rest of
    // the match. Nothing registers a type before `spawnCommanders`, and this being the first
    // statement in it is what keeps that true.
    //
    // READ FROM THE MOUNTED CONTENT rather than compiled in, because the two installs disagree:
    // retail ships 6 classes with 5 non-1.0 entries, FAF ships 9 with 10, and they differ on the
    // numbers too — a structure takes 0.066666 of an Overcharge under retail and 0.25 under FAF.
    // See `core/data/ArmorDefs.hpp`.
    //
    // Content with no armour definition gets the `default`-only table, which is exactly the
    // pre-P10.1 engine: every unit ordinary, every weapon a flat scalar. That is why this is
    // safe to run unconditionally and why `make verify` still MATCHes for a scene without it.
    if (const auto armorSource = content.read(std::string{rm::data::kArmorDefinitionPath})) {
        const std::string text{reinterpret_cast<const char*>(armorSource->data()),
                               armorSource->size()};
        rm::data::ArmorTable armor = rm::data::armorTableFromSource(text);
        std::printf("skirmish: armour table holds %zu class(es) and %zu multiplier(s)\n",
                    armor.registry.size(), armor.multipliers.size());
        scene.catalog.setArmor(std::move(armor.registry), std::move(armor.multipliers));
    } else {
        std::printf("skirmish: no %.*s in the mounted content — every unit is ordinary armour\n",
                    static_cast<int>(rm::data::kArmorDefinitionPath.size()),
                    rm::data::kArmorDefinitionPath.data());
    }

    // The roster and the opening, before anything is ordered: both decide WHAT gets built, and
    // the first extractor is ordered during this same setup.
    scene.roster = buildRoster(content);
    if (const auto fromFile = rm::data::loadOpening("data/opening.lua")) {
        scene.opening = *fromFile;
    } else {
        // Not an error. A build run from outside the repo has no `data/` beside it, and
        // `defaultOpening()` states the same plan in C++ precisely so that still plays.
        std::printf("skirmish: no data/opening.lua — using the built-in opening\n");
    }
    std::printf("skirmish: roster holds %zu units; opening builds %zu structure(s),"
                " wave of %zu\n",
                scene.roster.size(), scene.opening.structures.size(),
                scene.opening.waveSize);

    scene.armies = rm::sim::freeForAll(starts.size());

    // One participant per army, the human driving the first. `--armies 4` with two alliances
    // is the 2v2 §7 P2.4 asks to be checked by hand, and `--alliances N` below sets it up.
    //
    // `--observer` seats nobody. Every army gets a script and `playerArmy` stays `kNoArmy`, so
    // nothing is selectable and no click is authorised — which is what watching the scripted
    // opponents play each other means, and it goes through the same authorisation as everything
    // else rather than through a "spectator" special case.
    const int human = observer ? rm::sim::kNoArmy : 0;
    scene.players = rm::sim::onePlayerPerArmy(starts.size(), human);
    scene.playerArmy = human;

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
            // FROM THE MOTION CLASS, not from the blueprint's own slope and depth — those
            // govern building placement (P3.4, `core/data/MoveDef.hpp`).
            const rm::data::MoveDef move = rm::data::moveDefFor(unit->def);

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
            const rm::UnitTypeIndex type =
                scene.catalog.add(&scene.definitions.back(), gAppTickRate);
            // The type draws with the batch just pushed. A MAP now rather than an identity —
            // see `Scene::batchForType` for why the two numbers had to come apart (`#3090`).
            scene.setBatchForType(type, scene.batches.size() - 1);
            scene.setPathForType(type, path);
            scene.setTypeTraits(type, move.maxSlopeDegrees, move.maxWaterDepthElmos,
                                unit->def.meshToElmos);

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
    // Published before the gather because the gather reads snapshots now, and outside a tick
    // there is no tick to have published one. Twice, so `previous` and `current` are the same
    // state — a scene that has only just been built has nothing to interpolate.
    scene.publish(0);
    scene.publish(0);
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
            .mass = gAppTickRate.magPerTick(rm::sim::kCommanderTrickleMassPerSecond),
            .energy = gAppTickRate.magPerTick(rm::sim::kCommanderTrickleEnergyPerSecond),
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
    auto found = scene.typeForBlueprint.find(blueprintPath);
    if (found == scene.typeForBlueprint.end()) {
        const auto unit = resolveUnitFromContent(std::string{blueprintPath}, content);
        if (!unit) {
            std::fprintf(stderr, "spawn: no unit at %s in the mounted content\n",
                         std::string{blueprintPath}.c_str());
            return std::nullopt;
        }

        scene.models.push_back(unit->model);
        // From the MOTION CLASS. The fallback rules that used to be here — "a structure's zero
        // slope means the default, and a ground mover's zero depth means does not wade" — were
        // guesses standing in for a fact the blueprint had all along: `RULEUMT_*` says what this
        // unit crosses (P3.4). ADR-027 said passability comes from motion class; now it does.
        const rm::data::MoveDef move = rm::data::moveDefFor(unit->def);
        scene.batches.push_back(rm::UnitBatch{
            .model = &scene.models.back(),
            .instances = {},
            .textures = rm::TexturePair{
                .diffuse = scene.textures.resolve(content, unit->albedoPath, "albedo"),
                .shading = scene.textures.resolve(content, unit->shadingPath, "specTeam"),
            },
        });
        scene.definitions.push_back(unit->def);
        const rm::UnitTypeIndex type =
            scene.catalog.add(&scene.definitions.back(), gAppTickRate);
        scene.setBatchForType(type, scene.batches.size() - 1);
        scene.setPathForType(type, blueprintPath);
        scene.setTypeTraits(type, move.maxSlopeDegrees, move.maxWaterDepthElmos,
                            unit->def.meshToElmos);

        found = scene.typeForBlueprint.emplace(std::string{blueprintPath}, type).first;
    }

    const rm::UnitTypeIndex type = found->second;
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
        motion.speedPerTick = gAppTickRate.perTick(def.speedElmosPerSecond);
        if (def.turnRateRadiansPerSecond > 0.0f) {
            motion.turnPerTick = gAppTickRate.bradPerTick(def.turnRateRadiansPerSecond);
        } else {
            motion.turnPerTick =
                gAppTickRate.bradPerTick(rm::sim::kDefaultTurnRateRadiansPerSecond);
        }
    }

    const rm::sim::UnitId id = scene.store.spawn(rm::sim::UnitStore::Spawn{
        .type = type,
        .transform = transform,
        .motion = motion,
        .health = rm::sim::Health{.current = def.health, .maximum = def.health},
    });

    // `UnitCreated` is THE CALLER'S to raise (§7 P6.1). The sim never spawns a unit — a spawn
    // needs a model out of the VFS, which is exactly the line the sim does not cross — so this
    // is the one event kind that cannot come from a pass. Raised here rather than at each of
    // the three spawn sites, because this is the one that loads content.
    scene.events.emit(rm::sim::Event{
        .kind = rm::sim::EventKind::UnitCreated,
        .unit = id,
        .army = motion.armyIndex,
        .at = {transform.x, transform.y, transform.z},
    });

    // The store may have grown its arrays, so any span into them is stale — which for the
    // sim means the caller-side tick has to re-read them, and for the renderer means the
    // draw gather has to run again before the batch spans are trusted.
    scene.grewThisTick = true;

    return id;
}

/// Finds (or loads and registers) the buildable entry for `blueprintPath`, so every
/// Construction of the same blueprint shares one definition and one index.
/// Registers a blueprint as buildable and returns its TYPE INDEX.
///
/// A `UnitTypeIndex`, not an index into a private list — which is what closes the `Build` hole
/// in `sim::applyCommand`. There used to be two registries: `scene.buildable` for things under
/// construction and `scene.definitions`/`scene.catalog` for things standing on the map, with a
/// `Construction::blueprintIndex` meaning "an index into whatever list the caller is building
/// from". The sim could not create a construction because it had no way to name a blueprint the
/// caller would recognise.
///
/// One registry fixes that: a type index means the same thing to the sim, the catalog and the
/// draw gather, and `buildablePaths` is now indexed BY type index so a finished construction can
/// still find its model.
[[nodiscard]] std::optional<rm::UnitTypeIndex> resolveBuildable(UnitScene& scene,
                                                                const rm::vfs::Vfs& content,
                                                                std::string_view blueprintPath) {
    // ONE REGISTRY (`#3090`). This used to keep its own `scene.buildable` list and return an
    // index into it, typed `rm::UnitTypeIndex` — a different number from the catalog's under the
    // same type name, which is why routing a build order through `sim::applyCommand` turned a
    // 36-mass extractor into an 18,000-mass experimental.
    //
    // Now a buildable type is a type: registered with the catalog like anything else, with NO
    // BATCH until something of it is actually built. That is what `Scene::batchForType` bought —
    // the gather no longer assumes a type index is a batch index, so a type may exist with
    // nothing to draw it, which is precisely what "buildable but not yet built" means.
    for (std::size_t type = 0; type < scene.pathForType.size(); ++type) {
        if (scene.pathForType[type] == blueprintPath) {
            return static_cast<rm::UnitTypeIndex>(type);
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

    scene.definitions.push_back(*def);
    const rm::UnitTypeIndex type = scene.catalog.add(&scene.definitions.back(), gAppTickRate);
    scene.setPathForType(type, blueprintPath);
    // The traits too, even though nothing of this type is standing yet: they are indexed by
    // type, so leaving a hole would make every LATER type read the wrong slope limit and the
    // wrong model scale. That is the bug this refactor introduced and then caught — see
    // `Scene::setTypeTraits`.
    const rm::data::MoveDef move = rm::data::moveDefFor(*def);
    scene.setTypeTraits(type, move.maxSlopeDegrees, move.maxWaterDepthElmos, def->meshToElmos);
    // Deliberately NO `setBatchForType`: nothing of this type exists yet. `spawnUnit` records
    // the batch when the first one is built.
    return type;
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
std::size_t adoptOwnerlessUnits(UnitScene& scene) {
    if (scene.playerArmy == rm::sim::kNoArmy) {
        return 0;  // an observer owns nothing, which is the point of `--observer`
    }

    std::size_t adopted = 0;
    for (rm::sim::MoveState& motion : scene.store.motion()) {
        if (motion.armyIndex == rm::sim::kNoArmy) {
            motion.armyIndex = scene.playerArmy;
            ++adopted;
        }
    }
    return adopted;
}

void orderFirstExtractors(UnitScene& scene, std::span<const rm::scenario::Marker> markers,
                          const rm::vfs::Vfs& content) {
    if (scene.armies.empty()) {
        return;
    }

    // The extractor blueprint, PER FACTION, from the opening's first step — which is what
    // P3.3 bought. This used to register one UEF path for everybody, with a comment saying
    // that four blueprints "would demonstrate nothing the one does not". Four blueprints is
    // now zero blueprints: the plan says `role = 'extractor'` and the roster says which.
    //
    // Resolved per army below rather than once here, because the answer differs by faction.
    if (scene.opening.structures.empty()) {
        return;

    }

    // Which commander is where, so each can be sent to its OWN nearest deposit rather than
    // all of them to one.
    float firstBuilderRate = 1.0f;
    // The last extractor's figures, for the summary line. Reported rather than assumed
    // identical across factions, because they are not: each faction's mex costs its own.
    rm::sim::Mag lastCost{};
    rm::sim::Mag lastEnergy{};
    rm::sim::Mag lastTime{};
    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr || !def->isBuilder()) {
            continue;
        }
        const int army = scene.store.motion()[slot].armyIndex;
        if (army == rm::sim::kNoArmy
            || static_cast<std::size_t>(army) >= scene.armies.size()) {
            continue;
        }
        firstBuilderRate = std::max(1.0f, def->buildRate);

        // PER FACTION. The extractor this commander builds is its own faction's, resolved from
        // the opening's first step through the roster — which is the whole of what P3.3 bought.
        const std::string blueprint = blueprintFor(
            scene, scene.armies[static_cast<std::size_t>(army)],
            extractorStep(scene.opening));
        const std::optional<rm::UnitTypeIndex> registered =
            blueprint.empty() ? std::nullopt : resolveBuildable(scene, content, blueprint);
        if (!registered) {
            continue;  // this faction fields no extractor this engine can read
        }
        const rm::unitdef::UnitDef* extractor = scene.catalog.def(*registered);

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
            // A mass deposit's position comes out of the map file as floats, so it converts
            // HERE — in the app, which is the legitimate boundary — rather than being carried
            // into the sim as one (§7 P10.0).
            .position = {rm::sim::fxFromFloat(nearest->position[0]),
                         rm::sim::fxFromFloat(nearest->position[1]),
                         rm::sim::fxFromFloat(nearest->position[2])},
            .cost = {.mass = extractor->buildCostMass,
                     .energy = extractor->buildCostEnergy},
            .buildTimeRemaining = extractor->buildTime,
            .totalBuildTime = extractor->buildTime,
            .buildPerTick = gAppTickRate.magPerTick(def->buildRate),
            .blueprintIndex = *registered,
        });

        lastCost = extractor->buildCostMass;
        lastEnergy = extractor->buildCostEnergy;
        lastTime = extractor->buildTime;
    }

    std::printf("economy: %zu extractor(s) ordered on the map's own deposits,"
                " %.0f mass / %.0f energy each over %.0fs\n",
                scene.building.size(),
                static_cast<double>(rm::sim::magToFloat(lastCost)),
                static_cast<double>(rm::sim::magToFloat(lastEnergy)),
                static_cast<double>(rm::sim::magToFloat(lastTime) / firstBuilderRate));
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
        // From the MOTION CLASS (P3.4). A decorative instance with no definition gets the
        // immobile MoveDef, which uses no ground grid — correct, since nothing routes it.
        const rm::data::MoveDef move =
            def.has_value() ? rm::data::moveDefFor(*def)
                            : rm::data::moveDefFor(rm::unitdef::MotionType::None);

        // The TYPE for these units. One per batch, and the two indices are the same number
        // by construction — which is the whole reason the old parallel-array hazard here is
        // gone: there is one array to append to, not four that had to be appended to
        // together or the next batch wrote its entries at the wrong index and read health
        // off the end. (That was a segfault the moment `--units` and `--skirmish` were given
        // together.)
        rm::UnitTypeIndex type = 0;
        if (def) {
            scene.definitions.push_back(*def);
            type = scene.catalog.add(&scene.definitions.back(), gAppTickRate);
        } else {
            type = scene.catalog.add(nullptr);  // a bare model: it neither fires nor dies
        }
        // The batch for this type is the one pushed below, so the mapping is recorded there
        // rather than asserted here. The assertion this replaces — `type == batches.size()` —
        // is the one that made a buildable type impossible to register early (`#3090`).
        scene.setTypeTraits(type, move.maxSlopeDegrees, move.maxWaterDepthElmos,
                            def.has_value() ? def->meshToElmos : 1.0f);

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
                    state.speedPerTick = gAppTickRate.perTick(def->speedElmosPerSecond);
                    state.turnPerTick = gAppTickRate.bradPerTick(
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
        scene.setBatchForType(type, scene.batches.size() - 1);
    }

    scene.interpolate = gInterpolate;

    // The batches' instance spans, filled from a snapshot now that every unit is in the store.
    // Published twice so `previous` and `current` agree: nothing has moved yet.
    scene.publish(0);
    scene.publish(0);
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

} // namespace rm::app
