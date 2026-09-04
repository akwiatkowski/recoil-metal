#pragma once

#include <string_view>

namespace rm::ui {

/// Cost and opacity policy for the HUD's shared backdrop material.
enum class EffectsLevel {
    Full,
    Reduced,
    Off,
};

[[nodiscard]] constexpr std::string_view effectsLevelName(EffectsLevel level) noexcept {
    switch (level) {
    case EffectsLevel::Full: return "full";
    case EffectsLevel::Reduced: return "reduced";
    case EffectsLevel::Off: return "off";
    }
    return "off";
}

/// The command-line value and whether it deliberately overrides the system preference.
struct EffectsPreference {
    EffectsLevel level = EffectsLevel::Full;
    bool explicitOverride = false;
};

/// Reduced Transparency means opaque HUD chrome unless an explicit test/player override exists.
[[nodiscard]] constexpr EffectsLevel resolveEffects(EffectsPreference preference,
                                                    bool systemReducesTransparency) noexcept {
    return systemReducesTransparency && !preference.explicitOverride
             ? EffectsLevel::Off
             : preference.level;
}

} // namespace rm::ui
