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
#include <memory>
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

TEST_CASE("the current save state preserves captured attachment offsets", "[save-state]") {
    UnitStore original;
    UnitStore::Spawn parent;
    parent.transform.x = rm::sim::Fx::fromInt(10);
    parent.transform.y = rm::sim::Fx::fromInt(20);
    parent.transform.z = rm::sim::Fx::fromInt(20);
    const auto parentId = original.spawn(parent);
    UnitStore::Spawn child;
    child.transform.x = rm::sim::Fx::fromInt(13);
    child.transform.y = rm::sim::Fx::fromInt(27);
    child.transform.z = rm::sim::Fx::fromInt(25);
    child.motion.moving = true;
    const auto childId = original.spawn(child);
    REQUIRE(original.attach(parentId, childId));

    RandomStream random{std::uint32_t{1}};
    const auto saved = SaveState::decode(
        SaveState::encode({.tick = 42, .random = random.snapshot(), .units = original.snapshot()}));
    REQUIRE(saved.has_value());
    REQUIRE(saved->units.attachmentOffsets[childId.index]
            == std::array<rm::sim::Fx, 2>{rm::sim::Fx::fromInt(3), rm::sim::Fx::fromInt(5)});
    REQUIRE(saved->units.attachmentHeights[childId.index] == rm::sim::Fx::fromInt(7));
    REQUIRE(saved->units.motion[childId.index].attached);

    UnitStore restored{saved->units};
    restored.transforms()[parentId.index].x = rm::sim::Fx::fromInt(30);
    restored.transforms()[parentId.index].y = rm::sim::Fx::fromInt(40);
    restored.transforms()[parentId.index].z = rm::sim::Fx::fromInt(40);
    restored.propagateAttachments();
    CHECK(restored.transforms()[childId.index].x == rm::sim::Fx::fromInt(33));
    CHECK(restored.transforms()[childId.index].y == rm::sim::Fx::fromInt(47));
    CHECK(restored.transforms()[childId.index].z == rm::sim::Fx::fromInt(45));
}

TEST_CASE("the current save state preserves DoNotTarget", "[save-state]") {
    UnitStore original;
    const auto unit = original.spawn({});
    REQUIRE(original.setDoNotTarget(unit, true));

    RandomStream random{std::uint32_t{1}};
    const auto saved = SaveState::decode(
        SaveState::encode({.tick = 42, .random = random.snapshot(), .units = original.snapshot()}));
    REQUIRE(saved.has_value());
    const UnitStore restored{saved->units};
    CHECK(restored.doNotTarget(unit));
}

TEST_CASE("the current save state preserves generation-safe automatic weapon incumbents", "[save-state]") {
    UnitStore original;
    const auto gunner = original.spawn({});
    const auto target = original.spawn({});
    original.health()[gunner.index].automaticTargets = {target};

    RandomStream random{std::uint32_t{1}};
    const auto saved = SaveState::decode(
        SaveState::encode({.tick = 42, .random = random.snapshot(), .units = original.snapshot()}));
    REQUIRE(saved.has_value());
    REQUIRE(saved->units.health[gunner.index].automaticTargets == std::vector<rm::sim::UnitId>{target});
}

TEST_CASE("the current save state restores shared queued commands and their allocators", "[save-state]") {
    UnitStore original;
    const auto first = original.spawn({});
    const auto second = original.spawn({});
    constexpr rm::CommandSource source = 7;
    const auto id = original.allocateCommandId(source);
    REQUIRE(id.has_value());

    auto command = std::make_shared<rm::sim::SharedCommand>();
    command->source = source;
    command->id = *id;
    command->kind = rm::sim::CommandKind::Move;
    command->units = {first, second};
    command->creationSerial = original.allocateCommandSerial();
    REQUIRE(original.registerCommand(command));
    original.orders()[first.index].append(rm::sim::QueuedCommand{first, command});
    original.orders()[first.index].markCurrentActive();
    original.orders()[second.index].append(rm::sim::QueuedCommand{second, command});
    original.orders()[first.index].currentMutable()->setTargetPosition(rm::sim::Fx::fromInt(12),
                                                                        rm::sim::Fx::fromInt(34));
    original.orders()[second.index].currentMutable()->setTargetPosition(rm::sim::Fx::fromInt(56),
                                                                         rm::sim::Fx::fromInt(78));
    command.reset();

    RandomStream random{std::uint32_t{1}};
    const auto saved = SaveState::decode(
        SaveState::encode({.tick = 42, .random = random.snapshot(), .units = original.snapshot()}));
    REQUIRE(saved.has_value());
    UnitStore restored{saved->units};

    REQUIRE(restored.orders()[first.index].size() == 1);
    REQUIRE(restored.orders()[second.index].size() == 1);
    const auto& firstEntry = restored.orders()[first.index].entries().front();
    const auto& secondEntry = restored.orders()[second.index].entries().front();
    CHECK(&firstEntry.payload() == &secondEntry.payload());
    CHECK(restored.orders()[first.index].activeEntry() == &firstEntry);
    CHECK(firstEntry.targetX() == rm::sim::Fx::fromInt(12));
    CHECK(firstEntry.targetZ() == rm::sim::Fx::fromInt(34));
    CHECK(secondEntry.targetX() == rm::sim::Fx::fromInt(56));
    CHECK(secondEntry.targetZ() == rm::sim::Fx::fromInt(78));
    CHECK(restored.liveCommand(*id).get() == &firstEntry.payload());
    CHECK(restored.allocateCommandSerial() == 1);
    CHECK(restored.allocateCommandId(source) == rm::commandId(source, 1));
}

TEST_CASE("historic attachment saves derive offsets from their transforms", "[save-state]") {
    UnitStore original;
    const auto parent = original.spawn({.transform = {.x = rm::sim::Fx::fromInt(10),
                                                       .z = rm::sim::Fx::fromInt(20)}});
    const auto child = original.spawn({.transform = {.x = rm::sim::Fx::fromInt(13),
                                                      .z = rm::sim::Fx::fromInt(25)}});
    REQUIRE(original.attach(parent, child));
    original.health()[parent.index].veterancy.kills = 7;

    RandomStream random{std::uint32_t{1}};
    const SaveState state{.tick = 42, .random = random.snapshot(), .units = original.snapshot()};
    std::vector<std::byte> v7 = SaveState::encode(state);
    constexpr std::size_t kSlots = 2;
    // v8 appends allocator and queue state, v9 the silo-ammo section, v10 the
    // redirector section, v11 the per-motion air section (a count word plus one
    // fixed record per motion slot — empty vectors still write their counts, which is
    // what makes these trailers computable without parsing). This fixture starts from
    // the final published v7 shape, so the historical-layout edits below must remove
    // all four later trailers first. V12 extends command records in place, so this empty-queue
    // fixture adds no bytes for it; v13 adds one combat-state byte per air record.
    constexpr std::size_t kV8CommandStateBytes = sizeof(rm::CommandSerial)
                                                  + std::size_t{rm::kInvalidCommandSource}
                                                        * sizeof(std::uint32_t)
                                                  + 2 * sizeof(std::uint32_t)
                                                  + kSlots
                                                        * (sizeof(std::uint8_t)
                                                           + sizeof(std::uint32_t));
    constexpr std::size_t kV9SiloAmmoBytes = sizeof(std::uint32_t);
    constexpr std::size_t kV10RedirectBytes = sizeof(std::uint32_t);
    constexpr std::size_t kV13AirBytes = sizeof(std::uint32_t) + kSlots * 35;
    v7.resize(v7.size() - kV13AirBytes - kV10RedirectBytes - kV9SiloAmmoBytes
              - kV8CommandStateBytes);
    writeU32(v7, 4, 7);
    writeU32(v7, 16, static_cast<std::uint32_t>(v7.size() - 20));
    // v4 adds the offset collection, v5 adds DoNotTarget, v6 adds one automatic-target count
    // to every health record, and v7 adds attached motion plus the local height. Removing the
    // additions recreates the published v3 shape, which stored the attachment graph but none of
    // those later states.
    constexpr std::size_t kV7MotionBytes = 56;
    constexpr std::size_t kV6HealthBytes = 68;
    constexpr std::size_t kAutomaticTargetCountOffset = 48;
    const std::size_t units = 20 + sizeof(std::uint32_t) + readU32(v7, 20);
    const std::size_t health = units
                               + sizeof(std::uint32_t) + kSlots * sizeof(std::uint32_t)
                               + sizeof(std::uint32_t) + sizeof(std::uint64_t)
                               + sizeof(std::uint32_t) + kSlots * sizeof(std::uint32_t)
                               + sizeof(std::uint32_t) + kSlots * 18
                                 + sizeof(std::uint32_t) + kSlots * kV7MotionBytes
                                 + sizeof(std::uint32_t);
    const auto publishedV7 = SaveState::decode(v7);
    REQUIRE(publishedV7.has_value());
    CHECK(publishedV7->units.orders.empty());

    constexpr std::size_t kAttachedMotionOffset = 13;
    const std::size_t motion = health - sizeof(std::uint32_t) - kSlots * kV7MotionBytes;
    std::vector<std::byte> v6 = v7;
    constexpr std::size_t kDoNotTargetBytes = sizeof(std::uint32_t) + kSlots * sizeof(std::uint8_t);
    constexpr std::size_t kAttachmentHeightBytes = sizeof(std::uint32_t) + kSlots * sizeof(std::int32_t);
    v6.erase(v6.end() - static_cast<std::ptrdiff_t>(kDoNotTargetBytes + kAttachmentHeightBytes),
             v6.end() - static_cast<std::ptrdiff_t>(kDoNotTargetBytes));
    for (std::size_t slot = kSlots; slot-- > 0;) {
        const std::size_t attached = motion + sizeof(std::uint32_t) + slot * kV7MotionBytes
                                     + kAttachedMotionOffset;
        v6.erase(v6.begin() + static_cast<std::ptrdiff_t>(attached));
    }
    writeU32(v6, 4, 6);
    writeU32(v6, 16, static_cast<std::uint32_t>(v6.size() - 20));
    const auto publishedV6 = SaveState::decode(v6);
    REQUIRE(publishedV6.has_value());
    CHECK(publishedV6->units.orders.empty());
    CHECK(publishedV6->units.motion[child.index].attached);

    for (std::size_t slot = kSlots; slot-- > 0;) {
        const std::size_t count = health + slot * kV6HealthBytes + kAutomaticTargetCountOffset;
        v7.erase(v7.begin() + static_cast<std::ptrdiff_t>(count),
                  v7.begin() + static_cast<std::ptrdiff_t>(count + sizeof(std::uint32_t)));
    }
    for (std::size_t slot = kSlots; slot-- > 0;) {
        const std::size_t attached = motion + sizeof(std::uint32_t) + slot * kV7MotionBytes
                                     + kAttachedMotionOffset;
        v7.erase(v7.begin() + static_cast<std::ptrdiff_t>(attached));
    }
    v7.erase(v7.end() - static_cast<std::ptrdiff_t>(kDoNotTargetBytes + kAttachmentHeightBytes),
             v7.end() - static_cast<std::ptrdiff_t>(kDoNotTargetBytes));

    std::vector<std::byte> v5 = v7;
    writeU32(v5, 4, 5);
    writeU32(v5, 16, static_cast<std::uint32_t>(v5.size() - 20));

    std::vector<std::byte> v4 = v5;
    v4.resize(v4.size() - (sizeof(std::uint32_t) + kSlots * sizeof(std::uint8_t)));
    writeU32(v4, 4, 4);
    writeU32(v4, 16, static_cast<std::uint32_t>(v4.size() - 20));

    std::vector<std::byte> v3 = v4;
    v3.resize(v3.size() - (sizeof(std::uint32_t) + kSlots * 2 * sizeof(std::int32_t)));
    writeU32(v3, 4, 3);
    writeU32(v3, 16, static_cast<std::uint32_t>(v3.size() - 20));

    const auto checkDerivedOffset = [&](const std::vector<std::byte>& bytes) {
        const auto saved = SaveState::decode(bytes);
        REQUIRE(saved.has_value());
        CHECK(saved->units.health[parent.index].automaticTargets.empty());
        CHECK(saved->units.health[parent.index].veterancy.kills == 7);
        UnitStore restored{saved->units};
        CHECK(restored.attachmentOffsetOf(child)
              == std::array<rm::sim::Fx, 2>{rm::sim::Fx::fromInt(3), rm::sim::Fx::fromInt(5)});
        restored.transforms()[parent.index].x = rm::sim::Fx::fromInt(30);
        restored.transforms()[parent.index].z = rm::sim::Fx::fromInt(40);
        restored.propagateAttachments();
        CHECK(restored.transforms()[child.index].x == rm::sim::Fx::fromInt(33));
        CHECK(restored.transforms()[child.index].z == rm::sim::Fx::fromInt(45));
    };

    checkDerivedOffset(SaveState::encodeV1(state));
    checkDerivedOffset(SaveState::encodeV2(state));
    checkDerivedOffset(v3);
    checkDerivedOffset(v4);
    checkDerivedOffset(v5);
}

TEST_CASE("a v9 save round-trips silo ammunition state", "[save-state]") {
    RandomStream random{std::uint32_t{1}};
    const SaveState state{
        .tick = 42,
        .random = random.snapshot(),
        .siloAmmo = {{.owner = {.index = 3, .generation = 2},
                      .weapon = 1,
                      .slot = 1,  // the nuke slot — a v9 field this test must prove survives
                      .stored = 4,
                      .capacity = 7,
                      .totalTicks = 2400,
                      .elapsedTicks = 17,
                      .costPerTick = {.mass = rm::sim::Mag::fromInt(1),
                                      .energy = rm::sim::Mag::fromInt(150)},
                      .delivered = {.mass = rm::sim::Mag::fromInt(1),
                                    .energy = rm::sim::Mag::fromInt(75)}}},
    };
    const auto bytes = SaveState::encode(state);
    const auto restored = SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->siloAmmo.size() == 1);
    CHECK(restored->siloAmmo.front().slot == 1);
    CHECK(restored->siloAmmo.front().stored == 4);
    CHECK(restored->siloAmmo.front().elapsedTicks == 17);
    CHECK(SaveState::encode(*restored) == bytes);
}

TEST_CASE("a v10 save round-trips redirector state", "[save-state]") {
    RandomStream random{std::uint32_t{1}};
    const SaveState state{
        .tick = 42,
        .random = random.snapshot(),
        .redirects = {{.owner = {.index = 5, .generation = 1},
                       .radiusElmos = rm::sim::fxFromFloat(40.0f),
                       .cooldownTicks = 10,
                       .remaining = 4}},
    };
    const auto bytes = SaveState::encode(state);
    const auto restored = SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->redirects.size() == 1);
    CHECK(restored->redirects.front().remaining == 4);
    CHECK(restored->redirects.front().cooldownTicks == 10);
    CHECK(SaveState::encode(*restored) == bytes);
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

TEST_CASE("a v13 save round-trips winged-flight state", "[save-state]") {
    rm::sim::UnitStore original;
    const auto flyer = original.spawn({});
    rm::sim::MoveState& motion = original.motion()[flyer.index];
    motion.canFly = true;
    motion.airState = rm::sim::MoveState::AirState::Up;
    motion.airCombatState = rm::sim::MoveState::AirCombatState::TailChase;
    motion.velocity = {rm::sim::Fx::fromInt(10), rm::sim::Fx::fromInt(1),
                       rm::sim::Fx::fromInt(-3)};
    motion.altitudeRef = rm::sim::Fx::fromInt(80);
    motion.fuelRatio = rm::sim::Fx::fromRatio(1, 2);
    motion.idleTicks = 7;
    motion.airMaxSpeedElmosPerSec = rm::sim::Fx::fromInt(160);

    RandomStream random{std::uint32_t{1}};
    const auto bytes =
        SaveState::encode({.tick = 42, .random = random.snapshot(), .units = original.snapshot()});
    const auto restored = SaveState::decode(bytes);
    REQUIRE(restored.has_value());
    REQUIRE(restored->units.motion.size() == original.motion().size());
    const rm::sim::MoveState& back = restored->units.motion[flyer.index];
    CHECK(back.airState == rm::sim::MoveState::AirState::Up);
    CHECK(back.airCombatState == rm::sim::MoveState::AirCombatState::TailChase);
    CHECK(back.canFly);
    CHECK(back.velocity[0] == rm::sim::Fx::fromInt(10));
    CHECK(back.altitudeRef == rm::sim::Fx::fromInt(80));
    CHECK(back.idleTicks == 7);
    CHECK(back.airMaxSpeedElmosPerSec == rm::sim::Fx::fromInt(160));
    CHECK(SaveState::encode(*restored) == bytes);
}
