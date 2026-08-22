#include "core/data/Roster.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>

namespace rm::data {

namespace {

std::optional<RosterEntry> resolveExact(const Roster& roster, sim::Faction faction,
                                        unitdef::Role role, int tech,
                                        std::span<const std::string> required) {
    if (tech > 0) {
        if (const std::optional<RosterEntry> exact =
                roster.pick(faction, role, tech, required)) {
            return exact;
        }
        // Fall back to the cheapest at any tier rather than refusing: a faction may field no T1
        // of something and still field a T2 — and the retail data is inconsistent about
        // declaring tiers at all (the Cybran T1 tank declares none). The `required` tags still
        // apply, so an air factory is never an acceptable substitute for a land one.
    }
    return roster.pickCheapest(faction, role, required);
}

} // namespace

std::string RosterEntry::path() const {
    // The corpus's own layout: `/units/<ID>/<ID>_unit.bp`. Derived rather than stored twice,
    // so an id and its path cannot disagree.
    return "/units/" + id + "/" + id + "_unit.bp";
}

std::optional<sim::Faction> factionOf(const unitdef::UnitDef& def) noexcept {
    if (def.hasCategory("UEF")) {
        return sim::Faction::Uef;
    }
    if (def.hasCategory("CYBRAN")) {
        return sim::Faction::Cybran;
    }
    if (def.hasCategory("AEON")) {
        return sim::Faction::Aeon;
    }
    if (def.hasCategory("SERAPHIM")) {
        return sim::Faction::Seraphim;
    }
    return std::nullopt;
}

Roster Roster::build(std::span<const unitdef::UnitDef> units, std::span<const std::string> ids) {
    Roster roster;
    const std::size_t count = std::min(units.size(), ids.size());
    roster.entries_.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::optional<sim::Faction> faction = factionOf(units[i]);
        if (!faction) {
            continue;  // scenery and shared props have no faction; see the header
        }
        roster.entries_.push_back(RosterEntry{
            .id = ids[i],
            .description = units[i].description,
            .role = unitdef::roleOf(units[i]),
            .faction = *faction,
            .tech = unitdef::techOf(units[i]),
            .costMass = units[i].buildCostMass,
            .costEnergy = units[i].buildCostEnergy,
            .buildTime = units[i].buildTime,
            .health = units[i].health,
            .categories = units[i].categories,
        });
    }

    // The whole tie-break, expressed once as an ordering rather than at each call site.
    std::sort(roster.entries_.begin(), roster.entries_.end(),
              [](const RosterEntry& a, const RosterEntry& b) {
                  return std::tie(a.faction, a.role, a.tech, a.costMass, a.id)
                         < std::tie(b.faction, b.role, b.tech, b.costMass, b.id);
              });
    return roster;
}

namespace {

/// Whether an entry carries every required tag. `categories` is sorted, so each is a binary
/// search — the same shape as `UnitDef::hasAllCategories`, over the roster's own copy.
[[nodiscard]] bool carries(const RosterEntry& entry, std::span<const std::string> required) {
    return std::all_of(required.begin(), required.end(), [&entry](const std::string& tag) {
        return std::binary_search(entry.categories.begin(), entry.categories.end(), tag);
    });
}

} // namespace

bool RosterEntry::matches(const unitdef::CategoryExpression& expression) const {
    for (const unitdef::CategoryTerm& term : expression) {
        if (term.empty()) {
            continue;  // a term with no tags matches nothing, per BuildTree's rule
        }
        if (unitdef::isIdReference(term)) {
            // A lowercase blueprint id naming a unit directly. Entry ids are uppercase, as
            // the directories spell them, so the comparison folds case one way.
            const std::string& reference = term.front();
            if (reference.size() == id.size()
                && std::equal(reference.begin(), reference.end(), id.begin(),
                              [](char a, char b) {
                                  return a == static_cast<char>(
                                             std::tolower(static_cast<unsigned char>(b)));
                              })) {
                return true;
            }
            continue;
        }
        const bool allPresent =
            std::all_of(term.begin(), term.end(), [this](const std::string& tag) {
                return std::binary_search(categories.begin(), categories.end(), tag);
            });
        if (allPresent) {
            return true;
        }
    }
    return false;
}

std::vector<RosterEntry> Roster::buildableBy(
    sim::Faction faction, const unitdef::CategoryExpression& expression) const {
    std::vector<RosterEntry> found;
    if (expression.empty()) {
        return found;  // builds nothing, per BuildTree: an empty expression is not "everything"
    }
    for (const RosterEntry& entry : entries_) {
        if (entry.faction == faction && entry.matches(expression)) {
            found.push_back(entry);
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const RosterEntry& a, const RosterEntry& b) {
                         return std::tie(a.costMass, a.id) < std::tie(b.costMass, b.id);
                     });
    return found;
}

std::vector<RosterEntry> Roster::all(sim::Faction faction, unitdef::Role role,
                                     std::span<const std::string> required) const {
    std::vector<RosterEntry> found;
    for (const RosterEntry& entry : entries_) {
        if (entry.faction == faction && entry.role == role && carries(entry, required)) {
            found.push_back(entry);
        }
    }
    // Already cheapest-first within a tech tier by the sort above; this makes it cheapest-first
    // across tiers too, which is what a caller listing options wants.
    std::stable_sort(found.begin(), found.end(),
                     [](const RosterEntry& a, const RosterEntry& b) {
                         return std::tie(a.costMass, a.id) < std::tie(b.costMass, b.id);
                     });
    return found;
}

std::optional<RosterEntry> Roster::pick(sim::Faction faction, unitdef::Role role, int tech,
                                        std::span<const std::string> required) const {
    // The first match in sorted order IS the cheapest, then lowest id — no comparison here.
    for (const RosterEntry& entry : entries_) {
        if (entry.faction == faction && entry.role == role && entry.tech == tech
            && carries(entry, required)) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<RosterEntry> Roster::pickCheapest(sim::Faction faction, unitdef::Role role,
                                                std::span<const std::string> required) const {
    const std::vector<RosterEntry> found = all(faction, role, required);
    if (found.empty()) {
        return std::nullopt;
    }
    return found.front();
}

std::optional<RosterEntry> resolveStep(const Roster& roster, sim::Faction faction,
                                      unitdef::Role role, int tech,
                                      std::span<const std::string> required,
                                      std::span<const std::string> fallback) {
    if (const std::optional<RosterEntry> strict =
            resolveExact(roster, faction, role, tech, required);
        strict.has_value() || fallback.empty()) {
        return strict;
    }
    // The strict requirement matched nothing and the plan offered a looser one. See
    // `OpeningStep::fallback` for why that is the only way to express "a tank if you have one".
    return resolveExact(roster, faction, role, tech, fallback);
}

} // namespace rm::data
