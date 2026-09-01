#include "core/sim/SaveState.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace rm::sim {
namespace {

// A fixed tag lets the decoder reject arbitrary data before interpreting its fields.
constexpr std::array<std::byte, 4> kMagic{
    std::byte{static_cast<unsigned char>('R')}, std::byte{static_cast<unsigned char>('M')},
    std::byte{static_cast<unsigned char>('S')}, std::byte{static_cast<unsigned char>('V')}};
// The first on-disk layout; later layouts must use a different value.
constexpr std::uint32_t kVersion = 1;
// MT19937 serializes its 624 state words plus an index, all as unsigned decimal numbers separated
// by one space. This rejects oversized malformed frames before they allocate their payload.
constexpr std::size_t kMaxRandomStatePayload =
    (RandomStream::Snapshot::state_size + 1)
        * (std::numeric_limits<std::uint32_t>::digits10 + 1)
    + RandomStream::Snapshot::state_size;

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

[[nodiscard]] bool readU32(std::span<const std::byte> bytes, std::size_t& offset,
                           std::uint32_t& value) {
    if (bytes.size() - offset < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset++]))
                 << shift;
    }
    return true;
}

[[nodiscard]] bool readU64(std::span<const std::byte> bytes, std::size_t& offset,
                           std::uint64_t& value) {
    if (bytes.size() - offset < sizeof(value)) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset++]))
                 << shift;
    }
    return true;
}

} // namespace

std::vector<std::byte> SaveState::encodeV1(const SaveState& state) {
    // The standard stream operators preserve every MT19937 state word and its index.
    std::ostringstream randomState;
    randomState.imbue(std::locale::classic());
    randomState << state.random;
    const std::string payload = randomState.str();
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("MT19937 state exceeds the v1 save-state payload limit");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(kMagic.size() + sizeof(kVersion) + sizeof(state.tick) + sizeof(std::uint32_t)
                  + payload.size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    appendU32(bytes, kVersion);
    appendU64(bytes, state.tick);
    appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    for (const char character : payload) {
        bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return bytes;
}

std::optional<SaveState> SaveState::decodeV1(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    if (bytes.size() < kMagic.size()
        || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }
    offset += kMagic.size();

    std::uint32_t version{};
    std::uint64_t tick{};
    std::uint32_t payloadSize{};
    if (!readU32(bytes, offset, version) || version != kVersion || !readU64(bytes, offset, tick)
        || !readU32(bytes, offset, payloadSize) || bytes.size() - offset != payloadSize
        || payloadSize > kMaxRandomStatePayload) {
        return std::nullopt;
    }

    std::string payload;
    payload.reserve(payloadSize);
    for (const std::byte value : bytes.subspan(offset)) {
        payload.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
    }

    std::istringstream randomState{payload};
    randomState.imbue(std::locale::classic());
    RandomStream::Snapshot random;
    if (!(randomState >> random)) {
        return std::nullopt;
    }
    // The frame has one canonical representation, so equivalent but altered payload text is not
    // silently accepted as a valid save.
    std::ostringstream canonicalState;
    canonicalState.imbue(std::locale::classic());
    canonicalState << random;
    if (canonicalState.str() != payload) {
        return std::nullopt;
    }
    return SaveState{.tick = tick, .random = std::move(random)};
}

} // namespace rm::sim
