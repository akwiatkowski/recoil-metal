#pragma once

#include <cstdint>

namespace rm::ui {

// Retail's multifunction-bar overlay toggles (`multifunction.lua`: teamcolor via
// `TeamColorMode`, economy via `RenderOverlayEconomy`, military via
// `SetActiveOverlays`/`SetOverlayFilters({})`; keys Ctrl-W/Ctrl-E/Ctrl-R per
// `defaultKeyMap.lua`). All three are presentation-only: none reaches the
// command sink (`C-341`/`P13`). This header owns the toggle state so the
// session key path and the headless capture cannot derive different answers.

enum class Overlay : std::uint8_t { Military, Economy, TeamColor };

struct OverlayState {
    bool military = false;
    bool economy = false;
    bool teamColor = false;
};

inline void toggleOverlay(OverlayState& state, Overlay overlay) noexcept {
    switch (overlay) {
    case Overlay::Military: state.military = !state.military; break;
    case Overlay::Economy: state.economy = !state.economy; break;
    case Overlay::TeamColor: state.teamColor = !state.teamColor; break;
    }
}

[[nodiscard]] inline bool overlaySet(const OverlayState& state,
                                     Overlay overlay) noexcept {
    switch (overlay) {
    case Overlay::Military: return state.military;
    case Overlay::Economy: return state.economy;
    case Overlay::TeamColor: return state.teamColor;
    }
}

} // namespace rm::ui
