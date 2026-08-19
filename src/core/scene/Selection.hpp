#pragma once

#include <cstddef>
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
[[nodiscard]] std::vector<SelectionEntry> applyClick(std::span<const SelectionEntry> current,
                                                     std::optional<SelectionEntry> hit,
                                                     bool addToSet);

} // namespace rm
