#pragma once

#include "core/sim/Fx.hpp"

#include "core/lua/LuaTable.hpp"
#include "core/unit/Weapon.hpp"
#include "core/vfs/AssetSearch.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unitdef {

// What a unit moves through, and this engine's authority on where it may go.
//
// Supreme Commander's own names, because it is the family that HAS this concept:
// a `.bp` states `Physics.MotionType = 'RULEUMT_Land'` and states no slope or
// wading limit at all, so the class is the whole of what the file says about
// passability. All 568 shipped unit blueprints carry one, and these eight values
// are the complete set (None 374, Air 60, Land 50, Water 27, Hover 19,
// Amphibious 17, SurfacingSub 13, AmphibiousFloating 8).
//
// BAR definitions describe the same thing differently — a per-unit `maxslope`
// and `maxwaterdepth`, plus `canfly` — so its loader fills this in from what it
// does say, and everything downstream asks one question of both families.
enum class MotionType : std::uint8_t {
    None,                ///< immobile. Buildings, and 374 of the 568 blueprints
    Land,                ///< ground only; water of any depth is out
    Air,                 ///< flies, and is not subject to a ground grid at all
    Water,               ///< surface ships — water ONLY, and cannot come ashore
    Hover,               ///< ground and water surface alike
    Amphibious,          ///< ground, and the seabed underneath the water
    AmphibiousFloating,  ///< ground, and floating on the surface
    SurfacingSub,        ///< submarines: submerged, surfacing to fire
};

/// The motion type a `.bp`'s `RULEUMT_*` string names, or nullopt for a string
/// this does not know. Nothing in the retail corpus is unknown — the check
/// exists so a mod that invents a class says so rather than being read as
/// immobile, which would look like a unit that simply refuses to move.
[[nodiscard]] std::optional<MotionType> motionTypeFromName(std::string_view name) noexcept;

/// Whether a motion type moves over ground the passability grid describes.
///
/// The grid answers "may a ground unit stand here", so this is the question of
/// whether that grid is the right tool at all. False for `Air` (no ground
/// involved), for `None` (nothing moves), and for `Water` and `SurfacingSub` —
/// those two need the INVERSE of the grid, water deep enough rather than
/// shallow enough, which this engine cannot yet express. Routing a ship over the
/// ground grid would path it across dry land, so it is refused rather than
/// approximated.
[[nodiscard]] bool travelsOnGround(MotionType motion) noexcept;

/// One ordinary FA bubble shield as authored by `Defense.Shield`.
/// `ShieldSize` is a diameter: Shield.lua creates its sphere at `Size / 2` (`:816-821`).
struct ShieldSpec {
    sim::Mag maximum{};
    sim::Fx radiusElmos{};
    sim::Fx verticalOffsetElmos{};
    float regenPerSecond = 0.0f;
    sim::Seconds regenDelay{};
    sim::Seconds rechargeDelay{};

    [[nodiscard]] bool exists() const noexcept {
        return maximum > sim::Mag{} && radiusElmos > sim::Fx{};
    }
};

// What a unit *is*, read from the game's own unit definitions rather than
// hardcoded here.
//
// Until now the sim moved everything at BAR's Pawn's speed, because one unit
// had been read by hand. A BAR unit definition is a flat Lua data table —
//
//     return { armpw = { speed = 87, maxslope = 17, objectname = "Units/ARMPW.s3o", ... } }
//
// — which is exactly what core/lua parses, so this is a data slice rather than
// a behavioural one, and it follows the same rule as every other loader here:
// formats convert, behaviour gets reimplemented.
//
// Only the fields this engine can act on are read. A definition carries dozens
// more (weapons, build options, costs, categories) and there is nothing to do
// with them yet; adding a field here should mean something reads it.
struct UnitDef {
    std::string name;       ///< the table's own key, e.g. "armpw"
    std::string modelPath;  ///< `objectname`, e.g. "Units/ARMPW.s3o"

    /// The display name a player reads — "Mass Extractor" — or empty when the content states
    /// none. Supreme Commander's `Description` field carries it inline (567 of 568 blueprints,
    /// English fallback after the `<LOC key>` prefix). BAR keeps names outside the unit files
    /// (`language/en/units.json`), which this loader does not read yet — a BAR unit therefore
    /// shows its id, exactly what every unit showed before this field existed.
    std::string description;

    /// The strategic icon's name — `icon_land1_directfire` — or empty when the content states
    /// none (18 of 568, plus every BAR unit). What the map shows where a unit is too small to
    /// read: 550 blueprints declare one of 100 distinct names encoding class, tier and role,
    /// and the glyphs ship at `/textures/ui/common/game/strategicicons/<name>_rest.dds`. A
    /// unit without one keeps the plain team-colour square, which is the honest fallback.
    std::string strategicIcon;

    /// Elmos per second. Recoil's modern `speed` field is already per second;
    /// only the legacy `maxVelocity` is per frame (UnitDef.cpp:442-443).
    float speedElmosPerSecond = 0.0f;

    /// Radians per second, converted out of the circle divisions per frame the
    /// file is authored in — `turnrate / 65536 * 2*pi * 30`
    /// (SpringMath.h:16-17, GlobalConstants.h:52).
    float turnRateRadiansPerSecond = 0.0f;

    /// As authored, in the degrees the passability grid expects.
    float maxSlopeDegrees = 0.0f;
    float maxWaterDepthElmos = 0.0f;

    /// Footprint in heightmap SQUARES, already scaled by the engine's
    /// SPRING_FOOTPRINT_SCALE of 2 (UnitDef.cpp:671-672, GlobalConstants.h:17)
    /// — so a `footprintx` of 2 in the file is 4 squares, or 32 elmos, here.
    ///
    /// The BUILD-grid quantity: whole squares a unit occupies. A `.scmap` unit
    /// states this separately from its collision size and only when it has one
    /// (363 of 568, essentially the structures), so for the rest this is derived
    /// from the size below rather than read.
    int footprintSquaresX = 0;
    int footprintSquaresZ = 0;

    /// How much room the unit takes up, in elmos, for collision and separation.
    ///
    /// STORED rather than derived from the footprint squares above, because the
    /// two families disagree about whether the quantity is an integer. BAR's
    /// footprint is whole squares and this is half its larger side, exactly as
    /// before. Supreme Commander states `SizeX`/`SizeZ` in fractional ogrids —
    /// 418 of 568 are fractional and 154 are under a single ogrid, the smallest
    /// 0.01 — so rounding them into squares would inflate the smallest unit's
    /// radius from 0.04 elmos to 4, a hundredfold, and pack a crowd of them
    /// against a spacing none of them needs.
    float collisionRadiusElmos = 0.0f;

    /// How TALL the collision box is, in elmos — `SizeY`, the axis the radius above discards.
    ///
    /// READ FOR ONE REASON: a sensor is not on the ground. The raycast sight model
    /// (`sim::Intel.hpp`, `VisionStyle::Recoil`) asks where the eye IS, and it was being handed
    /// the unit's own transform — the terrain height under its feet — so every unit in the game
    /// looked out from ankle level and a commander could not see over a rise its own head
    /// cleared. The top of the collision box is the honest stand-in: the blueprints state no
    /// sensor mount, and this is the only height they state at all.
    ///
    /// Zero for content that omits `SizeY`, which reads as ground level and is exactly the old
    /// behaviour — a silent default that changes nothing rather than a guess.
    float sizeYElmos = 0.0f;

    /// How tall the unit LOOKS, in elmos — `Physics.MeshExtentsY`, or the collision height when
    /// the blueprint states no extents.
    ///
    /// A DIFFERENT QUESTION FROM `sizeYElmos`, and the shipped corpus makes the difference
    /// plain: a UEF land factory is `SizeY = 0.6` and `MeshExtentsY = 4.5`, because its
    /// collision box is the low apron tanks drive over and its mesh is the whole gantry above.
    /// Sight wants the first (an eye sits on the body); the construction effect wants the
    /// second, because it sweeps a plane up the MODEL and a plane that finished at 4.8 elmos
    /// would leave a factory's roof standing complete before the work was a fifth done.
    float meshHeightElmos = 0.0f;

    /// What this unit moves through. See MotionType: for the Supreme Commander
    /// family it is read from the file, for BAR it is inferred from the fields
    /// that family does state.
    MotionType motion = MotionType::None;

    // --- intel -------------------------------------------------------------
    //
    // How far this unit can see, and by what means (ADR-037). Elmos, converted at parse
    // time — a `.bp` states ogrids and BAR states elmos, the same split as every other
    // distance in this struct.
    //
    // MOHO'S NAMES, per the plan's rule that a native field carries the original's
    // spelling and units: `VisionRadius`, `WaterVisionRadius`, `RadarRadius`,
    // `SonarRadius`. BAR's `sightdistance`, `radardistance` and `sonardistance` are read
    // into the first, third and fourth — the same quantities under different spellings.
    //
    // ZERO IS THE DEFAULT AND IT MEANS ZERO. 177 of the 568 blueprints declare no
    // VisionRadius at all — wrecks, props, walls — and a unit that states no radius has
    // none. Defaulting to anything else would hand sight to things that never had it, and
    // the symptom (an enemy base visible for no reason) points nowhere near the cause.
    //
    // WHAT THE CORPUS DECLARES, measured over the 568: VisionRadius 391, WaterVisionRadius
    // 70, SonarRadius 67, RadarRadius 57, OmniRadius 17, RadarStealth 15, SonarStealth 9,
    // FreeIntel 8, the two stealth FIELD radii 7 each, JamRadius 7 — and CloakFieldRadius
    // **zero**, which is worth recording because ADR-037's follow-up list named it as one of
    // the nine. Retail ships no cloak field; only `Cloak` on 4 units.
    //
    // Four of the remaining ones are still out, each for a reason rather than for tidiness.
    // The two stealth FIELDS and the jammer act on OTHER units, so they are a second grid and
    // a contact rule rather than a field here — and `JamRadius` is a TABLE in the blueprints,
    // not a scalar, so even reading it is a different job. `WaterVisionRadius` is read below
    // and unused: nothing in this sim is submerged, so a grid for it would have nothing to
    // answer about and no test that could tell it from an empty one.
    float visionRadiusElmos = 0.0f;

    /// How far it sees UNDER water, which Forged Alliance treats as its own sense.
    ///
    /// BAR has no equivalent and this stays zero for that family rather than being filled
    /// in from the land radius: BAR expresses underwater detection through sonar, which
    /// 148 of its units carry, and copying `sightdistance` here would give every tank
    /// submarine detection.
    float waterVisionRadiusElmos = 0.0f;

    float radarRadiusElmos = 0.0f;
    float sonarRadiusElmos = 0.0f;

    /// How far it sees EVERYTHING, cloaked and stealthed alike. 17 units declare one.
    ///
    /// A SENSE OF ITS OWN rather than a bigger vision radius, because what makes omni omni is
    /// not its reach — several are shorter than the same unit's radar — but that nothing hides
    /// from it. It is the counter the stealth flags below exist to have.
    float omniRadiusElmos = 0.0f;

    /// Whether this unit is invisible to the sense named, to anyone without omni.
    ///
    /// FLAGS RATHER THAN RADII, which is what the blueprints state: `RadarStealth = true` on
    /// 15 units, `SonarStealth = true` on 9, `Cloak = true` on 4. A stealthed unit is not
    /// harder to detect, it is ABSENT from that sense — which is why these are read at contact
    /// time rather than subtracted from anybody's radius.
    bool radarStealth = false;
    bool sonarStealth = false;
    bool cloak = false;

    /// Whether everyone always knows where it is, regardless of any sense. 8 units declare it.
    ///
    /// Civilian objectives and campaign markers, mostly — a thing the scenario wants on every
    /// player's map from the first tick. It BEATS stealth, because a blueprint stating both is
    /// stating that this particular object is meant to be seen.
    bool freeIntel = false;

    /// The stealth FIELDS: everything of this unit's own alliance inside the radius is
    /// absent from the named sense, exactly as the per-unit flags make their one carrier.
    /// Seven retail units declare each (the B4203 stealth generators). CloakFieldRadius
    /// appears ZERO times in retail — measured, and the reason there is no field for it.
    float radarStealthFieldRadiusElmos = 0.0f;
    float sonarStealthFieldRadiusElmos = 0.0f;

    /// The jammer: `JammerBlips` false radar contacts scattered within `JamRadius.Max` of
    /// the carrier, shown to any hostile radar that covers it. Seven retail units. The
    /// blueprint states the radius as a {Min, Max} table; Max is read because every retail
    /// pair is equal and the larger bound is the honest reach of a deception.
    float jamRadiusElmos = 0.0f;
    int jammerBlips = 0;

    // --- economy -----------------------------------------------------------
    //
    // What it costs to make and what it makes. All four are stated by essentially every
    // shipped unit (718 state a cost, 703 a build time), which is why they are read
    // unconditionally rather than behind a check.

    /// What building this costs in total, and how many build units of work it takes.
    /// A builder's `buildRate` divided into `buildTime` gives the seconds.
    /// FIXED POINT (`Mag`), converted at parse time. `BuildCostEnergy` reaches 10,008,000 in
    /// the corpus (XSB2401), which is why these are magnitudes rather than geometry.
    sim::Mag buildCostMass{};
    sim::Mag buildCostEnergy{};
    sim::Mag buildTime{};

    /// How fast this unit builds, in build units per second. 143 units state one — the
    /// commanders, engineers and factories. Zero means it cannot build.
    float buildRate = 0.0f;

    /// What it produces per second once standing. 37 units make mass and 33 make energy;
    /// a mass extractor is 2 a second, a power generator 20.
    float producesMassPerSecond = 0.0f;
    float producesEnergyPerSecond = 0.0f;

    /// What it costs to RUN, per second, once standing. Energy only: 104 units state
    /// `MaintenanceConsumptionPerSecondEnergy` and not one states a mass counterpart.
    ///
    /// A mass extractor burns 2 a second against the 2 mass it makes, so an economy that
    /// ignores this runs richer than the game's — and it is what makes power generation a
    /// decision rather than a formality.
    float upkeepEnergyPerSecond = 0.0f;

    /// How much of each it lets its owner hold. 62 units state storage.
    sim::Mag storageMass{};
    sim::Mag storageEnergy{};

    /// What this unit's WRECK is worth: `BuildCost* × Wreckage.*Mult`, computed at parse
    /// time so the sim never multiplies a float (`Unit.lua:1769-1770`). Measured across the
    /// corpus: 504 of 568 blueprints state a Wreckage table and every one says
    /// `MassMult = 0.9, EnergyMult = 0` — and the 64 without one (the ACUs, the walls)
    /// leave nothing to reclaim, which is why the default is zero rather than 0.9
    /// (`Unit.lua:1762-1765` makes no wreck at all for them).
    ///
    /// Deliberately NOT applied here: FAF's tech-tier discount (0.9/0.8/0.7/0.6,
    /// `Unit.lua:1776-1792`, a FAF addition rather than retail), the submerged 0.6, and the
    /// overkill/fraction-complete scaling — all need state the parse cannot see.
    sim::Mag wreckMass{};
    sim::Mag wreckEnergy{};

    /// The value one point of a reclaimer's `BuildRate` recovers per second, as a ratio.
    ///
    /// `Prop.lua:270-287`: seconds = TimeReclaim × value / (10 × BuildRate), so the rate is
    /// 10 / TimeReclaim — and a unit wreck's TimeReclaim is `ReclaimTimeMultiplier × 2`
    /// (`Unit.lua:1771`, the global "reclaim is twice as slow" balance knob), which makes
    /// this 5 for every wreck in the corpus. The 10 is FA's own tick rate, a constant of
    /// the formula rather than of our clock.
    sim::Fx reclaimPerBuildRate{};

    /// How far this unit can build, repair and reclaim, in elmos.
    /// `Economy.MaxBuildDistance` × 8 — only 10 of 568 blueprints state one (the ACUs say
    /// 10, the UEF T1 engineer 5); everything else inherits the engine's default of 5
    /// ogrids, which `blueprints-units.lua:250` spells out as the fallback.
    float buildDistanceElmos = 40.0f;

    // --- adjacency ---------------------------------------------------------

    /// The SKIRT — the concrete apron around a structure, in ogrids (`Physics.SkirtSize*`).
    /// Adjacency is decided skirt-to-skirt, not footprint-to-footprint: the game snaps
    /// structures to abutting skirts, and the buff tables key on the contact. Zero for
    /// everything mobile and for structures that state none.
    float skirtSquaresX = 0.0f;
    float skirtSquaresZ = 0.0f;

    /// Which adjacency buff table this structure GRANTS to its neighbours — the root
    /// `Adjacency` field, e.g. `T1PowerGeneratorAdjacencyBuffs`. Empty for the many
    /// structures that grant nothing (factories receive; they do not give).
    std::string adjacencyBuffs;

    /// Whether this unit can build anything at all.
    [[nodiscard]] bool isBuilder() const noexcept { return buildRate > 0.0f; }

    // --- level of detail -----------------------------------------------------
    //
    // `Display.Mesh.LODs`: the finest level's cutoff and whether a coarser mesh follows.
    // 563 of 568 blueprints declare a cutoff; the coarse mesh itself is found by convention
    // (`<id>_lod1.scm`, resolveMeshInVfs level 1) and usually brings its own albedo.
    // The cutoff's unit is the engine's own camera-distance figure; the renderer scales it
    // once into elmos (Scene::kLodElmosPerCutoff) — labelled a calibration, not a fact.
    float lodCutoff = 0.0f;
    bool hasLod1 = false;
    std::string lod1Albedo;   ///< `LODs[2].AlbedoName`, or empty to share the fine albedo
    std::string lod1Spec;     ///< `LODs[2].SpecularName`, same fallback

    // --- threat ------------------------------------------------------------
    //
    // The blueprint's own estimate of how dangerous this unit is, per domain —
    // `Defense.*ThreatLevel`. Authored for exactly one consumer: the AI, whose builder
    // conditions and platoon formation compare summed threat against thresholds. A tank is
    // 1, a T2 point defence 8, an ACU 60 — the scale is the corpus's own and is only ever
    // compared with itself.
    float surfaceThreat = 0.0f;
    float airThreat = 0.0f;
    float subThreat = 0.0f;
    float economyThreat = 0.0f;

    /// What this unit UPGRADES INTO — `General.UpgradesTo`, upper-cased to match `name`.
    /// The tech path for factories (UEB0101 -> UEB0201 -> UEB0301) and extractors; empty
    /// for the majority that upgrade into nothing. The sim reads it to tell an upgrade
    /// order from a build order: same command, different completion.
    std::string upgradesTo;

    // --- categories --------------------------------------------------------
    //
    // What the blueprint SAYS this unit is, as a sorted list of the tags it declares.
    // `07-ai-and-gamesetup.md §4.2` shows the shape: a Cybran T2 tank carries
    // `BUILTBYTIER2FACTORY`, `CYBRAN`, `DIRECTFIRE`, `LAND`, `MOBILE`, `TANK`, `TECH2` and
    // six more. Every classification this engine makes above the sim is derived from these.
    //
    // SORTED, so `hasCategory` is a binary search rather than a scan — it is asked once per
    // category per unit while an expression is evaluated over the whole corpus, which is
    // 568 units times a handful of terms times every builder.

    /// The tags, sorted and deduplicated. Empty for content that declares none, which is
    /// every BAR unit — that family states its kind differently, and an empty list is the
    /// honest report of that rather than a guess.
    std::vector<std::string> categories;

    /// What this unit can build, as the EXPRESSION the blueprint states rather than a list.
    ///
    /// `Economy.BuildableCategory`, where a space means AND and the list means OR — see
    /// `core/unit/BuildTree.hpp` for why that is the single largest converter requirement.
    /// Empty for the 463 shipped units that build nothing.
    ///
    /// Kept as the expression and not resolved here, because resolving needs the WHOLE unit
    /// set: "which units is this builder allowed to make" is not a fact about this blueprint.
    std::vector<std::vector<std::string>> buildableCategory;

    /// Expressions a commander UPGRADE would add — `BuildableCategoryAdds`.
    ///
    /// Parsed and kept, not applied. `07 §4.3` says commander upgrades are out of scope for
    /// milestone 1 "but must be recorded as deferred", so this is that record: when upgrades
    /// land, applying one is a set union on `BuildTree` rather than a new mechanism.
    std::vector<std::vector<std::string>> buildableCategoryAdds;

    /// Whether this unit declares a tag. Case-sensitive: the corpus is consistently upper
    /// case, and a case-insensitive compare would hide a typo in a data file rather than
    /// failing on it.
    [[nodiscard]] bool hasCategory(std::string_view tag) const noexcept;

    /// Whether it declares ALL of them. What one `BuildableCategory` term needs, since a
    /// space in that expression means AND (`07 §4.3`).
    [[nodiscard]] bool hasAllCategories(std::span<const std::string_view> tags) const noexcept;

    /// What this unit shoots with. Empty for the 321 shipped units that shoot nothing,
    /// and for every BAR unit — that family states its weapons in a shape this engine
    /// does not read yet, and an empty list is the honest report of that.
    std::vector<Weapon> weapons;

    /// The unit's longest reach, in elmos, over the weapons it will actually fire.
    /// Zero for something unarmed. What a targeting sweep needs before it looks at
    /// individual weapons.
    [[nodiscard]] sim::Fx maxWeaponRange() const noexcept;

    /// The factor that takes the MESH's own coordinates to elmos.
    ///
    /// One number holding what is two conversions in the file, deliberately.
    /// A `.s3o` is authored in elmos already, so BAR's is 1. A `.scm` is authored
    /// in whatever the artist used and the blueprint's `Display.UniformScale`
    /// takes it to OGRIDS, which are 8 elmos each — so a medium tank's 0.07 is
    /// really 0.56, and applying only the first step leaves it an eighth of its
    /// size. That mistake has already been made once here, on the props
    /// (AGENT.md), and it presents as a scatter of specks rather than as a scale
    /// bug. Combining the two removes the chance to make it again.
    ///
    /// Cross-checked against the mesh: UEL0201's geometry is 8.09 x 11.93 units,
    /// which at 0.07 is 0.57 x 0.84 ogrids against the 0.7 x 0.9 collision box the
    /// same blueprint declares — a box slightly larger than the model it holds,
    /// which is what a collision box should be.
    float meshToElmos = 1.0f;

    /// FIXED POINT (`Mag`), converted at parse time — the sim never sees the float
    /// (PLAN2.md §5.1). `Mag` because `MaxHealth` reaches 5,000,000 in the corpus.
    sim::Mag health{};

    /// Ordinary projectile-colliding bubble; personal and transport shields remain zero.
    ShieldSpec shield;

    /// What this unit is made of, for the damage table — `Defense.ArmorType`
    /// (`14-blueprint-census.md §8.7`: 604 of 606 blueprints state one).
    ///
    /// **A NAME, NOT AN `ArmorClass`, and deliberately** (PLAN2.md §7 P10.1). Resolving it
    /// here would mean handing every blueprint parser an `ArmorRegistry`, and a parser that
    /// needs a registry is a parser a test cannot call with a Lua table and nothing else.
    /// Resolution happens once in `sim::UnitCatalog`, which is already the place where content
    /// becomes sim-ready — it is where a blueprint's per-second rates become per-tick ones for
    /// exactly the same reason.
    ///
    /// Empty means the blueprint stated none, which resolves to `default`. BAR blueprints do
    /// not carry this field at all; their armour classes come from `armordefs.lua` the other
    /// way round, as a class listing the units that belong to it, and the importer inverts it
    /// into this field.
    std::string armorType;

    /// Whether this unit flies.
    ///
    /// Worth carrying because an aircraft is not a ground unit with wings: BAR's
    /// flyers set `canfly` and carry a `turnradius` INSTEAD of a `turnrate`, so
    /// they have no turn rate to read at all. They also have no business being
    /// routed over a ground passability grid.
    bool canFly = false;

    /// Whether this definition describes something that moves. Buildings share
    /// the format and simply have no speed, and treating one as a stationary
    /// unit with a zero turn rate is more useful than rejecting the file.
    [[nodiscard]] bool isMobile() const noexcept { return speedElmosPerSecond > 0.0f; }

    /// Footprint radius in elmos — half the larger side.
    ///
    /// The BAR derivation, kept because it is how that family's `collisionRadiusElmos`
    /// is arrived at and a test asserts the two agree. Prefer the stored radius:
    /// this one has no answer for a unit whose size was never whole squares.
    [[nodiscard]] float footprintRadiusElmos() const noexcept;
};

/// Reads every unit a `.lua` definition file declares.
///
/// Usually one, keyed by name — `return { armpw = { ... } }` — so the name comes
/// from the file's CONTENTS rather than its filename; the two agree throughout
/// BAR, but only one of them is what the engine keys on. Twenty-six BAR files
/// declare several units at once, which is why this returns a list.
///
/// Refuses a file whose top-level table mixes in non-table entries. That is the
/// shape of the thirteen BAR files which BUILD their units in a loop
/// (`local def = { maxacc = 0, ... }` and so on) — Lua programs, which this
/// reader is documented not to evaluate. Without the check, the first key in
/// such a file ("maxacc") would be read as a unit's name.
[[nodiscard]] std::expected<std::vector<UnitDef>, lua::ParseError> loadFileAll(
    const std::filesystem::path& path);

/// The first unit a file declares — the common case, where there is only one.
[[nodiscard]] std::expected<UnitDef, lua::ParseError> loadFile(const std::filesystem::path& path);

/// Finds the model file a definition names, searching the asset roots.
///
/// Resolution is by BASENAME and case-insensitive: definitions say
/// `Units/ARMPW.s3o` while the file on disk is `armpw.s3o`, and the
/// subdirectory in the name does not always match the one it lives in either.
/// Returns an empty path when nothing matches.
[[nodiscard]] std::filesystem::path resolveModel(const vfs::AssetSearch& search,
                                                 std::string_view objectName);

/// The scripted opponent's wave size, from this unit's own numbers — the model
/// data/opening.lua documents: a commander (12,000 hp, 100 dps) kills one attacker every
/// hp/100 seconds, survivors land dps × killTime × N(N+1)/2, and the smallest N clearing
/// the commander's health at the tank-derived margin wins. Clamped to [5, 60]; a unit with
/// no health or no gun gets the old hand-derived 20 rather than a division by nothing.
///
/// CONTENT ARITHMETIC, deliberately here and not in `core/sim`: it reads authored floats
/// and runs once when a wave unit resolves, which is the load-time side of the fixed-point
/// boundary (§5.2) — the sim only ever sees the resulting count.
[[nodiscard]] std::size_t waveSizeFor(const UnitDef& unit) noexcept;

} // namespace rm::unitdef
