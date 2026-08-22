#include "app/FafOpponent.hpp"

#include "app/Match.hpp"
#include "app/Scene.hpp"

#include "core/sim/BuildOrder.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>

namespace rm::ai {
namespace {

// --- The driver -----------------------------------------------------------------------------
//
// The Lua half of the opponent: brains, snapshots, and the builder walk. Everything below
// runs inside the same sandbox the corpus loaded into, so `Builders`, `BuilderGroups`,
// `BaseBuilderTemplates`, `PlatoonTemplates`, `categories` and `import` are the corpus's own.
//
// THE HONESTY RULE, one layer up from the binding stubs: a condition that asks the brain for
// a method the adapter has not implemented FAILS CLOSED — the builder simply never fires —
// and the miss is counted by name. `__rm_faf_missing()` is that ledger, and the sanity
// report prints it, so "which brain method next" is always answered by data.
//
// EMBEDDED rather than shipped as a data file: the driver is part of the adapter, versioned
// with the C++ that marshals for it, and a test must never run against a stale copy on disk.
constexpr const char* kFafDriver = R"lua(
if __rm_faf == nil then

__rm_faf = { brains = {}, missing = {}, condErrors = {}, cats = {}, scenario = false }

local factionNames = { 'UEF', 'Aeon', 'Cybran', 'Seraphim' }

-- One category set per blueprint id, built from the tag list C++ sends once per type.
-- In Moho every unit id is itself a category, so the id joins its own set.
function __rm_faf_type(bp, tags)
    if __rm_faf.cats[bp] then return end
    local set = {}
    for _, tag in ipairs(tags) do set[string.upper(tag)] = true end
    set[string.upper(bp)] = true
    __rm_faf.cats[bp] = set
end

-- The map's own markers, installed where the corpus looks for them —
-- `Scenario.MasterChain._MASTERCHAIN_.Markers` — and then ANNOUNCED the way Moho announces
-- them: MarkerUtilities pre-seeds its Mass/Hydrocarbon caches EMPTY and fills them from a
-- hook it wraps around `CreateResourceDeposit`, which the engine calls once per deposit at
-- map load. Installing the table without making those calls leaves every mass-marker query
-- answering zero, which switched off the corpus's whole expansion economy.
function __rm_faf_scenario(markers, sizeX, sizeZ, armies)
    if __rm_faf.scenario then return end
    __rm_faf.scenario = true
    local t = {}
    for i, m in ipairs(markers) do
        t[m.name or ('Marker' .. i)] = { type = m.type, position = { m.x, m.y, m.z } }
    end
    Scenario = Scenario or {}
    Scenario.MasterChain = { _MASTERCHAIN_ = { Markers = t } }
    ScenarioInfo.size = { sizeX, sizeZ }
    ScenarioInfo.MapData = { PlayableRect = { 0, 0, sizeX, sizeZ } }

    -- What Setup() leans on. Army names are the map's own ARMY_n convention; the heights
    -- are flat zeros until a heightfield binding exists — markers carry their own y, and
    -- nothing here reads the terrain for anything but a land/water guess.
    ListArmies = function()
        local names = {}
        for i = 1, (armies or 0) do names[i] = 'ARMY_' .. i end
        return names
    end
    GetTerrainHeight = function() return 0 end
    GetSurfaceHeight = function() return 0 end
    -- The base the Setup() hook wraps. The engine's own would spawn a deposit prop; this
    -- sim owns deposits already, so the base is a no-op and the HOOK is the point.
    CreateResourceDeposit = CreateResourceDeposit or function() end

    import('/lua/sim/markerutilities.lua').Setup()
    for _, m in ipairs(markers) do
        if m.type == 'Mass' or m.type == 'Hydrocarbon' then
            CreateResourceDeposit(m.type, m.x, m.y or 0, m.z, 1)
        end
    end
end

-- Unit objects in snapshots are plain tables; the methods conditions call on them live on
-- this shared metatable. A snapshot only ever holds LIVING units, which is why
-- BeenDestroyed is false and completion is 1 — both facts, not guesses.
__rm_faf.unitMeta = {
    __index = {
        BeenDestroyed = function() return false end,
        GetAIBrain = function(u) return u.__brain end,
        IsBeingBuilt = function() return false end,
        GetFractionComplete = function() return 1 end,
        GetPosition = function(u) return { u.x, 0, u.z } end,
        IsUnitState = function() return false end,
    },
}

-- Moho's economy family, in BOTH spellings the corpus uses: free globals, and entries in
-- `moho.aibrain_methods` — the condition files do `local GetEconomyIncome =
-- moho.aibrain_methods.GetEconomyIncome` at IMPORT, capturing whatever sits there forever.
-- That is why this driver must install BEFORE the entry points load: a captured stub is
-- nil-into-arithmetic for the rest of the match. Requested maps to usage, the closest
-- thing the sim meters.
function GetEconomyIncome(brain, kind) return brain:GetEconomyIncome(kind) end
function GetEconomyRequested(brain, kind) return brain:GetEconomyUsage(kind) end
function GetEconomyStored(brain, kind) return brain:GetEconomyStored(kind) end
function GetEconomyTrend(brain, kind) return brain:GetEconomyTrend(kind) end
function GetEconomyStoredRatio(brain, kind) return brain:GetEconomyStoredRatio(kind) end
moho.aibrain_methods.GetEconomyIncome = GetEconomyIncome
moho.aibrain_methods.GetEconomyRequested = GetEconomyRequested
moho.aibrain_methods.GetEconomyStored = GetEconomyStored
moho.aibrain_methods.GetEconomyTrend = GetEconomyTrend
moho.aibrain_methods.GetEconomyStoredRatio = GetEconomyStoredRatio

-- The army-wide cap family, free globals in Moho. The cap is FAF's default lobby cap; the
-- cost total is a headcount until unit cap-costs are parsed.
function GetArmyUnitCostTotal(armyIndex)
    local brain = __rm_faf.brains[armyIndex - 1]
    return brain and #brain.snap.units or 0
end
function GetArmyUnitCap() return 1000 end

-- --- The brain: what a condition may ask ---------------------------------------------------
--
-- Methods close over the snapshot the C++ side refreshes before every decision pass.
-- Economy numbers are PER TICK, which is what FAF's own GetEconomyIncome returns — the
-- condition thresholds in the corpus (0.8 mass, 10 energy) only make sense at that scale.

local function recordMissing(name)
    __rm_faf.missing[name] = (__rm_faf.missing[name] or 0) + 1
end

local brainMeta = {
    __index = function(brain, key)
        recordMissing(key)
        return nil  -- the caller's pcall turns this into a failed condition
    end,
}

local methods = {}
function methods:GetArmyIndex() return self.army + 1 end
function methods:GetFactionIndex() return self.faction end
function methods:GetArmyStartPos() return self.startX, self.startZ end
function methods:GetEconomyStored(kind)
    if kind == 'MASS' then return self.snap.mass end
    return self.snap.energy
end
function methods:GetEconomyStoredRatio(kind)
    if kind == 'MASS' then
        return self.snap.massStorage > 0 and self.snap.mass / self.snap.massStorage or 0
    end
    return self.snap.energyStorage > 0 and self.snap.energy / self.snap.energyStorage or 0
end
function methods:GetEconomyIncome(kind)
    if kind == 'MASS' then return self.snap.massIncome end
    return self.snap.energyIncome
end
function methods:GetEconomyUsage(kind)
    if kind == 'MASS' then return self.snap.massUsage end
    return self.snap.energyUsage
end
function methods:GetEconomyTrend(kind)
    return self:GetEconomyIncome(kind) - self:GetEconomyUsage(kind)
end
function methods:GetCurrentUnits(category)
    return EntityCategoryCount(category, self.snap.units)
end
function methods:GetListOfUnits(category, needToBeIdle)
    local out = {}
    for _, u in ipairs(self.snap.units) do
        if EntityCategoryContains(category, u) and (not needToBeIdle or u.idle) then
            table.insert(out, u)
        end
    end
    return out
end
function methods:GetUnitsAroundPoint(category, position, radius)
    local out = {}
    for _, u in ipairs(self.snap.units) do
        if EntityCategoryContains(category, u)
            and VDist2(u.x, u.z, position[1], position[3]) <= radius then
            table.insert(out, u)
        end
    end
    return out
end
-- Whether the footprint at `position` is free. Judged against every army's standing
-- structures — the one cross-army fact the snapshot carries — because "is this mass
-- deposit taken" is exactly the question this answers for the corpus's mex logic.
function methods:CanBuildStructureAt(bp, position)
    for _, s in ipairs(self.snap.occupied) do
        if VDist2(s[1], s[2], position[1], position[3]) < 5 then
            return false
        end
    end
    return true
end
-- Threat, answered with what the adapter knows: nothing. Zero is "no threat seen", which
-- is also what a brain with no intel grids would see — it makes the AI bold, and it makes
-- expansion conditions pass, both of which are the honest consequence of no intel yet.
function methods:GetThreatAtPosition() return 0 end
function methods:GetEngineerManagerUnitsBeingBuilt(category)
    return EntityCategoryCount(category, self.snap.underway or {})
end
function methods:GetEngineersWantingAssistance() return 0 end
function methods:GetManagerCount() return 1 end
-- FAF keeps every unmanaged unit in the 'ArmyPool' platoon, and the unit-count conditions
-- reach units THROUGH it. This adapter assigns no platoons, so the pool is simply the army —
-- which is the honest answer, not a shortcut.
function methods:GetPlatoonUniquelyNamed(name)
    if name == 'ArmyPool' then return self.pool end
    return nil
end

-- --- Boot: the builder list, assembled from FAF's own registries ---------------------------

function __rm_faf_boot(army, info)
    __rm_faf_scenario(info.markers, info.sizeX, info.sizeZ, info.armies)

    local brain = {
        army = army,
        faction = info.faction,
        startX = info.startX,
        startZ = info.startZ,
        snap = { units = {}, occupied = {} },
        Name = 'rm-faf-' .. tostring(army),
    }
    for name, fn in pairs(methods) do brain[name] = fn end

    -- Flags the corpus reads off the brain. All false, all true statements about this
    -- adapter: no cheat multipliers, no transports requested, no pre-built base.
    brain.CheatEnabled = false
    brain.TransportRequested = false
    brain.PreBuilt = false
    brain.islandCheck = false
    brain.islandMarker = false
    brain.LowEnergyMode = false
    brain.LowMassMode = false
    brain.ReclaimFailCounter = 0
    brain.ReclaimFailTimeStamp = 0

    -- Refreshed every decision pass from the snapshot; the corpus's own base-ai maintains
    -- this table from a thread, and the efficiency conditions read it directly.
    brain.EconomyOverTimeCurrent = {}

    -- Where each kind's budgeted walk resumes next pass (see walkBudgeted).
    brain.cursor = {}

    -- Per-builder position in its BuildStructures queue (see the engineer walk).
    brain.progress = {}

    -- The pool platoon (see GetPlatoonUniquelyNamed): counts over the army's own units.
    brain.pool = {
        GetNumCategoryUnits = function(pool, category, coords, radius)
            local n = 0
            for _, u in ipairs(brain.snap.units) do
                if EntityCategoryContains(category, u)
                    and (not coords or not radius
                         or VDist2(u.x, u.z, coords[1], coords[3]) <= radius) then
                    n = n + 1
                end
            end
            return n
        end,
        GetPlatoonUnits = function(pool)
            return brain.snap.units
        end,
        -- Threat as a headcount. The blueprints' own threat values are not parsed yet, so
        -- one unit is one threat — wave thresholds still scale with army size, which is the
        -- behaviour the corpus wants from this number.
        GetPlatoonThreat = function(pool, threatType, category, position, radius)
            local n = 0
            for _, u in ipairs(brain.snap.units) do
                if EntityCategoryContains(category, u)
                    and (not position or not radius
                         or VDist2(u.x, u.z, position[1], position[3]) <= radius) then
                    n = n + 1
                end
            end
            return n
        end,
    }

    -- The manager stand-ins: enough shape for conditions that navigate
    -- `BuilderManagers[locationType]`, honest about being location MAIN and nothing else.
    -- Their counting methods answer over the whole army, because MAIN is the whole base.
    local coords = function() return { info.startX, 0, info.startZ } end
    local function countUnits(category)
        return EntityCategoryCount(category, brain.snap.units)
    end
    local function countUnderway(category)
        return EntityCategoryCount(category, brain.snap.underway or {})
    end
    local manager = {
        GetLocationCoords = coords,
        Radius = 120,
        GetNumFactories = function() return countUnits(categories.STRUCTURE * categories.FACTORY) end,
        GetNumCategoryFactories = function(self, category) return countUnits(category) end,
        GetNumCategoryUnits = function(self, category) return countUnits(category) end,
        GetNumCategoryBeingBuilt = function(self, category) return countUnderway(category) end,
        -- A list, not a count: callers table.getn it. Nobody wants assistance — the
        -- adapter has no assist orders to give.
        GetEngineersWantingAssistance = function() return {} end,
    }
    brain.BuilderManagers = {
        MAIN = {
            Position = { info.startX, 0, info.startZ },
            EngineerManager = manager,
            FactoryManager = manager,
        },
    }
    -- Filled below once the template is known — conditions read it off the location.
    setmetatable(brain, brainMeta)

    -- FAF's own base description drives the list: template -> builder groups -> builders,
    -- flattened and sorted once. Priority first, name as the tiebreak, so the walk order is
    -- a fact about the data rather than about table iteration.
    local template = BaseBuilderTemplates[info.base]
    local list = {}
    local function addGroup(groupName)
        local group = BuilderGroups[groupName]
        if not group then return end
        for _, builderName in ipairs(group) do
            local spec = Builders[builderName]
            if spec then
                table.insert(list, { spec = spec, kind = group.BuildersType })
            end
        end
    end
    if template then
        for _, groupName in ipairs(template.Builders or {}) do addGroup(groupName) end
        for _, groupName in ipairs(template.NonCheatBuilders or {}) do addGroup(groupName) end
    end
    table.sort(list, function(a, b)
        local pa, pb = a.spec.Priority or 0, b.spec.Priority or 0
        if pa ~= pb then return pa > pb end
        return (a.spec.BuilderName or '') < (b.spec.BuilderName or '')
    end)
    brain.builders = list
    if template then
        brain.BuilderManagers.MAIN.BaseSettings = template.BaseSettings
    end

    brain.buildingTemplates = import('/lua/buildingtemplates.lua').BuildingTemplates
    __rm_faf.brains[army] = brain
    return #list
end

-- --- Conditions: the corpus's own code, fail-closed ----------------------------------------

local function conditionsPass(brain, spec)
    for _, cond in ipairs(spec.BuilderConditions or {}) do
        local fn, args
        if type(cond[1]) == 'function' then
            fn, args = cond[1], cond[2] or {}
        else
            local module = import(cond[1])
            fn = module and module[cond[2]]
            args = cond[3] or {}
            if not fn then
                recordMissing(tostring(cond[1]) .. ':' .. tostring(cond[2]))
                return false
            end
        end
        -- 'LocationType' is FAF's placeholder, substituted per base when a real manager
        -- instantiates a builder. The stand-in has exactly one base.
        local actual = {}
        for i, v in ipairs(args) do
            actual[i] = (v == 'LocationType') and 'MAIN' or v
        end
        local ok, result = pcall(fn, brain, unpack(actual))
        if not ok then
            local key = tostring(result)
            __rm_faf.condErrors[key] = (__rm_faf.condErrors[key] or 0) + 1
            return false
        end
        if not result then return false end
    end
    return true
end

-- The budgeted walk: at most `budget` of one kind's builders get their conditions checked
-- per pass, continuing round-robin from where the last pass stopped. FAF's own managers
-- work the same way — builders are re-checked periodically, not all of them every tick —
-- and it is what keeps a pass inside the watchdog's instruction budget: the expensive
-- conditions (marker sorts) are only ever a bounded slice of a pass. Priority still rules
-- WITHIN the walk because the list is priority-sorted; across passes it becomes "soon"
-- rather than "first", which is the trade the budget buys.
local function walkBudgeted(brain, kindName, budget, visit)
    local list = brain.builders
    local n = #list
    if n == 0 then return end
    local start = brain.cursor[kindName] or 1
    if start > n then start = 1 end
    local i = start
    local checked = 0
    repeat
        local item = list[i]
        i = (i % n) + 1
        if item.kind == kindName then
            checked = checked + 1
            if conditionsPass(brain, item.spec) and visit(item) then
                brain.cursor[kindName] = i
                return
            end
        end
    until i == start or checked >= budget
    brain.cursor[kindName] = i
end

local function buildingIdFor(brain, structureName)
    local perFaction = brain.buildingTemplates and brain.buildingTemplates[brain.faction]
    if not perFaction then return nil end
    for _, entry in ipairs(perFaction) do
        if entry[1] == structureName then return entry[2] end
    end
    return nil
end

-- --- The decision pass ---------------------------------------------------------------------
--
-- One structure at a time, factories minus work in flight, one attack wave per pass: the
-- serialization is the stand-in for the manager stack, stated here once. What fires within
-- those slots is entirely the corpus's call.

function __rm_faf_decide(army, snap)
    local brain = __rm_faf.brains[army]
    if not brain then return {} end
    brain.snap = snap
    for _, u in ipairs(snap.units) do u.__brain = brain end

    -- What base-ai's economy thread maintains, refreshed from the sim's own numbers.
    -- Requested is the best figure the sim meters today (upkeep; construction spends rather
    -- than requests), so a surplus economy reads as fully efficient — capped at 2, as the
    -- corpus's own ratios are in practice.
    local eco = brain.EconomyOverTimeCurrent
    eco.MassIncome = snap.massIncome
    eco.EnergyIncome = snap.energyIncome
    eco.MassRequested = snap.massUsage
    eco.EnergyRequested = snap.energyUsage
    eco.MassEfficiencyOverTime = math.min(snap.massIncome / math.max(snap.massUsage, 0.0001), 2)
    eco.EnergyEfficiencyOverTime =
        math.min(snap.energyIncome / math.max(snap.energyUsage, 0.0001), 2)
    eco.MassTrendOverTime = snap.massIncome - snap.massUsage
    eco.EnergyTrendOverTime = snap.energyIncome - snap.energyUsage

    local decisions = {}

    -- The builder pool: idle engineers first, the commander last — FAF's own habit, and it
    -- keeps the commander free once real engineers exist. One structure may be underway PER
    -- POOL MEMBER: the sim funds a construction without modelling the builder standing at
    -- it, so this count is what stands in for attendance — each engineer trained buys one
    -- more parallel build, exactly what an engineer is for.
    local builderPool = {}
    for _, u in ipairs(snap.units) do
        if u.idle and EntityCategoryContains(categories.ENGINEER - categories.COMMAND, u) then
            table.insert(builderPool, u)
        end
    end
    for _, u in ipairs(snap.units) do
        if EntityCategoryContains(categories.COMMAND, u) then
            table.insert(builderPool, u)
        end
    end
    local slots = #builderPool - snap.structuresUnderway

    if slots > 0 then
        walkBudgeted(brain, 'EngineerBuilder', 20, function(item)
            local construction = item.spec.BuilderData and item.spec.BuilderData.Construction
            local names = construction and construction.BuildStructures
            if not names or #names == 0 then return false end
            -- BuildStructures is a QUEUE the engineer works through, not a menu that
            -- restarts at the first entry — 'CDR Initial Easy' is one factory then six
            -- power generators, and restarting built six factories instead. Progress is
            -- remembered per builder; a BuildOnce builder (FAF's own marker) is spent when
            -- its queue is, while the rest wrap the way a repeating builder re-fires.
            local once = false
            for _, fn in ipairs(item.spec.PlatoonAddFunctions or {}) do
                if fn[2] == 'BuildOnce' then once = true end
            end
            local progress = brain.progress[item.spec.BuilderName] or 1
            if progress > #names then
                if once then return false end
                progress = 1
            end
            while progress <= #names do
                local structure = names[progress]
                local bp = buildingIdFor(brain, structure)
                progress = progress + 1
                if bp then
                    brain.progress[item.spec.BuilderName] = progress
                    local builderUnit = builderPool[#builderPool - slots + 1]
                    table.insert(decisions, {
                        kind = 'build', bp = bp, structure = structure,
                        builder = builderUnit.h, name = item.spec.BuilderName,
                    })
                    slots = slots - 1
                    return slots <= 0
                end
            end
            brain.progress[item.spec.BuilderName] = progress
            return false
        end)
    end

    -- Factories: the corpus picks the unit, one train per factory not already working.
    local factories = {}
    for _, u in ipairs(snap.units) do
        if EntityCategoryContains(categories.STRUCTURE * categories.FACTORY, u) then
            table.insert(factories, u)
        end
    end
    local free = #factories - snap.mobileUnderway
    if free > 0 then
        walkBudgeted(brain, 'FactoryBuilder', 20, function(item)
            local template = PlatoonTemplates[item.spec.PlatoonTemplate]
            local squads = template and template.FactionSquads
            local squad = squads and squads[factionNames[brain.faction]]
            local bp = squad and squad[1] and squad[1][1]
            if not bp then return false end
            -- A factory only builds its own domain — the unit id's third letter is FAF's
            -- own encoding (l land, a air, s sea), and the factory's categories carry the
            -- matching tag. Without this a land factory accepted air-scout orders the sim
            -- then refused every second, wasting the slot.
            local letter = string.sub(bp, 3, 3)
            local need = (letter == 'a' and categories.AIR)
                or (letter == 's' and categories.NAVAL) or categories.LAND
            for _, factory in ipairs(factories) do
                if not factory.__taken and EntityCategoryContains(need, factory) then
                    factory.__taken = true
                    table.insert(decisions, {
                        kind = 'train', bp = bp,
                        builder = factory.h, name = item.spec.BuilderName,
                    })
                    free = free - 1
                    break
                end
            end
            return free <= 0
        end)
    end

    -- Platoon forming: the corpus's form builders decide WHEN an attack goes out; the
    -- squad's own category and size decide WHO. One wave per pass.
    walkBudgeted(brain, 'PlatoonFormBuilder', 15, function(item)
        local template = PlatoonTemplates[item.spec.PlatoonTemplate]
        local squads = template and template.GlobalSquads
        if not squads then return false end
        local gathered = {}
        local wanted = 0
        for _, squad in ipairs(squads) do
            wanted = wanted + (squad[2] or 1)
            local cap = squad[3] or 1
            for _, u in ipairs(snap.units) do
                if cap <= 0 then break end
                if u.idle and EntityCategoryContains(squad[1], u) then
                    table.insert(gathered, u.h)
                    cap = cap - 1
                end
            end
        end
        if #gathered >= wanted and wanted > 0 then
            table.insert(decisions, {
                kind = 'attack', units = gathered, name = item.spec.BuilderName,
            })
            return true
        end
        return false
    end)

    return decisions
end

-- The ledgers, for the sanity report. Sorted worst-first so the top line is the next task.
local function sortedLedger(t)
    local lines = {}
    for name, count in pairs(t) do
        table.insert(lines, { name = name, count = count })
    end
    table.sort(lines, function(a, b)
        if a.count ~= b.count then return a.count > b.count end
        return a.name < b.name
    end)
    local out = {}
    for _, line in ipairs(lines) do
        table.insert(out, line.name .. ' x' .. tostring(line.count))
    end
    return out
end
function __rm_faf_missing() return sortedLedger(__rm_faf.missing) end
function __rm_faf_cond_errors() return sortedLedger(__rm_faf.condErrors) end

end
)lua";

/// FAF's lobby order, which BuildingTemplates and FactionSquads both index by.
[[nodiscard]] int fafFactionIndex(rm::sim::Faction faction) noexcept {
    switch (faction) {
    case rm::sim::Faction::Uef: return 1;
    case rm::sim::Faction::Aeon: return 2;
    case rm::sim::Faction::Cybran: return 3;
    case rm::sim::Faction::Seraphim: return 4;
    }
    return 1;
}

/// "/units/UEB1103/UEB1103_unit.bp" from "ueb1103" — the id in the blueprint tree's own case.
[[nodiscard]] std::string blueprintPathFor(std::string id) {
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return "/units/" + id + "/" + id + "_unit.bp";
}

/// Reads an array of strings a driver ledger function returns. Empty on any failure —
/// the ledgers are diagnostics, and a diagnostic must never take the match down.
[[nodiscard]] std::vector<std::string> readStringArray(FafAi& ai, const char* fn) {
    std::vector<std::string> out;
    lua_State* lua = ai.state();
    if (lua == nullptr) {
        return out;
    }
    lua_getglobal(lua, fn);
    if (!lua_isfunction(lua, -1) || lua_pcall(lua, 0, 1, 0) != 0) {
        lua_pop(lua, 1);
        return out;
    }
    if (lua_istable(lua, -1)) {
        const auto n = static_cast<lua_Integer>(lua_rawlen(lua, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(lua, -1, i);
            if (const char* s = lua_tostring(lua, -1); s != nullptr) {
                out.emplace_back(s);
            }
            lua_pop(lua, 1);
        }
    }
    lua_pop(lua, 1);
    return out;
}

} // namespace

bool installFafDriver(FafAi& ai) {
    return ai.eval(kFafDriver);
}

std::vector<std::string> fafMissingBrainMethods(FafAi& ai) {
    return readStringArray(ai, "__rm_faf_missing");
}

std::vector<std::string> fafConditionErrors(FafAi& ai) {
    return readStringArray(ai, "__rm_faf_cond_errors");
}

FafOpponent::FafOpponent(FafAi& sandbox, int army) : sandbox_(sandbox), army_(army) {}

/// Teaches the driver this type's category set, once per blueprint id per opponent —
/// `__rm_faf_type` is a no-op for a type the sandbox already knows.
void FafOpponent::teachType(lua_State* lua, const rm::unitdef::UnitDef& def) {
    if (!sentTypes_.insert(def.name).second) {
        return;
    }
    lua_getglobal(lua, "__rm_faf_type");
    lua_pushstring(lua, def.name.c_str());
    lua_newtable(lua);
    lua_Integer tagIndex = 1;
    for (const std::string& tag : def.categories) {
        lua_pushstring(lua, tag.c_str());
        lua_rawseti(lua, -2, tagIndex++);
    }
    if (lua_pcall(lua, 2, 0, 0) != 0) {
        lua_pop(lua, 1);
    }
}

void FafOpponent::observe(const World& world, std::span<const rm::sim::Event> /*events*/) {
    world_ = &world;
}

void FafOpponent::advance(rm::TickIndex /*tick*/) {
    decisions_.clear();
    handles_.clear();
    if (world_ == nullptr || !sandbox_.ready()) {
        return;
    }
    lua_State* lua = sandbox_.state();
    const rm::app::UnitScene& scene = world_->scene;
    const auto armyIndex = static_cast<std::size_t>(army_);
    if (armyIndex >= scene.armies.size() || armyIndex >= world_->starts.size()) {
        return;
    }
    // This entry drives the VM through `state()` directly, so it refuels itself — without
    // this, the watchdog bills every pass against one budget and kills a legitimate pass
    // some sixty seconds in.
    sandbox_.refuel();

    if (!booted_) {
        if (!installFafDriver(sandbox_)) {
            std::printf("faf-opponent: driver failed: %s\n", sandbox_.lastError().c_str());
            return;
        }
        // __rm_faf_boot(army, info)
        lua_getglobal(lua, "__rm_faf_boot");
        lua_pushinteger(lua, army_);
        lua_newtable(lua);
        lua_pushinteger(lua, fafFactionIndex(scene.armies[armyIndex].faction));
        lua_setfield(lua, -2, "faction");
        const rm::mapinfo::StartPosition& start = world_->starts[armyIndex];
        lua_pushnumber(lua, static_cast<lua_Number>(start.x));
        lua_setfield(lua, -2, "startX");
        lua_pushnumber(lua, static_cast<lua_Number>(start.z));
        lua_setfield(lua, -2, "startZ");
        lua_pushinteger(lua, world_->field.squaresX * rm::kSquareSize);
        lua_setfield(lua, -2, "sizeX");
        lua_pushinteger(lua, world_->field.squaresZ * rm::kSquareSize);
        lua_setfield(lua, -2, "sizeZ");
        lua_pushinteger(lua, static_cast<lua_Integer>(scene.armies.size()));
        lua_setfield(lua, -2, "armies");
        // 'NormalMain' is FAF's own default skirmish base; template CHOICE (per-map scoring
        // via each template's FirstBaseFunction) is a later pass.
        lua_pushstring(lua, "NormalMain");
        lua_setfield(lua, -2, "base");
        lua_newtable(lua);
        {
            lua_Integer next = 1;
            for (const rm::scenario::Marker& marker : world_->markers) {
                lua_newtable(lua);
                lua_pushstring(lua, marker.name.c_str());
                lua_setfield(lua, -2, "name");
                lua_pushstring(lua, marker.type.c_str());
                lua_setfield(lua, -2, "type");
                lua_pushnumber(lua, static_cast<lua_Number>(marker.position[0]));
                lua_setfield(lua, -2, "x");
                lua_pushnumber(lua, static_cast<lua_Number>(marker.position[1]));
                lua_setfield(lua, -2, "y");
                lua_pushnumber(lua, static_cast<lua_Number>(marker.position[2]));
                lua_setfield(lua, -2, "z");
                lua_rawseti(lua, -2, next++);
            }
        }
        lua_setfield(lua, -2, "markers");
        if (lua_pcall(lua, 2, 1, 0) != 0) {
            std::printf("faf-opponent: boot failed for army %d: %s\n", army_,
                        lua_tostring(lua, -1));
            lua_pop(lua, 1);
            return;
        }
        lua_pop(lua, 1);  // builder count, informational
        booted_ = true;
    }

    // --- The snapshot -------------------------------------------------------------------
    //
    // Arrays in slot order, nothing keyed by anything hashed: the pass must read the same
    // world in the same order every run.
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    lua_newtable(lua);  // snap

    // Economy, per tick — the scale FAF's own thresholds are written against.
    const rm::sim::Economy& economy = scene.economies[armyIndex];
    const auto pushNumber = [lua](const char* name, float value) {
        lua_pushnumber(lua, static_cast<lua_Number>(value));
        lua_setfield(lua, -2, name);
    };
    pushNumber("mass", rm::sim::magToFloat(economy.stored.mass));
    pushNumber("energy", rm::sim::magToFloat(economy.stored.energy));
    pushNumber("massStorage", rm::sim::magToFloat(economy.storage.mass));
    pushNumber("energyStorage", rm::sim::magToFloat(economy.storage.energy));
    pushNumber("massIncome", rm::sim::magToFloat(economy.incomePerTick.mass));
    pushNumber("energyIncome", rm::sim::magToFloat(economy.incomePerTick.energy));
    // Usage is what construction and upkeep drain; the sim spends rather than meters it, so
    // the honest per-tick figure available today is upkeep alone. Stated here once.
    pushNumber("massUsage", 0.0f);
    pushNumber("energyUsage", rm::sim::magToFloat(economy.upkeepPerTick.energy));

    // What is under construction, twice over: the counts that gate the driver's slots, and
    // `underway` — category-carrying entries for the corpus's own "how many of these are
    // already being built" conditions.
    std::size_t structuresUnderway = 0;
    std::size_t mobileUnderway = 0;
    lua_newtable(lua);  // snap.underway
    lua_Integer underwayIndex = 1;
    for (const rm::sim::Construction& construction : scene.building) {
        if (construction.armyIndex != army_ || construction.finished()) {
            continue;
        }
        const rm::unitdef::UnitDef* def =
            scene.catalog.def(static_cast<rm::UnitTypeIndex>(construction.blueprintIndex));
        if (def == nullptr) {
            continue;
        }
        if (def->isMobile()) {
            ++mobileUnderway;
        } else {
            ++structuresUnderway;
        }
        teachType(lua, *def);
        lua_newtable(lua);
        lua_pushstring(lua, def->name.c_str());
        lua_setfield(lua, -2, "bp");
        lua_getglobal(lua, "__rm_faf");
        lua_getfield(lua, -1, "cats");
        lua_getfield(lua, -1, def->name.c_str());
        lua_setfield(lua, -4, "__cats");
        lua_pop(lua, 2);
        lua_rawseti(lua, -2, underwayIndex++);
    }
    lua_setfield(lua, -2, "underway");
    lua_pushinteger(lua, static_cast<lua_Integer>(structuresUnderway));
    lua_setfield(lua, -2, "structuresUnderway");
    lua_pushinteger(lua, static_cast<lua_Integer>(mobileUnderway));
    lua_setfield(lua, -2, "mobileUnderway");

    // Units: this army's, alive, each carrying its shared category set. `occupied` is every
    // army's standing structures — the one cross-army fact, for "is this deposit taken".
    lua_newtable(lua);  // snap.units
    lua_Integer unitIndex = 1;
    lua_newtable(lua);  // snap.occupied (kept on the stack below units, set at the end)
    lua_Integer occupiedIndex = 1;
    lua_insert(lua, -2);  // [snap, occupied, units]

    for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
        if (!health[slot].alive()) {
            continue;
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
        if (def == nullptr) {
            continue;
        }
        const rm::sim::Transform& transform = scene.store.transforms()[slot];
        const float x = rm::sim::fxToFloat(transform.x);
        const float z = rm::sim::fxToFloat(transform.z);

        if (!def->isMobile()) {
            lua_pushvalue(lua, -2);  // occupied
            lua_newtable(lua);
            lua_pushnumber(lua, static_cast<lua_Number>(x));
            lua_rawseti(lua, -2, 1);
            lua_pushnumber(lua, static_cast<lua_Number>(z));
            lua_rawseti(lua, -2, 2);
            lua_rawseti(lua, -2, occupiedIndex++);
            lua_pop(lua, 1);
        }

        if (motion[slot].armyIndex != army_) {
            continue;
        }

        teachType(lua, *def);

        handles_.push_back(scene.store.idAt(slot));
        lua_newtable(lua);  // the unit
        lua_pushstring(lua, def->name.c_str());
        lua_setfield(lua, -2, "bp");
        lua_pushinteger(lua, static_cast<lua_Integer>(handles_.size()));
        lua_setfield(lua, -2, "h");
        lua_pushnumber(lua, static_cast<lua_Number>(x));
        lua_setfield(lua, -2, "x");
        lua_pushnumber(lua, static_cast<lua_Number>(z));
        lua_setfield(lua, -2, "z");
        // Idle: not moving. Whether a builder is mid-construction is answered by the
        // *Underway counts, because the sim does not associate a construction with its
        // builder — stated in the driver where the counts gate decisions.
        lua_pushboolean(lua, motion[slot].moving ? 0 : 1);
        lua_setfield(lua, -2, "idle");
        // __cats: the shared set, and the shared unit metatable, from the driver's cache.
        lua_getglobal(lua, "__rm_faf");
        lua_getfield(lua, -1, "cats");
        lua_getfield(lua, -1, def->name.c_str());
        lua_setfield(lua, -4, "__cats");
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "unitMeta");
        lua_setmetatable(lua, -3);
        lua_pop(lua, 1);
        lua_rawseti(lua, -2, unitIndex++);
    }

    lua_setfield(lua, -3, "units");     // snap.units
    lua_setfield(lua, -2, "occupied");  // snap.occupied

    // --- The decision pass --------------------------------------------------------------
    lua_getglobal(lua, "__rm_faf_decide");
    lua_insert(lua, -2);  // [__rm_faf_decide, snap]
    lua_pushinteger(lua, army_);
    lua_insert(lua, -2);  // [__rm_faf_decide, army, snap]
    if (lua_pcall(lua, 2, 1, 0) != 0) {
        std::printf("faf-opponent: decide failed for army %d: %s\n", army_,
                    lua_tostring(lua, -1));
        lua_pop(lua, 1);
        return;
    }

    if (lua_istable(lua, -1)) {
        const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(lua, -1, i);
            convertDecision(lua);
            lua_pop(lua, 1);
        }
    }
    lua_pop(lua, 1);
}

/// One driver decision table -> one (or several) port Decisions. Reads the table at the top
/// of the stack and leaves the stack as it found it.
void FafOpponent::convertDecision(lua_State* lua) {
    const auto field = [lua](const char* name) -> std::string {
        lua_getfield(lua, -1, name);
        const char* s = lua_tostring(lua, -1);
        std::string out = s != nullptr ? s : "";
        lua_pop(lua, 1);
        return out;
    };
    const auto handleAt = [this, lua](int index) -> std::optional<rm::sim::UnitId> {
        lua_Integer h = 0;
        if (index == 0) {
            lua_getfield(lua, -1, "builder");
            h = lua_tointeger(lua, -1);
            lua_pop(lua, 1);
        } else {
            h = index;
        }
        if (h < 1 || static_cast<std::size_t>(h) > handles_.size()) {
            return std::nullopt;
        }
        return handles_[static_cast<std::size_t>(h - 1)];
    };

    const std::string kind = field("kind");
    const rm::app::UnitScene& scene = world_->scene;

    if (kind == "build" || kind == "train") {
        const std::optional<rm::sim::UnitId> builder = handleAt(0);
        if (!builder || !scene.store.alive(*builder)) {
            return;
        }
        const std::string bp = field("bp");
        if (bp.empty()) {
            return;
        }

        std::optional<std::array<rm::sim::Fx, 3>> site;
        if (kind == "train") {
            // Where the factory stands; the spawn path rolls the unit off it.
            const rm::sim::Transform& transform = scene.store.transforms()[builder->index];
            site = std::array<rm::sim::Fx, 3>{transform.x, rm::sim::Fx{}, transform.z};
        } else {
            const std::string structure = field("structure");
            const bool wantsDeposit = structure.find("Resource") != std::string::npos
                                      || structure.find("MassExtraction") != std::string::npos;
            const rm::mapinfo::StartPosition& start =
                world_->starts[static_cast<std::size_t>(army_)];
            const std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x), rm::sim::Fx{},
                                                  rm::sim::fxFromFloat(start.z)};
            if (wantsDeposit) {
                const rm::scenario::Marker* deposit =
                    rm::app::nearestFreeDeposit(scene, world_->markers, home);
                if (deposit != nullptr) {
                    site = rm::app::fxPoint(deposit->position);
                }
            } else {
                site = rm::sim::structureSite(home, world_->centreX, world_->centreZ,
                                              structureSlot_++);
            }
        }
        if (!site) {
            return;
        }
        decisions_.push_back(Decision{
            .kind = Decision::Kind::StartConstruction,
            .blueprint = blueprintPathFor(field("bp")),
            .site = *site,
            .builder = *builder,
        });
        if (rm::app::gFafLog) {
            std::printf("  [faf %d] %s '%s' -> %s\n", army_, kind.c_str(),
                        field("name").c_str(), bp.c_str());
        }
        return;
    }

    if (kind == "attack") {
        std::size_t sent = 0;
        // The corpus said when and who; aiming at the nearest enemy commander is the
        // adapter's one tactical opinion, the same one the scripted wave holds.
        const rm::mapinfo::StartPosition& start =
            world_->starts[static_cast<std::size_t>(army_)];
        const std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x), rm::sim::Fx{},
                                              rm::sim::fxFromFloat(start.z)};
        const std::optional<std::array<rm::sim::Fx, 3>> target =
            rm::app::nearestEnemyCommander(scene, army_, home);
        if (!target) {
            return;
        }
        lua_getfield(lua, -1, "units");
        if (lua_istable(lua, -1)) {
            const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(lua, -1, i);
                const auto h = static_cast<int>(lua_tointeger(lua, -1));
                lua_pop(lua, 1);
                if (const std::optional<rm::sim::UnitId> unit =
                        h >= 1 && static_cast<std::size_t>(h) <= handles_.size()
                            ? std::optional<rm::sim::UnitId>{handles_[static_cast<std::size_t>(
                                  h - 1)]}
                            : std::nullopt) {
                    decisions_.push_back(Decision{
                        .kind = Decision::Kind::Move,
                        .unit = *unit,
                        .toX = (*target)[0],
                        .toZ = (*target)[2],
                    });
                    ++sent;
                }
            }
        }
        lua_pop(lua, 1);
        if (rm::app::gFafLog && sent > 0) {
            std::printf("  [faf %d] attack '%s' -> %zu unit(s)\n", army_,
                        field("name").c_str(), sent);
        }
    }
}

} // namespace rm::ai
