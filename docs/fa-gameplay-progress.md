# Forged Alliance gameplay progress

This is the single operating dashboard for answering two questions:

1. How close is Recoil Metal to broad retail Forged Alliance gameplay?
2. What exact subsystem task should run next?

Detailed retail evidence remains canonical in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md). This dashboard summarizes that ledger; it
does not replace its claims, addresses, counterevidence, or confirmation gate.

**Snapshot:** 2026-09-03, dirty Recoil Metal worktree through the `WP-29` tactical silo ammunition
slice — projectile-economy parsing, one `SiloAmmo` component per (unit, slot) created at spawn,
`C-241` auto-refill with `C-083`/`C-084` whole-tick economy funding, `C-085` launch-then-guarded-
consume, generation-safe owner-death reaping, SaveState v9 — plus the `C-239`/`C-240` capture
timing and transfer-inventory reads, the `C-211` own-queue-before-guardee slice, and the `C-236`–
`C-238` capture/ownership-transfer contract, and prior: the `C-091` automatic-incumbent slice plus
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
[`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception), interception accounting against
the new stored-ammo state (`C-086`/`C-087`) in `WP-29`. **Current EXE-analysis action:** the
unread `HasSiloAmmo` callers (the zero-ammo launch gate, `C-085`'s open edge) and the capture
increment semantic at `Unit+0x690` (`C-239`).

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
| [`FA-CMD`](#fa-cmd---commands-controls-and-factories) | Commands, controls, factories | `WP-12`-`14` | 70% | 50% | 75% | Extend the guard ladder with `C-183`'s attack branch (`GuardScanRadius`, ordinary-attacker delegation) ahead of the remaining assist branches. |
| [`FA-ECON`](#fa-econ---economy-construction-and-engineering) | Economy, construction, engineering | `WP-15`-`19` | 70% | 55% | 90% | Name the capture increment at `Unit+0x690` and read `Sim::TransferUnit`'s copy/reset inventory, then specify the smallest capture slice. |
| [`FA-LAND`](#fa-land---land-navigation-formations-and-spatial-world) | Land navigation, formations, spatial world | `WP-20`, `21`, `26` | 85% | 30% | 95% | Add bounded formation rotation or category matching without changing path-service ordering. |
| [`FA-AIR`](#fa-air---aircraft-flight-combat-and-staging) | Aircraft flight, combat, staging | `WP-22` | 30% | 10% | 95% | Implement the deterministic winged-aircraft mover foundation. |
| [`FA-NAVY`](#fa-navy---surface-and-submerged-warfare) | Surface and submerged warfare | `WP-23`-`24` | 35% | 10% | 75% | Implement one complete `SurfacingSub` dive/surface slice. |
| [`FA-TRANSPORT`](#fa-transport---attachments-cargo-and-ferries) | Attachments, cargo, ferries | `WP-25` | 35% | 5% | 95% | Add parent/self bone indices and authored rest-bone composition to generic attachments. |
| [`FA-WEAPONS`](#fa-weapons---targeting-weapons-and-projectiles) | Targeting, weapons, projectiles | `WP-27`-`28` | 97% | 72% | 90% | Implement `C-157` target exemption for engineer reclaim/capture, then add the remaining death and manual-fire paths. |
| [`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception) | Silos, missiles, interception | `WP-29` | 35% | 15% | 95% | Add interception accounting against stored ammo (`C-086`/`C-087`), reading `HasSiloAmmo`'s callers for the fire gate first. |
| [`FA-DAMAGE`](#fa-damage---damage-death-and-shields) | Damage, death, shields | `WP-30`-`32` | 70% | 40% | 75% | Add the next collidable shield slice: area-shield admission and stacking. |
| [`FA-INTEL`](#fa-intel---vision-radar-sonar-and-counter-intel) | Vision, radar, sonar, counter-intel | `WP-33` | 75% | 45% | 90% | Apply radar-position error to automatic targeting without changing contact identity or ordering. |
| [`FA-PROGRESS`](#fa-progress---enhancements-veterancy-and-special-units) | Enhancements, veterancy, special units | `WP-34`-`36` | 35% | 20% | 35% | Complete the enhancement lifecycle specification around `CUnitScriptTask`. |
| [`FA-TERRAIN`](#fa-terrain---mutable-terrain-and-craters) | Mutable terrain and craters | `WP-37` | 0% | 0% | 25% | Trace one crater from damage through terrain, pathing, and rendering invalidation. |
| [`FA-AI`](#fa-ai---retail-ai-and-native-manager-boundary) | Retail AI and native manager boundary | `WP-38` | 35% | 5% | 30% | Remove the remaining condition-budget overruns (`recoil-metal-4754`). |
| [`FA-UI`](#fa-ui---player-interface-and-advanced-controls) | Player interface and advanced controls | `WP-39`-`40` | 65% | 5% | 15% | Capture the HUD visual/GPU baseline (`recoil-metal-3628`). |
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
own-queue-before-guardee branch is now implemented with its lifecycle closed in `C-211`, but the
rest of `C-183`'s nine-step guard ladder — attack via `GuardScanRadius` ahead of every assist, the
transitive build-assist chain walk, reclaim-copy, and the repair scan — remains absent.

```text
/goal Advance FA-CMD by implementing C-183's guard-attack branch for a guarding combat unit:
GuardScanRadius acquisition delegating to the ordinary attacker path, ordered after factory assist
and ahead of the remaining assist branches. Start with a focused guard-order regression; preserve
the active Assist/Guard head, shared-command ownership, ids, counters, serials, replay, and hashes;
do not synthesize commands. Run make test and make verify, then update WP-12 and FA-CMD.
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

**Largest gap:** aircraft use simplified terrain-following X/Z movement instead of retail's
trapezoidal flight solver, combat states, fuel, takeoff/landing, and staging.

```text
/goal Advance FA-AIR by implementing the deterministic winged-aircraft mover foundation from
C-221: explicit velocity and trapezoidal integration at 10 Hz plus the minimum takeoff/landing
state needed by one real aircraft. Start with focused fixed-point tests, run make test and make
verify, enumerate deferred combat/fuel/staging behavior in WP-22, and refresh FA-AIR.
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

**Largest gap:** automatic acquisition and explicit Attack orders share one target path, blocking
exact forced-target, minimum-range, arc, incumbency, and empty-priority behavior.

**Current slice:** `C-158` now admits automatic acquisition of current radar contacts but gives an
unidentified target retail's sentinel priority row `9999`, so it competes by score until the
viewing alliance has seen that exact unit generation. Sonar-only contacts remain rejected.
`C-167` applies the 4x out-of-arc score penalty, while `C-156` rejects a weapon with no authored
`TargetPriorities`. `C-091` retains a generation-safe automatic incumbent per weapon while it
remains eligible, and replaces it only for a strictly better candidate; fixed-hull aiming consults
the same incumbent. Explicit Attack remains separate, including when its target has gone stale.
`C-157` now rejects automatic candidates outside the immutable playable rectangle before
restriction/range/scoring and clears an incumbent that leaves it; live matches use their full map
extent and explicit Attack remains unrestricted. These automatic-acquisition slices do not wire
`SetDoNotTarget` or the engineer reclaim/capture target-exemption rule through the FAF Lua API.

```text
/goal Advance WP-15 with type-safe unit-target Reclaim/Capture command support before implementing
`C-157`'s engineer `IsTargetExempt` reject. Feature-only reclaim handles cannot safely name a unit;
keep the completed playable-rectangle filter, explicit Attack, candidate caches, and
projectile-defense targeting out of scope. Run make test and make verify, then update WP-15 and
FA-WEAPONS.
```

### FA-MISSILES - Silos, Missiles, And Interception

**Largest gap:** the tactical silo ammunition path now runs end to end, but interception
accounting, strategic-nuke silos, and the counter/fire commands are absent.

**Current slice:** `C-095` gives projectile-target weapons a separate nearest-hostile,
in-range 2-D acquisition path with `max(MaxRadius, MaxRadius * TrackingRadius)` reach; it neither
uses nor mutates unit-target priority/incumbency state. `C-086` runs its interceptors through the
ordinary swept projectile tick and selects the earliest contact deterministically. Fixed-hull
defence aims before firing. **WP-29's ammo path is implemented:** projectile blueprints resolve
through the VFS for Economy costs/BuildTime; a CAiSiloBuildImpl-shaped `SiloAmmo` component per
(unit, slot) — slot from `NukeWeapon` alone (`C-081`/`C-082`, first same-slot weapon wins per
`C-085`) — is created at spawn, auto-refills tactical-first while `stored < capacity` (`C-241`),
funds as a whole-tick economy-event consumer with partial-delivery residue (`C-083`/`C-084`,
UEB4302: 2400 ticks, 1.5 mass / 150 energy per tick from ART-S013/ART-S001), launches then
guarded-decrements (`C-085`), reaps generation-safe when the owner dies, round-trips in SaveState
v9 and participates in the match hash. **Named divergences:** no build queue, adjacency modifier
fixed at 1, zero-ammo fire gate unimplemented (`HasSiloAmmo`'s callers unread), `--units` crowds
unwired; an interceptor still consumes both shots rather than modelling `C-087`'s projectile
damage. No projectile guidance/lead, shooter cap, projectile health, or flare path.

```text
/goal Advance FA-MISSILES by implementing interception accounting against the stored-ammo state:
read HasSiloAmmo's callers first to close the zero-ammo gate, then model C-087 projectile damage
between interceptor and target instead of the consume-both shortcut. Use real intercepting
blueprints, write failing tests first, run make test and make verify, record the nuke-slot and
command-queue slices as the next boundary in WP-29, and refresh FA-MISSILES.
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

**Largest gap:** the deterministic FAF Lua opening works through many bindings, but condition
budget overruns and stand-in native managers keep it far from the retail AI architecture.

```text
/goal Advance FA-AI by completing item recoil-metal-4754: reproduce and remove the remaining FAF
condition instruction-budget overruns from the 650-second sanity run without raising the budget
blindly. Add focused regressions, rerun the AI report and long sanity match, update WP-38 and
FA-AI, and record the next missing native-manager behavior exposed.
```

### FA-UI - Player Interface And Advanced Controls

**Largest gap:** the native HUD is usable but lacks a measured baseline and much of retail's
data-driven command-page, overlay, key-context, and split-view behavior.

```text
/goal Advance FA-UI by completing HUD slice 0 / item recoil-metal-3628: capture the current visual
and GPU-performance baseline for default, selected-unit, commander, engineer, placement, observer,
and optional FAF-skin states at 1280x720, 1600x900, built-in Retina fullscreen, and the 5K display.
Record logical size, backing scale, HUD vertex count, GPU frame time, and an outside-repo contact
sheet with reproducible commands. Update WP-39/40 and FA-UI, then identify the smallest follow-up.
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
