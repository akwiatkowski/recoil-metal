#include "core/audio/MusicDirector.hpp"

namespace rm::audio {

MusicDirector::MusicDirector(Mixer& mixer) : mixer_(&mixer) {}

void MusicDirector::playState() {
    if (state_ == MusicState::Battle) {
        mixer_->setMusicIntensity(1);
        ++battlePlays_;
    } else {
        mixer_->setMusicIntensity(0);
        ++basePlays_;
    }
    mixer_->playMusic();
}

void MusicDirector::update(std::span<const sim::Event> events, int focusArmy,
                           std::uint64_t tick, std::uint32_t ticksPerSecond, bool paused) {
    if (paused || ticksPerSecond == 0) {
        return;
    }
    // The window and gaps scale with the match rate: 3s of battle window, 20s of
    // quiet, 3s of fade — the authored seconds, not a 10 Hz literal.

    const auto window = static_cast<std::uint64_t>(kBattleWindowSeconds * static_cast<float>(ticksPerSecond));
    const auto idleFor = static_cast<std::uint64_t>(kIdleFallbackSeconds * static_cast<float>(ticksPerSecond));
    const auto fadeFor = static_cast<std::uint64_t>(kFadeGapSeconds * static_cast<float>(ticksPerSecond));
    for (const sim::Event& event : events) {
        if (event.kind != sim::EventKind::UnitDamaged || event.army != focusArmy) {
            continue;
        }
        if (recentCount_ < kBattleEvents) {
            recentHits_[recentCount_++] = tick;
        } else {
            for (std::size_t i = 1; i < kBattleEvents; ++i) {
                recentHits_[i - 1] = recentHits_[i];
            }
            recentHits_[kBattleEvents - 1] = tick;
        }
        lastBattleTick_ = tick;
        everBattled_ = true;
    }

    // Slide the window: hits older than 3s stop counting toward the twenty.
    while (recentCount_ > 0 && tick - recentHits_[0] > window) {
        for (std::size_t i = 1; i < recentCount_; ++i) {
            recentHits_[i - 1] = recentHits_[i];
        }
        --recentCount_;
    }

    if (state_ == MusicState::Base && recentCount_ >= kBattleEvents) {
        state_ = MusicState::Battle;
        playState();
        return;
    }
    if (state_ == MusicState::Battle && everBattled_ && tick - lastBattleTick_ >= idleFor + fadeFor) {
        state_ = MusicState::Base;
        recentCount_ = 0;
        playState();
    }
}

} // namespace rm::audio
