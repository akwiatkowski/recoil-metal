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
#include <fstream>
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
};

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

void fuelHook(lua_State* lua, lua_Debug*) {
    Sandbox* sandbox = sandboxOf(lua);
    if (sandbox == nullptr) {
        return;
    }
    sandbox->fuel -= 1000;
    if (sandbox->fuel <= 0) {
        // No %lld: lua_pushfstring supports only %d %f %s %p %c %U %%, and passing %lld makes
        // Lua raise "invalid option '%l'" INSTEAD of this message — which is how this bug first
        // showed up, as a format complaint standing in for a real diagnosis.
        luaL_error(lua, "instruction budget exhausted (%d M instructions) — a loop that never "
                        "yields, most likely waiting on a stubbed WaitSeconds",
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
/// index, so 254 registrations share one piece of code and one counter array.
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
    return 0;
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
[[nodiscard]] std::string rewriteMohoSource(const std::string& source) {
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

            if (word == "function") {
                blocks.push_back(Block{Kind::Function, false});
                inParams = true;
                parenDepth = 0;
                params.clear();
            } else if (word == "if") {
                pendingThen = true;
            } else if (word == "elseif") {
                pendingThen = false;  // shares the `if`'s block; its `then` opens nothing
            } else if (word == "then") {
                if (pendingThen) {
                    blocks.push_back(Block{Kind::Plain, false});
                    pendingThen = false;
                }
            } else if (word == "for" || word == "while") {
                pendingDo = true;
            } else if (word == "do") {
                blocks.push_back(Block{pendingDo ? Kind::Loop : Kind::Plain, false});
                pendingDo = false;
            } else if (word == "repeat") {
                blocks.push_back(Block{Kind::Loop, false});
            } else if (word == "end" || word == "until") {
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
    sandbox->fuel = kInstructionBudget;
    if (luaL_loadbuffer(lua, source.data(), source.size(), chunk.c_str()) != 0
        || lua_pcall(lua, 0, 1, 0) != 0) {
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
    --sandbox->importDepth;
    if (sandbox->verbose) {
        std::printf("ok\n");
        std::fflush(stdout);
    }
    sandbox->modules.push_back(ModuleLoad{raw, LoadOutcome::Executed, {}});

    // A module that returns nothing still gets a table, because the caller will index it.
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
    }
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

local categoryMeta = {}
local function newCategory()
    return setmetatable({ __isCategory = true }, categoryMeta)
end
categoryMeta.__mul   = newCategory   -- intersection
categoryMeta.__add   = newCategory   -- union
categoryMeta.__sub   = newCategory   -- difference
categoryMeta.__unm   = newCategory   -- negation
categoryMeta.__index = function() return newCategory() end

categories = setmetatable({}, {
    __index = function(t, key)
        local category = newCategory()
        rawset(t, key, category)     -- cached, so `categories.LAND == categories.LAND`
        return category
    end,
})

-- Engine constructors the corpus calls at load time. Vectors are plain tables in Moho too, with
-- the same field names, so these are real rather than stubbed — cheap, and it means positions
-- the AI computes are positions we can read back.
-- With a metatable, because `utils.lua:973` reads one back off a constructed vector and holds
-- onto it (`local vector_metatable = getmetatable(Vector(0,0,0))`). A plain table made that nil
-- and took utils.lua down with it.
local vectorMeta = {}
function Vector(x, y, z) return setmetatable({ x, y, z, x = x, y = y, z = z }, vectorMeta) end
function Vector2(x, y)   return setmetatable({ x, y, x = x, y = y }, vectorMeta) end

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
)lua";

} // namespace

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
        return countedStub;
    };
    const auto fidelityFor = [](std::string_view name) {
        if (name == "import") {
            return Fidelity::Known;
        }
        if (name == "LOG" || name == "WARN" || name == "SPEW" || name == "_ALERT") {
            return Fidelity::Known;
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
    lua_sethook(state_, fuelHook, LUA_MASKCOUNT, 1000);

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

[[nodiscard]] std::filesystem::path findCorpus() {
    for (const char* candidate : {"vendor/ai/faf", "../vendor/ai/faf", "../../vendor/ai/faf"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate);
        }
    }
    return {};
}

} // namespace

void reportFafSandbox() {
    std::printf("\n=== FAF AI sandbox (ADR-039) =========================================\n");

    const std::filesystem::path root = findCorpus();
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
