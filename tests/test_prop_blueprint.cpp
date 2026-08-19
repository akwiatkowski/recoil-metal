#include "core/map/PropBlueprint.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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

    REQUIRE(blueprint->lods.size() == 1);  // only lod0 exists on disk here
    CHECK(blueprint->lods[0].mesh == box.dir() / "Tree01_lod0.scm");
    CHECK(blueprint->lods[0].albedo == box.dir() / "Tree01_albedo.dds");

    // The whole reason this file is read. 0.04, not 1.0 — the map's own
    // per-instance scale is 1.0 for every prop in the retail corpus.
    CHECK(blueprint->uniformScale == Approx(0.04f));
}

TEST_CASE("each level keeps its own mesh, texture and cutoff") {
    // A blueprint lists its LODs coarsening outwards, each with its own albedo — a
    // distant tree gets a smaller one. Pairing a level's geometry with another
    // level's texture is invisible either way round: soft where it should be sharp,
    // or sharp on geometry that no longer deserves it.
    const Sandbox box;
    box.write("Tree01_prop.bp", kPalmish);
    box.touch("Tree01_lod0.scm");
    box.touch("Tree01_lod1.scm");
    box.touch("Tree01_albedo.dds");
    box.touch("Tree01_lod1_albedo.dds");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Tree01_prop.bp");
    REQUIRE(blueprint.has_value());
    REQUIRE(blueprint->lods.size() == 2);

    CHECK(blueprint->lods[0].mesh.filename() == "Tree01_lod0.scm");
    CHECK(blueprint->lods[0].albedo.filename() == "Tree01_albedo.dds");
    CHECK(blueprint->lods[0].cutoffElmos == Approx(30.0f * 8.0f));

    CHECK(blueprint->lods[1].mesh.filename() == "Tree01_lod1.scm");
    CHECK(blueprint->lods[1].albedo.filename() == "Tree01_lod1_albedo.dds");
    CHECK(blueprint->lods[1].cutoffElmos == Approx(200.0f * 8.0f));

    // And the coarsest cutoff is the distance past which the prop is not drawn.
    CHECK(blueprint->drawDistanceElmos() == Approx(200.0f * 8.0f));
}

TEST_CASE("a level whose mesh is absent ends the list rather than failing the prop") {
    // The table describes three levels; only two are on disk. A prop with a fine
    // level and no coarse one is still a prop.
    const Sandbox box;
    box.write("Tree01_prop.bp", kPalmish);
    box.touch("Tree01_lod0.scm");
    // no Tree01_lod1.scm

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Tree01_prop.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->lods.size() == 1);
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
    REQUIRE(blueprint->lods.size() == 1);
    CHECK(blueprint->lods[0].albedo.empty());
    // No cutoff stated means no limit, not a limit of zero.
    CHECK(blueprint->lods[0].cutoffElmos == std::numeric_limits<float>::infinity());
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
    REQUIRE_FALSE(blueprint->lods.empty());
    CHECK(blueprint->lods[0].mesh == box.dir() / "Tree01_lod0.scm");
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
    std::size_t markers = 0;
    std::size_t levels = 0;
    std::size_t multiLevel = 0;
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
        if (blueprint && blueprint->effect != rm::prop::Effect::None) {
            // A blueprint that draws nothing and marks an ambient effect instead.
            ++markers;
            CHECK(blueprint->lods.empty());
            CHECK(blueprint->uniformScale == 0.0f);
        } else if (blueprint) {
            ++meshes;
            REQUIRE_FALSE(blueprint->lods.empty());
            levels += blueprint->lods.size();
            if (blueprint->lods.size() > 1) {
                ++multiLevel;
            }
            float previousCutoff = 0.0f;
            for (const rm::prop::BlueprintLod& lod : blueprint->lods) {
                CHECK(std::filesystem::is_regular_file(lod.mesh, ec));
                // Cutoffs increase outwards, which is what makes choosing a level
                // for a distance a walk from the finest rather than a search.
                CHECK(lod.cutoffElmos >= previousCutoff);
                previousCutoff = lod.cutoffElmos;
            }
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
    // The effect markers: lava steam, water mist, underwater bubbles, blowing sand
    // and blowing snow. Recognised rather than reported as broken content.
    INFO(markers << " effect markers, " << emitters << " unrecognised");
    CHECK(markers >= 8);
    // Props are authored far larger than they are drawn — the reason
    // UniformScale cannot be defaulted away.
    CHECK(smallestScale < 0.1f);

    // Most props have one level; the ones that matter have three. Every
    // heavily-placed tree in the corpus does, which is where the saving is, because
    // those are the blueprints placed ten thousand times over.
    INFO(levels << " levels across " << meshes << " blueprints, " << multiLevel
                << " with more than one");
    CHECK(multiLevel > 80);
    CHECK(levels > meshes);
}


// --- Effect markers ----------------------------------------------------------

TEST_CASE("a blueprint that draws nothing marks an ambient effect") {
    // Eight of the blueprints the stock maps reference draw no geometry at all:
    // UniformScale is 0 and MeshName points at an editor marker. What they mark is a
    // particle effect, and the blueprint names WHICH in prose and in its own file
    // name — and says nothing whatever about how it should look, because that lives
    // in Lua this project does not run.
    const Sandbox box;
    box.write("LavaSteam01_prop.bp", R"BP(
PropBlueprint {
    Display = {
        Mesh = {
            LODs = {
                {
                    AlbedoName = '/env/common/props/marker01_albedo.dds',
                    MeshName = '/env/common/props/marker01_lod0.scm',
                    ShaderName = 'TMeshNoNormals',
                },
            },
        },
        UniformScale = 0,
    },
    Interface = { HelpText = 'Small lava steam steam' },
}
)BP");

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/LavaSteam01_prop.bp");
    REQUIRE(blueprint.has_value());
    CHECK(blueprint->effect == rm::prop::Effect::Steam);
    // Exactly one of the two is meaningful: this one is a place, not a thing.
    CHECK(blueprint->lods.empty());
}

TEST_CASE("each kind of marker is told apart") {
    const Sandbox box;
    const auto markerNamed = [&box](const std::string& name) {
        box.write(name + "_prop.bp",
                  "PropBlueprint { Display = { Mesh = { LODs = { { ShaderName = '' } } }, "
                  "UniformScale = 0 } }");
        return rm::prop::loadFile(box.root(), "/env/Test/Props/" + name + "_prop.bp");
    };

    CHECK(markerNamed("LavaSteam03")->effect == rm::prop::Effect::Steam);
    CHECK(markerNamed("WaterSurfaceMist01")->effect == rm::prop::Effect::Mist);
    CHECK(markerNamed("UnderwaterBubbles01")->effect == rm::prop::Effect::Bubbles);
    CHECK(markerNamed("DesertBlowingSand02")->effect == rm::prop::Effect::BlowingSand);
    CHECK(markerNamed("BlowingSnow01")->effect == rm::prop::Effect::BlowingSnow);
}

TEST_CASE("a marker whose effect nobody recognises is reported, not invented") {
    // A scale of zero says "draws nothing"; the name says which effect. Something
    // that says the first and not the second is content this reader does not
    // understand, and guessing an effect for it would put steam wherever a future
    // marker happens to be placed.
    const Sandbox box;
    box.write("SomethingElse_prop.bp",
              "PropBlueprint { Display = { Mesh = { LODs = { { ShaderName = '' } } }, "
              "UniformScale = 0 } }");

    const auto blueprint =
        rm::prop::loadFile(box.root(), "/env/Test/Props/SomethingElse_prop.bp");
    REQUIRE_FALSE(blueprint.has_value());
    CHECK(blueprint.error().code == rm::MapError::Code::MissingMesh);
}

TEST_CASE("MeshName names the mesh when the blueprint states one") {
    // The format's own way, against the convention it otherwise falls back to.
    // Twelve of the shipped blueprints use it.
    const Sandbox box;
    box.write("Odd_prop.bp", R"BP(
PropBlueprint {
    Display = {
        Mesh = { LODs = { { MeshName = '/env/Test/Props/Elsewhere_lod0.scm' } } },
        UniformScale = 1,
    },
}
)BP");
    box.touch("Elsewhere_lod0.scm");
    // ...and deliberately NOT Odd_lod0.scm, so only MeshName can find it.

    const auto blueprint = rm::prop::loadFile(box.root(), "/env/Test/Props/Odd_prop.bp");
    REQUIRE(blueprint.has_value());
    REQUIRE(blueprint->lods.size() == 1);
    CHECK(blueprint->lods[0].mesh.filename() == "Elsewhere_lod0.scm");
}
