#pragma once

#include "core/sim/Economy.hpp"
#include "core/sim/FeatureStore.hpp"
#include "core/sim/RandomStream.hpp"
#include "core/sim/Skirmish.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

/// End-of-tick economy, construction and army lifecycle state. This is one save subsystem;
/// projectiles, feature pools, intel history and pending path searches are separate concerns.
struct EconomyArmyState {
    std::vector<Army> armies;
    std::vector<Economy> economies;
    std::vector<Construction> building;
    std::vector<int> commandersEver;
    VictoryMode victoryMode = VictoryMode::Assassination;
    Resources baseStorage;
    bool over = false;
    bool winnerPending = false;
    std::optional<int> pendingWinner;
    TickCount winnerStableTicks = 0;
    TickCount defeatPollElapsedTicks = 0;
    std::vector<TickCount> defeatCleanupRemainingTicks;

    [[nodiscard]] static EconomyArmyState capture(const Match& match);
    /// Rebind spans after replacing their owning vectors. Call after constructing the runner,
    /// whose constructor derives an initial `over` value that the saved lifecycle must replace.
    void restore(Match& match, std::vector<Economy>& economyStorage,
                 std::vector<int>& commanderStorage) const;
};

/// Versioned simulation snapshot. Its fields do not yet cover a general mid-combat app resume.
struct SaveState {
    std::uint64_t tick{};
    RandomStream::Snapshot random{};
    std::uint64_t pathServiceBeats{};
    UnitStore::Snapshot units{};
    std::vector<SiloAmmo> siloAmmo;
    std::vector<MissileRedirect> redirects;
    std::optional<EconomyArmyState> economyArmies;
    std::vector<EnhancementWork> enhancements;
    std::vector<CaptureWork> captures;
    /// Wreck pool, when the scene leaves anything behind. Null scenes (and old readers)
    /// keep no features, exactly like the match's nullable pool.
    std::optional<FeatureStore::Snapshot> features;

    [[nodiscard]] static std::vector<std::byte> encodeV1(const SaveState& state);
    [[nodiscard]] static std::optional<SaveState> decodeV1(std::span<const std::byte> bytes);
    /// The published v2 format includes path-service and route-revalidation phase state.
    [[nodiscard]] static std::vector<std::byte> encodeV2(const SaveState& state);
    [[nodiscard]] static std::optional<SaveState> decodeV2(std::span<const std::byte> bytes);
    /// V19 adds continuous enhancement work and its carried resource allocation.
    /// V20 adds funded unit-capture tasks with their progress budgets.
    /// V21 adds the bank tuning (`KRoll`, `BankFactor`) to the aircraft snapshot.
    /// V22 adds the attachment bone record beside the historical offset sections.
    /// V23 adds the wreck pool (nullable, trailing).
    [[nodiscard]] static std::vector<std::byte> encode(const SaveState& state);
    /// Decodes all supported save versions, including v1 and the published v2 format.
    [[nodiscard]] static std::optional<SaveState> decode(std::span<const std::byte> bytes);
};

} // namespace rm::sim
