# Overnight task: per-unit sanity test matrix for all 568 retail blueprints

Work in `/Users/olek/projects/llm/games/recoil-metal`. Read `AGENT.md` first — its rules
are binding (`mise exec --` for every tool invocation, TDD for anything pure, comments
explain why and cite sources, `make test` must be green before done).

## Goal

Create `tests/test_unit_sanity.cpp` — a corpus-wide sanity pass that checks every unit
DOES the basic things its blueprint implies: kills what it should kill, dies to what
should kill it, wins/loses group fights sensibly, and aims towers at the enemy. This is
the foundation for later per-unit feature tests.

## What already exists — reuse it, do not rebuild it

`tests/test_unit_behaviors.cpp` is the model. It iterates `~/projects/llm/input/faf/units`
(568 `*_unit.bp` files), parses each with `rm::unitbp::loadFile`, and runs blueprint-derived
contracts per unit inside `DYNAMIC_SECTION(def.name)`:

- `struct Battle` — owns a `rm::test::Roster`, two free-for-all armies, a projectile list,
  an event queue, and flat terrain. `tick()` calls `aimAtTargets`, `fireWeapons`,
  `advanceProjectiles`. `add()` spawns a unit, `damaged()` checks health, `firedFor()`
  matches weapon labels on firing events.
- `targetDefFor(weapon)` synthesizes prey categories satisfying the weapon's
  TargetPriorities/TargetRestrict contract.
- `tests/support/TestRoster.hpp` — `addType`, `add(type,x,z,army,hp)`, `health()`,
  `motion()`, `transform()`, `reindex()`.
- `tests/support/FxMatchers.hpp`, `rm::test::fx`, `rm::test::asFloat`, `rm::test::at`.

Tag the new case `[corpus][sanity][unit-matrix]` so `tools/content_acceptance.py`
(`make test-content`) picks it up. Current corpus pass is ~54s; keep the new one in the
same order of magnitude — derive tick budgets, don't blanket-iterate.

## The contracts, in implementation order

Get each green for the whole corpus before starting the next.

1. **LETHAL** — every unit with at least one firing weapon kills a defenceless synthetic
   dummy. Spawn prey (categories via `targetDefFor`, or MOBILE/ALLUNITS fallback) at the
   weapon's mid-range on the layer it can reach; dummy HP and tick budget derived from the
   weapon's own damage/rateOfFire with ~3x headroom. Assert the dummy dies, not just takes
   damage. Skip (don't fail) units whose weapons need an untestable acquisition contract —
   same convention as ENGAGE's third case.

2. **MORTAL** — every unit dies to an overpowered synthetic attacker (a synthetic UnitDef
   with one weapon: huge damage, generous range, matching TargetPriorities, any layer the
   victim occupies). Assert the victim's death within the budget. Proves damage/armor
   application works on every blueprint, not only the ones that shoot back.

3. **SQUAD** — repeat the LETHAL duel at 4v4: four shooters in a line (~10 elmo spacing),
   four prey. Assert all prey die and at least one shooter survives. Catches group-targeting
   and AoE degeneracies a 1v1 misses. Fixed-point sim means deterministic outcomes.

4. **ORDERED** — the one judgment call: pair real corpus units for duels. A unit-vs-unit
   outcome is emergent balance, not a blueprint contract — keep the rule explicit and
   conservative so failures mean something:
   - Eligible pair: both armed, mutual engagement possible (at least one shared layer both
     can target each other on), same faction prefix, and the "stronger" unit's
     `buildCostMass` >= 2x the weaker's.
   - Assertion: the stronger unit wins the 1v1 (weaker dies first).
   - Where a unit has no eligible partner, skip — synthetic dummies already cover the floor.
   - Expect counterexamples. When the corpus fails on a pairing, the failure is a finding:
     put it in a `// known-balance:` list at the top of the file with the matchup and the
     observed outcome, and keep the test green by excluding documented cases ONLY after
     verifying the mechanism (e.g. kiting, out-ranging, counters). Do not blanket-skip.

5. **TOWER-AIM** — for STRUCTURE-category units with a slewable turreted weapon, verify the
   mount slews toward a hostile before the first firing event, using the authored
   `TurretMountSpec` when the catalog provides one (fall back to the synthetic mount
   `checkTurret` already builds). Structure has no hull to bring round — the turret does
   all the work, which is the point of the check.

## Rules for the implementation

- One TEST_CASE iterating the corpus, `DYNAMIC_SECTION(def.name)` per unit — failures
  must report per unit name via `INFO()` like the behaviors file does.
- Every expectation derived from the blueprint under test — no hand-maintained stat
  tables. Budgets come from the unit's own damage/HP/range numbers.
- Deterministic: fixed-point sim, flat `HeightField`, no wall-clock.
- Register the file in `CMakeLists.txt` next to `test_unit_behaviors.cpp`.
- Verify: `mise exec -- cmake --build build`, run the new case, then
  `mise exec -- ctest --test-dir build --output-on-failure` fully green.

## Deliverable

Commit to a NEW branch `feat/unit-sanity-matrix` (never directly on main) with a message
describing the contracts. If you could not finish all five contracts, commit what is green
and leave the rest listed at the top of the file as TODO with the reason.
