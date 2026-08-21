#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/TickRate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unitdef {

// How much a weapon hurts a particular kind of target (PLAN2.md §7 P10.1, `ADR-033`, D12).
//
// WHY THIS EXISTS. A weapon dealt one number, so every unit was a damage-per-second figure and
// unit composition could not matter. Rock-paper-scissors in an RTS *is* the damage table: an
// anti-air missile that does a tenth of its damage to ground is the same weapon entry with a
// different vector, and with a scalar there is nowhere to write that down.
//
// ---------------------------------------------------------------------------------------
// WHY THIS SHAPE, AND NOT EITHER REFERENCE ENGINE'S. Read this before "simplifying" it.
// ---------------------------------------------------------------------------------------
//
// The two content families model the same idea transposed, and the whole design question is
// which of them the RUNTIME should look like:
//
//   **Recoil/BAR** — a unit declares an armour class; a weapon's `damage = { class = n }` table
//   holds an ABSOLUTE figure per class (`Sim/Misc/DamageArray.h`, `WeaponDef.cpp:432-437`).
//
//   **Supreme Commander** — a unit declares `Defense.ArmorType`; a weapon declares `DamageType`;
//   `lua/armordefinition.lua` holds `[ArmorType][DamageType] -> MULTIPLIER`, and an unlisted
//   pair means 1.0.
//
// We take Recoil's shape — absolute damage per class — and TRANSPOSE FA's matrix into it at
// import. The measurement that decides it, counted directly from
// `reference/FAF-fa/lua/armordefinition.lua:48-118` rather than taken from a summary:
//
//   * 8 armour classes x 9 damage types.
//   * **Exactly 10 non-1.0 entries**, taking 4 distinct values (0.0, 0.032, 0.25, 0.55).
//   * The damage types that appear at all are Normal (1.0 everywhere), Overcharge, Deathnuke,
//     ExperimentalFootfall, CzarBeam and TacticalMissile.
//
// So when the matrix is flattened into weapons, **about 16 of the 494 shipped weapons acquire
// any override at all** and the other 478 stay a bare scalar — exactly what they are today.
// The sparse form is therefore not a space optimisation; it is what the data actually is.
//
// (Report `02 §9.6` estimates "~6 non-1.0 multipliers". Counted, it is 10 entries / 4 distinct
// values. Its conclusion — "a clean transpose", "one of the cheapest high-value wins" — is
// unaffected and correct. Recorded because a later reader will find the two numbers.)
//
// WHAT WE REFUSED, and why:
//
//   * **Keeping FA's shape** — weapon carries damage + a damage type, a global matrix is
//     consulted at impact. Preserves the authored structure, but costs a matrix lookup per
//     damage event, and BAR content has no damage types AT ALL, so importing BAR would mean
//     inventing a synthetic one per weapon. The transpose runs once at load instead.
//   * **Copying `DamageArray` literally.** It is `std::vector<float> damages` — a heap
//     allocation per weapon def and a pointer chase per lookup, for a table that is empty in
//     97 % of cases, sized to the number of armour classes whether or not the weapon cares.
//   * **A hash map keyed by class name.** Rejected on the grounds `IdPool.hpp` already gives
//     for rejecting Recoil's `SimObjectIDPool`: a hash container has an iteration order, an
//     iteration order is a determinism hazard, and the replay hash is this project's success
//     criterion. A string hash in the damage inner loop is the smaller of the two objections.

/// The armour classes a match knows about, by name.
///
/// **NAMES IN THE DATA, A DENSE INDEX IN THE TICK.** Resolution happens exactly once, here, at
/// load — the same rule §5.1 applies to durations, for the same reason: a conversion that
/// happens in a tick is a conversion that can differ between two runs of it.
///
/// SORTED, NOT FIRST-SEEN. The index a class gets is a function of the SET of names, not of the
/// order they were encountered in. Recoil does this too (`DamageArrayHandler.cpp:43-45`) and it
/// matters more here than there: first-seen interning would make the numbering depend on load
/// order, so a content change that merely reordered a file would silently renumber every class
/// and change what a recorded match means.
///
/// CASE-FOLDED, WHICH RECOIL IS NOT, and this is a deliberate divergence with a measurement
/// behind it. `14-blueprint-census.md §8.7` found `Structure` on 222 units and `STRUCTURE` on
/// one — the same class spelled two ways. Recoil's `armordefs.lua` keys are case-sensitive, so
/// that one unit would get a private armour class with no multipliers defined against it and
/// would quietly take full damage from everything. Folding costs a `tolower` per character at
/// load and removes a class of content bug that is invisible until someone wonders why one
/// building is unusually fragile.
///
/// A VALUE, not a global (D7). Two matches in one process may have different content mounted.
class ArmorRegistry {
public:
    /// The registry every match has even with no content: just `default`.
    ArmorRegistry();

    /// Builds a registry from the class names a game declares — FA's `armordefinition.lua`
    /// armour-type list, Recoil's `armordefs.lua` keys.
    ///
    /// Names are folded to lower case, sorted, and de-duplicated; `default` is always class 0
    /// whether or not it appears in the list. Names past the 255th are DROPPED rather than
    /// silently aliased, because `ArmorClass` is 8 bits (`core/Types.hpp`) and a class that
    /// cannot be named is better than two classes that share a number.
    [[nodiscard]] static ArmorRegistry fromNames(std::span<const std::string_view> names);

    /// The class with this name, or `kDefaultArmor` when there is none.
    ///
    /// FALLING BACK RATHER THAN FAILING is Recoil's behaviour too — `WeaponDef.cpp:432-437`
    /// silently skips damage-table keys it does not recognise. It is the right default for
    /// content: a blueprint naming an armour class the game does not define is a typo or a
    /// half-installed mod, and the playable answer is "ordinary armour", not "no damage".
    [[nodiscard]] ArmorClass classFor(std::string_view name) const noexcept;

    /// Whether this name is one the registry actually knows.
    ///
    /// Separate from `classFor` precisely BECAUSE that one cannot fail: an importer wants to
    /// report a typo, and it cannot tell a genuine `default` from a fallback otherwise.
    [[nodiscard]] bool knows(std::string_view name) const noexcept;

    /// The name of a class, or an empty view for an index this registry does not hold.
    [[nodiscard]] std::string_view name(ArmorClass armor) const noexcept;

    /// How many classes there are, `default` included. Never zero.
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

private:
    /// `names_[0]` is always `"default"`. `names_[1..]` are sorted, which is what makes
    /// `classFor` a binary search and the numbering independent of load order.
    std::vector<std::string> names_;
};

// What one weapon does to one target, as a value.
//
// FIXED SIZE AND TRIVIALLY COPYABLE, and that is load-bearing rather than tidy. A projectile
// carries one, and `StateHash` walks projectiles every tick: a profile that heap-allocated
// (Recoil's does) would put a pointer in hashed state, and a profile that varied in size would
// make the hash depend on an allocation. Everything else here follows from keeping that true.
struct DamageProfile {
    /// How many per-class overrides fit inline.
    ///
    /// SIX, and here is the arithmetic so the number can be revisited rather than guessed at
    /// again. When FA's matrix is flattened, a weapon's override count is the number of armour
    /// classes whose multiplier for THAT weapon's damage type is not 1.0. Counted from
    /// `armordefinition.lua`: Overcharge hits 3 classes (Structure, ExperimentalStructure,
    /// TMD), Deathnuke 3, ExperimentalFootfall 2, CzarBeam 1, TacticalMissile 1. **The worst
    /// case in the whole of Forged Alliance is 3.**
    ///
    /// Six is that with headroom, chosen for the half of the corpus that is NOT measured: BAR's
    /// `armordefs.lua` is mod-defined and a BAR weapon may list more classes than an FA one
    /// ever needs. When BAR blueprints are loaded for real, re-measure and either shrink this
    /// or accept a spill — but do not make it a `vector`, which is the thing the first
    /// paragraph forbids.
    ///
    /// `addOverride` REPORTS a full profile rather than dropping quietly, so if six is ever
    /// wrong the importer says so instead of the content being subtly mis-tuned.
    static constexpr std::size_t kMaxOverrides = 6;

    /// Damage against any class this profile says nothing else about.
    ///
    /// The whole profile for 478 of Forged Alliance's 494 weapons, which is why it is a plain
    /// member and not `overrides[0]`.
    sim::Mag base{};

    /// How long a hit disables rather than destroys — FA's `EMP` and `Stun` damage types,
    /// Recoil's `paralyzeDamageTime` (`DamageArray.h`).
    ///
    /// **HERE FROM THE START, though nothing reads it yet**, and that is the one speculative
    /// field in this file. The justification is retrofit cost, which is also why P10.1 comes
    /// before P10.3: paralysis is an ACCUMULATOR — damage that fills a meter and drains — so it
    /// cannot be expressed as a smaller `Mag` later. Adding it afterwards means revisiting every
    /// damage call site a second time, and the first time is what this phase is spending.
    ///
    /// SECONDS, not ticks, because this struct is CONTENT: the same split `Weapon::burstDelay`
    /// already makes against `UnitCatalog::WeaponRates::burstDelayTicks` (§5.1). Whatever
    /// carries a profile into the sim converts through a `TickRate` on the way.
    sim::Seconds paralyze{};

    /// Damage against a specific class. Parallel to `overrideArmor` and only the first
    /// `overrideCount` entries mean anything.
    ///
    /// TWO PARALLEL ARRAYS rather than one array of `{ArmorClass, Mag}` pairs, which is the
    /// opposite of the choice `SpatialGrid::Entry` makes and worth the inconsistency: a pair
    /// would be padded from 9 bytes to 16 by the `Mag`'s alignment, so six of them would cost
    /// 96 bytes instead of 54. They cannot come apart because nothing sorts them — `addOverride`
    /// is the only writer and it appends to both.
    std::array<sim::Mag, kMaxOverrides> overrideDamage{};
    std::array<ArmorClass, kMaxOverrides> overrideArmor{};

    /// How many of the arrays above are in use.
    std::uint8_t overrideCount = 0;

    /// What this weapon does to a target of this class.
    ///
    /// A LINEAR SCAN, deliberately, and it beats the indexed lookup it replaces. The scan is
    /// over at most six bytes of `overrideArmor` — one cache line, no branch misprediction
    /// worth the name — where Recoil's `damages[typeIndex]` is a load through a `vector`'s
    /// heap pointer. And in the overwhelmingly common case `overrideCount` is 0, so the loop
    /// does not execute at all and this returns `base`.
    [[nodiscard]] sim::Mag against(ArmorClass armor) const noexcept;

    /// Records what this weapon does to one class.
    ///
    /// Returns false when the profile is full (see `kMaxOverrides`) or when the override is
    /// redundant — `armor == kDefaultArmor`, which is what `base` already means. A caller that
    /// gets false has content it cannot represent and should say so.
    ///
    /// Re-stating a class OVERWRITES rather than appending, so a table that lists one twice
    /// resolves to its last entry instead of to whichever the scan met first.
    bool addOverride(ArmorClass armor, sim::Mag damage) noexcept;

    /// Whether this does anything at all. The question `Weapon::fires` asks, kept here so the
    /// answer accounts for a profile whose base is zero but which hurts something specific.
    [[nodiscard]] bool harmful() const noexcept;
};

/// Whether two profiles say the same thing.
///
/// A FREE FUNCTION and hand-written, where most small structs here get `= default`. Defaulted
/// comparison would compare all `kMaxOverrides` slots including the ones past `overrideCount`,
/// which hold whatever they were last assigned — so two profiles that agree on every override
/// they actually have would compare unequal because of a slot neither of them uses.
[[nodiscard]] bool operator==(const DamageProfile& a, const DamageProfile& b) noexcept;

/// A profile that does `damage` to everything. The shape 478 of 494 shipped weapons have, and
/// what every existing call site means when it passes a bare `Mag`.
[[nodiscard]] DamageProfile flatDamage(sim::Mag damage) noexcept;

// --- Importing -------------------------------------------------------------------------
//
// The two families' tables both land in a `DamageProfile`, and this is where they differ. Both
// functions take the registry rather than a name-to-index map because resolution is the thing
// that must happen once (see `ArmorRegistry`).

/// One row of Supreme Commander's `armordefinition.lua`: what one armour class does to the
/// damage aimed at it.
///
/// Sparse in the file and sparse here — `Experimental` lists ONLY `ExperimentalFootfall 0.0`,
/// and Experimental units are plainly not immune to everything else, which is the proof that an
/// UNLISTED pair means 1.0 rather than 0.0. Getting that backwards would make most of the
/// roster invulnerable, so it is written down.
struct ArmorMultiplier {
    ArmorClass armor = kDefaultArmor;
    std::string damageType;
    /// The multiplier, as authored. A `float` because this is content at load; it is applied
    /// to a `Mag` and never survives into the sim.
    float multiplier = 1.0f;
};

/// Transposes Supreme Commander's matrix into one weapon's profile.
///
/// `damage` is the weapon's authored `Damage`, `damageType` its `DamageType`, and `matrix`
/// every row of `armordefinition.lua`. The result carries an override for each class whose
/// multiplier against THIS damage type is not 1.0 — which, per the measurement at the top of
/// this file, is nothing at all for 478 of the 494 shipped weapons.
///
/// The comparison is case-insensitive on the damage type for the same reason class names are
/// folded: the corpus spells things two ways and one of them would otherwise mean nothing.
[[nodiscard]] DamageProfile damageFromMatrix(sim::Mag damage, std::string_view damageType,
                                             std::span<const ArmorMultiplier> matrix);

} // namespace rm::unitdef
