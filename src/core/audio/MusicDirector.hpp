#pragma once

// Dynamic music, the UI-layer state machine retail's Lua owns (C-488: WP-42).
//
// Retail ships no native music manager (C-459's negative, re-confirmed) — music is
// Lua-driven over the cue API (`lua/UserMusic.lua`, `gamemain.lua:86,511,386-396`,
// `main.lua:230-255`). This is that state machine in one place: two one-entry cue
// lists (Battle/Base_Building), a 20-event/3s-window battle detector fed SOLELY by
// focus-army damage events, a 20s inactivity fall-back with a 3s fade gap,
// round-robin indices, and pause passthrough.
//
// Presentation only: the director holds no sim state and never feeds the hash. It
// drives the mixer's music layers — intensity 1 for Battle, 0 for Base_Building —
// and plays the newly entered layer on a swap, exactly the cue-over-music-bus path
// the mixer already carries. Pause freezes the clock (no decay, no window slide,
// no layer swap); round-robin indices advance per play so a second swap re-cues
// the next entry of the same list.

#include "core/audio/Mixer.hpp"
#include "core/sim/Events.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace rm::audio {

/// Which list is cued: the base ambience or the battle stinger.
enum class MusicState : std::uint8_t { Base = 0, Battle = 1 };

class MusicDirector {
public:
    /// Twenty damage events inside the 3s window trip the battle layer
    /// (`UserMusic.lua` battle detector). Ticks at the match rate (10 Hz).
    static constexpr std::size_t kBattleEvents = 20;
    static constexpr std::uint32_t kBattleWindowTicks = 30;
    /// Twenty quiet seconds fall back to base, plus a 3s fade gap before the
    /// swap is audible.
    static constexpr std::uint32_t kIdleFallbackTicks = 200;
    static constexpr std::uint32_t kFadeGapTicks = 30;

    explicit MusicDirector(Mixer& mixer);

    /// One tick of the music clock. `focusArmy` is the viewed army; only its
    /// damage feeds the detector. `tick` is the match tick; `ticksPerSecond`
    /// converts the authored seconds to ticks. `paused` freezes the machine.
    void update(std::span<const sim::Event> events, int focusArmy, std::uint64_t tick,
                std::uint32_t ticksPerSecond, bool paused);

    /// The cued list, and how many times each list has been (re-)played — the
    /// round-robin is observable as a count, one play per swap.
    [[nodiscard]] MusicState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t battlePlays() const noexcept { return battlePlays_; }
    [[nodiscard]] std::size_t basePlays() const noexcept { return basePlays_; }

private:
    void playState();

    Mixer* mixer_;
    MusicState state_ = MusicState::Base;
    /// Match ticks of the focus-army damage events still inside the window.
    std::uint64_t recentHits_[kBattleEvents] = {};
    std::size_t recentCount_ = 0;
    /// Last tick a focus-army damage event arrived; quiet starts here.
    std::uint64_t lastBattleTick_ = 0;
    bool everBattled_ = false;
    std::size_t battlePlays_ = 0;
    std::size_t basePlays_ = 0;
};

} // namespace rm::audio
