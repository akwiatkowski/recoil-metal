#pragma once

#include "core/sim/Army.hpp"
#include "core/unit/BuildTree.hpp"
#include "core/unit/Role.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::data {

// Which blueprint is "the T1 extractor" for a faction — asked by role, answered from the corpus.
//
// WHY THIS EXISTS (PLAN2.md §7 P3.3). `core/sim/BuildOrder.hpp` named four blueprint paths:
//
//     inline constexpr std::string_view kExtractorBlueprint = "/units/UEB1103/UEB1103_unit.bp";
//
// Four paths, all UEF, in a C++ header — so the scripted opponent could only ever play UEF, and
// changing what it opens with meant editing and recompiling. Both reference engines keep ZERO
// game rules in C++ (§1.1); this is where ours start leaving.
//
// The lookup it replaces is `role + faction + tech -> blueprint id`, which is only answerable
// with the whole corpus in hand — the same reason `BuildTree` is materialised rather than
// evaluated on demand. A roster is built once at content load and asked thereafter.
//
// TIE-BREAKING IS PART OF THE CONTRACT, because more than one blueprint matches most queries: a
// faction has several T1 structures that produce energy. Ties go to the CHEAPEST by mass cost,
// then to the lowest blueprint id — so the answer is the entry-level thing a build order wants,
// and it is the same answer every run. Sorting on cost rather than on file order is what makes
// "the T1 power generator" mean the small one rather than whichever the directory walk reached
// first.

/// One entry: a blueprint, and what it is.
struct RosterEntry {
    /// The blueprint id, as its directory spells it — `UEB1103`.
    std::string id;

    /// The display name — "Mass Extractor" — or empty when the content states none.
    ///
    /// CARRIED HERE, not looked up when a panel asks, because the roster is exactly the list
    /// of units that are NOT loaded: a build menu offers what the player has not built, and
    /// `scene.definitions` holds only what has been registered. The roster outlives the defs
    /// it was built from, so anything the interface will want must ride along.
    std::string description;

    /// Where it lives, for the loader. Derived from the id rather than stored twice.
    [[nodiscard]] std::string path() const;

    unitdef::Role role = unitdef::Role::Unknown;
    sim::Faction faction = sim::Faction::Uef;
    int tech = 0;

    /// Whether this entry satisfies a `BuildableCategory` expression — any one term, all of
    /// its tags, or a lowercase id reference naming this entry. The factory menu's whole
    /// question, answered against the roster's own copy of the categories so the defs the
    /// roster was built from need not outlive it.
    [[nodiscard]] bool matches(const unitdef::CategoryExpression& expression) const;

    /// What it costs, which is what breaks a tie.
    sim::Mag costMass{};

    // What the hover card reads. The same reasoning as `description`: the def is not loaded,
    // so the facts a player weighs before building — full cost, time, toughness — ride here.
    sim::Mag costEnergy{};
    sim::Mag buildTime{};  ///< the blueprint's own units; seconds = buildTime / buildRate
    sim::Mag health{};

    /// Its categories, kept so a `requires` filter can be applied without going back to the
    /// definition — the roster outlives the span it was built from.
    std::vector<std::string> categories;
};

/// Every unit the corpus holds, indexed by what it is for.
class Roster {
public:
    /// Builds from a set of definitions and their ids, which must be the same length and in
    /// the same order — the caller has both from its own directory walk.
    ///
    /// Units whose faction cannot be read are SKIPPED rather than guessed at. A blueprint with
    /// no faction tag is campaign scenery or a shared prop, and putting it in a faction's
    /// roster would let a build order pick it.
    [[nodiscard]] static Roster build(std::span<const unitdef::UnitDef> units,
                                      std::span<const std::string> ids);

    /// The blueprint for a role, faction and tech, or nothing.
    ///
    /// Nothing is an ordinary answer, not an error: no faction has a T1 experimental. A caller
    /// that needs one must say what it does without it.
    /// `requires` are extra category tags the pick must also carry — `{"LAND"}` to disambiguate
    /// a factory from the air one. Empty accepts anything of the role.
    [[nodiscard]] std::optional<RosterEntry> pick(
        sim::Faction faction, unitdef::Role role, int tech,
        std::span<const std::string> required = {}) const;

    /// The same, at the lowest tech that has one. What a build order that just wants "an
    /// extractor" should ask, so it does not have to know which tiers a faction fields.
    [[nodiscard]] std::optional<RosterEntry> pickCheapest(
        sim::Faction faction, unitdef::Role role,
        std::span<const std::string> required = {}) const;

    /// Everything of a faction that satisfies a `BuildableCategory` expression, cheapest
    /// first — the FACTORY's menu, where the structure tray's is `all` by role. The
    /// expression decides membership because that is what the content says a factory
    /// builds; roles would be this engine's taxonomy standing in for the game's own.
    [[nodiscard]] std::vector<RosterEntry> buildableBy(
        sim::Faction faction, const unitdef::CategoryExpression& expression) const;

    /// Everything of a role and faction, cheapest first. For a caller listing options rather
    /// than picking one — the structure tray.
    [[nodiscard]] std::vector<RosterEntry> all(sim::Faction faction, unitdef::Role role,
                                               std::span<const std::string> required = {}) const;

    /// One entry BY NAME, or nothing.
    ///
    /// The tech path needs this and nothing else does: `General.UpgradesTo` names its successor
    /// by blueprint id — `'ueb0201'` — and every other lookup here is by what a unit IS rather
    /// than by what it is called. Case-insensitive, because the field is lower case and
    /// `RosterEntry::id` carries the id as its directory spells it, which is upper.
    [[nodiscard]] std::optional<RosterEntry> byId(std::string_view id) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    /// Sorted by (faction, role, tech, cost, id), which is what makes every lookup a range and
    /// every tie-break implicit in the order rather than a comparison at the call site.
    std::vector<RosterEntry> entries_;
};

/// Resolves one opening step against a roster: the single place the `tech > 0` branch lives.
///
/// A FREE FUNCTION rather than a method, and rather than the two-line branch at each call site.
/// It was written out twice — once in the app and once in a test — and the two disagreed
/// immediately: the test called `pick(..., tech, ...)` with `tech == 0`, which looks for a unit
/// declaring tier zero rather than for the cheapest at any tier, and reported that no faction
/// fields a tank. A branch duplicated between an engine and its test is a branch that will
/// diverge, and the divergence looks like a content bug.
///
/// Declared here rather than in `Opening.hpp` so that `Opening` stays a description with no
/// opinion about how it is resolved.
[[nodiscard]] std::optional<RosterEntry> resolveStep(const Roster& roster, sim::Faction faction,
                                                    unitdef::Role role, int tech,
                                                    std::span<const std::string> required,
                                                    std::span<const std::string> fallback = {});

/// The faction a blueprint's categories name, or nothing.
///
/// The four tags are `UEF`, `CYBRAN`, `AEON`, `SERAPHIM`. Nothing for content that carries none,
/// which is scenery and shared props — 07 §4.4 derives `far_faction` from exactly this.
[[nodiscard]] std::optional<sim::Faction> factionOf(const unitdef::UnitDef& def) noexcept;

} // namespace rm::data
