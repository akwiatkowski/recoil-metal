#include "core/ui/EconomyWindow.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace rm::ui {

namespace {

// The bands, top to bottom: readouts, the fabricator control, the table's own header,
// the rows, and the footer that holds paging. Named once because the rect's height, the
// row capacity, and every hit region all derive from the same arithmetic — two copies of
// "where the table starts" is a click landing one row off.
constexpr float kWindowWidth = 620.0f;
constexpr float kHeaderHeight = 74.0f;    ///< eyebrow + two resource rows + breath
constexpr float kFabStripHeight = 38.0f;
constexpr float kTableHeadHeight = 16.0f;
constexpr float kRowPitch = 17.0f;
constexpr float kFooterHeight = 26.0f;
constexpr float kFixedHeight =
    kHeaderHeight + kFabStripHeight + kTableHeadHeight + kFooterHeight;
/// Per-fabricator toggle cells only while they stay readable — past this the slider is
/// the control and the cells would be a row of indistinguishable squares.
constexpr std::size_t kMaxFabCells = 12;

constexpr float kEyebrowBaseline = 18.0f;
constexpr float kResourceBaseline = 40.0f;
constexpr float kResourcePitch = 24.0f;

/// Where the first table row's band begins, relative to the rect.
constexpr float kTableTop = kHeaderHeight + kFabStripHeight + kTableHeadHeight;

// Table columns, measured from the row rect's left edge (row width is rect.width - 2·kPad
// = 602 at the fixed window width): the name column, the job column, then right-aligned
// readouts, and finally the control cells at the far edge.
constexpr float kJobColumn = 176.0f;
constexpr float kPauseCellWidth = 20.0f;
constexpr float kPriorityCellWidth = 46.0f;  ///< both segments together
constexpr float kCellGap = 3.0f;

[[nodiscard]] std::string fitLine(std::span<const text::Glyph> glyphs,
                                  std::string_view value, float width) {
    const std::vector<std::string> lines = wrapToWidth(glyphs, value, width, 1);
    return lines.empty() ? std::string{} : lines.front();
}

/// A drain figure the way the rows show it: integer, signed by the colour it is drawn
/// in rather than by a glyph, so "-24" never spends a column on a minus.
[[nodiscard]] std::string formatDrain(float value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f", static_cast<double>(std::fabs(value)));
    return text;
}

[[nodiscard]] float tableTop(const Rect& rect) noexcept { return rect.y + kTableTop; }

[[nodiscard]] Rect fabStripRect(const Rect& rect) noexcept {
    return {rect.x + kPad, rect.y + kHeaderHeight, rect.width - 2.0f * kPad,
            kFabStripHeight};
}

}  // namespace

// --- Layout ---------------------------------------------------------------------

std::size_t econRowCapacity(const FrameLayout& frame) noexcept {
    const float room = frame.battlefield.height - kMargin - kFixedHeight;
    if (room <= 0.0f) {
        return 0;
    }
    return std::max<std::size_t>(1, static_cast<std::size_t>(room / kRowPitch));
}

Rect economyWindowRect(const FrameLayout& frame, std::size_t rows) noexcept {
    const std::size_t shown = std::min(rows, econRowCapacity(frame));
    const float height = kFixedHeight + static_cast<float>(shown) * kRowPitch;
    return {frame.battlefield.x + (frame.battlefield.width - kWindowWidth) * 0.5f,
            frame.battlefield.y + kMargin, kWindowWidth, height};
}

std::size_t econRowsFor(const Rect& rect) noexcept {
    const float room = rect.height - kFixedHeight;
    if (room < 0.0f) {
        return 0;
    }
    // No +1 here, unlike the production panel's version: this rect is sized to whole
    // rows exactly, so a partial pitch is a caller's arithmetic showing, not a row.
    return static_cast<std::size_t>(room / kRowPitch);
}

EconPage econPage(const Rect& rect, std::size_t rows, std::size_t requested) noexcept {
    const std::size_t room = econRowsFor(rect);
    if (room == 0 || rows == 0) {
        return {};
    }
    const std::size_t pages = (rows - 1) / room + 1;
    const std::size_t page = std::min(requested, pages - 1);
    const std::size_t first = page * room;
    return {page, pages, first, std::min(room, rows - first)};
}

// --- Hit regions ------------------------------------------------------------------

Rect econCloseRect(const Rect& rect) noexcept {
    return {rect.right() - kPad - 16.0f, rect.y + 4.0f, 16.0f, 16.0f};
}

Rect econSliderTrack(const Rect& rect) noexcept {
    const Rect strip = fabStripRect(rect);
    return {strip.right() - 224.0f, strip.y + (strip.height - 10.0f) * 0.5f, 150.0f, 10.0f};
}

Rect econRowRect(const Rect& rect, std::size_t row) noexcept {
    return {rect.x + kPad, tableTop(rect) + static_cast<float>(row) * kRowPitch,
            rect.width - 2.0f * kPad, kRowPitch - 1.0f};
}

Rect econPauseCellRect(const Rect& rect, std::size_t row) noexcept {
    const Rect rowRect = econRowRect(rect, row);
    return {rowRect.right() - kPauseCellWidth, rowRect.y + 1.0f, kPauseCellWidth,
            rowRect.height - 2.0f};
}

Rect econPriorityCellRect(const Rect& rect, std::size_t row) noexcept {
    const Rect pause = econPauseCellRect(rect, row);
    return {pause.x - kCellGap - kPriorityCellWidth, pause.y, kPriorityCellWidth,
            pause.height};
}

Rect econFabCellRect(const Rect& rect, std::size_t fab) noexcept {
    const Rect strip = fabStripRect(rect);
    // Cells start after the label and the totals readout — the same fixed anchor the
    // draw uses, or the square a click lands on is not the square that was drawn.
    return {strip.x + 200.0f + static_cast<float>(fab) * 13.0f,
            strip.y + (strip.height - 12.0f) * 0.5f, 12.0f, 12.0f};
}

Rect econPageButtonRect(const Rect& rect, bool next) noexcept {
    constexpr float width = 22.0f;
    const float y = rect.bottom() - kFooterHeight + 3.0f;
    return {next ? rect.x + kPad + width + kCellGap : rect.x + kPad, y, width, 16.0f};
}

bool econCloseAt(const Rect& rect, float x, float y) noexcept {
    return rect.width > 0.0f && econCloseRect(rect).contains(x, y);
}

std::optional<int> econPageStepAt(const Rect& rect, const EconomyWindowView& view,
                                  float x, float y, std::size_t requested) noexcept {
    if (!rect.contains(x, y)) {
        return std::nullopt;
    }
    const EconPage page = econPage(rect, view.rows.size(), requested);
    if (page.pages <= 1) {
        return std::nullopt;
    }
    if (page.page > 0 && econPageButtonRect(rect, false).contains(x, y)) return -1;
    if (page.page + 1 < page.pages && econPageButtonRect(rect, true).contains(x, y)) return 1;
    return std::nullopt;
}

std::optional<std::size_t> econRowAt(const Rect& rect, const EconomyWindowView& view,
                                     float x, float y, std::size_t requested) noexcept {
    if (!rect.contains(x, y)) {
        return std::nullopt;
    }
    const EconPage page = econPage(rect, view.rows.size(), requested);
    for (std::size_t row = 0; row < page.shown; ++row) {
        if (econRowRect(rect, row).contains(x, y)) {
            // The body is everything EXCEPT the control cells at the right edge —
            // those mean their own clicks and are asked first by the caller.
            if (econPriorityCellRect(rect, row).contains(x, y)
                || econPauseCellRect(rect, row).contains(x, y)) {
                continue;
            }
            return page.first + row;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> econPriorityAt(const Rect& rect,
                                        const EconomyWindowView& view, float x, float y,
                                        std::size_t requested) noexcept {
    if (!rect.contains(x, y)) {
        return std::nullopt;
    }
    const EconPage page = econPage(rect, view.rows.size(), requested);
    for (std::size_t row = 0; row < page.shown; ++row) {
        if (econPriorityCellRect(rect, row).contains(x, y)) {
            return page.first + row;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> econPauseAt(const Rect& rect, const EconomyWindowView& view,
                                       float x, float y, std::size_t requested) noexcept {
    if (!rect.contains(x, y)) {
        return std::nullopt;
    }
    const EconPage page = econPage(rect, view.rows.size(), requested);
    for (std::size_t row = 0; row < page.shown; ++row) {
        if (econPauseCellRect(rect, row).contains(x, y)
            && view.rows[page.first + row].pausable) {
            return page.first + row;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> econFabAt(const Rect& rect, const EconomyWindowView& view,
                                     float x, float y) noexcept {
    if (!rect.contains(x, y) || view.fabricators.size() > kMaxFabCells) {
        return std::nullopt;
    }
    for (std::size_t fab = 0; fab < view.fabricators.size(); ++fab) {
        if (econFabCellRect(rect, fab).contains(x, y)) {
            return fab;
        }
    }
    return std::nullopt;
}

bool econSliderAt(const Rect& rect, const EconomyWindowView& view, float x,
                  float y) noexcept {
    if (!rect.contains(x, y) || view.fabricators.empty() || view.fabDrawTotal <= 0.0f) {
        return false;
    }
    // A fat hit band around the thin track — a slider you cannot grab is not a slider.
    Rect grab = econSliderTrack(rect);
    grab.y -= 6.0f;
    grab.height += 12.0f;
    return grab.contains(x, y);
}

float econSliderValueAt(const Rect& rect, const EconomyWindowView& view,
                        float x) noexcept {
    const Rect track = econSliderTrack(rect);
    const float fraction = std::clamp((x - track.x) / track.width, 0.0f, 1.0f);
    return fraction * view.fabDrawTotal;
}

// --- Row shape ---------------------------------------------------------------------

std::vector<EconRowView> econGroupedRows(std::vector<EconRowView> rows) {
    // The merge key is (type, job, tier): same-type units on the same job at the same
    // priority are one row with one verb. Tier is part of the key deliberately — two
    // engineers on the same mex at different tiers are two rows, each cleanly togglable,
    // and the weir divider then separates them for free.
    std::map<std::string, std::size_t> slotOf;
    std::vector<EconRowView> grouped;
    for (EconRowView& row : rows) {
        const std::string key = row.id + '\x1F' + row.job + '\x1F'
                              + static_cast<char>(row.priority);
        auto [it, inserted] = slotOf.try_emplace(key, grouped.size());
        if (inserted) {
            grouped.push_back(std::move(row));
            continue;
        }
        EconRowView& into = grouped[it->second];
        // Drain-weighted funded: the row's water level reads as what its members
        // actually got, weighted by what they asked for. A zero-drain member counts
        // equally rather than pulling the level toward a number it never earned.
        const float rowWeight = row.massPerSecond + row.energyPerSecond;
        const float intoWeight = into.massPerSecond + into.energyPerSecond;
        const float totalWeight = rowWeight + intoWeight;
        into.funded = totalWeight > 0.0f
            ? (into.funded * intoWeight + row.funded * rowWeight) / totalWeight
            : (into.funded * static_cast<float>(into.members.size())
               + row.funded * static_cast<float>(row.members.size()))
                  / static_cast<float>(into.members.size() + row.members.size());
        into.massPerSecond += row.massPerSecond;
        into.energyPerSecond += row.energyPerSecond;
        into.pausedCount += row.pausedCount;
        into.pausable = into.pausable || row.pausable;
        into.anySelected = into.anySelected || row.anySelected;
        into.members.insert(into.members.end(), row.members.begin(), row.members.end());
    }
    return grouped;
}

std::size_t econSortRows(std::vector<EconRowView>& rows) {
    const auto rank = [](BuildPriority tier) {
        // High first; Low last. Normal sits between them by its enum value.
        return static_cast<int>(BuildPriority::High) - static_cast<int>(tier);
    };
    const auto drain = [](const EconRowView& row) {
        return row.massPerSecond + row.energyPerSecond;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const EconRowView& a,
                                                   const EconRowView& b) {
        if (rank(a.priority) != rank(b.priority)) return rank(a.priority) < rank(b.priority);
        if (drain(a) != drain(b)) return drain(a) > drain(b);
        if (a.id != b.id) return a.id < b.id;
        if (a.job != b.job) return a.job < b.job;
        // The last resort has to be a sim-stable identity, not address order: members
        // arrive in slot order, so the smallest UnitId is deterministic across a replay.
        const std::uint32_t aFirst = a.members.empty() ? 0 : a.members.front().index;
        const std::uint32_t bFirst = b.members.empty() ? 0 : b.members.front().index;
        return aFirst < bFirst;
    });
    return static_cast<std::size_t>(std::ranges::count_if(
        rows, [](const EconRowView& row) { return row.priority == BuildPriority::High; }));
}

// --- Drawing -------------------------------------------------------------------------

void appendEconomyWindow(Geometry& out, const text::Font& labelFont,
                         const text::Font& readoutFont, const Theme& theme,
                         const Rect& rect, const EconomyWindowView& view,
                         std::size_t requested) {
    if (rect.width < 320.0f || rect.height < kFixedHeight || !labelFont.usable()) {
        return;
    }
    appendPanel(out, labelFont, theme, rect.x, rect.y, rect.width, rect.height);

    // The eyebrow row: what the window IS on the left, how it closes on the right, and —
    // only while the bank is actually short — which resource is binding the allocation.
    (void)text::appendText(out.label, labelFont.glyphs, "ECONOMY", rect.x + kPad,
                           rect.y + kEyebrowBaseline, theme.label);
    const Rect close = econCloseRect(rect);
    text::appendRect(out.chrome, labelFont, close.x, close.y, close.width, close.height,
                     theme.well);
    (void)text::appendText(out.label, labelFont.glyphs, "X", close.x + 5.0f,
                           close.y + 13.0f, theme.label);
    if (view.stalling() && readoutFont.usable()) {
        const std::string bound = view.massBinding ? "MASS-BOUND" : "ENERGY-BOUND";
        const std::string reading =
            bound + " " + std::to_string(static_cast<int>(view.fundedFraction * 100.0f))
            + "%";
        const float width = text::measureText(readoutFont.glyphs, reading);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, reading,
                               close.x - 8.0f - width, rect.y + kEyebrowBaseline, kWarn);
    }

    // THE RESOURCE ROWS. Same facts the top-left module carries — the window is the
    // module's expansion, not a second instrument to disagree with — plus, while a stall
    // lasts, the tier channels at the right: what PRIORITY took off the top against what
    // REGULAR received of the rest. Two bars over the two rows, and nothing at all when
    // there is nothing to say: a full weir draws no weir.
    const bool weir = view.stalling();
    for (std::size_t i = 0; i < view.resources.size(); ++i) {
        const ResourceView& resource = view.resources[i];
        const float baseline = rect.y + kResourceBaseline + static_cast<float>(i)
                                                           * kResourcePitch;
        text::appendRect(out.chrome, labelFont, rect.x + kPad, baseline - 8.0f, kChip,
                         kChip, resource.tint);
        (void)text::appendText(out.label, labelFont.glyphs, resource.name,
                               rect.x + kPad + kChip + 5.0f, baseline, kInk);
        if (!readoutFont.usable()) {
            continue;
        }
        const std::string stored = formatAmount(resource.gauge.stored) + " / "
                                 + formatAmount(resource.gauge.capacity);
        const float storedWidth = text::measureText(readoutFont.glyphs, stored);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, stored,
                               rect.x + kPad + 196.0f - storedWidth, baseline, kInk);
        const std::string income = formatRate(resource.gauge.incomePerSecond);
        const std::string drain = formatRate(-resource.gauge.drainPerSecond);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, income,
                               rect.x + kPad + 210.0f, baseline, kGain);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, drain,
                               rect.x + kPad + 262.0f, baseline, kLoss);

        // The storage gauge, then the tier channels beside it under scarcity.
        const float gaugeX = rect.x + kPad + 330.0f;
        const float gaugeWidth = 140.0f;
        text::appendRect(out.chrome, labelFont, gaugeX, baseline - 6.0f, gaugeWidth,
                         kGaugeHeight, theme.well);
        text::appendRect(out.chrome, labelFont, gaugeX, baseline - 6.0f,
                         gaugeWidth * resource.gauge.fill(), kGaugeHeight, resource.tint);

        if (weir) {
            // PRIORITY's channel rides the MASS row's height, REGULAR's the ENERGY row's
            // — position IS the label: above the weir and below it, the same division
            // the table shows with its dashed rule.
            const float channelX = gaugeX + gaugeWidth + 14.0f;
            const float channelWidth = rect.right() - kPad - channelX - 46.0f;
            const float channelY = rect.y + kResourceBaseline - 10.0f
                                 + static_cast<float>(i) * kResourcePitch;
            const char* tierName = i == 0 ? "PRIORITY" : "REGULAR";
            const float tierRatio = i == 0 ? view.tierFunded[2] : view.tierFunded[1];
            const bool asked = i == 0 ? view.tierAsked[2] : view.tierAsked[1];
            text::appendRect(out.chrome, labelFont, channelX, channelY, channelWidth,
                             5.0f, theme.well);
            if (asked) {
                text::appendRect(out.chrome, labelFont, channelX, channelY,
                                 channelWidth * std::clamp(tierRatio, 0.0f, 1.0f), 5.0f,
                                 tierRatio >= 0.995f ? theme.edgeLit : kWarn);
            }
            (void)text::appendText(out.label, labelFont.glyphs, tierName,
                                   channelX + channelWidth + 6.0f, channelY + 8.0f,
                                   theme.label);
        }
    }

    // THE FABRICATOR STRIP — the graduated panic control. Cells are the manual override;
    // the track is the budget: fill to what is allowed, ticks at each fab's marginal
    // draw so a detent position reads as "the next fab costs this much more".
    const Rect strip = fabStripRect(rect);
    text::appendRect(out.chrome, labelFont, strip.x, strip.y, strip.width,
                     strip.height, theme.well);
    (void)text::appendText(out.label, labelFont.glyphs, "FABRICATORS", strip.x + 8.0f,
                           strip.y + strip.height - 12.0f, theme.label);
    if (view.fabricators.empty()) {
        (void)text::appendText(out.label, labelFont.glyphs, "NONE BUILT",
                               strip.x + 96.0f, strip.y + strip.height - 12.0f,
                               theme.label);
    } else {
        if (readoutFont.usable()) {
            const std::string totals =
                "+" + formatAmount(view.fabYieldActive) + "M  -"
                + formatAmount(view.fabDrawActive) + "E";
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, totals,
                                   strip.x + 96.0f, strip.y + strip.height - 12.0f,
                                   kInk);
        }
        if (view.fabricators.size() <= kMaxFabCells) {
            for (std::size_t fab = 0; fab < view.fabricators.size(); ++fab) {
                const Rect cell = econFabCellRect(rect, fab);
                text::appendRect(out.chrome, labelFont, cell.x, cell.y, cell.width,
                                 cell.height,
                                 view.fabricators[fab].running ? kMass : theme.edge);
            }
        }
        const Rect track = econSliderTrack(rect);
        text::appendRect(out.chrome, labelFont, track.x, track.y, track.width,
                         track.height, theme.edge);
        if (view.fabDrawTotal > 0.0f) {
            // The budget's reach, then the realized draw inside it — the difference is
            // what the fabs are leaving on the table or over.
            const float reach = std::clamp(view.fabBudget / view.fabDrawTotal, 0.0f, 1.0f);
            text::appendRect(out.chrome, labelFont, track.x, track.y,
                             track.width * reach, track.height, fade(kEnergy, 0.35f));
            const float running = std::clamp(view.fabDrawActive / view.fabDrawTotal,
                                             0.0f, 1.0f);
            text::appendRect(out.chrome, labelFont, track.x, track.y,
                             track.width * running, track.height, kEnergy);
            // Detents: a tick where each fab's marginal draw lands, so the slider's
            // meaningful stops are visible rather than learned by dragging.
            float cumulative = 0.0f;
            std::vector<float> draws;
            draws.reserve(view.fabricators.size());
            for (const EconFabView& fab : view.fabricators) draws.push_back(fab.energyDraw);
            std::sort(draws.begin(), draws.end());
            for (const float draw : draws) {
                cumulative += draw;
                const float at = track.x + track.width
                               * std::clamp(cumulative / view.fabDrawTotal, 0.0f, 1.0f);
                text::appendRect(out.chrome, labelFont, at, track.y - 2.0f, 1.0f,
                                 track.height + 4.0f, kInk);
            }
        }
        if (readoutFont.usable()) {
            const std::string budget =
                std::to_string(static_cast<int>(std::lround(view.fabBudget))) + " E/S";
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, budget,
                                   track.right() + 8.0f, strip.y + strip.height - 12.0f,
                                   kEnergy);
        }
    }

    // THE TABLE EYEBROW. "Producers" because that is what the rows are: everything that
    // can take a tier, working or not.
    const float headBaseline = rect.y + kTableTop - 4.0f;
    (void)text::appendText(out.label, labelFont.glyphs, "PRODUCERS", rect.x + kPad,
                           headBaseline, theme.label);
    if (view.rows.empty()) {
        (void)text::appendText(out.label, labelFont.glyphs, "NOTHING PRODUCING",
                               rect.x + kPad + 80.0f, headBaseline, theme.label);
    }

    // THE ROWS. Each carries its funded fraction as a water level from the bottom of its
    // band — a stalled row is literally a channel run shallow — with the name and job on
    // the left, the drain in the resource's own colour, and the two-state priority and
    // pause cells on the right.
    const EconPage page = econPage(rect, view.rows.size(), requested);
    for (std::size_t row = 0; row < page.shown; ++row) {
        const std::size_t i = page.first + row;
        const EconRowView& entry = view.rows[i];
        const Rect rowRect = econRowRect(rect, row);

        // THE WEIR RULE between the last priority row and the first regular one —
        // drawn only under scarcity, same rule as the header channels: a funded
        // economy has nothing for the divider to say.
        if (weir && i == view.weirIndex && i > 0) {
            const float rule = rowRect.y - 1.0f;
            for (float dash = rowRect.x; dash < rowRect.right() - 4.0f; dash += 12.0f) {
                text::appendRect(out.chrome, labelFont, dash, rule, 8.0f, 1.0f,
                                 fade(theme.edgeLit, 0.8f));
            }
        }

        const bool full = entry.funded >= 0.995f;
        text::appendRect(out.chrome, labelFont, rowRect.x, rowRect.y, rowRect.width,
                         rowRect.height, fade(theme.well, 0.55f));
        if (entry.funded > 0.0f) {
            const float level = rowRect.height * std::clamp(entry.funded, 0.0f, 1.0f);
            text::appendRect(out.chrome, labelFont, rowRect.x,
                             rowRect.bottom() - level, rowRect.width, level,
                             full ? fade(theme.edgeLit, 0.22f) : fade(kWarn, 0.30f));
        }
        if (entry.anySelected) {
            text::appendRect(out.chrome, labelFont, rowRect.x, rowRect.y, 2.0f,
                             rowRect.height, theme.edgeLit);
        }
        if (view.focus && *view.focus == i) {
            text::appendRect(out.chrome, labelFont, rowRect.x, rowRect.y, rowRect.width,
                             1.0f, theme.edgeLit);
            text::appendRect(out.chrome, labelFont, rowRect.x, rowRect.bottom() - 1.0f,
                             rowRect.width, 1.0f, theme.edgeLit);
        }

        const float baseline = rowRect.y + 12.5f;
        std::string title = entry.name;
        if (entry.members.size() > 1) {
            title += " x" + std::to_string(entry.members.size());
        }
        const Rect pause = econPauseCellRect(rect, row);
        const Rect priority = econPriorityCellRect(rect, row);
        const float nameWidth = kJobColumn - 12.0f;
        (void)text::appendText(out.label, labelFont.glyphs,
                               fitLine(labelFont.glyphs, title, nameWidth),
                               rowRect.x + 8.0f, baseline,
                               entry.allPaused() ? theme.label : kInk);
        (void)text::appendText(
            out.label, labelFont.glyphs,
            fitLine(labelFont.glyphs, entry.job, priority.x - kJobColumn - 130.0f),
            rowRect.x + kJobColumn, baseline, theme.label);

        if (readoutFont.usable()) {
            // The drain in the resource's own colour, right-aligned before the cells —
            // mass green, energy amber, the same reading rule the header rows keep.
            const std::string energy = formatDrain(entry.energyPerSecond) + "e";
            const std::string mass = formatDrain(entry.massPerSecond) + "m";
            const float energyWidth = text::measureText(readoutFont.glyphs, energy);
            const float massWidth = text::measureText(readoutFont.glyphs, mass);
            const float energyX = priority.x - 8.0f - energyWidth;
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, energy,
                                   energyX, baseline, kEnergy);
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, mass,
                                   energyX - 4.0f - massWidth, baseline, kMass);
            if (!full) {
                const std::string funded =
                    std::to_string(static_cast<int>(entry.funded * 100.0f)) + "%";
                const float fundedWidth = text::measureText(readoutFont.glyphs, funded);
                (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, funded,
                                       energyX - 4.0f - massWidth - 8.0f - fundedWidth,
                                       baseline, kWarn);
            }
        }

        // THE PRIORITY CELL — two segments, R and P, the lit one the tier the row sits
        // at. LOW exists internally and is named plainly rather than mapped onto either
        // button: the window's contract is two states, and a Low row is something the
        // B key made, not something this control owns.
        const float segWidth = (priority.width - 2.0f) * 0.5f;
        const bool regular = entry.priority == BuildPriority::Normal;
        const bool high = entry.priority == BuildPriority::High;
        text::appendRect(out.chrome, labelFont, priority.x, priority.y, segWidth,
                         priority.height, regular ? theme.edgeLit : theme.well);
        text::appendRect(out.chrome, labelFont, priority.x + segWidth + 2.0f,
                         priority.y, segWidth, priority.height,
                         high ? kWarn : theme.well);
        (void)text::appendText(out.label, labelFont.glyphs, "R",
                               priority.x + segWidth * 0.5f - 3.0f,
                               priority.y + priority.height - 4.0f, kInk);
        (void)text::appendText(out.label, labelFont.glyphs, "P",
                               priority.x + segWidth + 2.0f + segWidth * 0.5f - 3.0f,
                               priority.y + priority.height - 4.0f, kInk);
        if (entry.priority == BuildPriority::Low) {
            (void)text::appendText(out.label, labelFont.glyphs, "LOW",
                                   priority.x - 26.0f, baseline, theme.label);
        }

        // THE PAUSE CELL — two bars as the glyph rather than a letter, because P is
        // already taken by the control beside it. Lit when every member is held.
        text::appendRect(out.chrome, labelFont, pause.x, pause.y, pause.width,
                         pause.height,
                         entry.allPaused() ? theme.edgeLit : theme.well);
        if (entry.pausable) {
            const float barY = pause.y + 3.0f;
            text::appendRect(out.chrome, labelFont, pause.x + 5.0f, barY, 3.0f,
                             pause.height - 6.0f, kInk);
            text::appendRect(out.chrome, labelFont, pause.x + 11.0f, barY, 3.0f,
                             pause.height - 6.0f, kInk);
        }
    }

    // THE FOOTER — paging on the left, the keyboard contract on the right.
    if (page.pages > 1) {
        for (const bool next : {false, true}) {
            const Rect button = econPageButtonRect(rect, next);
            const bool enabled = next ? page.page + 1 < page.pages : page.page > 0;
            text::appendRect(out.chrome, labelFont, button.x, button.y, button.width,
                             button.height, enabled ? theme.well : fade(theme.well, 0.4f));
            (void)text::appendText(out.label, labelFont.glyphs, next ? ">" : "<",
                                   button.x + 6.0f, button.y + 13.0f,
                                   enabled ? kInk : theme.label);
        }
        if (readoutFont.usable()) {
            const std::string number = std::to_string(page.page + 1) + "/"
                                     + std::to_string(page.pages);
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, number,
                                   econPageButtonRect(rect, true).right() + 6.0f,
                                   rect.bottom() - 8.0f, theme.label);
        }
    }
    if (readoutFont.usable()) {
        const std::string hint = "P PRIORITY  S PAUSE  M CLOSE";
        const float width = text::measureText(readoutFont.glyphs, hint);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, hint,
                               rect.right() - kPad - width, rect.bottom() - 8.0f,
                               theme.label);
    }
}

} // namespace rm::ui
