#include "app/FafOpponent.hpp"

#include "app/Match.hpp"
#include "app/Scene.hpp"

#include "core/sim/BuildOrder.hpp"
#include "core/unit/UnitBlueprint.hpp"

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

__rm_faf = { brains = {}, missing = {}, condErrors = {}, cats = {}, threat = {}, scenario = false }

local factionNames = { 'UEF', 'Aeon', 'Cybran', 'Seraphim' }

-- One category set per blueprint id, built from the tag list C++ sends once per type.
-- In Moho every unit id is itself a category, so the id joins its own set.
function __rm_faf_type(bp, tags, threat)
    if __rm_faf.cats[bp] then return end
    local set = {}
    for _, tag in ipairs(tags) do set[string.upper(tag)] = true end
    set[string.upper(bp)] = true
    __rm_faf.cats[bp] = set
    __rm_faf.threat[bp] = threat or {}
end

-- One unit's threat in one of Moho's domains, from the blueprint's own estimates. The
-- corpus asks with a zoo of spellings; unknown ones read as surface, the common case.
function __rm_faf_threat(u, threatType)
    local t = __rm_faf.threat[u.bp]
    if not t then return 0 end
    if threatType == 'Air' or threatType == 'AntiAir' then return t.a or 0 end
    if threatType == 'Sub' or threatType == 'AntiSub' or threatType == 'Naval' then
        return t.u or 0
    end
    if threatType == 'Economy' then return t.e or 0 end
    if threatType == 'Overall' then
        return (t.s or 0) + (t.a or 0) + (t.u or 0) + (t.e or 0)
    end
    return t.s or 0
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
        -- 'Upgrading' is the one state the snapshot tracks (the upgrade-in-place tech
        -- path); everything else honestly answers false.
        IsUnitState = function(u, state)
            if state == 'Upgrading' then return u.upgrading == true end
            return false
        end,
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
-- Threat at a point: the blueprints' own estimates summed over ENEMIES THE ARMY CAN SEE —
-- the snapshot's enemies list is already intel-filtered, so fog hides threat exactly as it
-- hides units. The ring-to-radius mapping is Guessed against Moho's iMAP cells: one ring
-- is read as roughly one and a half grid squares.
function methods:GetThreatAtPosition(position, rings, _, threatType)
    local radius = ((rings or 0) + 1) * 96
    local total = 0
    for _, e in ipairs(self.snap.enemies or {}) do
        if VDist2(e.x, e.z, position[1], position[3]) <= radius then
            total = total + __rm_faf_threat(e, threatType)
        end
    end
    return total
end
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

local function buildingIdFor(brain, structureName)
    local perFaction = brain.buildingTemplates and brain.buildingTemplates[brain.faction]
    if not perFaction then return nil end
    for _, entry in ipairs(perFaction) do
        if entry[1] == structureName then return entry[2] end
    end
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

    -- Per-builder position in its BuildStructures queue (see the engineer walk).
    brain.progress = {}

    -- The condition cadence cache (see conditionsPass), keyed by spec table.
    brain.condCache = {}

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
        -- Real threat now: the blueprints' own Defense.*ThreatLevel estimates, summed
        -- over the matching units — the number the corpus's wave thresholds were
        -- authored against.
        GetPlatoonThreat = function(pool, threatType, category, position, radius)
            local total = 0
            for _, u in ipairs(brain.snap.units) do
                if EntityCategoryContains(category, u)
                    and (not position or not radius
                         or VDist2(u.x, u.z, position[1], position[3]) <= radius) then
                    total = total + __rm_faf_threat(u, threatType)
                end
            end
            return total
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
        Radius = 200,
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

    -- Every unit id this faction's factory builders could ask for, handed back so the
    -- adapter can teach their category sets — the tech gate below needs the categories of
    -- units that do not exist yet, which only the blueprints know.
    local wanted = {}
    local seen = {}
    local function want(bp)
        if bp and not seen[bp] then
            seen[bp] = true
            table.insert(wanted, bp)
        end
    end
    for _, item in ipairs(list) do
        if item.kind == 'FactoryBuilder' then
            local template = PlatoonTemplates[item.spec.PlatoonTemplate]
            local squads = template and template.FactionSquads
            local squad = squads and squads[factionNames[info.faction]]
            want(squad and squad[1] and squad[1][1])
        elseif item.kind == 'EngineerBuilder' then
            local construction = item.spec.BuilderData and item.spec.BuilderData.Construction
            for _, structure in ipairs((construction and construction.BuildStructures) or {}) do
                want(buildingIdFor(brain, structure))
            end
        end
    end
    return wanted
end

-- --- Conditions: the corpus's own code, fail-closed ----------------------------------------

local function conditionsPass(brain, spec)
    -- CACHED ON A CADENCE, which is FAF's own design: BrainConditionsMonitor re-checks a
    -- condition every few seconds and serves the cached answer between — evaluating every
    -- builder's full condition list every pass is what blew the instruction budget once
    -- the priority walk saw the whole list. Three seconds, keyed by the spec table itself
    -- (stable for the life of the brain).
    local now = brain.snap.tick or 0
    local cached = brain.condCache[spec]
    if cached and (now - cached.tick) < 30 then
        return cached.result
    end
    local result = true
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
                result = false
                break
            end
        end
        if result then
            -- 'LocationType' is FAF's placeholder, substituted per base when a real manager
            -- instantiates a builder. The stand-in has exactly one base.
            local actual = {}
            for i, v in ipairs(args) do
                actual[i] = (v == 'LocationType') and 'MAIN' or v
            end
            local ok, value = pcall(fn, brain, unpack(actual))
            if not ok then
                local key = tostring(value)
                __rm_faf.condErrors[key] = (__rm_faf.condErrors[key] or 0) + 1
                result = false
            elseif not value then
                result = false
            end
        end
        if not result then break end
    end
    brain.condCache[spec] = { tick = now, result = result }
    return result
end

-- The walk: top of the priority order down, first passing builder wins — FAF's own
-- semantics, and the fix for a real bug: an earlier round-robin cursor version made
-- priority mean "soon" rather than "first", so the no-condition ACU opening queue lost
-- its slot to far-mex builders from the tail and no power was ever built. The full walk
-- is affordable because the caller refuels the watchdog per pass; if a pass ever outgrows
-- the budget again, the watchdog says so by name rather than by a stall.
local function walkPriority(brain, kindName, visit)
    for _, item in ipairs(brain.builders) do
        if item.kind == kindName and conditionsPass(brain, item.spec) and visit(item) then
            return
        end
    end
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
        -- What the NEXT builder in the pool may legally build, by the tag the blueprints
        -- grant its tier — the sim would refuse anyway, but a refused decision burns the
        -- slot for a whole pass, which is how a T3 power plant order starved a base.
        local nextBuilder = builderPool[#builderPool - slots + 1]
        local builderTag =
            (EntityCategoryContains(categories.COMMAND, nextBuilder) and 'BUILTBYCOMMANDER')
            or (EntityCategoryContains(categories.TECH3, nextBuilder) and 'BUILTBYTIER3ENGINEER')
            or (EntityCategoryContains(categories.TECH2, nextBuilder) and 'BUILTBYTIER2ENGINEER')
            or 'BUILTBYTIER1ENGINEER'
        walkPriority(brain, 'EngineerBuilder', function(item)
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
                -- FAF's InstanceCount: how many of this builder's platoons may exist at
                -- once. The nearest honest equivalent here is unfinished constructions of
                -- the same blueprint — without it the condition cadence re-fires a passing
                -- builder every pass and a hundred power plants go up at 3% funding each.
                if bp then
                    local bpUpper = string.upper(bp)
                    local cats = __rm_faf.cats[bpUpper]
                    if cats and not cats[builderTag] then
                        bp = nil
                    else
                        local live = 0
                        for _, w in ipairs(snap.underway or {}) do
                            if w.bp == bpUpper then live = live + 1 end
                        end
                        if live >= (item.spec.InstanceCount or 1) then
                            bp = nil
                        end
                    end
                end
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
        if not u.upgrading
            and EntityCategoryContains(categories.STRUCTURE * categories.FACTORY, u) then
            table.insert(factories, u)
        end
    end
    local free = #factories - snap.mobileUnderway
    if free > 0 then
        walkPriority(brain, 'FactoryBuilder', function(item)
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
            -- And only its own TIER: the unit must carry the BUILTBYTIERxFACTORY tag the
            -- factory's tech level grants — FAF's factory manager checks CanBuildPlatoon
            -- the same way, and without it a T3 engineer order wastes a T1 factory's slot
            -- every single pass.
            local unitCats = __rm_faf.cats[string.upper(bp)]
            for _, factory in ipairs(factories) do
                local tierTag = (EntityCategoryContains(categories.TECH3, factory)
                                     and 'BUILTBYTIER3FACTORY')
                    or (EntityCategoryContains(categories.TECH2, factory)
                            and 'BUILTBYTIER2FACTORY')
                    or 'BUILTBYTIER1FACTORY'
                if not factory.__taken and EntityCategoryContains(need, factory)
                    and unitCats and unitCats[tierTag] then
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
    -- squad's own category and size decide WHO. One wave per pass. A template whose Plan
    -- is UnitUpgradeAI is the OTHER thing form builders do — form a platoon of one
    -- structure and upgrade it in place; that becomes an upgrade decision, never a march.
    walkPriority(brain, 'PlatoonFormBuilder', function(item)
        local template = PlatoonTemplates[item.spec.PlatoonTemplate]
        local squads = template and template.GlobalSquads
        if not squads then return false end
        if template.Plan == 'UnitUpgradeAI' then
            for _, u in ipairs(snap.units) do
                if u.idle and not u.upgrading and EntityCategoryContains(squads[1][1], u) then
                    table.insert(decisions, {
                        kind = 'upgrade', builder = u.h, name = item.spec.BuilderName,
                    })
                    return true
                end
            end
            return false
        end
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
    lua_newtable(lua);  // the blueprint's threat estimates, for __rm_faf_threat
    lua_pushnumber(lua, static_cast<lua_Number>(def.surfaceThreat));
    lua_setfield(lua, -2, "s");
    lua_pushnumber(lua, static_cast<lua_Number>(def.airThreat));
    lua_setfield(lua, -2, "a");
    lua_pushnumber(lua, static_cast<lua_Number>(def.subThreat));
    lua_setfield(lua, -2, "u");
    lua_pushnumber(lua, static_cast<lua_Number>(def.economyThreat));
    lua_setfield(lua, -2, "e");
    if (lua_pcall(lua, 3, 0, 0) != 0) {
        lua_pop(lua, 1);
    }
}

void FafOpponent::observe(const World& world, std::span<const rm::sim::Event> /*events*/) {
    world_ = &world;
}

void FafOpponent::advance(rm::TickIndex tick) {
    decisions_.clear();
    handles_.clear();
    plannedThisPass_.clear();
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
        // Boot hands back every unit id this faction's factory builders could ask for.
        // Their category sets are taught NOW, from the blueprints themselves — the tech
        // gate has to know the categories of units that do not exist yet, and a read-only
        // parse per candidate at boot is what that costs.
        if (lua_istable(lua, -1)) {
            const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(lua, -1, i);
                const char* id = lua_tostring(lua, -1);
                lua_pop(lua, 1);
                if (id == nullptr) {
                    continue;
                }
                const std::string path = blueprintPathFor(id);
                const std::optional<std::vector<std::byte>> bytes =
                    world_->content.read(path);
                if (!bytes) {
                    continue;
                }
                const auto def = rm::unitbp::load(
                    std::string_view{reinterpret_cast<const char*>(bytes->data()),
                                     bytes->size()},
                    path);
                if (def) {
                    teachType(lua, *def);
                }
            }
        }
        lua_pop(lua, 1);
        booted_ = true;
    }

    // --- The snapshot -------------------------------------------------------------------
    //
    // Arrays in slot order, nothing keyed by anything hashed: the pass must read the same
    // world in the same order every run.
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    lua_newtable(lua);  // snap
    lua_pushinteger(lua, static_cast<lua_Integer>(tick));
    lua_setfield(lua, -2, "tick");

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
    // Usage is everything last tick tried to pay — construction drain plus upkeep, the
    // figure FA's efficiency conditions divide income by. This number is what lets the
    // corpus's own economy gates see over-commitment and stop starting new work.
    pushNumber("massUsage", rm::sim::magToFloat(economy.requestedLastTick.mass));
    pushNumber("energyUsage", rm::sim::magToFloat(economy.requestedLastTick.energy));

    // What is under construction, twice over: the counts that gate the driver's slots, and
    // `underway` — category-carrying entries for the corpus's own "how many of these are
    // already being built" conditions. Upgrades are in NEITHER counter: they occupy their
    // own unit, which the snapshot marks `upgrading` and the driver excludes from its
    // factory pool — counting them here as well would charge the capacity twice.
    std::size_t structuresUnderway = 0;
    std::size_t mobileUnderway = 0;
    std::vector<rm::sim::UnitId> upgrading;
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
        if (construction.isUpgrade()) {
            upgrading.push_back(construction.upgradeOf);
        } else if (def->isMobile()) {
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
        if (std::find(upgrading.begin(), upgrading.end(), handles_.back())
            != upgrading.end()) {
            lua_pushboolean(lua, 1);
            lua_setfield(lua, -2, "upgrading");
        }
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

    // The enemies this army's ALLIANCE can currently see — position and blueprint id, which
    // with the taught threat tables is everything GetThreatAtPosition needs. Intel-filtered
    // here so fog hides threat exactly as it hides units; the AI reads the same grid the
    // renderer's fog does.
    lua_newtable(lua);  // snap.enemies
    {
        const int alliance = scene.armies[armyIndex].alliance;
        lua_Integer enemyIndex = 1;
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            if (!health[slot].alive()) {
                continue;
            }
            const int owner = motion[slot].armyIndex;
            if (owner < 0 || static_cast<std::size_t>(owner) >= scene.armies.size()
                || scene.armies[static_cast<std::size_t>(owner)].alliance == alliance) {
                continue;
            }
            const rm::sim::Transform& transform = scene.store.transforms()[slot];
            if (!scene.intel.sees(alliance, rm::sim::IntelKind::Vision, transform.x,
                                  transform.z)) {
                continue;
            }
            const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(slot));
            if (def == nullptr) {
                continue;
            }
            teachType(lua, *def);
            lua_newtable(lua);
            lua_pushstring(lua, def->name.c_str());
            lua_setfield(lua, -2, "bp");
            lua_pushnumber(lua, static_cast<lua_Number>(rm::sim::fxToFloat(transform.x)));
            lua_setfield(lua, -2, "x");
            lua_pushnumber(lua, static_cast<lua_Number>(rm::sim::fxToFloat(transform.z)));
            lua_setfield(lua, -2, "z");
            lua_rawseti(lua, -2, enemyIndex++);
        }
    }
    lua_setfield(lua, -2, "enemies");

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

    if (kind == "upgrade") {
        // The corpus picked WHICH unit upgrades (UnitUpgradeAI's platoon of one); the
        // blueprint says what it becomes. The sim pins the site to the unit itself.
        const std::optional<rm::sim::UnitId> unit = handleAt(0);
        if (!unit || !scene.store.alive(*unit)) {
            return;
        }
        const rm::unitdef::UnitDef* def = scene.catalog.def(scene.store.typeAt(unit->index));
        if (def == nullptr || def->upgradesTo.empty()) {
            return;
        }
        const rm::sim::Transform& at = scene.store.transforms()[unit->index];
        decisions_.push_back(Decision{
            .kind = Decision::Kind::StartConstruction,
            .blueprint = blueprintPathFor(def->upgradesTo),
            .site = {at.x, rm::sim::Fx{}, at.z},
            .builder = *unit,
        });
        if (rm::app::gFafLog) {
            std::printf("  [faf %d] upgrade '%s' -> %s\n", army_, field("name").c_str(),
                        def->upgradesTo.c_str());
        }
        return;
    }

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
            const auto plannedNear = [this](const std::array<rm::sim::Fx, 3>& at) {
                for (const std::array<rm::sim::Fx, 3>& planned : plannedThisPass_) {
                    const float dx = rm::sim::fxToFloat(planned[0]) - rm::sim::fxToFloat(at[0]);
                    const float dz = rm::sim::fxToFloat(planned[2]) - rm::sim::fxToFloat(at[2]);
                    if (dx * dx + dz * dz < 8.0f * 8.0f) {
                        return true;
                    }
                }
                return false;
            };
            if (wantsDeposit) {
                // The nearest deposit free of BOTH the sim's claims and this pass's own
                // plans — nearestFreeDeposit only knows the first kind, so the search runs
                // here with both filters.
                const rm::sim::Fx claimed = rm::sim::Fx::fromInt(8);
                const rm::scenario::Marker* best = nullptr;
                rm::sim::Fx bestDistance{};
                for (const rm::scenario::Marker& marker : world_->markers) {
                    if (!marker.isType("Mass")) {
                        continue;
                    }
                    const std::array<rm::sim::Fx, 3> at = rm::app::fxPoint(marker.position);
                    if (plannedNear(at)) {
                        continue;
                    }
                    bool taken = false;
                    for (const rm::sim::Construction& work : scene.building) {
                        if (rm::sim::groundDistanceElmos(work.position, at) < claimed) {
                            taken = true;
                            break;
                        }
                    }
                    if (taken) {
                        continue;
                    }
                    const rm::sim::Fx distance = rm::sim::groundDistanceElmos(home, at);
                    if (best == nullptr || distance < bestDistance) {
                        best = &marker;
                        bestDistance = distance;
                    }
                }
                if (best != nullptr) {
                    site = rm::app::fxPoint(best->position);
                }
            } else {
                // FIRST FREE SLOT, not an ever-growing ring: an incrementing counter walked
                // three hundred structures outward until "is there a radar at this base"
                // honestly answered no — the base had left its own radius. Reusing freed
                // slots keeps the base a base.
                for (int slot = 0; slot < 96; ++slot) {
                    const std::array<rm::sim::Fx, 3> candidate = rm::sim::structureSite(
                        home, world_->centreX, world_->centreZ, slot);
                    bool taken = false;
                    const auto near = [&](rm::sim::Fx ax, rm::sim::Fx az) {
                        const float dx = rm::sim::fxToFloat(ax) - rm::sim::fxToFloat(candidate[0]);
                        const float dz = rm::sim::fxToFloat(az) - rm::sim::fxToFloat(candidate[2]);
                        return dx * dx + dz * dz < 6.0f * 6.0f;
                    };
                    for (rm::UnitIndex slotIndex = 0; slotIndex < scene.store.slotCount();
                         ++slotIndex) {
                        if (!scene.store.health()[slotIndex].alive()) {
                            continue;
                        }
                        const rm::unitdef::UnitDef* standing =
                            scene.catalog.def(scene.store.typeAt(slotIndex));
                        if (standing != nullptr && !standing->isMobile()
                            && near(scene.store.transforms()[slotIndex].x,
                                    scene.store.transforms()[slotIndex].z)) {
                            taken = true;
                            break;
                        }
                    }
                    if (!taken) {
                        for (const rm::sim::Construction& work : scene.building) {
                            if (!work.finished() && near(work.position[0], work.position[2])) {
                                taken = true;
                                break;
                            }
                        }
                    }
                    if (!taken && plannedNear(candidate)) {
                        taken = true;
                    }
                    if (!taken) {
                        site = candidate;
                        break;
                    }
                }
            }
        }
        if (!site) {
            return;
        }
        plannedThisPass_.push_back(*site);
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
