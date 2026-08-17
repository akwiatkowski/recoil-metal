#include "core/model/Sca.hpp"

#include "core/map/ByteReader.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace {

using rm::MapError;

// Header field offsets, after the 4-byte magic.
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffFrameCount = 8;
constexpr std::size_t kOffDuration = 12;
constexpr std::size_t kOffBoneCount = 16;
constexpr std::size_t kOffNameOffset = 20;
constexpr std::size_t kOffLinkOffset = 24;
constexpr std::size_t kOffDataOffset = 28;
constexpr std::size_t kOffFrameStride = 32;

/// Bytes per bone in a frame: a position and a quaternion.
constexpr std::size_t kKeySize = 7 * 4;

/// Bytes before the per-bone keys in each frame: the frame's time and a flags
/// word that is 0 on every animation examined.
constexpr std::size_t kFrameHeaderSize = 8;

constexpr std::size_t kMaxNameLength = 256;

[[nodiscard]] std::unexpected<MapError> fail(MapError::Code code, std::string message) {
    return std::unexpected{MapError{code, std::move(message)}};
}

[[nodiscard]] bool markerPrecedes(std::span<const std::byte> bytes, std::size_t offset,
                                  const char (&marker)[4]) noexcept {
    if (offset < 4 || offset > bytes.size()) {
        return false;
    }
    return std::memcmp(bytes.data() + offset - 4, marker, 4) == 0;
}

[[nodiscard]] rm::sca::Key readKey(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return rm::sca::Key{
        .position = {{rm::readF32(bytes, offset), rm::readF32(bytes, offset + 4),
                      rm::readF32(bytes, offset + 8)}},
        .rotation = {{rm::readF32(bytes, offset + 12), rm::readF32(bytes, offset + 16),
                      rm::readF32(bytes, offset + 20), rm::readF32(bytes, offset + 24)}},
    };
}

} // namespace

namespace rm::sca {

std::expected<Animation, MapError> load(std::span<const std::byte> bytes) {
    constexpr std::size_t kHeaderSize = 36;
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail(MapError::Code::NotScmap,
                    "not a Supreme Commander animation: expected the 4-byte magic \"ANIM\"");
    }

    const std::uint32_t version = readU32(bytes, kOffVersion);
    if (version != kVersion) {
        return fail(MapError::Code::BadHeader,
                    "unsupported .sca version " + std::to_string(version)
                        + " (this reader decodes retail v" + std::to_string(kVersion) + ")");
    }

    const auto frameCount = static_cast<std::size_t>(readU32(bytes, kOffFrameCount));
    const float duration = readF32(bytes, kOffDuration);
    const auto boneCount = static_cast<std::size_t>(readU32(bytes, kOffBoneCount));
    const auto nameOffset = static_cast<std::size_t>(readU32(bytes, kOffNameOffset));
    const auto linkOffset = static_cast<std::size_t>(readU32(bytes, kOffLinkOffset));
    const auto dataOffset = static_cast<std::size_t>(readU32(bytes, kOffDataOffset));
    const auto frameStride = static_cast<std::size_t>(readU32(bytes, kOffFrameStride));

    if (!markerPrecedes(bytes, nameOffset, kNameMarker)
        || !markerPrecedes(bytes, linkOffset, kLinkMarker)
        || !markerPrecedes(bytes, dataOffset, kDataMarker)) {
        return fail(MapError::Code::BadHeader,
                    "a section offset does not point just past its NAME/LINK/DATA marker, "
                    "so the header is not describing this file");
    }

    if (boneCount == 0 || frameCount == 0) {
        return fail(MapError::Code::BadGeometry, "animation has no bones or no frames");
    }

    // The stride is stated AND derivable. Checking the two against each other
    // is what proves the per-bone record is seven floats and not, say, a matrix.
    const std::size_t expectedStride = kFrameHeaderSize + boneCount * kKeySize;
    if (frameStride != expectedStride) {
        return fail(MapError::Code::BadHeader,
                    "frame stride " + std::to_string(frameStride) + " does not match "
                        + std::to_string(boneCount) + " bones (" + std::to_string(expectedStride)
                        + ")");
    }

    // One reference record sits between the marker and the first frame; the
    // frames then run to the last byte. Requiring EXACTLY that is the same acid
    // test the .scmap reader uses, and it holds on all 474 retail animations.
    const std::size_t end = dataOffset + kKeySize + frameCount * frameStride;
    if (end != bytes.size()) {
        return fail(MapError::Code::Truncated,
                    "frame data ends at " + std::to_string(end) + " in a "
                        + std::to_string(bytes.size())
                        + "-byte file; a header field is being read wrongly");
    }

    Animation animation;
    animation.duration = duration;

    // --- Bone names --------------------------------------------------------
    // Packed NUL-terminated strings, one after another, in bone order.
    animation.boneNames.reserve(boneCount);
    std::size_t at = nameOffset;
    for (std::size_t i = 0; i < boneCount; ++i) {
        std::string name;
        while (at < bytes.size() && name.size() <= kMaxNameLength) {
            const auto c = static_cast<char>(bytes[at++]);
            if (c == '\0') {
                break;
            }
            name.push_back(c);
        }
        if (name.empty() || name.size() > kMaxNameLength) {
            return fail(MapError::Code::Truncated,
                        "bone name " + std::to_string(i) + " is empty or unterminated");
        }
        animation.boneNames.push_back(std::move(name));
    }
    if (at > linkOffset) {
        return fail(MapError::Code::Truncated,
                    "bone names overrun the LINK section, so the bone count is wrong");
    }

    // --- Parent links ------------------------------------------------------
    animation.boneParents.reserve(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const std::int32_t parent = readI32(bytes, linkOffset + i * 4);
        if (parent < -1 || parent >= static_cast<std::int32_t>(boneCount)) {
            return fail(MapError::Code::BadGeometry,
                        "bone " + std::to_string(i) + " links to " + std::to_string(parent)
                            + ", which is not a bone");
        }
        animation.boneParents.push_back(parent);
    }

    // --- Frames ------------------------------------------------------------
    animation.reference = readKey(bytes, dataOffset);

    animation.frames.reserve(frameCount);
    float previousTime = -1.0f;
    for (std::size_t f = 0; f < frameCount; ++f) {
        const std::size_t frameAt = dataOffset + kKeySize + f * frameStride;

        Frame frame;
        frame.time = readF32(bytes, frameAt);
        if (frame.time < previousTime) {
            return fail(MapError::Code::BadGeometry,
                        "frame " + std::to_string(f) + " goes back in time");
        }
        previousTime = frame.time;

        frame.bones.reserve(boneCount);
        for (std::size_t b = 0; b < boneCount; ++b) {
            frame.bones.push_back(readKey(bytes, frameAt + kFrameHeaderSize + b * kKeySize));
        }

        animation.frames.push_back(std::move(frame));
    }

    return animation;
}

std::expected<Animation, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return fail(MapError::Code::Truncated, "could not open \"" + path.string() + "\"");
    }

    const std::vector<char> data{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};

    auto animation = load(std::as_bytes(std::span{data}));
    if (animation) {
        animation->name = path.stem().string();
    }
    return animation;
}

} // namespace rm::sca
