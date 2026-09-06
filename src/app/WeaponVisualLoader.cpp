#include "core/scene/WeaponVisuals.hpp"
#include "app/FafAi.hpp"
#include "core/unit/UnitBlueprint.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <memory>
#include <set>
#include <cctype>

namespace rm {
namespace {
struct VisualHost { const vfs::Vfs& content; int fuel = 0; };

int readSource(lua_State* state) {
    auto* host = static_cast<VisualHost*>(lua_touserdata(state, lua_upvalueindex(1)));
    const char* path = lua_tostring(state, 1);
    if (!path) { lua_pushnil(state); return 1; }
    const auto bytes = host->content.read(path);
    if (!bytes) { lua_pushnil(state); return 1; }
    const auto source = ai::rewriteLegacyLua({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    lua_pushlstring(state, source.data(), source.size());
    return 1;
}
void budget(lua_State* state, lua_Debug*) {
    auto* host = *static_cast<VisualHost**>(lua_getextraspace(state));
    if (--host->fuel <= 0) luaL_error(state, "visual script instruction budget exhausted");
}

// Only declarations run. Native simulation base classes have no visual data; constructing
// an instance or invoking an engine API here is an error, never a simulated success.
constexpr const char* bootstrap = R"lua(
function Class(...)
    local bases = {...}
    return function(fields)
        return setmetatable(fields, {
            __index = function(_, key)
                for _, base in ipairs(bases) do
                    local value = base[key]
                    if value ~= nil then return value end
                end
            end,
            __call = function() error('visual loader cannot instantiate simulation classes') end,
        })
    end
end
State = Class
function TableCat(...)
    local out = {}
    for _, t in ipairs({...}) do for _, value in ipairs(t) do out[#out+1] = value end end
    return out
end
function __rm_iter(value, ...)
    if type(value) == 'table' then return next, value, nil end
    return value, ...
end
function Vector(x,y,z) return {x,y,z} end
unpack = table.unpack
table.getn = function(t) return #t end
math.mod = math.fmod
string.gfind = string.gmatch
moho = setmetatable({}, {__index=function(t,k)
    local methods = {}; rawset(t,k,methods); return methods
end})
local native = {
    ['/lua/sim/entity.lua']=true, ['/lua/sim/weapon.lua']=true,
    ['/lua/sim/unit.lua']=true,
    ['/lua/defaultunits.lua']=true, ['/lua/terranunits.lua']=true,
    ['/lua/aeonunits.lua']=true, ['/lua/cybranunits.lua']=true,
    ['/lua/seraphimunits.lua']=true,
}
local modules = {}
local function importModule(path)
    path = string.lower(path)
    if path:sub(1,1) ~= '/' then path = '/lua/' .. path end
    if modules[path] then return modules[path] end
    if native[path] then
        local shell = setmetatable({}, {__index=function(t,k)
            local base = Class(){}; rawset(t,k,base); return base
        end})
        modules[path] = shell
        return shell
    end
    local source = __readVisualSource(path)
    if not source then error('missing visual module: '..path) end
    local env = setmetatable({}, {__index=_G})
    modules[path] = env
    env.import = importModule
    local chunk, why = load(source, '@'..path, 't', env)
    if not chunk then modules[path]=nil; error(why) end
    local ok, why = pcall(chunk)
    if not ok then modules[path]=nil; error(why) end
    return env
end
import = importModule
function visualEmitters(path, label, effect)
    local module = import(path)
    local class = module.TypeClass
    if not class then error('no TypeClass in '..path) end
    local out = {}
    local scale = 1
    local function add(value)
        if type(value)=='string' and value ~= '' then out[#out+1]=value
        elseif type(value)=='table' then for _, v in ipairs(value) do add(v) end end
    end
    if label then
        local weapon = class.Weapons and class.Weapons[label]
        local beam = weapon and weapon.BeamType
        if effect then
            local source = effect == 'FxMuzzleFlash' and weapon or beam
            if source then
                add(source[effect])
                local suffix = effect:match('^FxImpact(.+)$')
                scale = suffix and source['Fx'..suffix..'HitScale'] or source.FxMuzzleFlashScale or 1
            end
        elseif beam then add(beam.FxBeam) end
    elseif effect then
        add(class[effect])
        local suffix = effect:match('^FxImpact(.+)$')
        if suffix then scale = class['Fx'..suffix..'HitScale'] or 1 end
    else
        add(class.BeamName); add(class.PolyTrail); add(class.PolyTrails); add(class.FxTrails)
    end
    return out, scale
end
)lua";
}

WeaponVisuals loadWeaponVisuals(const vfs::Vfs& content) {
    WeaponVisuals result;
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    if (!state) { result.unavailable.push_back("cannot create visual Lua state"); return result; }
    auto* lua = state.get();
    VisualHost host{content};
    *static_cast<VisualHost**>(lua_getextraspace(lua)) = &host;
    luaL_requiref(lua, "_G", luaopen_base, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(lua, 1);
    for (const auto* name : {"dofile", "loadfile", "collectgarbage"}) {
        lua_pushnil(lua); lua_setglobal(lua, name);
    }
    lua_pushlightuserdata(lua, &host);
    lua_pushcclosure(lua, readSource, 1); lua_setglobal(lua, "__readVisualSource");
    if (luaL_dostring(lua, bootstrap) != LUA_OK) {
        result.unavailable.emplace_back(lua_tostring(lua,-1)); return result;
    }
    std::set<std::string> failedScripts;
    const auto resolve = [&](const std::string& path, const std::string& key, const char* label,
                             const char* effect = nullptr) {
        if (failedScripts.contains(path)) return;
        host.fuel = 20000; // Twenty million VM instructions per query, including imports.
        lua_sethook(lua, budget, LUA_MASKCOUNT, 1000);
        lua_getglobal(lua, "visualEmitters");
        lua_pushstring(lua, path.c_str());
        if (label) lua_pushstring(lua, label); else lua_pushnil(lua);
        if (effect) lua_pushstring(lua, effect); else lua_pushnil(lua);
        if (lua_pcall(lua, 3, 2, 0) != LUA_OK) {
            failedScripts.insert(path);
            // Content may raise a non-string error value; luaL_tolstring pushes a printable copy.
            result.unavailable.push_back(key + ": " + luaL_tolstring(lua,-1,nullptr));
            lua_pop(lua,2); return;
        }
        std::vector<std::string> emitters;
        const float scale = static_cast<float>(lua_tonumber(lua,-1));
        lua_pop(lua,1);
        const auto size = lua_rawlen(lua,-1);
        for (std::size_t i=1; i<=size; ++i) {
            lua_rawgeti(lua,-1,static_cast<lua_Integer>(i));
            if (lua_isstring(lua,-1)) emitters.emplace_back(lua_tostring(lua,-1));
            lua_pop(lua,1);
        }
        lua_pop(lua,1);
        loadWeaponMaterials(result, content, key, emitters);
        auto normalized = key;
        for (auto& c : normalized) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        result.scales[normalized] = scale;
        // An explicitly empty Lua effect list means no flash, not a generic replacement.
        if (effect && emitters.empty()) result.definitions.try_emplace(normalized);
    };
    for (const auto& path : content.list("/projectiles", "_script.lua")) {
        const auto key = path.substr(0, path.size()-11) + "_proj.bp";
        resolve(path,key,nullptr);
        for (const auto* effect : {"FxImpactLand", "FxImpactWater", "FxImpactNone",
             "FxImpactUnderWater", "FxImpactProjectile", "FxImpactProjectileUnderWater",
             "FxImpactProp", "FxImpactShield", "FxImpactUnit", "FxImpactAirUnit"})
            resolve(path, key + "#" + effect, nullptr, effect);
    }
    for (const auto& path : content.list("/units", "_unit.bp")) {
        const auto bytes = content.read(path);
        if (!bytes) continue;
        const auto def = unitbp::load({reinterpret_cast<const char*>(bytes->data()),bytes->size()}, path);
        if (!def) continue;
        for (const auto& weapon : def->weapons) {
            const auto script = path.substr(0,path.size()-8) + "_script.lua";
            const auto key = def->name + ":" + weapon.label;
            if (weapon.beam) resolve(script, key, weapon.label.c_str());
            if (weapon.beam) {
                for (const auto* effect : {"FxImpactLand", "FxImpactWater", "FxImpactNone",
                     "FxImpactUnderWater", "FxImpactProjectile", "FxImpactProjectileUnderWater",
                     "FxImpactProp", "FxImpactShield", "FxImpactUnit", "FxImpactAirUnit"})
                    resolve(script, key + "#" + effect, weapon.label.c_str(), effect);
            }
            resolve(script, key + "#FxMuzzleFlash", weapon.label.c_str(), "FxMuzzleFlash");
        }
    }
    return result;
}
} // namespace rm
