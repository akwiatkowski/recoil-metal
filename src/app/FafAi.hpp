#pragma once

// The FAF AI sandbox: a Lua 5.1 VM with the engine surface FAF's AI expects (ADR-039).
//
// WHAT THIS IS FOR. ADR-039 commits to hosting FAF's own AI rather than reimplementing it, and
// states the premise plainly: we do not fully know how Moho works and are not going to find out
// before writing the adapter. So the goal here is not an AI that plays well. It is an AI that
// RUNS, plus a report of what it is running badly — which is the only way "analyse later which
// parts are not connected" becomes an answer rather than an investigation.
//
// EVERY NAME IS BOUND, and that is the load-time guarantee worth having. A missing binding in
// Lua is a nil-call forty minutes into a match; a bound stub is a counted, named no-op. So all
// 254 names from `FafApi.inc` are installed before a line of AI Lua runs, each carrying a
// confidence tag:
//
//   Known    — implemented against this engine, believed correct.
//   Guessed  — implemented from the annotation's signature and how the corpus uses it. Moho is
//              closed; this is inference, and it is labelled so nobody later reads it as fact.
//   Stub     — returns nil (or a neutral value) and counts the call. Not implemented.
//
// A Stub with four thousand calls is the next thing to implement. A Known with zero calls is
// dead surface. That ranking IS the plan for the next pass, and it costs a counter per name.
//
// WHY THE VM IS NOT A DEPENDENCY OF THE SIM. Nothing in `core/` can reach this: the sandbox is
// reachable only through `rm::ai::Opponent`, so an AI can be wrong without the simulation being
// wrong. That asymmetry is what makes hosting a foreign AI reasonable where hosting the foreign
// SIM was not (PLAN.md's Moho analysis).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace rm::ai {

/// How much we believe a binding.
enum class Fidelity : std::uint8_t {
    Stub,     ///< counted no-op
    Guessed,  ///< inferred from the annotation and the corpus's use of it
    Known,    ///< implemented against this engine
};

[[nodiscard]] std::string_view fidelityName(Fidelity fidelity) noexcept;

/// What became of one attempted module load.
///
/// WHY THIS IS NOT A BOOL. `import` is deliberately forgiving — the corpus imports across the
/// whole game while this vendors only the AI subset, so most misses are files we chose not to
/// fetch — and a forgiving import that returns an empty table makes a FAILED file look exactly
/// like a loaded one. That is the silent-success ADR-039 exists to avoid, so the outcome is
/// recorded per module and the distinction between "not there" and "there and broken" survives.
enum class LoadOutcome : std::uint8_t {
    Executed,  ///< parsed and ran to completion
    Missing,   ///< no such file under the vendored subset
    Failed,    ///< found, but parsing or running it raised
};

struct ModuleLoad {
    std::string path;
    LoadOutcome outcome = LoadOutcome::Missing;
    /// The Lua error, when `outcome` is Failed.
    std::string error;
};

/// One engine name the AI can call, and what happened to it.
struct Binding {
    std::string name;
    Fidelity fidelity = Fidelity::Stub;
    /// Call sites in the corpus — the static count, from the generator.
    int sites = 0;
    /// Calls actually made this run. The number that ranks the work.
    std::size_t calls = 0;
};

/// A Lua 5.1 VM carrying the engine surface FAF's AI expects.
///
/// One per match, not one per army: the corpus is 83,006 lines and `import` caches modules, so
/// a VM per opponent would parse it all again for each. Army identity travels with the brain
/// object rather than with the interpreter.
class FafAi {
public:
    /// `root` is the vendored corpus — `vendor/ai/faf`, where `make ai` puts it.
    ///
    /// `verbose` traces every module load to stdout as it happens. On by default for the
    /// in-game AI (`--ai faf`), because the whole point of this pass is that a failure is
    /// pasteable: a trace that stops mid-line names the file that hung, which a summary printed
    /// at the end never can.
    explicit FafAi(std::filesystem::path root, bool verbose = false);
    ~FafAi();

    FafAi(const FafAi&) = delete;
    FafAi& operator=(const FafAi&) = delete;
    FafAi(FafAi&&) = delete;
    FafAi& operator=(FafAi&&) = delete;

    /// Whether the VM came up and the surface installed.
    [[nodiscard]] bool ready() const noexcept { return state_ != nullptr; }

    /// Loads one corpus file by its FAF-relative path (`/lua/AI/aiutilities.lua`), running it
    /// and caching what it returns — the same contract `import` gives the AI itself.
    ///
    /// Returns whether it loaded. The error, if any, is in `lastError`.
    [[nodiscard]] bool import(std::string_view path);

    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    /// Resumes every forked thread that is due at `tick` — Moho's scheduler beat, one call
    /// per sim tick. Returns how many resumed. A thread that errors records its message
    /// (`threadErrors`) and dies alone; the rest keep running, which is what lets one broken
    /// platoon behaviour not take the brain with it.
    std::size_t pump(long long tick);

    /// Threads forked and not yet finished, killed or crashed.
    [[nodiscard]] std::size_t threadsAlive() const;

    /// Every error a thread has died with, in order. The runtime half of `modules()`.
    [[nodiscard]] std::vector<std::string> threadErrors() const;

    /// Runs a chunk in the sandbox's globals — the adapter's own hatch, for tests and for
    /// the opponent bridge to come. False on error, with `lastError` set.
    [[nodiscard]] bool eval(std::string_view chunk);

    /// When on, the corpus's LOG/WARN/SPEW lines print (prefixed "FAF:") instead of being
    /// counted silently — the corpus explains itself well when allowed to speak.
    void setLogPassthrough(bool enabled);

    /// Resets the instruction budget. `eval`, `import` and `pump` refuel themselves; a
    /// caller driving the VM through `state()` directly (the opponent bridge) must refuel
    /// per entry, or the watchdog bills its passes cumulatively and kills a legitimate one.
    void refuel();

    /// Turns the call profiler on or off: which of the CORPUS'S OWN functions ran, counted
    /// per definition site. The binding report's other half — that one counts calls INTO
    /// the engine, this one counts the AI's code running. Costs a hook check per call, so
    /// it is off unless a sanity run asks.
    void setProfiling(bool enabled);

    /// The profile, most-called first: "file:line (name)" against its call count.
    [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> callProfile() const;

    /// Every module load attempted, in the order attempted — including the ones `import` was
    /// forgiving about, which is the point.
    [[nodiscard]] std::vector<ModuleLoad> modules() const;

    /// Every binding and what it did, ordered by calls made then by corpus call sites — so the
    /// front of the list is the work queue.
    [[nodiscard]] std::vector<Binding> report() const;

    /// How many distinct names are installed. The load-time assertion ADR-039 asks for: if this
    /// is not the generator's total, something failed to register and the AI would have hit a
    /// nil mid-match instead of a counted stub.
    [[nodiscard]] std::size_t boundCount() const noexcept;

    /// The raw state, for the adapter that drives it. Null until `ready()`.
    [[nodiscard]] lua_State* state() const noexcept { return state_; }

private:
    std::filesystem::path root_;
    bool verbose_ = false;
    lua_State* state_ = nullptr;
    std::string lastError_;
};

/// Boots the sandbox, loads the AI corpus, and prints what happened — the `--ai-debug` report.
///
/// Written to be PASTED. Every line is self-describing, failures carry their Lua error and the
/// file that raised it, and the module trace is flushed as it goes so a hang leaves the name of
/// the file it hung on rather than nothing at all.
///
/// Finds the corpus by walking up from the working directory, so it works from the repo root and
/// from `build/` alike. Says so and returns if `make ai` has not been run.
void reportFafSandbox();

/// Where the vendored corpus lives for this checkout, or empty — the same search
/// `reportFafSandbox` uses, exposed so the sanity run can boot its own sandbox.
[[nodiscard]] std::filesystem::path defaultCorpus();

/// Imports the brain's entry-point modules — the same list `reportFafSandbox` loads, exposed
/// so the sanity run measures the WHOLE corpus rather than the eight bootstrap files. Turn
/// the profiler on first: most of what runs today runs at load time, and a sanity report
/// that misses it says "nothing ran" about ninety-five modules of running code.
void importAiEntryPoints(FafAi& ai);

} // namespace rm::ai
