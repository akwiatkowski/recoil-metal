#include "core/unit/BuildTree.hpp"

#include <algorithm>

namespace rm::unitdef {

CategoryTerm parseCategoryTerm(std::string_view text) {
    CategoryTerm tags;
    std::size_t at = 0;
    while (at < text.size()) {
        // Whitespace-separated, and any whitespace counts: the corpus uses single spaces, but a
        // hand-written data file will use whatever it uses, and splitting on one specific
        // character would fail on a tab in a way that reads as a missing unit.
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'
                                    || text[at] == '\r')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\t' && text[at] != '\n'
               && text[at] != '\r') {
            ++at;
        }
        if (at > start) {
            tags.emplace_back(text.substr(start, at - start));
        }
    }
    return tags;
}

bool isIdReference(const CategoryTerm& term) noexcept {
    if (term.size() != 1) {
        return false;  // an id is one token; a category expression that is one token is upper
    }
    return std::none_of(term.front().begin(), term.front().end(),
                        [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
}

bool matchesExpression(const CategoryExpression& expression, const UnitDef& def) {
    for (const CategoryTerm& term : expression) {
        if (term.empty()) {
            // A term that reduced to nothing is skipped rather than matching everything. The
            // difference is not academic: `hasAllCategories({})` is true by definition, so
            // treating an empty term as a match would make one malformed expression turn every
            // wall in the game into a build option.
            continue;
        }
        if (isIdReference(term)) {
            // An UPGRADE target, named by blueprint id. Compared case-insensitively because the
            // reference is lower case and `UnitDef::name` carries the id as the directory
            // spells it, which is upper.
            const std::string& wanted = term.front();
            if (wanted.size() == def.name.size()
                && std::equal(wanted.begin(), wanted.end(), def.name.begin(),
                              [](unsigned char a, unsigned char b) {
                                  const auto lower = [](unsigned char c) {
                                      return c >= 'A' && c <= 'Z'
                                                 ? static_cast<unsigned char>(c - 'A' + 'a')
                                                 : c;
                                  };
                                  return lower(a) == lower(b);
                              })) {
                return true;
            }
            continue;
        }

        std::vector<std::string_view> tags;
        tags.reserve(term.size());
        for (const std::string& tag : term) {
            tags.emplace_back(tag);
        }
        if (def.hasAllCategories(tags)) {
            return true;  // ANY term is enough — the list means OR
        }
    }
    return false;
}

BuildTree BuildTree::materialise(std::span<const UnitDef> units) {
    BuildTree tree;
    tree.options_.resize(units.size());
    tree.declared_.assign(units.size(), false);

    for (std::size_t builder = 0; builder < units.size(); ++builder) {
        const CategoryExpression& expression = units[builder].buildableCategory;
        tree.declared_[builder] = !expression.empty();
        if (expression.empty()) {
            continue;
        }

        for (std::size_t candidate = 0; candidate < units.size(); ++candidate) {
            // A builder may build itself — a factory that produces engineers that build
            // factories is the normal shape — so there is no self-exclusion here. What would
            // be wrong is excluding it: `BUILTBYTIER1FACTORY UEF STRUCTURE LAND` legitimately
            // includes another factory.
            if (matchesExpression(expression, units[candidate])) {
                tree.options_[builder].push_back(candidate);
            }
        }
        // Ascending, which it already is by construction — stated so the guarantee survives a
        // future change to the loop order.
        std::sort(tree.options_[builder].begin(), tree.options_[builder].end());
    }

    return tree;
}

std::span<const std::size_t> BuildTree::optionsFor(std::size_t index) const noexcept {
    if (index >= options_.size()) {
        return {};
    }
    return options_[index];
}

bool BuildTree::declaresExpression(std::size_t index) const noexcept {
    return index < declared_.size() && declared_[index];
}

std::size_t BuildTree::builderCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(options_.begin(), options_.end(),
                      [](const std::vector<std::size_t>& o) { return !o.empty(); }));
}

} // namespace rm::unitdef
