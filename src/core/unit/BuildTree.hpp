#pragma once

#include "core/unit/UnitDef.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rm::unitdef {

// What each builder can build, materialised from the category expressions the blueprints state.
//
// WHY THIS IS THE BLOCKER (PLAN2.md §7 P3.2). `07-ai-and-gamesetup.md §4.3` calls this "a hard
// blocker for any skirmish, not just for the AI", and the reason is that **Supreme Commander
// ships no build lists.** It ships expressions:
//
//     Economy = { BuildableCategory = { 'BUILTBYTIER1ENGINEER UEF' }, … }
//
// A SPACE MEANS AND; THE LIST MEANS OR. So a T1 UEF engineer can build anything tagged both
// `BUILTBYTIER1ENGINEER` and `UEF`, and a T1 UEF factory — which states three expressions —
// anything matching any one of them. Nothing anywhere in the corpus says "engineers build
// mexes"; that fact only exists once every expression has been evaluated against every unit.
//
// Until it is, no build order can be issued by anything: `ai_simpleai.lua`'s classifier puts
// every builder into `SimpleUndefinedUnitDefs` because `#unitDef.buildOptions == 0`, and our own
// scripted opponent has to name blueprint paths in a C++ header instead — which is exactly what
// P3.3 deletes, and cannot delete before this exists.
//
// 105 of the 568 shipped units declare an expression. Measured, not estimated.
//
// WHAT IS DEFERRED, and the report says to record it: `BuildableCategoryAdds` on a commander's
// upgrades — `'BUILTBYTIER2COMMANDER UEF'` unlocked by the Tech 2 Engineering Suite. Commander
// upgrades are out of scope, so the ADDS are parsed and kept but not applied. When upgrades
// land, applying them is a set union on an existing structure rather than a new one.

// TWO FORMS, and the second is not in the report. `BuildableCategory` holds category
// expressions as above — and it also holds bare BLUEPRINT IDS, in lower case:
//
//     UEB1103 (T1 mass extractor):  BuildableCategory = { 'ueb1202' }
//
// `ueb1202` is the T2 mass extractor. That entry is not a category at all; it is SupCom's
// UPGRADE mechanism — a structure's build option is the literal id of what it becomes. 34 of the
// 105 declaring units use this form, which is every upgrade chain in the game: mex tiers, radar
// tiers, shield tiers, and the Cybran experimental's four assembly stages.
//
// Missing it is not a small gap. `07 §4.3` describes only the expression form, so a reading that
// followed the report exactly would resolve 71 of 105 builders and silently lose every upgrade
// — and would look correct, because the 71 are the ones a test would think to check.
//
// THE RULE: a term of exactly one tag with no upper-case letter is an id reference; anything
// else is a category expression. The corpus is consistent about the case, and the two forms
// cannot collide — no category in the archive is lower case, and no blueprint id is upper case
// where it appears here. Cheap to state, and it fails loudly rather than quietly if a mod
// breaks the convention: an unmatched id resolves to nothing, which `declaresExpression`
// reports.
//
// MEASURED OVER THE SHIPPED CORPUS: 104 of the 105 declaring units resolve to at least one
// option. The one that does not is URL0111, the Cybran Mobile Missile Launcher, which asks for
// `CYBRANMOBILEMISSILE` — a category nothing in the archive carries. That is a dangling
// reference in the retail data rather than a gap here, and it is exactly what the
// `declaresExpression`/empty-options distinction exists to report.

/// One term of an expression: ALL of these tags must be present.
///
/// Stored as strings rather than as an interned id, because the corpus's category vocabulary is
/// open — `BUILTBYTIER1ENGINEER` is not in any enum, and a mod may invent one. The comparison
/// is against `UnitDef::categories`, which is sorted, so a term costs one binary search per tag.
using CategoryTerm = std::vector<std::string>;

/// A whole expression: ANY term matching is enough.
///
/// Empty means "builds nothing", which is the correct reading of a unit that states no
/// expression — 463 of the 568 shipped units. NOT "builds everything": an expression that
/// reduced to nothing must never become a universal match, or every wall in the game becomes a
/// factory.
using CategoryExpression = std::vector<CategoryTerm>;

/// Splits one `'A B C'` string into a term. Whitespace-separated, empty tags dropped.
[[nodiscard]] CategoryTerm parseCategoryTerm(std::string_view text);

/// Whether a term is a blueprint-id reference rather than a category expression — one tag, no
/// upper case. See the note on the two forms above.
[[nodiscard]] bool isIdReference(const CategoryTerm& term) noexcept;

/// Whether a unit satisfies an expression — any one term, all of its tags, or an id that names
/// it.
///
/// False for an empty expression, per the note above. A term with no tags is skipped rather
/// than matching everything, for the same reason.
[[nodiscard]] bool matchesExpression(const CategoryExpression& expression, const UnitDef& def);

/// Every builder's options, resolved once against a whole unit set.
///
/// MATERIALISED RATHER THAN EVALUATED ON DEMAND, which is what `07 §4.3` asks for and is also
/// the cheaper shape: the answer depends on the entire unit set, so evaluating lazily would
/// mean either re-scanning 568 units per query or caching — and caching a set keyed on a
/// builder IS this.
///
/// Indices are into the span the tree was built from, so the caller's own ordering is what
/// comes back. Sorted ascending within each builder, which makes the result independent of the
/// order the expressions happened to be written in.
class BuildTree {
public:
    /// Resolves every expression against every unit. O(builders x units x terms), which for
    /// the shipped corpus is 105 x 568 x a handful — milliseconds, once, at load.
    [[nodiscard]] static BuildTree materialise(std::span<const UnitDef> units);

    /// What the unit at `index` can build, as indices into the same span. Empty for a
    /// non-builder, and for a builder whose expression matched nothing — those are different
    /// facts, and `declaresExpression` tells them apart.
    [[nodiscard]] std::span<const std::size_t> optionsFor(std::size_t index) const noexcept;

    /// Whether that unit stated an expression at all. The distinction matters when diagnosing:
    /// a builder with an expression and no options means the expression is wrong or the corpus
    /// is incomplete, and a builder with neither is simply not a builder.
    [[nodiscard]] bool declaresExpression(std::size_t index) const noexcept;

    /// How many units have at least one option. The number that was zero before this existed.
    [[nodiscard]] std::size_t builderCount() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return options_.size(); }

private:
    std::vector<std::vector<std::size_t>> options_;
    std::vector<bool> declared_;
};

} // namespace rm::unitdef
