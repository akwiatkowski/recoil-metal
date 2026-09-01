// Save-state v1 must restore the simulation's time and its deterministic random sequence.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/RandomStream.hpp"
#include "core/sim/SaveState.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using rm::sim::RandomStream;
using rm::sim::SaveState;

namespace {

void writeU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = std::byte{static_cast<unsigned char>(value >> shift)};
    }
}

} // namespace

TEST_CASE("a save state random stream resumes at exactly its saved MT state", "[save-state]") {
    RandomStream original{std::uint32_t{1}};
    (void)original.next();
    (void)original.next();

    RandomStream restored{original.snapshot()};
    CHECK(restored.next() == original.next());
    CHECK(restored.next() == original.next());
}

TEST_CASE("a v1 save state round-trips its tick and random stream", "[save-state]") {
    RandomStream random{std::uint32_t{1}};
    (void)random.next();

    const auto bytes = SaveState::encodeV1({.tick = 42, .random = random.snapshot()});
    const auto restored = SaveState::decodeV1(bytes);

    REQUIRE(restored.has_value());
    CHECK(restored->tick == 42);

    RandomStream resumed{restored->random};
    // A complete recurrence cycle proves that the codec preserved all 624 state words and the
    // stream index, not merely the next generated value.
    for (std::size_t draw = 0; draw <= RandomStream::Snapshot::state_size; ++draw) {
        CHECK(resumed.next() == random.next());
    }
    CHECK(SaveState::encodeV1(*restored) == bytes);
}

TEST_CASE("a v1 save state refuses invalid and truncated input", "[save-state]") {
    RandomStream random{std::uint32_t{1}};
    const auto bytes = SaveState::encodeV1({.tick = 42, .random = random.snapshot()});

    // One byte cannot describe a v1 save state, regardless of its value.
    const std::vector<std::byte> invalid{std::byte{0}};
    CHECK_FALSE(SaveState::decodeV1(invalid).has_value());

    REQUIRE(bytes.size() > 1);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        CHECK_FALSE(SaveState::decodeV1({bytes.data(), size}).has_value());
    }

    std::vector<std::byte> malformed = bytes;
    // v1's header is magic (4) + version (4) + tick (8) + payload length (4).
    constexpr std::size_t kPayloadOffset = 20;
    REQUIRE(malformed.size() > kPayloadOffset);
    malformed[kPayloadOffset] = std::byte{static_cast<unsigned char>('x')};
    CHECK_FALSE(SaveState::decodeV1(malformed).has_value());

    // Altering the framing length to include noncanonical whitespace must not make a distinct
    // byte stream an accepted equivalent of the original save.
    malformed = bytes;
    malformed.push_back(std::byte{static_cast<unsigned char>(' ')});
    writeU32(malformed, 16, static_cast<std::uint32_t>(malformed.size() - kPayloadOffset));
    CHECK_FALSE(SaveState::decodeV1(malformed).has_value());

    // An MT19937 snapshot has 624 words plus an index, at most ten digits each and one space
    // between values. One extra byte exceeds the decoder's v1 allocation limit.
    constexpr std::size_t kOversizedPayload =
        (RandomStream::Snapshot::state_size + 1)
            * (std::numeric_limits<std::uint32_t>::digits10 + 1)
        + RandomStream::Snapshot::state_size + 1;
    malformed.resize(kPayloadOffset + kOversizedPayload, std::byte{});
    writeU32(malformed, 16, static_cast<std::uint32_t>(kOversizedPayload));
    CHECK_FALSE(SaveState::decodeV1(malformed).has_value());
}
