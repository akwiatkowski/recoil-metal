#pragma once

// The FAF opponent (ADR-039's ignition): FAF's own builder data and condition code, playing
// an army behind the same port the scripted opponent plays behind (ADR-038).
//
// WHAT DECIDES AND WHAT PLACES. The corpus decides WHAT: builder specs — registered by FAF's
// own AIBuilders/AIBaseTemplates files at load — are walked in priority order, and each one's
// BuilderConditions are evaluated by calling THE CORPUS'S OWN condition functions
// (`/lua/editor/*.lua`) against a per-tick snapshot of the sim. The adapter decides WHERE:
// a passing structure builder yields a blueprint id via FAF's BuildingTemplates, and this
// class turns it into a placed `Decision` with the same site helpers the scripted opponent
// uses. That split is deliberate: placement is geometry against OUR passability and deposits,
// which the corpus cannot know, while judgement — what to build, when, in what order — is
// exactly the thing ADR-039 says we host rather than reimplement.
//
// WHAT THIS IS NOT, yet. FAF's manager stack (EngineerManager and friends) is not running:
// the driver stands in for it with a single MAIN base and a serialized build queue. Every
// brain method a condition asks for and does not find is COUNTED and reported by the sanity
// harness — the same honesty rule as the binding stubs, applied one layer up.
//
// DETERMINISM. Decisions come from Lua, so Lua must be deterministic: builder lists are
// sorted (priority, then name), snapshots are arrays in slot order, and the interpreter's
// string-hash seed is pinned in CMakeLists — stock 5.4 seeds it from ASLR, which would make
// `pairs` order, and therefore any decision that depends on it, differ between runs.

#include "app/FafAi.hpp"
#include "app/Opponent.hpp"

#include <set>
#include <map>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace rm::ai {

class FafOpponent final : public Opponent {
public:
    /// `sandbox` outlives the opponent and is shared by every FAF opponent in the match —
    /// one VM, one corpus, per-army brains inside it (see FafAi's one-per-match note).
    FafOpponent(FafAi& sandbox, int army);

    void observe(const World& world, std::span<const rm::sim::Event> events) override;
    void advance(rm::TickIndex tick) override;
    [[nodiscard]] std::span<const Decision> drain() const override { return decisions_; }

    [[nodiscard]] int army() const noexcept { return army_; }

private:
    /// One driver decision table (top of the Lua stack) into port Decisions.
    void convertDecision(lua_State* lua);

    /// Teaches the driver a blueprint's category set, once.
    void teachType(lua_State* lua, const rm::unitdef::UnitDef& def);

    FafAi& sandbox_;
    int army_ = -1;
    bool booted_ = false;
    /// Blueprint ids whose category sets the driver has been taught. Per opponent rather
    /// than per sandbox; re-teaching an id the sandbox knows is a cheap no-op there.
    std::set<std::string> sentTypes_;
    /// Like FAF's CanPathToCurrentEnemy, cache static MAIN-to-start connectivity per enemy.
    std::map<int, std::string> enemyPaths_;
    const World* world_ = nullptr;
    std::vector<Decision> decisions_;
    /// Lua refers to a unit by its packed `UnitId` — generation in the high 32 bits, index in
    /// the low — carried as an exact 64-bit Lua integer. A handle from an earlier pass is
    /// therefore still meaningful, and resolves through the script-object seam
    /// (`UnitStore::resolve`) rather than through a per-pass table.
    [[nodiscard]] static std::int64_t packHandle(rm::sim::UnitId id) noexcept;
    [[nodiscard]] static rm::sim::UnitId unpackHandle(std::int64_t handle) noexcept;
    /// `__rm_faf_beenDestroyed(h)`: retail's `Entity:BeenDestroyed` over the live store.
    static int beenDestroyedBinding(lua_State* lua);

    /// Sites already chosen THIS pass. Several decisions convert before any of them
    /// reaches `scene.building`, so the free-site and free-deposit checks would hand every
    /// one of them the same spot — four power plants on one slot, four extractors on one
    /// deposit. Cleared at the top of every advance.
    std::vector<std::array<rm::sim::Fx, 3>> plannedThisPass_;
};

/// Installs the driver chunk into the sandbox. Idempotent; false with `lastError` set when
/// the chunk itself fails, which is a bug here rather than in the corpus.
[[nodiscard]] bool installFafDriver(FafAi& ai);

/// Brain methods conditions asked for and did not find, worst first, as "name xN" lines —
/// the sanity report's fourth list, and the work queue for the next binding pass.
[[nodiscard]] std::vector<std::string> fafMissingBrainMethods(FafAi& ai);

/// Distinct errors raised inside condition functions, as "error xN" lines. A condition that
/// errors fails closed, so these are silent behaviour changes — worth naming.
[[nodiscard]] std::vector<std::string> fafConditionErrors(FafAi& ai);

/// Formats the complete condition-error section. Unlike the ranked summary sections, this
/// list is the repair queue: omitting its tail hides independently broken conditions.
[[nodiscard]] std::string formatFafConditionErrorReport(std::span<const std::string> errors);

} // namespace rm::ai
