// Validation against the REAL retail Supreme Commander animation corpus.
// Extracted alongside the models — see tests/test_real_scm.cpp — and skipped
// when they are not present.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/model/Pose.hpp"
#include "core/model/Sca.hpp"
#include "core/model/Scm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] std::vector<std::filesystem::path> corpus(const char* extension) {
    std::vector<std::filesystem::path> files;

    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return files;
    }
    const std::filesystem::path root =
        std::filesystem::path{home} / "projects/llm/input/faf/units";

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator{root, ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == extension) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST_CASE("every retail .sca decodes and its frames end exactly at EOF") {
    const auto animations = corpus(".sca");
    if (animations.empty()) {
        SKIP("Supreme Commander animations not extracted");
    }

    std::size_t keyframes = 0;
    float longest = 0.0f;

    for (const std::filesystem::path& path : animations) {
        const auto animation = rm::sca::loadFile(path);

        INFO("animation: " << path.filename().string());
        REQUIRE(animation.has_value());
        REQUIRE_FALSE(animation->empty());

        // The loader enforces the end-at-EOF rule; reaching here means it held.
        REQUIRE(animation->duration > 0.0f);
        REQUIRE(animation->boneParents.size() == animation->boneCount());

        // Every frame carries a key for every bone — the stride check in the
        // loader is what guarantees it, and this is the visible consequence.
        for (const rm::sca::Frame& frame : animation->frames) {
            REQUIRE(frame.bones.size() == animation->boneCount());
        }

        // The last key IS the duration. That the header's float and the frame
        // times agree is a second, independent check on the frame stride.
        REQUIRE(animation->frames.back().time == Approx(animation->duration).margin(0.01f));
        REQUIRE(animation->frames.front().time == Approx(0.0f).margin(0.01f));

        keyframes += animation->frames.size();
        longest = std::max(longest, animation->duration);
    }

    // Pinned against an independent Python read: 474 animations, 112562
    // keyframes. A corpus that quietly shrinks cannot pass as a clean run.
    REQUIRE(animations.size() == 474);
    REQUIRE(keyframes == 112562);
    REQUIRE(longest > 1.0f);
}

TEST_CASE("retail keyframe rotations are unit quaternions") {
    const auto animations = corpus(".sca");
    if (animations.empty()) {
        SKIP("Supreme Commander animations not extracted");
    }

    for (std::size_t i = 0; i < animations.size(); i += 53) {
        const auto animation = rm::sca::loadFile(animations[i]);
        REQUIRE(animation.has_value());

        for (const rm::sca::Frame& frame : animation->frames) {
            for (const rm::sca::Key& key : frame.bones) {
                const auto& q = key.rotation;
                const float length =
                    std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                INFO(animations[i].filename().string() << " at " << frame.time << "s");
                REQUIRE(length == Approx(1.0f).margin(0.02f));
            }
        }
    }
}

TEST_CASE("a real animation drives a real model's bones") {
    const auto animations = corpus(".sca");
    if (animations.empty()) {
        SKIP("Supreme Commander animations not extracted");
    }

    // Model and animation are separate files that must agree by bone name. Find
    // a pair that does — that they exist at all is the thing worth asserting.
    std::size_t matchedPairs = 0;
    std::size_t movedSomething = 0;

    for (const std::filesystem::path& path : animations) {
        // "DEL0204_adeath.sca" belongs to "DEL0204_lod0.scm" beside it.
        const std::string stem = path.stem().string();
        const std::size_t underscore = stem.find('_');
        if (underscore == std::string::npos) {
            continue;
        }
        const std::filesystem::path modelPath =
            path.parent_path() / (stem.substr(0, underscore) + "_lod0.scm");

        std::error_code ec;
        if (!std::filesystem::is_regular_file(modelPath, ec)) {
            continue;
        }

        const auto model = rm::scm::loadFile(modelPath);
        const auto animation = rm::sca::loadFile(path);
        if (!model || !animation) {
            continue;
        }

        const auto map = rm::mapBonesToAnimation(*model, *animation);
        const auto driven = static_cast<std::size_t>(
            std::count_if(map.begin(), map.end(), [](int i) { return i >= 0; }));
        if (driven == 0) {
            continue;
        }
        ++matchedPairs;

        // A pose partway through must differ from the rest pose somewhere, or
        // the animation is being read as a table of identical frames — which is
        // exactly what a wrong stride produces.
        const auto rest = rm::restPose(*model);
        const auto posed = rm::poseAt(*model, *animation, map, animation->duration * 0.5f);
        REQUIRE(posed.size() == rest.size());

        for (std::size_t i = 0; i < posed.size(); ++i) {
            const float moved = std::abs(posed[i].translation[0] - rest[i].translation[0])
                                + std::abs(posed[i].translation[1] - rest[i].translation[1])
                                + std::abs(posed[i].translation[2] - rest[i].translation[2])
                                + std::abs(posed[i].rotation[0] - rest[i].rotation[0]);
            if (moved > 0.01f) {
                ++movedSomething;
                break;
            }
        }

        if (matchedPairs >= 40) {
            break;
        }
    }

    REQUIRE(matchedPairs > 10);
    // Not every animation moves every model — but if NONE of forty did, the
    // keys are not being read.
    REQUIRE(movedSomething > matchedPairs / 2);
}

TEST_CASE("a buffer that is not a .sca is refused by its magic") {
    const std::vector<std::byte> notAnAnimation(64, std::byte{0x7f});

    const auto animation = rm::sca::load(notAnAnimation);

    REQUIRE_FALSE(animation.has_value());
    REQUIRE(animation.error().code == rm::MapError::Code::NotScmap);
}
