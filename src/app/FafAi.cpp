#include "app/FafAi.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
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

struct Sandbox {
    std::vector<Slot> slots;
    std::filesystem::path root;
    /// Cheap re-entrancy guard: `import` runs Lua that may import again, and a cycle in the
    /// corpus would otherwise recurse until the C stack gave out.
    int importDepth = 0;
    /// Every load attempted, so a forgiving import still leaves a trail.
    std::vector<ModuleLoad> modules;
};

[[nodiscard]] Sandbox* sandboxOf(lua_State* lua) {
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

/// Moho's Lua is not stock Lua, and the corpus proves it in three ways rather than the zero
/// ADR-039 first measured. That measurement checked `#` line comments (0 files here), `arg[]`
/// (0) and `table.getn`/`math.mod` (shimmed above) — and missed two extensions entirely,
/// because it was looking for the 5.0-isms PLAN.md had catalogued rather than for Moho's own:
///
///   `!=` for `~=`     108 uses across 39 files. Fixed here, lexically.
///   `continue`        a real statement in Moho, used as `if x then continue end`. NOT fixed —
///                     Lua 5.1 has no `goto` to lower it onto, so it needs either a newer Lua
///                     or a parser patch. Files using it still fail to load, by design: a
///                     silent workaround would be worse than a named blocker.
///
/// THIS DOES NOT MODIFY THE VENDORED FILE. The rewrite happens on the buffer between reading
/// and compiling, so `vendor/ai/faf` stays byte-identical to upstream and the never-modify rule
/// (ADR-039) holds. A transform in memory is the adapter's business; a patched file on disk
/// would be a fork.
///
/// Strings and comments are skipped, because `"a != b"` in a log message is not code and
/// rewriting it would change what the AI prints. Long brackets are tracked by level so
/// `[==[ ... ]==]` closes on its own delimiter.
[[nodiscard]] std::string rewriteMohoOperators(std::string source) {
    enum class Mode { Code, Line, Block, Quote };
    Mode mode = Mode::Code;
    char quote = '\0';
    int level = 0;

    const auto longBracket = [&source](std::size_t at, int& depth) {
        if (source[at] != '[') {
            return false;
        }
        std::size_t i = at + 1;
        int equals = 0;
        while (i < source.size() && source[i] == '=') {
            ++equals;
            ++i;
        }
        if (i < source.size() && source[i] == '[') {
            depth = equals;
            return true;
        }
        return false;
    };

    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        switch (mode) {
        case Mode::Code:
            if (c == '-' && i + 1 < source.size() && source[i + 1] == '-') {
                int depth = 0;
                if (i + 2 < source.size() && longBracket(i + 2, depth)) {
                    mode = Mode::Block;
                    level = depth;
                    i += 2;
                } else {
                    mode = Mode::Line;
                }
            } else if (c == '"' || c == '\'') {
                mode = Mode::Quote;
                quote = c;
            } else if (longBracket(i, level)) {
                mode = Mode::Block;
            } else if (c == '!' && i + 1 < source.size() && source[i + 1] == '=') {
                // `~=` is the same width as `!=`, so this is an in-place patch and every byte
                // offset in a later error message still points where the reader expects.
                source[i] = '~';
            } else if (c == '&' && i + 1 < source.size()
                       && (std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0)) {
                // A TABLE PREALLOCATION HINT, not arithmetic: Moho lets a constructor say how
                // much room to reserve, as in `local instance = {&1 &0}` (class.lua:579) —
                // LuaJIT's `table.new(narr, nhash)` written into the syntax. 14 constructors in
                // the corpus use it.
                //
                // Blanked rather than translated, because it is an optimisation with no
                // observable semantics: `{&1 &0}` and `{}` build the same table, one of them
                // having skipped a rehash. Blanked rather than DELETED so byte offsets survive
                // and a later error still points at the right column.
                //
                // Genuine bitwise `and` is left alone — it appears three times, always as
                // `x & y` with spaces around an operand, and never as `&` glued to a digit.
                std::size_t j = i;
                while (j < source.size()
                       && (source[j] == '&'
                           || std::isdigit(static_cast<unsigned char>(source[j])) != 0)) {
                    source[j] = ' ';
                    ++j;
                }
                i = j - 1;
            }
            break;
        case Mode::Line:
            if (c == '\n') {
                mode = Mode::Code;
            }
            break;
        case Mode::Quote:
            if (c == '\\') {
                ++i;  // an escape consumes the next byte, including a quote
            } else if (c == quote) {
                mode = Mode::Code;
            }
            break;
        case Mode::Block:
            if (c == ']') {
                std::size_t j = i + 1;
                int equals = 0;
                while (j < source.size() && source[j] == '=') {
                    ++equals;
                    ++j;
                }
                if (equals == level && j < source.size() && source[j] == ']') {
                    mode = Mode::Code;
                    i = j;
                }
            }
            break;
        }
    }
    return source;
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
    const std::string source = rewriteMohoOperators(buffer.str());

    ++sandbox->importDepth;
    const std::string chunk = "@" + file.string();
    if (luaL_loadbuffer(lua, source.data(), source.size(), chunk.c_str()) != 0
        || lua_pcall(lua, 0, 1, 0) != 0) {
        --sandbox->importDepth;
        const char* message = lua_tostring(lua, -1);
        sandbox->modules.push_back(
            ModuleLoad{raw, LoadOutcome::Failed, message != nullptr ? message : "?"});
        lua_pop(lua, 2);  // error message and the module cache
        lua_newtable(lua);
        return 1;
    }
    --sandbox->importDepth;
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

FafAi::FafAi(std::filesystem::path root) : root_(std::move(root)) {
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
    for (const char* module : {"/lua/system/class.lua", "/lua/system/utils.lua"}) {
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

} // namespace rm::ai
