-- Per-frame timing for the Recoil GL baseline.
--
-- LuaUI's main.lua defines callins as globals; no widget framework is involved.
--
-- The measured quantity is wall time between successive DrawScreen calls, i.e.
-- the engine's frame period. That is deliberately the SAME quantity
-- recoil-metal's offscreen benchmark reports as cpu_ms, because it is the only
-- axis available on both sides: Recoil exposes no GPU timer to Lua.
--
-- TWO THINGS THIS GETS RIGHT, both learned by getting them wrong first:
--
-- 1. Samples are buffered and emitted only after the run. Echoing inside the
--    loop puts a synchronous log write in every frame being measured, which
--    inflates the very number under test. The first attempt did that and
--    produced ~8.6 ms/frame; without it the figure is materially different.
--
-- 2. The timer is Spring.GetTimerMicros with DiffTimers(..., fromMicroSecs=true).
--    Spring.GetTimer has millisecond resolution, so at ~7 ms/frame every sample
--    quantised to a whole number and the percentiles landed on integers.
--    Note the timers are lightuserdata, NOT numbers: subtracting them directly
--    raises a Lua error, which silently kills the callin and yields no samples
--    at all rather than anything obviously wrong.

local WARMUP_FRAMES = 60
local TARGET_FRAMES = 2000

local previous = nil
local samples = {}
local skipped = 0
local finished = false

function DrawScreen()
	if finished then
		return
	end

	local now = Spring.GetTimerMicros()

	if previous ~= nil then
		-- returnMs=false gives seconds as a float (full precision);
		-- fromMicroSecs=true tells it the operands came from GetTimerMicros.
		local ms = Spring.DiffTimers(now, previous, false, true) * 1000.0

		if skipped < WARMUP_FRAMES then
			-- Map load, shader compilation and texture upload land in the first
			-- frames. Discarded on both sides of the comparison.
			skipped = skipped + 1
		else
			samples[#samples + 1] = ms
		end
	end

	previous = now

	if #samples >= TARGET_FRAMES then
		finished = true

		-- Emit only now that timing is over. Batched, because one Echo per
		-- sample is thousands of log writes.
		local chunk = {}
		for i = 1, #samples do
			chunk[#chunk + 1] = string.format("%d:%.4f", i, samples[i])
			if #chunk == 50 then
				Spring.Echo("RMBENCH " .. table.concat(chunk, " "))
				chunk = {}
			end
		end
		if #chunk > 0 then
			Spring.Echo("RMBENCH " .. table.concat(chunk, " "))
		end

		Spring.Echo(string.format("RMBENCH-DONE %d frames", #samples))
		-- quitforce skips the confirmation path, which needs no input device.
		Spring.SendCommands("quitforce")
	end
end
