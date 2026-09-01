#pragma once

#include "core/sim/RandomStream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

/// The complete v1 state: simulation time and the deterministic random generator.
struct SaveState {
    std::uint64_t tick{};
    RandomStream::Snapshot random{};

    [[nodiscard]] static std::vector<std::byte> encodeV1(const SaveState& state);
    [[nodiscard]] static std::optional<SaveState> decodeV1(std::span<const std::byte> bytes);
};

} // namespace rm::sim
