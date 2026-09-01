// Save-state v1 must restore the simulation's time and its deterministic random sequence.
#include <catch2/catch_test_macros.hpp>

#include "core/sim/RandomStream.hpp"
#include "core/sim/PathService.hpp"
#include "core/sim/SaveState.hpp"
#include "core/sim/UnitStore.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using rm::sim::RandomStream;
using rm::sim::SaveState;
using rm::sim::UnitStore;

namespace {

void writeU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = std::byte{static_cast<unsigned char>(value >> shift)};
    }
}

[[nodiscard]] std::uint32_t readU32(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset++])) << shift;
    }
    return value;
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

TEST_CASE("a v1 save state round-trips a unit store snapshot", "[save-state]") {
    UnitStore original;
    UnitStore::Spawn moving;
    moving.type = 7;
    moving.transform.x = rm::sim::Fx::fromInt(12);
    moving.transform.y = rm::sim::Fx::fromInt(13);
    moving.transform.z = rm::sim::Fx::fromInt(34);
    moving.transform.heading = 1234;
    moving.motion.armyIndex = 2;
    moving.motion.destinationX = rm::sim::Fx::fromInt(56);
    moving.motion.destinationZ = rm::sim::Fx::fromInt(78);
    moving.motion.moving = true;
    moving.motion.distanceTravelledElmos = rm::sim::Fx::fromInt(90);
    moving.motion.path.push_back({rm::sim::Fx::fromInt(56), rm::sim::Fx::fromInt(78)});
    moving.health.current = rm::sim::Mag::fromInt(123);
    moving.health.maximum = rm::sim::Mag::fromInt(500);
    const auto live = original.spawn(moving);
    const auto child = original.spawn({});

    const auto olderDead = original.spawn({});
    const auto newerDead = original.spawn({});
    original.kill(olderDead);
    original.kill(newerDead);
    REQUIRE(original.attach(live, child));

    RandomStream random{std::uint32_t{1}};
    const auto bytes = SaveState::encodeV1(
        {.tick = 42, .random = random.snapshot(), .units = original.snapshot()});
    const auto saved = SaveState::decodeV1(bytes);

    REQUIRE(saved.has_value());
    UnitStore restored{saved->units};
    REQUIRE(restored.alive(live));
    REQUIRE(restored.alive(child));
    REQUIRE_FALSE(restored.alive(olderDead));
    REQUIRE_FALSE(restored.alive(newerDead));
    CHECK(restored.transforms()[live.index].x == rm::sim::Fx::fromInt(12));
    CHECK(restored.transforms()[live.index].y == rm::sim::Fx::fromInt(13));
    CHECK(restored.transforms()[live.index].z == rm::sim::Fx::fromInt(34));
    CHECK(restored.transforms()[live.index].heading == 1234);
    CHECK(restored.health()[live.index].current == rm::sim::Mag::fromInt(123));
    CHECK(restored.health()[live.index].maximum == rm::sim::Mag::fromInt(500));
    CHECK(restored.motion()[live.index].armyIndex == 2);
    CHECK(restored.motion()[live.index].destinationX == rm::sim::Fx::fromInt(56));
    CHECK(restored.motion()[live.index].destinationZ == rm::sim::Fx::fromInt(78));
    CHECK(restored.motion()[live.index].moving);
    CHECK(restored.motion()[live.index].distanceTravelledElmos == rm::sim::Fx::fromInt(90));
    REQUIRE(restored.motion()[live.index].path.size() == 1);
    CHECK(restored.motion()[live.index].path[0]
          == std::array<rm::sim::Fx, 2>{rm::sim::Fx::fromInt(56), rm::sim::Fx::fromInt(78)});
    CHECK(restored.parentOf(child) == live);
    CHECK(restored.childrenOf(live) == std::vector<rm::sim::UnitId>{child});

    CHECK(restored.spawn({}) == original.spawn({}));
}

TEST_CASE("save-state versions preserve route phase and versioned factory repeat state", "[save-state]") {
    UnitStore original;
    UnitStore::Spawn moving;
    moving.motion.path.push_back({rm::sim::Fx::fromInt(56), rm::sim::Fx::fromInt(78)});
    moving.motion.pathPhaseStartX = 3;
    moving.motion.pathPhaseStartZ = 5;
    moving.motion.pathPhaseCellsX = 128;
    const auto unit = original.spawn(moving);
    REQUIRE(original.setFactoryRepeat(unit, true));

    RandomStream random{std::uint32_t{1}};
    const SaveState state{.tick = 42, .random = random.snapshot(), .pathServiceBeats = 91,
                          .units = original.snapshot()};
    const auto v3 = SaveState::encode(state);
    const auto restored = SaveState::decode(v3);

    REQUIRE(restored.has_value());
    CHECK(restored->pathServiceBeats == 91);
    rm::sim::PathService resumedPaths;
    resumedPaths.restoreServiceBeats(restored->pathServiceBeats);
    CHECK(resumedPaths.lastServiceBeat() == 90);
    const auto& motion = restored->units.motion[unit.index];
    CHECK(motion.pathPhaseStartX == 3);
    CHECK(motion.pathPhaseStartZ == 5);
    CHECK(motion.pathPhaseCellsX == 128);
    CHECK(restored->units.factoryRepeat[unit.index]);
    CHECK(SaveState::encode(*restored) == v3);

    // v2 was published before factory repeat. It must remain decodable and default the new
    // state to disabled rather than interpreting a different v2 payload shape.
    const auto v2 = SaveState::encodeV2(state);
    const auto published = SaveState::decodeV2(v2);
    REQUIRE(published.has_value());
    CHECK(published->units.motion[unit.index].pathPhaseCellsX == 128);
    CHECK_FALSE(published->units.factoryRepeat[unit.index]);
    CHECK(SaveState::encodeV2(*published) == v2);
    REQUIRE(SaveState::decode(v2).has_value());

    const auto v1 = SaveState::encodeV1({.tick = 42, .random = random.snapshot(),
                                          .units = original.snapshot()});
    const auto old = SaveState::decode(v1);
    REQUIRE(old.has_value());
    CHECK(old->units.motion[unit.index].pathPhaseCellsX == 0);
    CHECK_FALSE(old->units.factoryRepeat[unit.index]);
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

TEST_CASE("a v1 save state refuses invalid unit allocator and attachment state", "[save-state]") {
    UnitStore store;
    const auto parent = store.spawn({});
    const auto child = store.spawn({});
    const auto dead = store.spawn({});
    store.kill(dead);
    REQUIRE(store.attach(parent, child));

    RandomStream random{std::uint32_t{1}};
    const auto bytes = SaveState::encodeV1(
        {.tick = 42, .random = random.snapshot(), .units = store.snapshot()});
    constexpr std::size_t kPayloadOffset = 20;
    const std::size_t units = kPayloadOffset + sizeof(std::uint32_t) + readU32(bytes, kPayloadOffset);

    // IdPool starts with three generations, followed by one free-list entry for `dead`.
    std::vector<std::byte> malformed = bytes;
    const std::size_t freeIndex = units + sizeof(std::uint32_t) + 3 * sizeof(std::uint32_t)
                                  + sizeof(std::uint32_t);
    writeU32(malformed, freeIndex, std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(SaveState::decodeV1(malformed).has_value());

    // The parent list follows the fixed-size arrays in this all-default three-slot snapshot.
    malformed = bytes;
    constexpr std::size_t kTransformBytes = 18;
    constexpr std::size_t kMotionBytes = 43;
    constexpr std::size_t kHealthBytes = 64;
    const std::size_t parents = units
                                + sizeof(std::uint32_t) + 3 * sizeof(std::uint32_t)
                                + sizeof(std::uint32_t) + sizeof(std::uint32_t)
                                + sizeof(std::uint64_t)
                                + sizeof(std::uint32_t) + 3 * sizeof(std::uint32_t)
                                + sizeof(std::uint32_t) + 3 * kTransformBytes
                                + sizeof(std::uint32_t) + 3 * kMotionBytes
                                + sizeof(std::uint32_t) + 3 * kHealthBytes
                                + sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t);
    // Parent slot zero is absent; slot one names `parent` immediately after its presence flag.
    writeU32(malformed, parents + sizeof(std::uint32_t) + 2, std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(SaveState::decodeV1(malformed).has_value());
}
