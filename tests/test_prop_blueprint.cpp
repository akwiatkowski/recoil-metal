#include "core/map/PropBlueprint.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using Catch::Approx;

namespace {

/// A blueprint tree on disk, laid out the way the extracted game content is:
/// `<root>/env/<biome>/props/<name>_prop.bp` beside `<name>_lod0.scm`.
class Sandbox {
public:
    Sandbox() : root_{std::filesystem::temp_directory_path() / "rm_test_props"} {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(dir());
    }
    ~Sandbox() { std::filesystem::remove_all(root_); }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    Sandbox(Sandbox&&) = delete;
    Sandbox& operator=(Sandbox&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path dir() const { return root_ / "env" / "Test" / "Props"; }

    void write(const std::string& name, std::string_view contents) const {
        std::ofstream out{dir() / name};
        out << contents;
    }

    /// A stand-in mesh or texture. Contents are never read by the blueprint
    /// reader — it resolves paths, it does not load geometry.
    void touch(const std::string& name) const { write(name, "not really a mesh"); }

private:
    std::filesystem::path root_;
};

constexpr std::string_view kPalmish = R"BP(
PropBlueprint {
    Audio = {
        TreeFall = Sound {
            Bank = 'AmbientTest',
            Cue = 'Gen_Tree_Crush',
        },
    },
    Categories = { 'RECLAIMABLE' },
    Display = {
        Mesh = {
            LODs = {
                {
                    AlbedoName = 'Tree01_albedo.dds',
                    LODCutoff = 30,
                    ShaderName = 'NormalMappedAlpha',
                },
                {
                    AlbedoName = 'Tree01_lod1_albedo.dds',
                    LODCutoff = 200,
                    ShaderName = 'VertexNormal',
                },
            },
        },
        UniformScale = 0.04,
    },
    ScriptClass = 'Tree',
}
)BP";

} // namespace

TEST_CASE("a blueprint yields the mesh beside it, its first albedo, and its scale") {
    const Sandbox box;
    box.write("Tree01_prop.bp", kPalmish);
    box.touch("Tree01_lod0.scm");
    box.touch("Tree01_albedo.dds");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Tree01_prop.bp");
    REQUIRE(blueprint.has_value());

    CHECK(blueprint->mesh == box.dir() / "Tree01_lod0.scm");
    CHECK(blueprint->albedo == box.dir() / "Tree01_albedo.dds");

    // The whole reason this file is read. 0.04, not 1.0 — the map's own
    // per-instance scale is 1.0 for every prop in the retail corpus.
    CHECK(blueprint->uniformScale == Approx(0.04f));
}

TEST_CASE("the finest LOD's texture is the one taken") {
    // A blueprint lists its LODs coarsening outwards, each with its own albedo
    // (a distant tree gets a smaller one). Only the mesh's LOD 0 is drawn, so
    // pairing it with a later LOD's texture would put a low-resolution image on
    // full-resolution geometry — subtly soft, and nothing on screen says why.
    const Sandbox box;
    box.write("Tree01_prop.bp", kPalmish);
    box.touch("Tree01_lod0.scm");
    box.touch("Tree01_albedo.dds");
    box.touch("Tree01_lod1_albedo.dds");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Tree01_prop.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->albedo.filename() == "Tree01_albedo.dds");
}

TEST_CASE("a blueprint that states no scale is drawn at its own size") {
    const Sandbox box;
    box.write("Rock_prop.bp", R"BP(
PropBlueprint {
    Display = { Mesh = { LODs = { { ShaderName = 'VertexNormal' } } } },
}
)BP");
    box.touch("Rock_lod0.scm");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Rock_prop.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->uniformScale == Approx(1.0f));
    CHECK(blueprint->albedo.empty());
}

TEST_CASE("a blueprint with no mesh beside it is an emitter, not a failure") {
    // Eight of the 207 blueprints the stock maps name are particle emitters —
    // lava steam, blowing sand, underwater bubbles, water surface mist. They
    // parse perfectly and have no geometry, so the caller needs to tell them
    // apart from a genuinely broken read.
    const Sandbox box;
    box.write("Steam_prop.bp", R"BP(
PropBlueprint {
    Display = { Mesh = { LODs = { } } },
    ScriptClass = 'Emitter',
}
)BP");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Steam_prop.bp");
    REQUIRE_FALSE(blueprint.has_value());
    CHECK(blueprint.error().code == rm::MapError::Code::MissingMesh);
}

TEST_CASE("a blueprint that is not there, and one that is not Lua data") {
    const Sandbox box;

    const auto absent = rm::prop::loadFile(box.root(), "/env/Test/Props/Nothing_prop.bp");
    REQUIRE_FALSE(absent.has_value());

    box.write("Broken_prop.bp", "PropBlueprint { Display = someVariable }");
    box.touch("Broken_lod0.scm");
    const auto broken = rm::prop::loadFile(box.root(), "/env/Test/Props/Broken_prop.bp");
    REQUIRE_FALSE(broken.has_value());
    // Refused rather than read as a blueprint with no Display, which would draw
    // an unscaled untextured prop and look like a content problem.
    CHECK(broken.error().code == rm::MapError::Code::BadHeader);
}

TEST_CASE("a game-relative path without its leading slash still resolves") {
    // Defensive: the leading slash is what makes the path absolute in the game's
    // virtual filesystem, and joining it onto a real root resolves to "/" and
    // loses the root entirely. Callers pass what the map file said, so this
    // accepts both spellings rather than trusting every one of them to strip it.
    const Sandbox box;
    box.write("Tree01_prop.bp", kPalmish);
    box.touch("Tree01_lod0.scm");
    box.touch("Tree01_albedo.dds");

    const auto blueprint = rm::prop::loadFile(box.root(), "env/Test/Props/Tree01_prop.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->mesh == box.dir() / "Tree01_lod0.scm");
}

// --- The real corpus ---------------------------------------------------------

TEST_CASE("every blueprint the retail props ship resolves, or is an emitter") {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        SKIP("no HOME");
    }
    const std::filesystem::path root = std::filesystem::path{home} / "projects/llm/input/faf";

    std::error_code ec;
    if (!std::filesystem::is_directory(root / "env", ec)) {
        SKIP("extracted Forged Alliance env content not present");
    }

    std::size_t meshes = 0;
    std::size_t emitters = 0;
    std::size_t failures = 0;
    std::string firstFailure;
    float smallestScale = 1e9f;

    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "env", ec}) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".bp") {
            continue;
        }
        const std::string relative =
            std::filesystem::relative(entry.path(), root, ec).generic_string();

        const auto blueprint = rm::prop::loadFile(root, relative);
        if (blueprint) {
            ++meshes;
            CHECK(std::filesystem::is_regular_file(blueprint->mesh, ec));
            CHECK(blueprint->uniformScale > 0.0f);
            smallestScale = std::min(smallestScale, blueprint->uniformScale);
        } else if (blueprint.error().code == rm::MapError::Code::MissingMesh) {
            ++emitters;
        } else {
            ++failures;
            if (firstFailure.empty()) {
                firstFailure = relative + " — " + blueprint.error().message;
            }
        }
    }

    INFO("first failure: " << firstFailure);
    CHECK(failures == 0);
    // 335 blueprints ship; the ones with no mesh are the emitters.
    CHECK(meshes > 300);
    CHECK(emitters > 0);
    // Props are authored far larger than they are drawn — the reason
    // UniformScale cannot be defaulted away.
    CHECK(smallestScale < 0.1f);
}
