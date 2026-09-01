#pragma once

#include <cstdint>
#include <random>

namespace rm::sim {

/// Deterministic simulation randomness with an explicitly restorable MT19937 state.
class RandomStream {
public:
    using Snapshot = std::mt19937;

    explicit RandomStream(std::uint32_t seed);
    explicit RandomStream(Snapshot snapshot);

    [[nodiscard]] std::uint32_t next();
    [[nodiscard]] Snapshot snapshot() const;

private:
    Snapshot engine_;
};

} // namespace rm::sim
