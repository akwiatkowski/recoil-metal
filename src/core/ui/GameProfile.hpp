#pragma once

#include <cstdint>
#include <string_view>

namespace rm::ui {

/// Which game's interface vocabulary and material policy this run uses.
///
/// This is deliberately a closed value rather than a resolver or plugin interface. The engine
/// has four concrete presentations to support, and adding indirection would not add a fifth.
enum class GameProfile : std::uint8_t { Fa, Bar, Neutral, ClassicFaf };

[[nodiscard]] constexpr std::string_view gameProfileName(GameProfile profile) noexcept {
    switch (profile) {
        case GameProfile::Fa: return "fa";
        case GameProfile::Bar: return "bar";
        case GameProfile::Neutral: return "neutral";
        case GameProfile::ClassicFaf: return "faf";
    }
    return "fa";
}

} // namespace rm::ui
