#include "app/FafAi.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>

namespace rm::ai {
namespace {

/// The registry key under which the binding table lives, so a C function can find its own
/// counter without a global. A pointer to a static is the idiomatic Lua 5.1 unique key.
char kBindingsKey = 0;

/// One binding's mutable state, kept C++-side. Lua holds an index into this rather than the
/// string, so counting a call is an array bump.
struct Slot {
    std::string name;
    Fidelity fidelity = Fidelity::Stub;
    int sites = 0;
    std::size_t calls = 0;
};

struct Sandbox;
[[nodiscard]] Sandbox* sandboxOf(lua_State* lua);

/// One forked thread: the coroutine (held alive by a registry ref), its handle table (what
/// the AI stores in TrashBags and passes to KillThread), and when it next wants to run.
struct Thread {
    int coroutine = LUA_NOREF;
    int handle = LUA_NOREF;
    long long wake = 0;
    int firstArgs = -1;  ///< args to pass on the FIRST resume; -1 after it has run once
    bool dead = false;
};

struct Sandbox {
    std::vector<Slot> slots;
    std::filesystem::path root;
    /// Cheap re-entrancy guard: `import` runs Lua that may import again, and a cycle in the
    /// corpus would otherwise recurse until the C stack gave out.
    int importDepth = 0;
    /// Every load attempted, so a forgiving import still leaves a trail.
    std::vector<ModuleLoad> modules;
    bool verbose = false;
    /// Instructions the current chunk may still execute before the watchdog stops it.
    long long fuel = 0;
    /// How many times the watchdog has fired since the last refill. See `fuelHook`.
    int overruns = 0;

    /// The scheduler: Moho's thread model, on coroutines. `ForkThread` files one here,
    /// `pump` resumes what is due, `WaitSeconds`/`WaitTicks` yield a wake delay. Errors are
    /// recorded, not fatal — one broken platoon thread must not stop the brain.
    ///
    /// A DEQUE, and the choice is load-bearing twice. (1) `pump` holds a `Thread&` across
    /// `lua_resume`, and the resumed corpus code may FORK — a vector's push_back would
    /// reallocate out from under that reference; deque promises the existing elements never
    /// move. (2) Slots are never erased, because a handle's `__rm_thread` field carries a
    /// slot index: compacting would silently re-point every older handle at the wrong
    /// thread. Dead slots are reclaimed at the registry level by pump instead.
    std::deque<Thread> threads;
    std::vector<std::string> threadErrors;
    long long tick = 0;              ///< the pump's clock, in sim ticks
    std::size_t currentThread = SIZE_MAX;  ///< index being resumed, for CurrentThread

    /// The call profile: which of the CORPUS'S OWN functions ran, and how often — the other
    /// half of the binding report, which only counts calls INTO the engine. Keyed by
    /// file:line and name, filled by the hook when profiling is on. A map because the sanity
    /// report sorts it once at the end; the per-call cost is one lookup.
    bool profiling = false;
    std::map<std::string, std::size_t> profile;

    /// Print LOG/WARN/SPEW instead of only counting them (`--ai-log`).
    bool logPassthrough = false;
};

/// The adapter's tick rate assumption, matching the app's default. WaitSeconds converts
/// through this at yield time; if the app ever runs the sandbox at another rate, the pump's
/// caller owns the conversion instead.
constexpr long long kTicksPerSecond = 10;

/// THE WATCHDOG, and it is not optional when hosting someone else's code.
///
/// Before the `continue` transform landed, files using it failed to PARSE, so nothing in them
/// ran. Once they parsed, their top level ran — and a module that spins waiting on a stubbed
/// `WaitSeconds` never yields, so the test hung instead of failing. A hang reports nothing; a
/// budget reports which file and how far it got.
///
/// Counted in VM instructions rather than wall time so the answer is the same on every machine
/// and in every build — a timeout would make the report depend on how busy the laptop was.
constexpr long long kInstructionBudget = 20'000'000;

/// How many exhausted budgets one chunk or decision pass may swallow before the watchdog
/// stops refilling. The corpus wraps its condition calls in `pcall`, so a single runaway
/// condition is caught and the pass carries on — which is right, and the budget must be
/// restored for the work after it or every subsequent thousand instructions raise again
/// (that cascade is what turned one slow condition into a dozen "failures" in the SCMP_009
/// duel logs). Four keeps a pathological `while true do pcall(...) end` bounded at five
/// budgets, about a hundred million instructions, rather than forever.
constexpr int kMaxOverruns = 4;

void refill(Sandbox& sandbox) noexcept {
    sandbox.fuel = kInstructionBudget;
    sandbox.overruns = 0;
}

void fuelHook(lua_State* lua, lua_Debug* ar) {
    Sandbox* sandbox = sandboxOf(lua);
    if (sandbox == nullptr) {
        return;
    }
    if (ar != nullptr && ar->event == LUA_HOOKCALL) {
        // The profiler's half of the hook: name the corpus function being entered. Only
        // functions defined in the vendored tree count — the adapter's own shims (`__rm_iter`
        // runs hundreds of times) and the engine bindings would otherwise drown the signal,
        // and the question the profile answers is "which of the AI'S code ran". Keyed from
        // `/lua/` because that is where a FAF path becomes meaningful, and because Lua has
        // already truncated the front of `short_src` to fit LUA_IDSIZE anyway.
        if (sandbox->profiling && lua_getinfo(lua, "nS", ar) != 0 && ar->source != nullptr
            && ar->what != nullptr && std::strcmp(ar->what, "Lua") == 0) {
            if (const char* site = std::strstr(ar->short_src, "/lua/"); site != nullptr) {
                std::string key{site};
                key += ":" + std::to_string(ar->linedefined);
                if (ar->name != nullptr) {
                    key += std::string{" ("} + ar->name + ")";
                }
                ++sandbox->profile[key];
            }
        }
        return;
    }
    sandbox->fuel -= 1000;
    if (sandbox->fuel <= 0) {
        // Refill BEFORE raising, so the code that catches this error gets a whole budget for
        // what follows; past kMaxOverruns the fuel stays spent and every hook raises, which
        // is what guarantees the chunk ends.
        if (++sandbox->overruns <= kMaxOverruns) {
            sandbox->fuel = kInstructionBudget;
        }
        // No %lld: lua_pushfstring supports only %d %f %s %p %c %U %%, and passing %lld makes
        // Lua raise "invalid option '%l'" INSTEAD of this message — which is how this bug first
        // showed up, as a format complaint standing in for a real diagnosis.
        luaL_error(lua, "instruction budget exhausted (%d M instructions) — a loop that "
                        "never yields",
                   static_cast<int>(kInstructionBudget / 1'000'000));
    }
}

Sandbox* sandboxOf(lua_State* lua) {
    lua_pushlightuserdata(lua, &kBindingsKey);
    lua_rawget(lua, LUA_REGISTRYINDEX);
    auto* sandbox = static_cast<Sandbox*>(lua_touserdata(lua, -1));
    lua_pop(lua, 1);
    return sandbox;
}

/// Every stub is this one function. Which name it is arrives as an upvalue holding the slot
/// index, so 256 registrations share one piece of code and one counter array.
int countedStub(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const auto slot = static_cast<std::size_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    if (sandbox != nullptr && slot < sandbox->slots.size()) {
        ++sandbox->slots[slot].calls;
    }
    // Nil, deliberately, and not zero or an empty table. A stub that returns something
    // plausible lets the AI carry on into a wrong decision; nil usually stops it at the call,
    // which is where the report can still name the cause.
    lua_pushnil(lua);
    return 1;
}

/// `LOG`/`WARN`/`SPEW` — 322 and 109 call sites. Real rather than stubbed because a corpus that
/// cannot log is a corpus that cannot tell us why it failed, which is the whole point of this
/// pass. Silent by default; the report counts them.
int logSink(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const auto slot = static_cast<std::size_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    if (sandbox != nullptr && slot < sandbox->slots.size()) {
        ++sandbox->slots[slot].calls;
    }
    if (sandbox != nullptr && sandbox->logPassthrough) {
        // The corpus's own voice, one line per call, arguments joined the way Moho's LOG
        // joins them. luaL_tolstring honours __tostring and never raises here.
        std::printf("  FAF:");
        const int args = lua_gettop(lua);
        for (int i = 1; i <= args; ++i) {
            std::printf(" %s", luaL_tolstring(lua, i, nullptr));
            lua_pop(lua, 1);
        }
        std::printf("\n");
    }
    return 0;
}

/// `GetGameTick()` — the sim tick, which the pump already carries for the thread scheduler.
/// Real because corpus housekeeping loops (navutils' path renderer, grid updates) call it
/// every tick to pace themselves, and a nil there turns arithmetic into an error.
int gameTick(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const auto slot = static_cast<std::size_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    if (sandbox != nullptr && slot < sandbox->slots.size()) {
        ++sandbox->slots[slot].calls;
    }
    lua_pushinteger(lua, sandbox != nullptr ? static_cast<lua_Integer>(sandbox->tick) : 0);
    return 1;
}

/// `GetGameTimeSeconds()` — the tick divided back into the seconds the corpus thinks in.
/// The time-gated builder conditions (`GreaterThanGameTime`) compare against it directly,
/// and a nil there is a comparison error that switches those builders off silently.
int gameTimeSeconds(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const auto slot = static_cast<std::size_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    if (sandbox != nullptr && slot < sandbox->slots.size()) {
        ++sandbox->slots[slot].calls;
    }
    const long long tick = sandbox != nullptr ? sandbox->tick : 0;
    lua_pushnumber(lua, static_cast<lua_Number>(tick) / static_cast<lua_Number>(kTicksPerSecond));
    return 1;
}

/// Resolves a FAF-relative path (`/lua/AI/aiutilities.lua`) under the vendored corpus.
[[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& root,
                                            std::string_view path) {
    std::string relative{path};
    while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.erase(relative.begin());
    }
    return root / relative;
}

/// `DiskFindFiles(directory, pattern)` — the engine's file enumeration, real rather than
/// stubbed because the corpus uses it to discover plugin files (custom factions, builder
/// packs) and a nil return walks straight into a for-loop. Answers from the VENDORED corpus,
/// which is the honest disk this sandbox has: a directory we did not fetch enumerates as
/// empty, exactly as an absent directory would in Moho.
int diskFindFiles(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const char* directory = luaL_optstring(lua, 1, "/");
    const char* pattern = luaL_optstring(lua, 2, "*");

    lua_newtable(lua);
    if (sandbox == nullptr) {
        return 1;
    }

    // The one glob form the corpus uses: a `*` prefix on a suffix — `*.lua`, `*_unit.bp`.
    std::string want{pattern};
    const bool anyPrefix = !want.empty() && want.front() == '*';
    if (anyPrefix) {
        want.erase(want.begin());
    }

    const std::filesystem::path base = resolve(sandbox->root, directory);
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec)) {
        return 1;  // absent is empty, not an error — most of the game is deliberately unfetched
    }

    int index = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = it->path().filename().string();
        const bool matches = anyPrefix ? name.size() >= want.size()
                                             && name.compare(name.size() - want.size(),
                                                             want.size(), want)
                                                    == 0
                                       : name == want;
        if (!matches) {
            continue;
        }
        // Back to the corpus's own spelling: absolute-from-root, forward slashes.
        std::string relative =
            std::filesystem::relative(it->path(), sandbox->root, ec).generic_string();
        lua_pushstring(lua, ("/" + relative).c_str());
        lua_rawseti(lua, -2, ++index);
    }
    return 1;
}


// --- The thread model --------------------------------------------------------------------
//
// Moho's threads, on Lua coroutines. FAF's whole runtime is written against ForkThread /
// WaitSeconds / WaitTicks — every manager is a loop that works a little and waits — and this
// is what was missing when every budget death was blamed on "a stubbed WaitSeconds". A fork
// is a coroutine plus a wake time; the pump resumes what is due each tick; a wait is a yield
// carrying the delay. One broken thread records an error and dies alone.

/// The handle's Destroy/SetPriority live in one shared metatable, created on demand.
void pushThreadHandleMeta(lua_State* lua);

int forkThread(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    luaL_checktype(lua, 1, LUA_TFUNCTION);
    if (sandbox == nullptr) {
        lua_pushnil(lua);
        return 1;
    }
    const int args = lua_gettop(lua) - 1;

    // The coroutine, with the function and its arguments moved across. The watchdog hook is
    // PER THREAD in Lua, so each coroutine arms its own — without this, a runaway forked
    // loop would be the one thing the budget could not stop.
    lua_State* co = lua_newthread(lua);
    lua_sethook(co, fuelHook, LUA_MASKCOUNT | LUA_MASKCALL, 1000);
    lua_pushvalue(lua, 1);
    lua_xmove(lua, co, 1);
    for (int i = 0; i < args; ++i) {
        lua_pushvalue(lua, 2 + i);
    }
    if (args > 0) {
        lua_xmove(lua, co, args);
    }

    Thread thread;
    thread.coroutine = luaL_ref(lua, LUA_REGISTRYINDEX);  // pops the thread object
    thread.wake = sandbox->tick;  // due at the next pump — a fork runs soon, not now
    thread.firstArgs = args;

    // The handle: what TrashBags hold and KillThread takes. Index is 1-based so a plain
    // truthiness test on the field works from Lua.
    lua_newtable(lua);
    lua_pushinteger(lua, static_cast<lua_Integer>(sandbox->threads.size() + 1));
    lua_setfield(lua, -2, "__rm_thread");
    pushThreadHandleMeta(lua);
    lua_setmetatable(lua, -2);
    lua_pushvalue(lua, -1);
    thread.handle = luaL_ref(lua, LUA_REGISTRYINDEX);

    sandbox->threads.push_back(thread);
    return 1;  // the handle
}

/// Marks a thread dead by handle. Shared by KillThread and handle:Destroy().
int killThread(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    if (sandbox == nullptr || !lua_istable(lua, 1)) {
        return 0;
    }
    lua_getfield(lua, 1, "__rm_thread");
    const lua_Integer index = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    if (index >= 1 && static_cast<std::size_t>(index) <= sandbox->threads.size()) {
        sandbox->threads[static_cast<std::size_t>(index - 1)].dead = true;
    }
    return 0;
}

int threadHandleDestroy(lua_State* lua) { return killThread(lua); }
int threadHandleNoop(lua_State*) { return 0; }

void pushThreadHandleMeta(lua_State* lua) {
    if (luaL_newmetatable(lua, "rm_faf_thread") != 0) {
        lua_newtable(lua);
        lua_pushcfunction(lua, threadHandleDestroy);
        lua_setfield(lua, -2, "Destroy");
        lua_pushcfunction(lua, threadHandleNoop);
        lua_setfield(lua, -2, "SetPriority");
        lua_setfield(lua, -2, "__index");
    }
}

/// `WaitSeconds`/`WaitTicks`. Inside a coroutine: yield the delay in ticks, floored at one —
/// a zero-tick wait is still a yield, which is exactly what breaks the spin the watchdog
/// used to kill. At the top level (module load, no coroutine): return immediately — Moho
/// never waits during import either, and a no-op is the honest translation.
int waitTicksCount(lua_State* lua, long long ticks) {
    if (lua_isyieldable(lua) == 0) {
        return 0;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(std::max(1ll, ticks)));
    return lua_yield(lua, 1);
}

int waitSeconds(lua_State* lua) {
    const double seconds = luaL_optnumber(lua, 1, 0.0);
    return waitTicksCount(lua,
                          static_cast<long long>(seconds * static_cast<double>(kTicksPerSecond)));
}

int waitTicks(lua_State* lua) {
    return waitTicksCount(lua, static_cast<long long>(luaL_optinteger(lua, 1, 1)));
}

int currentThread(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    if (sandbox == nullptr || sandbox->currentThread >= sandbox->threads.size()) {
        lua_pushnil(lua);
        return 1;
    }
    lua_rawgeti(lua, LUA_REGISTRYINDEX, sandbox->threads[sandbox->currentThread].handle);
    return 1;
}

/// Moho's Lua is not stock Lua, and the corpus proves it in five ways rather than the zero
/// ADR-039 first measured. That measurement checked the 5.0-isms `PLAN.md` catalogued on the SIM
/// corpus — `#` line comments (0 files here), `arg[]` (0), `table.getn`/`math.mod` (shimmed) —
/// and missed Moho's own extensions entirely, because it was looking for the wrong dialect.
/// Loading the files found them:
///
///   `!=` for `~=`        108 uses, 39 files. Patched in place; same width, so byte offsets hold.
///   `{&1 &0}`            14 table-preallocation hints, LuaJIT's `table.new` written into the
///                        syntax. Blanked — an optimisation with no observable semantics.
///   bitwise `& | << >>`  48 files. Native in 5.4, which is why the VM is 5.4 and not 5.1.
///   `continue`           a real statement in 25 of 208 files. Lowered onto 5.4's `goto`.
///   implicit `arg`       15 sites, including class.lua, which the whole object model needs.
///                        Restored as `table.pack(...)`, which is exactly 5.1's semantics.
///
/// THIS DOES NOT MODIFY THE VENDORED FILE. Every rewrite happens on the buffer between reading
/// and compiling, so `vendor/ai/faf` stays byte-identical to upstream and ADR-039's hard rule
/// holds. A transform in memory is the adapter's business; a patched file on disk is a fork.
///
/// ONE FORWARD PASS, and that is a correctness property rather than a performance one: the first
/// attempt at the `arg` shim scanned BACKWARDS from every `)` to find its `(`, which is
/// quadratic and hung the build on a 3,681-line file. Nothing here looks backwards.
///
/// LINE NUMBERS ARE PRESERVED. No rewrite inserts a newline, so an error at line 158 still means
/// line 158 of the file a reader will open.
[[nodiscard]] std::string rewriteMohoSource(const std::string& source, bool hashComments = false) {
    /// What kind of block we are inside. `continue` binds to the innermost enclosing LOOP, and
    /// the search for it stops at a Function boundary — a `continue` inside a closure nested in
    /// a loop belongs to that closure, not to the loop.
    enum class Kind : std::uint8_t { Loop, Plain, Function };
    struct Block {
        Kind kind = Kind::Plain;
        bool needsLabel = false;
    };

    std::string out;
    out.reserve(source.size() + source.size() / 8);
    std::vector<Block> blocks;

    bool pendingDo = false;    // saw `for`/`while`, waiting for the `do` that opens its body
    bool pendingThen = false;  // saw `if`, waiting for its `then` (an `elseif`'s does not open)
    bool inParams = false;     // inside a function's parameter list

    // The SIXTH Moho-ism, found the day the AI was asked to run rather than parse: bare
    // table iteration. `for k, v in class do` — no pairs() — is legal LuaPlus, and the
    // corpus does it 1,134 times; class.lua's own object model is built on it. Stock Lua
    // calls the table as an iterator, which with the Class factory's __call metamethod is
    // an infinite loop the watchdog reports as a budget death at class.lua:551.
    //
    // The rewrite wraps the WHOLE in-list: `in __rm_iter(<exprs>)`. That is safe for every
    // form — `__rm_iter` (kDialectShims) passes a function triple straight through and
    // turns a bare table into `next, t, nil` — so no expression analysis is needed, and a
    // multi-value `pairs(t)` inside the parentheses keeps all three values by Lua's own
    // last-argument rule.
    bool forHeader = false;    // saw `for`, deciding numeric (`=`) against generic (`in`)
    bool wrapOpen = false;     // emitted `__rm_iter(`, owes a `)` before the header's `do`
    int wrapFnDepth = 0;       // `function` opened inside the wrapped exprs (pathological)
    int parenDepth = 0;
    std::string params;

    const auto isWord = [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; };

    /// A long bracket `[[`, `[==[` — returns its level, or -1.
    const auto longLevel = [&source](std::size_t at) {
        if (source[at] != '[') {
            return -1;
        }
        std::size_t i = at + 1;
        int equals = 0;
        while (i < source.size() && source[i] == '=') {
            ++equals;
            ++i;
        }
        return (i < source.size() && source[i] == '[') ? equals : -1;
    };
    const auto copyLong = [&](std::size_t& i, int level) {
        const std::size_t open = i;
        i += static_cast<std::size_t>(level) + 2;
        while (i < source.size()) {
            if (source[i] == ']') {
                std::size_t j = i + 1;
                int equals = 0;
                while (j < source.size() && source[j] == '=') {
                    ++equals;
                    ++j;
                }
                if (equals == level && j < source.size() && source[j] == ']') {
                    i = j + 1;
                    break;
                }
            }
            ++i;
        }
        out.append(source, open, i - open);
    };

    std::size_t i = 0;
    while (i < source.size()) {
        const char c = source[i];

        if (hashComments && c == '#') {
            out.append("--");
            ++i;
            while (i < source.size() && source[i] != '\n') out.push_back(source[i++]);
            continue;
        }
        // A comment, short or long: copied verbatim, because `!=` in prose is not code.
        if (c == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            const int level = i + 2 < source.size() ? longLevel(i + 2) : -1;
            if (level >= 0) {
                out.append("--");
                i += 2;
                copyLong(i, level);
            } else {
                const std::size_t start = i;
                while (i < source.size() && source[i] != '\n') {
                    ++i;
                }
                out.append(source, start, i - start);
            }
            continue;
        }
        // A quoted string, likewise — rewriting `"a != b"` would change what the AI prints.
        if (c == '"' || c == '\'') {
            const std::size_t start = i;
            const char quote = c;
            ++i;
            while (i < source.size()) {
                if (source[i] == '\\') {
                    ++i;
                } else if (source[i] == quote) {
                    break;
                }
                ++i;
            }
            i = std::min(i + 1, source.size());
            out.append(source, start, i - start);
            continue;
        }
        if (const int level = longLevel(i); level >= 0) {
            copyLong(i, level);
            continue;
        }

        // A word: the keywords that open and close blocks, and `continue`.
        if (isWord(static_cast<unsigned char>(c)) && (std::isdigit(static_cast<unsigned char>(c)) == 0)) {
            const std::size_t start = i;
            while (i < source.size() && isWord(static_cast<unsigned char>(source[i]))) {
                ++i;
            }
            const std::string_view word(source.data() + start, i - start);
            // Retail AIUtilities.lua contains `> 0then`. Its older lexer accepts this;
            // Lua 5.4 treats the adjacent keyword as a malformed numeric suffix.
            if (hashComments && word == "then" && start > 0
                && std::isdigit(static_cast<unsigned char>(source[start-1])) != 0) out.push_back(' ');

            if (word == "function") {
                blocks.push_back(Block{Kind::Function, false});
                inParams = true;
                parenDepth = 0;
                params.clear();
                if (wrapOpen) {
                    ++wrapFnDepth;  // a function literal inside the wrapped exprs
                }
            } else if (word == "if") {
                pendingThen = true;
            } else if (word == "elseif") {
                pendingThen = false;  // shares the `if`'s block; its `then` opens nothing
            } else if (word == "then") {
                if (pendingThen) {
                    blocks.push_back(Block{Kind::Plain, false});
                    pendingThen = false;
                }
            } else if (word == "for") {
                pendingDo = true;
                forHeader = true;
            } else if (word == "while") {
                pendingDo = true;
            } else if (word == "in" && forHeader) {
                // The generic for: wrap the whole in-list. Same line, no newline, so every
                // downstream error still names the right line.
                forHeader = false;
                wrapOpen = true;
                out.append("in __rm_iter(");
                continue;
            } else if (word == "do") {
                if (wrapOpen && wrapFnDepth == 0) {
                    out.append(") ");
                    wrapOpen = false;
                }
                blocks.push_back(Block{pendingDo ? Kind::Loop : Kind::Plain, false});
                pendingDo = false;
                forHeader = false;  // a numeric for reached its body without an `in`
            } else if (word == "repeat") {
                blocks.push_back(Block{Kind::Loop, false});
            } else if (word == "end" || word == "until") {
                if (wrapOpen && wrapFnDepth > 0 && word == "end") {
                    --wrapFnDepth;  // the inline function inside the wrapped exprs closed
                }
                if (!blocks.empty()) {
                    if (blocks.back().needsLabel) {
                        // At the very END of the loop body, which is the one place Lua allows a
                        // label to be jumped to past local declarations. Same line, so nothing
                        // downstream shifts.
                        out.append("::__continue__:: ");
                    }
                    blocks.pop_back();
                }
            } else if (word == "continue") {
                bool bound = false;
                for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
                    if (block->kind == Kind::Function) {
                        break;
                    }
                    if (block->kind == Kind::Loop) {
                        block->needsLabel = true;
                        bound = true;
                        break;
                    }
                }
                if (bound) {
                    out.append("goto __continue__");
                    if (inParams) {
                        params.append(word);
                    }
                    continue;
                }
            }

            if (inParams) {
                params.append(word);
            }
            out.append(word);
            continue;
        }

        // Punctuation.
        if (c == '=' && forHeader) {
            forHeader = false;  // `for i = 1, n do` — numeric, nothing to wrap
        }
        if (c == '!' && i + 1 < source.size() && source[i + 1] == '=') {
            out.append("~=");
            i += 2;
            continue;
        }
        if (c == '&' && i + 1 < source.size()
            && std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0) {
            // `{&1 &0}` — a preallocation hint. Dropped: `{&1 &0}` and `{}` build the same
            // table, one having skipped a rehash. Genuine bitwise `and` appears three times in
            // the corpus and always as `x & y`, never glued to a digit.
            while (i < source.size()
                   && (source[i] == '&' || std::isdigit(static_cast<unsigned char>(source[i])) != 0)) {
                ++i;
            }
            continue;
        }
        if (inParams) {
            if (c == '(') {
                ++parenDepth;
            } else if (c == ')') {
                --parenDepth;
                if (parenDepth == 0) {
                    out.push_back(c);
                    ++i;
                    inParams = false;
                    if (params.find("...") != std::string::npos) {
                        // 5.1 defined `arg` implicitly inside a vararg function; 5.2 removed it
                        // and 5.4 has no compatibility switch. `table.pack` is exactly the old
                        // semantics, `n` field included. Emitted for every vararg function
                        // rather than only those mentioning `arg`, because deciding which do
                        // would need scope analysis and an unused pack costs one table.
                        out.append(" local arg = table.pack(...);");
                    }
                    continue;
                }
            }
            params.push_back(c);
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/// `import` — 592 call sites, the most-called name in the corpus and the one binding that
/// cannot be a stub: every AI file begins by importing the ones it builds on, so a stubbed
/// import means nothing loads at all.
///
/// Moho's own import caches by path and returns the module's table, which is what the corpus
/// relies on — `local AIUtils = import('/lua/ai/aiutilities.lua')` at the top of a file, then
/// `AIUtils.Foo()` throughout. Case-insensitivity is real too: the corpus spells the same file
/// '/lua/AI/aiutilities.lua' and '/lua/ai/aiutilities.lua', and Moho's VFS did not care.
int importModule(lua_State* lua) {
    Sandbox* sandbox = sandboxOf(lua);
    const auto slot = static_cast<std::size_t>(lua_tointeger(lua, lua_upvalueindex(1)));
    if (sandbox != nullptr && slot < sandbox->slots.size()) {
        ++sandbox->slots[slot].calls;
    }
    if (sandbox == nullptr) {
        lua_newtable(lua);
        return 1;
    }

    const char* raw = lua_tostring(lua, 1);
    if (raw == nullptr) {
        lua_newtable(lua);
        return 1;
    }
    std::string key{raw};
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // The module cache, keyed by the lowercased path.
    lua_getfield(lua, LUA_REGISTRYINDEX, "rm_faf_modules");
    lua_getfield(lua, -1, key.c_str());
    if (!lua_isnil(lua, -1)) {
        lua_remove(lua, -2);
        return 1;
    }
    lua_pop(lua, 1);

    if (sandbox->importDepth > 64) {
        lua_pop(lua, 1);
        lua_newtable(lua);  // a cycle: hand back something inert rather than blow the C stack
        return 1;
    }

    // Case-insensitive resolution: try the path as written, then walk the directory for a
    // case-folded match, because the corpus is inconsistent and the filesystem here is not.
    std::filesystem::path file = resolve(sandbox->root, raw);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        const std::filesystem::path dir = file.parent_path();
        std::string want = file.filename().string();
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::filesystem::is_directory(dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                std::string have = entry.path().filename().string();
                std::transform(have.begin(), have.end(), have.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (have == want) {
                    file = entry.path();
                    break;
                }
            }
        }
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        // A missing module is NOT an error here. The corpus imports across the whole game —
        // UI, sim, campaign — and this vendors only the AI subset, so most misses are files we
        // deliberately did not fetch. An empty table lets the importer carry on to whatever it
        // can do without them, which is more informative than stopping at the first one.
        lua_pop(lua, 1);
        sandbox->modules.push_back(ModuleLoad{raw, LoadOutcome::Missing, {}});
        lua_newtable(lua);
        return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = rewriteMohoSource(buffer.str());

    if (sandbox->verbose) {
        std::printf("  [ai] import %*s%s ... ", sandbox->importDepth * 2, "", raw);
        std::fflush(stdout);
    }
    ++sandbox->importDepth;
    const std::string chunk = "@" + file.string();
    // Refuelled per chunk rather than per VM: a module that legitimately does a lot of work at
    // load time should not starve the next one.
    // Refuelled per chunk. The hook itself is armed once at VM creation and never cleared:
    // clearing it here disarmed the PARENT's watchdog every time a nested import returned, so
    // an outer module could spin forever while its own budget sat untouched.
    refill(*sandbox);
    if (luaL_loadbuffer(lua, source.data(), source.size(), chunk.c_str()) != 0) {
        --sandbox->importDepth;
        const char* message = lua_tostring(lua, -1);
        if (sandbox->verbose) {
            std::printf("FAILED: %s\n", message != nullptr ? message : "?");
            std::fflush(stdout);
        }
        sandbox->modules.push_back(
            ModuleLoad{raw, LoadOutcome::Failed, message != nullptr ? message : "?"});
        lua_pop(lua, 2);  // error message and the module cache
        lua_newtable(lua);
        return 1;
    }

    // MOHO'S IMPORT RETURNS THE MODULE'S ENVIRONMENT, and this is the semantic the whole
    // corpus is built on: a FAF module exports by DECLARING GLOBALS — `BuilderManager =
    // Class(...) {...}` at file scope — and `import('/lua/sim/BuilderManager.lua')
    // .BuilderManager` reads that global out of the module's own table. The first version
    // returned the chunk's return value, which FAF modules never provide, so every
    // cross-module symbol was nil and `Class(nil)` took the whole manager layer down with
    // "setmetatable: table expected". Each module gets a fresh environment whose misses
    // fall through to _G, so engine globals and the corpus's declare-globals stay visible.
    lua_newtable(lua);                              // the module's environment
    lua_newtable(lua);                              // its metatable
    lua_pushglobaltable(lua);
    lua_setfield(lua, -2, "__index");               // reads fall back to _G
    lua_setmetatable(lua, -2);

    // `__moduleinfo`, as Moho's import injects it (documented by FAF's /lua/system/import.lua):
    // name, who-imports-me, and the reload-tracking flag. Modules write hot-reload hooks onto
    // it at file scope — `function __moduleinfo.OnDirty()` in navutils.lua — so without the
    // table the file fails at load over a feature this sandbox will never trigger.
    lua_newtable(lua);                              // __moduleinfo
    lua_pushstring(lua, raw);
    lua_setfield(lua, -2, "name");
    lua_newtable(lua);
    lua_setfield(lua, -2, "used_by");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "track_imports");
    lua_setfield(lua, -2, "__moduleinfo");
    lua_pushvalue(lua, -1);                         // keep a copy under the chunk
    lua_insert(lua, -3);                            // [env, chunk, env]
    const char* upvalue = lua_setupvalue(lua, -2, 1);  // chunk's _ENV := env
    if (upvalue == nullptr) {
        lua_pop(lua, 1);  // a chunk with no upvalue at all; keep the plain environment copy
    }

    if (lua_pcall(lua, 0, 0, 0) != 0) {
        --sandbox->importDepth;
        const char* message = lua_tostring(lua, -1);
        if (sandbox->verbose) {
            std::printf("FAILED: %s\n", message != nullptr ? message : "?");
            std::fflush(stdout);
        }
        sandbox->modules.push_back(
            ModuleLoad{raw, LoadOutcome::Failed, message != nullptr ? message : "?"});
        lua_pop(lua, 1);  // the error message; the env below still caches
        // The partially-initialised environment is still cached and returned: Moho's import
        // does the same, and half a module is far more informative to a dependant than an
        // empty table — most failures are one symbol deep in a file that defined ten.
    }
    else {
        if (sandbox->verbose) {
            std::printf("ok\n");
            std::fflush(stdout);
        }
        sandbox->modules.push_back(ModuleLoad{raw, LoadOutcome::Executed, {}});
    }
    --sandbox->importDepth;

    // The environment IS the module. Cached even on failure — see above.
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -3, key.c_str());
    lua_remove(lua, -2);
    return 1;
}

/// The three dialect shims the measurement called for: `table.getn` (115 sites) and `math.mod`
/// (1) were removed after Lua 5.0, and the corpus uses both while also using 5.1's `#` operator
/// at 49 sites. So it is 5.1 code with two 5.0 leftovers, and two lines cover them.
/// The engine objects the corpus expects to already exist.
///
/// `moho` holds the method tables every class in the corpus inherits from: `platoon.lua:42` is
/// `Platoon = Class(moho.platoon_methods) { ... }`, and `aibrain.lua` the same for brains. They
/// are populated from the SAME counted-stub table as everything else, so an AI calling
/// `self:GetPlatoonUnits()` reaches a named no-op that shows up in the report rather than a nil.
///
/// `categories` is the unit-category algebra — `categories.FACTORY * categories.TECH1 -
/// categories.AIR`, at 185 `EntityCategoryContains` call sites. Any name yields a category, and
/// the operators compose them, so the corpus's expressions evaluate rather than raising. What
/// they evaluate TO is inert, which is the honest state: this engine has role classification
/// (P3.1) that could answer these, and connecting the two is the next real piece of work.
constexpr const char* kEngineObjects = R"lua(
local methods = ...

-- EACH FAMILY GETS ITS OWN TABLE, sharing the stub FUNCTIONS but not the table identity.
--
-- Sharing one table looked economical and was a bug: `Class(moho.platoon_methods)` calls
-- `setmetatable` on the base it is given, so with one shared object every class overwrote the
-- previous class's metatable until the __index chain closed into a cycle — and a lookup on that
-- cycle spins forever. The watchdog caught it as "instruction budget exhausted" in
-- class.lua:585, which is exactly the kind of diagnosis a hang cannot give.
--
-- The stub closures are shared deliberately: the call counter lives in the closure's upvalue, so
-- one report still covers every family.
local function methodTable()
    local copy = {}
    for name, stub in pairs(methods) do
        copy[name] = stub
    end
    return copy
end

moho = {
    aibrain_methods     = methodTable(),
    platoon_methods     = methodTable(),
    unit_methods        = methodTable(),
    entity_methods      = methodTable(),
    prop_methods        = methodTable(),
    projectile_methods  = methodTable(),
    weapon_methods      = methodTable(),
    manipulator_methods = methodTable(),
    CAiBrain            = methodTable(),
    CPlatoon            = methodTable(),
}

-- REAL CATEGORIES. A category value is an expression tree — atoms are the tags a blueprint
-- declares (`categories.MOBILE`), and `* + -` build intersection, union and difference the
-- way Moho's EntityCategory algebra does. `__rm_catMatch` evaluates one against a unit's
-- tag SET (upper-cased, and including the unit's own id: in Moho every unit id is a
-- category, which is how `categories.ueb0101` names one unit). This replaces the earlier
-- synthetic object that answered every operation with a fresh opaque category — good enough
-- to let data files load, useless the moment a condition asks "is this unit MOBILE".
local categoryMeta = {}
local function catNode(op, a, b)
    return setmetatable({ __cat = op, a = a, b = b }, categoryMeta)
end
categoryMeta.__mul = function(a, b) return catNode('and', a, b) end
categoryMeta.__add = function(a, b) return catNode('or', a, b) end
categoryMeta.__sub = function(a, b) return catNode('sub', a, b) end
categoryMeta.__unm = function(a) return catNode('neg', a) end

categories = setmetatable({}, {
    __index = function(t, key)
        local category = catNode('tag', string.upper(key))
        rawset(t, key, category)     -- cached, so `categories.LAND == categories.LAND`
        return category
    end,
})

function __rm_catMatch(cat, set)
    if cat == nil or set == nil then return false end
    local op = cat.__cat
    -- ALLUNITS is the universal engine category, absent from authored BP tags.
    -- e.g. victory.lua uses ALLUNITS - WALL for annihilation (C-210).
    if op == 'tag' then return cat.a == 'ALLUNITS' or set[cat.a] == true end
    if op == 'and' then return __rm_catMatch(cat.a, set) and __rm_catMatch(cat.b, set) end
    if op == 'or'  then return __rm_catMatch(cat.a, set) or __rm_catMatch(cat.b, set) end
    if op == 'sub' then return __rm_catMatch(cat.a, set) and not __rm_catMatch(cat.b, set) end
    if op == 'neg' then return not __rm_catMatch(cat.a, set) end
    return false
end

-- The EntityCategory family, real now that categories can be evaluated. These overwrite the
-- counted stubs the macro pass installed (this chunk runs after it), so their calls no longer
-- appear in the binding report — the profile shows them running instead. A unit object is
-- anything carrying a `__cats` tag set, which is the contract the adapter's snapshots keep.
function ParseEntityCategory(text)
    local result = nil
    for word in string.gmatch(text or '', '[^%s%*]+') do
        local atom = categories[word]
        result = result and (result * atom) or atom
    end
    return result
end

function EntityCategoryContains(cat, unit)
    return __rm_catMatch(cat, unit and rawget(unit, '__cats'))
end

function EntityCategoryFilterDown(cat, units)
    local out = {}
    for _, u in ipairs(units or {}) do
        if EntityCategoryContains(cat, u) then table.insert(out, u) end
    end
    return out
end

function EntityCategoryCount(cat, units)
    local n = 0
    for _, u in ipairs(units or {}) do
        if EntityCategoryContains(cat, u) then n = n + 1 end
    end
    return n
end

-- Engine constructors the corpus calls at load time. Vectors are plain tables in Moho too, with
-- the same field names, so these are real rather than stubbed — cheap, and it means positions
-- the AI computes are positions we can read back.
-- With a metatable, because `utils.lua:973` reads one back off a constructed vector and holds
-- onto it (`local vector_metatable = getmetatable(Vector(0,0,0))`). A plain table made that nil
-- and took utils.lua down with it.
local vectorMeta = {}
function Vector(x, y, z) return setmetatable({ x, y, z, x = x, y = y, z = z }, vectorMeta) end
function Vector2(x, y)   return setmetatable({ x, y, x = x, y = y }, vectorMeta) end

-- The distance family, real for the same reason Vector is: positions the AI computes must be
-- positions we can compare. Moho's VDist3 ignores nothing — all three axes — while VDist2 is
-- the ground-plane distance over x and z, which is why its arguments are scalars not vectors.
-- These overwrite the counted stubs (this chunk runs after the macro pass).
function VDist2(x1, z1, x2, z2)
    local dx, dz = x1 - x2, z1 - z2
    return math.sqrt(dx * dx + dz * dz)
end
function VDist2Sq(x1, z1, x2, z2)
    local dx, dz = x1 - x2, z1 - z2
    return dx * dx + dz * dz
end
function VDist3(a, b)
    local dx, dy, dz = a[1] - b[1], (a[2] or 0) - (b[2] or 0), a[3] - b[3]
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end
function VDist3Sq(a, b)
    local dx, dy, dz = a[1] - b[1], (a[2] or 0) - (b[2] or 0), a[3] - b[3]
    return dx * dx + dy * dy + dz * dz
end

-- ScenarioInfo is the match description Moho publishes before anything else loads. Only the
-- shape matters here: the corpus indexes it during construction and would otherwise stop at the
-- first field. Filled in for real when the adapter knows the map (ADR-039's next step).
ScenarioInfo = ScenarioInfo or {
    Options = {},
    ArmySetup = {},
    MapData = { PlayableRect = { 0, 0, 1024, 1024 } },
    size = { 1024, 1024 },
    type = 'skirmish',
}
)lua";

constexpr const char* kDialectShims = R"lua(
table.getn = table.getn or function(t) return #t end
table.setn = table.setn or function() end
math.mod   = math.mod   or math.fmod
-- 5.1 -> 5.4: `unpack` became `table.unpack` (43 bare call sites in the corpus), and
-- `loadstring` became `load`. `setfenv`/`getfenv` need no shim: the corpus uses neither, which
-- is what made leaving 5.1 cheap.
unpack     = unpack     or table.unpack
loadstring = loadstring or load
-- 5.0 -> 5.1: `string.gfind` became `string.gmatch`. The shim matters in REVERSE: utils.lua
-- runs `rawset(string, 'gmatch', string.gfind)` for its own compatibility, which on 5.4
-- CLOBBERS the real gmatch with nil unless gfind names it first.
string.gfind = string.gfind or string.gmatch

-- FAF's own import.lua (which the C import replaces) declares the module-cache global that
-- factions.lua and friends probe to ask "is the UI loaded". Empty is the honest answer for
-- a headless skirmish: no UI modules, no active mods.
__modules     = __modules     or {}
__active_mods = __active_mods or {}

-- Bare table iteration, the rewriter's other half. `for k, v in t do` is legal LuaPlus and
-- the corpus does it 1,134 times; the rewrite wraps every generic for's in-list in this.
-- A function triple passes straight through (`pairs(t)` inside the parentheses keeps all
-- three values by the last-argument rule); a bare table iterates raw, exactly as Moho does.
function __rm_iter(f, s, v)
    if type(f) == "table" then
        return next, f, nil
    end
    return f, s, v
end

-- Moho's table extensions, captured as locals by class.lua and trashbag.lua BEFORE the
-- corpus's own utils.lua can define its versions — so they must exist first. `or`-guarded,
-- and utils.lua overwrites most of them later with FAF's own, which is the right order of
-- authority: ours are the floor, theirs are the house rules.
table.empty   = table.empty   or function(t) return next(t) == nil end
table.getsize = table.getsize or function(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    return n
end
table.find = table.find or function(t, value)
    for k, v in pairs(t) do
        if v == value then return k end
    end
    return nil
end
table.copy = table.copy or function(t)
    local copy = {}
    for k, v in pairs(t) do copy[k] = v end
    return copy
end
table.removeByValue = table.removeByValue or function(t, value)
    for k, v in ipairs(t) do
        if v == value then
            table.remove(t, k)
            return
        end
    end
end
)lua";

} // namespace

std::string rewriteLegacyLua(const std::string& source) { return rewriteMohoSource(source, true); }

std::string_view fidelityName(Fidelity fidelity) noexcept {
    switch (fidelity) {
    case Fidelity::Known:
        return "known";
    case Fidelity::Guessed:
        return "guessed";
    case Fidelity::Stub:
        break;
    }
    return "stub";
}

FafAi::FafAi(std::filesystem::path root, bool verbose)
    : root_(std::move(root)), verbose_(verbose) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root_, ec)) {
        lastError_ = "no vendored FAF corpus at " + root_.string() + " — run `make ai`";
        return;
    }

    state_ = luaL_newstate();
    if (state_ == nullptr) {
        lastError_ = "could not create a Lua state";
        return;
    }
    luaL_openlibs(state_);

    auto* sandbox = new Sandbox{};
    sandbox->root = root_;
    sandbox->verbose = verbose_;

    // Names that get a real implementation rather than a counted stub. Everything else in
    // FafApi.inc lands on `countedStub`, which is the honest default: not implemented, counted.
    const auto special = [](std::string_view name) -> lua_CFunction {
        if (name == "import") {
            return importModule;
        }
        if (name == "LOG" || name == "WARN" || name == "SPEW" || name == "_ALERT") {
            return logSink;
        }
        if (name == "DiskFindFiles") {
            return diskFindFiles;
        }
        if (name == "ForkThread") {
            return forkThread;
        }
        if (name == "KillThread") {
            return killThread;
        }
        if (name == "GetGameTick") {
            return gameTick;
        }
        if (name == "GetGameTimeSeconds") {
            return gameTimeSeconds;
        }
        if (name == "lazyimport") {
            // Eager where Moho is lazy: the real one defers loading until first field
            // access. Loading now instead is semantically safe — same module, same cache —
            // and a stub's nil errors on the very access laziness exists to serve.
            return importModule;
        }
        return countedStub;
    };
    const auto fidelityFor = [](std::string_view name) {
        if (name == "import") {
            return Fidelity::Known;
        }
        if (name == "LOG" || name == "WARN" || name == "SPEW" || name == "_ALERT") {
            return Fidelity::Known;
        }
        if (name == "DiskFindFiles") {
            return Fidelity::Known;
        }
        if (name == "ForkThread" || name == "KillThread") {
            return Fidelity::Known;
        }
        if (name == "GetGameTick" || name == "GetGameTimeSeconds") {
            return Fidelity::Known;
        }
        if (name == "lazyimport") {
            return Fidelity::Guessed;  // eager, where Moho defers to first access
        }
        return Fidelity::Stub;
    };

#define RM_FAF_GLOBAL(NAME, CALLS) sandbox->slots.push_back(Slot{NAME, Fidelity::Stub, CALLS, 0});
#define RM_FAF_METHOD(NAME, CALLS) sandbox->slots.push_back(Slot{NAME, Fidelity::Stub, CALLS, 0});
#define RM_FAF_SHIM(NAME, CALLS) sandbox->slots.push_back(Slot{NAME, Fidelity::Stub, CALLS, 0});
#include "app/FafApi.inc"

    for (Slot& slot : sandbox->slots) {
        slot.fidelity = fidelityFor(slot.name);
    }

    lua_pushlightuserdata(state_, &kBindingsKey);
    lua_pushlightuserdata(state_, sandbox);
    lua_rawset(state_, LUA_REGISTRYINDEX);

    lua_newtable(state_);
    lua_setfield(state_, LUA_REGISTRYINDEX, "rm_faf_modules");

    // Globals go on _G. Methods and shims go into one shared metatable that every engine object
    // this adapter hands to Lua uses, so `unit:GetPosition()` reaches a counted stub rather than
    // a nil — the load-time guarantee, applied to methods as well as free functions.
    lua_newtable(state_);  // the shared method table
    std::size_t index = 0;
    std::size_t globals = 0;

#undef RM_FAF_GLOBAL
#undef RM_FAF_METHOD
#undef RM_FAF_SHIM
#define RM_FAF_GLOBAL(NAME, CALLS)                                    \
    lua_pushinteger(state_, static_cast<lua_Integer>(index));         \
    lua_pushcclosure(state_, special(NAME), 1);                       \
    lua_setglobal(state_, NAME);                                      \
    ++index;                                                          \
    ++globals;
#define RM_FAF_METHOD(NAME, CALLS)                                    \
    lua_pushinteger(state_, static_cast<lua_Integer>(index));         \
    lua_pushcclosure(state_, countedStub, 1);                         \
    lua_setfield(state_, -2, NAME);                                   \
    ++index;
#define RM_FAF_SHIM(NAME, CALLS)                                      \
    lua_pushinteger(state_, static_cast<lua_Integer>(index));         \
    lua_pushcclosure(state_, countedStub, 1);                         \
    lua_setfield(state_, -2, NAME);                                   \
    ++index;
#include "app/FafApi.inc"

    lua_setfield(state_, LUA_REGISTRYINDEX, "rm_faf_methods");
    (void)globals;

    // The wait family is not in FafApi.inc — the generator counted only names the corpus
    // calls as globals it does not itself define, and FAF wraps its waits — so the real
    // implementations register by hand. They are the thread model's other half.
    lua_pushcfunction(state_, waitSeconds);
    lua_setglobal(state_, "WaitSeconds");
    lua_pushcfunction(state_, waitTicks);
    lua_setglobal(state_, "WaitTicks");
    lua_pushcfunction(state_, currentThread);
    lua_setglobal(state_, "CurrentThread");

    // Chunk first, THEN its argument — lua_pcall reads the stack as [function, arg1, ...], and
    // pushing the method table before the chunk left them the wrong way round, so the bootstrap
    // ran with nil and `moho` silently never existed.
    if (luaL_loadbuffer(state_, kEngineObjects, std::strlen(kEngineObjects), "@rm:engine-objects")
            != 0
        || (lua_getfield(state_, LUA_REGISTRYINDEX, "rm_faf_methods"), lua_pcall(state_, 1, 0, 0))
               != 0) {
        const char* message = lua_tostring(state_, -1);
        lastError_ = message != nullptr ? message : "engine objects failed";
        lua_pop(state_, 1);
    }

    // Armed once and never cleared — see the note in importModule.
    lua_sethook(state_, fuelHook, LUA_MASKCOUNT | LUA_MASKCALL, 1000);

    if (luaL_dostring(state_, kDialectShims) != 0) {
        const char* message = lua_tostring(state_, -1);
        lastError_ = message != nullptr ? message : "dialect shims failed";
        lua_pop(state_, 1);
    }

    // THE BOOTSTRAP. `Class` and `ClassSimple` are not engine functions and are not in the
    // annotations — they are the corpus's own OO helper, declared global by
    // `lua/system/class.lua` (its first line is `---@declare-global`). Moho ran that during
    // startup, long before any AI file loaded, so every manager, brain and platoon assumes it is
    // simply there: `Platoon = Class(moho.platoon_methods) { ... }` is platoon.lua's first
    // statement.
    //
    // Running it here is what makes the corpus's own object model work, and it is not a
    // substitute we wrote — it is FAF's file, executed in FAF's order.
    // The corpus's own `---@declare-global` modules, in dependency order. Moho ran these during
    // startup long before any AI file loaded, so every manager, brain and platoon assumes they
    // are simply there — `Platoon = Class(moho.platoon_methods) {...}` is platoon.lua's first
    // statement, and `TrashBag()` appears in the constructor of nearly every manager.
    //
    // These are FAF's files run in FAF's order, not substitutes we wrote. `repr` and `trashbag`
    // come before `utils`, which uses both.
    // Imported AND MERGED INTO _G: Moho ran these with `doscript`, into the shared global
    // environment — that is what `---@declare-global` means — while `import` now gives every
    // module a private environment (the Moho semantic the manager layer needs). The merge is
    // doscript's observable effect: `Class`, `TrashBag` and the builder tables become plain
    // globals every later module can reach.
    for (const char* module : {
             "/lua/system/class.lua",   // FIRST: everything below is built with Class()
             "/lua/system/repr.lua",
             "/lua/system/trashbag.lua",
             "/lua/system/utils.lua",
             "/lua/system/GlobalBaseTemplate.lua",
             "/lua/system/GlobalBuilderGroup.lua",
             "/lua/system/GlobalBuilderTemplate.lua",
             "/lua/system/GlobalPlatoonTemplate.lua",
         }) {
        (void)import(module);
        const std::string merge = std::string{"local m = import('"} + module
                                  + "'); for k, v in pairs(m) do rawset(_G, k, v) end";
        if (luaL_dostring(state_, merge.c_str()) != 0) {
            lua_pop(state_, 1);
        }
    }
}

FafAi::~FafAi() {
    if (state_ != nullptr) {
        delete sandboxOf(state_);
        lua_close(state_);
        state_ = nullptr;
    }
}

bool FafAi::import(std::string_view path) {
    if (state_ == nullptr) {
        return false;
    }
    lua_getglobal(state_, "import");
    lua_pushlstring(state_, path.data(), path.size());
    if (lua_pcall(state_, 1, 1, 0) != 0) {
        const char* message = lua_tostring(state_, -1);
        lastError_ = message != nullptr ? message : "import failed";
        lua_pop(state_, 1);
        return false;
    }
    lua_pop(state_, 1);
    // The return value is always a table — `import` is forgiving — so the answer comes from the
    // trail instead. The last entry is this path, unless nested imports pushed their own after
    // it, so search backwards for it.
    const Sandbox* sandbox = sandboxOf(state_);
    if (sandbox == nullptr) {
        return false;
    }
    for (auto it = sandbox->modules.rbegin(); it != sandbox->modules.rend(); ++it) {
        if (it->path == path) {
            lastError_ = it->error;
            return it->outcome == LoadOutcome::Executed;
        }
    }
    return true;  // already cached from an earlier call, which only happens after a success
}

std::size_t FafAi::pump(long long tick) {
    if (state_ == nullptr) {
        return 0;
    }
    Sandbox* sandbox = sandboxOf(state_);
    if (sandbox == nullptr) {
        return 0;
    }
    sandbox->tick = tick;

    std::size_t resumed = 0;
    // By index, not iterator: a resumed thread may FORK, growing the container. The forked
    // thread has wake == tick and runs this same pass, which is Moho's behaviour — a forked
    // thread starts without waiting a sim beat. A `Thread&` stays valid across the resume
    // because `threads` is a deque (see Sandbox::threads), which is what makes holding the
    // reference through a call that can grow the container legal.
    for (std::size_t i = 0; i < sandbox->threads.size(); ++i) {
        Thread& thread = sandbox->threads[i];
        if (thread.dead || thread.wake > tick) {
            continue;
        }
        lua_rawgeti(state_, LUA_REGISTRYINDEX, thread.coroutine);
        lua_State* co = lua_tothread(state_, -1);
        lua_pop(state_, 1);
        if (co == nullptr) {
            thread.dead = true;
            continue;
        }

        sandbox->currentThread = i;
        refill(*sandbox);
        int results = 0;
        const int args = thread.firstArgs >= 0 ? thread.firstArgs : 0;
        thread.firstArgs = -1;
        const int status = lua_resume(co, state_, args, &results);
        sandbox->currentThread = SIZE_MAX;
        ++resumed;

        if (status == LUA_YIELD) {
            // The yield carries the delay in ticks (the wait family's contract). A bare
            // coroutine.yield() waits one tick, which is Moho's smallest beat.
            const long long delay =
                results > 0 ? std::max(1ll, static_cast<long long>(lua_tointeger(co, -1)))
                            : 1;
            lua_settop(co, 0);
            thread.wake = tick + delay;
        } else {
            if (status != LUA_OK) {
                const char* message = lua_tostring(co, -1);
                sandbox->threadErrors.push_back(message != nullptr ? message : "?");
            }
            thread.dead = true;
        }
    }

    // Reclaim what dead threads hold at the registry, wherever they died: resumed to
    // completion here, killed from another thread's body through KillThread, or destroyed
    // through their own handle mid-run. The coroutine ref is its Lua stack; the handle ref
    // is the table a TrashBag may still point at — both are immortal until this unrefs
    // them, so without the sweep every fork/kill cycle leaks one of each for the rest of
    // the match. The slots themselves stay (see Sandbox::threads): only the refs grow.
    for (Thread& thread : sandbox->threads) {
        if (!thread.dead) {
            continue;
        }
        if (thread.coroutine != LUA_NOREF) {
            luaL_unref(state_, LUA_REGISTRYINDEX, thread.coroutine);
            thread.coroutine = LUA_NOREF;
        }
        if (thread.handle != LUA_NOREF) {
            luaL_unref(state_, LUA_REGISTRYINDEX, thread.handle);
            thread.handle = LUA_NOREF;
        }
    }
    return resumed;
}

std::size_t FafAi::threadsAlive() const {
    if (state_ == nullptr) {
        return 0;
    }
    const Sandbox* sandbox = sandboxOf(state_);
    if (sandbox == nullptr) {
        return 0;
    }
    std::size_t alive = 0;
    for (const Thread& thread : sandbox->threads) {
        if (!thread.dead) {
            ++alive;
        }
    }
    return alive;
}

std::vector<std::string> FafAi::threadErrors() const {
    if (state_ == nullptr) {
        return {};
    }
    const Sandbox* sandbox = sandboxOf(state_);
    return sandbox != nullptr ? sandbox->threadErrors : std::vector<std::string>{};
}

bool FafAi::eval(std::string_view chunk) {
    if (state_ == nullptr) {
        return false;
    }
    Sandbox* sandbox = sandboxOf(state_);
    if (sandbox != nullptr) {
        refill(*sandbox);
    }
    if (luaL_loadbuffer(state_, chunk.data(), chunk.size(), "@rm:eval") != 0
        || lua_pcall(state_, 0, 0, 0) != 0) {
        const char* message = lua_tostring(state_, -1);
        lastError_ = message != nullptr ? message : "eval failed";
        lua_pop(state_, 1);
        return false;
    }
    return true;
}

void FafAi::setProfiling(bool enabled) {
    if (state_ == nullptr) {
        return;
    }
    if (Sandbox* sandbox = sandboxOf(state_)) {
        sandbox->profiling = enabled;
    }
}

std::vector<std::pair<std::string, std::size_t>> FafAi::callProfile() const {
    if (state_ == nullptr) {
        return {};
    }
    const Sandbox* sandbox = sandboxOf(state_);
    if (sandbox == nullptr) {
        return {};
    }
    std::vector<std::pair<std::string, std::size_t>> sorted{sandbox->profile.begin(),
                                                            sandbox->profile.end()};
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return sorted;
}

void FafAi::setLogPassthrough(bool enabled) {
    Sandbox* sandbox = state_ != nullptr ? sandboxOf(state_) : nullptr;
    if (sandbox != nullptr) {
        sandbox->logPassthrough = enabled;
    }
}

void FafAi::refuel() {
    Sandbox* sandbox = state_ != nullptr ? sandboxOf(state_) : nullptr;
    if (sandbox != nullptr) {
        refill(*sandbox);
    }
}

std::vector<ModuleLoad> FafAi::modules() const {
    if (state_ == nullptr) {
        return {};
    }
    const Sandbox* sandbox = sandboxOf(state_);
    return sandbox != nullptr ? sandbox->modules : std::vector<ModuleLoad>{};
}

std::size_t FafAi::boundCount() const noexcept {
    if (state_ == nullptr) {
        return 0;
    }
    const Sandbox* sandbox = sandboxOf(state_);
    return sandbox != nullptr ? sandbox->slots.size() : 0;
}

std::vector<Binding> FafAi::report() const {
    std::vector<Binding> bindings;
    if (state_ == nullptr) {
        return bindings;
    }
    const Sandbox* sandbox = sandboxOf(state_);
    if (sandbox == nullptr) {
        return bindings;
    }
    bindings.reserve(sandbox->slots.size());
    for (const Slot& slot : sandbox->slots) {
        bindings.push_back(Binding{slot.name, slot.fidelity, slot.sites, slot.calls});
    }
    // Calls made first, then corpus call sites: what the AI actually reached for outranks what
    // it might have, and the static count breaks ties among everything it never got to.
    std::sort(bindings.begin(), bindings.end(), [](const Binding& a, const Binding& b) {
        if (a.calls != b.calls) {
            return a.calls > b.calls;
        }
        if (a.sites != b.sites) {
            return a.sites > b.sites;
        }
        return a.name < b.name;
    });
    return bindings;
}

namespace {

/// The files a brain is built from, in the order a match would reach them.
constexpr const char* kAiEntryPoints[] = {
    "/lua/AI/aiutilities.lua",
    "/lua/AI/aiattackutilities.lua",
    "/lua/AI/AIBehaviors.lua",
    "/lua/AI/aibuildstructures.lua",
    "/lua/aibrain.lua",
    "/lua/platoon.lua",
    "/lua/sim/BuilderManager.lua",
    "/lua/sim/EngineerManager.lua",
    "/lua/sim/FactoryBuilderManager.lua",
    "/lua/aibrains/base-ai.lua",
};

[[nodiscard]] std::filesystem::path findCorpusImpl() {
    for (const char* candidate : {"vendor/ai/faf", "../vendor/ai/faf", "../../vendor/ai/faf"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return {};
}

} // namespace

std::filesystem::path defaultCorpus() { return findCorpusImpl(); }

void importAiEntryPoints(FafAi& ai) {
    for (const char* path : kAiEntryPoints) {
        (void)ai.import(path);
    }

    // The data sweeps FAF's own simInit.lua does at session setup: every platoon template,
    // builder group and base template registers itself into the global tables the
    // Global*Template.lua systems installed at bootstrap. Swept with the corpus's own idiom —
    // DiskFindFiles then import — so the sanity report counts it the way a match would.
    // Then the brain registry: index.lua names every brain the lobby can seat, and importing
    // each is what pulls in the modern managers/tasks/templates trees.
    (void)ai.eval(R"(
        for _, dir in __rm_iter({ '/lua/AI/PlatoonTemplates',
                                  '/lua/AI/AIBuilders',
                                  '/lua/AI/AIBaseTemplates' }) do
            for _, file in __rm_iter(DiskFindFiles(dir, '*.lua')) do
                import(file)
            end
        end
        local index = import('/lua/aibrains/index.lua')
        for _, spec in __rm_iter(index.keyToBrain or {}) do
            import(spec[1])
        end
    )");
}

void reportFafSandbox() {
    std::printf("\n=== FAF AI sandbox (ADR-039) =========================================\n");

    const std::filesystem::path root = findCorpusImpl();
    if (root.empty()) {
        std::printf("  no corpus: vendor/ai/faf is missing. Run `make ai`.\n");
        std::printf("======================================================================\n\n");
        return;
    }
    std::printf("  corpus  %s\n", root.c_str());

    FafAi ai(root, /*verbose=*/true);
    if (!ai.ready()) {
        std::printf("  sandbox FAILED to start: %s\n", ai.lastError().c_str());
        std::printf("======================================================================\n\n");
        return;
    }
    std::printf("  bound   %zu engine names before any AI Lua ran\n", ai.boundCount());
    std::printf("  loading %zu entry points:\n", std::size(kAiEntryPoints));

    for (const char* path : kAiEntryPoints) {
        (void)ai.import(path);
    }

    std::size_t executed = 0;
    std::size_t missing = 0;
    std::size_t failed = 0;
    for (const ModuleLoad& module : ai.modules()) {
        switch (module.outcome) {
        case LoadOutcome::Executed: ++executed; break;
        case LoadOutcome::Missing:  ++missing;  break;
        case LoadOutcome::Failed:   ++failed;   break;
        }
    }
    std::printf("\n  MODULES  %zu executed, %zu missing (not vendored), %zu failed\n", executed,
                missing, failed);

    // Distinct failures only. The corpus imports the same file from many places, so the raw list
    // repeats one root cause a dozen times and buries the others.
    std::vector<std::string> seen;
    std::printf("\n  DISTINCT FAILURES — these are the work queue:\n");
    for (const ModuleLoad& module : ai.modules()) {
        if (module.outcome != LoadOutcome::Failed) {
            continue;
        }
        if (std::find(seen.begin(), seen.end(), module.error) != seen.end()) {
            continue;
        }
        seen.push_back(module.error);
        std::printf("    %-40s %s\n", module.path.c_str(), module.error.c_str());
    }
    if (seen.empty()) {
        std::printf("    (none)\n");
    }

    std::printf("\n  ENGINE CALLS the corpus made while loading (top 15):\n");
    std::size_t shown = 0;
    for (const Binding& binding : ai.report()) {
        if (binding.calls == 0 || shown >= 15) {
            break;
        }
        std::printf("    %-30s %6zu calls  %5d sites  %s\n", binding.name.c_str(), binding.calls,
                    binding.sites, std::string(fidelityName(binding.fidelity)).c_str());
        ++shown;
    }
    if (shown == 0) {
        std::printf("    (none — nothing executed far enough to call the engine)\n");
    }
    std::printf("======================================================================\n\n");
}

} // namespace rm::ai
