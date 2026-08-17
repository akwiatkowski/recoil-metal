// Validation against the real BAR model corpus.
//
// Same reasoning as tests/test_real_map.cpp: the synthetic writer and the parser
// share one reading of the spec, so they can agree while both being wrong. These
// tests read files nobody here wrote.
//
// The corpus is FAR's reference clone of BAR, which is gitignored and
// re-fetchable, so its absence is normal and these tests SKIP rather than fail.
#include <catch2/catch_test_macros.hpp>

#include "core/model/S3o.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// BAR's model directory inside FAR's reference clone.
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

    for (const auto& entry : std::filesystem::directory_iterator{modelDir(), ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".s3o") {
            models.push_back(entry.path());
        }
    }

    // Deterministic order so a failure names the same file on every run.
    std::sort(models.begin(), models.end());
    return models;
}

} // namespace

TEST_CASE("the whole BAR model corpus parses", "[real-model]") {
    const auto models = realModels();
    if (models.empty()) {
        SKIP("no .s3o corpus at " + modelDir().string());
    }

    // Worth stating: this is 127 files at the time of writing, and the value is
    // in the breadth — hand-written fixtures cannot reproduce whatever odd
    // choices a decade of modelling tools baked into these.
    REQUIRE(models.size() >= 100);

    std::vector<std::string> failures;
    std::size_t totalBones = 0;
    std::size_t totalTriangles = 0;

    for (const auto& path : models) {
        const auto model = rm::s3o::loadFile(path);
        if (!model) {
            failures.push_back(path.filename().string() + ": " + model.error().message);
            continue;
        }
        totalBones += model->bones.size();
        totalTriangles += model->triangleCount();
    }

    // Name the offenders rather than just counting them.
    if (!failures.empty()) {
        std::string report = "models failed to parse:";
        for (const std::string& failure : failures) {
            report += "\n  " + failure;
        }
        FAIL(report);
    }

    REQUIRE(totalBones >= models.size());  // at least a root each
    REQUIRE(totalTriangles > 0);
}

TEST_CASE("every real model is internally consistent", "[real-model]") {
    const auto models = realModels();
    if (models.empty()) {
        SKIP("no .s3o corpus present");
    }

    for (const auto& path : models) {
        const auto model = rm::s3o::loadFile(path);
        REQUIRE(model.has_value());

        const std::string where = path.filename().string();

        // Index count is a whole number of triangles.
        REQUIRE(model->indices.size() % 3 == 0);

        // Every index addresses a real vertex. This is the check that would fire
        // if the per-piece index rebasing were wrong.
        const auto vertexCount = static_cast<std::uint32_t>(model->vertices.size());
        for (const std::uint32_t index : model->indices) {
            if (index >= vertexCount) {
                FAIL(where + " has index " + std::to_string(index) + " against "
                     + std::to_string(vertexCount) + " vertices");
            }
        }

        // Every vertex names a real bone.
        for (const rm::ModelVertex& vertex : model->vertices) {
            if (vertex.boneIndex >= model->bones.size()) {
                FAIL(where + " has a vertex naming bone " + std::to_string(vertex.boneIndex)
                     + " against " + std::to_string(model->bones.size()) + " bones");
            }
        }

        // The hierarchy is a tree rooted at bone 0: every parent link points
        // backwards, which is what makes a single forward pass enough to resolve
        // transforms later.
        REQUIRE(model->bones[0].parent == -1);
        for (std::size_t i = 1; i < model->bones.size(); ++i) {
            const int parent = model->bones[i].parent;
            if (parent < 0 || static_cast<std::size_t>(parent) >= i) {
                FAIL(where + " bone " + std::to_string(i) + " has parent "
                     + std::to_string(parent) + ", which is not an earlier bone");
            }
        }

        // No NaNs reached the geometry.
        for (const rm::ModelVertex& vertex : model->vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis])
                    || !std::isfinite(vertex.normal[axis])) {
                    FAIL(where + " has a non-finite position or normal");
                }
            }
        }

        // Normals are either unit length or deliberately zeroed.
        for (const rm::ModelVertex& vertex : model->vertices) {
            const float length = std::sqrt(vertex.normal[0] * vertex.normal[0]
                                         + vertex.normal[1] * vertex.normal[1]
                                         + vertex.normal[2] * vertex.normal[2]);
            if (length > 1e-4f && std::abs(length - 1.0f) > 1e-3f) {
                FAIL(where + " has a normal of length " + std::to_string(length));
            }
        }
    }
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

    // A real unit model references at least a diffuse texture.
    REQUIRE_FALSE(model->textures[0].empty());

    // And it occupies actual space.
    REQUIRE(model->radius > 0.0f);
    REQUIRE(model->boundsMax[1] > model->boundsMin[1]);
}

TEST_CASE("multi-piece models exist in the corpus and rebase correctly", "[real-model]") {
    const auto models = realModels();
    if (models.empty()) {
        SKIP("no .s3o corpus present");
    }

    // If every model were a single piece, the hierarchy and index-rebasing paths
    // would be untested by this corpus and the sweep above would prove much less
    // than it appears to.
    std::size_t multiPiece = 0;
    std::size_t deepest = 0;

    for (const auto& path : models) {
        const auto model = rm::s3o::loadFile(path);
        REQUIRE(model.has_value());

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

    REQUIRE(multiPiece > 0);
    REQUIRE(deepest > 0);
}

TEST_CASE("corpus totals match an independent parse of the same files", "[real-model]") {
    // These numbers came from a Python reader written separately from s3o.h, not
    // from this loader — the same cross-check technique used on the benchmark
    // CSVs. They agree exactly, which is much stronger evidence than the
    // relational checks above: an error in merging per-piece geometry into one
    // array would keep every relation intact while changing the totals.
    //
    // They pin the BAR clone as it stood on 2026-08-17. The corpus is a
    // gitignored, re-fetchable reference checkout, so if it is updated these
    // will need re-deriving — hence the guard on the file count, which makes a
    // corpus change look like a corpus change rather than a loader bug.
    constexpr std::size_t kFiles = 127;
    constexpr std::size_t kBones = 555;
    constexpr std::size_t kVertices = 89467;
    constexpr std::size_t kMultiPiece = 71;

    const auto models = realModels();
    if (models.empty()) {
        SKIP("no .s3o corpus present");
    }
    if (models.size() != kFiles) {
        SKIP("corpus has " + std::to_string(models.size()) + " files, not the "
             + std::to_string(kFiles) + " these totals were derived from");
    }

    std::size_t bones = 0;
    std::size_t vertices = 0;
    std::size_t multiPiece = 0;

    for (const auto& path : models) {
        const auto model = rm::s3o::loadFile(path);
        REQUIRE(model.has_value());
        bones += model->bones.size();
        vertices += model->vertices.size();
        if (model->bones.size() > 1) {
            ++multiPiece;
        }
    }

    REQUIRE(bones == kBones);
    REQUIRE(vertices == kVertices);
    REQUIRE(multiPiece == kMultiPiece);
}
