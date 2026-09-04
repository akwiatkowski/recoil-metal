#pragma once

#include "core/ui/Hud.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rm::ui {

// The selected factory's production, in the deck's command rectangle.
//
// WHAT A PLAYER ASKS OF A FACTORY is "what is it making, how far along, and what comes next"
// — three facts the roster and the build tray cannot carry: the roster says a factory is
// selected, the tray says what it COULD build. This panel says what it WILL build, in order,
// with the count on each order (a shift-clicked stack of five tanks is one row), the progress
// of the one under way, and whether the queue repeats. It reads as a list on an instrument,
// like the other panels, and never rearranges: the current order is always the first row.

struct ProductionEntry {
    std::string id;    ///< blueprint id, e.g. "URL0107"
    std::string name;  ///< the unit's description, or the id when it has none
    std::uint32_t count = 1;
};

struct ProductionView {
    std::string factoryName;
    std::string factoryId;
    std::vector<ProductionEntry> queue;  ///< current first
    bool building = false;               ///< a construction is under way
    float progress = 0.0f;               ///< of the current build, 0..1
    bool repeat = false;

    [[nodiscard]] bool empty() const noexcept { return factoryName.empty(); }
};

/// How many order rows the rectangle has room for below the header and the progress bar.
[[nodiscard]] std::size_t productionRowsFor(const Rect& rect) noexcept;

/// Draws the panel into `rect`. Rows past the room are summarised as "+N MORE" rather than
/// dropped silently, for the same reason the roster says what it paged away.
void appendProductionPanel(Geometry& out, const text::Font& labelFont,
                           const text::Font& readoutFont, const Theme& theme, const Rect& rect,
                           const ProductionView& view);

} // namespace rm::ui
