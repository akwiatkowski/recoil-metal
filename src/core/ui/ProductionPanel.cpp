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
constexpr float kFooter = 6.0f;  ///< breathing room under the last baseline

}  // namespace

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
    if (rect.width <= 0.0f || rect.height <= 0.0f || view.empty() || !labelFont.usable()) {
        return;
    }
    appendPanel(out, labelFont, theme, rect.x, rect.y, rect.width, rect.height);

    // The header names the factory; the corner says whether the queue loops. "REPEAT" in ink
    // when on, quiet when off, so the state is readable at a glance without a toggle widget.
    const float titleBaseline = rect.y + kTitleBaseline;
    (void)text::appendText(out.label, labelFont.glyphs, view.factoryName, rect.x + kInset,
                           titleBaseline, kInk);
    if (readoutFont.usable()) {
        const std::string corner = view.repeat ? "REPEAT" : "ONCE";
        const float width = text::measureText(readoutFont.glyphs, corner);
        (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, corner,
                               rect.right() - kInset - width, titleBaseline,
                               view.repeat ? kInk : fade(kInk, 0.5f));
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
        (void)text::appendText(out.label, labelFont.glyphs,
                               entry.name.empty() ? entry.id : entry.name, rect.x + kInset,
                               baseline, i == 0 ? kInk : theme.label);
        if (readoutFont.usable()) {
            const std::string count = "x" + std::to_string(entry.count);
            const float width = text::measureText(readoutFont.glyphs, count);
            (void)text::appendText(out.foregroundReadout, readoutFont.glyphs, count,
                                   rect.right() - kInset - width, baseline, kInk);
        }
        baseline += kRowPitch;
    }
    if (overflow && room > 0) {
        const std::string more = "+" + std::to_string(view.queue.size() - shown) + " MORE";
        (void)text::appendText(out.label, labelFont.glyphs, more, rect.x + kInset, baseline,
                               theme.label);
    }
}

} // namespace rm::ui
