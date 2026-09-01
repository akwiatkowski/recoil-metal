#include "core/sim/RandomStream.hpp"

#include <utility>

namespace rm::sim {

RandomStream::RandomStream(std::uint32_t seed) : engine_(seed) {}

RandomStream::RandomStream(Snapshot snapshot) : engine_(std::move(snapshot)) {}

std::uint32_t RandomStream::next() {
    return engine_();
}

RandomStream::Snapshot RandomStream::snapshot() const {
    return engine_;
}

} // namespace rm::sim
