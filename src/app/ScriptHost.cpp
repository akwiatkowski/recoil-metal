#include "app/ScriptHost.hpp"

#include "app/FafAi.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace rm::app {
namespace {

/// Bootstrap. `Class(bases)` returns a class table; `__rm_construct(classTable)` returns
/// an instance table that records the four lifecycle hooks as fields. Both halves keep
/// out of the engine-side metatable so a class hierarchy does not chain through them.
constexpr const char* kBootstrap = R"lua(
-- Class/State/ChangeState: the shipped `class.lua` object model in miniature
-- (`C-318`, `mohodata/lua/system/class.lua`). Every task class is built with
-- this `Class`; shipped states are `State { ... }` placeholders inside the
-- class spec which materialize into real derived classes here, exactly where
-- retail's `StateProxiesToClasses` runs at class creation. A state derives
-- from its containing class (`setmetatable(state, {__index = class})`), so
-- the instance keeps its fields across `ChangeState` while method lookup
-- moves to the new state — including sibling states reached through the
-- chained class. `OnExitState`/`OnEnterState` fire around the switch when
-- present. Retail also kills the old `Main` thread and forks the new state's
-- `Main` here; this host has no threads (`ForkThread`/`WaitFor` live in the
-- FAF sandbox), so `Main` is ignored.
-- `moho` engine-method tables (`C-228`/`C-388`): retail's `ScriptTask.lua:58`
-- runs `Class(moho.ScriptTask_Methods)` — `moho` is an engine global, so the
-- host seeds a stub table carrying an (empty) metatable that marks it an
-- engine object, the way the FAF sandbox seeds its `moho.*_methods` tables
-- (`src/app/FafAi.cpp`, `kEngineObjects`). The stub contributes no methods:
-- the task natives arrive from the host's class/instance stamping below.
-- `table.getn` (Lua 5.0): `class.lua:77` captures it at load; 5.4 dropped
-- it. Same floor as the FAF sandbox's `kDialectShims`.
table.getn = table.getn or function(t) return #t end
local __rm_engineMeta = {}
moho = {
    ScriptTask_Methods = setmetatable({}, __rm_engineMeta),
}
local ClassTag = {}
function Class(...)
    local bases = {...}
    return function(fields)
        fields = fields or {}
        for key, value in pairs(fields) do
            if type(value) == 'table' and value[ClassTag] == 'state' then
                local state = { __state = key }
                state.__index = state
                for k, v in pairs(value.spec) do state[k] = v end
                setmetatable(state, { __index = fields })
                fields[key] = state
            end
        end
        setmetatable(fields, {
            [ClassTag] = 'class',
            __index = function(_, key)
                for _, base in ipairs(bases) do
                    local v = base and base[key]
                    if v ~= nil then return v end
                end
            end,
            __call = function(klass, ...)
                local self = setmetatable({}, {__index = klass})
                if klass.OnPreCreate then klass.OnPreCreate(self, ...) end
                if klass.OnCreate then klass.OnCreate(self, ...) end
                return self
            end,
        })
        return fields
    end
end
function State(spec)
    return { [ClassTag] = 'state', spec = spec or {} }
end
function ChangeState(obj, newstate)
    if type(newstate) == 'string' then newstate = obj[newstate] end
    assert(type(newstate) == 'table' and newstate.__state ~= nil,
        'ChangeState: target is not a State')
    local already = getmetatable(obj)
    if already ~= nil and already.__index == newstate then return end
    local onExit = obj.OnExitState
    if onExit then onExit(obj) end
    setmetatable(obj, { __index = newstate })
    local onEnter = obj.OnEnterState
    if onEnter then onEnter(obj) end
end




local modules = {}
local function importModule(path)
    path = string.lower(path)
    if path:sub(1,1) ~= '/' then path = '/lua/' .. path end
    if modules[path] then return modules[path] end
    local source = __rm_read(path)
    if not source then error('missing script module: '..path) end
    -- The module env falls back to `_G` (`Class`, `State`, `ChangeState`,
    -- `moho`, `SecondsPerTick`, `__rm_iter`, `table.getn` all live there), so
    -- shipped `import` chains resolve host globals the same way a file loaded
    -- straight into the global env would. `import` itself is pinned on the
    -- env so nested `import(...)` calls inside the loaded chunk keep working.
    -- The loaded module's globals (`ScriptTask`, `TASKSTATUS`, `AIRESULT`,
    -- `EnhanceTask`) stay on the env table, which is what `import` returns —
    -- so the importer's `local X = import(...).X` reads them. They are ALSO
    -- mirrored onto `_G`: retail's task chunks run in the shared global env,
    -- and sibling hooks concatenated by `C-311` may reference each other's
    -- globals without importing.
    local env = setmetatable({ import = importModule }, {__index = _G})
    modules[path] = env
    setmetatable(env, {__index = _G, __newindex = _G})
    local chunk, why = load(source, '@'..path, 't', env)
    setmetatable(env, {__index = _G})
    if not chunk then modules[path] = nil; error(why) end
    local ok, why = pcall(chunk)
    if not ok then modules[path] = nil; error(why) end
    return env
end
import = importModule

-- Bare-table `for ... in <table>` is legal LuaPlus and the corpus does it
-- 1,134 times (`class.lua`'s own object model is built on it). The load-path
-- rewrite wraps every generic-for in-list in `__rm_iter(...)`, which passes
-- a function triple straight through and turns a bare table into
-- `next, t, nil` (same shim as the FAF sandbox's `kDialectShims`).
function __rm_iter(f, s, v)
    if type(f) == 'table' then
        return next, f, nil
    end
    return f, s, v
end

-- `doscript` is the same `C-311` hook-aware concat as `import`, run in the
-- caller's environment rather than a fresh module one (`C-311`:
-- `SCR_LuaDoFileConcat` serves both; `0x004d4de0`). `env` defaults to the
-- caller: with no argument the chunk runs in `_G`, which is the observable
-- effect retail bootstrap files rely on. A missing file is a Lua error, not
-- the empty table `import` hands back for deliberately unvendored AI files.
function doscript(path, env)
    path = string.lower(path)
    if path:sub(1,1) ~= '/' then path = '/lua/' .. path end
    local source = __rm_read(path)
    if not source then error('doscript: cannot open '..path) end
    local chunk, why = load(source, '@'..path, 't', env or _G)
    if not chunk then error(why) end
    return chunk()
end

-- The host instantiates one class table per (unit, task); the constructor invokes the
-- two creation hooks (OnPreCreate, OnCreate) and stores the lifecycle functions so the
-- dispatcher can invoke them per beat. `tick` is the no-state fallback only:
-- per-beat dispatch re-reads the instance's live `TaskTick`, which is the current
-- state's after `ChangeState` (`C-318`).
function __rm_construct(klass, commandData)
    local self = klass(commandData)
    rawset(self, '__rm_hooks',
        {pre = klass.OnPreCreate, create = klass.OnCreate,
         tick = klass.TaskTick, destroy = klass.OnDestroy})
    return self
end
)lua";

constexpr const char* kHostTable = "rm_script_host";

[[nodiscard]] std::string toString(lua_State* lua, int index) {
    const char* raw = lua_tostring(lua, index);
    return raw != nullptr ? std::string{raw} : std::string{};
}

int concatSource(lua_State* lua) {
    // `C-311` (`SCR_LuaDoFileConcat`, `0x004d4de0`): every `doscript`/`import`
    // load collects the base file plus each registered hookdir's copy of the
    // same path as ONE chunk, in hookdir registration order — the same rule
    // `ensureInstance` applies to the task's own modulePath below.
    auto* content = static_cast<const rm::vfs::Vfs*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const char* path = lua_tostring(lua, 1);
    if (path == nullptr) { lua_pushnil(lua); return 1; }
    const auto bytes = content->read(path);
    if (!bytes) { lua_pushnil(lua); return 1; }
    std::string concatenated{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    for (const std::string& hookPath : content->hooksFor(path)) {
        if (const auto hookBytes = content->read(hookPath)) {
            concatenated.push_back('\n');
            concatenated.append(reinterpret_cast<const char*>(hookBytes->data()),
                                hookBytes->size());
        }
    }
    const std::string translated = rm::ai::rewriteLegacyLua(concatenated);
    lua_pushlstring(lua, translated.data(), translated.size());
    return 1;
}
// `C-228` task natives. Retail's `CUnitScriptTask` exposes exactly four
// bindings on the task object — `GetUnit`, `SetAIResult(0..3)`, and the latent
// `LUnitMove`/`LUnitMoveNear` pair with zero shipped callers (`C-437`, dead
// surface kept as no-ops). The unit table the task drives carries the
// `EnhanceTask.lua:11-80` beat surface retail's own scripts touch:
// `SetWorkProgress`/`SetUnitState`/`OnWorkBegin`/`OnWorkEnd`/`OnWorkFail`/
// `OnBuildProgress` (state only), `IsMobile`/`IsMoving`/`IsPaused` (all false
// here — the sim owns motion), `GetNavigator():AbortMove()` (records the
// abort), `GetResourceConsumed()` (0 until a test publishes funding),
// `WorkProgress` (the funded fraction), `GetBuildRate` (1.0 build unit per
// beat), and `SecondsPerTick` (1/10 s at the 10 Hz `kDefaultTicksPerSecond`,
// `core/sim/TickRate.hpp`).
constexpr const char* kTaskNativesKey = "rm_script_task_native";
struct TaskNative {
    rm::sim::UnitId unit{};
    rm::sim::ScriptTaskState* state = nullptr;
};
[[nodiscard]] int pushUnitTable(lua_State* lua, TaskNative* native);
[[nodiscard]] TaskNative* taskNative(lua_State* lua) {
    // Reads, never writes: two `lua_getfield` pushes with no interleaved
    // script execution, so relative `-1`/`-2` addressing is exact here. (The
    // writer side — `NativeGuard` — cannot assume that; see its note.)
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return nullptr;
    }
    lua_getfield(lua, -1, "current");
    auto* native = static_cast<TaskNative*>(lua_touserdata(lua, -1));
    lua_pop(lua, 2);
    return native;
}
[[nodiscard]] int taskNoopFn(lua_State* lua) {
    (void)lua;
    return 0;
}
[[nodiscard]] int taskFalseFn(lua_State* lua) {
    lua_pushboolean(lua, 0);
    return 1;
}
[[nodiscard]] int taskBuildRateFn(lua_State* lua) {
    lua_pushnumber(lua, 1.0);
    return 1;
}
[[nodiscard]] int taskConsumedFn(lua_State* lua) {
    // `C-436` (`EnhanceTask.lua:51-56`): the `Enhancing` beat accrues ONLY
    // when `obtained > 0`. The sim publishes funding per unit
    // (`publishTaskFunding`); unset units read 0 — the funded stall.
    TaskNative* native = taskNative(lua);
    double consumed = 0.0;
    if (native != nullptr) {
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        const std::string key = std::to_string(native->unit.generation) + ":"
                                + std::to_string(native->unit.index) + ":funding";
        lua_getfield(lua, -1, key.c_str());
        if (lua_isnumber(lua, -1)) consumed = lua_tonumber(lua, -1);
        lua_pop(lua, 2);
    }
    lua_pushnumber(lua, consumed);
    return 1;
}
[[nodiscard]] int taskSetProgressFn(lua_State* lua) {
    // `SetWorkProgress` mirrors the fraction onto the unit table's
    // `WorkProgress` field — the field the shipped beat READS
    // (`local current = unit.WorkProgress`) and the test reads back.
    lua_pushvalue(lua, 2);
    lua_setfield(lua, 1, "WorkProgress");
    return 0;
}
[[nodiscard]] int taskSecondsPerTickFn(lua_State* lua) {
    // Called as `SecondsPerTick()` in shipped code (`EnhanceTask.lua:58`).
    // One beat is 1/10 s at the 10 Hz `kDefaultTicksPerSecond`
    // (`core/sim/TickRate.hpp`) — the same clock the `C-305` `n-1` quirk is
    // measured against.
    lua_pushnumber(lua, 0.1);
    return 1;
}
[[nodiscard]] int taskAbortMoveFn(lua_State* lua) {
    // `GetNavigator()` returns a one-method table; `AbortMove` records the abort.
    lua_newtable(lua);
    lua_pushcfunction(lua, &taskNoopFn);
    lua_setfield(lua, -2, "AbortMove");
    return 1;
}
[[nodiscard]] int taskGetUnitFn(lua_State* lua) {
    // Method call: `self` sits at index 1 and stays there; the unit table is
    // the single return value.
    TaskNative* native = taskNative(lua);
    if (native == nullptr) {
        lua_newtable(lua);
        return 1;
    }
    return pushUnitTable(lua, native);
}
[[nodiscard]] int taskSetAiResultFn(lua_State* lua) {
    TaskNative* native = taskNative(lua);
    const int result = static_cast<int>(luaL_checkinteger(lua, 2));
    if (native != nullptr && native->state != nullptr) {
        native->state->aiResult = result;
    }
    return 0;
}
[[nodiscard]] int pushUnitTable(lua_State* lua, TaskNative* native) {
    if (native == nullptr) {
        return luaL_error(lua, "GetUnit: no active task");
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    const std::string key = std::to_string(native->unit.generation) + ":"
                            + std::to_string(native->unit.index);
    lua_getfield(lua, -1, key.c_str());
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, key.c_str());
        lua_pushcfunction(lua, &taskFalseFn);
        lua_setfield(lua, -2, "IsMobile");
        lua_pushcfunction(lua, &taskFalseFn);
        lua_setfield(lua, -2, "IsMoving");
        lua_pushcfunction(lua, &taskFalseFn);
        lua_setfield(lua, -2, "IsPaused");
        lua_pushcfunction(lua, &taskSetProgressFn);
        lua_setfield(lua, -2, "SetWorkProgress");
        lua_pushcfunction(lua, &taskNoopFn);
        lua_setfield(lua, -2, "SetUnitState");
        lua_pushcfunction(lua, &taskConsumedFn);
        lua_setfield(lua, -2, "GetResourceConsumed");
        lua_pushcfunction(lua, &taskNoopFn);
        lua_setfield(lua, -2, "OnWorkBegin");
        lua_pushcfunction(lua, &taskNoopFn);
        lua_setfield(lua, -2, "OnWorkEnd");
        lua_pushcfunction(lua, &taskNoopFn);
        lua_setfield(lua, -2, "OnWorkFail");
        lua_pushcfunction(lua, &taskNoopFn);
        lua_setfield(lua, -2, "OnBuildProgress");
        lua_pushcfunction(lua, &taskBuildRateFn);
        lua_setfield(lua, -2, "GetBuildRate");
        lua_pushcfunction(lua, &taskAbortMoveFn);
        lua_setfield(lua, -2, "GetNavigator");
        lua_pushnumber(lua, 0.0);
        lua_setfield(lua, -2, "WorkProgress");
        lua_pushnumber(lua, 1.0);
        lua_setfield(lua, -2, "WorkItemBuildTime");
        // `publishTaskFunding` may have run before the first `GetUnit` (the
        // test publishes right after `applyCommand`); apply stored values so
        // the shipped `Enhancing` beat sees them on its first tick.
        lua_getfield(lua, -2, (std::string{key} + ":buildtime").c_str());
        if (lua_isnumber(lua, -1)) {
            lua_setfield(lua, -2, "WorkItemBuildTime");
        } else {
            lua_pop(lua, 1);
        }
    }
    lua_remove(lua, -2);
    return 1;
}

// Binds `current` around exactly one guarded `lua_pcall` so `GetUnit` /
// `SetAIResult` see the dispatching task. The writer side pins the natives
// table with `lua_absindex` before allocating the userdata: the guard enters
// with caller slots beneath (`[fn, instance]` in taskTick), so a raw `-2`
// would address the caller's slot instead of the table (`C-228` SIGABRT).
struct NativeGuard {
    lua_State* lua = nullptr;
    explicit NativeGuard(lua_State* l, rm::sim::UnitId unit,
                         rm::sim::ScriptTaskState* state) : lua(l) {
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        if (!lua_istable(lua, -1)) {
            lua_pop(lua, 1);
            lua_newtable(lua);
            lua_setfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
            lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        }
        const int table = lua_absindex(lua, -1);
        auto* native = static_cast<TaskNative*>(
            lua_newuserdata(lua, sizeof(TaskNative)));
        new (native) TaskNative{unit, state};
        lua_setfield(lua, table, "current");
        lua_pop(lua, 1);
    }
    ~NativeGuard() noexcept {
        // Runs after the pcall with a known stack, so `-2` is exact here;
        // the unwind check keeps a C++-thrown Lua error (possible if Lua
        // ever builds with `__cplusplus` instead of longjmp) from turning a
        // catchable script error into a mid-unwind API panic.
        if (std::uncaught_exceptions() > 0) return;
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        if (lua_istable(lua, -1)) {
            lua_pushnil(lua);
            lua_setfield(lua, -2, "current");
        }
        lua_pop(lua, 1);
    }
};
}  // namespace

struct ScriptHost::Impl {
    const rm::vfs::Vfs& content;
    std::unique_ptr<lua_State, decltype(&lua_close)> state;
    std::vector<std::string> errors;

    explicit Impl(const rm::vfs::Vfs& c)
        : content(c), state(luaL_newstate(), lua_close) {
        if (!state) return;
        auto* lua = state.get();
        luaL_requiref(lua, "_G", luaopen_base, 1); lua_pop(lua, 1);
        luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(lua, 1);
        luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(lua, 1);
        luaL_requiref(lua, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(lua, 1);
        for (const auto* name : {"dofile", "loadfile", "collectgarbage"}) {
            lua_pushnil(lua); lua_setglobal(lua, name);
        }
        lua_pushlightuserdata(lua, const_cast<rm::vfs::Vfs*>(&content));
        lua_pushcclosure(lua, concatSource, 1);
        lua_setglobal(lua, "__rm_read");
        if (luaL_dostring(lua, kBootstrap) != LUA_OK) {
            errors.emplace_back(toString(lua, -1));
            lua_pop(lua, 1);
        }
        lua_newtable(lua);
        // `tasks[name] = {source = "...", modulePath = "..."}` -- populated by `registerTask`.
        // `instances[key] = ref` -- cached registry references so repeated dispatches
        // (and `onDestroy` followed by `taskTick` from a stale lifetime, should that ever
        // happen) reuse one instance.
        lua_setfield(lua, LUA_REGISTRYINDEX, kHostTable);
    }
};

ScriptHost::ScriptHost(const rm::vfs::Vfs& content)
    : content_(content),
      lua_(static_cast<lua_State*>(nullptr)) {
    impl_ = std::make_unique<Impl>(content_);
    lua_ = impl_->state.get();
}

ScriptHost::~ScriptHost() = default;

void ScriptHost::registerTask(std::string_view taskName, ScriptTaskRegistration reg) {
    lua_State* lua = lua_;
    lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, kHostTable);
        lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    }
    lua_getfield(lua, -1, "tasks");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, -2, "tasks");
        lua_getfield(lua, -1, "tasks");
    }
    lua_pushlstring(lua, taskName.data(), taskName.size());
    lua_newtable(lua);
    lua_pushlstring(lua, reg.source.data(), reg.source.size());
    lua_setfield(lua, -2, "source");
    if (!reg.modulePath.empty()) {
        lua_pushlstring(lua, reg.modulePath.data(), reg.modulePath.size());
        lua_setfield(lua, -2, "modulePath");
    }
    lua_settable(lua, -3);
    lua_pop(lua, 2);
}
void ScriptHost::setActiveMods(std::vector<rm::vfs::ActiveMod> mods) {
    // `C-313` (`0x0052fece`): retail builds `__active_mods` natively from
    // `gameInfo.GameMods`, so the host owns the table and Lua reads it. Order
    // is `before`/`after` uid constraints, uid-alphabetical otherwise
    // (`mods.lua` `ModComp`). `ui_only` mods never reach the sim session —
    // the user session appends them separately — so the sim table carries
    // sim mods only. Ticks here are 100 ms (`TickRate` 10 Hz,
    // `core/sim/TickRate.hpp`); retail's 10 Hz sim clock is the same source
    // the `WaitTicks(n)` `n-1` scheduler quirk (`C-305`) is measured against.
    lua_State* lua = lua_;
    if (lua == nullptr) return;
    mods.erase(std::remove_if(mods.begin(), mods.end(),
                              [](const rm::vfs::ActiveMod& mod) { return mod.uiOnly; }),
               mods.end());
    const std::vector<rm::vfs::ActiveMod> ordered = rm::vfs::orderActiveMods(std::move(mods));
    lua_newtable(lua);
    int index = 0;
    for (const rm::vfs::ActiveMod& mod : ordered) {
        lua_newtable(lua);
        lua_pushlstring(lua, mod.uid.data(), mod.uid.size());
        lua_setfield(lua, -2, "uid");
        lua_pushlstring(lua, mod.name.data(), mod.name.size());
        lua_setfield(lua, -2, "name");
        const std::string location = rm::vfs::modMountPoint(mod);
        lua_pushlstring(lua, location.data(), location.size());
        lua_setfield(lua, -2, "location");
        const std::string hookdir = mod.hookdir.empty() ? "/hook" : mod.hookdir;
        lua_pushlstring(lua, hookdir.data(), hookdir.size());
        lua_setfield(lua, -2, "hookdir");
        lua_pushboolean(lua, mod.uiOnly);
        lua_setfield(lua, -2, "ui_only");
        lua_rawseti(lua, -2, ++index);
    }
    // `gameInfo.GameMods` is the same table — retail builds `__active_mods`
    // FROM it, so both names see the one list.
    lua_pushvalue(lua, -1);
    lua_setglobal(lua, "__active_mods");
    lua_getglobal(lua, "gameInfo");
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
    }
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "GameMods");
    lua_setglobal(lua, "gameInfo");
    lua_pop(lua, 1);
}
void ScriptHost::publishTaskFunding(rm::sim::UnitId unit, double buildTimeSeconds,
                                     double consumed) {
    lua_State* lua = lua_;
    if (lua == nullptr) return;
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    }
    const std::string key = std::to_string(unit.generation) + ":"
                            + std::to_string(unit.index);
    lua_pushnumber(lua, consumed);
    lua_setfield(lua, -2, (key + ":funding").c_str());
    lua_pushnumber(lua, buildTimeSeconds);
    lua_setfield(lua, -2, (key + ":buildtime").c_str());
    // The unit table may already exist (created by an earlier `GetUnit`);
    // mirror the build time onto it so the shipped beat reads it live.
    lua_getfield(lua, -1, key.c_str());
    if (lua_istable(lua, -1)) {
        lua_pushnumber(lua, buildTimeSeconds);
        lua_setfield(lua, -2, "WorkItemBuildTime");
    }
    lua_pop(lua, 2);
}

double ScriptHost::taskWorkProgress(rm::sim::UnitId unit) const {
    lua_State* lua = lua_;
    if (lua == nullptr) return -1.0;
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return -1.0;
    }
    const std::string key = std::to_string(unit.generation) + ":"
                            + std::to_string(unit.index);
    lua_getfield(lua, -1, key.c_str());
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 2);
        return -1.0;
    }
    lua_getfield(lua, -1, "WorkProgress");
    const double progress = lua_isnumber(lua, -1) ? lua_tonumber(lua, -1) : -1.0;
    lua_pop(lua, 3);
    return progress;
}

double ScriptHost::taskGlobalNumber(const char* name) const {
    lua_State* lua = lua_;
    if (lua == nullptr || name == nullptr) return -1.0;
    lua_getglobal(lua, name);
    const double value = lua_isnumber(lua, -1) ? lua_tonumber(lua, -1) : -1.0;
    lua_pop(lua, 1);
    return value;
}

ScriptHost::Instance ScriptHost::ensureInstance(rm::sim::UnitId unit,
                                                std::string_view task,
                                                rm::sim::ScriptTaskState& state,
                                                std::span<const std::uint8_t> commandData) {
(void)state;
    Instance out{};
    lua_State* lua = lua_;
    if (lua == nullptr) {
        return out;
    }
    const std::string key = std::to_string(unit.generation) + ":" + std::to_string(unit.index)
                          + ":" + std::string{task};

    // Cache hit: a previous `onCreate` already brought the source in. The instance stays
    // in the registry until `onDestroy` retires it.
    lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, kHostTable);
        lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    }
    lua_getfield(lua, -1, "instances");
    // Entry shape is exactly `[host]` here (balanced on every call), so the
    // `-2` below addresses `host`, not a caller slot. See `NativeGuard`.
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, -2, "instances");
        lua_getfield(lua, -1, "instances");
    }
    lua_pushlstring(lua, key.data(), key.size());
    lua_gettable(lua, -2);
    if (lua_isnumber(lua, -1)) {
        out.self = static_cast<std::uint32_t>(lua_tointeger(lua, -1));
        lua_pop(lua, 3);
        return out;
    }
    lua_pop(lua, 1);

    // First dispatch for this (unit, task). Load the source. `tasks` lives on
    // the host table (`-2`), not the instances subtable (`-1`). When the name
    // has no registration, fall through to `C-388` (`0x00629530`): sprintf
    // `/lua/sim/tasks/<TaskName>.lua` through the VFS, and on a miss load
    // `/lua/sim/ScriptTask.lua` (`ScriptTask.lua` fallback). Both branches run
    // the same `C-311` hook concat below, so only the resolved path differs.
    // `C-388`'s fallback load is silent here: the retail diagnostic (`Can't
    // find task %s, using ScriptTask directly`) is a log line, and the host's
    // `errors()` channel is reserved for hard failures.
    std::string fallbackPath;  // nonempty when the resolved path came from C-388
    lua_getfield(lua, -2, "tasks");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        fallbackPath = std::string{"/lua/sim/tasks/"} + std::string{task} + ".lua";
    } else {
        lua_pushlstring(lua, task.data(), task.size());
        lua_gettable(lua, -2);
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 2);  // registration miss, host table
            fallbackPath = std::string{"/lua/sim/tasks/"} + std::string{task} + ".lua";
        }
    }
    std::string effectiveSource;
    std::string effectiveChunkName;
    std::string resolvedPath;  // VFS path that actually supplied the chunk
    if (!fallbackPath.empty()) {
        // Both miss shapes above restored [host, instances]; re-fetch `tasks`
        // so every path below enters load/pcall with [host, instances, tasks],
        // matching the pre-existing pops after `pcall` (which drop `tasks`).
        lua_getfield(lua, -2, "tasks");
        if (!content_.read(fallbackPath)) {
            fallbackPath = "/lua/sim/tasks/ScriptTask.lua";
        }
        const auto bytes = content_.read(fallbackPath);
        if (!bytes) {
            impl_->errors.emplace_back(std::string{"unknown task: "} + std::string{task});
            lua_settop(lua, 0);
            return out;
        }
        resolvedPath = fallbackPath;
        effectiveChunkName = "@" + fallbackPath;
    } else {
        lua_getfield(lua, -1, "source");
        const std::string source = toString(lua, -1);
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "modulePath");
        const std::string modulePath = lua_isnil(lua, -1) ? std::string{} : toString(lua, -1);
        lua_pop(lua, 2);  // modulePath value, registration table; [host, instances, tasks]
        if (!modulePath.empty()) {
            resolvedPath = modulePath;
            effectiveChunkName = "@" + modulePath;
        } else {
            effectiveSource = source;
            effectiveChunkName = "@rm:registered";
        }
    }
    if (resolvedPath.empty()) {
        // Inlined-source registration: nothing to read from the VFS.
    } else {
        // `C-311` (`SCR_LuaDoFileConcat`): the base file plus every registered
        // hookdir's copy of the same path load as ONE chunk, so a hook shares
        // the original's locals and environment; order is base, then hookdirs
        // in registration order (`/schook` first, then `__active_mods`).
        const auto bytes = content_.read(resolvedPath);
        if (!bytes) {
            impl_->errors.emplace_back("missing module: " + resolvedPath);
            lua_settop(lua, 0);
            return out;
        }
        std::string concatenated{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
        for (const std::string& hookPath : content_.hooksFor(resolvedPath)) {
            if (const auto hookBytes = content_.read(hookPath)) {
                concatenated.push_back('\n');
                concatenated.append(reinterpret_cast<const char*>(hookBytes->data()),
                                    hookBytes->size());
            }
        }
        effectiveSource = rm::ai::rewriteLegacyLua(concatenated);
    }
    if (effectiveSource.empty()) {
        impl_->errors.emplace_back("registration has no source nor modulePath");
        lua_settop(lua, 0);
        return out;
    }
    // Run the chunk. Lua 5.4's default chunk env is `_G`, so `TypeClass = ...` lands
    // there and `lua_getglobal` will find it below. Shipped task files use
    // `import(...)` for their own bases (`EnhanceTask.lua` imports
    // `ScriptTask.lua`); the bootstrap publishes that global, so no per-chunk
    // module env is needed here.
    if (luaL_loadbuffer(lua, effectiveSource.data(), effectiveSource.size(),
                        effectiveChunkName.c_str()) != LUA_OK) {
        impl_->errors.emplace_back("load failed: " + toString(lua, -1));
        lua_settop(lua, 0);
        return out;
    }
    if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
        impl_->errors.emplace_back("chunk error: " + toString(lua, -1));
        lua_settop(lua, 0);
        return out;
    }
    lua_settop(lua, 0);  // drop host/instances/tasks; construction starts clean

    // Most task chunks declare their own `TypeClass`, which the host reads
    // back here. Real `EnhanceTask.lua` does NOT — it declares `EnhanceTask`
    // and leaves `TypeClass` to the retail loader's `TypeClass = EnhanceTask`
    // postfix (`C-388`). Fall back to the `import` return: the task chunk ran
    // in a module env, so its last `import('/lua/sim/ScriptTask.lua')` return
    // is long gone — instead re-import the task path and read the class the
    // chunk's globals named (the chunk mirrored them onto `_G`, so the global
    // matching the task name is the class).
    lua_getglobal(lua, "TypeClass");
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_getglobal(lua, std::string{task}.c_str());
    }
    if (!lua_istable(lua, -1)) {
        // The chunk ran but bound neither `TypeClass` nor the task-named
        // global — report what `_G` has so the test can debug without
        // attaching a debugger.
        std::string details = "source does not declare TypeClass";
        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 1);
            lua_getglobal(lua, "Class");
            if (lua_isnil(lua, -1)) {
                details += "; _G.Class also nil (bootstrap not loaded?)";
            }
            lua_pop(lua, 1);
        } else {
            details += " (got " + std::string{lua_typename(lua, lua_type(lua, -1))} + ")";
            lua_pop(lua, 1);
        }
        impl_->errors.emplace_back(details);
        return out;
    }
    // The class table carries the four `CUnitScriptTask` task natives so the
    // instance inherits them through `__index` before `OnCreate` runs: retail's
    // `OnCreate` already calls `self:GetUnit()` on its first beat.
    lua_pushcfunction(lua, &taskGetUnitFn);
    lua_setfield(lua, -2, "GetUnit");
    lua_pushcfunction(lua, &taskSetAiResultFn);
    lua_setfield(lua, -2, "SetAIResult");
    lua_pushcfunction(lua, &taskNoopFn);
    lua_setfield(lua, -2, "LUnitMove");
    lua_pushcfunction(lua, &taskNoopFn);
    lua_setfield(lua, -2, "LUnitMoveNear");
    lua_pushcfunction(lua, &taskSecondsPerTickFn);
    lua_setglobal(lua, "SecondsPerTick");
    // Use the bootstrap's `__rm_construct` to call the class as a constructor (which fires
    // OnPreCreate + OnCreate) and rebind the four lifecycle hooks on the returned instance.
    lua_getglobal(lua, "__rm_construct");
    if (!lua_isfunction(lua, -1)) {
        impl_->errors.emplace_back("__rm_construct missing");
        lua_pop(lua, 2);
        return out;
    }
    lua_insert(lua, -2);  // __rm_construct, TypeClass
    // `C-228` (`ScriptTask.lua:61-63`, `EnhanceTask.lua:28,40,76`): retail's
    // N->L `OnCreate(commandData:TABLE)` stores the order table as
    // `self.CommandData`. The native intake carries the order table's bytes
    // (live producer `Match.cpp:1147`, decoded as `std::string(data.begin(),`
    // `data.end())` in `Enhancement.cpp:588`); mirror it here so
    // `self.CommandData.Enhancement` names the work.
    lua_newtable(lua);
    if (!commandData.empty()) {
        lua_pushlstring(lua, reinterpret_cast<const char*>(commandData.data()), commandData.size());
        lua_setfield(lua, -2, "Enhancement");
    }
    {
        // `OnPreCreate`/`OnCreate` already call `self:GetUnit()`, so the
        // native context must be live across construction, not only after.
        NativeGuard preGuard{lua, unit, &state};
        if (lua_pcall(lua, 2, 1, 0) != LUA_OK) {
            impl_->errors.emplace_back("construct failed: " + toString(lua, -1));
            lua_pop(lua, 1);
            return out;
        }
    }
    if (!lua_istable(lua, -1)) {
        impl_->errors.emplace_back("class call did not return an instance");
        lua_pop(lua, 1);
        return out;
    }
    pushTaskNatives(unit, key, &state);
    const int ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    out.self = static_cast<std::uint32_t>(ref);
    clearTaskNatives();

    // Cache the reference keyed by (unit, task) for a later dispatch to reuse.
    lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    lua_getfield(lua, -1, "instances");
    lua_pushlstring(lua, key.data(), key.size());
    lua_pushinteger(lua, static_cast<lua_Integer>(ref));
    lua_settable(lua, -3);
    lua_pop(lua, 2);
    return out;
}
void ScriptHost::pushTaskNatives(rm::sim::UnitId unit, const std::string& taskKey,
                                 rm::sim::ScriptTaskState* state) {
    // Records the dispatch's native context and stamps the four `CUnitScriptTask`
    // bindings onto the fresh instance table on top of the stack.
    // (`0x0062a1b0`… `0x0062a040`; latent pair dead per `C-437`.)
    (void)taskKey;
    lua_State* lua = lua_;
    if (lua == nullptr) {
        return;
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    }
    const int table = lua_absindex(lua, -1);
    auto* native = static_cast<TaskNative*>(
        lua_newuserdata(lua, sizeof(TaskNative)));
    new (native) TaskNative{unit, state};
    lua_setfield(lua, table, "current");
    lua_pop(lua, 1);
    // Stack: [instance]. Install the task natives directly on it.
    lua_pushcfunction(lua, &taskGetUnitFn);
    lua_setfield(lua, -2, "GetUnit");
    lua_pushcfunction(lua, &taskSetAiResultFn);
    lua_setfield(lua, -2, "SetAIResult");
    lua_pushcfunction(lua, &taskNoopFn);
    lua_setfield(lua, -2, "LUnitMove");
    lua_pushcfunction(lua, &taskNoopFn);
    lua_setfield(lua, -2, "LUnitMoveNear");
}

void ScriptHost::clearTaskNatives() {
    lua_State* lua = lua_;
    if (lua == nullptr) {
        return;
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (lua_istable(lua, -1)) {
        lua_pushnil(lua);
        lua_setfield(lua, -2, "current");
    }
    lua_pop(lua, 1);
}

void ScriptHost::bindTaskNatives(rm::sim::UnitId unit, const std::string& taskKey,
                                 rm::sim::ScriptTaskState* state) {
    // `OnDestroy`-path binder: records the native context WITHOUT pushing
    // caller-visible slots. The destroy dispatch enters with `[instance]`
    // and pops it right after the pcall; a full `pushTaskNatives` would
    // interleave table/userdata slots the frame's pops do not expect.
    (void)taskKey;
    lua_State* lua = lua_;
    if (lua == nullptr) {
        return;
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
        lua_getfield(lua, LUA_REGISTRYINDEX, kTaskNativesKey);
    }
    const int table = lua_absindex(lua, -1);
    auto* native = static_cast<TaskNative*>(
        lua_newuserdata(lua, sizeof(TaskNative)));
    new (native) TaskNative{unit, state};
    lua_setfield(lua, table, "current");
    lua_pop(lua, 1);
}

void ScriptHost::onPreCreate(rm::sim::UnitId, std::string_view,
                             std::span<const std::uint8_t>,
                             rm::sim::ScriptTaskState&) noexcept {
    // No-op: Lua OnPreCreate fires inside `ensureInstance`, which `onCreate` immediately
    // schedules. The engine-side distinction (pre vs post native setup) has no analog here.
}

void ScriptHost::onCreate(rm::sim::UnitId unit, std::string_view task,
                          std::span<const std::uint8_t> commandData,
                          rm::sim::ScriptTaskState& state) {
    (void)ensureInstance(unit, task, state, commandData);
}

std::int32_t ScriptHost::taskTick(rm::sim::UnitId unit, std::string_view task,
                                   std::span<const std::uint8_t> commandData,
                                   rm::sim::ScriptTaskState& state) {
    const Instance instance = ensureInstance(unit, task, state, commandData);
    lua_State* lua = lua_;
    if (lua == nullptr || instance.self == 0) {
        return static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done);
    }
    lua_rawgeti(lua, LUA_REGISTRYINDEX, static_cast<lua_Integer>(instance.self));
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done);
    }
    // The instance's `TaskTick` resolves through its metatable — the class, or
    // the current state after `ChangeState` swaps it (`C-318`). A cached copy
    // would freeze dispatch on the creation-time state, so every beat re-reads
    // the live field. `__rm_hooks.tick` is only the no-state fallback (kept
    lua_getfield(lua, -1, "TaskTick");
    if (!lua_isfunction(lua, -1)) {
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "__rm_hooks");
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "tick");
            lua_remove(lua, -2);  // drop __rm_hooks; leave tick
        } else {
            lua_pop(lua, 1);
            lua_getfield(lua, -1, "TaskTick");
        }
    }
    if (!lua_isfunction(lua, -1)) {
        // No TaskTick declared: by retail's contract the default is "repeat next beat",
        // which `ScriptTaskStatus::NextBeat` (= 1) encodes. Tests can pin their own.
        if (!lua_isnil(lua, -1)) {
            const int t = lua_type(lua, -1);
            lua_pop(lua, 1);
            impl_->errors.emplace_back(
                "TaskTick is " + std::string{lua_typename(lua, t)} + ", not function");
        } else {
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);  // instance
        return 1;
    }
    // Stack: `[instance, fn]`. Rotate so the function is below its argument: `[fn, instance]`.
    // pcall with nargs=1 then sees fn at `-nargs - 1 = -2` and instance at `-1`.
    lua_insert(lua, -2);
    {
        NativeGuard guard{lua, unit, &state};
        if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
            impl_->errors.emplace_back("TaskTick error: " + toString(lua, -1));
            lua_pop(lua, 1);  // exactly the error object; the arg was consumed
            return static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done);
        }
    }
    // `lua_pcall(nargs=1, nresults=1)` leaves exactly `[result]`: the callee
    // and its argument are consumed. Popping 2 here underflows the frame and,
    // when `tbclist` happens to sit above the new top, `lua_settop` runs
    // `luaF_close` over garbage and panics (`C-228` SIGABRT).
    const std::int32_t result = lua_isnumber(lua, -1) ? static_cast<std::int32_t>(lua_tointeger(lua, -1)) : 1;
    lua_pop(lua, 1);
    return result;
}

void ScriptHost::onDestroy(rm::sim::UnitId unit, std::string_view task,
                           std::span<const std::uint8_t> commandData,
                           rm::sim::ScriptTaskState& state) {
    const Instance instance = ensureInstance(unit, task, state, commandData);
    lua_State* lua = lua_;
    const std::string key = std::to_string(unit.generation) + ":" + std::to_string(unit.index)
                          + ":" + std::string{task};

    if (lua != nullptr && instance.self != 0) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, static_cast<lua_Integer>(instance.self));
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "__rm_hooks");
            if (lua_istable(lua, -1)) {
                lua_getfield(lua, -1, "destroy");
                lua_remove(lua, -2);  // drop __rm_hooks; leave destroy
            } else {
                lua_pop(lua, 1);
                lua_getfield(lua, -1, "OnDestroy");
            }
            if (lua_isfunction(lua, -1)) {
                lua_insert(lua, -2);
                bindTaskNatives(unit, std::string{task}, &state);
                if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
                    impl_->errors.emplace_back("OnDestroy error: " + toString(lua, -1));
                    lua_pop(lua, 1);
                }
                clearTaskNatives();
            } else {
                lua_pop(lua, 1);
            }
        } else {
            lua_pop(lua, 1);
        }
        luaL_unref(lua, LUA_REGISTRYINDEX, static_cast<int>(instance.self));
    }

    // Drop the cached (key, ref) entry so a fresh task on this unit re-enters via
    // OnPreCreate/OnCreate rather than dispatching against the destroyed table.
    if (lua != nullptr) {
        lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "instances");
            if (lua_istable(lua, -1)) {
                lua_pushlstring(lua, key.data(), key.size());
                lua_pushnil(lua);
                lua_settable(lua, -3);
            }
            lua_pop(lua, 2);
        } else {
            lua_pop(lua, 1);
        }
    }
}

const std::vector<std::string>& ScriptHost::errors() const noexcept {
    if (impl_ != nullptr) {
        return impl_->errors;
    }
    static const std::vector<std::string> empty;
    return empty;
}

} // namespace rm::app
