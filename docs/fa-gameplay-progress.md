# Forged Alliance gameplay progress

This is the single operating dashboard for answering two questions:

1. How close is Recoil Metal to broad retail Forged Alliance gameplay?
2. What exact subsystem task should run next?

Detailed retail evidence remains canonical in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md). This dashboard summarizes that ledger; it
does not replace its claims, addresses, counterevidence, or confirmation gate.

**Snapshot:** 2026-09-06, main through `bf0ef56`. This day lands the skirmish
economy correction, engineer construction tiers with per-tier upgrade cancellation
(ADR-095), weapon visuals resolved from the original Lua declarations with authored
emitters, muzzle/impact effects, live beam endpoints, ribbon trails and original
projectile meshes with mesh-blueprint LOD tables (ADR-096 to ADR-099, ADR-101,
ADR-103), the FAF watchdog cascade fix, a native reclaim-grid snapshot and a
dispatched reclaim decision (ADR-100, ADR-102), the script-object lifecycle seam
(ADR-104), unit reclaim as a typed work target with the `C-157` targeting exemption
(ADR-105, SaveState v17, command log v4), a per-process VFS test scratch directory,
and the last two native input cases, completing the 24-case matrix. Strict
retail-content acceptance passes 1,635 cases with no skips. The 1,800-second retail
SCMP_009 duel runs with zero instruction-budget failures and no missing brain
methods; with reclaim decisions active it no longer ends inside 1,800 seconds
([skirmish acceptance](skirmish-economy-acceptance.md)). `make verify` still
diverges at tick 0 against the golden recorded before the economy correction;
reblessing it is a pending decision that also gates the FA-AIR and FA-PERSIST
golden halves. Implementation does not establish whole-WP parity.

**Prior snapshot:** 2026-09-05, main through `76e781b`, plus the native inactive-window
acceptance fix and evidence reconciliation. The selected
batch adds combat Guard, individual factory cancellation, bounded air combat
states 3-7, economy/army save continuation, observed FAF query bindings and wreck
blast damage. Native input and bounded simulation tests are documented below.
The required retail projectile archive and retail-map/golden gates remain blocked
by the disconnected install. Existing subsystem estimates are retained until
that acceptance can be completed; implementation does not establish whole-WP parity.

**Earlier snapshot:** 2026-09-04, Recoil Metal main through HUD slice 7 — the measured interface
baseline (`docs/hud-baseline.md`, `tools/hud_baseline.sh`, `--backing`, `--bench-hud`,
`--bench-size`) — after the `WP-22` controller read and six follow-ups (parked flyers recharge, the look-ahead max pyramid, attached children take their
carrier's tilt, mesh extents and build-effect bones parsed with a bone-to-world helper, the
factory production panel in the deck's command rectangle, UEF construction beams from authored
builder bones to descending build-cube edges, structured runtime logging, a bounded fixed
inspector with paged selection/build instruments and a dispatching 4x3 command rack,
resource/game-profile descriptors plus one aspect-preserving minimap projection for preview, fog,
pips, input, and true-line camera footprint, explicit BAR and neutral vocabulary/material fallbacks,
the exact retail mobile-builder approach-to-range gate, per-instance builder torso/arm/tool aiming from authored
bone rigs, unit `_NormalsTS` maps with retail `.gaa` decoding and UV1, the final HUD
stress/accessibility and real-window performance pass, and README counts at 1391) —
`ComputeAirControl`, `CalcWingedLift`, the damping factor and the terrain look-ahead read and
implemented (`C-244`–`C-246`): authored `KMove`/`KLift` gains with their damping terms, the
vertical lift-off to half elevation that replaces the invented runway roll, the pyramid look-ahead
feeding the altitude reference, followed by the exact ordinary two-stage landing target and descent
clamps (`C-247`); FA-AIR 60/50/95, headline unchanged at 55/25/70. Prior, same day:
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
Implemented       [############--------] about 60%
Retail-validated  [######--------------] about 30%
Retail-analyzed   [##############------] about 70%
```

The row estimates average 59.05%, 29.25% and 68.75%, respectively. The 2026-09-06
refreshes raised `FA-CMD` and `FA-ECON` implementation by 5, `FA-PRESENT` by 15
(analyzed by 15), `FA-SIM` by 5, `FA-AI` by 5 and `FA-WEAPONS` by 1 (validated by 3)
for the weapon-visuals, ribbon-trail, projectile-mesh, upgrade-cancellation,
script-object-seam, reclaim-decision and unit-reclaim/C-157 slices.

Only **1 of 45 work packages**, `WP-15` economy, currently passes the complete retail
confirmation gate. The three percentages must never be combined: understanding absent behavior
does not make the game more complete.

The independent evidence-state count is **27 of 45 WPs at `Analyzed`**. That 60% inventory count
and the 70% equal-subsystem estimate answer different questions and are shown together to keep the
headline honest.

**Current implementation critical path:**
Finish the selected batch's retail-content, retail-map and golden acceptance when
the install is available. The remaining [`FA-CMD`](#fa-cmd---commands-controls-and-factories)
implementation gaps include the earlier refuel/staging rungs of `C-183`; explicit
combat Guard now reuses the tested assistance ladder without granting builder powers.
**Current EXE-analysis action:**
the capture increment semantic at `Unit+0x690`
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
| [`FA-SIM`](#fa-sim---simulation-kernel-and-object-lifecycle) | Simulation kernel and object lifecycle | `WP-03`-`04` | 70% | 45% | 90% | Give the FAF unit proxies live position and health reads through the seam; then WP-04's RNG/checksum comparison. |
| [`FA-CONTENT`](#fa-content---vfs-blueprints-maps-and-bootstrap) | VFS, blueprints, maps, bootstrap | `WP-05`, `06`, `09` | 65% | 35% | 55% | Trace and test exact retail SCD mount/override precedence. |
| [`FA-LUA`](#fa-lua---gameplay-lua-and-mod-contract) | Gameplay Lua and mod contract | `WP-07`-`08` | 10% | 5% | 30% | Measure the exact Moho contract for the milestone-20 skirmish slice. |
| [`FA-MATCH`](#fa-match---armies-setup-and-victory-rules) | Armies, setup, victory rules | `WP-10`-`11` | 60% | 25% | 85% | Recover the retail lobby/scenario victory-mode selector; do not wire a synthetic app setting. |
| [`FA-CMD`](#fa-cmd---commands-controls-and-factories) | Commands, controls, factories | `WP-12`-`14` | 85% | 70% | 75% | Specify the refuel/staging rung of `C-183`; Guard is accepted on the retail map (`make test-guard-ui`). |
| [`FA-ECON`](#fa-econ---economy-construction-and-engineering) | Economy, construction, engineering | `WP-15`-`19` | 75% | 55% | 90% | Name the capture increment at `Unit+0x690` and read `Sim::TransferUnit`'s copy/reset inventory, then specify the smallest capture slice. |
| [`FA-LAND`](#fa-land---land-navigation-formations-and-spatial-world) | Land navigation, formations, spatial world | `WP-20`, `21`, `26` | 85% | 30% | 95% | Add bounded formation rotation or category matching without changing path-service ordering. |
| [`FA-AIR`](#fa-air---aircraft-flight-combat-and-staging) | Aircraft flight, combat, staging | `WP-22` | 65% | 55% | 95% | Golden acceptance of planar states 3-7 once the baseline is reblessed (retail-content half passed 2026-09-06); full banking remains. |
| [`FA-NAVY`](#fa-navy---surface-and-submerged-warfare) | Surface and submerged warfare | `WP-23`-`24` | 35% | 10% | 75% | Implement one complete `SurfacingSub` dive/surface slice. |
| [`FA-TRANSPORT`](#fa-transport---attachments-cargo-and-ferries) | Attachments, cargo, ferries | `WP-25` | 35% | 5% | 95% | Add parent/self bone indices and authored rest-bone composition to generic attachments. |
| [`FA-WEAPONS`](#fa-weapons---targeting-weapons-and-projectiles) | Targeting, weapons, projectiles | `WP-27`-`28` | 98% | 75% | 90% | Add the remaining death and manual-fire paths; `C-157`'s capture half waits on Capture. |
| [`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception) | Silos, missiles, interception | `WP-29` | 55% | 35% | 95% | Add interceptor guidance/lead and shooter caps, then the build queue and UI. |
| [`FA-DAMAGE`](#fa-damage---damage-death-and-shields) | Damage, death, shields | `WP-30`-`32` | 78% | 50% | 80% | Specify PersonalBubble and transport coverage. |
| [`FA-INTEL`](#fa-intel---vision-radar-sonar-and-counter-intel) | Vision, radar, sonar, counter-intel | `WP-33` | 75% | 45% | 90% | Apply radar-position error to automatic targeting without changing contact identity or ordering. |
| [`FA-PROGRESS`](#fa-progress---enhancements-veterancy-and-special-units) | Enhancements, veterancy, special units | `WP-34`-`36` | 35% | 20% | 35% | Complete the enhancement lifecycle specification around `CUnitScriptTask`. |
| [`FA-TERRAIN`](#fa-terrain---mutable-terrain-and-craters) | Mutable terrain and craters | `WP-37` | 0% | 0% | 25% | Trace one crater from damage through terrain, pathing, and rendering invalidation. |
| [`FA-AI`](#fa-ai---retail-ai-and-native-manager-boundary) | Retail AI and native manager boundary | `WP-38` | 40% | 5% | 30% | Specify islandMarker from its callers; measure how reclaim decisions change the SCMP_009 duel's course. |
| [`FA-UI`](#fa-ui---player-interface-and-advanced-controls) | Player interface and advanced controls | `WP-39`-`40` | 75% | 5% | 15% | Trace and implement the first retail data-driven command page from `WP-40`. |
| [`FA-PRESENT`](#fa-present---animation-effects-and-audio) | Animation, effects, audio | `WP-41`-`42` | 75% | 5% | 45% | Honour `LodCutoff` distances and XACT pitch/volume ranges on weapon cues; script-driven manipulators remain. |
| [`FA-PERSIST`](#fa-persist---replay-hashing-and-saveresume) | Replay, hashing, save/resume | `WP-43`-`44` | 70% | 20% | 90% | Golden acceptance of save continuation (now v17) once the baseline is reblessed (retail-content half passed 2026-09-06); general app saves remain absent. |

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

The script-object lifecycle seam exists (ADR-104): `UnitStore::resolve` names a handle's
state as `Alive`, `Destroyed` (dead this tick, slot still readable, retail's `BeenDestroyed`)
or `Stale` (released by `retireDead`), computed fresh on every call; `ScriptObject.hpp` maps
retail's two resolver families onto it, raising "Game object has been destroyed" verbatim
for the strict one. `[script-handle]` tests cover all three states, slot reuse and hash
neutrality. Tick order, the tombstone store and the state hash are unchanged.

**Largest gap:** no script host uses the seam yet — the FAF driver still hands Lua per-pass
indices — and retail's purge delay is represented by the generation, not by a deferred kill.

```text
/goal Advance FA-SIM by routing the FAF driver's unit handles through the script-object seam:
replace per-pass index handles with UnitId-backed objects resolved through requireAlive and
lifecycleSlot, raise the retail error on stale use, and add a destroyed-handle test through
the driver. Preserve deterministic state, run make test and make verify, then update WP-03
and the FA-SIM dashboard row with evidence.
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
Build assistance now follows active Assist targets transitively to the terminal builder, with a
visited set so cyclic guard chains contribute no work; reach is still measured to that resolved
builder, preserving the existing range rule. The remaining selected ladder branches now run too:
the recovered 3D leash returns before attack, reclaim copies the guardee's active feature target,
and repair scans around the guardee while ranking candidates by distance to the guard.
The native `CUnitScriptTask` boundary is also implemented: script issues are authorized and logged,
the command stage runs retail task-status timing, lifecycle cleanup is queue-owned, and opaque host
state survives SaveState v12 and hashing without coupling Lua to the sim core.
Explicit combat-unit Guard now reuses that ladder while checking authored build,
repair and reclaim eligibility. Stable-ID cancellation removes one active or
pending factory row without consuming unrelated work, and now also a pending or
active structure upgrade together with its dependent later tiers, through the same
production panel; engineers and factories share the authored `BuildableCategory`
menu by tier (ADR-095). Guard's mirrored construction
identity is separate from its retained own-build command. Command-log v3 retains
v2 readers. Native acceptance exercises Guard targeting, Stop, paginated queue
cancellation and Clear Queue; see [native input acceptance](native-input-acceptance.md).
Guard is accepted on the retail map: `make test-guard-ui` replays
`tests/fixtures/hud-guard.commands` on SCMP_009 twice — a mortar guards a tank that walks
250 elmos across the terrain — and requires matching pixels and hashes with the guard
order still standing and the guard away from the factory (`hud-order:` line). Strict
`make test-content` runs every `[corpus][guard]` case with no skips.
Still absent: the earlier refuel/staging and ferry rungs and exact multi-weapon guard arbitration.

```text
/goal Specify the refuel/staging rung of C-183 from its retail callers and implement the
smallest bounded slice that lets a guarded air unit refuel at a staging platform. Preserve
authored capabilities and retained construction identity, and run make test and make verify
before updating FA-CMD.
```

### FA-ECON - Economy, Construction, And Engineering

**Largest gap:** explicit repair now has a tested semantic, replay, economy, and UI slice against
`C-182`, and the capture contract is fully read: costs (`C-236`), callback order (`C-237`),
replacement-unit transfer (`C-238`), the reclaim-idiom timing with its funding gate and
multi-captor aggregation (`C-239`), and the native transfer inventory — transform copied,
transports rebuilt, old entity destroyed (`C-240`). What still blocks an implementation slice:
the progress-increment semantic at `Unit+0x690` and `Sim::TransferUnit`'s complete native
copy/reset field inventory — all named, none guessed. Ordinary mobile construction now follows
`C-248`: the active Build order routes the engineer toward the site and creates no construction
until centre distance minus the builder's smaller footprint and target's larger skirt is within
`Economy.MaxBuildDistance`; factory production and upgrades retain their separate immediate paths.
T1/T2/T3 engineers now offer their authored construction tiers, including shields and
experimentals, while unenhanced ACUs keep the T1 menu until enhancements are modelled;
extractor upgrade chains can be cancelled per tier (`[engineer-tiers]`, `make test-upgrade-ui`).

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
events with landing on arrival; the landing target stays at half the adjusted authored elevation
until the last 0.5 elmo, then switches to the surface, with retail's separate ordinary and
`TRANSPORTATION` descent clamps (`C-222`, `C-247`); the
`AutoLandTime` idle timer (`C-222`); `FuelUseTime` drain with no native consequence at zero
(`C-223`). Winged entity attacks now implement `C-224` states 1 and 2: the head-on run flies
through weapon range at maximum airspeed and is the only state using `AttackElevation`; the
tail chase starts when target direction and both forward vectors agree inside the recovered
30-degree cone, and floors desired speed at `MinAirspeed`. Both states expose the
`MakingAttackRun` lifecycle. The planar reduction now also runs sustained-turn
states 3/4/5, breakoff 6 and off-map recovery 7. Authored timers and match-owned
MT19937 draws choose transitions; new entity attacks reset tactical counters.
SaveState v16 and hashing preserve controller state, cached tuning and the random
sequence. Real UEA0102 turn/recovery checkpoints continue with matching per-tick
hashes; the inspected app pursuit replay matches 900 ticks. See ADR-091 for the
planar controller boundary and [unit evidence](unit-capability-matrix.md).

**Largest gap:** full three-axis banking/orientation torque remains outside the planar model.
Bomb-drop prediction for state 1, the cargo mass ratio, `Hover`,
`POD`-only random initialization of the otherwise-zero `CUnitMotion+0x9c` elevation adjustment,
staging and carrier docking (`C-225`) also remain.

```text
/goal Finish the current air slice's retail-map and golden acceptance. Attribute the first
hash divergence before considering a new baseline; v16 now hashes the future RNG sequence.
Keep full banking, cargo inertia and bomb prediction explicitly separate from the implemented
planar states, then choose the next evidenced controller gap and refresh FA-AIR.
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

**Largest gap:** ordinary bubbles and personal UnitShield boxes now share the collidable shield
path. `CollisionCenter*` and `CollisionSize*` drive box containment and projectile sweeps, while a
conservative bound is used only for spatial admission. The remaining C-143 residue is the owner's
armour multiplier; PersonalBubble and transport coverage still need their separate semantics.

```text
/goal Advance FA-DAMAGE by applying the shield owner's armour multiplier from C-143 instead of
the synthetic Shield armour class. Write mixed-owner and damage-type regressions first, preserve
the established sphere/box admission and one-charge rules, run make test and make verify, then
update WP-30/31 plus the remaining PersonalBubble and transport-shield boundary.
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
experimental script/native interactions are largely absent. The serializable native task host is
now available; the missing work is the Lua adapter and EnhanceTask's actual gameplay contract.

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
`make ai-sanity` now fails if an overrun returns. The previously observed
`GetCurrentEnemy` and `GetUnitBlueprint` gaps are implemented and exercised by
their FAF consumers. Current-enemy lookup returns a stable brain proxy;
blueprint lookup returns cached published tables. The 2026-09-05 local BAR-map
runs completed 6,500 ticks with two/eight armies, built 16/64 units, and reported
zero module failures, thread errors or instruction-budget overruns. They fired
no shots and do not replace the retail-map combat run. Remaining observed fields
are GridReclaim, Nickname and islandMarker; no dummy bindings were added.
The 2026-09-06 retail SCMP_009 duel (1,800 s, two FAF armies) is now free of
instruction-budget failures: the watchdog refills before raising with a bounded
cap, first condition-cache expiries are staggered across passes, and unit counts
are memoised per pass by category text (ADR-100). The earlier 13 decision
failures and 38 condition errors were one genuine overrun per pass plus a
watchdog cascade. Team 0 still wins. `GridReclaim` is now answered from a native
per-cell reclaim snapshot (ADR-102): only `ReclaimAvailableInGrid` reads it in this
slice. The reclaim builders now act: the driver sends an idle engineer to the
richest cell near the base, capped by InstanceCount, and the match resolves the
cell to its most valuable wreck (`2d0b749`, `bf0ef56`). With that the same duel
issues about two hundred reclaim orders and no longer ends inside 1,800 seconds;
the change of course is recorded, not judged. The 650-second two- and eight-army
retail sanity runs on 2026-09-06 reported no instruction-budget overruns, zero
thread errors and 107 modules executed. See
[skirmish acceptance](skirmish-economy-acceptance.md).

```text
/goal Rerun the two/eight-army 650-second sanity matches with full retail content. Specify
GridReclaim, Nickname and islandMarker from their actual callers and state ownership before
implementing them. Keep missing conditions fail-closed and update WP-38 and FA-AI with measured
construction/combat progression and remaining native-manager gaps.
```

### FA-UI - Player Interface And Advanced Controls

**Current slice:** HUD slice 3 replaces the lower floating/variable instruments with one bounded
deck. The selection bay has a fixed inspector whose priority is armed mode, hovered build or
command, hovered roster type, then selection summary. Roster slots and the two-row build palette
page without moving or shrinking, their page state belongs to selection and builder type, and
only visible-page art enters the fixed icon atlas. The bottom-right 4x3 rack keeps existing FA
command positions stable, visibly disables unsupported capabilities, swallows its whole rectangle,
and dispatches Stop, movement, attack, patrol, assist, repair, reclaim, and overcharge through the
same semantic command path as keyboard/context orders.

HUD slice 4 makes the four concrete game profiles the authority for resource vocabulary and
ownership: FA/classic say Mass and take the player's faction livery, BAR says Metal and uses a
deterministic warm field-acrylic fallback, and neutral says Material with no faction ownership.
The minimap exposes one content rectangle and scale; preview, fog, pips, click/drag input, and the
camera footprint use it. Rectangular maps letterbox, clicks in the bars miss, and footprint edges
are rotated quads rather than axis-aligned bounding slivers. X1CA_003 (2048x1024) was captured both
whole-map and close-camera to verify the 2:1 content rectangle and true-line footprint.

HUD slice 5 splits world and interface encoding only when a translucent backdrop is requested.
Full and Reduced render the complete post-water world into one full-resolution shader-readable
target, downsample it to one quarter-resolution pair, encode one MPS Gaussian blur, restore the
sharp world, and let only panel surfaces sample the blur before later chrome and text. Off keeps
the former direct render path and allocates or encodes none of those frame resources. macOS
Reduced Transparency resolves automatic effects to Off; `--ui-effects` supplies the explicit
Full/Reduced/Off override used by captures and benchmarks. At 2560x1440 pixels Full measured
4.844 ms GPU mean against Off's 4.832 ms; at 5120x2880 it measured 9.911 ms against 9.521 ms.

HUD slice 6 keeps the frame and semantic palette fixed while giving each surface a concrete
absorption, saturation, and tint response: UEF laminated tactical glass, Aeon opalescent ceramic,
Cybran smoked composite, Seraphim crystalline plate, BAR field acrylic, and restrained Neutral
observer chrome. The complete retail `generic_brd` nine-slice remains the classic FAF material;
missing or partial art falls back as one unit to Neutral rather than combining faction glass with
stray classic pieces. Black panel-shadow quads retain ordinary alpha and no longer trigger or
sample the shared blur. Seven 1280x720 contact captures verify identical module silhouettes,
typography, information colours, and hit geometry across the presentations.

HUD slice 7 closes the native-HUD acceptance pass. Stress cases cover 501 build options, 501
selection types, oversized production labels, missing icons, page invalidation, dead and mixed
builders, fixed command/resource capacity, safe areas, 1x/2x display changes, rectangular maps,
and every presentation/effects level. Long production names now truncate visibly inside their
fixed panel instead of crossing the repeat/count columns. Full, Reduced, and Off screenshot smoke
tests preserve semantic layer order. Real windows sustain 60 Hz at 5088x2862 pixels: Full measured
6.694 ms GPU mean and Off 6.792 ms, a noise-level difference. The complete 1391-test suite passes
and the 7000-tick golden replay remains identical.

The first `WP-40` data-driven command page now follows retail's selection boundary:
`lua/ui/game/orders.lua:SetAvailableOrders` receives `GetUnitCommandData(newSelection)`, whose
per-unit authority is `General.CommandCaps`. The loader retains only authored `true` caps while
remembering that the table existed, so BAR and synthetic content keep capability inference but an
FA unit's explicit `false` wins. The Cybran Mantis is the end-to-end discriminator: its auxiliary
BuildRate arm exposes Assist and Repair while its blueprint-forbidden Reclaim cell stays disabled.
Mixed selections retain the rack's existing any-capable-unit semantics and no slot geometry moves.

The native HUD also has a measured baseline (HUD slice 0, `docs/hud-baseline.md`,
`tools/hud_baseline.sh`): seven interface states — default, commander, mixed selection,
engineer, placement, observer, FAF chrome — at 1280x720 and 1600x900 at 1x and at the 14-inch
MacBook Pro and 5K logical sizes at 2x, each a deterministic headless capture with per-layer
HUD vertex counts and an offscreen GPU frame time with and without the interface. Two tracked
flags made the 2x points possible: `--backing` gives a capture or benchmark a display scale, and
`--bench-hud` / `--bench-size` let the benchmark draw the interface a capture shows. `--select-type <blueprint id>` selects
the first matching unit by deterministic draw order, so an injected UEL0105 can now be captured
inside the five-minute skirmish with its army, economy, textures, and 15-option build palette.
Both offscreen benchmark modes now run that same composition; world-only clears only the 2D HUD
and minimap after composition, keeping particles, construction effects, world overlays, and
strategic-icon fallback decisions identical.

**Largest gap:** the first retail command page is data-driven, but toggle caps/order overrides,
idle selectors, overlays, key contexts, and split views remain absent (`WP-40`).

```text
/goal Advance FA-UI with the next WP-40 retail boundary: ToggleCaps plus OrderOverrides, keeping
unsupported toggle actions visibly disabled until their simulation state exists. Add focused tests,
run the full suite and golden replay, and update FA-UI.
```

### FA-PRESENT - Animation, Effects, And Audio

**Current slice:** UEF construction now emits its paired blue-white streams from every authored
`General.BuildBones.BuildEffectBones` model bone to the nearest two top corners of the target's
`Physics.MeshExtents` cube. The endpoints descend with progress and exchange places every 0.6
seconds, matching `EffectUtilities.lua:227-303`; stalled work emits nothing. Other factions keep
the existing centre stream until their authored construction styles land.

Weapon effects now resolve from the original Lua declarations — projectile and unit
scripts, faction classes, `EffectTemplates.lua`, `defaultcollisionbeams.lua` — executed in an
isolated Lua state: PolyTrails, FxTrails, beams, `FxMuzzleFlash` and the impact lists, with
their emitter blueprints and DDS textures. Emitters follow authored curves, emission rates,
blend modes including inverse modulation and refraction, and sort order; muzzle emitters
track the resolved bone; beams follow live endpoints for the authored lifetime; ribbon
trails draw the original TrailBlueprints over each shot's recorded path; original
projectile meshes draw through the unit pipeline, pointed along the shot's velocity
(ADR-096 to ADR-103, [weapon visuals acceptance](weapon-visuals-acceptance.md)).

Weapon fire now plays the blueprint's own `Audio.Fire` cue: the companion `.xsb` sound bank
resolves the cue name to wave-bank entries and `WeaponSounds` picks one take per shot by
the shooter's handle (ADR-106). The retail UEF weapon bank's Gauss cue resolves to entries
6, 5 and 7 as measured on the bytes.

**Largest gap:** script-driven manipulators do not run; weapon cues ignore `LodCutoff`
distances, XACT pitch/volume ranges, RPC curves and instance limits. Mesh blueprints' LOD
tables are honoured (100 retail projectile meshes load); only blueprints whose mesh
blueprint is absent from the archives keep their strips. The gallery verifies this renderer,
not pixel parity with retail.

```text
/goal Advance FA-PRESENT by honouring the weapon cues' authored ranges: read the XACT
pitch/volume variation and the instance limit per cue, map LodCutoff through SupCom.xgs to a
distance, and apply them in the mixer. Write tests on the retail UELWeapon pair, run make
test, update WP-42 and FA-PRESENT, and leave dynamic music and broad effect hosting as
explicit later slices.
```

### FA-PERSIST - Replay, Hashing, And Save/Resume

**Current slice:** SaveState v16 retains v1-v15 decoding and adds optional economy,
construction identity/funding, army lifecycle and aircraft-controller state to
the existing units, queues, tick and RNG envelope. Fresh-scene economy/army
continuation matches 690 further tick hashes through construction, allied sharing,
defeat cleanup and winner confirmation. Restore owns the arrays and rebinds match
spans. Whole-match overflow sharing now runs before capacity clamping.

**Largest gap:** this is not a general mid-combat app save. Projectile/feature
pools, pending path searches, intel history, external input and the opponent VM
remain outside the envelope. Replay is a separate compatibility boundary.

```text
/goal Finish full-content and golden verification of the v16 economy/army continuation slice.
Before claiming general app save/load, inventory every omitted authoritative group and implement
its continued-hash proof. Keep pending path searches and replay transport separate, preserve old
readers, and update WP-44 and FA-PERSIST.
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
