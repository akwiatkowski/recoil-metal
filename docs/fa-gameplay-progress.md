# Forged Alliance gameplay progress

This is the single operating dashboard for answering two questions:

1. How close is Recoil Metal to broad retail Forged Alliance gameplay?
2. What exact subsystem task should run next?

Detailed retail evidence remains canonical in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md). This dashboard summarizes that ledger; it
does not replace its claims, addresses, counterevidence, or confirmation gate.

**Snapshot:** 2026-09-04, Recoil Metal main through HUD slice 0 — the measured interface
baseline (`docs/hud-baseline.md`, `tools/hud_baseline.sh`, `--backing`, `--bench-hud`,
`--bench-size`) — after the `WP-22` controller read and six follow-ups (parked flyers recharge, the look-ahead max pyramid, attached children take their
carrier's tilt, mesh extents and build-effect bones parsed with a bone-to-world helper, the
factory production panel in the deck's command rectangle, README counts at 1357) —
`ComputeAirControl`, `CalcWingedLift`, the damping factor and the terrain look-ahead read and
implemented (`C-244`–`C-246`): authored `KMove`/`KLift` gains with their damping terms, the
vertical lift-off to half elevation that replaces the invented runway roll, the pyramid look-ahead
feeding the altitude reference; FA-AIR 55/40/95, headline unchanged at 55/25/70. Prior, same day:
the winged-mover foundation `0a555cb` (`C-221`–`C-223`, SaveState v11). Prior: the `WP-29`
redirect follow-up —
a non-strategic enemy MISSILE inside a redirector radius turns back on its live launcher, one
redirect per rate cycle with per-tick cooldown, SaveState v10 (`C-088` MissileRedirect, URL0303
`Defense.AntiMissile{Radius=5, RedirectRateOfFire=1}`); prior: the `C-183` guard-attack slice —
`AI.GuardScanRadius` parses and Assist-capable guards acquire through the ordinary path over
range-overridden weapon copies, pursuing outside weapon reach with Assist preserved (leash
endpoints unread, combat-unit guard orders deferred); the critical path reroutes to `FA-CMD`
because its remaining ladder branches are fully analyzed while `FA-MISSILES` leftovers need
fresh EXE reads. Prior: the `C-243` capture-increment read and the `WP-29` flare follow-up —
hostile category-matching shots inside a flare radius re-aim at the flare owner undamaged
(`C-088` (c), UAB4201 `Flare{Category='MISSILE', Radius=15}`); prior: the restriction follow-up
(TMD/SMD allow-split), the interception follow-up (`Defense.MaxHealth` pools, damage-not-destroy,
the `C-242` zero-ammo gate on both fire branches), the `WP-29` tactical silo ammunition slice
(`SiloAmmo` per unit/slot, `C-241` auto-refill, `C-083`/`C-084` whole-tick funding, `C-085`
launch-then-guarded-consume, SaveState v9) plus the `C-239`/`C-240` capture timing and
transfer-inventory reads, the `C-211` own-queue-before-guardee slice, and the `C-236`–`C-238`
capture/ownership-transfer contract, and prior: the `C-091` automatic-incumbent slice plus
group-move fan-out, 3-D generic attachments, silo weapon metadata, SaveState v8 command
round-tripping, the `C-210` Supremacy category-predicate foundation, generation-safe
retained-radar-contact reaping, and the `C-183`/`C-211` guarded-unit factory-mirroring slice,
retail artifact `ART-E001`
(`c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0`).

## Headline

Equal-weight average across the 20 gameplay subsystem rows below; `FA-FOUND` is excluded:

```text
Implemented       [###########---------] about 55%
Retail-validated  [#####---------------] about 25%
Retail-analyzed   [##############------] about 70%
```

Only **1 of 45 work packages**, `WP-15` economy, currently passes the complete retail
confirmation gate. The three percentages must never be combined: understanding absent behavior
does not make the game more complete.

The independent evidence-state count is **27 of 45 WPs at `Analyzed`**. That 60% inventory count
and the 70% equal-subsystem estimate answer different questions and are shown together to keep the
headline honest.

**Current implementation critical path:**
[`FA-CMD`](#fa-cmd---commands-controls-and-factories), the remaining `C-183` guard-ladder
branches (build-assist chain, reclaim-copy, repair scan) plus the leash-endpoint read in
`WP-12`. **Current EXE-analysis action:** the capture increment semantic at `Unit+0x690`
(`C-239`/`C-243`) plus `Sim::TransferUnit`'s native copy/reset inventory, and naming the third
`HasSiloAmmo` caller at `0x005DEAD0`.

## Reading The Scores

Scores are engineering estimates rounded to 5%, normally uncertain by about +/- 10%. They are
navigation aids, not measured code coverage.

| Axis | What earns progress |
|---|---|
| **Implemented** | Tested, end-to-end Recoil Metal behavior that a player or gameplay system can use. Parsers, stubs, and plans alone do not count. |
| **Retail-validated** | The implemented behavior was compared with direct retail EXE, Lua, blueprint, or controlled-observation evidence and structurally matches, or its intentional divergence passed the documented gate. A known unfixed mismatch does not count. |
| **Retail-analyzed** | Retail behavior is specified tightly enough to implement: decisive paths or shipped Lua/data are understood, ordering and state ownership are known, and material alternatives are excluded. |

Useful anchors for all three axes: 0% absent/unexamined, 25% one narrow slice, 50% common paths
with major gaps, 75% broad ordinary behavior, 100% the declared subsystem scope is complete.
Subsystems differ in size, so the table is more important than the headline average.

## Subsystem Dashboard

Stable IDs are task handles. Do not rename an ID when its score or next task changes. Every work
package appears in exactly one row, so the dashboard has an explicit denominator.
`FA-FOUND` reports research-deliverable completeness rather than gameplay implementation and is
excluded from the headline.

| ID | Subsystem | Source WPs | Implemented | Retail-validated | Retail-analyzed | Exact next task |
|---|---|---|---:|---:|---:|---|
| [`FA-FOUND`](#fa-found---retail-build-and-api-foundation) | Retail build and API foundation | `WP-00`-`02` | n/a | 75% | 85% | Bind an authoritative Steam depot/build manifest to `ART-E001`. |
| [`FA-SIM`](#fa-sim---simulation-kernel-and-object-lifecycle) | Simulation kernel and object lifecycle | `WP-03`-`04` | 65% | 40% | 90% | Add the safe script-object lifecycle seam needed by the Lua host. |
| [`FA-CONTENT`](#fa-content---vfs-blueprints-maps-and-bootstrap) | VFS, blueprints, maps, bootstrap | `WP-05`, `06`, `09` | 65% | 35% | 55% | Trace and test exact retail SCD mount/override precedence. |
| [`FA-LUA`](#fa-lua---gameplay-lua-and-mod-contract) | Gameplay Lua and mod contract | `WP-07`-`08` | 10% | 5% | 30% | Measure the exact Moho contract for the milestone-20 skirmish slice. |
| [`FA-MATCH`](#fa-match---armies-setup-and-victory-rules) | Armies, setup, victory rules | `WP-10`-`11` | 60% | 25% | 85% | Recover the retail lobby/scenario victory-mode selector; do not wire a synthetic app setting. |
| [`FA-CMD`](#fa-cmd---commands-controls-and-factories) | Commands, controls, factories | `WP-12`-`14` | 75% | 55% | 75% | Add the remaining guard-ladder branches (build-assist chain, reclaim-copy, repair scan) and read the leash endpoints. |
| [`FA-ECON`](#fa-econ---economy-construction-and-engineering) | Economy, construction, engineering | `WP-15`-`19` | 70% | 55% | 90% | Name the capture increment at `Unit+0x690` and read `Sim::TransferUnit`'s copy/reset inventory, then specify the smallest capture slice. |
| [`FA-LAND`](#fa-land---land-navigation-formations-and-spatial-world) | Land navigation, formations, spatial world | `WP-20`, `21`, `26` | 85% | 30% | 95% | Add bounded formation rotation or category matching without changing path-service ordering. |
| [`FA-AIR`](#fa-air---aircraft-flight-combat-and-staging) | Aircraft flight, combat, staging | `WP-22` | 55% | 40% | 95% | Read `CalcWingedOrientation` for the `+0x9c` landing adjustment and `C-222`'s two-stage descent, then start `C-224`'s attack-run pair. |
| [`FA-NAVY`](#fa-navy---surface-and-submerged-warfare) | Surface and submerged warfare | `WP-23`-`24` | 35% | 10% | 75% | Implement one complete `SurfacingSub` dive/surface slice. |
| [`FA-TRANSPORT`](#fa-transport---attachments-cargo-and-ferries) | Attachments, cargo, ferries | `WP-25` | 35% | 5% | 95% | Add parent/self bone indices and authored rest-bone composition to generic attachments. |
| [`FA-WEAPONS`](#fa-weapons---targeting-weapons-and-projectiles) | Targeting, weapons, projectiles | `WP-27`-`28` | 97% | 72% | 90% | Implement `C-157` target exemption for engineer reclaim/capture, then add the remaining death and manual-fire paths. |
| [`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception) | Silos, missiles, interception | `WP-29` | 55% | 35% | 95% | Add interceptor guidance/lead and shooter caps, then the build queue and UI. |
| [`FA-DAMAGE`](#fa-damage---damage-death-and-shields) | Damage, death, shields | `WP-30`-`32` | 70% | 40% | 75% | Add the next collidable shield slice: area-shield admission and stacking. |
| [`FA-INTEL`](#fa-intel---vision-radar-sonar-and-counter-intel) | Vision, radar, sonar, counter-intel | `WP-33` | 75% | 45% | 90% | Apply radar-position error to automatic targeting without changing contact identity or ordering. |
| [`FA-PROGRESS`](#fa-progress---enhancements-veterancy-and-special-units) | Enhancements, veterancy, special units | `WP-34`-`36` | 35% | 20% | 35% | Complete the enhancement lifecycle specification around `CUnitScriptTask`. |
| [`FA-TERRAIN`](#fa-terrain---mutable-terrain-and-craters) | Mutable terrain and craters | `WP-37` | 0% | 0% | 25% | Trace one crater from damage through terrain, pathing, and rendering invalidation. |
| [`FA-AI`](#fa-ai---retail-ai-and-native-manager-boundary) | Retail AI and native manager boundary | `WP-38` | 35% | 5% | 30% | Implement the next observed manager methods: `GetCurrentEnemy` and `GetUnitBlueprint`. |
| [`FA-UI`](#fa-ui---player-interface-and-advanced-controls) | Player interface and advanced controls | `WP-39`-`40` | 65% | 5% | 15% | Start HUD slice 3 (`recoil-metal-3937`): fixed inspector, paged roster/build palette, and command rack. |
| [`FA-PRESENT`](#fa-present---animation-effects-and-audio) | Animation, effects, audio | `WP-41`-`42` | 55% | 5% | 25% | Complete one blueprint-audio-to-XSB-cue playback path. |
| [`FA-PERSIST`](#fa-persist---replay-hashing-and-saveresume) | Replay, hashing, save/resume | `WP-43`-`44` | 70% | 20% | 90% | Version and round-trip authoritative economy and army state in SaveState. |

## Starting Work

For a bounded one-turn task, use the stable ID:

```text
Work on FA-CMD's exact next task from docs/fa-gameplay-progress.md. Keep the scope to that task,
use its source WPs and claims as the retail contract, verify it, and refresh the dashboard row.
```

For sustained work, copy the subsystem's `/goal` prompt below. Code goals require a failing
focused test first, the full suite with `make test`, and `make verify` for simulation changes.
Analysis-only goals require exact artifact locators, counterevidence, and an updated claim or WP
row. Every goal ends by updating this dashboard's score, next task, snapshot revision/date, and
the detailed evidence ledger when retail knowledge changed.

## Goal Prompts

### FA-FOUND - Retail Build And API Foundation

**Largest gap:** the executable is hash-identified, but no authoritative Steam depot/build
manifest proves exactly which final retail build it represents.

```text
/goal Advance FA-FOUND by obtaining and recording the authoritative Steam app 9420 depot/build
manifest, matching it against ART-E001 and the preserved retail files, documenting any remaining
provenance uncertainty, and updating WP-00 plus docs/fa-gameplay-progress.md with the evidence.
```

### FA-SIM - Simulation Kernel And Object Lifecycle

**Largest gap:** core deterministic simulation exists, but there is no retail-shaped script object
whose native handle is resolved safely and invalidated during deferred destruction.

```text
/goal Advance FA-SIM by implementing the smallest safe script-object lifecycle seam: stable native
entity handles, fresh resolution, deferred-destruction invalidation, and destroyed-handle failure
tests. Preserve deterministic state, run make test and make verify, then update WP-03 and the
FA-SIM dashboard row with evidence.
```

### FA-CONTENT - VFS, Blueprints, Maps, And Bootstrap

**Largest gap:** content loads through a layered VFS, but exact retail SCD mount and override order
has not been traced or tested.

```text
/goal Advance FA-CONTENT by tracing retail SCD mount and override precedence from ART-E001 and the
owned archive corpus, recording exact locators and counterexamples, then adding a discriminating
VFS precedence test and fixing Recoil Metal only if it differs. Update WP-05 and the FA-CONTENT
dashboard row.
```

### FA-LUA - Gameplay Lua And Mod Contract

**Largest gap:** the FAF AI uses Lua 5.4, but retail unit/projectile gameplay scripts, scheduler,
script objects, and mod hooks are not hosted.

```text
/goal Advance FA-LUA by measuring and publishing the exact native Moho API contract required by
the milestone-20 skirmish slice: simInit.lua, Unit.lua, defaultweapons.lua, its eight units, and
their projectiles. Produce a reproducible ranked binding list, update WP-07, and refresh FA-LUA
without implementing speculative bindings.
```

### FA-MATCH - Armies, Setup, And Victory Rules

**Largest gap:** the deterministic match layer now exposes `VictoryMode` and implements the
`C-210` Supremacy qualifying-unit predicate, but the app runner has no selected mode to pass to
it. Both production constructors currently use the Assassination default; CLI has no victory
option, and map loading intentionally reads `_save.lua` rather than `_scenario.lua`. Allied
victory, winner stability, delayed cleanup, and unit caps remain incomplete.

```text
/goal Advance FA-MATCH by recovering the retail lobby or scenario contract that selects a victory
mode. Do not infer a `ScenarioInfo.Options` key from the empty FAF shim or add a CLI/UI setting.
Only once an exact field/value locator exists, pass it into Match and exercise Assassination and
Supremacy through the app-level path. Preserve the 3-second poll cadence and C-210's
STRUCTURE-or-ENGINEER-minus-WALL predicate, run make test and make verify, record remaining
allied-victory/stability/cleanup gaps in WP-11, and refresh FA-MATCH.
```

### FA-CMD - Commands, Controls, And Factories

**Largest gap:** the app-level live/replay cutover covers build, PostSpawn roll-off, queued,
stopped, replaced, born-unit, factory-repeat, and guarded-factory mirror commands. The
own-queue-before-guardee branch is implemented with its lifecycle closed in `C-211`, and the
`C-183` ATTACK branch is implemented for Assist-capable guards — `AI.GuardScanRadius` parses,
acquisition runs the ordinary path over range-overridden weapon copies with shared incumbency
and threaded recon, pursuit holds inside weapon reach and chases outside it, Assist stays head.
Still absent: the transitive build-assist chain walk, reclaim-copy, the repair scan, the leash
(endpoints unread), and combat-unit guard orders (Assist refusal for non-builders is pinned —
a Guard order is the deferred vehicle).

```text
/goal Advance FA-CMD by implementing the next C-183 guard-ladder branch for Assist-capable
guards: the build-assist transitive chain walk through Unit+0x4e0 with visited set and cycle
guard, then reclaim-copy (guardee state 28) and the repair scan (GuardScanRadius around the
guardee, layer mask 0x100, nearest to the guard). Read the leash endpoints first — the formula
is known, the anchor is not. Start with focused guard-order regressions, preserve the active
head and all ids/counters/serials, run make test and make verify, then update WP-12 and FA-CMD.
```

### FA-ECON - Economy, Construction, And Engineering

**Largest gap:** explicit repair now has a tested semantic, replay, economy, and UI slice against
`C-182`, and the capture contract is fully read: costs (`C-236`), callback order (`C-237`),
replacement-unit transfer (`C-238`), the reclaim-idiom timing with its funding gate and
multi-captor aggregation (`C-239`), and the native transfer inventory — transform copied,
transports rebuilt, old entity destroyed (`C-240`). What still blocks an implementation slice:
the progress-increment semantic at `Unit+0x690` and `Sim::TransferUnit`'s complete native
copy/reset field inventory — all named, none guessed.

```text
/goal Advance FA-ECON by reading the capture progress increment at Unit+0x690 (C-239's open edge,
used at 0x0060B7C9-0x0060B7D9) and the native copy/reset branches of Sim::TransferUnit
(0x0074DC40-0x0074E4D6, C-240) with exact ART-E001 locators and counterevidence. Update WP-17,
C-239/C-240, and FA-ECON, then define the smallest capture implementation slice the evidence
supports. Do not derive heterogeneous-captor duration before Unit+0x690 is named.
```

### FA-LAND - Land Navigation, Formations, And Spatial World

**Largest gap:** plain multi-unit ground `Move` now assigns canonical UnitId ranks to distinct local
targets, preserving the shared click anchor and one existing per-army FIFO request per unit. Its
unrotated radius-based line is deliberately only an intake foundation: retail Lua geometry,
category matching, and arrival re-forming remain absent.

```text
/goal Advance FA-LAND by porting one bounded formation geometry from ART-S007:lua/formations.lua
onto the existing deterministic group-move fan-out. Preserve canonical UnitId ordering, shared
click anchors, per-army FIFO path-service budget, and C-176/C-177 retry semantics. Start with a
focused group-order regression, leave HPA*, reservations, category matching, and arrival
re-forming explicitly deferred, run make test and make verify, then refresh FA-LAND.
```

### FA-AIR - Aircraft Flight, Combat, And Staging

**Current slice:** winged flyers run retail's disjoint `canFly` mover with its controller read
from the executable (`C-221`, `C-244`–`C-246`): explicit per-second velocity integrated
trapezoidally at the 0.1 step; `a = KMove × desired − damp × v` horizontally and `KLift × lift
− KLiftDamping × vy` vertically, gains as authored; the desired velocity toward the target at
`min(distance, cruise)`; `CalcWingedLift`'s two branches, so a slow aircraft lifts straight up
to half its `Physics.Elevation` before the horizontal gate lets it fly forward — the retail
lift-off, replacing the runway roll the first slice invented; the pyramid terrain look-ahead
feeding a slewing altitude reference and the squared horizontal hold-back; `Bottom/Up/Top/Down`
events with landing on arrival and touchdown on the surface (water included); the
`AutoLandTime` idle timer (`C-222`); `FuelUseTime` drain with no native consequence at zero
(`C-223`). SaveState v11 carries the air state; the hash gates it on `canFly`, so the golden
log is unchanged.

**Largest gap:** the landing elevation adjustment at `CUnitMotion+0x9c` and the two-stage
descent are modelled as "elevation off the deck, zero while landing"; the cargo mass ratio,
banking/orientation torque, `Hover`, the combat state machine (`C-224`), staging and carrier
docking (`C-225`) are absent.

```text
/goal Advance FA-AIR by reading CalcWingedOrientation (0x006c4230) for the elevation adjustment
at CUnitMotion+0x9c and C-222's two-stage descent (Elevation x 0.5 until within 0.5 elmos, then
0), replacing the landing model in Movement.cpp with the read one. Keep the golden log
byte-identical, run make test and make verify, then refresh FA-AIR.
```

### FA-NAVY - Surface And Submerged Warfare

**Largest gap:** surface units work generically, but the sim has no current/target water layers,
dive command, depth transition, water vision, or underwater targeting.

```text
/goal Advance FA-NAVY by implementing one end-to-end SurfacingSub dive/surface slice from C-200
through C-205: current and target layer, Dive command, sinusoidal depth transition, and atomic
final layer flip. Use a real blueprint and focused timing/targetability tests, run make test and
make verify, then update WP-24 and FA-NAVY.
```

### FA-TRANSPORT - Attachments, Cargo, And Ferries

**Current slice:** generic attachments capture a deterministic local X/Y/Z offset, suspend child
movement, propagate it parent-before-child after movement and collision, and persist it in
SaveState v7 while v1-v6 derive the local height from saved transforms. Attached children remain
in the collision grid, matching `C-196`. Parent death detaches surviving children and clears their
local offsets. This is deliberately not transport loading: authored bone indices/composition,
capacity, load/unload, storage, ferry, and carrier death are still absent.

```text
/goal Advance FA-TRANSPORT by adding parent/self bone indices and authored rest-bone composition
from C-195/C-196 to existing generic attachments without inventing transport capacity or load
commands. Preserve deterministic parent-before-child order, attached motion suspension,
collision-grid presence, and the existing post-movement/post-collision propagation points. Run make
test and make verify, record the deferred transport controller behavior in WP-25, and refresh
FA-TRANSPORT.
```

### FA-WEAPONS - Targeting, Weapons, And Projectiles

**Largest gap:** automatic acquisition cannot yet exempt enemy units that an allied engineer is
reclaiming or capturing, because those commands cannot yet carry a type-safe unit target.

**Current slice:** `C-158` now admits automatic acquisition of current radar contacts but gives an
unidentified target retail's sentinel priority row `9999`, so it competes by score until the
viewing alliance has seen that exact unit generation. Sonar-only contacts remain rejected.
`C-167` applies the 4x out-of-arc score penalty, while `C-156` rejects a weapon with no authored
`TargetPriorities`. `C-091` retains a generation-safe automatic incumbent per weapon while it
remains eligible, and replaces it only for a strictly better candidate; fixed-hull aiming consults
the same incumbent. Explicit Attack remains separate, including when its target has gone stale.
`C-157` now rejects automatic candidates outside the immutable playable rectangle before
restriction/range/scoring and clears an incumbent that leaves it; live matches use their full map
extent and explicit Attack remains unrestricted. `SetDoNotTarget` is represented, persisted,
hashed, and consulted during automatic acquisition; the engineer reclaim/capture target-exemption
rule remains unwired because those command paths do not yet expose their unit targets.

```text
/goal Advance WP-15 with type-safe unit-target Reclaim/Capture command support before implementing
`C-157`'s engineer `IsTargetExempt` reject. Feature-only reclaim handles cannot safely name a unit;
keep the completed playable-rectangle filter, explicit Attack, candidate caches, and
projectile-defense targeting out of scope. Run make test and make verify, then update WP-15 and
FA-WEAPONS.
```

### FA-MISSILES - Silos, Missiles, And Interception

**Largest gap:** interception accounting now runs, but strategic-nuke silos, the counter/fire
commands, flares, guidance, and the build queue are absent.

**Current slice:** `C-095` gives projectile-target weapons a separate nearest-hostile,
in-range 2-D acquisition path with `max(MaxRadius, MaxRadius * TrackingRadius)` reach; it neither
uses nor mutates unit-target priority/incumbency state. `C-086` runs its interceptors through the
ordinary swept projectile tick and selects the earliest contact deterministically. Fixed-hull
defence aims before firing. **WP-29's ammo path is implemented** (projectile VFS resolution,
`SiloAmmo` per unit/slot, `C-241` auto-refill, `C-083`/`C-084` whole-tick funding, `C-085`
launch-then-guarded-consume, generation-safe reaping, SaveState v9, hash). **Interception
follow-up implemented (`C-242`, `C-087`):** shots carry their authored `Defense.MaxHealth`
pool and interception damages instead of force-destroying — a 30-damage interceptor one-shots
a 25-HP nuke, a 1-damage one needs several passes at a tactical; pool-less shots still die to
any contact and the interceptor is always consumed. **The zero-ammo gate is closed:** a counted
weapon with an empty slot holds fire on both fire branches, and a counted weapon with no silo
record still fires. **Restriction follow-up implemented (`C-088`):** shots inherit their
authored projectile categories at launch, and point-defence acquisition admits only shots
passing the weapon's allow/disallow lists — TMD-pattern fires at tactical missiles only,
SMD-pattern at strategic ones, UNTARGETABLE refused, category-less ordinary shells never
triggering an allow list. **Flare follow-up implemented (`C-088` (c)):** an in-flight hostile
shot carrying the flare category that enters the radius re-aims at the flare owner at unchanged
speed — diverted, never damaged, no cooldown. **Redirect follow-up implemented (`C-088`
MissileRedirect):** `Defense.AntiMissile{Radius, RedirectRateOfFire}` parses and a
non-strategic enemy MISSILE inside the radius turns back on its live launcher, one redirect per
rate cycle, dead owners reaped, SaveState v10. **Named divergences:** no build queue, adjacency
modifier fixed at 1, `--units` crowds unwired; the third `HasSiloAmmo` caller (`0x005DEAD0`) is
unnamed; the returned missile cannot yet damage its source side (no friendly-fire channel);
the cooldown may be 10 or 11 ticks (`WaitSeconds` runs n·10+1).

```text
/goal Advance FA-MISSILES by implementing interceptor guidance/lead and shooter caps, with exact
ART-E001 locators and counterevidence. Write failing tests first (restriction, flare, and
redirect behavior stay green), run make test and make verify, record the build queue and
tactical/nuke UI as the next boundary in WP-29, and refresh FA-MISSILES.
```

### FA-DAMAGE - Damage, Death, And Shields

**Largest gap:** ordinary damage is close, but shields are damage-gate records rather than
collidable entities, which prevents retail direct-fire and personal-shield semantics.

```text
/goal Advance FA-DAMAGE by introducing the smallest collidable shield-entity path needed for a
direct projectile to hit a dome before its owner, then remove the corresponding direct-fire
damage-gate shortcut. Write projectile/shield ordering tests first, run make test and make verify,
update WP-30/31 plus C-143 residues, and refresh FA-DAMAGE.
```

### FA-INTEL - Vision, Radar, Sonar, And Counter-Intel

**Largest gap:** a retained dead radar contact now reaps on the next intel update when a different
live generation reuses its slot, and is suppressed from projection immediately; time-based expiry
and other recon families remain incomplete.

```text
/goal Advance FA-INTEL by applying existing deterministic radar-position error to automatic
targeting without changing retained-contact identity, ordering, or visual identification. Preserve
generation-safe slot-reuse reaping; keep temporal expiry, cloak, jamming, water vision, sonar
memory, and Lua bindings out of scope. Run make test and make verify, then refresh WP-33 and
FA-INTEL.
```

### FA-PROGRESS - Enhancements, Veterancy, And Special Units

**Largest gap:** veterancy is implemented, but the generic enhancement lifecycle and bespoke
experimental script/native interactions are largely absent.

```text
/goal Advance FA-PROGRESS by completing the enhancement lifecycle specification around
IssueScript 0x006FD240, CUnitScriptTask 0x00629380, EnhanceTask.lua, SetUpgradedTo, cancellation,
and completion callbacks. Separate native task machinery from Lua-owned rules with exact evidence,
update WP-34 and FA-PROGRESS, and define the first host-dependent implementation slice without
faking it in C++.
```

### FA-TERRAIN - Mutable Terrain And Craters

**Largest gap:** mutable map entry points are anchored, but the crater path and all implementation
behavior are absent.

```text
/goal Advance FA-TERRAIN by tracing one retail crater/deformation path from damage through
FlattenMapRect or SetTerrainTypeRect, heightfield mutation, path invalidation, and render
invalidation. Record exact ART-E001 callers/downstream effects and counterevidence, update WP-37
and FA-TERRAIN, and create only the implementation task the trace supports.
```

### FA-AI - Retail AI And Native Manager Boundary

**Largest gap:** the deterministic FAF Lua opening works through many bindings, but stand-in
native managers keep it far from the retail AI architecture. The 650-second sanity run is clean
at both two armies and all eight SCMP_009 seats: no instruction-budget exhaustion without raising
the 20-million-instruction watchdog. The two-army run also reports zero thread errors.
`make ai-sanity` now fails if an overrun returns. The next observed condition gaps are
`GetCurrentEnemy` (90 calls in the
eight-army run) and `GetUnitBlueprint` (7).

```text
/goal Advance FA-AI by implementing the `GetCurrentEnemy` and `GetUnitBlueprint` behavior exposed
by the eight-army 650-second sanity run. Keep conditions fail-closed, add focused regressions,
rerun the AI report and long sanity match, update WP-38 and FA-AI, and record the next missing
native-manager behavior exposed.
```

### FA-UI - Player Interface And Advanced Controls

**Current slice:** the native HUD has a measured baseline (HUD slice 0, `docs/hud-baseline.md`,
`tools/hud_baseline.sh`): seven interface states — default, commander, mixed selection,
engineer, placement, observer, FAF chrome — at 1280x720 and 1600x900 at 1x and at the 14-inch
MacBook Pro and 5K logical sizes at 2x, each a deterministic headless capture with per-layer
HUD vertex counts and an offscreen GPU frame time with and without the interface. Two tracked
flags made the 2x points possible: `--backing` gives a capture or benchmark a display scale, and
`--bench-hud` / `--bench-size` let the benchmark draw the interface a capture shows. The factory
production panel now fills the deck's command rectangle. `--select-type <blueprint id>` selects
the first matching unit by deterministic draw order, so an injected UEL0105 can now be captured
inside the five-minute skirmish with its army, economy, textures, and 15-option build palette.
Both offscreen benchmark modes now run that same composition; world-only clears only the 2D HUD
and minimap after composition, keeping particles, construction effects, world overlays, and
strategic-icon fallback decisions identical.

**Largest gap:** the interface is functionally usable but carries none of retail's data-driven
command pages, overlays, key contexts, or split views (`WP-40`).

```text
/goal Advance FA-UI with HUD slice 3 (recoil-metal-3937): replace the floating selection card
and vertical command tray with the fixed inspector, paged stable roster/build palette, and the
existing-command rack in the deck. Keep every capture headless and deterministic, run make test,
and refresh FA-UI.
```

### FA-PRESENT - Animation, Effects, And Audio

**Largest gap:** generic rendering and mixing exist, but script-driven manipulators/effects and
blueprint/XSB-authored sound behavior do not run end to end.

```text
/goal Advance FA-PRESENT by implementing one ordinary weapon's complete blueprint Audio field to
XSB cue resolution to positional mixer playback path using owned retail data. Write parser and cue
selection tests, verify the sound in a real match, run make test, update WP-42 and FA-PRESENT, and
leave dynamic music and broad effect hosting as explicit later slices.
```

### FA-PERSIST - Replay, Hashing, And Save/Resume

**Largest gap:** SaveState v8 now round-trips shared queued commands, their mutable per-unit
execution state, live command lookup, and allocators while v1-v7 remain decodable. Economy,
armies, and path-service runtime state are still absent from resumed matches. The savegame
envelope may gain independent versions; replay playback remains bound to the existing command
format and must continue importing old replay streams.

```text
/goal Advance FA-PERSIST by encoding authoritative economy and army state in the versioned
save/resume envelope, preserving the existing UnitStore, queued-command, tick, and RNG state.
Add focused round-trip and continued-hash tests; run make test and make verify, update WP-44 and
FA-PERSIST, and leave path-service runtime searches and replay transport as later authoritative
groups.
```

## Maintenance Contract

When work changes a subsystem:

1. Update the canonical WP row and claims in `fa-exe-analysis-plan.md` if retail evidence changed.
2. Update this row's three estimates, largest gap, exact next task, and goal prompt.
3. Recompute the gameplay headline as an equal-weight average excluding `FA-FOUND`, rounded to 5%.
4. Update the snapshot date/revision and state `dirty worktree` when applicable.
5. Keep every `WP-00` through `WP-44` in exactly one subsystem row.
6. Recheck the confirmation count, `Analyzed` WP count, implementation critical path, EXE-analysis
   action, and matching summary at the top of `fa-exe-analysis-plan.md`.
