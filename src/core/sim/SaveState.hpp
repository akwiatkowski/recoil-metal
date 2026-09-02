#pragma once

#include "core/sim/RandomStream.hpp"
#include "core/sim/UnitStore.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

/// The complete save state: simulation time, deterministic random generator, and pathing/unit state.
struct SaveState {
    std::uint64_t tick{};
    RandomStream::Snapshot random{};
    std::uint64_t pathServiceBeats{};
    UnitStore::Snapshot units{};

    [[nodiscard]] static std::vector<std::byte> encodeV1(const SaveState& state);
    [[nodiscard]] static std::optional<SaveState> decodeV1(std::span<const std::byte> bytes);
    /// The published v2 format includes path-service and route-revalidation phase state.
    [[nodiscard]] static std::vector<std::byte> encodeV2(const SaveState& state);
    [[nodiscard]] static std::optional<SaveState> decodeV2(std::span<const std::byte> bytes);
    /// The latest v5 format additionally preserves DoNotTarget state.
    [[nodiscard]] static std::vector<std::byte> encode(const SaveState& state);
    /// Decodes all supported save versions, including v1 and the published v2 format.
    [[nodiscard]] static std::optional<SaveState> decode(std::span<const std::byte> bytes);
};

} // namespace rm::sim
