#include "core/ui/ProductionPanel.hpp"

#include <algorithm>

namespace rm::ui {

namespace {

constexpr float kInset = 8.0f;
constexpr float kTitleBaseline = 21.0f;  ///< from the rectangle's top
constexpr float kBarTop = 30.0f;
constexpr float kBarHeight = 6.0f;
constexpr float kFirstRowBaseline = 56.0f;
constexpr float kRowPitch = 16.0f;
constexpr float kFooter = 34.0f;  ///< 6-point gap plus the clear-queue control
constexpr float kCancelWidth = 68.0f;  ///< room for CANCEL and its inset

[[nodiscard]] std::string fitLine(std::span<const text::Glyph> glyphs,
                                  std::string_view value, float width) {
    const std::vector<std::string> lines = wrapToWidth(glyphs, value, width, 1);
    return lines.empty() ? std::string{} : lines.front();
}

}  // namespace

Rect productionPanelRect(const FrameLayout& frame) noexcept {
    constexpr float kPanelHeight = 154.0f;  // five rows plus header, progress and clear
    constexpr float kGap = 6.0f;
    if (frame.commands.width < 180.0f || frame.battlefield.height < kPanelHeight) {
        return {};
    }
    return {frame.commands.x, frame.commands.y - kGap - kPanelHeight,
            frame.commands.width, kPanelHeight};
}

Rect productionRepeatRect(const Rect& rect) noexcept {
    return {rect.right() - 112.0f, rect.y + 5.0f, 104.0f, 22.0f};
}

Rect productionClearRect(const Rect& rect, bool paged) noexcept {
    const float width = rect.width - 2 * kInset;
    return {rect.x + kInset, rect.bottom() - 28.0f, paged ? width * 0.5f - 4.0f : width, 22.0f};
}

ProductionPage productionPage(const Rect& rect, std::size_t orders,
                               std::size_t requested) noexcept {
    const auto room = productionRowsFor(rect);
    if (room == 0 || orders == 0) return {};
    const auto pages = (orders - 1) / room + 1;
    const auto page = std::min(requested, pages - 1);
    const auto first = page * room;
    return {page, pages, first, std::min(room, orders - first)};
}

Rect productionCancelRect(const Rect& rect, std::size_t row) noexcept {
    if (row >= productionRowsFor(rect)) return {};
    return {rect.right() - kInset - kCancelWidth,
            rect.y + kFirstRowBaseline - 13.0f + static_cast<float>(row) * kRowPitch,
            kCancelWidth, 15.0f};
}

Rect productionPageButtonRect(const Rect& rect, bool next) noexcept {
    const auto clear = productionClearRect(rect, true);
    constexpr float width = 22.0f;
    return {next ? rect.right() - kInset - width : clear.right() + kInset,
            clear.y, width, clear.height};
}

std::optional<CommandId> productionCancelAt(const Rect& rect, const ProductionView& view,
                                            float x, float y, std::size_t requested) noexcept {
    if (view.empty() || rect.width < 128 || rect.height < 90 || !rect.contains(x, y)) return {};
    const auto page = productionPage(rect, view.queue.size(), requested);
    for (std::size_t row = 0; row < page.shown; ++row) {
        const auto id = view.queue[page.first + row].commandId;
        if (id != kInvalidCommandId && productionCancelRect(rect, row).contains(x, y)) return id;
    }
    return {};
}

std::optional<int> productionPageStepAt(const Rect& rect, const ProductionView& view,
                                        float x, float y, std::size_t requested) noexcept {
    if (view.empty() || rect.width < 128 || rect.height < 90 || !rect.contains(x, y)) return {};
    const auto page = productionPage(rect, view.queue.size(), requested);
    if (page.page > 0 && productionPageButtonRect(rect, false).contains(x, y)) return -1;
    if (page.page + 1 < page.pages && productionPageButtonRect(rect, true).contains(x, y)) return 1;
    return {};
}

std::optional<sim::CommandKind> productionCommandAt(
    const Rect& rect, const ProductionView& view, float x, float y) noexcept {
    if (view.empty() || rect.width <= 0 || rect.height <= 0 || !rect.contains(x, y)) {
        return std::nullopt;
    }
    if (productionRepeatRect(rect).contains(x, y)) return sim::CommandKind::ToggleFactoryRepeat;
    if ((!view.queue.empty() || view.building)
        && productionClearRect(rect, productionPage(rect, view.queue.size()).pages > 1).contains(x, y)) {
        return sim::CommandKind::Stop;
    }
    return std::nullopt;
}

std::size_t productionRowsFor(const Rect& rect) noexcept {
    const float room = rect.height - kFirstRowBaseline - kFooter;
    if (room < 0.0f) {
        return 0;
    }
    return static_cast<std::size_t>(room / kRowPitch) + 1;
}

void appendProductionPanel(Geometry& out, const text::Font& labelFont,
                           const text::Font& readoutFont, const Theme& theme, const Rect& rect,
                           const ProductionView& view, std::size_t requested) {
    if (rect.width < 128.0f || rect.height < 90.0f || view.empty() || !labelFont.usable()) {
        return;
    }
    appendPanel(out, labelFont, theme, rect.x, rect.y, rect.width, rect.height);

    // The repeat button is both a control and the current state, not a second stored toggle.
    const Rect repeat = productionRepeatRect(rect);
    text::appendRect(out.chrome, labelFont, repeat.x, repeat.y, repeat.width, repeat.height,
                     view.repeat ? theme.edgeLit : theme.well);
    const float titleBaseline = rect.y + kTitleBaseline;
    const std::string corner = view.repeat ? "REPEAT ON" : "REPEAT OFF";
    const float cornerWidth = readoutFont.usable()
                                ? text::measureText(readoutFont.glyphs, corner)
                                : 0.0f;
    const float titleWidth = std::max(0.0f, rect.width - 128.0f);
    const std::string title = fitLine(labelFont.glyphs, view.factoryName, titleWidth);
    (void)text::appendText(out.label, labelFont.glyphs, title, rect.x + kInset,
                           titleBaseline, kInk);
    if (readoutFont.usable()) {
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, corner,
                               rect.right() - kInset - cornerWidth, titleBaseline,
                               kInk);
    }

    // The progress of the build under way: a well the full width, filled from the left. An
    // idle factory shows the empty well, which is the honest picture of "nothing is happening".
    const float barWidth = rect.width - kInset * 2.0f;
    text::appendRect(out.chrome, labelFont, rect.x + kInset, rect.y + kBarTop, barWidth,
                     kBarHeight, theme.well);
    if (view.building) {
        const float filled = barWidth * std::clamp(view.progress, 0.0f, 1.0f);
        text::appendRect(out.chrome, labelFont, rect.x + kInset, rect.y + kBarTop, filled,
                         kBarHeight, theme.edgeLit);
    }

    const auto page = productionPage(rect, view.queue.size(), requested);
    const Rect clear = productionClearRect(rect, page.pages > 1);
    text::appendRect(out.chrome, labelFont, clear.x, clear.y, clear.width, clear.height,
                     theme.well);
    const std::string clearLabel = fitLine(labelFont.glyphs, "CLEAR QUEUE", clear.width - kInset);
    (void)text::appendText(out.label, labelFont.glyphs, clearLabel,
                           clear.x + kInset / 2, clear.y + 16.0f,
                           view.queue.empty() && !view.building ? theme.label : kInk);
    if (page.pages > 1) {
        for (const bool next : {false, true}) {
            const auto button = productionPageButtonRect(rect, next);
            text::appendRect(out.chrome, labelFont, button.x, button.y, button.width,
                             button.height, theme.well);
            const bool enabled = next ? page.page + 1 < page.pages : page.page > 0;
            (void)text::appendText(out.label, labelFont.glyphs, next ? ">" : "<",
                button.x + 5, button.y + 16, enabled ? kInk : theme.label);
        }
        const auto previous = productionPageButtonRect(rect, false);
        const auto next = productionPageButtonRect(rect, true);
        const auto& font = readoutFont.usable() ? readoutFont : labelFont;
        const auto number = fitLine(font.glyphs, std::to_string(page.page + 1) + "/"
            + std::to_string(page.pages), next.x - previous.right() - 8);
        auto& layer = readoutFont.usable() ? out.foregroundReadout : out.label;
        (void)text::appendText(layer, font.glyphs, number,
            previous.right() + 4, previous.y + 16, kInk);
    }

    // The orders, current first, one row each: the name on the left, the count on the right.
    const std::size_t room = productionRowsFor(rect);
    float baseline = rect.y + kFirstRowBaseline;
    if (view.queue.empty()) {
        if (room > 0) {
            (void)text::appendText(out.label, labelFont.glyphs, "IDLE", rect.x + kInset,
                                   baseline, theme.label);
        }
        return;
    }
    for (std::size_t row = 0; row < page.shown; ++row) {
        const auto i = page.first + row;
        const ProductionEntry& entry = view.queue[i];
        const auto cancel = productionCancelRect(rect, row);
        text::appendRect(out.chrome, labelFont, cancel.x, cancel.y, cancel.width,
                         cancel.height, theme.well);
        (void)text::appendText(out.label, labelFont.glyphs, "CANCEL", cancel.x + 4, baseline,
            entry.commandId == kInvalidCommandId ? theme.label : kInk);
        const std::string count = readoutFont.usable()
            ? fitLine(readoutFont.glyphs, "x" + std::to_string(entry.count),
                      std::max(0.0f, cancel.x - rect.x - 2 * kInset))
            : std::string{};
        const float countWidth = readoutFont.usable()
                                   ? text::measureText(readoutFont.glyphs, count)
                                   : 0.0f;
        const float nameWidth = rect.width - kInset * 3.0f - kCancelWidth
                              - (readoutFont.usable() ? countWidth + kInset : 0.0f);
        const std::string name = fitLine(labelFont.glyphs,
                                         entry.name.empty() ? entry.id : entry.name, nameWidth);
        (void)text::appendText(out.label, labelFont.glyphs, name, rect.x + kInset, baseline,
                               i == 0 ? kInk : theme.label);
        if (readoutFont.usable()) {
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, count,
                                   cancel.x - kInset - countWidth, baseline, kInk);
        }
        baseline += kRowPitch;
    }
}

} // namespace rm::ui
