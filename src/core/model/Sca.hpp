#pragma once

#include "core/Error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rm::sca {

// "ANIM", not NUL-terminated.
inline constexpr char kMagic[4] = {'A', 'N', 'I', 'M'};

/// The only version retail Forged Alliance ships.
inline constexpr std::uint32_t kVersion = 5;

// Section markers, immediately preceding the data their header offset names —
// the same self-check .scm has.
inline constexpr char kNameMarker[4] = {'N', 'A', 'M', 'E'};
inline constexpr char kLinkMarker[4] = {'L', 'I', 'N', 'K'};
inline constexpr char kDataMarker[4] = {'D', 'A', 'T', 'A'};

/// Position and rotation of one bone at one instant. Rigid: the format carries
/// no scale, which is what lets a pose compose as quaternion-plus-vector rather
/// than as a matrix.
struct Key {
    std::array<float, 3> position{{0.0f, 0.0f, 0.0f}};
    std::array<float, 4> rotation{{1.0f, 0.0f, 0.0f, 0.0f}};  ///< quaternion (w, x, y, z)
};

/// One keyframe: a time in seconds and every bone's local transform at it.
struct Frame {
    float time = 0.0f;
    std::vector<Key> bones;
};

// A decoded Supreme Commander animation.
//
// This is the thing glTF would have lost. Recoil parses glTF animation tracks
// and then never reads them (ADR-004), and .s3o carries no rotation at all — so
// .sca is the only keyframed animation any format in scope actually delivers.
//
// The bone list is the ANIMATION's, not the model's: the two agree in practice
// but nothing in either file enforces it, so they are matched by name at
// playback (core/model/Pose.hpp) rather than assumed to be parallel.
struct Animation {
    std::string name;
    float duration = 0.0f;  ///< seconds; equals the last frame's time

    std::vector<std::string> boneNames;
    std::vector<int> boneParents;  ///< -1 for the root

    std::vector<Frame> frames;

    // A single position/rotation record sits between the DATA marker and the
    // first frame. Its purpose is not established — it is identity on the
    // animations examined — so it is decoded and exposed rather than skipped
    // silently or given a meaning it may not have.
    Key reference;

    [[nodiscard]] bool empty() const noexcept { return frames.empty() || boneNames.empty(); }
    [[nodiscard]] std::size_t boneCount() const noexcept { return boneNames.size(); }
};

// Parses a Supreme Commander .sca.
//
// Self-checking in the same way the corpus was validated: the frame stride the
// header states must equal what the bone count implies, and the frames must end
// exactly at EOF. Both hold for all 474 retail animations, and either failing
// means the file is not what the header claims.
[[nodiscard]] std::expected<Animation, MapError> load(std::span<const std::byte> bytes);

[[nodiscard]] std::expected<Animation, MapError> loadFile(const std::filesystem::path& path);

} // namespace rm::sca
