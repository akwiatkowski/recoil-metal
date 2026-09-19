#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <optional>
#include <span>
#include <vector>

namespace rm {

// Which unit is selected: a batch and an index into it.
//
// Identity only. How a selection is *drawn* — a ring on the ground under each
// entry — belongs to the caller, so that the rules below can be decided and
// tested without a renderer. Nothing about the unit itself changes, so there is
// no per-unit state to restore when it leaves the set.
struct SelectionEntry {
    std::size_t batch = 0;
    std::size_t instance = 0;

    [[nodiscard]] friend bool operator==(const SelectionEntry&,
                                         const SelectionEntry&) noexcept = default;
};

/// The selection after a click.
///
/// Extracted from the click callback because that is inside an AppKit event
/// handler, where nothing can reach it: the only way to check "does shift-click
/// add" was to click. The rules are five lines and five cases, and none of them
/// needs a window.
///
///   miss, unmodified   clear — clicking empty ground deselects
///   miss, modified     unchanged; a near-miss while adding should not throw
///                      away what is already selected
///   hit already in     remove it, whatever the modifier — the rule is about
///                      the unit being in the set, not about the key
///   hit, unmodified    replace the selection with it
///   hit, modified      append it
///
/// Order is preserved, because orders are issued by walking the selection and
/// two identical clicks must do the same thing twice.
/// A TEMPLATE over what identifies a unit, because that changed and the rules did not.
/// A selection used to be a list of `SelectionEntry` — a batch and an index into it — and is
/// now a list of `sim::UnitId`, while the renderer still wants the pair to draw an outline
/// with. The five cases below are about set membership and nothing else, so they are stated
/// once for any identity that compares equal.
/// The identity is STATED, never deduced — `applyClick<UnitId>(...)`. Deduction would work
/// from some arguments and not others (a braced empty selection, a `nullopt` miss, a
/// `vector` converting to a span), so half the call sites would name the type and half
/// would not. One rule for every call is worth three characters.
template <typename Id>
[[nodiscard]] std::vector<Id> applyClick(std::span<const std::type_identity_t<Id>> current,
                                         std::optional<std::type_identity_t<Id>> hit,
                                         bool addToSet) {
    if (!hit) {
        // A miss while adding is a near-miss, not an instruction to throw the
        // selection away.
        return addToSet ? std::vector<Id>{current.begin(), current.end()} : std::vector<Id>{};
    }

    const auto existing = std::find(current.begin(), current.end(), *hit);
    if (existing != current.end()) {
        std::vector<Id> without;
        without.reserve(current.size() - 1);
        std::copy_if(current.begin(), current.end(), std::back_inserter(without),
                     [&hit](const Id& entry) { return !(entry == *hit); });
        return without;
    }

    if (!addToSet) {
        return {*hit};
    }

    std::vector<Id> extended{current.begin(), current.end()};
    extended.push_back(*hit);
    return extended;
}

/// The selection after a band-select (a drag rectangle released over the world).
///
/// The click rules' sibling, and the cases rhyme with `applyClick`'s deliberately:
///
///   empty box, unmodified   clear — dragging over empty ground deselects, exactly as
///                           clicking it does
///   empty box, modified     unchanged; a near-miss while adding keeps what you had
///   hits, unmodified        the box REPLACES the selection
///   hits, modified          the box's units are appended, minus the ones already present —
///                           unlike a modified click, a re-boxed unit is NOT toggled out,
///                           because a box is aimed at an area rather than at a unit, and
///                           evicting half your army because the new box overlapped the old
///                           selection is never what the drag meant
///
/// Order: existing first (orders walk the selection, and what was selected keeps its
/// place), then the box's units in the order given.
template <typename Id>
[[nodiscard]] std::vector<Id> applyBand(std::span<const std::type_identity_t<Id>> current,
                                        std::span<const std::type_identity_t<Id>> inBox,
                                        bool addToSet) {
    if (inBox.empty()) {
        return addToSet ? std::vector<Id>{current.begin(), current.end()} : std::vector<Id>{};
    }
    if (!addToSet) {
        return {inBox.begin(), inBox.end()};
    }

    std::vector<Id> extended{current.begin(), current.end()};
    for (const Id& id : inBox) {
        if (std::find(extended.begin(), extended.end(), id) == extended.end()) {
            extended.push_back(id);
        }
    }
    return extended;
}

/// The box's own preference filter, applied to what the box CAUGHT before `applyBand`
/// decides what the selection becomes.
///
/// BAR's rule: when the box caught at least one mobile combat unit, the engineers and
/// buildings caught with it fall out — a drag across a fight means the fighters, and
/// grabbing a worker out of it by accident is the common case the rule exists for. A box
/// with no combat unit at all keeps everything it caught, or there would be no way to box
/// the builders themselves. The predicate answers "is this one mobile combat" — what that
/// means for a unit is the caller's (`unitdef::isMobileCombat`), because this header's rules
/// are about identities, not about what a unit IS.
///
/// Order is the box's own, preserved, for the same reason `applyBand` keeps order.
template <typename Id, typename Pred>
[[nodiscard]] std::vector<Id>
preferMobileCombat(std::span<const std::type_identity_t<Id>> inBox, Pred&& isCombat) {
    if (!std::any_of(inBox.begin(), inBox.end(), isCombat)) {
        return {inBox.begin(), inBox.end()};
    }
    std::vector<Id> fighters;
    fighters.reserve(inBox.size());
    std::copy_if(inBox.begin(), inBox.end(), std::back_inserter(fighters),
                 std::forward<Pred>(isCombat));
    return fighters;
}

/// `C-336`: what a control-group recall selects. Retail's `ApplySelectionSet`
/// filters the stored set to `ALLUNITS - (FACTORY - MOBILE)` — everything
/// except an immobile factory — and falls back to the immobile factories only
/// when that leaves nothing, so a group that IS a row of factories still
/// recalls. `isImmobileFactory` is the caller's predicate over the stored ids;
/// dead-unit pruning happens before this runs (the session's recall already
/// erases the dead).
template <typename Id, typename Pred>
[[nodiscard]] std::vector<Id>
applySelectionSet(std::span<const std::type_identity_t<Id>> stored, Pred&& isImmobileFactory) {
    std::vector<Id> mobile;
    std::vector<Id> factories;
    for (const Id& id : stored) {
        (isImmobileFactory(id) ? factories : mobile).push_back(id);
    }
    return mobile.empty() ? factories : mobile;
}

/// `C-367`: the `UI_SelectByCategory` modifier flags, closed alphabet —
/// `+add +nearest +idle +inview +goto +excludeengineers` — as data rather than
/// as a console string, since our dispatch is typed (`C-338` divergence).
struct SelectModifiers {
    /// `+add`: union with the current selection instead of replacing it.
    bool add = false;
    /// `+nearest`: keep only the candidate closest to the caller's point.
    bool nearest = false;
    /// `+idle`: keep only candidates the `isIdle` predicate accepts.
    bool idle = false;
    /// `+inview`: keep only candidates the `inView` predicate accepts.
    bool inView = false;
    /// `+goto`: the caller moves the camera to the result — reported back
    /// through `SelectByCategoryResult::goTo` rather than acted on here.
    bool goTo = false;
    /// `+excludeengineers`: drop candidates the `isEngineer` predicate accepts.
    bool excludeEngineers = false;
};

template <typename Id>
struct SelectByCategoryResult {
    std::vector<Id> selection;
    /// `+goto` was passed: the caller moves the camera to the result's centre.
    bool goTo = false;
};

/// `C-367`'s modifier algebra over a candidate set, in retail's order: the
/// filters (`+inview`, `+idle`, `+excludeengineers`) narrow the candidates,
/// `+nearest` keeps the single closest, and `+add` decides whether the result
/// unions with or replaces the current selection. `+goto` is reported, not
/// applied — the camera belongs to the caller. Predicates answer per-id;
/// `distanceTo` is asked only under `+nearest`.
template <typename Id, typename InView, typename IsIdle, typename IsEngineer,
          typename DistanceTo>
[[nodiscard]] SelectByCategoryResult<Id> selectByCategory(
    std::span<const std::type_identity_t<Id>> current,
    std::span<const std::type_identity_t<Id>> candidates,
    const SelectModifiers& modifiers, InView&& inView, IsIdle&& isIdle,
    IsEngineer&& isEngineer, DistanceTo&& distanceTo) {
    std::vector<Id> kept;
    for (const Id& id : candidates) {
        if (modifiers.inView && !inView(id)) continue;
        if (modifiers.idle && !isIdle(id)) continue;
        if (modifiers.excludeEngineers && isEngineer(id)) continue;
        kept.push_back(id);
    }
    if (modifiers.nearest && !kept.empty()) {
        const auto closest = std::min_element(kept.begin(), kept.end(),
            [&distanceTo](const Id& a, const Id& b) {
                return distanceTo(a) < distanceTo(b);
            });
        kept = {*closest};
    }
    SelectByCategoryResult<Id> result{.goTo = modifiers.goTo};
    if (modifiers.add) {
        result.selection.assign(current.begin(), current.end());
        for (const Id& id : kept) {
            if (std::find(result.selection.begin(), result.selection.end(), id)
                == result.selection.end()) {
                result.selection.push_back(id);
            }
        }
    } else {
        result.selection = std::move(kept);
    }
    return result;
}


} // namespace rm
