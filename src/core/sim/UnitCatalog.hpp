#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/TickRate.hpp"
#include "core/unit/Adjacency.hpp"
#include "core/unit/Armor.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstddef>
#include <vector>

namespace rm::sim {

// What each unit TYPE is, looked up by the index a unit carries.
//
// WHY THIS EXISTS. It is the one piece that makes the batch grouping unnecessary. Until now
// a unit's definition came from its BATCH — `SkirmishGroup::def`, one def shared by every
// unit in one instanced draw — which is why every sim pass took a span of groups and looped
// `for group / for instance`. Move the def behind a per-unit type index and the groups have
// nothing left to be: the passes take the store and loop once (PLAN2.md §7 P1.4).
//
// NON-OWNING, deliberately. The definitions are loaded from the VFS and owned by whoever
// loaded them — a deque in the app, a plain object in a test. The catalog is a lookup table,
// so a test can build one from two stack `UnitDef`s and the sim never learns what a VFS is.
// The pointers must outlive the catalog, which is the same contract `SkirmishGroup::def`
// already had.
//
// Type indices are handed out in first-seen order, which is the order batches were created
// in — so the catalog's numbering mirrors the layout it replaces. That is not required by
// anything here; it is recorded because it makes the two comparable while both exist.
class UnitCatalog {
public:
    /// What a type contributes per TICK, derived once when the type is registered.
    ///
    /// WHY DERIVED HERE (PLAN2.md §5.1). A blueprint authors rates per SECOND, which is a fact
    /// about the unit; how much that is per tick depends on the clock, which is a fact about
    /// the sim. The conversion must happen exactly once, and "when the catalog learns about
    /// the type" is the only moment that is both after the rate is known and before any tick
    /// runs. Doing it inside the income pass instead would put a float divide in a loop that
    /// runs over every unit every tick — and, worse, would leave the per-second value where a
    /// later reader could use it directly.
    ///
    /// `Mag` throughout: `BuildCostEnergy` reaches 10,008,000 in the corpus, and a rate summed
    /// over a hundred producers needs the same headroom as the total it feeds.
    struct Rates {
        Mag massPerTick{};
        Mag energyPerTick{};
        Mag upkeepEnergyPerTick{};
        Mag buildPerTick{};

        /// How far this type builds, repairs and reclaims, in elmos — converted here for
        /// the same reason `IntelRadii` is: `UnitDef` states a float because content does,
        /// and the reach is compared against fixed-point distances inside the tick.
        Fx buildReachElmos{};

        /// Retail's mobile-build range test subtracts the builder's smaller footprint
        /// dimension and the product's larger construction skirt from centre distance before
        /// comparing `MaxBuildDistance`. Kept separately because a command combines values
        /// from two different types.
        Fx buildFootprintElmos{};
        Fx buildSkirtElmos{};

        /// Hull regeneration, health per tick, from `Defense.RegenRate`.
        ///
        /// The type's BASE rate only. A unit's actual rate is this plus its veterancy bonus,
        /// which varies per unit and so cannot live in a per-type table — see
        /// `tickRegeneration`.
        Mag regenPerTick{};
    };

    struct EnhancementEffects {
        std::optional<Mag> buildPerTick;
        Mag healthAdd{};
        Mag regenPerTickAdd{};
        std::vector<std::string> buildableAdds;
    };
    [[nodiscard]] const EnhancementEffects* enhancementEffects(
        UnitTypeIndex type, std::string_view name) const noexcept {
        if (type >= enhancements_.size()) return nullptr;
        const auto found = enhancements_[type].find(name);
        return found == enhancements_[type].end() ? nullptr : &found->second;
    }

    /// How far one type sees, in elmos, in the type the sim can do arithmetic in.
    ///
    /// Converted here for the same reason the rates above are: `UnitDef` states floats
    /// because content does, and a pass that converted per emitter per update would be
    /// putting a float in the middle of the tick — which `tools/check_no_sim_floats.sh`
    /// exists to forbid.
    ///
    /// No water vision. It is parsed and it is not used: nothing in this sim is submerged
    /// yet, so there is nothing for it to answer about. See `IntelKind`.
    struct IntelRadii {
        Fx vision{};
        Fx radar{};
        Fx sonar{};
        Fx omni{};

        /// How far above its own feet this type's sensors sit, in elmos.
        ///
        /// ONLY THE RAYCAST STYLE READS IT (`VisionStyle::Recoil`), because only a model that
        /// consults terrain can have an opinion about eye level; Forged Alliance's discs are
        /// flat and take no height at all. Before this the raycast was handed the unit's
        /// TRANSFORM — the ground under its feet — so a commander could not see over a rise its
        /// own head cleared, and every unit in the game shared one blind ankle-level view.
        ///
        /// The top of the collision box (`UnitDef::sizeYElmos`), because the blueprints state
        /// no sensor mount and that is the only height they state.
        Fx eyeHeight{};

        /// Whether this TYPE is absent from a sense, to anyone without omni. Flags rather than
        /// radii, which is what the blueprints state — see `UnitDef`'s note.
        bool radarStealth = false;
        bool sonarStealth = false;
        bool cloak = false;

        /// Whether everyone always knows where it is. Beats stealth.
        bool freeIntel = false;

        /// The stealth FIELDS this type projects over its own alliance, and the jammer's
        /// deception. Radii like the senses above; `jammerBlips` is a count.
        Fx radarStealthField{};
        Fx sonarStealthField{};
        Fx jamRadius{};
        int jammerBlips = 0;
    };

    /// One ordinary bubble in fixed-point, per-tick simulation units.
    struct ShieldInfo {
        Mag maximum{};
        unitdef::ShieldShape shape = unitdef::ShieldShape::Sphere;
        Fx radiusElmos{};
        Fx verticalOffsetElmos{};
        std::array<Fx, 3> boxHalfExtentsElmos{};
        std::array<Fx, 3> collisionCenterElmos{};
        Fx boundingRadiusElmos{};
        Mag regenPerTick{};
        TickCount regenDelay = 0;
        TickCount recharge = 0;

        [[nodiscard]] bool exists() const noexcept {
            return maximum > Mag{} && boundingRadiusElmos > Fx{};
        }
    };

    /// One type's place in the adjacency game (`core/unit/Adjacency.hpp`), in the types
    /// the sim can do arithmetic in. Derived here for the same reason the rates are.
    struct AdjacencyInfo {
        /// Half the skirt, in elmos — the concrete apron adjacency is decided across.
        /// Zero for everything mobile, which is also what excludes it from the pair scan.
        Fx skirtHalfXElmos{};
        Fx skirtHalfZElmos{};
        Fx skirtCentreOffsetXElmos{};
        Fx skirtCentreOffsetZElmos{};

        /// The receiver-size row, 0..4 for SIZE4..SIZE20. This is meaningful only when
        /// `receives` is true: the blueprint must author exactly one valid `SIZE<n>`
        /// category as well as `STRUCTURE`. Skirt geometry decides contact, never this row.
        std::uint8_t sizeIndex = 0;

        /// Whether this type may receive adjacency. A malformed or absent authored size is
        /// not silently inferred from its skirt: that would assign a retail buff row the
        /// blueprint did not state.
        bool receives = false;

        /// What standing beside this type ADDS to a neighbour, indexed by the
        /// NEIGHBOUR's `sizeIndex`. Already fixed point; already per the giver's table.
        std::array<Fx, unitdef::kAdjacencySizeSteps> givesMassProduction{};
        std::array<Fx, unitdef::kAdjacencySizeSteps> givesEnergyProduction{};
        std::array<Fx, unitdef::kAdjacencySizeSteps> givesEnergyUpkeep{};

        /// Whether this type sits in the adjacency game at all — a structure with a
        /// skirt. The pair scan skips everything else without touching the arrays.
        [[nodiscard]] bool participates() const noexcept {
            return skirtHalfXElmos > Fx{} && skirtHalfZElmos > Fx{};
        }
    };

    /// One type's adjacency data. Zeroes for an unregistered index or a type with no
    /// definition — a decorative crowd neither gives nor receives.
    [[nodiscard]] const AdjacencyInfo& adjacency(UnitTypeIndex type) const noexcept {
        static constexpr AdjacencyInfo kNoAdjacency{};
        return type < adjacency_.size() ? adjacency_[type] : kNoAdjacency;
    }

    /// What one WEAPON's authored rates come to per tick.
    ///
    /// Here for the same reason the economy's are: `MuzzleVelocity` is elmos per second and
    /// `RateOfFire` is shots per second, both facts about the weapon, and both only become
    /// numbers the sim can use once a clock is known. Deriving them at the firing pass would
    /// mean a float divide per weapon per unit per tick.
    struct WeaponRates {
        /// Elmos per tick. Zero for the 111 weapons that state no muzzle velocity — those are
        /// instantaneous, and `launch` gives them a speed that crosses their own range in a
        /// tick so nothing divides by zero.
        Fx muzzlePerTick{};

        /// Ticks between shots, never less than one.
        TickCount reloadTicks = 1;

        /// Ticks between the shots WITHIN a burst, never less than one. Equal to
        /// `reloadTicks` for a weapon that does not burst, so the firing pass needs no
        /// branch on whether it does — it always reloads by one of the two.
        TickCount burstDelayTicks = 1;

        /// How many shots one trigger-pull delivers. One for an ordinary weapon.
        ///
        /// An `int` rather than a `TickCount`: it is a count of shots, not of ticks, and the
        /// two being different types is the whole point of §5.1.
        int burstSize = 1;

        /// What this weapon does, per armour class (PLAN2.md §7 P10.1, `ADR-033`).
        ///
        /// RESOLVED HERE for the same reason the rates above are: the blueprint states a
        /// `Damage` and a `DamageType` NAME, and turning a name into a class index needs a
        /// registry — a fact about the match's content, not about the weapon. The name-to-index
        /// resolution has to happen exactly once, and "when the catalog learns about the type"
        /// is the only moment that is both after the registry exists and before any tick runs.
        ///
        /// For a catalog with no armour context (the default — see `setArmor`) this is
        /// `flatDamage(weapon.damage)`, which is exactly the scalar the sim used before P10.1.
        /// That equivalence is what lets the whole corpus of existing tests stay untouched.
        unitdef::DamageProfile damage{};
    };

    // --- Armour (PLAN2.md §7 P10.1, `ADR-033`, D12) ---------------------------------
    //
    // OPTIONAL AND SET ONCE, rather than a constructor argument. Three reasons, and the third
    // is the one that decided it:
    //
    //   1. A catalog is default-constructed in about forty places, most of them tests that
    //      care about economy or reload timing and have no opinion about armour.
    //   2. The registry is a property of the MATCH's content, not of any one type, so it is
    //      the wrong thing to pass per `add` call.
    //   3. Without it, every unit is `kDefaultArmor` and every profile is flat — which is
    //      byte-for-byte the behaviour the sim had before armour classes existed. So a caller
    //      that says nothing gets the old engine, and `make verify` stays a strict check.

    /// Gives the catalog the content's armour classes and Supreme Commander's multiplier
    /// matrix.
    ///
    /// **CALL THIS BEFORE `add`.** Resolution happens when a type is registered, so a type
    /// added first keeps the flat profile it was given. Asserted rather than silently tolerated
    /// would be better; it is documented instead because the catalog has no way to fail — and
    /// `add` is `[[nodiscard]]`-returning an index, not a status.
    ///
    /// `matrix` may be empty, which is the BAR case: that family states absolute damage per
    /// armour class in its own weapon defs rather than a multiplier table, so the transpose
    /// has nothing to do and the importer fills profiles directly.
    void setArmor(unitdef::ArmorRegistry registry,
                  std::vector<unitdef::ArmorMultiplier> matrix);

    /// What a type is made of. `kDefaultArmor` for an unregistered index, for a type with no
    /// definition, and for a definition whose blueprint states no `ArmorType` — all three of
    /// which mean "ordinary", which is the answer that keeps a match playable.
    [[nodiscard]] ArmorClass armorOf(UnitTypeIndex type) const noexcept {
        return type < armor_.size() ? armor_[type] : kDefaultArmor;
    }

    /// One weapon's damage table for an arbitrary amount.
    ///
    /// PUBLIC and taking the amount separately, because a death weapon states its damage in
    /// RINGS (`NukeInnerRingDamage`) rather than in `Damage`, so the figure to transpose is not
    /// always `weapon.damage`. The hot firing path does not use this — it reads the profile
    /// `add` computed once — but a death blast happens rarely enough to resolve on demand, which
    /// is cheaper than threading a weapon index through the death report.
    [[nodiscard]] unitdef::DamageProfile profileFor(const unitdef::Weapon& weapon,
                                                    Mag amount) const;

    /// The registry this catalog resolved against. Empty of everything but `default` unless
    /// `setArmor` was called — which is what a test that never mentions armour gets.
    [[nodiscard]] const unitdef::ArmorRegistry& armor() const noexcept { return armor_names_; }
    /// Registers a definition and returns the index units of that type will carry.
    ///
    /// Null is allowed and gets an index like anything else: a decorative crowd has no
    /// definition, earns nothing and fires nothing, and "no def" has to be representable
    /// rather than a reason to reject the unit.
    /// The rate is defaulted so the many callers that want the ordinary clock need not say
    /// so, and is taken by value because a `TickRate` is two words.
    [[nodiscard]] UnitTypeIndex add(const unitdef::UnitDef* def, TickRate rate = TickRate{});

    /// What a type contributes per tick. Zeroes for an unregistered index or a type with no
    /// definition, which is what a decorative crowd should earn.
    [[nodiscard]] const Rates& rates(UnitTypeIndex type) const noexcept {
        static constexpr Rates kNone{};
        return type < rates_.size() ? rates_[type] : kNone;
    }

    /// How far a type sees. Zeroes for an unregistered index or a type with no definition —
    /// a decorative crowd sees nothing, which is the right answer rather than a reason to
    /// reject it.
    [[nodiscard]] const IntelRadii& intel(UnitTypeIndex type) const noexcept {
        static constexpr IntelRadii kNone{};
        return type < intel_.size() ? intel_[type] : kNone;
    }

    [[nodiscard]] const ShieldInfo& shield(UnitTypeIndex type) const noexcept {
        static constexpr ShieldInfo kNone{};
        return type < shields_.size() ? shields_[type] : kNone;
    }

    [[nodiscard]] Fx largestShieldRadius() const noexcept { return largestShieldRadius_; }

    /// One weapon's per-tick rates. Bounds-checked in both dimensions, returning zeroes for
    /// anything unregistered — a pass that indexed past the end would otherwise read whatever
    /// was next in memory, and this is called from the inner loop of firing.
    [[nodiscard]] const WeaponRates& weaponRates(UnitTypeIndex type,
                                                 std::size_t weapon) const noexcept {
        static constexpr WeaponRates kNone{};
        if (type >= weapons_.size() || weapon >= weapons_[type].size()) {
            return kNone;
        }
        return weapons_[type][weapon];
    }

    /// The definition for a type, or null — for an unregistered index as well as for a type
    /// registered without one. A pass that reads this must handle null either way, so
    /// bounds-checking to the same answer costs nothing and removes a crash.
    [[nodiscard]] const unitdef::UnitDef* def(UnitTypeIndex type) const noexcept {
        return type < defs_.size() ? defs_[type] : nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return defs_.size(); }

private:
    /// One weapon's damage table, through the matrix when there is one and flat when there is
    /// not. Private because it depends on `armor_matrix_` and is only meaningful during `add`.
    [[nodiscard]] unitdef::DamageProfile damageFor(const unitdef::Weapon& weapon) const;

    std::vector<const unitdef::UnitDef*> defs_;

    /// Parallel to `defs_`, index-locked by construction: all four only ever grow by one, in
    /// `add`.
    std::vector<Rates> rates_;
    std::vector<std::map<std::string, EnhancementEffects, std::less<>>> enhancements_;
    std::vector<std::vector<WeaponRates>> weapons_;
    std::vector<ArmorClass> armor_;
    std::vector<IntelRadii> intel_;
    std::vector<AdjacencyInfo> adjacency_;
    std::vector<ShieldInfo> shields_;
    Fx largestShieldRadius_{};

    /// The content's armour classes and Supreme Commander's multiplier table. Both empty of
    /// anything but `default` until `setArmor` — see the note there.
    unitdef::ArmorRegistry armor_names_;
    std::vector<unitdef::ArmorMultiplier> armor_matrix_;
};

} // namespace rm::sim
