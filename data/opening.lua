-- The scripted opponent's opening.
--
-- This file exists because the same plan used to be five constants in a C++ header
-- (core/sim/BuildOrder.hpp): four blueprint paths and kAttackWaveTanks. Changing the
-- opening meant editing and recompiling, and the four paths were all UEF, so the
-- opponent could only ever play one faction.
--
-- Everything here is stated by ROLE. The roles resolve through a Roster at load
-- (core/data/Roster.hpp), which answers "the T1 extractor for THIS faction" from the
-- blueprint corpus — so this one file drives UEF, Cybran, Aeon and Seraphim.
--
-- tech = 0 means "the cheapest this faction fields", which is what an opening usually
-- wants: it should not have to know which tiers a faction has.

{
    -- What the commander builds, in order. The first is ordered the moment it spawns.
    structures = {
        { role = 'extractor', tech = 1 },  -- first: the only thing affordable at t=0
        { role = 'energy',    tech = 1 },  -- second: everything after this is energy-bound
        { role = 'extractor', tech = 1 },  -- third: tanks are mass-bound
        -- LAND, explicitly. A role alone is ambiguous in a game with three domains: the
        -- cheapest T1 factory is the AIR factory, so without this the opening built one and
        -- the "tank wave" came out as air scouts. Found by running the match.
        { role = 'factory',   tech = 1, requires = { 'LAND' } },
    },

    wave = {
        -- What the factory produces, and what the attack is made of.
        -- A TANK IF THE FACTION HAS ONE, otherwise any T1 land unit.
        --
        -- The deleted constant named UEL0201, the UEF T1 tank, and the obvious translation was
        -- `requires = { 'LAND', 'TANK' }`. That resolves for UEF, Aeon and Seraphim and returns
        -- nothing for Cybran — because CYBRAN FIELDS NO T1 TANK. Its T1 army is bots (URL0107
        -- and siblings); the tank line starts at T2 with URL0202. A faction design decision in
        -- Supreme Commander, not a gap in the data.
        --
        -- Dropping TANK entirely gets everyone a bot, and 40 bots do NOT close a match that 20
        -- tanks do: bots die four times faster than the wave model assumes, so they are killed
        -- before they arrive in force. Measured, over a 900-second match that never resolved.
        --
        -- So the plan says both, which is what a human would write.
        unit = { role = 'raider', tech = 1, requires = { 'LAND', 'TANK' }, fallback = { 'LAND' } },

        -- How many before the one attack launches.
        --
        -- ARITHMETIC, NOT TASTE, and the numbers are the blueprints': a UEL0001 commander
        -- (12000 hp, 100 dps — its zephyr states Damage = 100, RateOfFire = 1) kills one
        -- 300 hp UEL0201 every 3 seconds, so a wave of N tanks at 24 dps each lands roughly
        -- 24 * 3 * N(N+1)/2 damage before it is gone. N = 20 gives 15,120 against the
        -- commander's 12,000; N = 19 gives 13,680. The margin over the model's optimism
        -- (travel time, walls of dead tanks blocking the living) is deliberate, and the stream
        -- of reinforcements behind the wave is what actually closes a match the model gets
        -- wrong.
        --
        -- ONE NUMBER FOR EVERY FACTION, which is a known simplification: the same model gives
        -- N = 39 for a bot, so a Cybran wave of 20 is under-strength. The wave size wants to be
        -- per-unit rather than per-plan, and that is a change to this file's shape rather than
        -- to the engine — recorded here rather than pretended away.
        size = 20,
    },
}
