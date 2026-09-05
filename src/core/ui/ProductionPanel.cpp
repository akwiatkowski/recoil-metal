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

Rect productionClearRect(const Rect& rect) noexcept {
    return {rect.x + kInset, rect.bottom() - 28.0f, rect.width - 2 * kInset, 22.0f};
}

std::optional<sim::CommandKind> productionCommandAt(
    const Rect& rect, const ProductionView& view, float x, float y) noexcept {
    if (view.empty() || rect.width <= 0 || rect.height <= 0 || !rect.contains(x, y)) {
        return std::nullopt;
    }
    if (productionRepeatRect(rect).contains(x, y)) return sim::CommandKind::ToggleFactoryRepeat;
    if ((!view.queue.empty() || view.building) && productionClearRect(rect).contains(x, y)) {
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
                           const ProductionView& view) {
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

    const Rect clear = productionClearRect(rect);
    text::appendRect(out.chrome, labelFont, clear.x, clear.y, clear.width, clear.height,
                     theme.well);
    const std::string clearLabel = fitLine(labelFont.glyphs, "CLEAR QUEUE", clear.width - kInset);
    (void)text::appendText(out.label, labelFont.glyphs, clearLabel,
                           clear.x + kInset / 2, clear.y + 16.0f,
                           view.queue.empty() && !view.building ? theme.label : kInk);

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
    const bool overflow = view.queue.size() > room;
    const std::size_t shown = overflow ? (room > 0 ? room - 1 : 0) : view.queue.size();
    for (std::size_t i = 0; i < shown; ++i) {
        const ProductionEntry& entry = view.queue[i];
        const std::string count = "x" + std::to_string(entry.count);
        const float countWidth = readoutFont.usable()
                                   ? text::measureText(readoutFont.glyphs, count)
                                   : 0.0f;
        const float nameWidth = rect.width - kInset * 2.0f
                              - (readoutFont.usable() ? countWidth + kInset : 0.0f);
        const std::string name = fitLine(labelFont.glyphs,
                                         entry.name.empty() ? entry.id : entry.name, nameWidth);
        (void)text::appendText(out.label, labelFont.glyphs, name, rect.x + kInset, baseline,
                               i == 0 ? kInk : theme.label);
        if (readoutFont.usable()) {
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, count,
                                   rect.right() - kInset - countWidth, baseline, kInk);
        }
        baseline += kRowPitch;
    }
    if (overflow && room > 0) {
        const std::string more = "+" + std::to_string(view.queue.size() - shown) + " MORE";
        const std::string fitted =
            fitLine(labelFont.glyphs, more, rect.width - kInset * 2.0f);
        (void)text::appendText(out.label, labelFont.glyphs, fitted, rect.x + kInset, baseline,
                               theme.label);
    }
}

} // namespace rm::ui
