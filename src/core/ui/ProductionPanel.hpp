#pragma once

#include "core/ui/Hud.hpp"
#include "core/sim/Command.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rm::ui {

// The active selected factory's production, above the deck's command rectangle.
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
    CommandId commandId = kInvalidCommandId;
};

struct ProductionView {
    std::string factoryName;
    std::string factoryId;
    std::vector<ProductionEntry> queue;  ///< current first
    bool building = false;               ///< a construction is under way
    float progress = 0.0f;               ///< of the current build, 0..1
    /// Build units per SECOND lent by assisting engineers and stations, on top of the
    /// factory's own rate; zero when nobody is helping. The only place a player can see that
    /// an Assist order is doing anything.
    float assistRate = 0.0f;
    bool repeat = false;
    bool canRepeat = true;               ///< upgrades are never repeatable

    [[nodiscard]] bool empty() const noexcept { return factoryName.empty(); }
};

/// Same anchored rectangle for rendering, click interception and drag exclusion.
[[nodiscard]] Rect productionPanelRect(const FrameLayout& frame) noexcept;
[[nodiscard]] Rect productionRepeatRect(const Rect& rect) noexcept;
[[nodiscard]] Rect productionClearRect(const Rect& rect, bool paged = false) noexcept;
struct ProductionPage {
    std::size_t page = 0;
    std::size_t pages = 1;
    std::size_t first = 0;
    std::size_t shown = 0;
};
[[nodiscard]] ProductionPage productionPage(const Rect& rect, std::size_t orders,
                                            std::size_t requested = 0) noexcept;
[[nodiscard]] Rect productionCancelRect(const Rect& rect, std::size_t row) noexcept;
[[nodiscard]] Rect productionPageButtonRect(const Rect& rect, bool next) noexcept;
[[nodiscard]] std::optional<CommandId> productionCancelAt(
    const Rect& rect, const ProductionView& view, float x, float y, std::size_t page = 0) noexcept;
[[nodiscard]] std::optional<int> productionPageStepAt(
    const Rect& rect, const ProductionView& view, float x, float y, std::size_t page = 0) noexcept;
[[nodiscard]] std::optional<sim::CommandKind> productionCommandAt(
    const Rect& rect, const ProductionView& view, float x, float y) noexcept;

/// How many order rows the rectangle has room for below the header and the progress bar.
[[nodiscard]] std::size_t productionRowsFor(const Rect& rect) noexcept;

/// Draws a page of orders, each with a cancellation control addressing its stable command id.
void appendProductionPanel(Geometry& out, const text::Font& labelFont,
                           const text::Font& readoutFont, const Theme& theme, const Rect& rect,
                           const ProductionView& view, std::size_t page = 0);

} // namespace rm::ui
