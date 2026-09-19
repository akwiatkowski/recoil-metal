#pragma once

#include "core/sim/Command.hpp"

#include <optional>

namespace rm::ui {

/// `C-339`: the armed-command state machine — retail's `commandmode.lua` as
/// one transition function, since our key dispatch is typed rather than
/// console strings (`C-338` divergence). The state itself is a plain
/// `std::optional<CommandKind>` in the session: nothing else about a command
/// mode is remembered between the arming and the click.
///
///   arm           a key or a rack cell names the next order's kind — the
///                 session writes the optional directly
///   issued        a world click sent it — the order queues behind the
///                 selection's work when Shift is held AND STAYS ARMED
///                 (`commandmode.lua:90-95`: `issuedOneCommand and not
///                 IsKeyDown('Shift')` is what ends the mode); a plain click
///                 disarms
///   cancel        Escape / a right-click on a panel / a swallowed click —
///                 retail's `EndCommandMode` → `ClearBuildTemplates()`; the
///                 session resets the optional directly
///
/// The function exists so the one rule with a branch in it — Shift keeps the
/// mode armed — is stated and tested once rather than re-derived at each of
/// the dozen places a click can issue an order.
inline void commandModeIssued(std::optional<sim::CommandKind>& armed,
                              bool shiftHeld) noexcept {
    if (!shiftHeld) {
        armed.reset();
    }
}

} // namespace rm::ui
