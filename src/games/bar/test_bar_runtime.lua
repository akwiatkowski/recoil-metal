-- Runs in BAR's upstream Test Runner, not a substitute simulation. GPL-2.0-or-later.
-- No spawned units or free resources: both teams use their actual commanders.
-- Timeouts are simulation frames: dbg_test_runner.lua::Test.waitUntil uses GetGameFrame.
local DRAW_TIMEOUT_SECONDS = 10 -- Allow the first visible frame after UI initialization.
local MOVE_TIMEOUT_SECONDS = 30 -- A commander needs only a short local walk.
local BUILD_TIMEOUT_SECONDS = 120 -- Bound real solar construction using starting resources.
local AI_TIMEOUT_SECONDS = 300 -- Allow SimpleAI's normal economic decision/build cycles.
local TILE_SIZE = 8 -- Spring's SQUARE_SIZE, rts/Sim/Misc/GlobalConstants.h (elmos).
local MOVE_DISTANCE = 16 * TILE_SIZE -- Local fixture target: sixteen terrain squares.
local MOVE_OBSERVED_DISTANCE = 4 * TILE_SIZE -- Require visible movement, not command intake.
local BORDER_MARGIN = 4 * TILE_SIZE -- Keep the fixture clear of the map edge.
local MOVE_DIRECTIONS = 8 -- Cardinal and diagonal headings, spaced by a quarter-right-angle.
local BUILD_DIRECTIONS = 16 -- Finer search around the commander for a buildable footprint.
local BUILD_MIN_RADIUS = 12 * TILE_SIZE -- Leave clearance beyond the commander footprint.
local BUILD_MAX_RADIUS = 32 * TILE_SIZE -- Local sites within a short builder approach.
local BUILD_RADIUS_STEP = 4 * TILE_SIZE -- Search each four-square ring.
local BUILDABLE = 2 -- Spring.TestBuildOrder returns 2 for an unobstructed valid site.
local PIXEL_QUANTUM = 1 / 255 -- One RGB8 color step; gl.ReadPixels returns normalized floats.
local MIN_VISIBLE_SAMPLES = 2 -- A cursor-sized mark cannot substitute for the world and UI.

local function test()
    local function frames(seconds) return seconds * Game.gameSpeed end
    Test.expectCallin("DrawScreen")
    Test.expectCallin("DrawScreenPost")
    -- Startup sets cheat/devlua idempotently before invoking the runner.
    -- Toggle visibility once here, after those synchronized commands arrive.
    Spring.SendCommands("globallos")
    assert(Spring.GetLocalTeamID() == 0, "Player is not controlling team 0")
    assert(not Spring.GetSpectatingState(), "Player is a spectator")
    assert(Spring.GetTeamLuaAI(1) == "SimpleAI", "Real SimpleAI opponent is absent")
    assert(Game.mapSizeX > 0 and Game.mapSizeZ > 0, "Terrain is absent")
    assert(WG.buildmenu and WG.ordermenu, "BAR build/order UI did not initialize")

    local commander
    Test.waitUntil(function()
        for _, unit in ipairs(Spring.GetTeamUnits(0)) do
            local def = UnitDefs[Spring.GetUnitDefID(unit)]
            if def and def.customParams and def.customParams.iscommander then
                commander = unit
                break
            end
        end
        return commander ~= nil
    end, frames(DRAW_TIMEOUT_SECONDS))
    Spring.SelectUnitArray({ commander })
    Test.waitUntilCallin("DrawScreen", function() return true end, frames(DRAW_TIMEOUT_SECONDS))
    -- DrawScreenPost runs after Rml composition and cursor drawing (CGame::Draw).
    -- Sample separated quarter-viewport landmarks, away from the central cursor.
    -- This catches the cursor-only black frame even when world/UI callbacks run.
    Test.waitUntilCallin("DrawScreenPost", function()
        local width, height = gl.GetViewSizes()
        local visible = 0
        for _, point in ipairs({ { 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 } }) do
            local r, g, b = gl.ReadPixels(math.floor(width * point[1] / 4),
                math.floor(height * point[2] / 4), 1, 1)
            if r + g + b > PIXEL_QUANTUM then visible = visible + 1 end
        end
        return visible >= MIN_VISIBLE_SAMPLES
    end, frames(DRAW_TIMEOUT_SECONDS))
    Spring.SendCommands("screenshot")
    Spring.Echo("BAR-SMOKE completed frame retains visible world/UI; screenshot requested")

    local x, _, z = Spring.GetUnitPosition(commander)
    local defID = Spring.GetUnitDefID(commander)
    local targetX, targetZ
    -- Polar offsets: x=r*cos(angle), z=r*sin(angle), in elmos/radians.
    -- Source: https://en.wikipedia.org/wiki/Polar_coordinate_system#Cartesian_coordinates
    for step = 0, MOVE_DIRECTIONS - 1 do
        local angle = step * 2 * math.pi / MOVE_DIRECTIONS
        local tx, tz = x + math.cos(angle) * MOVE_DISTANCE, z + math.sin(angle) * MOVE_DISTANCE
        if tx > BORDER_MARGIN and tz > BORDER_MARGIN
            and tx < Game.mapSizeX - BORDER_MARGIN and tz < Game.mapSizeZ - BORDER_MARGIN
            and Spring.TestMoveOrder(defID, tx, Spring.GetGroundHeight(tx, tz), tz) then
            targetX, targetZ = tx, tz
            break
        end
    end
    assert(targetX, "No traversable move target near player commander")
    Spring.GiveOrderToUnit(commander, CMD.MOVE, { targetX, Spring.GetGroundHeight(targetX, targetZ), targetZ }, 0)
    Test.waitUntil(function()
        local nx, _, nz = Spring.GetUnitPosition(commander)
        -- Squared Euclidean distance avoids a square root; compare elmos squared.
        return nx and (nx - x) ^ 2 + (nz - z) ^ 2 > MOVE_OBSERVED_DISTANCE ^ 2
    end, frames(MOVE_TIMEOUT_SECONDS))
    Spring.GiveOrderToUnit(commander, CMD.STOP, {}, 0)
    Spring.Echo("BAR-SMOKE player MOVE changed commander position")

    x, _, z = Spring.GetUnitPosition(commander)
    local solar = UnitDefNames.armsolar.id
    local bx, by, bz
    for radius = BUILD_MIN_RADIUS, BUILD_MAX_RADIUS, BUILD_RADIUS_STEP do
        for step = 0, BUILD_DIRECTIONS - 1 do
            local angle = step * 2 * math.pi / BUILD_DIRECTIONS
            local tx, tz = x + math.cos(angle) * radius, z + math.sin(angle) * radius
            local px, py, pz = Spring.Pos2BuildPos(solar, tx, Spring.GetGroundHeight(tx, tz), tz, 0)
            if px > BORDER_MARGIN and pz > BORDER_MARGIN
                and px < Game.mapSizeX - BORDER_MARGIN and pz < Game.mapSizeZ - BORDER_MARGIN
                and Spring.TestBuildOrder(solar, px, py, pz, 0) == BUILDABLE then
                bx, by, bz = px, py, pz
                break
            end
        end
        if bx then break end
    end
    assert(bx, "No valid solar build site near player commander")
    Spring.GiveOrderToUnit(commander, -solar, { bx, by, bz, 0 }, 0)
    Test.waitUntil(function()
        for _, unit in ipairs(Spring.GetTeamUnits(0)) do
            if Spring.GetUnitDefID(unit) == solar then
                local _, _, _, _, progress = Spring.GetUnitHealth(unit)
                if progress and progress >= 1 then return true end
            end
        end
        return false
    end, frames(BUILD_TIMEOUT_SECONDS))
    Spring.Echo("BAR-SMOKE player BUILD completed armsolar using actual resources")

    -- Enemy orders are not delivered to LuaUI's UnitCommand callin. Observe
    -- authoritative queues through BAR's existing synchronized test RPC instead.
    -- Read-only queries: the AI, not the test, submits and pays for these orders.
    local aiBuild
    local deadline = Spring.GetGameFrame() + frames(AI_TIMEOUT_SECONDS)
    repeat
        aiBuild = SyncedRun(function()
            for _, builder in ipairs(Spring.GetTeamUnits(1)) do
                local command = Spring.GetUnitCurrentCommand(builder)
                if command and command < 0 and UnitDefs[-command].isBuilding then
                    local finished = {}
                    for _, unit in ipairs(Spring.GetTeamUnits(1)) do
                        local _, _, _, _, progress = Spring.GetUnitHealth(unit)
                        if progress and progress >= 1 then finished[unit] = true end
                    end
                    return { definition = -command, finished = finished }
                end
            end
            return false
        end, frames(DRAW_TIMEOUT_SECONDS))
        if not aiBuild then Test.waitFrames(Game.gameSpeed) end
    until aiBuild or Spring.GetGameFrame() >= deadline
    assert(aiBuild, "SimpleAI did not issue an economic construction order")

    local complete = false
    deadline = Spring.GetGameFrame() + frames(AI_TIMEOUT_SECONDS)
    repeat
        local expected = aiBuild.definition
        local finished = aiBuild.finished
        complete = SyncedRun(function(locals)
            for _, unit in ipairs(Spring.GetTeamUnits(1)) do
                if not locals.finished[unit] and Spring.GetUnitDefID(unit) == locals.expected then
                    local _, _, _, _, progress = Spring.GetUnitHealth(unit)
                    if progress and progress >= 1 then return true end
                end
            end
            return false
        end, frames(DRAW_TIMEOUT_SECONDS))
        if not complete then Test.waitFrames(Game.gameSpeed) end
    until complete or Spring.GetGameFrame() >= deadline
    assert(complete, "SimpleAI's commanded building did not finish")
    Spring.Echo("BAR-SMOKE SimpleAI issued a build order and completed the commanded building")
    Spring.SendCommands("screenshot")
    Spring.Echo("BAR-SMOKE PASS; upstream runner writes testlog/results.json and quits")
end

return { test = test }
