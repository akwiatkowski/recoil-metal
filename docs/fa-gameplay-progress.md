# Forged Alliance gameplay progress

This is the single operating dashboard for answering two questions:

1. How close is Recoil Metal to broad retail Forged Alliance gameplay?
2. What exact subsystem task should run next?

Detailed retail evidence remains canonical in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md). This dashboard summarizes that ledger; it
does not replace its claims, addresses, counterevidence, or confirmation gate.

**Snapshot:** 2026-09-02, Recoil Metal worktree through the `C-091` automatic-incumbent slice, retail artifact
`ART-E001` (`c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0`).

## Headline

Equal-weight average across the 20 gameplay subsystem rows below; `FA-FOUND` is excluded:

```text
Implemented       [#########-----------] about 45%
Retail-validated  [####----------------] about 20%
Retail-analyzed   [#############-------] about 65%
```

Only **1 of 45 work packages**, `WP-15` economy, currently passes the complete retail
confirmation gate. The three percentages must never be combined: understanding absent behavior
does not make the game more complete.

The independent evidence-state count is **27 of 45 WPs at `Analyzed`**. That 60% inventory count
and the 65% equal-subsystem estimate answer different questions and are shown together to keep the
headline honest.

**Current implementation critical path:**
[`FA-CMD`](#fa-cmd---commands-controls-and-factories), closing the app-level live-versus-replay
acceptance gap in `WP-12` slice 4. **Current EXE-analysis action:**
[`FA-ECON`](#fa-econ---economy-construction-and-engineering), reading capture and ownership
transfer. Further broad EXE discovery is not the blocker.

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
| [`FA-MATCH`](#fa-match---armies-setup-and-victory-rules) | Armies, setup, victory rules | `WP-10`-`11` | 55% | 20% | 85% | Implement retail victory-mode/category predicates and allied-victory handling. |
| [`FA-CMD`](#fa-cmd---commands-controls-and-factories) | Commands, controls, factories | `WP-12`-`14` | 60% | 40% | 70% | Implement the retail guard/factory out-of-band queue-edit path without weakening shared-command ownership. |
| [`FA-ECON`](#fa-econ---economy-construction-and-engineering) | Economy, construction, engineering | `WP-15`-`19` | 70% | 55% | 85% | Read capture plus ownership transfer, then specify the implementation. |
| [`FA-LAND`](#fa-land---land-navigation-formations-and-spatial-world) | Land navigation, formations, spatial world | `WP-20`, `21`, `26` | 75% | 30% | 95% | Implement deterministic formation fan-out without weakening the per-army path-service budget. |
| [`FA-AIR`](#fa-air---aircraft-flight-combat-and-staging) | Aircraft flight, combat, staging | `WP-22` | 30% | 10% | 95% | Implement the deterministic winged-aircraft mover foundation. |
| [`FA-NAVY`](#fa-navy---surface-and-submerged-warfare) | Surface and submerged warfare | `WP-23`-`24` | 35% | 10% | 75% | Implement one complete `SurfacingSub` dive/surface slice. |
| [`FA-TRANSPORT`](#fa-transport---attachments-cargo-and-ferries) | Attachments, cargo, ferries | `WP-25` | 25% | 5% | 95% | Add authored bone-local transforms and attachment motion state before transport load/unload. |
| [`FA-WEAPONS`](#fa-weapons---targeting-weapons-and-projectiles) | Targeting, weapons, projectiles | `WP-27`-`28` | 97% | 72% | 90% | Implement `C-157` target exemption for engineer reclaim/capture, then add the remaining death and manual-fire paths. |
| [`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception) | Silos, missiles, interception | `WP-29` | 15% | 5% | 90% | Implement tactical silo ammo production and decrement-then-fire. |
| [`FA-DAMAGE`](#fa-damage---damage-death-and-shields) | Damage, death, shields | `WP-30`-`32` | 70% | 40% | 75% | Add the next collidable shield slice: area-shield admission and stacking. |
| [`FA-INTEL`](#fa-intel---vision-radar-sonar-and-counter-intel) | Vision, radar, sonar, counter-intel | `WP-33` | 70% | 45% | 90% | Add bounded retained-contact reaping/redetection and radar-error aiming. |
| [`FA-PROGRESS`](#fa-progress---enhancements-veterancy-and-special-units) | Enhancements, veterancy, special units | `WP-34`-`36` | 35% | 20% | 35% | Complete the enhancement lifecycle specification around `CUnitScriptTask`. |
| [`FA-TERRAIN`](#fa-terrain---mutable-terrain-and-craters) | Mutable terrain and craters | `WP-37` | 0% | 0% | 25% | Trace one crater from damage through terrain, pathing, and rendering invalidation. |
| [`FA-AI`](#fa-ai---retail-ai-and-native-manager-boundary) | Retail AI and native manager boundary | `WP-38` | 35% | 5% | 30% | Remove the remaining condition-budget overruns (`recoil-metal-4754`). |
| [`FA-UI`](#fa-ui---player-interface-and-advanced-controls) | Player interface and advanced controls | `WP-39`-`40` | 65% | 5% | 15% | Capture the HUD visual/GPU baseline (`recoil-metal-3628`). |
| [`FA-PRESENT`](#fa-present---animation-effects-and-audio) | Animation, effects, audio | `WP-41`-`42` | 55% | 5% | 25% | Complete one blueprint-audio-to-XSB-cue playback path. |
| [`FA-PERSIST`](#fa-persist---replay-hashing-and-saveresume) | Replay, hashing, save/resume | `WP-43`-`44` | 60% | 20% | 90% | Version and round-trip queued commands and command allocator state in SaveState. |

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

**Largest gap:** the current last-ACU-standing rule omits retail modes, 3-second defeat polling,
15-second winner stability, delayed cleanup, and unit caps.

```text
/goal Advance FA-MATCH by implementing retail's defeat poll and stable-victory timing from
lua/victory.lua behind failing match-rule tests, including delayed defeated-army cleanup. Run
make test and make verify, record deliberate mode gaps in WP-11, and refresh FA-MATCH.
```

### FA-CMD - Commands, Controls, And Factories

**Largest gap:** the app-level live/replay cutover covers build, PostSpawn roll-off, queued,
stopped, replaced, born-unit, and factory-repeat commands; retail's guard-driven factory queue
edits remain absent.

```text
/goal Advance FA-CMD by implementing the retail guard/factory out-of-band queue-edit path from
C-211 and C-183. Start with a focused shared-command lifecycle regression, preserve canonical
accepted sets, source counters, creation serials, queue ownership, and hashes. Run make test and
make verify, then update WP-12 and FA-CMD.
```

### FA-ECON - Economy, Construction, And Engineering

**Largest gap:** explicit repair now has a tested semantic, replay, economy, and UI slice against
`C-182`; capture, gifting, ownership transfer, and their callback order remain unread and
unimplemented.

```text
/goal Advance FA-ECON by reading CUnitCaptureTask::TaskTick at 0x0060AEB0 together with
Sim::TransferUnit at 0x0074DC40, recovering capture cost/progress, completion, ownership transfer,
gifting, and callback order with exact ART-E001 locators and counterevidence. Update WP-17, its
claims, and FA-ECON; create the smallest implementation task justified by the result.
```

### FA-LAND - Land Navigation, Formations, And Spatial World

**Largest gap:** completed land routes now detect a newly blocked final cell on their deterministic
mod-7/mod-13 phase and return through the C-176 retry path, but formations are absent.

```text
/goal Advance FA-LAND by implementing deterministic formation fan-out for one group move while
preserving the per-army FIFO path-service budget and C-176/C-177 retry semantics. Start with a
focused group-order regression, leave HPA* and reservations explicitly deferred, run make test and
make verify, then refresh FA-LAND.
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

**Current slice:** generic attachments capture a deterministic local X/Z offset, propagate it
parent-before-child after movement and collision, refresh their terrain/water/air layer, and persist
it in SaveState v4 while v1-v3 derive it from saved transforms. Attached children remain in the
collision grid, matching `C-196`. Parent death detaches surviving children and clears their local
offsets. This is deliberately not transport loading: bones, attachment motion state, capacity,
load/unload, storage, ferry, and carrier death are still absent.

```text
/goal Advance FA-TRANSPORT by adding authored bone-local transforms and attachment motion state
from C-195/C-196 without inventing transport capacity or load commands. Preserve deterministic
parent-before-child order, collision-grid presence, and the existing post-movement/post-collision
propagation points. Run make test and make verify, record the deferred transport controller behavior
in WP-25, and refresh FA-TRANSPORT.
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

**Largest gap:** retail silo and interception behavior is well analyzed, but Recoil Metal has no
complete ammunition production, launch accounting, or counter-missile gameplay path.

**Current slice:** `C-095` now gives projectile-target weapons a separate nearest-hostile,
in-range 2-D acquisition path with `max(MaxRadius, MaxRadius * TrackingRadius)` reach; it neither
uses nor mutates unit-target priority/incumbency state.
`C-086` runs its interceptors through the ordinary swept projectile tick and selects the earliest
contact deterministically. Fixed-hull defence aims before firing. This is deliberately a baseline:
there is no projectile guidance/lead, shooter cap, ammo, target restriction, projectile health, or
flare path; a positive-damage interceptor consumes both shots rather than modelling `C-087`'s
projectile damage.

```text
/goal Advance FA-MISSILES by implementing one tactical silo ammunition path from C-081 through
C-084: economy-throttled ammo build progress, integer stored ammo, and decrement-before-launch.
Use a real launcher blueprint and failing scarcity/launch tests, run make test and make verify,
record interception as the next slice in WP-29, and refresh FA-MISSILES.
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

**Largest gap:** retained radar contacts now expose their stable identity and maybe-dead status,
but their bounded reaping/redetection policy and other recon families remain incomplete.

```text
/goal Advance FA-INTEL by implementing bounded retained-radar reaping/redetection after C-079.
Start with a reproducible re-contact or expiry rule and preserve UnitId/last-known position/order;
keep cloak, jamming, water vision, sonar memory, and Lua bindings out of scope. Run make test and
make verify, then refresh WP-33 and FA-INTEL.
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

**Largest gap:** `UnitStore` can now preserve its allocator-safe live and tombstone state, but that
snapshot is not yet encoded by the versioned save/resume envelope or resumed as a full match.

```text
/goal Advance FA-PERSIST by encoding the allocator-safe UnitStore snapshot in the versioned v1
save/resume envelope and restoring it into a fresh store. Add focused round-trip and continued-hash
tests; preserve the existing tick/RNG envelope. Run make test and make verify, update WP-44 and
FA-PERSIST, and leave commands, economy, armies, and path-service state as later authoritative
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
