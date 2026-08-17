// Validation against the REAL retail Supreme Commander model corpus.
//
// The format was derived from these files, so they are the only thing that can
// catch a misreading. Assets are never committed (AGENT.md rule 3); they are
// extracted once from the retail install's units.scd, and these tests SKIP when
// that has not been done:
//
//   python3 -c "import zipfile,os;z=zipfile.ZipFile('.../gamedata/units.scd');\
//     [z.extract(i, os.path.expanduser('~/projects/llm/input/faf')) \
//      for i in z.infolist() if i.filename.lower().endswith(('.scm','.sca'))]"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/model/Scm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path corpusRoot() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }
    return std::filesystem::path{home} / "projects/llm/input/faf/units";
}

[[nodiscard]] std::vector<std::filesystem::path> corpus() {
    std::vector<std::filesystem::path> models;

    std::error_code ec;
    const std::filesystem::path root = corpusRoot();
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return models;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator{root, ec}) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".scm") {
            models.push_back(entry.path());
        }
    }

    std::sort(models.begin(), models.end());
    return models;
}

} // namespace

TEST_CASE("every retail .scm decodes into the shared Model") {
    const auto models = corpus();
    if (models.empty()) {
        SKIP("Supreme Commander models not extracted");
    }

    std::size_t bones = 0;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    int deepest = 0;

    for (const std::filesystem::path& path : models) {
        const auto model = rm::scm::loadFile(path);

        INFO("model: " << path.filename().string());
        REQUIRE(model.has_value());
        REQUIRE_FALSE(model->empty());
        REQUIRE(model->family == rm::Family::SupremeCommander);

        // Every index and bone reference was checked in range by the loader; a
        // model that got here cannot dangle.
        REQUIRE(model->indices.size() % 3 == 0);

        int depth = 0;
        for (const rm::ModelBone& bone : model->bones) {
            REQUIRE(bone.parent < static_cast<int>(model->bones.size()));
            REQUIRE_FALSE(bone.name.empty());

            int walk = 0;
            for (int at = bone.parent; at >= 0; at = model->bones[static_cast<std::size_t>(at)].parent) {
                ++walk;
                REQUIRE(walk < 64);  // a cycle would otherwise hang here
            }
            depth = std::max(depth, walk);
        }

        bones += model->bones.size();
        vertices += model->vertices.size();
        triangles += model->triangleCount();
        deepest = std::max(deepest, depth);
    }

    // Pinned against an independent Python read of the same corpus, the same way
    // the .s3o loader was checked: 1148 models, 8379 bones, 1226230 vertices,
    // 838471 triangles, hierarchy depth 7. A corpus that quietly shrinks cannot
    // pass as a clean run over nothing.
    REQUIRE(models.size() == 1148);
    REQUIRE(bones == 8379);
    REQUIRE(vertices == 1226230);
    REQUIRE(triangles == 838471);
    REQUIRE(deepest == 7);
}

TEST_CASE("bone rotations are unit quaternions") {
    const auto models = corpus();
    if (models.empty()) {
        SKIP("Supreme Commander models not extracted");
    }

    // The quaternion is stored w first, which is not universal. If the field
    // order were wrong the values would still be four floats — but they would
    // not consistently be unit length, and the accumulated hierarchy would drift.
    std::size_t checked = 0;
    for (std::size_t i = 0; i < models.size(); i += 37) {  // a spread sample
        const auto model = rm::scm::loadFile(models[i]);
        REQUIRE(model.has_value());

        for (const rm::ModelBone& bone : model->bones) {
            const auto& q = bone.rotation;
            const float length =
                std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            INFO("model " << models[i].filename().string() << " bone " << bone.name);
            REQUIRE(length == Approx(1.0f).margin(0.01f));
            ++checked;
        }
    }
    REQUIRE(checked > 50);
}

TEST_CASE("a .scm's vertices are in model space, not bone-local") {
    const auto models = corpus();
    if (models.empty()) {
        SKIP("Supreme Commander models not extracted");
    }

    // This is the difference that would render as a scattered model rather than
    // as an error. A bone-local model's vertices cluster around the origin per
    // bone; a model-space one's do not, so adding the bone offset would move
    // geometry that is already placed.
    const auto model = rm::scm::loadFile(models.front());
    REQUIRE(model.has_value());

    for (const rm::ModelVertex& vertex : model->vertices) {
        REQUIRE(model->modelSpacePosition(vertex) == vertex.position);
    }

    REQUIRE(model->radius > 0.0f);
    REQUIRE(model->boundsMax[1] > model->boundsMin[1]);
}

TEST_CASE("a buffer that is not a .scm is refused by its magic") {
    const std::vector<std::byte> notAModel(128, std::byte{0x42});

    const auto model = rm::scm::load(notAModel);

    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::NotScmap);
}

TEST_CASE("a header whose offsets miss their markers is refused") {
    const auto models = corpus();
    if (models.empty()) {
        SKIP("Supreme Commander models not extracted");
    }

    std::ifstream file{models.front(), std::ios::binary};
    REQUIRE(file);
    std::vector<char> data{std::istreambuf_iterator<char>{file},
                           std::istreambuf_iterator<char>{}};
    REQUIRE(data.size() > 64);

    // Shift the vertex offset by four bytes. Every length still fits the file
    // and the geometry would decode into plausible garbage — the SKEL/VTXL/TRIS
    // markers are the only thing that catches it.
    data[16] = static_cast<char>(static_cast<unsigned char>(data[16]) + 4);

    const auto model = rm::scm::load(std::as_bytes(std::span{data}));

    REQUIRE_FALSE(model.has_value());
    REQUIRE(model.error().code == rm::MapError::Code::BadHeader);
}
