// Validation against the real BAR model corpus.
//
// Same reasoning as tests/test_real_map.cpp: the synthetic writer and the parser
// share one reading of the spec, so they can agree while both being wrong. These
// tests read files nobody here wrote.
//
// The corpus is FAR's reference clone of BAR, gitignored and re-fetchable, so its
// absence is normal and these tests SKIP rather than fail.
//
// NOTE ON SCOPE, because the first version of this file got it wrong: the sweep
// recurses. BAR keeps its debug spheres and PBR test cubes in the top level of
// objects3d/ and its actual units in subdirectories — objects3d/Units alone has
// 1365 files. Reading only the top level covered 127 of 2034 models and reported
// a maximum hierarchy depth of 2 where the real corpus reaches 17.
//
// The whole corpus is swept ONCE, in a single test, because parsing 2034 models
// is 5.3M vertices and repeating it per-assertion would dominate the suite.
#include <catch2/catch_test_macros.hpp>

#include "core/model/S3o.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const char* kRelativeModelDir =
    "projects/llm/games/forged-alliance-reborn/reference/BAR/objects3d";

[[nodiscard]] std::filesystem::path modelDir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::filesystem::path{home} / kRelativeModelDir;
}

[[nodiscard]] std::vector<std::filesystem::path> realModels() {
    std::vector<std::filesystem::path> models;

    std::error_code ec;
    if (!std::filesystem::is_directory(modelDir(), ec)) {
        return models;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator{modelDir(), ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".s3o") {
            models.push_back(entry.path());
        }
    }

    // Deterministic order so a failure names the same file on every run.
    std::sort(models.begin(), models.end());
    return models;
}

/// Everything the sweep checks about one model. Returns a description on
/// failure so the caller can name the file.
[[nodiscard]] std::string checkModel(const rm::Model& model) {
    // Index count is a whole number of triangles.
    if (model.indices.size() % 3 != 0) {
        return "index count " + std::to_string(model.indices.size()) + " is not a multiple of 3";
    }

    // Every index addresses a real vertex. This is what fires if the per-piece
    // index rebasing is wrong.
    const auto vertexCount = static_cast<std::uint32_t>(model.vertices.size());
    for (const std::uint32_t index : model.indices) {
        if (index >= vertexCount) {
            return "index " + std::to_string(index) + " against "
                 + std::to_string(vertexCount) + " vertices";
        }
    }

    if (model.bones.empty()) {
        return "no bones";
    }

    // The hierarchy is a tree rooted at bone 0 with every parent link pointing
    // backwards, which is what makes one forward pass enough to resolve
    // transforms later.
    if (model.bones[0].parent != -1) {
        return "root bone has parent " + std::to_string(model.bones[0].parent);
    }
    for (std::size_t i = 1; i < model.bones.size(); ++i) {
        const int parent = model.bones[i].parent;
        if (parent < 0 || static_cast<std::size_t>(parent) >= i) {
            return "bone " + std::to_string(i) + " has parent " + std::to_string(parent)
                 + ", which is not an earlier bone";
        }
    }

    for (const rm::ModelVertex& vertex : model.vertices) {
        if (vertex.boneIndex >= model.bones.size()) {
            return "vertex names bone " + std::to_string(vertex.boneIndex) + " against "
                 + std::to_string(model.bones.size()) + " bones";
        }

        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis])) {
                return "non-finite position or normal";
            }
        }

        // Normals are either unit length or deliberately zeroed.
        const float length = std::sqrt(vertex.normal[0] * vertex.normal[0]
                                     + vertex.normal[1] * vertex.normal[1]
                                     + vertex.normal[2] * vertex.normal[2]);
        if (length > 1e-4f && std::abs(length - 1.0f) > 1e-3f) {
            return "normal of length " + std::to_string(length);
        }
    }

    return {};
}

} // namespace

TEST_CASE("the whole BAR model corpus parses and is internally consistent", "[real-model]") {
    const auto models = realModels();
    if (models.empty()) {
        SKIP("no .s3o corpus at " + modelDir().string());
    }

    // Totals cross-checked against a Python reader written separately from
    // s3o.h — the same technique used on the benchmark CSVs. They agree
    // exactly, which is stronger evidence than the invariants below: an error
    // merging per-piece geometry into one array would keep every invariant
    // intact while changing the totals.
    //
    // Pinned as of 2026-08-17. The corpus is a re-fetchable reference checkout,
    // so the file-count guard makes an upstream change read as a corpus change
    // rather than a loader bug.
    constexpr std::size_t kFiles = 2034;
    constexpr std::size_t kBones = 16399;
    constexpr std::size_t kVertices = 5283604;
    constexpr std::size_t kTriangles = 3517557;
    constexpr std::size_t kMultiPiece = 1104;
    constexpr std::size_t kDeepestHierarchy = 17;

    std::vector<std::string> failures;
    std::size_t bones = 0;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::size_t multiPiece = 0;
    std::size_t deepest = 0;

    for (const auto& path : models) {
        const auto model = rm::s3o::loadFile(path);
        if (!model) {
            failures.push_back(path.filename().string() + ": " + model.error().message);
            continue;
        }

        if (const std::string problem = checkModel(*model); !problem.empty()) {
            failures.push_back(path.filename().string() + ": " + problem);
            continue;
        }

        bones += model->bones.size();
        vertices += model->vertices.size();
        triangles += model->triangleCount();
        if (model->bones.size() > 1) {
            ++multiPiece;
        }

        for (std::size_t i = 0; i < model->bones.size(); ++i) {
            std::size_t depth = 0;
            for (int at = model->bones[i].parent; at >= 0;
                 at = model->bones[static_cast<std::size_t>(at)].parent) {
                ++depth;
            }
            deepest = std::max(deepest, depth);
        }
    }

    // Name the offenders rather than just counting them.
    if (!failures.empty()) {
        std::string report = std::to_string(failures.size()) + " of "
                           + std::to_string(models.size()) + " models failed:";
        for (std::size_t i = 0; i < failures.size() && i < 20; ++i) {
            report += "\n  " + failures[i];
        }
        FAIL(report);
    }

    if (models.size() != kFiles) {
        WARN("corpus has " + std::to_string(models.size()) + " models, not the "
             + std::to_string(kFiles) + " the pinned totals were derived from; "
             + "totals not checked");
        return;
    }

    REQUIRE(bones == kBones);
    REQUIRE(vertices == kVertices);
    REQUIRE(triangles == kTriangles);
    REQUIRE(multiPiece == kMultiPiece);

    // Real content reaches 17 levels deep, so the recursive walk is genuinely
    // exercised rather than only by synthetic fixtures.
    REQUIRE(deepest == kDeepestHierarchy);
}

TEST_CASE("a known real model has the shape its file says", "[real-model]") {
    // One model pinned by name so a regression names something concrete rather
    // than "the corpus changed".
    const std::filesystem::path path = modelDir() / "cormissile.s3o";

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        SKIP("cormissile.s3o not present");
    }

    const auto model = rm::s3o::loadFile(path);
    REQUIRE(model.has_value());

    REQUIRE(model->name == "cormissile");
    REQUIRE_FALSE(model->bones.empty());
    REQUIRE_FALSE(model->empty());

    // Real unit models reference a texture pair; S3O carries the names only.
    REQUIRE(model->textures[0] == "cor_color.dds");
    REQUIRE(model->textures[1] == "cor_other.dds");

    REQUIRE(model->radius > 0.0f);
    REQUIRE(model->boundsMax[1] > model->boundsMin[1]);
}
