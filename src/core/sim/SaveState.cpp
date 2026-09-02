#include "core/sim/SaveState.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
constexpr std::uint32_t kVersion1 = 1;
constexpr std::uint32_t kVersion2 = 2;
constexpr std::uint32_t kVersion3 = 3;
constexpr std::uint32_t kVersion4 = 4;
constexpr std::uint32_t kVersion5 = 5;
constexpr std::uint32_t kVersion6 = 6;
// MT19937 serializes its 624 state words plus an index, all as unsigned decimal numbers separated
// by one space. This rejects oversized malformed frames before they allocate their payload.
constexpr std::size_t kMaxRandomStatePayload =
    (RandomStream::Snapshot::state_size + 1)
        * (std::numeric_limits<std::uint32_t>::digits10 + 1)
    + RandomStream::Snapshot::state_size;

class PayloadWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void count(std::size_t value) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("save-state collection exceeds v1's 32-bit limit");
        }
        u32(static_cast<std::uint32_t>(value));
    }
    void text(const std::string& value) {
        count(value.size());
        for (const char character : value) {
            u8(static_cast<std::uint8_t>(character));
        }
    }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (offset_ == bytes_.size()) return false;
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t& value) {
        std::uint8_t lo{}, hi{};
        if (!u8(lo) || !u8(hi)) return false;
        value = static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi) << 8);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) { std::uint8_t byte{}; if (!u8(byte)) return false; value |= static_cast<std::uint32_t>(byte) << shift; }
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) { std::uint8_t byte{}; if (!u8(byte)) return false; value |= static_cast<std::uint64_t>(byte) << shift; }
        return true;
    }
    [[nodiscard]] bool i32(std::int32_t& value) { std::uint32_t raw{}; return u32(raw) && (value = std::bit_cast<std::int32_t>(raw), true); }
    [[nodiscard]] bool i64(std::int64_t& value) { std::uint64_t raw{}; return u64(raw) && (value = std::bit_cast<std::int64_t>(raw), true); }
    [[nodiscard]] bool count(std::size_t& value, std::size_t minimumBytes = 0) {
        std::uint32_t raw{};
        if (!u32(raw)) return false;
        if constexpr (sizeof(std::size_t) < sizeof(raw)) {
            if (raw > std::numeric_limits<std::size_t>::max()) return false;
        }
        value = raw;
        return minimumBytes == 0 || value <= (bytes_.size() - offset_) / minimumBytes;
    }
    [[nodiscard]] bool text(std::string& value) {
        std::size_t size{};
        if (!count(size) || size > bytes_.size() - offset_) return false;
        value.clear(); value.reserve(size);
        for (std::size_t i = 0; i < size; ++i) value.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[offset_++])));
        return true;
    }
    [[nodiscard]] bool finished() const { return offset_ == bytes_.size(); }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

void writeId(PayloadWriter& writer, UnitId id) { writer.u32(id.index); writer.u32(id.generation); }
[[nodiscard]] bool readId(PayloadReader& reader, UnitId& id) { return reader.u32(id.index) && reader.u32(id.generation); }

void writeUnits(PayloadWriter& w, const UnitStore::Snapshot& s, bool includesPathPhase,
                 bool includesFactoryRepeat, bool includesAttachmentOffsets, bool includesDoNotTarget,
                 bool includesAutomaticTargets) {
    w.count(s.ids.generations.size()); for (Generation v : s.ids.generations) w.u32(v);
    w.count(s.ids.free.size()); for (UnitIndex v : s.ids.free) w.u32(v);
    w.u64(s.ids.live);
    w.count(s.generations.size()); for (Generation v : s.generations) w.u32(v);
    w.count(s.transforms.size()); for (const Transform& v : s.transforms) { w.i32(v.x.raw()); w.i32(v.y.raw()); w.i32(v.z.raw()); w.u16(v.heading); w.u16(v.pitch); w.u16(v.roll); }
    w.count(s.motion.size()); for (const MoveState& v : s.motion) { w.i32(v.armyIndex); w.i32(v.destinationX.raw()); w.i32(v.destinationZ.raw()); w.u8(v.moving); w.u8(v.airborne); w.u8(v.surfaceWater); w.i32(v.speedPerTick.raw()); w.i32(v.turnPerTick); w.i32(v.radiusElmos.raw()); w.i32(v.distanceTravelledElmos.raw()); w.count(v.path.size()); for (const auto& p : v.path) { w.i32(p[0].raw()); w.i32(p[1].raw()); } w.u64(v.pathIndex); if (includesPathPhase) { w.i32(v.pathPhaseStartX); w.i32(v.pathPhaseStartZ); w.i32(v.pathPhaseCellsX); } }
    w.count(s.health.size()); for (const Health& v : s.health) { w.i64(v.current.raw()); w.i64(v.maximum.raw()); w.i64(v.shield.current.raw()); w.i64(v.shield.maximum.raw()); w.u32(v.shield.regenDelayRemaining); w.u32(v.shield.rechargeRemaining); w.count(v.reloadRemaining.size()); for (int x : v.reloadRemaining) w.i32(x); w.count(v.burstRemaining.size()); for (int x : v.burstRemaining) w.i32(x); if (includesAutomaticTargets) { w.count(v.automaticTargets.size()); for (UnitId target : v.automaticTargets) writeId(w, target); } writeId(w, v.lastHitBy); w.i32(v.veterancy.kills); w.i32(v.veterancy.level); }
    w.count(s.types.size()); for (UnitTypeIndex v : s.types) w.u16(v);
    if (includesFactoryRepeat) { w.count(s.factoryRepeat.size()); for (bool v : s.factoryRepeat) w.u8(v); }
    w.count(s.parents.size()); for (const auto& v : s.parents) { w.u8(v.has_value()); if (v) writeId(w, *v); }
    w.count(s.children.size()); for (const auto& list : s.children) { w.count(list.size()); for (UnitId id : list) writeId(w, id); }
    if (includesAttachmentOffsets) { w.count(s.attachmentOffsets.size()); for (const auto& offset : s.attachmentOffsets) { w.i32(offset[0].raw()); w.i32(offset[1].raw()); } }
    if (includesDoNotTarget) { w.count(s.doNotTarget.size()); for (bool v : s.doNotTarget) w.u8(v); }
}

[[nodiscard]] bool readUnits(PayloadReader& r, UnitStore::Snapshot& s, bool includesPathPhase,
                               bool includesFactoryRepeat, bool includesAttachmentOffsets,
                               bool includesDoNotTarget, bool includesAutomaticTargets) {
    std::size_t n{};
    if (!r.count(n, 4)) return false; s.ids.generations.resize(n); for (auto& v : s.ids.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 4)) return false; s.ids.free.resize(n); for (auto& v : s.ids.free) if (!r.u32(v)) return false;
    std::uint64_t live{};
    if (!r.u64(live) || live > std::numeric_limits<std::size_t>::max() || !r.count(n, 4)) return false;
    s.ids.live = static_cast<std::size_t>(live);
    s.generations.resize(n); for (auto& v : s.generations) if (!r.u32(v)) return false;
    if (!r.count(n, 18)) return false; s.transforms.resize(n); for (auto& v : s.transforms) { std::int32_t x{},y{},z{}; if (!r.i32(x)||!r.i32(y)||!r.i32(z)||!r.u16(v.heading)||!r.u16(v.pitch)||!r.u16(v.roll)) return false; v.x=Fx::fromRaw(x); v.y=Fx::fromRaw(y); v.z=Fx::fromRaw(z); }
    if (!r.count(n, includesPathPhase ? 58 : 46)) return false; s.motion.resize(n); for (auto& v : s.motion) { std::int32_t x{},z{},speed{},radius{},distance{}; std::uint8_t moving{},airborne{},water{}; if (!r.i32(v.armyIndex)||!r.i32(x)||!r.i32(z)||!r.u8(moving)||!r.u8(airborne)||!r.u8(water)||moving>1||airborne>1||water>1||!r.i32(speed)||!r.i32(v.turnPerTick)||!r.i32(radius)||!r.i32(distance)||!r.count(n,8)) return false; v.destinationX=Fx::fromRaw(x); v.destinationZ=Fx::fromRaw(z); v.moving=moving; v.airborne=airborne; v.surfaceWater=water; v.speedPerTick=Fx::fromRaw(speed); v.radiusElmos=Fx::fromRaw(radius); v.distanceTravelledElmos=Fx::fromRaw(distance); v.path.resize(n); for(auto& p:v.path){if(!r.i32(x)||!r.i32(z))return false;p={Fx::fromRaw(x),Fx::fromRaw(z)};} std::uint64_t index{}; if(!r.u64(index)||index>std::numeric_limits<std::size_t>::max())return false; v.pathIndex=static_cast<std::size_t>(index); if (includesPathPhase && (!r.i32(v.pathPhaseStartX) || !r.i32(v.pathPhaseStartZ) || !r.i32(v.pathPhaseCellsX))) return false; }
    if (!r.count(n, 52)) return false; s.health.resize(n); for (auto& v : s.health) { std::int64_t a{},b{},c{},d{}; if(!r.i64(a)||!r.i64(b)||!r.i64(c)||!r.i64(d)||!r.u32(v.shield.regenDelayRemaining)||!r.u32(v.shield.rechargeRemaining)||!r.count(n,4))return false; v.current=Mag::fromRaw(a);v.maximum=Mag::fromRaw(b);v.shield.current=Mag::fromRaw(c);v.shield.maximum=Mag::fromRaw(d);v.reloadRemaining.resize(n);for(auto& x:v.reloadRemaining)if(!r.i32(x))return false;if(!r.count(n,4))return false;v.burstRemaining.resize(n);for(auto& x:v.burstRemaining)if(!r.i32(x))return false;if (includesAutomaticTargets) { if (!r.count(n, 8)) return false; v.automaticTargets.resize(n); for (auto& target : v.automaticTargets) if (!readId(r, target)) return false; } if(!readId(r,v.lastHitBy)||!r.i32(v.veterancy.kills)||!r.i32(v.veterancy.level))return false; }
    if (!r.count(n,2)) return false; s.types.resize(n); for(auto& v:s.types)if(!r.u16(v))return false;
    if (includesFactoryRepeat) { if (!r.count(n, 1)) return false; s.factoryRepeat.resize(n); for (auto&& v : s.factoryRepeat) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    if (!r.count(n,1)) return false; s.parents.resize(n); for(auto& v:s.parents){std::uint8_t has{};if(!r.u8(has)||has>1)return false;if(has){UnitId id;if(!readId(r,id))return false;v=id;}}
    if (!r.count(n,4)) return false; s.children.resize(n); for(auto& list:s.children){if(!r.count(n,8))return false;list.resize(n);for(auto& id:list)if(!readId(r,id))return false;}
    if (includesAttachmentOffsets) { if (!r.count(n, 8)) return false; s.attachmentOffsets.resize(n); for (auto& offset : s.attachmentOffsets) { std::int32_t x{}, z{}; if (!r.i32(x) || !r.i32(z)) return false; offset = {Fx::fromRaw(x), Fx::fromRaw(z)}; } }
    if (includesDoNotTarget) { if (!r.count(n, 1)) return false; s.doNotTarget.resize(n); for (auto&& v : s.doNotTarget) { std::uint8_t enabled{}; if (!r.u8(enabled) || enabled > 1) return false; v = enabled; } }
    const std::size_t slots=s.transforms.size();
    if (s.ids.generations.size()!=slots || s.generations.size()!=slots || s.motion.size()!=slots
         || s.health.size()!=slots || s.types.size()!=slots
          || (includesFactoryRepeat && s.factoryRepeat.size()!=slots) || s.parents.size()!=slots
          || s.children.size()!=slots || (includesAttachmentOffsets && s.attachmentOffsets.size()!=slots)
          || (includesDoNotTarget && s.doNotTarget.size()!=slots)
         || s.ids.live>slots || s.ids.free.size()>slots) return false;

    std::vector<bool> free(slots);
    for (UnitIndex index : s.ids.free) {
        if (index >= slots || free[index]) return false;
        free[index] = true;
    }
    std::size_t liveSlots = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (free[slot]) {
            if (s.generations[slot] == s.ids.generations[slot]) return false;
        } else {
            if (s.ids.generations[slot] == 0 || s.generations[slot] != s.ids.generations[slot]) return false;
            ++liveSlots;
        }
    }
    if (liveSlots != s.ids.live) return false;

    const auto isLive = [&](UnitId id) {
        return id.index < slots && !free[id.index] && id.generation != 0
               && id.generation == s.ids.generations[id.index];
    };
    for (std::size_t child = 0; child < slots; ++child) {
        if (s.parents[child] && (!isLive(*s.parents[child]) || free[child])) return false;
    }
    for (std::size_t parent = 0; parent < slots; ++parent) {
        std::vector<bool> seenChild(slots);
        for (UnitId child : s.children[parent]) {
            if (!isLive(UnitId{static_cast<UnitIndex>(parent), s.ids.generations[parent]})
                || !isLive(child) || seenChild[child.index] || !s.parents[child.index]
                || *s.parents[child.index]
                       != UnitId{static_cast<UnitIndex>(parent), s.ids.generations[parent]}) return false;
            seenChild[child.index] = true;
        }
    }
    for (std::size_t child = 0; child < slots; ++child) {
        if (s.parents[child]
            && std::find(s.children[s.parents[child]->index].begin(),
                         s.children[s.parents[child]->index].end(),
                         UnitId{static_cast<UnitIndex>(child), s.ids.generations[child]})
                   == s.children[s.parents[child]->index].end()) return false;
        std::size_t steps = 0;
        for (std::optional<UnitId> parent = s.parents[child]; parent; parent = s.parents[parent->index]) {
            if (++steps > slots) return false;
        }
    }
    if (!includesFactoryRepeat) s.factoryRepeat.resize(slots, false);
    if (!includesDoNotTarget) s.doNotTarget.resize(slots, false);
    if (!includesAttachmentOffsets) {
        s.attachmentOffsets.resize(slots);
        for (std::size_t child = 0; child < slots; ++child) {
            if (s.parents[child]) {
                const UnitIndex parent = s.parents[child]->index;
                s.attachmentOffsets[child] = {s.transforms[child].x - s.transforms[parent].x,
                                              s.transforms[child].z - s.transforms[parent].z};
            }
        }
    }
    return true;
}

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

[[nodiscard]] std::vector<std::byte> encode(const SaveState& state, std::uint32_t version) {
    // The standard stream operators preserve every MT19937 state word and its index.
    std::ostringstream randomState;
    randomState.imbue(std::locale::classic());
    randomState << state.random;
    PayloadWriter payloadWriter;
    payloadWriter.text(randomState.str());
    if (version >= kVersion2) payloadWriter.u64(state.pathServiceBeats);
    writeUnits(payloadWriter, state.units, version >= kVersion2, version >= kVersion3,
                 version >= kVersion4, version >= kVersion5, version >= kVersion6);
    const std::vector<std::byte> payload = payloadWriter.take();
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("MT19937 state exceeds the v1 save-state payload limit");
    }

    std::vector<std::byte> bytes;
    bytes.reserve(kMagic.size() + sizeof(version) + sizeof(state.tick) + sizeof(std::uint32_t)
                  + payload.size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    appendU32(bytes, version);
    appendU64(bytes, state.tick);
    appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

[[nodiscard]] std::optional<SaveState> decode(std::span<const std::byte> bytes,
                                              std::optional<std::uint32_t> requiredVersion) {
    std::size_t offset = 0;
    if (bytes.size() < kMagic.size()
        || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }
    offset += kMagic.size();

    std::uint32_t version{};
    std::uint64_t tick{};
    std::uint32_t payloadSize{};
    if (!readU32(bytes, offset, version)
        || (version != kVersion1 && version != kVersion2 && version != kVersion3
            && version != kVersion4 && version != kVersion5 && version != kVersion6)
        || (requiredVersion && version != *requiredVersion) || !readU64(bytes, offset, tick)
        || !readU32(bytes, offset, payloadSize) || bytes.size() - offset != payloadSize) {
        return std::nullopt;
    }

    PayloadReader reader{bytes.subspan(offset)};
    std::string payload;
    if (!reader.text(payload) || payload.size() > kMaxRandomStatePayload) return std::nullopt;

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
    std::uint64_t pathServiceBeats{};
    if (version >= kVersion2 && !reader.u64(pathServiceBeats)) return std::nullopt;
    UnitStore::Snapshot units;
    if (!readUnits(reader, units, version >= kVersion2, version >= kVersion3, version >= kVersion4,
                    version >= kVersion5, version >= kVersion6)
        || !reader.finished()) return std::nullopt;
    SaveState decoded{.tick = tick,
                      .random = std::move(random),
                      .pathServiceBeats = pathServiceBeats,
                      .units = std::move(units)};
    // One binary representation per state rejects alternate encodings and trailing data.
    const std::vector<std::byte> canonical = encode(decoded, version);
    if (canonical.size() != bytes.size()
        || !std::equal(canonical.begin(), canonical.end(), bytes.begin())) return std::nullopt;
    return decoded;
}

std::vector<std::byte> SaveState::encodeV1(const SaveState& state) {
    return rm::sim::encode(state, kVersion1);
}

std::optional<SaveState> SaveState::decodeV1(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, kVersion1);
}

std::vector<std::byte> SaveState::encodeV2(const SaveState& state) {
    return rm::sim::encode(state, kVersion2);
}

std::optional<SaveState> SaveState::decodeV2(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, kVersion2);
}

std::vector<std::byte> SaveState::encode(const SaveState& state) {
    return rm::sim::encode(state, kVersion6);
}

std::optional<SaveState> SaveState::decode(std::span<const std::byte> bytes) {
    return rm::sim::decode(bytes, std::nullopt);
}

} // namespace rm::sim
