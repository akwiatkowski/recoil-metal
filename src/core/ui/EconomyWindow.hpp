#pragma once

// The economy-management overlay — the sluice gate, not a dashboard.
//
// THE JOB, stated once so the layout below can be judged against it: this window exists for
// the moment income cannot pay everyone. Its two questions are "who is starving whom" and
// "which of these drains do I cut" — so it is built as a THROTTLE: the header reads the
// allocator's answer per priority tier, the fabricator strip is the graduated panic control,
// and the table is a handle on every producer, not a report about them.
//
// THE WEIR. The window's one signature, and it exists only under scarcity: while the bank
// funds everything, the header is plain readouts and the table is one list. When a stall
// appears, the header splits each resource's funding into what PRIORITY work took off the
// top and what REGULAR work received of the rest — and the same division runs through the
// table as a dashed rule between the priority rows and the regular ones. A stalled economy
// is not a number here; it is a channel running visibly dry.
//
// EVERYTHING HERE IS ARITHMETIC OVER A VIEW STRUCT, same rule as the rest of core/ui:
// the app gathers `EconomyWindowView` from the scene, and every rect a click can mean is a
// pure function tested without a pixel.

#include "core/sim/Command.hpp"
#include "core/ui/Hud.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace rm::ui {

/// One mass fabricator: the unit's handle for the toggle, its draw for the slider's
/// detents, and its yield for the strip's totals.
struct EconFabView {
    sim::UnitId unit{};
    std::string name;
    float energyDraw = 0.0f;  ///< e/s while running
    float massYield = 0.0f;   ///< m/s while running
    bool running = false;
};

/// One table row: a unit, or every unit sharing one (type, job, tier) — grouped so a bulk
/// verb on the row means "all of these", which is the action a stall actually wants.
struct EconRowView {
    std::string name;      ///< display name of the type ("T1 Engineer", "Land Factory")
    std::string id;        ///< blueprint id
    std::string job;       ///< what it is doing: "IDLE", a product name, "ASSISTING …"
    std::vector<sim::UnitId> members;
    float massPerSecond = 0.0f;    ///< charged drain last tick, summed over members
    float energyPerSecond = 0.0f;
    float funded = 1.0f;           ///< the granted ratio the row draws as a water level
    BuildPriority priority = BuildPriority::Normal;
    std::size_t pausedCount = 0;
    bool pausable = false;
    bool anySelected = false;

    [[nodiscard]] bool allPaused() const noexcept {
        return !members.empty() && pausedCount == members.size();
    }
};

/// Everything the window draws, gathered per frame. `weirIndex` is the row count that is
/// PRIORITY — rows before it sit above the dashed rule — and `focus` is the
/// keyboard-navigated row, when one is armed.
struct EconomyWindowView {
    ResourceViews resources;
    float fundedFraction = 1.0f;
    bool massBinding = false;
    /// The granted ratio per tier (index: `BuildPriority`); only honest while stalling.
    std::array<float, 3> tierFunded{1.0f, 1.0f, 1.0f};
    std::array<bool, 3> tierAsked{};

    std::vector<EconFabView> fabricators;
    float fabBudget = 0.0f;      ///< e/s the slider currently allows
    float fabDrawTotal = 0.0f;   ///< e/s if every fab ran — the slider's right end
    float fabDrawActive = 0.0f;  ///< e/s the running fabs actually draw
    float fabYieldActive = 0.0f; ///< m/s the running fabs actually make

    std::vector<EconRowView> rows;
    std::size_t weirIndex = 0;
    std::optional<std::size_t> focus;

    [[nodiscard]] bool stalling() const noexcept { return fundedFraction < 0.995f; }
};

// --- Layout -------------------------------------------------------------------

/// How many table rows fit under the fixed bands at this frame's battlefield height.
[[nodiscard]] std::size_t econRowCapacity(const FrameLayout& frame) noexcept;

/// The window rectangle for `rows` table rows — anchored top-centre of the battlefield,
/// tall as the content needs, capped by `econRowCapacity`.
[[nodiscard]] Rect economyWindowRect(const FrameLayout& frame, std::size_t rows) noexcept;

/// Rows the rectangle's table band can hold — the same count the rect was sized from.
[[nodiscard]] std::size_t econRowsFor(const Rect& rect) noexcept;

struct EconPage {
    std::size_t page = 0;
    std::size_t pages = 1;
    std::size_t first = 0;
    std::size_t shown = 0;
};

[[nodiscard]] EconPage econPage(const Rect& rect, std::size_t rows,
                                std::size_t requested = 0) noexcept;

// --- Hit regions ---------------------------------------------------------------
// One function per thing a click can mean, addressed against the view as last DRAWN —
// the same "clicks read what the player saw" rule the other panels keep.

[[nodiscard]] Rect econCloseRect(const Rect& rect) noexcept;
[[nodiscard]] Rect econSliderTrack(const Rect& rect) noexcept;
[[nodiscard]] Rect econRowRect(const Rect& rect, std::size_t row) noexcept;
[[nodiscard]] Rect econPriorityCellRect(const Rect& rect, std::size_t row) noexcept;
[[nodiscard]] Rect econPauseCellRect(const Rect& rect, std::size_t row) noexcept;
[[nodiscard]] Rect econFabCellRect(const Rect& rect, std::size_t fab) noexcept;
[[nodiscard]] Rect econPageButtonRect(const Rect& rect, bool next) noexcept;

[[nodiscard]] bool econCloseAt(const Rect& rect, float x, float y) noexcept;
[[nodiscard]] std::optional<int> econPageStepAt(
    const Rect& rect, const EconomyWindowView& view, float x, float y,
    std::size_t page = 0) noexcept;
/// The row whose BODY is under the point — meaning "select these and go look" — or none.
[[nodiscard]] std::optional<std::size_t> econRowAt(
    const Rect& rect, const EconomyWindowView& view, float x, float y,
    std::size_t page = 0) noexcept;
/// The row whose [R|P] control is under the point, or none.
[[nodiscard]] std::optional<std::size_t> econPriorityAt(
    const Rect& rect, const EconomyWindowView& view, float x, float y,
    std::size_t page = 0) noexcept;
/// The row whose pause cell is under the point, or none.
[[nodiscard]] std::optional<std::size_t> econPauseAt(
    const Rect& rect, const EconomyWindowView& view, float x, float y,
    std::size_t page = 0) noexcept;
/// The fabricator whose toggle cell is under the point, or none. Cells exist only while
/// the count keeps them readable — past it the slider is the control.
[[nodiscard]] std::optional<std::size_t> econFabAt(
    const Rect& rect, const EconomyWindowView& view, float x, float y) noexcept;
[[nodiscard]] bool econSliderAt(const Rect& rect, const EconomyWindowView& view,
                                float x, float y) noexcept;
/// The energy budget, in e/s, that the track position `x` means: linear over
/// `fabDrawTotal`, clamped to the track's ends.
[[nodiscard]] float econSliderValueAt(const Rect& rect, const EconomyWindowView& view,
                                      float x) noexcept;

// --- Row shape ------------------------------------------------------------------

/// Merges rows that share (id, job, tier) into one grouped row — summed members and
/// drains, drain-weighted funded, all-paused count. The app emits per-unit rows; this is
/// what makes "Land Factory ×2" one row with one verb.
[[nodiscard]] std::vector<EconRowView> econGroupedRows(std::vector<EconRowView> rows);

/// Display order — PRIORITY first, then drain, then a stable id tie-break — and the index
/// the weir rule falls at (the count of priority-tier rows). One function because the two
/// facts are the same fact: the divider belongs where the order puts it.
[[nodiscard]] std::size_t econSortRows(std::vector<EconRowView>& rows);

// --- Drawing --------------------------------------------------------------------

/// The window itself: header readouts and tier channels, the fabricator strip with its
/// budget track, and one page of producer rows — each carrying its funded fraction as a
/// water level, its [R|P] control, and its pause cell.
void appendEconomyWindow(Geometry& out, const text::Font& labelFont,
                         const text::Font& readoutFont, const Theme& theme,
                         const Rect& rect, const EconomyWindowView& view,
                         std::size_t page = 0);

} // namespace rm::ui
