#include "app/FafOpponent.hpp"

#include "app/Match.hpp"
#include "app/Scene.hpp"

#include "core/sim/BuildOrder.hpp"
#include "core/sim/ScriptObject.hpp"
#include "core/lua/LuaTable.hpp"
#include "core/unit/UnitBlueprint.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
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
// Keep each literal below the standard's 64 KiB support limit. Concatenating at
// initialization still evaluates the driver as one Lua chunk with shared locals.
const std::string kFafDriver = std::string{R"lua(
if __rm_faf == nil then

__rm_faf = { brains = {}, armyViews = {}, blueprints = {}, missing = {}, condErrors = {}, cats = {}, threat = {}, scenario = false }
ArmyBrains = {}
function IsAlly(a, b)
    local first, second = ArmyBrains[a], ArmyBrains[b]
    return first ~= nil and second ~= nil and first.alliance == second.alliance
end
function IsEnemy(a, b)
    local first, second = ArmyBrains[a], ArmyBrains[b]
    return first ~= nil and second ~= nil and first.alliance ~= second.alliance
end

local function unitBlueprint(id)
    local key = string.lower(id)
    if not __rm_faf.blueprints[key] then
        -- Read the complete source table: callers need fields outside the simulation's
        -- normalized UnitDef, e.g. Physics.SkirtSizeX in UnitCountBuildConditions.lua:984.
        local bp = __rm_faf_load_blueprint(key)
        -- The deterministic metadata consumed by StructureCheck/EngineerManager;
        -- FAF's system/Blueprints.lua:498,863 derives these from id and Categories.
        bp.BlueprintId = bp.BlueprintId or key
        bp.CategoriesHash = {}
        for _, category in ipairs(bp.Categories or {}) do bp.CategoriesHash[category] = true end
        __rm_faf.blueprints[key] = bp
    end
    return __rm_faf.blueprints[key]
end
GetUnitBlueprintByName = unitBlueprint

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
    if threatType == 'Air' then
        local tags = __rm_faf.cats[u.bp]
        return tags and tags.AIR and (t.a or 0) or 0
    end
    if threatType == 'AntiAir' then return t.a or 0 end
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

-- A unit handle is the packed UnitId: generation in the high 32 bits, index in the low —
-- exact in Lua 5.4's 64-bit integers. The same packing the native side uses, so a test can
-- forge one.
function __rm_faf_handle(index, generation)
    return generation * 4294967296 + index
end

-- Unit objects in snapshots are plain tables; the methods conditions call on them live on
-- this shared metatable. A snapshot only ever holds LIVING units, which is why completion
-- is 1 — a fact, not a guess. BeenDestroyed asks the live store through the script-object
-- seam when the native binding is present (the match), and is false in the bare sandbox.
__rm_faf.unitMeta = {
    __index = {
        BeenDestroyed = function(u)
            if __rm_faf_beenDestroyed then return __rm_faf_beenDestroyed(u.h) end
            return false
        end,
        GetAIBrain = function(u) return u.__brain end,
        IsBeingBuilt = function() return false end,
        GetFractionComplete = function() return 1 end,
        GetHealthPercent = function(u) return u.healthPercent end,
        GetPosition = function(u) return { u.x, 0, u.z } end,
        GetBlueprint = function(u) return unitBlueprint(u.bp) end,
        HasEnhancement = function(u, name) return u.enhancements and u.enhancements[name] == true or false end,
        IsUnitState = function(u, state)
            if state == 'Upgrading' then return u.upgrading == true end
            if state == 'Enhancing' then return u.enhancing == true end
            return false
        end,
    },
}

-- Moho's economy family, in BOTH spellings the corpus uses: free globals, and entries in
-- `moho.aibrain_methods` — the condition files do `local GetEconomyIncome =
-- moho.aibrain_methods.GetEconomyIncome` at IMPORT, capturing whatever sits there forever.
-- That is why this driver must install BEFORE the entry points load: a captured stub is
-- nil-into-arithmetic for the rest of the match.
function GetEconomyIncome(brain, kind) return brain:GetEconomyIncome(kind) end
function GetEconomyRequested(brain, kind) return brain:GetEconomyRequested(kind) end
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
        -- MiscBuildConditions.IsIsland stores nil when no marker exists. That is
        -- a valid cached result, not a missing native API (FAF: lines 285-298).
        if key == 'islandMarker' then return nil end
        recordMissing(key)
        return nil  -- the caller's pcall turns this into a failed condition
    end,
}

local methods = {}
function methods:GetArmyIndex() return self.army + 1 end
function methods:GetFactionIndex() return self.faction end
function methods:GetArmyStartPos() return self.startX, self.startZ end
function methods:IsDefeated() return self.defeated == true end
function methods:GetUnitBlueprint(id) return unitBlueprint(id) end
-- The API comment calls this a number; actual callers require the brain object
-- (base-ai.lua:1125, platoon.lua:2799). Selection follows our existing attack-wave target.
function methods:GetCurrentEnemy()
    return __rm_faf.armyViews[self.snap.currentEnemy]
end
function __rm_faf_army(army, x, z, faction, defeated, alliance)
    local view = __rm_faf.armyViews[army]
    if not view then
        view = { army = army }
        for name, fn in pairs(methods) do view[name] = fn end
        setmetatable(view, brainMeta)
        __rm_faf.armyViews[army] = view
    end
    view.startX, view.startZ, view.faction, view.defeated = x, z, faction, defeated
    view.alliance = alliance
    view.BrainType = 'AI'
    local own = __rm_faf.brains[army]
    view.BuilderManagers = own and own.BuilderManagers or { MAIN = {Position = {x, 0, z}} }
    if own then
        own.startX, own.startZ, own.faction, own.defeated = x, z, faction, defeated
        own.alliance, own.BrainType = alliance, 'AI'
    end
    -- aiutilities.GetAlliesThreat excludes self by object identity. Keep foreign
    -- army views, but publish the actual querying brain at its one-based index.
    ArmyBrains[army + 1] = own or view
end
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
function methods:GetEconomyRequested(kind)
    if kind == 'MASS' then return self.snap.massRequested end
    return self.snap.energyRequested
end
function methods:GetEconomyTrend(kind)
    -- C-071 / retail 0x005968b6–0x005968c2: no per-second multiplier.
    -- The UI's Economy_Trend statistic is a different API.
    return self:GetEconomyIncome(kind) - self:GetEconomyUsage(kind)
end
-- A category expression is REBUILT on every condition call — `categories.LAND * categories.TECH1`
-- allocates fresh nodes — so node identity cannot key a cache; the expression's text can. The
-- key is stored on the node, so the cached atoms under `categories` pay for theirs once.
local function categoryKey(cat)
    -- Some corpus callers hand over a plain string. `__rm_catMatch` has always answered
    -- those with false, so they count zero; keep that answer rather than parsing them here.
    if type(cat) ~= 'table' then return tostring(cat) end
    local key = rawget(cat, '__key')
    if key then return key end
    local op = cat.__cat
    if op == 'tag' then
        key = cat.a
    elseif op == 'neg' then
        key = '-(' .. categoryKey(cat.a) .. ')'
    else
        key = '(' .. categoryKey(cat.a) .. ' ' .. op .. ' ' .. categoryKey(cat.b) .. ')'
    end
    rawset(cat, '__key', key)
    return key
end

-- Snapshot units share immutable blueprint tag sets. Cache category membership per
-- type/expression, not per tank; equivalent expressions are rebuilt by many builders.
local containsCategory = EntityCategoryContains
local membership = {}
function EntityCategoryContains(category, unit)
    local tags = unit and rawget(unit, '__cats')
    if not tags or not unit.bp or __rm_faf.cats[unit.bp] ~= tags then
        return containsCategory(category, unit)
    end
    local memo = membership[tags]
    if not memo then memo = {}; membership[tags] = memo end
    local key = categoryKey(category)
    local result = memo[key]
    if result == nil then
        result = containsCategory(category, unit)
        memo[key] = result
    end
    return result
end

-- Counted once per decision pass per distinct category. Every pass installs a fresh snapshot
-- and then asks about the same few hundred categories over the same units, condition after
-- condition — counting them each time was the one genuine instruction-budget overrun left in
-- the SCMP_009 duel once the watchdog cascade was fixed. The memo is keyed by the snapshot
-- table, so a new pass starts empty and the answer never outlives the units it counted.
local function countCurrentUnits(brain, category)
    if category == nil then return 0 end
    local memo = brain.countMemo
    if memo.snap ~= brain.snap then
        memo = { snap = brain.snap }
        brain.countMemo = memo
    end
    local key = categoryKey(category)
    local n = memo[key]
    if n == nil then
        n = EntityCategoryCount(category, brain.snap.units)
        memo[key] = n
    end
    return n
end

function methods:GetCurrentUnits(category)
    return countCurrentUnits(self, category)
end

-- The brain's reclaim grid, read from `snap.reclaim`, which the native side fills every pass
-- from the same feature pool the harvest pass drains. Retail's GridReclaim.lua is event-driven
-- over prop objects this adapter does not mirror; this view answers the one surface the
-- executed corpus reads — `ReclaimAvailableInGrid` (MiscBuildConditions.lua) asks for the
-- richest cell within a few rings of the base and compares its TotalMass with 10. Cell
-- geometry follows Grid.lua: 16 cells a side (8 on a 256 map), CellSize = max(size) / cells.
-- Platoon readers (ReclaimGridAI, the adaptive reclaim behaviour) also need GridBrain and
-- are not dispatched here.
local GridReclaimView = {}
GridReclaimView.__index = GridReclaimView
local function reclaimGridOf(view)
    local grid = view.brain.snap.reclaim
    if grid then return grid end
    local size = math.max(ScenarioInfo.size[1], ScenarioInfo.size[2])
    return { cellCount = 16, cellSize = size / 16, cells = {} }
end
function GridReclaimView:ToGridSpace(wx, wz)
    local grid = reclaimGridOf(self)
    local function axis(w)
        if not (w > 0) then return 1 end
        return math.min(grid.cellCount, math.floor(w / grid.cellSize) + 1)
    end
    return axis(wx), axis(wz)
end
function GridReclaimView:ToCellFromGridSpace(gx, gz)
    local column = reclaimGridOf(self).cells[gx]
    local cell = column and column[gz]
    return { X = gx, Z = gz,
             TotalMass = cell and cell.mass or 0,
             TotalEnergy = cell and cell.energy or 0,
             ReclaimCount = cell and cell.count or 0 }
end
function GridReclaimView:ToCellFromWorldSpace(wx, wz)
    return self:ToCellFromGridSpace(self:ToGridSpace(wx, wz))
end
-- The richest cell in the square of `radius` rings around (bx, bz); the cell itself when
-- the radius is zero or less. Never nil, matching GridReclaim.lua:213.
function GridReclaimView:MaximumInRadius(bx, bz, radius)
    local best = self:ToCellFromGridSpace(bx, bz)
    if not (radius > 0) then return best end
    local count = reclaimGridOf(self).cellCount
    for x = math.max(1, bx - radius), math.min(count, bx + radius) do
        for z = math.max(1, bz - radius), math.min(count, bz + radius) do
            local cell = self:ToCellFromGridSpace(x, z)
            if cell.TotalMass > best.TotalMass then best = cell end
        end
    end
    return best
end

-- Where a reclaim engineer should go: the richest reclaim-grid cell within `rings` of the
-- base, as world coordinates of its centre plus half a cell as reach, or nil when the richest
-- holds under the ten mass that `ReclaimAvailableInGrid` itself treats as nothing
-- (MiscBuildConditions.lua:369). The retail platoon behaviour
-- (AIPlatoonAdaptiveReclaimBehavior) is a state machine over props this adapter does not
-- run; this is the one decision it would reach first.
function __rm_faf_reclaimTarget(brain, rings)
    local grid = brain.GridReclaim
    local bx, bz = grid:ToGridSpace(brain.startX, brain.startZ)
    local cell = grid:MaximumInRadius(bx, bz, rings or 3)
    if cell.TotalMass < 10 then return nil end
    local size = reclaimGridOf(grid).cellSize
    return (cell.X - 0.5) * size, (cell.Z - 0.5) * size, size * 0.5, cell.TotalMass
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
function methods:PBMGetLocationCoords(location)
    local manager = self.BuilderManagers[location]
    return manager and manager.FactoryManager:GetLocationCoords() or nil
end
methods.GetLocationPosition = methods.PBMGetLocationCoords
function methods:GetUnitsAroundPoint(category, position, radius, alliance)
    alliance = alliance or 'Own'
    assert(alliance == 'Own' or alliance == 'Ally' or alliance == 'Enemy',
        'unknown unit-query alliance: ' .. tostring(alliance))
    radius = radius * 8 -- Public radii are ogrids; snapshot positions are elmos.
    local out = {}
    local function gather(units)
        for _, u in ipairs(units or {}) do
            local dx, dz = u.x - position[1], u.z - position[3]
            if EntityCategoryContains(category, u) and dx * dx + dz * dz <= radius * radius then
                table.insert(out, u)
            end
        end
    end
    if alliance == 'Enemy' then gather(self.snap.enemies)
    else
        gather(self.snap.units)
        if alliance == 'Ally' then gather(self.snap.allies) end
    end
    return out
end
function methods:GetNumUnitsAroundPoint(category, position, radius, alliance)
    return #self:GetUnitsAroundPoint(category, position, radius, alliance)
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
-- Threat over visible enemies in the queried iMAP cell and its neighboring rings.
-- FAF's IMAPSize is in ogrids; our observed positions are in elmos (8 per ogrid).
function methods:GetThreatAtPosition(position, rings, _, threatType)
    local cellSize = self.IMAPConfig.IMAPSize * 8
    local x, z = math.floor(position[1] / cellSize), math.floor(position[3] / cellSize)
    local total = 0
    for _, e in ipairs(self.snap.enemies or {}) do
        if math.abs(math.floor(e.x / cellSize) - x) <= (rings or 0)
            and math.abs(math.floor(e.z / cellSize) - z) <= (rings or 0) then
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

local function assistGuards(brain,u)
    return (u.guardCount or 0)+(brain.assistReservations[u.h] or 0)
end

local function buildingTargets(brain,kind,category,builderCategory,position,radius,location)
    -- Most late-game units cannot be assistees. Index active work once per snapshot,
    -- retaining snapshot unit/work order for the corpus's first-tie selection rule.
    if brain.assistSnapshot~=brain.snap then
        local byBuilder={}
        for _,work in ipairs(brain.snap.underway or {}) do
            if work.builder then
                local list=byBuilder[work.builder]
                if not list then list={}; byBuilder[work.builder]=list end
                list[#list+1]=work
            end
        end
        brain.assistCandidates={}
        for _,u in ipairs(brain.snap.units) do
            if byBuilder[u.h] then
                brain.assistCandidates[#brain.assistCandidates+1]={unit=u,work=byBuilder[u.h]}
            end
        end
        brain.assistSnapshot=brain.snap
        brain.assistMatches={}
    end
    local group=kind=='Engineer' and (categories.ENGINEER-categories.ENGINEERSTATION)
        or kind=='Factory' and categories.FACTORY or categories.ALLUNITS
    local required=group*(builderCategory or categories.ALLUNITS)
    local key=categoryKey(required)..':'..categoryKey(category)
    local matches=brain.assistMatches[key]
    if not matches then
        matches={}
        for _,candidate in ipairs(brain.assistCandidates) do
            if EntityCategoryContains(required,candidate.unit) then
                for _,work in ipairs(candidate.work) do
                    if EntityCategoryContains(category,work) then
                        matches[#matches+1]={unit=candidate.unit,work=work}
                        break
                    end
                end
            end
        end
        brain.assistMatches[key]=matches
    end
    local result={}
    for _,candidate in ipairs(matches) do
        local u=candidate.unit
        if (not location or (brain.unitLocations[u.h] or 'MAIN')==location)
            and (kind~='Engineer' or not u.upgrading)
            and (not position or VDist2(u.x,u.z,position[1],position[3])<=radius) then
            result[#result+1]=candidate
        end
    end
    return result
end

local function assistanceTargets(brain,kind,category,builderCategory,position,radius,location)
    local result={}
    for _,candidate in ipairs(buildingTargets(brain,kind,category,builderCategory,position,radius,location)) do
        local u=candidate.unit
        local limit=brain.assistLimits[u.h]
        -- Building counts do not depend on whether the producer wants helpers.
        if u.desiresAssist~=false and (not limit or assistGuards(brain,u)<limit) then
            result[#result+1]=candidate
        end
    end
    return result
end

function __rm_faf_boot(army, info)
    __rm_faf_scenario(info.markers, info.sizeX, info.sizeZ, info.armies)

    local brain = {
        army = army,
        faction = info.faction,
        startX = info.startX,
        startZ = info.startZ,
        snap = { units = {}, occupied = {} },
        CanPathToEnemy = {},
        hasNavalSite = info.hasNavalSite == true,
        Name = 'rm-faf-' .. tostring(army),
    }
    for name, fn in pairs(methods) do brain[name] = fn end
    -- This adapter has no lobby player name; its actual brain name identifies
    -- diagnostics such as MiscBuildConditions.ReclaimAvailableInGrid.
    brain.Nickname = brain.Name

    -- Flags the corpus reads off the brain. All false, all true statements about this
    -- adapter: no cheat multipliers, no transports requested, no pre-built base.
    brain.CheatEnabled = false
    brain.TransportRequested = false
    brain.PreBuilt = false
    brain.HasPlatoonList = false -- no legacy PBM list; BuilderManagers owns our locations
    brain.islandCheck = false
    brain.islandMarker = false
    brain.LowEnergyMode = false
    brain.LowMassMode = false
    brain.ReclaimFailCounter = 0
    brain.ReclaimFailTimeStamp = 0

    -- Refreshed every decision pass from the snapshot; the corpus's own base-ai maintains
    -- this table from a thread, and the efficiency conditions read it directly.
    brain.EconomyOverTimeCurrent = {}
    brain.economySamples = {}
    brain.economyTotals = {}
    brain.economySampleIndex = 1
    brain.economySampleTick = false
    -- Authored IMAPConfiguration: lua/aibrains/base-ai.lua:1640. Map sizes
    -- arrive here in elmos; keep the public config in FAF's original ogrids.
    local mapSize = math.max(info.sizeX, info.sizeZ) / 8
    if mapSize == 256 or mapSize == 512 then
        brain.IMAPConfig = { OgridRadius = 22.5, IMAPSize = 32, Rings = 2 }
    elseif mapSize == 1024 then
        brain.IMAPConfig = { OgridRadius = 45, IMAPSize = 64, Rings = 1 }
    elseif mapSize == 2048 then
        brain.IMAPConfig = { OgridRadius = 89.5, IMAPSize = 128, Rings = 0 }
    else
        brain.IMAPConfig = { OgridRadius = 180, IMAPSize = 256, Rings = 0 }
    end

    -- Per-builder position in its BuildStructures queue (see the engineer walk).
    brain.progress = {}
    brain.opening = false
    brain.openingDone = {}
    brain.assists = {}
    brain.assistLimits = {}
    brain.assistReservations = {}
    brain.fafTickScale = info.fafTickScale or 1
    brain.scoutSites = info.scoutSites or {}
    brain.scoutAssignments = {}
    brain.scoutVisits = { land = 0, air = 0 }
    brain.scoutSerial = 0
    brain.numOpponents = info.numOpponents or math.max(1, info.armies - 1)
    brain.mapSize = {info.sizeX, info.sizeZ}

    -- The condition cadence cache (see conditionsPass), keyed by spec table, with the
    -- serial that spreads first expiries; and the per-pass unit-count memo (see
    -- countCurrentUnits). Initialised here because the brain's __index records every
    -- unknown field read as a missing method.
    brain.condCache = {}
    brain.condSerial = 0
    brain.countMemo = { snap = false }
    brain.GridReclaim = setmetatable({ brain = brain }, GridReclaimView)

    -- The pool platoon (see GetPlatoonUniquelyNamed): counts over the army's own units.
    -- Pool-at-location and SeaAttackCondition pass manager radii in authored ogrids;
    -- both observed unit positions and manager coordinates are already in elmos.
    brain.pool = {
        GetNumCategoryUnits = function(pool, category, coords, radius)
            local n = 0
            for _, u in ipairs(brain.snap.units) do
                if EntityCategoryContains(category, u)
                    and (not coords or not radius
                         or VDist2(u.x, u.z, coords[1], coords[3]) <= radius*8) then
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
                         or VDist2(u.x, u.z, position[1], position[3]) <= radius*8) then
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
    local function countUnits(location,category)
        local cache=brain.managerCounts
        if not cache or cache.snap~=brain.snap or cache.locations~=brain.unitLocations then
            cache={snap=brain.snap,locations=brain.unitLocations,units={},counts={}}
            for _,unit in ipairs(brain.snap.units) do
                local owner=brain.unitLocations[unit.h] or 'MAIN'
                local list=cache.units[owner]
                if not list then list={}; cache.units[owner]=list end
                list[#list+1]=unit
            end
            brain.managerCounts=cache
        end
        local key=location..':'..categoryKey(category)
        if cache.counts[key]==nil then
            cache.counts[key]=EntityCategoryCount(category,cache.units[location] or {})
        end
        return cache.counts[key]
    end
    -- EngineerManager.lua:37–45,416: counts are restricted to the named
    -- consumption group before applying the caller's category (often just TECH2).
    local consumptionGroups={
        Engineers=categories.ENGINEER-categories.ENGINEERSTATION,
        EngineerStations=categories.ENGINEERSTATION,
        Fabricators=categories.MASSFABRICATION*categories.STRUCTURE,
        Shields=categories.SHIELD*categories.STRUCTURE,
        MobileShields=categories.SHIELD*categories.MOBILE,
        Intel=categories.STRUCTURE*(categories.SONAR+categories.RADAR+categories.OMNI),
        MobileIntel=categories.MOBILE-categories.ENGINEER-categories.SHIELD,
    }
    local manager = {
        LocationType = 'MAIN',
        -- Registration is ownership, not a distance query. FAF transfers engineers
        -- explicitly and finished children inherit their builder's current manager.
        AddUnit = function(self,unit)
            brain.unitLocations[unit.h]=self.LocationType
            brain.managerCounts=nil
        end,
        AddFactory = function(self,unit)
            brain.unitLocations[unit.h]=self.LocationType
            brain.managerCounts=nil
        end,
        RemoveUnit = function(self,unit)
            -- Empty location means explicitly unmanaged; nil is an initial MAIN unit.
            if (brain.unitLocations[unit.h] or 'MAIN')==self.LocationType then brain.unitLocations[unit.h]='' end
            brain.managerCounts=nil
        end,
        GetLocationCoords = coords,
        -- Read as a position by ReclaimAvailableInGrid (`manager.Location[1]`, `[3]`).
        Location = { info.startX, 0, info.startZ },
        Radius = 100, -- aibrains/{base,tech,adaptive,medium,rush,turtle}-ai MAIN setup (ogrids)
        GetNumFactories = function(self)
            return countUnits(self.LocationType,categories.STRUCTURE*categories.FACTORY)
        end,
        GetNumCategoryFactories = function(self,category)
            return countUnits(self.LocationType,categories.STRUCTURE*categories.FACTORY*category)
        end,
        -- The corpus calls this as `engineerManager:GetNumCategoryUnits('Engineers', category)`
        -- (UnitCountBuildConditions.lua:574, :949): the first argument is a GROUP NAME. Binding
        -- the group name as the category made every engineer cap read zero, and the
        -- priority-900 engineer builder never stopped — half of everything built was engineers.
        GetNumCategoryUnits = function(self, group, category)
            local members=consumptionGroups[group]
            return members and countUnits(self.LocationType,members*category) or 0
        end,
        GetNumCategoryBeingBuilt = function(self,category,producerCategory)
            return #buildingTargets(brain,'Engineer',category,producerCategory,nil,nil,self.LocationType)
        end,
        GetEngineersWantingAssistance = function(self,category,engineerCategory)
            local units={}
            for _,candidate in ipairs(assistanceTargets(brain,'Engineer',category,engineerCategory,
                nil,nil,self.LocationType)) do
                units[#units+1]=candidate.unit
            end
            return units
        end,
    }
    local function factoryManager(engineers)
        local factory={}
        for key,value in pairs(engineers) do factory[key]=value end
        factory.GetNumCategoryBeingBuilt=function(self,category,producerCategory)
            return #buildingTargets(brain,'Factory',category,producerCategory,nil,nil,self.LocationType)
        end
        factory.GetFactoriesWantingAssistance=function(self,category,producerCategory)
            local units={}
            for _,candidate in ipairs(assistanceTargets(brain,'Factory',category,producerCategory,
                nil,nil,self.LocationType)) do units[#units+1]=candidate.unit end
            return units
        end
        return factory
    end
    brain.BuilderManagers = {
        MAIN = {
            Position = { info.startX, 0, info.startZ },
            EngineerManager = manager,
            FactoryManager = factoryManager(manager),
        },
    }
    brain.builderLocations = {}
    brain.unitLocations = {}
    if brain.hasNavalSite then
        local naval = {}
        for key, value in pairs(manager) do naval[key] = value end
        naval.LocationType = 'NAVAL'
        naval.Location = { assert(info.navalX, 'naval site x missing'), 0,
                           assert(info.navalZ, 'naval site z missing') }
        naval.GetLocationCoords = function() return naval.Location end
        brain.BuilderManagers.NAVAL = {
            Position = naval.Location, EngineerManager = naval, FactoryManager = factoryManager(naval),
        }
    end
    -- Filled below once the template is known — conditions read it off the location.
    setmetatable(brain, brainMeta)

    -- FAF's own base description drives the list: template -> builder groups -> builders,
    -- flattened and sorted once. Priority first, name as the tiebreak, so the walk order is
    -- a fact about the data rather than about table iteration.
    local base = info.base
    if base == 'adaptive' or base == 'random' then
        ScenarioInfo.ArmySetup[brain.Name] = { AIPersonality = base }
        -- FAF GetHighestBuilder scores every main-base template. Sort names before
        -- consuming randomness so Lua table iteration cannot change a replay.
        local candidates = {}
        for name, candidate in pairs(BaseBuilderTemplates) do
            if candidate.FirstBaseFunction then table.insert(candidates, name) end
        end
        table.sort(candidates)
        local savedRandom, savedMapSize = Random, GetMapSize
        -- Local xorshift32, as in core/scene/Particles.cpp. This selects a strategy,
        -- not combat outcomes; repeat the same map/army setup for the same selection.
        local state = (info.sizeX ~ (info.sizeZ << 16) ~ (army + 1)) & 0xffffffff
        if state == 0 then state = 1 end
        Random = function(low, high)
            state = (state ~ (state << 13)) & 0xffffffff
            state = (state ~ (state >> 17)) & 0xffffffff
            state = (state ~ (state << 5)) & 0xffffffff
            return low + state % (high - low + 1)
        end
        -- Authored map thresholds use ogrids; our positions and scenario size use elmos.
        GetMapSize = function() return info.sizeX / 8, info.sizeZ / 8 end
        brain.GetMapWaterRatio = function() return info.waterRatio end
        local best, selected, personality = 0
        local ok, err = pcall(function()
            for _, name in ipairs(candidates) do
                local score, kind = BaseBuilderTemplates[name].FirstBaseFunction(brain)
                -- Some authored functions fall through at exact map-size boundaries.
                if score and score > best then best, selected, personality = score, name, kind end
            end
        end)
        Random, GetMapSize = savedRandom, savedMapSize
        if not ok then error('FAF base selection failed: ' .. tostring(err)) end
        assert(selected, 'FAF base selection found no eligible template')
        ScenarioInfo.ArmySetup[brain.Name].AIBase = selected
        if base == 'random' then ScenarioInfo.ArmySetup[brain.Name].AIPersonality = personality end
        base = selected
    end
    brain.baseTemplate = base
    brain.openingRushAir = info.base=='RushMainAir' or (info.base=='random'
        and ScenarioInfo.ArmySetup[brain.Name].AIPersonality=='rushair')
    local template = assert(BaseBuilderTemplates[base], 'unknown FAF base: ' .. tostring(base))
    local list = {}
    local added = {}
    local function addGroup(groupName)
        local group = BuilderGroups[groupName]
        if not group then return end
        local naval = groupName == 'EngineerNavalFactoryBuilder'
            or groupName == 'T1SeaFactoryBuilders' or groupName == 'T2SeaFactoryBuilders'
            or groupName == 'T3SeaFactoryBuilders' or groupName == 'FrequentSeaAttackFormBuilders'
        if naval and not brain.hasNavalSite then return end
        for _, builderName in ipairs(group) do
            local spec = Builders[builderName]
            local supported = spec ~= nil
            if groupName == 'FrequentSeaAttackFormBuilders' then
                supported = builderName == 'Frequent Sea Attack T1'
                    or builderName == 'Frequent Sea Attack T2'
                    or builderName == 'Frequent Sea Attack T3'
            end
            if supported and not added[builderName] then
                added[builderName] = true
                brain.builderLocations[spec] = naval and 'NAVAL' or 'MAIN'
                table.insert(list, { spec = spec, kind = group.BuildersType })
            end
        end
    end
    if template then
        for _, groupName in ipairs(template.Builders or {}) do addGroup(groupName) end
        for _, groupName in ipairs(template.NonCheatBuilders or {}) do addGroup(groupName) end
    end
    if brain.hasNavalSite then
        -- Surface production and factory upgrades share the ordinary tier/build-tree
        -- checks. Submersible products still require their separate underwater support.
        addGroup('EngineerNavalFactoryBuilder')
        addGroup('T1SeaFactoryBuilders')
        addGroup('T2SeaFactoryBuilders')
        addGroup('T3SeaFactoryBuilders')
        addGroup('FrequentSeaAttackFormBuilders')
    end
    table.sort(list, function(a, b)
        local pa, pb = a.spec.Priority or 0, b.spec.Priority or 0
        if pa ~= pb then return pa > pb end
        return (a.spec.BuilderName or '') < (b.spec.BuilderName or '')
    end)
    brain.builders = list
    if template then
        -- Per-brain copy: raising MAIN's sea cap must not mutate NormalMain globally and make
        -- a dry brain think it owns a naval lane too.
        local settings = {}
        for key, value in pairs(template.BaseSettings or {}) do settings[key] = value end
        local factoryCount = {}
        for key, value in pairs(settings.FactoryCount or {}) do factoryCount[key] = value end
        if brain.hasNavalSite then factoryCount.Sea = 1 end
        settings.FactoryCount = factoryCount
        brain.BuilderManagers.MAIN.BaseSettings = settings
        if brain.BuilderManagers.NAVAL then brain.BuilderManagers.NAVAL.BaseSettings = settings end
    end

    brain.buildingTemplates = import('/lua/buildingtemplates.lua').BuildingTemplates
    __rm_faf.brains[army] = brain
    local view = __rm_faf.armyViews[army]
    __rm_faf_army(army, info.startX, info.startZ, info.faction,
        view and view.defeated or false, view and view.alliance or army)

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
            local platoon = PlatoonTemplates[item.spec.PlatoonTemplate]
            if platoon and platoon.Plan == 'CommanderInitialBOAI' then
                for _, structure in ipairs({'T1LandFactory','T1AirFactory','T1SeaFactory',
                    'T1EnergyProduction','T1Resource'}) do want(buildingIdFor(brain,structure)) end
            end
            for _, structure in ipairs((construction and construction.BuildStructures) or {}) do
                want(buildingIdFor(brain, structure))
            end
        end
    end
    wanted.baseTemplate = base
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
    if cached and now < cached.expires then
        return cached.result
    end
    local result, failure = true, 'none'
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
                failure = tostring(cond[1]) .. ':' .. tostring(cond[2]) .. ' (missing)'
                result = false
                break
            end
        end
        if result then
            -- 'LocationType' is FAF's placeholder, substituted per base when a real manager
            -- instantiates a builder. Naval groups use the shipyard's own location.
            local actual = {}
            for i, v in ipairs(args) do
                actual[i] = (v == 'LocationType') and (brain.builderLocations[spec] or 'MAIN') or v
            end
            -- This corpus condition compares VDist3 of our elmo-valued marker/base
            -- positions with its authored ogrid distance (UnitCountBuildConditions).
            if cond[2]=='CanBuildOnHydroLessThanDistance' then actual[2]=actual[2]*8 end
            local ok, value = pcall(fn, brain, unpack(actual))
            if not ok then
                local key = tostring(value)
                __rm_faf.condErrors[key] = (__rm_faf.condErrors[key] or 0) + 1
                result = false
            elseif not value then
                result = false
            end
        end
        if not result then
            failure = type(cond[1])=='function' and 'inline condition' or tostring(cond[2])
            break
        end
    end
    -- A spec's FIRST expiry is spread over three passes (passes run every ten ticks), and
    -- it keeps that phase afterwards. Without this every builder was first evaluated in the
    -- same pass and so re-evaluated together every third pass; late in the SCMP_009 duel,
    -- with ~200 engineers to count per condition, that peak alone exhausted the instruction
    -- budget. The serial is per brain and assigned in walk order, so the phases are the
    -- same on every machine and every run.
    local hold = 30
    if not cached then
        brain.condSerial = brain.condSerial + 1
        hold = hold + 10 * (brain.condSerial % 3)
    end
    brain.condCache[spec] = { expires = now + hold, result = result,
        checkedAt = now, failure = failure, checks = (cached and cached.checks or 0)+1,
        passes = (cached and cached.passes or 0)+(result and 1 or 0) }
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
        if item.kind == kindName and (item.spec.Priority or 0) > 0
            and conditionsPass(brain, item.spec) and visit(item) then
            return
        end
    end
end

)lua"} + R"lua(
-- CommanderInitialBOAI, platoon.lua:4549–5015. Keep its resource-dependent phases
-- across observations; the native command queue owns construction and approach work.
-- All coordinates are elmos, so the authored squared-ogrid thresholds multiply by 64.
local function advanceOpening(brain, decisions)
    local opening = brain.opening
    if not opening then return end
    local snap = brain.snap
    local acu
    for _, u in ipairs(snap.units) do if u.h == opening.builder then acu=u; break end end
    if not acu then brain.opening=false; return end
    if opening.waitQueue and (acu.queueBusy or acu.building) then return end
    opening.waitQueue=false
    local now=snap.tick or 0
    local issued=0
    local function emit(kind, data)
        data=data or {}
        data.kind, data.builder, data.name = kind, acu.h, opening.name
        data.queued=issued>0
        table.insert(decisions,data)
        issued=issued+1
    end
    local function build(structure, site, anchor)
        local bp=buildingIdFor(brain,structure)
        if not bp then error('opening building template missing '..structure) end
        -- The app seeds one mex before the AI starts. It remains in the survey's
        -- original marker count, but a claimed marker must not become a second build.
        if site and not brain:CanBuildStructureAt(bp,{site[1],0,site[2]}) then return end
        emit('build',{bp=bp,structure=structure,site=site,anchor=anchor})
    end
    local function distance(site)
        return (acu.x-site[1])^2+(acu.z-site[2])^2
    end
    local function approachMass(site, after)
        emit('move',{x=site[1],z=site[2]})
        opening.massSite,opening.afterMassMove,opening.phase=site,after,'massMove'
    end
    local function wait(phase)
        opening.phase,opening.waitQueue=phase,true
    end
    local survey=opening.survey
    if opening.phase=='initial' then
        build(survey.inWater and 'T1SeaFactory'
            or (opening.rushair and 'T1AirFactory' or 'T1LandFactory'))
        if #survey.close>0 then
            build('T1Resource',table.remove(survey.close,1))
            wait('powerMass')
        elseif #survey.distant>0 then
            approachMass(table.remove(survey.distant,1),'powerMass')
        else wait('powerMass') end
        return
    end
    if opening.phase=='massMove' then
        if distance(opening.massSite)>165*64 and acu.queueBusy then return end
        emit('stop')
        build('T1Resource',opening.massSite)
        wait(opening.afterMassMove)
        return
    end
    if opening.phase=='powerMass' then
        for i=1,(survey.hydro and 1 or 2) do build('T1EnergyProduction') end
        local closeRemaining=#survey.close
        if #survey.close>=3 then
            for i=1,2 do build('T1Resource',table.remove(survey.close,1)) end
            build('T1EnergyProduction')
        end
        for _,site in ipairs(survey.close) do build('T1Resource',site) end
        survey.close={}
        if #survey.distant>0 and #survey.distant<3 and closeRemaining==0 then
            approachMass(table.remove(survey.distant,1),'distantMass')
        else wait('afterMass') end
        return
    end
    if opening.phase=='distantMass' then
        while #survey.distant>0 do
            local site=table.remove(survey.distant,1)
            if brain:CanBuildStructureAt(buildingIdFor(brain,'T1Resource'),{site[1],0,site[2]}) then
                approachMass(site,'distantMass')
                return
            end
        end
        opening.phase='afterMass'
    end
    if opening.phase=='afterMass' then
        if not survey.hydro then
            local count=3
            if opening.closeCount>0 and opening.closeCount<4 and opening.distantCount<=1 then count=2 end
            for i=1,count do build('T1EnergyProduction') end
            if opening.closeCount>3 then build('T1LandFactory') end
            wait('finish')
            return
        end
        if opening.closeCount+opening.distantCount==0 then brain.opening=false; return end
        opening.phase='hydroMove'
        if distance(survey.hydro)>144*64 then
            emit('move',{x=survey.hydro[1],z=survey.hydro[2]})
            return
        end
    end
    if opening.phase=='hydroMove' then
        if distance(survey.hydro)>100*64 and acu.queueBusy then return end
        emit('stop')
        opening.phase,opening.polls,opening.wake='hydroFind',0,now
    end
    if opening.phase=='hydroFind' then
        if now<opening.wake then return end
        local best,low,any
        for _,work in ipairs(snap.underway or {}) do
            if EntityCategoryContains(categories.HYDROCARBON,work) then
                for _,u in ipairs(snap.units) do
                    if u.h==work.builder and EntityCategoryContains(categories.ENGINEER,u) then
                        any=true
                        local dist=(acu.x-u.x)^2+(acu.z-u.z)^2
                        if (not low or dist<low) and dist<225*64 and (u.guardCount or 0)<20 then
                            best,low={builder=u.h,x=work.x,z=work.z},dist
                        end
                    end
                end
            end
        end
        if best then
            opening.assistee=best
            emit('guard',{target=best.builder})
            opening.phase,opening.wake='hydroGuard',now+30*brain.fafTickScale
        elseif any or opening.polls>=11 then brain.opening=false
        else
            opening.polls=opening.polls+1
            opening.wake=now+15*brain.fafTickScale
        end
        return
    end
    if opening.phase=='hydroGuard' then
        if now<opening.wake then return end
        local target=opening.assistee
        local finished,underway,targetAlive=false,false,false
        for _,u in ipairs(snap.units) do
            if u.h==target.builder then targetAlive=true end
            if EntityCategoryContains(categories.HYDROCARBON,u)
                and (u.x-target.x)^2+(u.z-target.z)^2<64 then finished=true end
        end
        for _,work in ipairs(snap.underway or {}) do
            if work.builder==target.builder and work.x==target.x and work.z==target.z
                and EntityCategoryContains(categories.HYDROCARBON,work) then underway=true; break end
        end
        if not finished and underway and targetAlive and acu.queueBusy then
            opening.wake=now+30*brain.fafTickScale
            return
        end
        emit('stop')
        local count=opening.closeCount+opening.distantCount
        if finished and (count>2 or (count>1 and brain:GetEconomyStored('MASS')>120)) then
            local width,height=brain.mapSize[1]/8,brain.mapSize[2]/8
            if width>512 or height>512 or opening.rushair then
                build('T1AirFactory',nil,survey.hydro)
            else
                build('T1LandFactory',nil,survey.hydro)
                -- Preserve the source's AND precedence on the height branch.
                if width>256 or (height>256
                    and brain:GetEngineerManagerUnitsBeingBuilt(categories.FACTORY*categories.AIR)<1
                    and brain:GetCurrentUnits(categories.FACTORY*categories.AIR)<1) then
                    build('T1AirFactory',nil,survey.hydro)
                end
            end
            wait('finish')
        else brain.opening=false end
        return
    end
    if opening.phase=='finish' then brain.opening=false end
end

-- --- The decision pass ---------------------------------------------------------------------
--
-- One structure at a time, factories minus work in flight, one attack wave per pass: the
-- serialization is the stand-in for the manager stack, stated here once. What fires within
-- those slots is entirely the corpus's call.

local function sampleEconomy(brain,snap)
    -- base-ai.lua:EconomyMonitor: 300 FAF ticks, sampled every ten ticks, with
    -- thirty initially-zero slots. Keep instantaneous brain queries separate:
    -- GreaterThanEconEfficiencyCombined deliberately checks both time scales.
    local now=snap.tick or 0
    if brain.economySampleTick and now<brain.economySampleTick+10*brain.fafTickScale then return end
    brain.economySampleTick=now
    local values={MassIncome=snap.massIncome,EnergyIncome=snap.energyIncome,
        MassRequested=snap.massRequested,EnergyRequested=snap.energyRequested,
        MassTrendOverTime=brain:GetEconomyTrend('MASS'),
        EnergyTrendOverTime=brain:GetEconomyTrend('ENERGY')}
    local index=brain.economySampleIndex
    local previous=brain.economySamples[index] or {}
    local totals,eco=brain.economyTotals,brain.EconomyOverTimeCurrent
    for name,value in pairs(values) do
        totals[name]=(totals[name] or 0)-(previous[name] or 0)+value
        eco[name]=totals[name]/30
    end
    brain.economySamples[index]=values
    brain.economySampleIndex=index%30+1
    eco.MassEfficiencyOverTime=math.min(totals.MassIncome/totals.MassRequested,2)
    eco.EnergyEfficiencyOverTime=math.min(totals.EnergyIncome/totals.EnergyRequested,2)
end

local function advanceAssists(brain,decisions)
    local living={}
    for _,u in ipairs(brain.snap.units) do living[u.h]=u end
    local handles={}
    for h in pairs(brain.assists) do handles[#handles+1]=h end
    table.sort(handles)
    for _,h in ipairs(handles) do
        local state=brain.assists[h]
        local helper=living[h]
        local now=brain.snap.tick or 0
        local release=not helper
        if helper and state.untilFinished and now>=state.checkAt then
            local underway=false
            for _,work in ipairs(brain.snap.underway or {}) do
                if work.builder==state.target and work.bp==state.bp
                    and work.command==state.command and work.remaining==state.remaining
                    and work.x==state.x and work.z==state.z then underway=true; break end
            end
            release=not living[state.target] or not underway or helper.queueBusy==false
            state.checkAt=now+15*brain.fafTickScale
        elseif helper and not state.untilFinished then
            release=now>=state.expires
        end
        if release then
            if helper then decisions[#decisions+1]={kind='stop',builder=h} end
            brain.assists[h]=nil
        end
    end
end

local function startAssist(brain,item,helper,decisions)
    local data=item.spec.BuilderData.Assist
    local kind=data.AssisteeType
    if kind~='Engineer' and kind~='Factory' and kind~='Structure' then
        recordMissing('assist target '..tostring(kind)); return false
    end
    local location=data.AssistLocation
    if location=='LocationType' then location=brain.builderLocations[item.spec] or 'MAIN' end
    local base=location and brain.BuilderManagers[location]
    if not base then return false end
    local count=0
    for _,state in pairs(brain.assists) do
        if state.name==item.spec.BuilderName then count=count+1 end
    end
    if count>=(item.spec.InstanceCount or 1) then return false end
    local builderCategory=data.AssisteeCategory or categories.ALLUNITS
    if type(builderCategory)=='string' then builderCategory=ParseEntityCategory(builderCategory) end
    local best,low
    for _,name in ipairs(data.BeingBuiltCategories or {'ALLUNITS'}) do
        local candidates=assistanceTargets(brain,kind,ParseEntityCategory(name),builderCategory,
            kind=='Structure' and base.Position or nil,base.EngineerManager.Radius*8,
            kind~='Structure' and location or nil)
        for _,candidate in ipairs(candidates) do
            local u=candidate.unit
            local guards=assistGuards(brain,u)
            local distance=VDist2(helper.x,helper.z,u.x,u.z)
            local score=data.AssistClosestUnit and distance or guards
            if u.h~=helper.h and guards<20 and distance<(data.AssistRange or 80)*8
                and (not low or score<low) then best,low=candidate,score end
        end
        -- EconAssistBody stops at the first nonempty category, even if out of range.
        if #candidates>0 then break end
    end
    if not best then return false end
    local now=brain.snap.tick or 0
    local work=best.work
    brain.assists[helper.h]={target=best.unit.h,name=item.spec.BuilderName,
        untilFinished=data.AssistUntilFinished,checkAt=now+10*brain.fafTickScale,
        expires=now+(10+(data.Time or 60)*10)*brain.fafTickScale,
        bp=work.bp,command=work.command,remaining=work.remaining,x=work.x,z=work.z}
    brain.assistReservations[best.unit.h]=(brain.assistReservations[best.unit.h] or 0)+1
    -- Foundations have no native unit handle yet. Guard their actual builder,
    -- and use the build command identity to release the helper when that work ends.
    decisions[#decisions+1]={kind='guard',builder=helper.h,target=best.unit.h,name=item.spec.BuilderName}
    return true
end

function __rm_faf_decide(army, snap)
    local brain = __rm_faf.brains[army]
    if not brain then return {} end
    brain.snap = snap
    local locations={}
    for _,u in ipairs(snap.units) do locations[u.h]=brain.unitLocations[u.h] or 'MAIN' end
    -- A surviving foundation can finish after its founder dies. Keep the ownership
    -- needed by its eventual completion event until that work leaves the snapshot.
    for _,work in ipairs(snap.underway or {}) do
        if work.builder and brain.unitLocations[work.builder] then
            locations[work.builder]=brain.unitLocations[work.builder]
        end
    end
    brain.unitLocations=locations
    brain.assistReservations={}
    brain.assistSnapshot=nil
    if snap.currentEnemy ~= nil and snap.enemyPath then
        local ownIndex, enemyIndex = brain:GetArmyIndex(), snap.currentEnemy + 1
        brain.CanPathToEnemy[ownIndex] = brain.CanPathToEnemy[ownIndex] or {}
        brain.CanPathToEnemy[ownIndex][enemyIndex] = { MAIN = snap.enemyPath }
    end
    for _, u in ipairs(snap.units) do u.__brain = brain end
    local naval = brain.BuilderManagers.NAVAL
    if naval then
        for _, u in ipairs(snap.units) do
            if EntityCategoryContains(categories.FACTORY * categories.NAVAL, u) then
                naval.Position[1], naval.Position[3] = u.x, u.z
                break
            end
        end
    end

    sampleEconomy(brain,snap)

    local decisions = {}
    advanceAssists(brain,decisions)
    advanceOpening(brain,decisions)

    -- Idle engineers first, commander last. Native construction owns its builder,
    -- including the approach order before a foundation exists. A global underway count
    -- cannot identify which pool member is free and used to overwrite busy builders.
    local builderPool = {}
    for _, u in ipairs(snap.units) do
        if u.idle and not u.reclaiming and not u.building
            and not brain.assists[u.h]
            and (not brain.opening or brain.opening.builder~=u.h)
            and EntityCategoryContains(categories.ENGINEER - categories.COMMAND, u) then
            table.insert(builderPool, u)
        end
    end
    for _, u in ipairs(snap.units) do
        if u.idle and not u.reclaiming and not u.building
            and not brain.assists[u.h]
            and (not brain.opening or brain.opening.builder~=u.h)
            and EntityCategoryContains(categories.COMMAND, u) then
            table.insert(builderPool, u)
        end
    end
    local slots = #builderPool

    if slots > 0 then
        walkPriority(brain, 'EngineerBuilder', function(item)
            -- Select the unit required by this platoon, not simply the first free engineer.
            -- Otherwise a T1 engineer hides both an idle commander and higher-tier engineers.
            local firstFree = #builderPool - slots + 1
            local selected = firstFree
            local template = PlatoonTemplates[item.spec.PlatoonTemplate]
            local squads = template and (template.GlobalSquads
                or (template.FactionSquads and template.FactionSquads[factionNames[brain.faction]]))
            local required = squads and squads[1] and squads[1][1]
            -- FactionSquads name blueprint IDs; GlobalSquads use category expressions.
            if type(required) == 'string' then required = ParseEntityCategory(required) end
            local assistPlan=template and template.Plan=='ManagerEngineerAssistAI'
            if required or assistPlan then
                selected = nil
                for index = firstFree, #builderPool do
                    local candidate=builderPool[index]
                    if (not required or EntityCategoryContains(required,candidate))
                        and (not assistPlan or brain.unitLocations[candidate.h]
                            ==(brain.builderLocations[item.spec] or 'MAIN')) then
                        selected = index
                        break
                    end
                end
                if not selected then return false end
            end
            local nextBuilder = builderPool[selected]
            local data = item.spec.BuilderData
            local function reserveBuilder()
                -- EngineerManager.AssignEngineerTask resets then applies this metadata
                -- for every selected platoon, including assistance and opening plans.
                brain.assistLimits[nextBuilder.h]=data and data.NumAssistees or nil
                builderPool[firstFree], builderPool[selected] = builderPool[selected], builderPool[firstFree]
                slots = slots - 1
            end
            local builderTag =
                (EntityCategoryContains(categories.COMMAND, nextBuilder) and 'BUILTBYCOMMANDER')
                or (EntityCategoryContains(categories.TECH3, nextBuilder) and 'BUILTBYTIER3ENGINEER')
                or (EntityCategoryContains(categories.TECH2, nextBuilder) and 'BUILTBYTIER2ENGINEER')
                or 'BUILTBYTIER1ENGINEER'
            if template and template.Plan=='ManagerEngineerAssistAI' and data and data.Assist then
                if not startAssist(brain,item,nextBuilder,decisions) then return false end
                reserveBuilder()
                return slots<=0
            end
            if template and template.Plan=='CommanderInitialBOAI' then
                if brain.opening or brain.openingDone[item.spec.BuilderName]
                    or not __rm_faf_opening_survey then return false end
                local survey=__rm_faf_opening_survey(nextBuilder.h)
                if not survey then return false end
                brain.opening={builder=nextBuilder.h,name=item.spec.BuilderName,phase='initial',
                    survey=survey,closeCount=#survey.close,distantCount=#survey.distant,
                    rushair=brain.openingRushAir}
                brain.openingDone[item.spec.BuilderName]=true
                advanceOpening(brain,decisions)
                reserveBuilder()
                return slots<=0
            end
            if data and data.Enhancement then
                local sequence = {}
                for _, name in ipairs(data.Enhancement) do
                    if not nextBuilder:HasEnhancement(name) then table.insert(sequence, name) end
                end
                -- EnhanceAI queues all scripts first; TimeBetweenEnhancements delays
                -- its completion polling, not the individual enhancement commands.
                if not template or template.Plan ~= 'EnhanceAI'
                    or not EntityCategoryContains(template.GlobalSquads[1][1], nextBuilder)
                    or not __rm_faf_enhancement_sequence
                    or not __rm_faf_enhancement_sequence(nextBuilder.h, sequence) then return false end
                for index, enhancement in ipairs(sequence) do
                    table.insert(decisions, {kind='enhance',builder=nextBuilder.h,
                        enhancement=enhancement,queued=index > 1,name=item.spec.BuilderName})
                end
                reserveBuilder()
                return slots <= 0
            end
            -- Reclaim builders carry no structure queue; they hand an engineer to the
            -- adaptive reclaim state machine. The decision that machine reaches first is
            -- "go to the richest cell near the base", which the match turns into a reclaim
            -- order on the best wreck there. `mapSearch` (the Excess builders' second
            -- condition argument) widens the search from three rings to eight, as the
            -- condition itself does.
            if data and data.StateMachine == 'AIPlatoonAdaptiveReclaimBehavior' then
                -- InstanceCount: how many reclaim platoons this builder may run at once.
                -- Engineers already reclaiming are those platoons.
                local active = 0
                for _, u in ipairs(snap.units) do
                    if u.reclaiming then active = active + 1 end
                end
                if active >= (item.spec.InstanceCount or 1) then return false end
                local rings = 3
                for _, cond in ipairs(item.spec.BuilderConditions or {}) do
                    if cond[2] == 'ReclaimAvailableInGrid' and cond[3] and cond[3][2] then
                        rings = 8
                    end
                end
                local x, z, radius = __rm_faf_reclaimTarget(brain, rings)
                if not x then return false end
                local builderUnit = nextBuilder
                table.insert(decisions, {
                    kind = 'reclaim', builder = builderUnit.h, name = item.spec.BuilderName,
                    x = x, z = z, radius = radius,
                })
                reserveBuilder()
                return slots <= 0
            end
            local construction = data and data.Construction
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
                    local allowed = cats and cats[builderTag]
                    if cats and builderTag == 'BUILTBYCOMMANDER' then
                        local installed = nextBuilder.enhancements or {}
                        local t3 = installed.T3Engineering
                        local t2 = t3 or installed.AdvancedEngineering
                        allowed = allowed or (t2 and cats.BUILTBYTIER2COMMANDER)
                            or (t3 and cats.BUILTBYTIER3COMMANDER)
                    end
                    if cats and not allowed then
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
                    local builderUnit = nextBuilder
                    table.insert(decisions, {
                        kind = 'build', bp = bp, structure = structure,
                        builder = builderUnit.h, name = item.spec.BuilderName,
                    })
                    reserveBuilder()
                    return slots <= 0
                end
            end
            brain.progress[item.spec.BuilderName] = progress
            return false
        end)
    end

    -- Reserve the upgrade slot before production. A factory's "idle" flag only means
    -- stationary: let its current product finish, then upgrade before training again.
    -- Keep the accepted intent across economy fluctuations, like an assigned upgrade
    -- platoon, but discard it when the generation-bearing handle disappears or upgrades.
    local upgrade = rawget(brain, 'pendingFactoryUpgrade') -- private state, not a native API
    local upgradeUnit
    if upgrade then
        for _, u in ipairs(snap.units) do
            if u.h == upgrade.builder and not u.upgrading then upgradeUnit = u; break end
        end
        if not upgradeUnit then upgrade = nil end
    end
    if not upgrade then
        walkPriority(brain, 'PlatoonFormBuilder', function(item)
            local template = PlatoonTemplates[item.spec.PlatoonTemplate]
            local squads = template and template.GlobalSquads
            if not squads or template.Plan ~= 'UnitUpgradeAI' then return false end
            for _, u in ipairs(snap.units) do
                if u.idle and not u.upgrading and EntityCategoryContains(squads[1][1], u) then
                    upgradeUnit = u
                    upgrade = { kind = 'upgrade', builder = u.h, name = item.spec.BuilderName }
                    return true
                end
            end
            return false
        end)
    end
    brain.pendingFactoryUpgrade = nil
    if upgrade then
        if upgradeUnit.building
            and EntityCategoryContains(categories.FACTORY, upgradeUnit) then
            brain.pendingFactoryUpgrade = upgrade
        else
            table.insert(decisions, upgrade)
        end
    end

    -- Factories: the corpus picks the unit, one train per factory not already working.
    local factories = {}
    for _, u in ipairs(snap.units) do
        if u ~= upgradeUnit and not u.upgrading
            and not u.building
            and EntityCategoryContains(categories.STRUCTURE * categories.FACTORY, u) then
            table.insert(factories, u)
        end
    end
    -- Only factories with nothing on the floor are candidates, so every train below lands
    -- on a factory that can take it; the sim would refuse one mid-product without a word.
    local free = #factories
    if free > 0 then
        walkPriority(brain, 'FactoryBuilder', function(item)
            local template = PlatoonTemplates[item.spec.PlatoonTemplate]
            local squads = template and template.FactionSquads
            local squad = squads and squads[factionNames[brain.faction]]
            local bp = squad and squad[1] and squad[1][1]
            if not bp then return false end
            -- Combat products use their factory's domain — the unit id's third letter is FAF's
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
            -- Factory Economy.BuildableCategory also permits MOBILE CONSTRUCTION
            -- across domains (e.g. UEB0302 builds the land engineer UEL0309).
            local construction = unitCats and unitCats.MOBILE and unitCats.CONSTRUCTION
            for _, factory in ipairs(factories) do
                local tierTag = (EntityCategoryContains(categories.TECH3, factory)
                                     and 'BUILTBYTIER3FACTORY')
                    or (EntityCategoryContains(categories.TECH2, factory)
                            and 'BUILTBYTIER2FACTORY')
                    or 'BUILTBYTIER1FACTORY'
                if not factory.__taken and (construction or EntityCategoryContains(need, factory))
                    and unitCats and unitCats[tierTag] and not unitCats.SUBMERSIBLE then
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

    -- ScoutingAI owns a separate formation slot and keeps its assignments until arrival
    -- or death. FAF platoon.lua:1190 alternates enemy-base visits with low-priority areas.
    local living = {}
    for _, u in ipairs(snap.units) do living[u.h] = u end
    for h, assignment in pairs(brain.scoutAssignments) do
        local u = living[h]
        if not u or (u.idle and not u.scoutingBusy and (snap.tick or 0) > assignment.tick) then
            brain.scoutAssignments[h] = nil
        end
    end
    for _, enemy in ipairs(snap.enemies or {}) do
        if EntityCategoryContains(categories.STRUCTURE - categories.MASSEXTRACTION, enemy) then
            local found = false
            for _, site in ipairs(brain.scoutSites) do
                if (site.x-enemy.x)^2 + (site.z-enemy.z)^2 < (100*8)^2 then
                    site.high = true; found = true; break
                end
            end
            if not found then table.insert(brain.scoutSites, {x=enemy.x,z=enemy.z,high=true}) end
        end
    end
    walkPriority(brain, 'PlatoonFormBuilder', function(item)
        local template = PlatoonTemplates[item.spec.PlatoonTemplate]
        if not template or template.Plan ~= 'ScoutingAI' then return false end
        local squad = template.GlobalSquads and template.GlobalSquads[1]
        if not squad then return false end
        local active = 0
        local reserved = {}
        for _, assignment in pairs(brain.scoutAssignments) do
            reserved[assignment.site] = true
            if assignment.name == item.spec.BuilderName then active = active + 1 end
        end
        if active >= (item.spec.InstanceCount or 1) then return false end
        for _, u in ipairs(snap.units) do
            if u.idle and not u.scoutingBusy and not brain.scoutAssignments[u.h]
                and EntityCategoryContains(squad[1], u) then
                local air = EntityCategoryContains(categories.AIR, u)
                local layer = air and 'air' or 'land'
                local preferHigh = brain.scoutVisits[layer] < brain.numOpponents
                local sites = {}
                for _, site in ipairs(brain.scoutSites) do
                    if not reserved[site] then table.insert(sites, site) end
                end
                table.sort(sites, function(a,b)
                    if a.high ~= b.high then return a.high == preferHigh end
                    if (a.visited or 0) ~= (b.visited or 0) then return (a.visited or 0) < (b.visited or 0) end
                    local da, db = (a.x-brain.startX)^2+(a.z-brain.startZ)^2,
                                   (b.x-brain.startX)^2+(b.z-brain.startZ)^2
                    if da ~= db then return da < db end
                    if a.x ~= b.x then return a.x < b.x end
                    return a.z < b.z
                end)
                for _, site in ipairs(sites) do
                    local x,z = site.x,site.z
                    if air then
                        local dx,dz = x-u.x,z-u.z
                        local length = math.sqrt(dx*dx+dz*dz)
                        if length == 0 then dx,dz,length = 1,0,1 end
                        local side = u.h % 2 == 0 and 1 or -1
                        -- DoAirScoutVecs: vision-radius lateral offset and 75 ogrids forward.
                        x = x + dz/length*u.vision*side + dx/length*75*8
                        z = z - dx/length*u.vision*side + dz/length*75*8
                        x = math.max(40, math.min(brain.mapSize[1]-40,x))
                        z = math.max(40, math.min(brain.mapSize[2]-40,z))
                    end
                    local route = __rm_faf_scout_route(u.h,x,z,brain.IMAPConfig.IMAPSize*8,brain.IMAPConfig.Rings)
                    -- platoon.lua:1225-1228 always issues the destination, even when
                    -- threat-aware routing fails. Ordinary movement still checks terrain.
                    if #route == 0 then route = {{x,z}} end
                    if #route > 0 then
                        brain.scoutSerial = brain.scoutSerial + 1
                        site.visited = brain.scoutSerial
                        brain.scoutVisits[layer] = site.high and (brain.scoutVisits[layer]+1) or 0
                        brain.scoutAssignments[u.h] = {site=site, name=item.spec.BuilderName, tick=snap.tick or 0}
                        table.insert(decisions, {kind='scout',builder=u.h,x=x,z=z,route=route,name=item.spec.BuilderName})
                        return true
                    end
                end
            end
        end
        return false
    end)

    walkPriority(brain, 'PlatoonFormBuilder', function(item)
        local template = PlatoonTemplates[item.spec.PlatoonTemplate]
        local squads = template and template.GlobalSquads
        if not squads then return false end
        if template.Plan == 'UnitUpgradeAI' or template.Plan == 'ScoutingAI' then
            return false  -- the upgrade slot above owns these
        end
        local gathered = {}
        local wanted = 0
        for _, squad in ipairs(squads) do
            wanted = wanted + (squad[2] or 1)
            local cap = squad[3] or 1
            for _, u in ipairs(snap.units) do
                if cap <= 0 then break end
                if u.idle and not EntityCategoryContains(categories.SCOUT, u)
                    and EntityCategoryContains(squad[1], u) then
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

function __rm_faf_builder_conditions()
    -- Observations only: never re-run a condition for reporting, and never imply
    -- that a short-circuited condition or an unvisited builder was evaluated.
    local rows={}
    for army,brain in pairs(__rm_faf.brains) do
        for spec,entry in pairs(brain.condCache) do
            rows[#rows+1]=string.format('army %d %s: checked tick %g, %d/%d passes, first failure: %s',
                army,spec.BuilderName or 'unnamed',entry.checkedAt,entry.passes,entry.checks,entry.failure)
        end
    end
    table.sort(rows)
    return rows
end

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

[[nodiscard]] std::optional<rm::unitdef::UnitDef> readBlueprint(const World& world,
                                                                 const std::string& path) {
    const std::optional<std::vector<std::byte>> bytes = world.content.read(path);
    if (!bytes) {
        return std::nullopt;
    }
    auto loaded = rm::unitbp::load(
        std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()}, path);
    if (!loaded) {
        return std::nullopt;
    }
    return std::move(*loaded);
}

void pushBlueprintValue(lua_State* lua, const rm::lua::Value& value) {
    using Type = rm::lua::Value::Type;
    switch (value.type) {
    case Type::None: lua_pushnil(lua); break;
    case Type::Bool: lua_pushboolean(lua, value.boolean); break;
    case Type::Number: lua_pushnumber(lua, value.number); break;
    case Type::Text: lua_pushlstring(lua, value.text.data(), value.text.size()); break;
    case Type::Table:
        lua_newtable(lua);
        for (std::size_t i = 0; i < value.items.size(); ++i) {
            pushBlueprintValue(lua, value.items[i]);
            lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
        }
        for (const auto& field : value.fields) {
            pushBlueprintValue(lua, field.value);
            lua_setfield(lua, -2, field.key.c_str());
        }
        break;
    }
}

int loadBlueprint(lua_State* lua) {
    const char* id = luaL_checkstring(lua, 1);
    const auto* content = static_cast<const rm::vfs::Vfs*>(
        lua_touserdata(lua, lua_upvalueindex(1)));
    // Destroy parsed tables and byte buffers BEFORE lua_error's longjmp.
    {
        const std::string path = blueprintPathFor(id);
        const auto bytes = content->read(path);
        if (!bytes) {
            lua_pushfstring(lua, "unit blueprint not found: %s", id);
        } else {
            const auto parsed = rm::lua::parseTable(std::string_view{
                reinterpret_cast<const char*>(bytes->data()), bytes->size()});
            if (parsed) {
                pushBlueprintValue(lua, *parsed);
                return 1;
            }
            lua_pushfstring(lua, "invalid unit blueprint %s: %s", id,
                            parsed.error().message.c_str());
        }
    }
    return lua_error(lua);
}

[[nodiscard]] std::string enemyPathType(const World& world, std::size_t own,
                                        std::size_t enemy) {
    const auto& from = world.starts[own];
    const auto& to = world.starts[enemy];
    // base-ai.lua:1617–1630 tries Land, then Amphibious; Air means neither route exists.
    // Use the same motion-class grids as our units, not a guessed map classification.
    for (const auto motion : {rm::unitdef::MotionType::Land, rm::unitdef::MotionType::Amphibious}) {
        const auto move = rm::data::moveDefFor(motion);
        const auto grid = rm::sim::buildPassability(world.field, world.scene.waterLevelElmos,
                                                   move.maxSlopeDegrees, move.maxWaterDepthElmos);
        if (!rm::sim::findPath(grid, rm::sim::fxFromFloat(from.x), rm::sim::fxFromFloat(from.z),
                              rm::sim::fxFromFloat(to.x), rm::sim::fxFromFloat(to.z)).empty()) {
            return motion == rm::unitdef::MotionType::Land ? "Land" : "Amphibious";
        }
    }
    return "Air";
}

[[nodiscard]] std::optional<std::array<rm::sim::Fx, 3>> nearestNavalSite(
    const World& world, const std::array<rm::sim::Fx, 3>& home, rm::sim::Fx radius) {
    if (!world.scene.hasWater) {
        return std::nullopt;
    }
    const rm::sim::PassabilityGrid water = rm::sim::buildSurfaceWaterPassability(
        world.field, world.scene.waterLevelElmos);
    const auto site = rm::sim::nearestPlaceableSite(water, home[0], home[2], radius);
    if (!site) {
        return std::nullopt;
    }
    return std::array<rm::sim::Fx, 3>{(*site)[0], rm::sim::fxFromFloat(world.scene.waterLevelElmos),
                                      (*site)[1]};
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

std::vector<std::string> fafBuilderConditions(FafAi& ai) {
    return readStringArray(ai, "__rm_faf_builder_conditions");
}

std::string formatFafConditionErrorReport(std::span<const std::string> errors) {
    if (errors.empty()) {
        return {};
    }
    std::string report = "  CONDITION ERRORS (each one fails closed):\n";
    for (const std::string& error : errors) {
        report += "    " + error + '\n';
    }
    return report;
}

FafOpponent::FafOpponent(FafAi& sandbox, int army, std::string baseTemplate)
    : sandbox_(sandbox), army_(army), baseTemplate_(std::move(baseTemplate)) {}

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

std::int64_t FafOpponent::packHandle(rm::sim::UnitId id) noexcept {
    // Generation high, index low; both are 32-bit, so the value is exact in a Lua integer
    // and never zero for a live unit (generations start at one).
    return static_cast<std::int64_t>((std::uint64_t{id.generation} << 32) | std::uint64_t{id.index});
}

rm::sim::UnitId FafOpponent::unpackHandle(std::int64_t handle) noexcept {
    const auto bits = static_cast<std::uint64_t>(handle);
    return rm::sim::UnitId{.index = static_cast<rm::UnitIndex>(bits & 0xffffffffu),
                           .generation = static_cast<rm::Generation>(bits >> 32)};
}

int FafOpponent::beenDestroyedBinding(lua_State* lua) {
    const auto* self = static_cast<const FafOpponent*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const lua_Integer handle = lua_tointeger(lua, 1);
    if (self == nullptr || !self->world_ || handle <= 0) {
        lua_pushboolean(lua, 1);  // nothing to resolve against is as destroyed as it gets
        return 1;
    }
    lua_pushboolean(lua, rm::sim::beenDestroyed(self->world_->scene.store, unpackHandle(handle)) ? 1 : 0);
    return 1;
}

int FafOpponent::enhancementSequenceBinding(lua_State* lua) {
    const auto* self = static_cast<const FafOpponent*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const auto id = unpackHandle(luaL_checkinteger(lua, 1));
    luaL_checktype(lua, 2, LUA_TTABLE);
    std::vector<std::string> sequence;
    const auto count = lua_rawlen(lua, 2);
    for (std::size_t index = 1; index <= count; ++index) {
        lua_rawgeti(lua, 2, static_cast<lua_Integer>(index));
        const char* name = lua_tostring(lua, -1);
        if (!name) { lua_pop(lua, 1); lua_pushboolean(lua, 0); return 1; }
        sequence.emplace_back(name);
        lua_pop(lua, 1);
    }
    const bool valid = self && self->world_
        && rm::sim::validateEnhancementSequence(self->world_->scene.store,
            self->world_->scene.catalog, id, sequence).has_value();
    lua_pushboolean(lua, valid);
    return 1;
}

int FafOpponent::openingSurveyBinding(lua_State* lua) {
    const auto* self = static_cast<const FafOpponent*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const auto id = unpackHandle(luaL_checkinteger(lua, 1));
    if (!self || !self->world_ || !self->world_->scene.store.alive(id)) {
        lua_pushnil(lua);
        return 1;
    }
    const auto& world = *self->world_;
    const auto& at = world.scene.store.transforms()[id.index];
    const float x = rm::sim::fxToFloat(at.x), z = rm::sim::fxToFloat(at.z);
    const auto move = rm::data::moveDefFor(rm::unitdef::MotionType::Amphibious);
    const auto grid = rm::sim::buildPassability(world.field, world.scene.waterLevelElmos,
        move.maxSlopeDegrees, move.maxWaterDepthElmos);
    const auto reachable = [&](const rm::scenario::Marker& marker) {
        return !rm::sim::findPath(grid, at.x, at.z, rm::sim::fxFromFloat(marker.position[0]),
            rm::sim::fxFromFloat(marker.position[2])).empty();
    };
    const auto distanceSquared = [&](const rm::scenario::Marker& marker) {
        const float dx = marker.position[0]-x, dz = marker.position[2]-z;
        return dx*dx + dz*dz;
    };
    const auto pushSite = [&](const rm::scenario::Marker& marker) {
        lua_newtable(lua);
        lua_pushnumber(lua,marker.position[0]); lua_rawseti(lua,-2,1);
        lua_pushnumber(lua,marker.position[2]); lua_rawseti(lua,-2,2);
    };
    // CommanderInitialBOAI (platoon.lua:4612–4635): preserve marker order and stop
    // the entire mass survey when either list reaches four. Distances are squared
    // ogrids in FAF; our markers and paths are elmos (8 elmos per ogrid).
    constexpr float kSquaredElmosPerOgrid = 8.0f*8.0f;
    lua_newtable(lua);
    lua_newtable(lua); // close
    lua_newtable(lua); // distant
    lua_Integer closeCount = 0, distantCount = 0;
    for (const auto& marker : world.markers) {
        if (!marker.isType("Mass")) continue;
        const float distance = distanceSquared(marker);
        if (distance >= 484*kSquaredElmosPerOgrid || !reachable(marker)) continue;
        pushSite(marker);
        if (distance < 165*kSquaredElmosPerOgrid) lua_rawseti(lua,-3,++closeCount);
        else lua_rawseti(lua,-2,++distantCount);
        if (closeCount == 4 || distantCount == 4) break;
    }
    lua_setfield(lua,-3,"distant");
    lua_setfield(lua,-2,"close");
    const rm::scenario::Marker* hydro = nullptr;
    float nearest = 65*65*kSquaredElmosPerOgrid;
    for (const auto& marker : world.markers) {
        if (marker.isType("Hydrocarbon") && distanceSquared(marker) <= nearest
            && (!hydro || distanceSquared(marker) < nearest)) {
            hydro = &marker;
            nearest = distanceSquared(marker);
        }
    }
    if (hydro && reachable(*hydro)) {
        pushSite(*hydro);
        lua_setfield(lua,-2,"hydro");
    }
    lua_pushboolean(lua,world.scene.hasWater
        && world.field.heightAtWorld(x,z) < world.scene.waterLevelElmos);
    lua_setfield(lua,-2,"inWater");
    return 1;
}

int FafOpponent::scoutRouteBinding(lua_State* lua) {
    const auto* self = static_cast<const FafOpponent*>(lua_touserdata(lua, lua_upvalueindex(1)));
    const auto id = unpackHandle(luaL_checkinteger(lua, 1));
    const auto x = rm::sim::fxFromFloat(static_cast<float>(luaL_checknumber(lua, 2)));
    const auto z = rm::sim::fxFromFloat(static_cast<float>(luaL_checknumber(lua, 3)));
    const float cellSize = static_cast<float>(luaL_checknumber(lua, 4));
    const int rings = static_cast<int>(luaL_checkinteger(lua, 5));
    lua_newtable(lua);
    if (!self || !self->world_ || cellSize <= 0 || rings < 0) return 1;
    const auto& world = *self->world_;
    const auto& scene = world.scene;
    if (!scene.store.alive(id)) return 1;
    const auto* def = scene.catalog.def(scene.store.typeAt(id.index));
    if (!def) return 1;
    std::vector<std::array<rm::sim::Fx, 2>> route;
    if (scene.store.motion()[id.index].canFly) {
        route.push_back({x, z});
    } else {
        const auto move = rm::data::moveDefFor(*def);
        auto grid = rm::sim::buildPassability(world.field, scene.waterLevelElmos,
                                             move.maxSlopeDegrees, move.maxWaterDepthElmos);
        std::map<std::pair<int, int>, float> threat;
        // The Lua VM is shared by armies. Resolve allegiance from the scout rather
        // than the opponent that most recently registered this closure.
        const int army = scene.store.motion()[id.index].armyIndex;
        if (army < 0 || static_cast<std::size_t>(army) >= scene.armies.size()) return 1;
        const int alliance = scene.armies[static_cast<std::size_t>(army)].alliance;
        const auto cell = [cellSize](rm::sim::Fx coordinate) {
            return static_cast<int>(std::floor(rm::sim::fxToFloat(coordinate) / cellSize));
        };
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            if (!scene.store.slotAlive(slot)) continue;
            const int owner = scene.store.motion()[slot].armyIndex;
            if (owner < 0 || static_cast<std::size_t>(owner) >= scene.armies.size()
                || scene.armies[static_cast<std::size_t>(owner)].alliance == alliance) continue;
            const auto& at = scene.store.transforms()[slot];
            if (!scene.intel.sees(alliance, rm::sim::IntelKind::Vision, at.x, at.z)) continue;
            const auto* enemy = scene.catalog.def(scene.store.typeAt(slot));
            if (enemy) threat[{cell(at.x), cell(at.z)}] += enemy->surfaceThreat;
        }
        for (int gz = 0; gz < grid.cellsZ; ++gz) for (int gx = 0; gx < grid.cellsX; ++gx) {
            const int tx = cell(grid.worldAtCellCentre(gx));
            const int tz = cell(grid.worldAtCellCentre(gz));
            float total = 0;
            for (int dz = -rings; dz <= rings; ++dz) for (int dx = -rings; dx <= rings; ++dx) {
                const auto found = threat.find({tx + dx, tz + dz});
                if (found != threat.end()) total += found->second;
            }
            // ScoutingAI's authored AntiSurface threshold (platoon.lua:1216).
            if (total > 400) grid.passable[static_cast<std::size_t>(gz * grid.cellsX + gx)] = 0;
        }
        const auto& at = scene.store.transforms()[id.index];
        route = rm::sim::findPath(grid, at.x, at.z, x, z);
    }
    lua_Integer index = 1;
    for (const auto& point : route) {
        lua_newtable(lua);
        lua_pushnumber(lua, rm::sim::fxToFloat(point[0]));
        lua_rawseti(lua, -2, 1);
        lua_pushnumber(lua, rm::sim::fxToFloat(point[1]));
        lua_rawseti(lua, -2, 2);
        lua_rawseti(lua, -2, index++);
    }
    return 1;
}

void FafOpponent::observe(const World& world, std::span<const rm::sim::Event> events) {
    world_.emplace(world);
    if (!booted_ || !sandbox_.ready()) return;
    lua_State* lua = sandbox_.state();
    for (const auto& event : events) {
        if (event.kind != rm::sim::EventKind::UnitFinished || event.army != army_) continue;
        // EngineerManager.UnitConstructionFinished / FactoryFinishBuilding inherit
        // the founder's current manager, including upgrade replacement handles.
        const int top = lua_gettop(lua);
        lua_getglobal(lua,"__rm_faf");
        lua_getfield(lua,-1,"brains");
        lua_rawgeti(lua,-1,army_);
        lua_getfield(lua,-1,"unitLocations");
        lua_rawgeti(lua,-1,packHandle(event.builder));
        if (lua_isnil(lua,-1)) {
            lua_pop(lua,1);
            lua_pushliteral(lua,"MAIN"); // initial army-pool units register at MAIN
        }
        lua_rawseti(lua,-2,packHandle(event.unit));
        lua_settop(lua,top);
    }
}

void FafOpponent::advance(rm::TickIndex tick) {
    decisions_.clear();
    plannedThisPass_.clear();
    if (!world_ || !sandbox_.ready()) {
        return;
    }
    lua_State* lua = sandbox_.state();
    const rm::app::UnitScene& scene = world_->scene;
    if (!placement_ || !placement_->matches(world_->field,scene.hasWater,scene.waterLevelElmos)) {
        placement_.emplace(world_->field,scene.hasWater,scene.waterLevelElmos);
    }
    const auto armyIndex = static_cast<std::size_t>(army_);
    if (armyIndex >= scene.armies.size() || armyIndex >= world_->starts.size()) {
        return;
    }
    // Queued foundations reserve space even before the native task starts. Another
    // engineer's decision must not reuse the second site of an opening queue.
    for (rm::UnitIndex slot=0; slot<scene.store.slotCount(); ++slot) {
        if (!scene.store.health()[slot].alive()) continue;
        for (const auto& order : scene.store.orders()[slot].entries()) {
            if (order.kind()!=rm::sim::CommandKind::Build) continue;
            const auto* product=scene.catalog.def(order.payload().buildType);
            if (product && !product->isMobile()) plannedThisPass_.push_back({
                .position={order.payload().targetX,{},order.payload().targetZ},
                .radius=product->collisionRadiusElmos});
        }
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
        // The shared match VFS outlives its opponents. The callback reads it lazily, so
        // queries also work for blueprints which have never spawned or entered a queue.
        lua_pushlightuserdata(lua, const_cast<rm::vfs::Vfs*>(&world_->content));
        lua_pushcclosure(lua, loadBlueprint, 1);
        lua_setglobal(lua, "__rm_faf_load_blueprint");
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
        // Authored coroutine waits are measured at FAF's fixed ten ticks/second.
        lua_pushnumber(lua,static_cast<double>(rm::app::gAppTickRate.ticksPerSecond())/10.0);
        lua_setfield(lua,-2,"fafTickScale");
        const std::array<rm::sim::Fx, 3> home{
            rm::sim::fxFromFloat(start.x), rm::sim::Fx{}, rm::sim::fxFromFloat(start.z)};
        bool hasNavalSite = false;
        const std::array<std::string, 1> navalCategory{{"NAVAL"}};
        if (const auto yard = scene.roster.pick(scene.armies[armyIndex].faction,
                                                rm::unitdef::Role::Factory, 1,
                                                navalCategory)) {
            if (const auto def = readBlueprint(*world_, yard->path())) {
                const auto site = nearestNavalSite(*world_, home,
                    rm::sim::fxFromFloat(def->collisionRadiusElmos));
                hasNavalSite = site.has_value();
                if (site) {
                    lua_pushnumber(lua, rm::sim::fxToFloat((*site)[0]));
                    lua_setfield(lua, -2, "navalX");
                    lua_pushnumber(lua, rm::sim::fxToFloat((*site)[2]));
                    lua_setfield(lua, -2, "navalZ");
                }
            }
        }
        lua_pushboolean(lua, hasNavalSite ? 1 : 0);
        lua_setfield(lua, -2, "hasNavalSite");
        if (baseTemplate_ == "adaptive" || baseTemplate_ == "random") {
            // Vertex-sampled water coverage for FAF's naval strategy gate.
            const auto& field = world_->field;
            std::size_t wet = 0;
            for (const auto raw : field.raw) {
                if (field.baseHeight + field.heightScale * raw < scene.waterLevelElmos) ++wet;
            }
            lua_pushnumber(lua, field.raw.empty() ? 0.0
                : static_cast<double>(wet) / static_cast<double>(field.raw.size()));
            lua_setfield(lua, -2, "waterRatio");
        }
        // Named templates or per-army adaptive/random scoring in the Lua bootstrap.
        lua_pushstring(lua, baseTemplate_.c_str());
        lua_setfield(lua, -2, "base");
        lua_newtable(lua);
        lua_Integer scoutSite=1;
        int opponents=0;
        for (std::size_t i=0; i<world_->starts.size(); ++i) {
            const bool occupied=i<scene.armies.size();
            if (occupied && scene.armies[i].alliance==scene.armies[armyIndex].alliance) continue;
            if (!occupied) {
                float enemyDistance = std::numeric_limits<float>::infinity();
                float alliedDistance = std::numeric_limits<float>::infinity();
                for (std::size_t j = 0; j < scene.armies.size() && j < world_->starts.size(); ++j) {
                    const float dx = world_->starts[j].x - world_->starts[i].x;
                    const float dz = world_->starts[j].z - world_->starts[i].z;
                    float& distance = scene.armies[j].alliance == scene.armies[armyIndex].alliance
                        ? alliedDistance : enemyDistance;
                    distance = std::min(distance, dx * dx + dz * dz);
                }
                // base-ai.lua:1420 gives enemy territory a 100-ogrid squared-distance
                // advantage, so near-equidistant vacant starts still receive scouts.
                constexpr float enemyBias = 100.0f * 100.0f * 8.0f * 8.0f;
                if (enemyDistance - enemyBias > alliedDistance) continue;
            }
            if (occupied) ++opponents;
            lua_newtable(lua);
            lua_pushnumber(lua,world_->starts[i].x); lua_setfield(lua,-2,"x");
            lua_pushnumber(lua,world_->starts[i].z); lua_setfield(lua,-2,"z");
            lua_pushboolean(lua,occupied); lua_setfield(lua,-2,"high");
            lua_rawseti(lua,-2,scoutSite++);
        }
        lua_setfield(lua,-2,"scoutSites");
        lua_pushinteger(lua,opponents); lua_setfield(lua,-2,"numOpponents");
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
            lua_getfield(lua, -1, "baseTemplate");
            std::printf("faf: army %d selected %s\n", army_, lua_tostring(lua, -1));
            lua_pop(lua, 1);
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

    // Brains share a VM; each pass must bind queries to its current observation,
    // not the World pointer retained by whichever opponent happened to boot last.
    lua_pushlightuserdata(lua, this);
    lua_pushcclosure(lua, beenDestroyedBinding, 1);
    lua_setglobal(lua, "__rm_faf_beenDestroyed");
    lua_pushlightuserdata(lua, this);
    lua_pushcclosure(lua, scoutRouteBinding, 1);
    lua_setglobal(lua, "__rm_faf_scout_route");
    lua_pushlightuserdata(lua, this);
    lua_pushcclosure(lua, enhancementSequenceBinding, 1);
    lua_setglobal(lua, "__rm_faf_enhancement_sequence");
    lua_pushlightuserdata(lua, this);
    lua_pushcclosure(lua, openingSurveyBinding, 1);
    lua_setglobal(lua, "__rm_faf_opening_survey");

    // --- The snapshot -------------------------------------------------------------------
    //
    // Arrays in slot order, nothing keyed by anything hashed: the pass must read the same
    // world in the same order every run.
    const std::span<const rm::sim::MoveState> motion = scene.store.motion();
    const std::span<const rm::sim::Health> health = scene.store.health();

    lua_newtable(lua);  // snap
    lua_pushinteger(lua, static_cast<lua_Integer>(tick));
    lua_setfield(lua, -2, "tick");

    for (std::size_t i = 0; i < scene.armies.size() && i < world_->starts.size(); ++i) {
        lua_getglobal(lua, "__rm_faf_army");
        lua_pushinteger(lua, static_cast<lua_Integer>(i));
        lua_pushnumber(lua, world_->starts[i].x);
        lua_pushnumber(lua, world_->starts[i].z);
        lua_pushinteger(lua, fafFactionIndex(scene.armies[i].faction));
        lua_pushboolean(lua, scene.armies[i].defeated);
        lua_pushinteger(lua, scene.armies[i].alliance);
        if (lua_pcall(lua, 6, 0, 0) != LUA_OK) {
            std::printf("faf-opponent: army snapshot failed: %s\n", lua_tostring(lua, -1));
            lua_pop(lua, 2);  // error and unfinished snapshot
            return;
        }
    }
    const auto& start = world_->starts[armyIndex];
    const std::array<rm::sim::Fx, 3> home{
        rm::sim::fxFromFloat(start.x), {}, rm::sim::fxFromFloat(start.z)};
    if (const auto target = rm::app::nearestEnemyCommander(scene, army_, home)) {
        // Resolve the very same target the attack adapter uses; preserve its alliance,
        // defeat, live-commander and deterministic slot-order rules in one authority.
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            const int owner = motion[slot].armyIndex;
            const auto* def = scene.catalog.def(scene.store.typeAt(slot));
            if (def && rm::sim::isCommanderId(def->name) && health[slot].alive()
                && owner >= 0 && static_cast<std::size_t>(owner) < scene.armies.size()
                && static_cast<std::size_t>(owner) < world_->starts.size()
                && rm::sim::hostile(scene.armies[armyIndex], scene.armies[static_cast<std::size_t>(owner)])
                && rm::sim::positionOf(scene.store.transforms()[slot]) == *target) {
                lua_pushinteger(lua, owner);
                lua_setfield(lua, -2, "currentEnemy");
                auto path = enemyPaths_.find(owner);
                if (path == enemyPaths_.end()) {
                    path = enemyPaths_.emplace(owner, enemyPathType(
                        *world_, armyIndex, static_cast<std::size_t>(owner))).first;
                }
                lua_pushstring(lua, path->second.c_str());
                lua_setfield(lua, -2, "enemyPath");
                break;
            }
        }
    }

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
    pushNumber("massRequested", rm::sim::magToFloat(economy.requestedLastTick.mass));
    pushNumber("energyRequested", rm::sim::magToFloat(economy.requestedLastTick.energy));
    pushNumber("massUsage", rm::sim::magToFloat(economy.usageLastTick.mass));
    pushNumber("energyUsage", rm::sim::magToFloat(economy.usageLastTick.energy));

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
        lua_pushinteger(lua,packHandle(construction.builder));
        lua_setfield(lua,-2,"builder");
        if (scene.store.alive(construction.builder)) {
            const auto* order=scene.store.orders()[construction.builder.index].active();
            if (order && order->kind()==rm::sim::CommandKind::Build) {
                lua_pushinteger(lua,order->payload().id);
                lua_setfield(lua,-2,"command");
                lua_pushinteger(lua,order->payload().remainingCount);
                lua_setfield(lua,-2,"remaining");
            }
        }
        lua_pushnumber(lua,rm::sim::fxToFloat(construction.position[0]));
        lua_setfield(lua,-2,"x");
        lua_pushnumber(lua,rm::sim::fxToFloat(construction.position[2]));
        lua_setfield(lua,-2,"z");
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

    std::map<std::int64_t,int> guardCounts;
    for (rm::UnitIndex slot=0; slot<scene.store.slotCount(); ++slot) {
        if (!health[slot].alive()) continue;
        const auto* head=scene.store.orders()[slot].current();
        if (head && (head->kind()==rm::sim::CommandKind::Guard
            || head->kind()==rm::sim::CommandKind::Assist)) ++guardCounts[packHandle(head->payload().target)];
    }

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

        const rm::sim::UnitId id = scene.store.idAt(slot);
        lua_newtable(lua);  // the unit
        lua_pushstring(lua, def->name.c_str());
        lua_setfield(lua, -2, "bp");
        lua_pushinteger(lua, packHandle(id));
        lua_setfield(lua, -2, "h");
        lua_pushnumber(lua, static_cast<lua_Number>(x));
        lua_setfield(lua, -2, "x");
        lua_pushnumber(lua, static_cast<lua_Number>(z));
        lua_setfield(lua, -2, "z");
        const float maximumHealth = rm::sim::magToFloat(health[slot].maximum);
        lua_pushnumber(lua, static_cast<lua_Number>(
                                maximumHealth > 0.0F
                                    ? rm::sim::magToFloat(health[slot].current) / maximumHealth
                                    : 0.0F));
        lua_setfield(lua, -2, "healthPercent");
        lua_newtable(lua);
        for (const auto& [position, name] : scene.store.enhancements()[slot]) {
            lua_pushboolean(lua, 1);
            lua_setfield(lua, -2, name.c_str());
        }
        lua_setfield(lua, -2, "enhancements");
        // A stationary enhancement still owns the commander. Construction and reclaim
        // expose their own busy flags below for the same builder-pool reservation.
        const auto* pending = scene.store.orders()[slot].current();
        lua_pushinteger(lua,guardCounts[packHandle(id)]);
        lua_setfield(lua,-2,"guardCount");
        lua_pushboolean(lua, pending != nullptr);
        lua_setfield(lua, -2, "queueBusy");
        const bool enhancementQueued = pending && pending->kind() == rm::sim::CommandKind::Script
            && pending->payload().scriptTask == "EnhanceTask";
        const auto* active = scene.store.orders()[slot].active();
        const bool enhancing = active && active->kind() == rm::sim::CommandKind::Script
            && active->payload().scriptTask == "EnhanceTask";
        lua_pushboolean(lua, enhancing);
        lua_setfield(lua, -2, "enhancing");
        lua_pushboolean(lua, !motion[slot].moving && !enhancementQueued);
        lua_setfield(lua, -2, "idle");
        // Reclaiming: standing at a wreck counts as idle above, so the reclaim builders read
        // this instead — retail's InstanceCount caps their platoons, and an engineer already
        // on one is not handed another every pass.
        const rm::sim::QueuedCommand* head = scene.store.orders()[slot].active();
        lua_pushboolean(lua, head && head->kind()==rm::sim::CommandKind::Move);
        lua_setfield(lua,-2,"scoutingBusy");
        lua_pushnumber(lua,def->visionRadiusElmos); lua_setfield(lua,-2,"vision");
        const bool reclaiming = head != nullptr
            && (head->kind() == rm::sim::CommandKind::Reclaim
                || head->kind() == rm::sim::CommandKind::ReclaimUnit);
        lua_pushboolean(lua, reclaiming ? 1 : 0);
        lua_setfield(lua, -2, "reclaiming");
        // Building: this unit owns an unfinished construction. A factory mid-product is
        // "idle" by the motion rule above, and a train sent to it is refused by the sim
        // every pass; the picker needs the truth to hand the order to a free factory.
        // An accepted Build may still be approaching its site, with no Construction
        // entry yet. Keep its engineer reserved throughout that native command.
        bool constructing = pending && (pending->kind() == rm::sim::CommandKind::Build
            || pending->kind() == rm::sim::CommandKind::Repair);
        for (const rm::sim::Construction& work : scene.building) {
            if (!work.finished() && work.builder == id) {
                constructing = true;
                break;
            }
        }
        lua_pushboolean(lua, constructing ? 1 : 0);
        lua_setfield(lua, -2, "building");
        if (enhancing || std::find(upgrading.begin(), upgrading.end(), id) != upgrading.end()) {
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

    // Allied units are shared knowledge; enemy queries only see vision contacts.
    // Publish category-bearing proxies so range/count queries share threat's intel filter.
    for (const bool allied : {false, true}) {
        lua_newtable(lua);
        const int alliance = scene.armies[armyIndex].alliance;
        lua_Integer next = 1;
        for (rm::UnitIndex slot = 0; slot < scene.store.slotCount(); ++slot) {
            if (!health[slot].alive()) continue;
            const int owner = motion[slot].armyIndex;
            if (owner < 0 || static_cast<std::size_t>(owner) >= scene.armies.size()
                || owner == army_) continue;
            if ((scene.armies[static_cast<std::size_t>(owner)].alliance == alliance) != allied) continue;
            const auto& at = scene.store.transforms()[slot];
            if (!allied && !scene.intel.sees(alliance, rm::sim::IntelKind::Vision, at.x, at.z)) continue;
            const auto* def = scene.catalog.def(scene.store.typeAt(slot));
            if (!def) continue;
            teachType(lua, *def);
            lua_newtable(lua);
            const int unitTable = lua_gettop(lua);
            lua_pushstring(lua, def->name.c_str());
            lua_setfield(lua, unitTable, "bp");
            lua_pushinteger(lua, packHandle(scene.store.idAt(slot)));
            lua_setfield(lua, unitTable, "h");
            lua_pushnumber(lua, rm::sim::fxToFloat(at.x));
            lua_setfield(lua, unitTable, "x");
            lua_pushnumber(lua, rm::sim::fxToFloat(at.z));
            lua_setfield(lua, unitTable, "z");
            lua_getglobal(lua, "__rm_faf");
            lua_getfield(lua, -1, "armyViews");
            lua_rawgeti(lua, -1, owner);
            lua_setfield(lua, unitTable, "__brain");
            lua_pop(lua, 1);
            lua_getfield(lua, -1, "cats");
            lua_getfield(lua, -1, def->name.c_str());
            lua_setfield(lua, unitTable, "__cats");
            lua_pop(lua, 1);
            lua_getfield(lua, -1, "unitMeta");
            lua_setmetatable(lua, unitTable);
            lua_pop(lua, 1);
            lua_rawseti(lua, -2, next++);
        }
        lua_setfield(lua, -2, allied ? "allies" : "enemies");
    }

    // snap.reclaim — the wreck mass and energy left per retail reclaim-grid cell (Grid.lua:
    // sixteen cells a side, eight on a 256 map, CellSize = max(sizeX, sizeZ) / cells). The
    // brain's GridReclaim view answers ReclaimAvailableInGrid from this, so the AI sees the
    // same pool the harvest pass drains. Only occupied cells are written; the view reads a
    // missing cell as empty.
    {
        const int sizeX = world_->field.squaresX * rm::kSquareSize;
        const int sizeZ = world_->field.squaresZ * rm::kSquareSize;
        const int cellCount = (sizeX == sizeZ && sizeX == 256) ? 8 : 16;
        const float cellSize = static_cast<float>(std::max(sizeX, sizeZ))
                               / static_cast<float>(cellCount);
        struct Cell {
            float mass = 0;
            float energy = 0;
            lua_Integer count = 0;
        };
        std::map<std::pair<int, int>, Cell> cells;  // ordered, so columns close in sequence
        const auto gridAxis = [&](rm::sim::Fx world) {
            const float w = rm::sim::fxToFloat(world);
            return w > 0 ? std::min(cellCount, static_cast<int>(w / cellSize) + 1) : 1;
        };
        const auto features = scene.features.all();
        for (rm::UnitIndex slot = 0; slot < features.size(); ++slot) {
            if (!scene.features.slotAlive(slot)) continue;
            const rm::sim::Feature& feature = features[slot];
            const float mass = rm::sim::magToFloat(feature.massRemaining);
            const float energy = rm::sim::magToFloat(feature.energyRemaining);
            if (mass <= 0 && energy <= 0) continue;
            Cell& cell = cells[{gridAxis(feature.at[0]), gridAxis(feature.at[2])}];
            cell.mass += mass;
            cell.energy += energy;
            ++cell.count;
        }
        lua_newtable(lua);  // snap.reclaim
        lua_pushinteger(lua, cellCount);
        lua_setfield(lua, -2, "cellCount");
        lua_pushnumber(lua, static_cast<lua_Number>(cellSize));
        lua_setfield(lua, -2, "cellSize");
        lua_newtable(lua);  // snap.reclaim.cells[gx][gz]
        int openColumn = 0;
        for (const auto& [key, cell] : cells) {
            if (key.first != openColumn) {
                if (openColumn != 0) lua_rawseti(lua, -2, openColumn);
                openColumn = key.first;
                lua_newtable(lua);
            }
            lua_newtable(lua);
            lua_pushnumber(lua, static_cast<lua_Number>(cell.mass));
            lua_setfield(lua, -2, "mass");
            lua_pushnumber(lua, static_cast<lua_Number>(cell.energy));
            lua_setfield(lua, -2, "energy");
            lua_pushinteger(lua, cell.count);
            lua_setfield(lua, -2, "count");
            lua_rawseti(lua, -2, key.second);
        }
        if (openColumn != 0) lua_rawseti(lua, -2, openColumn);
        lua_setfield(lua, -2, "cells");
        lua_setfield(lua, -2, "reclaim");
    }

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
    const rm::app::UnitScene& scene = world_->scene;
    // A packed handle resolves FRESH through the script-object seam (C-044). A unit that died
    // this tick is dropped silently — the ordinary race between census and order. A released
    // handle is retail's "Game object has been destroyed", and logged as such: it means Lua
    // kept a unit across passes, which the driver is not supposed to do.
    const auto resolveHandle = [this, &scene](lua_Integer h) -> std::optional<rm::sim::UnitId> {
        if (h <= 0) {
            return std::nullopt;
        }
        const rm::sim::UnitId id = unpackHandle(h);
        const rm::sim::UnitStore::Resolved resolved = scene.store.resolve(id);
        if (resolved.state == rm::sim::UnitStore::HandleState::Stale) {
            if (rm::app::gFafLog) {
                std::printf("  [faf %d] handle %u:%u: %.*s\n", army_, id.index, id.generation,
                            static_cast<int>(rm::sim::kDestroyedObjectError.size()),
                            rm::sim::kDestroyedObjectError.data());
            }
            return std::nullopt;
        }
        if (resolved.state == rm::sim::UnitStore::HandleState::Destroyed) {
            return std::nullopt;
        }
        return id;
    };
    const auto handleAt = [lua, &resolveHandle](int index) -> std::optional<rm::sim::UnitId> {
        lua_Integer h = 0;
        if (index == 0) {
            lua_getfield(lua, -1, "builder");
            h = lua_tointeger(lua, -1);
            lua_pop(lua, 1);
        } else {
            h = index;
        }
        return resolveHandle(h);
    };

    const std::string kind = field("kind");

    if (kind=="move" || kind=="stop" || kind=="guard") {
        const auto unit=handleAt(0);
        if (!unit) return;
        lua_getfield(lua,-1,"queued");
        const bool queued=lua_toboolean(lua,-1);
        lua_pop(lua,1);
        if (kind=="stop") decisions_.push_back({.kind=Decision::Kind::Stop,.unit=*unit});
        else if (kind=="guard") {
            lua_getfield(lua,-1,"target");
            const auto target=resolveHandle(lua_tointeger(lua,-1));
            lua_pop(lua,1);
            if (target) decisions_.push_back({.kind=Decision::Kind::Guard,
                .unit=*unit,.target=*target,.queued=queued});
        } else {
            lua_getfield(lua,-1,"x");
            lua_getfield(lua,-2,"z");
            const float x=static_cast<float>(lua_tonumber(lua,-2));
            const float z=static_cast<float>(lua_tonumber(lua,-1));
            const bool valid=lua_isnumber(lua,-2) && lua_isnumber(lua,-1)
                && std::isfinite(x) && std::isfinite(z) && x>=0 && z>=0
                && x<=world_->field.squaresX*rm::kSquareSize && z<=world_->field.squaresZ*rm::kSquareSize;
            lua_pop(lua,2);
            if (valid) decisions_.push_back({.kind=Decision::Kind::Move,.unit=*unit,
                .toX=rm::sim::fxFromFloat(x),.toZ=rm::sim::fxFromFloat(z),.queued=queued});
        }
        return;
    }

    if (kind == "enhance") {
        if (const auto unit = handleAt(0)) {
            lua_getfield(lua, -1, "queued");
            const bool queued = lua_toboolean(lua, -1);
            lua_pop(lua, 1);
            decisions_.push_back({.kind=Decision::Kind::Enhance, .enhancement=field("enhancement"),
                .unit=*unit, .queued=queued});
        }
        return;
    }

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

    if (kind == "reclaim") {
        // The corpus named the cell; the match picks the wreck in it.
        const std::optional<rm::sim::UnitId> builder = handleAt(0);
        if (!builder || !scene.store.alive(*builder)) {
            return;
        }
        const auto number = [lua](const char* name) {
            lua_getfield(lua, -1, name);
            const float value = static_cast<float>(lua_tonumber(lua, -1));
            lua_pop(lua, 1);
            return value;
        };
        decisions_.push_back(Decision{
            .kind = Decision::Kind::Reclaim,
            .unit = *builder,
            .toX = rm::sim::fxFromFloat(number("x")),
            .toZ = rm::sim::fxFromFloat(number("z")),
            .radius = rm::sim::fxFromFloat(number("radius")),
        });
        if (rm::app::gFafLog) {
            std::printf("  [faf %d] reclaim '%s' near (%.0f, %.0f)\n", army_, field("name").c_str(),
                        static_cast<double>(number("x")), static_cast<double>(number("z")));
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
        const std::string path = blueprintPathFor(bp);
        const auto product = kind=="build" ? readBlueprint(*world_,path)
                                           : std::optional<rm::unitdef::UnitDef>{};
        if (kind=="build" && !product) return;
        const float radius=product ? product->collisionRadiusElmos : 0;
        const auto terrain=scene.terrain(world_->field);
        const auto placeable = [&](const std::array<rm::sim::Fx,3>& at) {
            if (!product) return true;
            if (at[0]<rm::sim::Fx{} || at[2]<rm::sim::Fx{}
                || at[0]>rm::sim::Fx::fromInt(world_->field.squaresX*rm::kSquareSize)
                || at[2]>rm::sim::Fx::fromInt(world_->field.squaresZ*rm::kSquareSize)) return false;
            if (!terrain.resourceSitePlaceable(product->buildRestriction,at[0],at[2])) return false;
            if (product->motion==rm::unitdef::MotionType::Air) return true;
            auto move=rm::data::moveDefFor(*product);
            if (!move.usesGroundGrid && !move.usesSurfaceWaterGrid) {
                const auto* builderDef=scene.catalog.def(scene.store.typeAt(builder->index));
                if (!builderDef) return false;
                move=rm::data::moveDefFor(*builderDef);
            }
            // Same footprint/domain checks as native Build intake. The selected
            // product may not be registered until the decision reaches Match.
            const auto& grid=placement_->gridFor(move);
            return product->isMobile()
                ? rm::sim::sitePlaceable(grid,at[0],at[2],rm::sim::fxFromFloat(radius))
                : rm::sim::buildSitePlaceable(grid,at[0],at[2],rm::sim::fxFromFloat(radius),
                    scene.store,scene.catalog,scene.building);
        };

        std::optional<std::array<rm::sim::Fx, 3>> site;
        lua_getfield(lua, -1, "site");
        const bool explicitSite = !lua_isnil(lua, -1);
        if (explicitSite && lua_istable(lua, -1)) {
            lua_rawgeti(lua, -1, 1);
            lua_rawgeti(lua, -2, 2);
            if (lua_isnumber(lua, -2) && lua_isnumber(lua, -1)) {
                const float x = static_cast<float>(lua_tonumber(lua, -2));
                const float z = static_cast<float>(lua_tonumber(lua, -1));
                if (std::isfinite(x) && std::isfinite(z) && x >= 0 && z >= 0
                    && x <= world_->field.squaresX*rm::kSquareSize
                    && z <= world_->field.squaresZ*rm::kSquareSize) {
                    site = std::array{rm::sim::fxFromFloat(x),rm::sim::Fx{},rm::sim::fxFromFloat(z)};
                }
            }
            lua_pop(lua, 2);
        }
        lua_pop(lua, 1);
        if (explicitSite) {
            // Authored resource surveys choose a particular marker. Never silently
            // replace it with the nearest marker; native intake checks its footprint.
            if (!site) return;
        } else if (kind == "train") {
            // Where the factory stands; the spawn path rolls the unit off it.
            const rm::sim::Transform& transform = scene.store.transforms()[builder->index];
            site = std::array<rm::sim::Fx, 3>{transform.x, rm::sim::Fx{}, transform.z};
        } else {
            const std::string structure = field("structure");
            const bool wantsHydro = structure.find("HydroCarbon") != std::string::npos;
            const bool wantsDeposit = structure.find("Resource") != std::string::npos
                                       || structure.find("MassExtraction") != std::string::npos || wantsHydro;
            const bool wantsSeaFactory = structure.find("SeaFactory") != std::string::npos;
            const rm::mapinfo::StartPosition& start =
                world_->starts[static_cast<std::size_t>(army_)];
            std::array<rm::sim::Fx, 3> home{rm::sim::fxFromFloat(start.x), rm::sim::Fx{},
                                                  rm::sim::fxFromFloat(start.z)};
            lua_getfield(lua,-1,"anchor");
            if (lua_istable(lua,-1)) {
                lua_rawgeti(lua,-1,1); lua_rawgeti(lua,-2,2);
                const float x=static_cast<float>(lua_tonumber(lua,-2));
                const float z=static_cast<float>(lua_tonumber(lua,-1));
                const bool valid=lua_isnumber(lua,-2) && lua_isnumber(lua,-1)
                    && std::isfinite(x) && std::isfinite(z) && x>=0 && z>=0
                    && x<=world_->field.squaresX*rm::kSquareSize && z<=world_->field.squaresZ*rm::kSquareSize;
                lua_pop(lua,3);
                if (!valid) return;
                home={rm::sim::fxFromFloat(x),rm::sim::Fx{},rm::sim::fxFromFloat(z)};
            } else lua_pop(lua,1);
            const auto plannedNear = [this,radius](const std::array<rm::sim::Fx, 3>& at) {
                for (const auto& planned : plannedThisPass_) {
                    const float dx = rm::sim::fxToFloat(planned.position[0]) - rm::sim::fxToFloat(at[0]);
                    const float dz = rm::sim::fxToFloat(planned.position[2]) - rm::sim::fxToFloat(at[2]);
                    const float separation=radius+planned.radius;
                    if (dx * dx + dz * dz < separation*separation) {
                        return true;
                    }
                }
                return false;
            };
            if (wantsSeaFactory) {
                const auto def = readBlueprint(*world_, path);
                if (def && def->collisionRadiusElmos > 0.0f) {
                    site = nearestNavalSite(
                        *world_, home, rm::sim::fxFromFloat(def->collisionRadiusElmos));
                }
            } else if (wantsDeposit) {
                // The nearest deposit free of BOTH the sim's claims and this pass's own
                // plans — nearestFreeDeposit only knows the first kind, so the search runs
                // here with both filters.
                const rm::scenario::Marker* best = nullptr;
                rm::sim::Fx bestDistance{};
                for (const rm::scenario::Marker& marker : world_->markers) {
                    if (!marker.isType(wantsHydro ? "Hydrocarbon" : "Mass")) {
                        continue;
                    }
                    const std::array<rm::sim::Fx, 3> at = rm::app::fxPoint(marker.position);
                    if (plannedNear(at) || !placeable(at)) {
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
                    std::array<rm::sim::Fx, 3> candidate = rm::sim::structureSite(
                        home, world_->centreX, world_->centreZ, slot);
                    const auto snapped=terrain.buildSite(*product,candidate[0],candidate[2]);
                    candidate[0]=snapped[0]; candidate[2]=snapped[1];
                    if (!plannedNear(candidate) && placeable(candidate)) {
                        site = candidate;
                        break;
                    }
                }
            }
        }
        if (!site || !placeable(*site)) {
            return;
        }
        if (kind=="build") plannedThisPass_.push_back({.position=*site,.radius=radius});
        lua_getfield(lua, -1, "queued");
        const bool queued = lua_toboolean(lua, -1);
        lua_pop(lua, 1);
        decisions_.push_back(Decision{
            .kind = Decision::Kind::StartConstruction,
            .blueprint = path,
            .site = *site,
            .builder = *builder,
            .queued = queued,
        });
        if (rm::app::gFafLog) {
            std::printf("  [faf %d] %s '%s' -> %s\n", army_, kind.c_str(),
                        field("name").c_str(), bp.c_str());
        }
        return;
    }

    if (kind == "scout") {
        const auto unit = handleAt(0);
        if (!unit || !scene.store.alive(*unit)) return;
        lua_getfield(lua, -1, "route");
        const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(lua, -1, i);
            lua_rawgeti(lua, -1, 1);
            const auto x = rm::sim::fxFromFloat(static_cast<float>(lua_tonumber(lua, -1)));
            lua_pop(lua, 1);
            lua_rawgeti(lua, -1, 2);
            const auto z = rm::sim::fxFromFloat(static_cast<float>(lua_tonumber(lua, -1)));
            lua_pop(lua, 2);
            decisions_.push_back(Decision{
                .kind = Decision::Kind::Move, .unit = *unit, .toX = x, .toZ = z, .queued = i > 1,
            });
        }
        lua_pop(lua, 1);
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
        const rm::sim::PassabilityGrid water = rm::sim::buildSurfaceWaterPassability(
            world_->field, scene.waterLevelElmos);
        lua_getfield(lua, -1, "units");
        if (lua_istable(lua, -1)) {
            const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(lua, -1, i);
                const lua_Integer h = lua_tointeger(lua, -1);
                lua_pop(lua, 1);
                if (const std::optional<rm::sim::UnitId> unit = resolveHandle(h)) {
                    std::array<rm::sim::Fx, 2> objective{(*target)[0], (*target)[2]};
                    if (scene.store.motion()[unit->index].surfaceWater) {
                        const rm::sim::Transform& from = scene.store.transforms()[unit->index];
                        if (const auto reachable = rm::sim::reachablePointToward(
                                water, from.x, from.z, objective[0], objective[1])) {
                            objective = *reachable;
                        }
                    }
                    decisions_.push_back(Decision{
                        .kind = Decision::Kind::Move,
                        .unit = *unit,
                        .toX = objective[0],
                        .toZ = objective[1],
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
