

#pragma once

#include <cstdint>
#include <optional>

#include "core/scene/TeamColours.hpp"
#include "platform/KeyInput.hpp"

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

// Retail's team-color ON (`multifunction.lua` `calculateTeamColors`): the focus
// army paints itself Player blue and every other army by diplomacy — ally
// green, enemy red, civilians/neutrals burlywood — rather than by per-army
// colour. OFF keeps the unit's own army colour. First-slot defaults from
// `teamColorSettings.colors` (`ff436eee` Player, `ff40bf40` ally, `ffe80a0a`
// enemy, `ffdeb887` neutral), decoded AARRGGBB to linear 0..1.
[[nodiscard]] inline TeamColour iconTintFor(const OverlayState& state, int focusArmy,
                                            int army, int alliance, bool neutral,
                                            TeamColour own) noexcept {
    if (!state.teamColor) {
        return own;
    }
    if (army == focusArmy) {
        return TeamColour{0x43 / 255.0f, 0x6e / 255.0f, 0xee / 255.0f, 1.0f};
    }
    if (neutral) {
        return TeamColour{0xde / 255.0f, 0xb8 / 255.0f, 0x87 / 255.0f, 1.0f};
    }
    if (alliance >= 0 && alliance == focusArmy) {
        return TeamColour{0x40 / 255.0f, 0xbf / 255.0f, 0x40 / 255.0f, 1.0f};
    }
    return TeamColour{0xe8 / 255.0f, 0x0a / 255.0f, 0x0a / 255.0f, 1.0f};
}

// Key context for retail's overlay hotkeys (`lua/lua/keymap/defaultKeyMap.lua`:
// Ctrl-W `tog_military`, Ctrl-E `tog_defense` (unit color), Ctrl-R `tog_econ`).
// Plain presses keep their session meanings (E opens the economy window, R
// toggles reflections), so only the control-modified form resolves. Declared
// here so headless tests and the session key path share one answer.
[[nodiscard]] inline std::optional<Overlay> overlayForKey(Key key,
                                                          bool control) noexcept {
    if (!control) {
        return std::nullopt;
    }
    switch (key) {
    case Key::W: return Overlay::Military;
    case Key::E: return Overlay::TeamColor;
    case Key::R: return Overlay::Economy;
    default: return std::nullopt;
    }
}

} // namespace rm::ui
