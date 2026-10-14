#include "app/ScriptHost.hpp"

#include "app/FafAi.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <memory>
#include <string>
#include <utility>

namespace rm::app {
namespace {

/// Bootstrap. `Class(bases)` returns a class table; `__rm_construct(classTable)` returns
/// an instance table that records the four lifecycle hooks as fields. Both halves keep
/// out of the engine-side metatable so a class hierarchy does not chain through them.
constexpr const char* kBootstrap = R"lua(
function Class(...)
    local bases = {...}
    return function(fields)
        fields = fields or {}
        setmetatable(fields, {
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
State = Class

local modules = {}
local function importModule(path)
    path = string.lower(path)
    if path:sub(1,1) ~= '/' then path = '/lua/' .. path end
    if modules[path] then return modules[path] end
    local source = __rm_read(path)
    if not source then error('missing script module: '..path) end
    local env = setmetatable({}, {__index = _G})
    env.import = importModule
    modules[path] = env
    local chunk, why = load(source, '@'..path, 't', env)
    if not chunk then modules[path] = nil; error(why) end
    local ok, why = pcall(chunk)
    if not ok then modules[path] = nil; error(why) end
    return env
end
import = importModule

-- The host instantiates one class table per (unit, task); the constructor invokes the
-- two creation hooks (OnPreCreate, OnCreate) and stores the lifecycle functions so the
-- dispatcher can invoke them per beat.
function __rm_construct(klass)
    local self = klass()
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

int readSource(lua_State* lua) {
    auto* content = static_cast<const rm::vfs::Vfs*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const char* path = lua_tostring(lua, 1);
    if (path == nullptr) { lua_pushnil(lua); return 1; }
    const auto bytes = content->read(path);
    if (!bytes) { lua_pushnil(lua); return 1; }
    const std::string translated =
        rm::ai::rewriteLegacyLua({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    lua_pushlstring(lua, translated.data(), translated.size());
    return 1;
}

} // namespace

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
        lua_pushcclosure(lua, readSource, 1);
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

ScriptHost::Instance ScriptHost::ensureInstance(rm::sim::UnitId unit,
                                                std::string_view task,
                                                rm::sim::ScriptTaskState& state,
                                                std::span<const std::uint8_t> commandData) {
    (void)state; (void)commandData;
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

    // First dispatch for this (unit, task). Load the source.
    // `tasks` lives on the host table (`-2`), not the instances subtable (`-1`).
    lua_getfield(lua, -2, "tasks");
    if (lua_isnil(lua, -1)) {
        impl_->errors.emplace_back(std::string{"unknown task: "} + std::string{task});
        lua_pop(lua, 3);
        return out;
    }
    lua_pushlstring(lua, task.data(), task.size());
    lua_gettable(lua, -2);
    if (lua_isnil(lua, -1)) {
        impl_->errors.emplace_back(std::string{"unknown task: "} + std::string{task});
        lua_pop(lua, 4);
        return out;
    }
    lua_getfield(lua, -1, "source");
    const std::string source = toString(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, -1, "modulePath");
    const std::string modulePath = lua_isnil(lua, -1) ? std::string{} : toString(lua, -1);
    lua_pop(lua, 2);  // modulePath, registration table

    // Source resolution: a VFS-backed module path reads through `content` (lets a test
    // ship a real `<bpid>_script.lua` file from disk); an empty modulePath runs the
    // inlined `source`.
    std::string effectiveSource = source;
    std::string effectiveChunkName = "@rm:registered";
    if (!modulePath.empty()) {
        // `C-311` (`SCR_LuaDoFileConcat`): the base file plus every registered
        // hookdir's copy of the same path load as ONE chunk, so a hook shares
        // the original's locals and environment; order is base, then hookdirs
        // in registration order (`/schook` first, then `__active_mods`).
        const auto bytes = content_.read(modulePath);
        if (!bytes) {
            impl_->errors.emplace_back("missing module: " + modulePath);
            lua_pop(lua, 2);
            return out;
        }
        std::string concatenated{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
        for (const std::string& hookPath : content_.hooksFor(modulePath)) {
            if (const auto hookBytes = content_.read(hookPath)) {
                concatenated.push_back('\n');
                concatenated.append(reinterpret_cast<const char*>(hookBytes->data()),
                                    hookBytes->size());
            }
        }
        effectiveSource = rm::ai::rewriteLegacyLua(concatenated);
        effectiveChunkName = "@" + modulePath;
    }
    if (effectiveSource.empty()) {
        impl_->errors.emplace_back("registration has no source nor modulePath");
        lua_pop(lua, 1);
        return out;
    }
    if (luaL_loadbuffer(lua, effectiveSource.data(), effectiveSource.size(),
                        effectiveChunkName.c_str()) != LUA_OK) {
        impl_->errors.emplace_back("load failed: " + toString(lua, -1));
        lua_pop(lua, 2);
        return out;
    }
    // Run the chunk. Lua 5.4's default chunk env is `_G`, so `TypeClass = ...` lands
    // there and `lua_getglobal` will find it below.
    if (lua_pcall(lua, 0, 0, 0) != LUA_OK) {
        impl_->errors.emplace_back("chunk error: " + toString(lua, -1));
        lua_pop(lua, 2);
        return out;
    }
    lua_pop(lua, 1);  // host table

    lua_getglobal(lua, "TypeClass");
    if (!lua_istable(lua, -1)) {
        // The chunk ran but did not bind `TypeClass` to a table — report what `_G` has
        // so the test can debug without attaching a debugger.
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

    // Use the bootstrap's `__rm_construct` to call the class as a constructor (which fires
    // OnPreCreate + OnCreate) and rebind the four lifecycle hooks on the returned instance.
    lua_getglobal(lua, "__rm_construct");
    if (!lua_isfunction(lua, -1)) {
        impl_->errors.emplace_back("__rm_construct missing");
        lua_pop(lua, 2);
        return out;
    }
    lua_insert(lua, -2);  // __rm_construct, TypeClass
    if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
        impl_->errors.emplace_back("construct failed: " + toString(lua, -1));
        lua_pop(lua, 1);
        return out;
    }
    if (!lua_istable(lua, -1)) {
        impl_->errors.emplace_back("class call did not return an instance");
        lua_pop(lua, 1);
        return out;
    }

    const int ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    out.self = static_cast<std::uint32_t>(ref);

    // Cache the reference keyed by (unit, task) for a later dispatch to reuse.
    lua_getfield(lua, LUA_REGISTRYINDEX, kHostTable);
    lua_getfield(lua, -1, "instances");
    lua_pushlstring(lua, key.data(), key.size());
    lua_pushinteger(lua, static_cast<lua_Integer>(ref));
    lua_settable(lua, -3);
    lua_pop(lua, 2);
    return out;
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
    // Prefer `__rm_hooks.tick` (set by `__rm_construct`) over the per-instance raw field;
    // the per-instance raw field hits the metatable's `__index`, which can produce a
    // table value instead of a function value when the class table holds the function.
    lua_getfield(lua, -1, "__rm_hooks");
    if (lua_istable(lua, -1)) {
        lua_getfield(lua, -1, "tick");
        lua_remove(lua, -2);  // drop __rm_hooks; leave tick
    } else {
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "TaskTick");
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
    if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
        impl_->errors.emplace_back("TaskTick error: " + toString(lua, -1));
        lua_pop(lua, 2);
        return static_cast<std::int32_t>(rm::sim::ScriptTaskStatus::Done);
    }
    const std::int32_t result = lua_isnumber(lua, -1) ? static_cast<std::int32_t>(lua_tointeger(lua, -1)) : 1;
    lua_pop(lua, 2);
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
                // Stack: [instance, fn]. Rotate so pcall with nargs=1 sees fn at -2.
                lua_insert(lua, -2);
                if (lua_pcall(lua, 1, 0, 0) != LUA_OK) {
                    impl_->errors.emplace_back("OnDestroy error: " + toString(lua, -1));
                    lua_pop(lua, 1);
                }
            } else {
                lua_pop(lua, 1);
            }
        }
        lua_pop(lua, 1);  // instance
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
