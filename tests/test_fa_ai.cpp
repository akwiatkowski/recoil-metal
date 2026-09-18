// Player-perspective coverage for ai claims (see docs/fa-exe-analysis-plan.md):
// WP-38's observable contract — what the hosted FAF brain may know (C-355, C-357)
// and the rules it plays by (C-360).
//
// The claim texts enumerate retail's native object model (CInfluenceMap cells,
// CPlatoon squads, per-brain CTaskStages) that this adapter deliberately does not
// replicate — see build/re-fa/coverage/FA-AI.md. What a player can observe is the
// boundary: the AI's threat answers come from what its army can SEE, not from the
// ground truth, and the AIx cheat buffs are off.
#include <catch2/catch_test_macros.hpp>

#include "app/FafAi.hpp"
#include "app/FafOpponent.hpp"
#include "app/SceneBuild.hpp"
#include "core/sim/Intel.hpp"
#include "core/unit/UnitDef.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using rm::ai::FafAi;
using rm::ai::FafOpponent;

namespace {

[[nodiscard]] std::filesystem::path corpusRoot() {
    // The tests run from the build directory; the corpus sits beside the source tree.
    for (const char* candidate : {"vendor/ai/faf", "../vendor/ai/faf", "../../vendor/ai/faf"}) {
        if (std::filesystem::is_directory(candidate)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return {};
}

/// A tank with an authored threat estimate and, optionally, eyes. The threat
/// numbers are the corpus's own scale (a T1 tank is 1, an ACU 60 — see
/// `UnitDef`'s `Defense.*ThreatLevel` note).
[[nodiscard]] rm::unitdef::UnitDef tankDef(std::string name, float surfaceThreat,
                                           float visionElmos = 0.0f) {
    rm::unitdef::UnitDef def;
    def.name = std::move(name);
    def.categories = {"DIRECTFIRE", "LAND", "MOBILE", "TANK"};
    def.health = rm::sim::magFromFloat(100.0f);
    def.speedElmosPerSecond = 30.0f;
    def.collisionRadiusElmos = 2.0f;
    def.surfaceThreat = surfaceThreat;
    def.visionRadiusElmos = visionElmos;
    return def;
}

/// A two-army scene: army 0 is the FAF brain's, army 1 the enemy's. `enemyAt`
/// places the hostile; `ownVision` is how far army 0's spotter sees.
struct AiScene {
    rm::app::UnitScene scene;
    rm::HeightField field;
    std::vector<rm::mapinfo::StartPosition> starts;
    rm::vfs::Vfs content;

    AiScene() {
        scene.armies = {{.index = 0, .alliance = 0}, {.index = 1, .alliance = 1}};
        scene.economies.resize(2);
        field.squaresX = 64;
        field.squaresZ = 64;
        field.heightScale = 1.0f;
        field.raw.assign(field.sampleCount(), std::uint16_t{0});
        starts = {{.x = 0, .z = 100}, {.x = 400, .z = 100}};
    }

    rm::sim::UnitId spawn(const rm::unitdef::UnitDef& def, float x, float z, int army) {
        scene.definitions.push_back(def);
        const auto type = scene.catalog.add(&scene.definitions.back(), rm::sim::TickRate{});
        return scene.store.spawn({
            .type = type,
            .transform = {.x = rm::sim::fxFromFloat(x), .z = rm::sim::fxFromFloat(z)},
            .motion = {.armyIndex = army},
            .health = {.current = rm::sim::magFromFloat(100.0f),
                       .maximum = rm::sim::magFromFloat(100.0f)},
        });
    }

    /// Fog of war sized to the map, then stamped from the units' own senses.
    void see() {
        scene.intel.configure(2, rm::sim::fxFromFloat(field.widthElmos()),
                              rm::sim::fxFromFloat(field.depthElmos()),
                              rm::sim::VisionStyle::ForgedAlliance);
        scene.intel.update(scene.store, scene.catalog, scene.armies, nullptr,
                           rm::sim::TickRate{});
    }

    [[nodiscard]] rm::ai::World world() {
        return {.scene = scene, .content = content, .field = field,
                .starts = starts, .markers = {}};
    }
};

} // namespace

TEST_CASE("C-355/C-357: the brain's threat answers come from what it can see",
          "[fa-ai][threat]") {
    const auto root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    AiScene job;
    // The enemy tank is worth 20 surface threat and sits 300 elmos from the brain's
    // spotter, which sees 100. Until the spotter closes, the honest answer at the
    // tank's position is ZERO — the influence map retail keeps is a record of
    // contacts, and our observed-enemy snapshot is the same record (C-355's
    // per-army threat, answered from vision rather than a cell grid).
    (void)job.spawn(tankDef("spotter", 0.0f, 100.0f), 100, 100, 0);
    (void)job.spawn(tankDef("hostile", 20.0f), 400, 100, 1);
    job.see();

    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    FafOpponent opponent(ai, 0);
    const auto world = job.world();
    opponent.observe(world, {});
    opponent.advance(0);

    bool ok = ai.eval(R"(
        local brain = __rm_faf.brains[0]
        assert(brain:GetThreatAtPosition({400, 0, 100}, 0, true, 'AntiSurface') == 0,
            'an unobserved enemy contributes no threat')
        assert(#brain:GetThreatsAroundPosition({400, 0, 100}, 40, true, 'AntiSurface') == 0,
            'and lists no rows')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);

    // The spotter walks into vision range; the next snapshot sees the tank, and the
    // same queries now answer its authored threat — the {x, z, threat} triple
    // C-357's Around variant returns.
    job.scene.store.transforms()[1].x = rm::sim::fxFromFloat(180.0f);
    job.see();
    opponent.advance(10);
    ok = ai.eval(R"(
        local brain = __rm_faf.brains[0]
        assert(brain:GetThreatAtPosition({180, 0, 100}, 0, true, 'AntiSurface') == 20,
            'a spotted enemy contributes its blueprint threat')
        local rows = brain:GetThreatsAroundPosition({180, 0, 100}, 40, true, 'AntiSurface')
        assert(#rows == 1 and rows[1][1] == 180 and rows[1][2] == 100 and rows[1][3] == 20,
            'one {x, z, threat} row for the one visible enemy')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}

TEST_CASE("C-360: the hosted brain boots with AIx cheats off",
          "[fa-ai][cheats]") {
    const auto root = corpusRoot();
    if (root.empty()) {
        SKIP("no vendored corpus; run `make ai`");
    }
    // C-360's cheats are Lua buffs (CheatIncome/CheatBuildRate ×2, IntelCheat
    // vision) applied when the personality name says 'cheat'. The adapter plays a
    // fair opponent: the flag the corpus's SetupCheat reads is pinned false, so no
    // buff path can arm itself.
    AiScene job;
    (void)job.spawn(tankDef("spotter", 0.0f, 100.0f), 100, 100, 0);
    job.see();

    FafAi ai(root);
    REQUIRE(installFafDriver(ai));
    importAiEntryPoints(ai);
    FafOpponent opponent(ai, 0);
    const auto world = job.world();
    opponent.observe(world, {});
    opponent.advance(0);

    const bool ok = ai.eval(R"(
        local brain = __rm_faf.brains[0]
        assert(brain.CheatEnabled == false, 'the adapter plays fair')
    )");
    INFO(ai.lastError());
    REQUIRE(ok);
}
