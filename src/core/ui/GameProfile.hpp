#pragma once

#include <cstdint>
#include <string_view>

namespace rm::ui {

/// Which game's interface vocabulary and material policy this run uses.
///
/// This is deliberately a closed value rather than a resolver or plugin interface. The engine
/// has four concrete presentations to support, and adding indirection would not add a fifth.
enum class GameProfile : std::uint8_t { Fa, Bar, Neutral, ClassicFaf };

struct GameProfileDescriptor {
    std::string_view id;
    std::string_view primaryResource;
    bool factionOwned = false;
    bool classicChrome = false;
};

[[nodiscard]] constexpr GameProfileDescriptor gameProfile(GameProfile profile) noexcept {
    switch (profile) {
        case GameProfile::Fa: return {"fa", "MASS", true, false};
        case GameProfile::Bar: return {"bar", "METAL", false, false};
        case GameProfile::Neutral: return {"neutral", "MATERIAL", false, false};
        case GameProfile::ClassicFaf: return {"faf", "MASS", true, true};
    }
    return {"fa", "MASS", true, false};
}

[[nodiscard]] constexpr std::string_view gameProfileName(GameProfile profile) noexcept {
    return gameProfile(profile).id;
}

} // namespace rm::ui
