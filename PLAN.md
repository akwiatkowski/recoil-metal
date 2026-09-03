<!-- Generated and maintained by Claude -->
# PLAN.md — the road to a playable Supreme Commander skirmish

[`README.md`](README.md) is the log of what has been built, milestone by
milestone. This file is the direction: what "a simple Supreme Commander game"
means here, what it needs, and what it deliberately does not.

Milestones 1–20 are done and documented in the README. Their sections below
record how they landed; the current direction starts at **Beyond 20**.

Current Forged Alliance subsystem completion, exact next tasks, and copy-ready
goal prompts live in the canonical
[`gameplay progress dashboard`](docs/fa-gameplay-progress.md).

**Near target, stated once so it can be checked:** two armies on a retail
`.scmap`, each with an ACU, extracting mass, building from a factory, fighting
with weapons read from the shipped blueprints, and a match that *ends* — last
ACU standing. Playable on `SCMP_009` from the same binary that renders it today.

**The horizon, stated once so the near target has a direction:** an open-source
Moho. This engine, GPL-2.0-or-later like the Recoil code it adapts, playing
Supreme Commander on macOS **and Linux**, running the existing mods through a
hosted Lua layer while offering a clean modern API for new ones (ADR-028), and
playing multiplayer through FAF's open lobby ecosystem on its **own**
deterministic lockstep protocol — never the retail one, whose desyncs are the
reason FAF needs an engine at all (ADR-030). Beyond All Reason stays supported
at the content level — maps, models, blueprints — not as a Recoil replacement
(ADR-031). Who this is for: everyone who wants to upgrade the game and keep
playing it. The tracks from here to there are at the end of this file.

---

## The decision that shapes everything

Supreme Commander's gameplay is largely Lua: 228k lines across 1414 files, of
which a minimal skirmish would need ~38k (the sim, 568 unit scripts, 288
projectile scripts). Reusing it looks like the cheap road. It is not, and the
measurement is what says so.

That Lua is not a library to call. It is a **client of an API this engine would
have to provide first** — 315 distinct functions the sim subset calls that are
defined nowhere in those 228k lines, across 6734 call sites, plus 193 `On*`
callbacks the engine must fire back. `Unit.lua` alone — the class every unit
is — touches 127 of them, 30 in value-returning position.

And those 315 functions *are* the behaviour. Motion (1174 call sites), damage,
targeting, economy — all C++ in the original; the Lua orchestrates and
decorates. Hosting it means writing the same simulation **plus** a binding
layer **plus** a Lua 5.0 fork (the corpus uses `#` line comments in 1240 of
1414 files, and `table.getn`/`math.mod`/`arg[]`, all removed after 5.0) **plus**
a VFS with archive priority (`lua.scd` overrides `mohodata.scd`'s Unit.lua,
117 lines → 3715). And the sim's semantics would then have to match an
undocumented, closed-source API exactly, with failures surfacing thousands of
lines into someone else's script.

**Two arguments against hosting it turned out to be wrong, and are recorded here
so they are not made again.**

*It is not slow.* Measured on this machine, 1000 units at 10 Hz: the whole tick
in native C costs 5.5 µs; Moho's shape — native motion, Lua woken per unit per
tick making a handful of engine calls — costs 127 µs, of which 121 µs is the Lua.
That is **0.12% of a 100 ms tick budget**, about a sixth of what the renderer
spends *drawing* 800 animated units. The only way to make Lua expensive is to put
per-unit-per-tick arithmetic in it (6× native when measured), and Moho's API
shape structurally prevents that, because the 315 functions are the hot paths.
The real exposure is Lua 5.0's stop-the-world GC over thousands of unit tables —
an argument for 5.1 plus a compat shim, not against Lua.

*It is not all-or-nothing.* Lua is late-bound: a missing engine function errors
only on the line that calls it. So a host can be brought up incrementally — run
the scenario, hit `attempt to call a nil value`, implement that one function,
repeat. An earlier draft of this plan claimed the opposite and it was the
strongest argument it had.

**Decision: reimplement the semantics in C++ first, and treat a Lua host as the
destination rather than an alternative.** The near-term reason is time to a
playable skirmish: road A is the same simulation plus a binding layer plus a Lua
fork plus a VFS, against a closed-source API with no reference to diff. The
long-term reason to keep going is that **modding is how people change this
game**, and no C++ sim accepts a Supreme Commander mod at any price.

So the repo's existing rule still applies — *formats convert, behaviour gets
reimplemented* (ADR-004, ADR-005) — and the Lua is read as a **specification**,
the way `terrain.fx` and `water2.fx` were read for the ground and the sea:
`defaultweapons.lua` (837 lines) states the firing state machine, `Unit.lua` the
damage and build model, the blueprints the constants. Cited `file:line`, same as
the shaders. But the native sim is shaped so the host can arrive later — see the
next section, which is the part of this plan most easily lost.

Full numbers behind this section: `docs/recoil-metal/supcom-lua-gameplay-survey.md`
in the knowledge base.

---

## Designing for a Lua host that does not exist yet

Commenting alone does not buy this. Four decisions are free today and expensive
after milestone 20; everything else about a host can wait.

1. **The sim ticks at 10 Hz, matching Supreme Commander, not the current 30.**
   Its scripts hardcode the rate: `WaitSeconds(n)` is `WaitTicks(n * 10)`
   (`mohodata/lua/simInit.lua:37`). A 30 Hz sim runs every hosted script's
   timing 3× fast, and the fix after the fact is not the tick constant but every
   tuned value that grew up around it. This engine has no lockstep requirement
   forcing 10 Hz, and no reason to refuse it either — render stays decoupled at
   display rate, as it is now.

   **Done in milestone 16**, and it proved its own case twice. Two facts were
   sharing one number: BAR's `turnrate` is per *Recoil* frame, so the loader was
   using the sim's rate to get Recoil's 30, and changing the tick alone would have
   slowed every BAR unit's turning threefold in silence. And one tuned value had to
   follow the rate — the arrival radius, a flat 4 elmos, which at 10 Hz is less
   than one tick of travel, so a unit stepped past its goal every tick and never
   landed inside it.

2. **Native functions carry moho's names, semantics, and units.** `GetBlueprint`,
   `GetPosition`, `SetSpeed`, `SetAccel`, `SetGoal`, `GetHealth`, `AdjustHealth`,
   `DamageArea`, `CreateProjectile`, `SetConsumptionPerSecondMass`. Where the
   original takes ogrids or degrees, the C++ entry point does too and converts
   inward. That turns the eventual binding layer into a mechanical shim instead
   of a translation, and it costs exactly one naming decision per function.
   Where our semantics deliberately differ, say so at the definition.

   **Audited 2026-08-20, and amended by ADR-028.** Not one of the 319 names
   appears in `src/` — the engine grew its own, better vocabulary
   (`orderRouted`, `heightAt`, `tickEconomy`) — while the audit hand-wrote the
   moho→C++ mapping table this decision existed to avoid (~120 lines, in the
   survey doc). The cost this decision predicted has therefore been paid once,
   as a table. Amended: **the shim owns the name mapping; the engine keeps its
   vocabulary and owns the unit conversions at its boundary.** The table
   becomes committed code when the host arc starts.

3. **Events are a documented set, dispatched by name.** The sim fires
   `onCreate`, `onStopBeingBuilt`, `onKilled`, `onDamage`, `onStartBuild`,
   `onStopBuild`, `onGotTarget`, `onLostTarget`, `onImpact`, `onLayerChange` —
   the subset of the original's 193 that these milestones actually reach — as a
   listed, tested set rather than as calls scattered through the sim. A host
   subscribes to the same list.

   **Audited 2026-08-20: not yet true.** One event exists (`onFinalWaypoint`);
   the rest are implicit in C++ call order. The cheap honest version, owed
   before the combat and economy code goes stale: fire the ten into a log sink
   — no subscribers, no dispatcher machinery — so the *ordering* is pinned
   while it is still remembered, and replay tests can assert the stream.

4. **Content loads through a VFS with archive priority.** `.scd` files are ZIPs
   and miniz is already vendored. This is not Lua-specific work: **blueprint
   mods are the majority of real mods** and they work by layering an archive over
   the stock one, so a VFS earns its keep in the pure-C++ engine too. Extracting
   with a Python one-liner (`tests/test_real_scm.cpp:8`) is fine for fixtures and
   wrong for the app. **Done** — the engine now reads the archives in place,
   layered, and the whole scene loads through them (`feat(vfs)` commits after
   milestone 16); the 25 `MeshName` blueprints were the first thing that
   needed it.

**What this does *not* buy, stated plainly so nobody is surprised later:** the
hard part of hosting the game's Lua is semantic fidelity across ~200 functions,
and no amount of preparation today makes that free. These four decisions make
the shim mechanical and the timing correct. They do not make the fidelity job
smaller.

**The fork this section left open is decided — ADR-028: both, layered.**
"Moddable" and "runs existing Supreme Commander mods" are different products,
and the goals want both. So the engine's *own* clean Lua API is the real one —
what the C++ exposes, what new mods target, what gets tooling — and moho's API
is a **compatibility shim written in Lua on top of it**, dialect loader and
all (ADR-029), maintained where the community can patch fidelity without
touching the engine. Decisions 1–4 above serve both tiers, which is why they
could be taken early.

---

## Non-goals

Stated up front, because a skirmish is only reachable if most of the game is
out of scope. These bound milestones 16–20, not the project — multiplayer and
intel return as tracks at the end of this file.

- **Not 568 units.** Six to eight, hand-picked: ACU, engineer, land factory,
  mass extractor, power generator, T1 tank, T1 bot, point defence.
- **No shields, stealth, radar jamming, nukes, transports, experimentals,
  veterancy, adjacency bonuses, or tech tiers.**
- **No fog of war or intel.** Everything is visible. Intel is 16 symbols and
  165 call sites in the original and is its own milestone if ever wanted.
- **No game AI.** The opponent in milestone 20 is a scripted build order, and
  the plan says so in the code. Supreme Commander's own AI is 84,750 lines of
  Lua and is not being reimplemented.
- **No lockstep or multiplayer** — *in 16–20*. It returns as its own track
  (ADR-030). Single machine for now, so bit-exact determinism is not yet
  required — though the fixed tick keeps replays reproducible, which `--march`
  already relies on.
- **No campaign, objectives, cinematics, or the game's UI.** The HUD is ours
  and minimal.

---

## Milestones

### 16. Supreme Commander units read their own definitions

**Mostly done.** `--units UEL0201_unit.bp 40 --march 4096 4096 30` puts UEF
medium tanks on `SCMP_009` at 27 elmos/s, 1.57 rad/s and a 3.6-elmo radius, all
read from the file, and 26 of 40 route to the map centre with dust behind them.
![Supreme Commander units on a Supreme Commander map](docs/images/m16-supcom-units.jpg)

What the work actually turned out to be, since half the plan above was wrong:

- **`UnitBlueprint` needed no allow-list entry.** The reader scans to the first
  table literal, so a top-level wrapper's name is never looked at — the same
  source under any other name parses too. The allow-list governs calls in *value*
  position, and `UnitBlueprint` appears once per file, never nested. A test says
  so now, because the next person will assume otherwise.
- **`#` had to become a mid-line comment.** One blueprint in 568 writes
  `NukeCharge = Sound { # added for sound bug`. Widened by measurement: 209
  mid-line `#` comments across all 4065 shipped `.bp` files against **zero** uses
  as Lua's length operator. It stays refused directly after `=`, where it would be
  that operator and swallowing the line would drop the *next* field.
- **`SizeX`/`SizeZ` are at the file's ROOT, not under `Footprint`** — 568/568
  against 363, and `UniformScale` is under `Display`. The two are different
  quantities: a collision box in fractional ogrids, and a whole-square build
  footprint that only the structures state.
- **The collision radius had to become a stored float.** 418 of 568 sizes are
  fractional and 154 are under one ogrid, the smallest 0.01 — as whole squares
  that unit's radius would inflate from 0.04 elmos to 4.
- **The model was 14× too big, and had been since milestone 7.** `.scm` vertices
  are not in ogrids: raw extents run 10 to 262 units, which as ogrids would make
  one experimental 2096 elmos long. `Display.UniformScale` is the missing factor,
  exactly as for props, and it is stored as ONE combined `meshToElmos` because the
  two-step version has already been got wrong once here.
- **Passability comes from the motion class**, as predicted, and the numbers were
  already in the engine: `kDefaultMaxSlopeDegrees` of 17 is 25.5 real degrees, a
  gradient of 0.48, which is what BAR's ground units authorise too. The only slope
  figure the format carries is `Footprint.MaxSlope` — a gradient, not an angle, on
  aircraft landing sites. Water and SurfacingSub are **refused** rather than
  approximated: they need the inverse of the grid, and a ship routed over the
  ground grid drives across dry land.
- **A ground mover's zero depth is not a missing value.** The fallback read 0 as
  unstated and substituted BAR's 12 elmos, which would walk SupCom's land units
  into the sea. It asks the motion class now.
- **The corpus test earned its keep three times**: 38 units have no mesh beside
  them (25 name one with `MeshName`), a further "16 name another unit's id" is a
  regex artefact of `PlaceholderMeshName`, and two blueprints state a scale of
  zero. All three are in the code as comments.

The tick moved to **10 Hz** with it (decision 1), which exposed two facts sharing
one number — BAR's `turnrate` is per *Recoil* frame and wants 30, not our rate —
and one tuned value that had to follow the rate, the arrival radius.

Still open, and carried into 17:

- **A VFS with archive priority** (decision 4). Extraction is still a documented
  Python one-liner, which is right for fixtures and wrong for the app. The 25
  `MeshName` blueprints need a root, and only 21 resolve without one.
- **LOD switching.** Only level 0 is resolved; 563 blueprints declare cutoffs and
  `meshBeside` already takes a level, so this is wiring rather than research.
- **The MotionType ADR**, once a second reader needs the mapping.

### 17. Armies, and units that belong to one — **✔ done**

Nothing in the engine currently knows who owns a unit; team colour is indexed
by batch (`core/scene/TeamColours.hpp`).

- An `Army`: index, faction, colour, alliance. Ownership on every instance.
- Spawn from the map's own `ARMY_<n>` markers, which `scenario::loadStartPositions`
  already reads, and give each army its faction ACU.
- Read the rest of the marker table while there: 3508 `Mass` markers and the
  hydrocarbon sites are what milestone 19 needs, and the shipped nav graph
  (1974 Land / 2120 Amphibious / 1731 Air path nodes) is a free reference to
  check A* against. Note the stock maps place **no units** — all 61 army
  `Units` groups are empty — so spawning is the engine's job, not the map's.
- Selection restricted to your own army; colour taken from the army.

**Done when:** two ACUs face each other on `SCMP_009` in their armies' colours,
and clicking an enemy selects nothing.

### 18. Weapons, projectiles, and damage — **✔ done**

The first milestone where units can lose something.

- Read the `Weapon` table per blueprint: `RateOfFire`, `Damage`,
  `DamageRadius`, `MaxRadius`, `MuzzleVelocity`, `ProjectileId`, the turret
  bones. Spec to read alongside it: `mohodata/lua/sim/defaultweapons.lua` for
  the firing state machine and `lua/sim/Unit.lua` for the damage model.
- Target acquisition: nearest enemy within `MaxRadius`. The separation pass
  already sweeps neighbours per tick (`Movement.hpp:120`) — the same broadphase
  serves both.
- Projectiles: direct and ballistic, flight on the fixed tick, impact,
  `DamageArea` semantics with falloff. Health, death, and a wreck left behind.
- Turrets aim through the existing bone path, so a tank's barrel tracks what it
  shoots rather than the hull turning to face it.
- TDD applies in full: rate-of-fire timing, damage falloff at radius, the
  ballistic solution, and target selection are all pure.

**Done when:** two groups of tanks meet on a `.scmap`, fight, and one group is
left standing with wrecks between them.

**What it came to.** 414 shots over 120 seconds sends eight commanders to mutual
destruction on SCMP_009, and the fight unfolds tick by tick rather than resolving at once.
Three corrections the tests forced, each in the code: reloads were 10% slow (the counter
was checked before being decremented), a flat shot detonated at the muzzle (a unit's
position is at its FEET, so a level shot began at ground height), and with only a
ground-height test every weapon then flew over its target and missed. Emergent and
honest: nothing leads a target, so distant moving units are hard to hit and close ones are
not. Still absent — wreckage, a death explosion (99 of the 494 weapons ARE one, read and
never fired), and turret aiming, which is why firing is not gated on facing.

### 19. Economy and building — **✔ done**

- Mass and energy income, storage and drain per army; the map's `Mass` markers
  become extractor sites.
- Build costs from the blueprints (`BuildCostMass`, `BuildCostEnergy`,
  `BuildTime`) against a builder's `BuildRate`; consumption gated on income, so
  a stalled economy slows construction rather than cheating.
- The ACU and an engineer build structures; a land factory builds units and
  rolls them off (`IssueMoveOffFactory` is what the original does).
- Build progress shown simply — the game's build scaffold and drop-in animation
  are cosmetic and explicitly deferred.

**Done when:** the ACU builds an extractor and a factory, and the factory
produces tanks paid for out of a running economy.

### 20. A skirmish that ends — **✔ done**

- A scripted opponent: a fixed build order plus attack-move. Not an AI, and
  labelled as such in the source.
- Win condition: ACU destroyed → army defeated → last army standing wins.
- A minimal HUD — mass and energy, unit count, and a victory banner. Ours, not
  the game's.

**Done when:** a match on `SCMP_009` can be played from spawn to victory, and
`--screenshot` can prove each stage of it.

**Done, and provable in one line:** `--skirmish --armies 2 --play 520 --screenshot`
plays the whole match from spawn to the victory banner, deterministically — the
same command at any shorter duration is a screenshot of that stage of the same
match. Extractor at 5.9s, power at 18.4s, factory at 54.9s, first tank at 68.9s,
the attack at 335.0s with all 20 tanks routed, `team 1 WINS` at 502.2s — and the
falling commander's death explosion takes eleven of the attackers with it, which
is the milestone-18 nuke finally read aloud. Staged proof in
`docs/images/m20-1-extractor.jpg` … `m20-6-victory.jpg`.

What the milestone actually required, since most of it was not in the bullets:

- **The opponent is `core/sim/BuildOrder.hpp`** — three pure functions (the
  commander's next structure, the factory's next tank, the one attack launch)
  over a caller-built `ArmyView`, tested in `tests/test_build_order.cpp`. The
  wave size is the blueprints' own arithmetic — a 100-dps commander kills a
  300 hp tank every 3 s, so N tanks at 24 dps land `72·N(N+1)/2` damage before
  dying, and N=20 clears 12000 hp with a stated margin of two — and a test
  recomputes the minimum so the constant cannot drift into taste.
- **A finished construction had to BECOME A UNIT.** Milestone 19 ended at
  income bookkeeping; nothing it built ever stood on the map. `spawnUnit` now
  gives a finished Construction a batch, health, colour and motion, and a
  finished tank rolls off the factory toward a rally point — or straight into
  the attack once the wave has gone.
- **Income is recomputed from what is standing**, each tick, instead of
  accumulated when builds finish — a structure that dies now takes its
  production, upkeep and storage with it, which the incremental version
  silently got wrong.
- **Armies now start with their storage banked**, as the game does. The empty
  start survived milestone 19 only because the extractor was the sole build: a
  750-energy power generator against a 5-a-second trickle parks the build
  order for four minutes.
- **The match found a weapons bug the corpus tests could not.** The commander
  carries `ManualFire = true` (OverCharge, 12000 damage) and
  `EnabledByEnhancement` weapons (TacMissile, 2048-elmo range), and the loader
  read both as guns — the player's commander sniped the tank column from a
  fifth of the map away and killed 19 of 20 before anything reached it. Both
  flags are now read, both weapons now wait for orders and upgrades that never
  come, and `tests/test_combat.cpp` says so.
- **Three flags, all of them capture plumbing**: `--play SECONDS` (the march
  sim without the blanket move order — the scripted armies decide their own
  movement), `--armies N` (the first N start positions; the match this
  milestone describes is a duel, and SCMP_009 declares eight), and
  `--look X Z RADIUS` (aim a capture at a world point; a match's stages happen
  at one base or the other, and neither is the first unit `--focus` finds).

Still honest about its limits: the windowed path runs no combat or economy, so
the *interactive* match is the pre-run one — `--play` then hand the scene over.
Making the window fight is the natural next slice of playability, and it is
glue rather than research: every per-tick piece already runs in `march()`.

#### Owed before 20 can be called done (added 2026-08-20)

Milestones 17–19 are ticked, and the skirmish still is not close. Two things
are wrong when you actually watch it run, and neither is a milestone-20 task —
they are debts left by 16–18 that 20 cannot be built on top of:

- **Movement is not smooth.** The sim ticks at 10 Hz (cross-cutting note
  below) and the renderer draws sim state directly, so units visibly step ten
  times a second. The tick rate is correct and stays; what is missing is the
  render-side half of the decoupling that was assumed done — interpolate each
  drawn transform between the previous and current sim state by the fraction of
  a tick elapsed, so the picture is continuous while the simulation stays
  discrete. Nothing about determinism changes: interpolation is presentation
  only and must never feed back into sim state.
- **Units do not shoot.** Milestone 18 is marked done and the weapon code
  exists, but in a running match nothing fires. That is a diagnosis owed before
  any new work — target acquisition, the aiming gate added in
  `feat(sim): weapons have to be pointed at what they shoot`, range and
  cooldown, and whether the scripted opponent's units are ever given anything
  to shoot at. The failing case gets a test before the fix, per AGENT.md rule 4.

Until both are fixed, "a skirmish that ends" is not a thing that can be
demonstrated, and the milestone-25 goal of hosting that skirmish in Lua is
measuring against something that does not yet play.

---

## Cross-cutting

- **Tick.** The sim runs a fixed tick with a clock that clamps catch-up
  (`core/sim/Movement.hpp:27`, `:192`), currently 30 Hz. Milestone 16 moves it to
  **10 Hz** for the reason in decision 1 above, and because it is the last moment
  that change is free — every constant tuned after this point would have to move
  with it. Blueprint rates are authored per second, so the conversion is explicit
  and testable either way; the render loop is unaffected, being decoupled from the
  tick already.
- **Tests.** AGENT.md rule 4 stands: anything that does not touch the GPU gets a
  failing test first, and parsers are tested against the real retail corpus —
  568 blueprints is a corpus, so blueprint reading gets the same treatment the
  1148 models and 60 maps got.
- **Constants.** Every number comes from a blueprint field or a cited line of
  the game's own Lua. No balance invented silently; where something must be
  chosen (the MotionType mapping, the scripted opponent's build order), it is
  named as ours and given a reason.
- **The seam.** Sim state stays plain structs behind narrow free functions, as
  `Movement.hpp` already does. No Lua host is built in milestones 16–20; the four
  decisions above are what keep one buildable in 21+.
- **Parallel work.** From the parity campaign on, this plan is executed by a
  fleet: analysis agents fan out one per open possibility envelope, implementer
  agents take one work package each in a `git worktree`, and one integrator owns
  tick order and the golden log. The rules are in AGENT.md under *Working as a
  fleet*, the reasoning in `ADR-069`, and the running order in
  `docs/fa-gameplay-progress.md` — which stays the single scheduler. Nothing
  about the fleet changes what counts as done: the confirmation gate is the same
  gate a single agent would face.

---

## Beyond 20 — the tracks (added 2026-08-20)

The horizon at the top of this file decomposes into four tracks. Their order
is dependency order; their sizes are honest guesses except where measured.

**Where the Lua host stands, measured** (audit of 2026-08-20, method and
numbers in `docs/recoil-metal/supcom-lua-gameplay-survey.md` in the knowledge
base): the sim contract is 319 functions across 4,601 call sites; the native
sim covers **44 of them — 13.8% by function, 45.9% weighted by call site**.
The spread is the finding: the hot core exists (`GetBlueprint`, `SetGoal`,
`DamageArea`, `CreateProjectile`…), the 240-function tail does not. That table
is the progress bar from milestone 21 on, so the audit scripts move from the
scratchpad into `tools/` and re-run per milestone. One measurement is owed
*before* 21: the contract restricted to the skirmish slice — `simInit.lua` +
`Unit.lua` + `defaultweapons.lua` + the eight units and their projectiles.
That number, not 319, is what milestone 25 must cover.

### Track 0 — the playable window

The match exists; the window has to catch up to it. `core/sim/Skirmish` already
moves the whole tick into one tested call, and the interface grows Forged
Alliance's anatomy in the engine's own instrument language — full design with
tokens, layout and rejections in `docs/recoil-metal/fa-ui-design.md` (knowledge
base). The slices, each screenshot-provable:

- **UI-0**: the frame loop runs `tickSkirmish` — the window fights.
- **UI-1**: the minimap, from the `.scmap`'s own embedded 256² preview, army
  pips, and the camera's ground footprint as a grabbable trapezoid — click moves
  the view, zoom untouched. **Done bar the drag**: the preview had been skipped
  since the format was decoded, on a comment that called it "always 256x256
  RGBA8" and was never checked — it is a DDS container, uncompressed BGRA, on all
  60 stock maps, and a test says so now. It needed the one thing the original
  minimap note claimed it would not: a second fragment function, because the text
  shader reads a texture's red channel as coverage and would have drawn a
  photograph as a one-colour silhouette.
- **UI-2**: the selection roster — typed tiles, `×N` badges, hp underbars.
  **Done.** Bottom centre, grouped by TYPE in first-appearance order (sorting by
  count would reorder the row as units die, which destroys the by-position
  reading a tiled roster exists for), summed health per group rather than per
  unit, and the same icon atlas the build tray uses — one texture for both
  panels, because the renderer binds one and two atlases would be two binds or
  one panel silently drawn with the other's slots. Capped at twelve tiles with
  the overflow reported as `+N` rather than dropped in silence.
- **UI-3**: the build tray — the archives' own `_icon.dds` (539 ship in
  `textures.scd`), cost-at-the-builder's-rate on hover, a placement ghost
  validated against the passability grid, and the first player-enqueued
  Construction.

  **Done, bar the icons.** The tray READS and now ACTS: click a cell to arm,
  click the ground to place, right-click or a second panel click to cancel. The
  ghost is a ring in the interface's own cyan where the footprint fits and red
  where it does not, coloured by `sitePlaceable` — the same call the placement
  makes, so the ghost and the order cannot disagree about a spot. The build goes
  through `applyCommand` like every other order, so a player's construction is
  authorised, recorded in the command log, and replayable.

  **And it draws the game's own icons.** 538 ship in `textures.scd` at
  `textures/ui/common/icons/units/<ID>_icon.dds`, 64x64 DXT5 — and because 64 is
  a multiple of the 4x4 block, packing them into one atlas is a memcpy of
  compressed blocks rather than a decode, a blit and a recompress. One texture,
  one bind, one draw for the whole menu, repacked only when the selection
  changes. A blueprint with no icon (UEB5208 is one) keeps its blank square and
  its neighbours keep their own, because the packing is positional.

  The original note on what the tray READS, kept because it is still true:
  BAR's tight three-column grid above the minimap, mass cost on the face of
  each button rather than behind a hover, unaffordable options dimmed rather
  than hidden, a tier band per cell, and a lit cell under the cursor
  (`core/ui/BuildPanel.hpp`, `app::gatherBuildOptions`). It does not yet ACT:
  no icons (no atlas yet — a cell reserves the square and shows the blueprint
  id), no placement ghost, and clicking a cell does nothing. The enqueue is
  deliberately not smuggled in here — it is a behaviour change (authorisation,
  the command log, the sim raising `ConstructionStarted`) and it wants the
  build path routed through `applyCommand` first, which is its own job.

  Two things the work found, both recorded rather than quietly fixed. The
  options come from the **roster**, not from `BuildTree` over
  `scene.definitions` — the latter holds only the types REGISTERED so far, so
  a commander's menu came back with the single extractor the opening had just
  asked for, and everything a player has not built yet is exactly what a menu
  is for. And screen input was arriving in points against a layout in pixels,
  which is ADR-040 and which had been mis-aiming the minimap's own click since
  P7.4.

  **Getting an engineer in front of the panel took a fix, and the fix was not
  where it looked.** `data/opening.lua` builds four structures and then tanks,
  so a scripted match never produces an engineer — but `--units` did spawn one
  and it was invisible to the interface for a different reason: `resolveUnits`
  runs before `spawnCommanders` (a skirmish APPENDS to a `--units` crowd rather
  than replacing it), so those units carry `kNoArmy`, and `pickAcrossBatches`
  refuses anything that is not the player's. A `--units` crowd in a skirmish was
  therefore furniture — unselectable, unorderable, and unable to show any
  interface at all, which is most of what a crowd of test units is for.
  `adoptOwnerlessUnits` hands them to the seated player once there is one.
  `make shot-engineer` is the capture: **15 options for UEL0105**.
- **UI-4**: an explicit orders row, deferred while right-click covers it.

### Track 1 — the host arc, milestones 21–25

- **21. The VM.** A current Lua — 5.4, or the 5.1 family if the determinism
  work of ADR-030 prefers it — plus a lexer patch for `#` comments and a 5.0
  compat library (ADR-029). *Done when* all 1,414 corpus files parse and
  `lua/system/*.lua` executes against auto-generated logging stubs for every
  missing engine function. The stub log IS the ranked to-do list.
- **22. Scheduler and events.** `ForkThread`/`WaitTicks` as coroutines keyed
  on the 10 Hz tick — the spine everything sits on, only 3 corpus files touch
  `coroutine.` directly — and decision 3's event set dispatching to `On*`
  methods on script objects.
- **23. One unit, hosted.** UEL0201's real `Unit.lua` lifecycle end to end:
  `OnCreate` → `OnStopBeingBuilt` → native motion under `SetGoal` →
  `OnKilled`, **diffed tick by tick against the native-only run**. The native
  sim is not scaffolding to discard; it is the oracle no other engine
  reimplementation has had.
- **24. Weapons go Lua.** `defaultweapons.lua`'s firing state machine replaces
  native firing for that unit; its projectile scripts host too. The fidelity
  stress test — the densest cluster of the contract.
- **25. The milestone-20 skirmish, fully hosted**, coverage table published in
  the README. The structural work owed along the way is not function-shaped,
  so no `attempt to call a nil value` will point at it — it is named here
  instead: the object bridge (one script table bound to one native entity,
  designed once and reused by every binding), the categories algebra
  (`categories.TANK * categories.TECH1 - categories.UEF`, userdata with
  operators, 102+ sites), and `GetBlueprint` returning one shared table. The
  moho shim itself is Lua on the engine's own API (ADR-028).

After 25 breadth is mechanical, and the first payoff is demonstrable: a
blueprint mod layered through the VFS changes the game with zero engine
changes. That is the moment this is an open-source Moho rather than an engine
that reads Supreme Commander's files.

### Track 2 — determinism, lockstep, FAF (ADR-030)

Large, and parallelizable once the sim API stops moving: strict-FP discipline
with deterministic transcendentals (streflop's lesson — IEEE `+ − × ÷` are
already bit-exact everywhere, `sin` is not), a deterministic VM configuration
for sim state, and a per-tick state hash with an immediate desync report,
which is what turns FAF's mystery desyncs into diagnosable bugs. FAF
integration proper is the small end of the track: the lobby server, client,
and ICE adapter are open source, and the surface a new engine must speak is
documented by their own repos.

### Track 3 — Linux (ADR-032)

Medium and mostly mechanical when it starts: an RHI seam (WebGPU or SDL3 GPU
class), shaders moved to a single-source language and transpiled. Until then
the only obligation is confinement — platform and GPU code stays inside
`src/platform` and `src/render`, which is already the layout.

### Track 4 — Beyond All Reason content (ADR-031)

Mostly done and deliberately passive: maps, models, blueprints, and the
per-family tick semantics stay green; the Spring Lua API is refused. One
substrate, per-game sim personality.

### UI and AI, parked honestly

140k of the corpus's 228k lines are UI and AI. Neither is hosted, possibly
ever: the HUD is ours, the opponent is ours, and both can grow on the
engine's own API. The mod-compatibility promise is about the sim.
