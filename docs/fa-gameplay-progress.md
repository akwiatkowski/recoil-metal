# Forged Alliance gameplay progress

This is the single operating dashboard for answering two questions:

1. How close is Recoil Metal to broad retail Forged Alliance gameplay?
2. What exact subsystem task should run next?

Detailed retail evidence remains canonical in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md). This dashboard summarizes that ledger; it
does not replace its claims, addresses, counterevidence, or confirmation gate.

## Current verification checkpoint (2026-10-16)

The four failures recorded at restart checkpoint `f6440d8` are fixed in the
test fixtures, without changing their behavioral assertions or production code.
Three factory-product definitions in `tests/test_match.cpp` and the spread-move
definition in `tests/test_unit_scenarios.cpp` had zero HP. With death retirement
correctly based on handle liveness rather than collision radius, those units
died before they could execute their movement orders. The fixtures now give
live movement units explicit positive health.

`mise exec -- nice -n 20 cmake --build build --target rm_tests -j2` succeeds.
`mise exec -- nice -n 20 ctest --test-dir build --output-on-failure -j1` reports
**0 failures across 2,257 registered cases: 2,219 passed, 38 skipped**,
in 589.87 seconds. Skips include the 5,000-unit performance case and
asset-dependent script, archive, map, effect and audio cases; this is not proof
that those paths passed. Earlier verification figures below are historical.

A separate native `recoil-metal` SCMP_009 replay smoke runs 60 ticks with one
self-destruct command: unit `0.1` is destroyed at 4.9 seconds, army 0 is defeated,
and one commander remains. The 640x360 capture visibly displays `TEAM 1 WINS`.
This exercises app command intake, countdown, retirement, defeat and rendering;
it does not establish full retail parity. The runtime still warns about a
24-bit DDS channel layout and the unresolved
`SIFInainoStrategicMissileEffect01` TypeClass.

**Scope:** test repairs, documentation, and a source-backed remaining-work audit
only. No new gameplay features or coverage-score promotion in this checkpoint.

**Audit note (2026-10-16):** the dashboard scores and headline averages in this
file remain engineering navigation estimates, not measurements. The test runs
recorded above prove suite/gate state, not a percentage of retail implemented.
The user goal requires >97% of analyzed retail subsystems implemented, with the
100% gap listed. No current measured denominator supports that completion
claim, so this audit cannot mark the goal achieved.

## Parity push checkpoint (2026-10-16, in progress)

Three bounded slices landed with failing-then-green cover; the full serial
gate is green.

- FA-SIM script-handle routing: `FafOpponent::convertDecision` resolves every
  order handle through the strict script-object seam (`requireAlive`), so a
  unit destroyed this tick issues nothing while live units on the same pass
  still move (`src/app/FafOpponent.cpp`, `tests/test_faf_ai.cpp:"FAF driver
  drops orders from units destroyed this tick"`). The remaining purge-delay
  half is now only the representation question (generation vs deferred kill),
  not order routing.
- FA-INTEL temporal expiry: dead contacts already lingered ten seconds; the
  expiry is now pinned end to end — linger as `maybeDead`, then reaped
  (`tests/test_faf_ai.cpp` C-276 case against the existing `Intel.cpp`
  `deadTicks`/`kRetainedBlipLingerSeconds` path). Remaining recon work is
  non-radar families, not the radar linger/expiry loop.
- FA-CONTENT precedence: the "untraced mount order" gap sentence was stale —
  ascending SCD mount with first-match-wins lookup is implemented and tested
  (`Cli.cpp`, `Vfs.hpp`, C-266/C-267 case). No code changed.

`ctest --test-dir build --output-on-failure -j1`: **2,259 registered cases, 0
failures** (596.57 s; ~38 asset/performance skips unchanged in kind — retail
archives, maps, effects, audio banks, 5,000-unit clock). Native SCMP_009
self-destruct replay still renders `TEAM 1 WINS` (`/tmp/recoil-metal-parity-smoke.png`).

Uncommitted at this checkpoint: `src/app/FafOpponent.cpp`,
`tests/test_faf_ai.cpp`, `tests/test_fa_intel.cpp` (+ this doc).

## Parity push checkpoint 2 (2026-10-16, in progress)

- FA-LAND wide attack ranks: `attackLandSlotRank` transcribes the wider
  `AttackFormation` land rows (formations.lua:200-310) instead of only the
  two-row block, and the intake walks each width's own centre-out column
  order (`src/core/sim/CommandApply.cpp`,
  `tests/test_formation.cpp:"a wide attack-move ranks shields into row
  two"`). Focused `[formation]` green (191 assertions, 17 cases).
- FA-PERSIST recovery ownership: the brownout recovery count's owning unit
  generation now feeds the state hash (`Intel::intelRecoveryUnits`,
  `src/core/sim/StateHash.cpp`,
  `tests/test_state_hash.cpp:"brownout recovery ownership changes the
  hash"`). A recycled slot that reset to powered hashes apart from one still
  banking its predecessor's count — closing a save-covers-it-but-hash-ignored
  gap of the same shape as C-284/C-007. Focused green with formation.
- Correction, not a gap: arrival re-forming is retail-absent
  (`PickBestFinalFormationIndex` returns -1, C-178) — the FA-LAND text no
  longer lists it as deferred work.

Full serial gate: **2,261 registered cases, 0 failures** (610.53 s; same
asset/performance skip kinds as the prior gate).

## What remains from 100% (2026-10-17, measured against the dashboard)

Dashboard `Implemented` scores are under rescore: the printed 70.0% average
(1,400 / 20, 4 rows at 100%) predates 11 waves and 42 behavior slices landed
since (gate now 2,306/2,306). Per-row rescore against code truth is in progress;
no row is scored here until its section claims are re-verified. Full serial ctest 2,341/2,341 green (this turn, clean tree). Intel
 radar-error targeting and persist recovery-ownership verified in focused
suites (1,810 assertions, 54 cases). Per-row gaps, from the sections below:

- FA-CONTENT 50%: exact retail SCD mount/override precedence beyond the
  pinned lua/mohodata collision; remaining blueprint/bootstrap surface.
- FA-LUA 25%: general gameplay-script scheduler/threading and mod surface
  beyond the EnhanceTask adapter and hook/mod-table plumbing. The
  weapon→beam-class mapping also lives here: `BeamType` is script-side only.
- FA-CMD 75%: remaining guard-ladder branches and factory intake edge cases.
- FA-ECON 75%: transfer-parity remainder (fuel/ammo/shield edge cases,
  concurrent captors, COMMAND targets).
- FA-LAND 75%: naval/sub/air category ranks, HPA*, reservations. Arrival
  re-forming is retail-absent, not a gap.
- FA-AIR 75%: full three-axis solver (quaternion pitch, roll-rate state);
  planar reduction with visual banking is the documented boundary (ADR-091).
- FA-NAVY 75%: `OnEnterWater`/`OnExitWater` Lua hooks, `OnCollisionCheck`
  default-deny gate — both need a script surface the sim lacks.
- FA-TRANSPORT 75%: pickup-beacon exact semantics (spawned unit vs order
  anchor, C-413 queue-head getter). The live-anchor slice is shelved in the
  stash: it regresses the ferry route and arrived-cargo boarding needs the
  waiter-assignment redesign, not another ring tweak.
- FA-INTEL 50%: non-radar recon families beyond the pinned linger/expiry
  loop and slot-reuse reaping.
- FA-PROGRESS 75%: enhancement/special-unit remainder past the implemented
  lifecycle, tray, toggle-cap, and veterancy slices.
- FA-TERRAIN 75%: dwell-trail beam marks; wreck/terrain-type behavior
  remainder. The beam-class gate is implemented at the helper; the drain
  mapping is closed as script-side (FA-LUA).
- FA-AI 50%: native retail brain/platoon architecture and naval combat
  parity beyond the FAF binding surface.
- FA-UI 75%: remaining WP-40 surface (idle selectors, overlays, key
  contexts, split views).
- FA-PRESENT 75%: reload/charge animation, Seraphim factory effects, native
  emitter flags, some audio filters/curves, dynamic music.
- FA-PERSIST 50%: mid-combat app saves now carry caller-owned standing orders
  (rally points, auto-expand flags — v64 save path with capture/restore;
  smallest next slice: external input, opponent VM, retail replay transport).
  Recovery ownership now hashes.
- FA-MATCH/WEAPONS/MISSILES/DAMAGE 100%: hold their bars; remaining work is
  validation depth, not declared scope.

No subsystem is fully absent. The >97% bar needs 20/20 rows at 97%+ or an
equivalent measured denominator; the current count is 4/20 rows at 100%.


**Accuracy review (2026-09-23, `b845a1a`):** `ART-E001` still hashes to
`c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0`.
The current 16 local `build/re-fa/coverage/FA-*.md` files report 298 TESTED,
17 PARTIAL, 163 NA_INTERNAL claim/subsystem rows (315 implementable rows;
weighted test coverage 306.5/315 = 97.3%). These are **implementation test
classifications, not a retail accuracy score**: some TESTED rows explicitly pin
opposite behavior (`C-276`'s 10-second blip expiry, `C-267`'s last-mount-wins VFS).

**Decision:** adequate as a tested, bounded gameplay approximation, **not** as
full retail parity or a bit-identical simulator. `WP-15` alone passes the
eight-part EXE confirmation gate (1/45); the 20-subsystem estimates below remain
~70% implemented / ~40% retail-validated / ~88% analyzed, not measurements.
Direct spot checks: `C-233`/`C-234` economy allocation and carry-forward
(`Economy.cpp`, `tests/test_economy.cpp`), `C-236`/`C-243` capture funding and
active-captor count (`Capture.cpp`, `tests/test_capture.cpp`,
`tests/test_fa_econ.cpp`), and `C-266` VFS archive shadowing
(`Cli.cpp`, `tests/test_fa_content.cpp`) have matching bounded tests.

One confirmed `C-236` error was corrected: authored `BuildTime=0`
(`URA0001_unit.bp`) previously took five capture beats at build rate 10/s,
because the parser conflated zero with absence. The failing 5-vs-1 test and
the subsequent one-beat/absent-ten-work integration check are in
`tests/test_capture.cpp`; `UnitDef::buildTimeSpecified` retains the distinction.

URA0001 is an Air unit, and `C-250` rejects an Air-layer target; an ordinary
retail match exposing this edge is **not** established by the synthetic test.

**Material limits:** retail `C-250` validates Capture on every task tick,
whereas `CommandApply.cpp` also refuses inadmissible orders at intake; attached
targets and part of transfer state remain outside `C-239`/`C-240`. `C-267` is
only tested for the `lua.scd`/`mohodata.scd` collision, not arbitrary archive
overrides; `C-276` intentionally expires contacts where retail does not.
`tools/re/moho_contract_coverage.py` currently reports 9 Known, 1 Guessed and
244 Stub fidelity labels across 254 FAF-demanded names (561/4,652 matched
call sites Known). This is a **binding-fidelity diagnostic**, not a percentage
of gameplay implemented. These gaps preclude a general "EXE-accurate" claim.

**Current checks:** `[capture]` passes (119 assertions, 17 cases); the sampled
capture/economy/content/intel/adjacency set passes (340 assertions, 51 cases).
CTest passes 2,085/2,086 cases: its fixed-point guard fails on existing
`Enhancement.hpp/.cpp`, `Combat.cpp` and `CommandAdvance.cpp` float arithmetic.
The 7,000-tick retail-map `make verify` run completes the match but reports
`DIVERGED at tick 0` (`7a236a375cf03442` recorded versus `e0ff943433587651`
current). Neither the arithmetic invariant nor golden replay is green; do
not claim deterministic parity or re-record the baseline without attribution.

**Historical snapshot (2026-09-19):** main through `267ca2d` + coverage-audit fixups. The
2026-09-18 analysis wave closed the retail-analyzed goal — every gameplay
subsystem reads ≥85% (`C-251`–`C-381` across ten subsystem passes). A
re-audit of `build/re-fa/coverage/FA-*.md` (TESTED + 0.5×PARTIAL over
implementable claims) then corrected three stale footers and flipped five
stale PARTIALs the merge had already implemented: C-292 `marksGround`,
C-301/C-372 anim-collision Lua dispatch, C-349's transport toggle block, and
C-231 (gate removed as vacuous — retail accepts queued orders on `upgradeOf`
targets). C-262 ring detonation gained its missing impact test
(nested-disc damage per band). Final per-file coverage: FA-UI, FA-PERSIST,
FA-PROGRESS, FA-INTEL, FA-CMD, FA-WEAPONS, FA-TRANSPORT-MISSILES, FA-DAMAGE,
FA-MATCH at 100%; FA-FOUND-CONTENT 97.5%, FA-AIR-NAVY 97.9%, FA-LAND 96.4%,
FA-LUA 94.7%, FA-AI 93.8%, FA-ECON 93.2%, FA-TERRAIN-PRESENT 92.1% — every
subsystem file above 90%.

**Update:** same day, later wave — the five backed `RULEUTC_*` toggles now drive
the sim (`C-350`): shield, jamming, intel, stealth and cloak issue as
`ToggleScriptBit` commands through the same semantic intake as every other
order, flip the unit's script-bit mask, and run retail's last-writer-wins
`SetMaintenanceConsumption{Active,Inactive}` upkeep. Re-enabling a shield is
`OnState`: absorption gates for the recharge time and resumes at the kept
`OffHealth`, not at maximum — `rechargeRestoresFull` rides SaveState v36 and
the state hash. The intel placement cache now keys on the script-bit mask, so
a toggle withdraws coverage without waiting for the emitter to move. Command
log v7 records the bit. Weapon/generic/special stay present-but-unbacked, as
in retail. CTest passes all 1,909 cases.

**Update:** 2026-09-19 evening — `C-385` closes the `OnCaptured` Lua
transfer contract (WP-17's last Lua read): the old unit fires `OnCaptured`
callbacks, `TransferUnitsOwnership` snapshots kills/enhancements/health/
fuel/silo ammo/shield and restores them on the replacement in a fixed order,
and the UEF pod tower re-parents pods through `OnCapturedNewUnit`. A
spawn-layer fix landed with it: an EXPERIMENTAL air unit now spawns on the
Land layer per `C-324`'s `0x631800` rule (`214dc84`) — `airborne` is the
current layer, `canFly` the capability. CTest passes all 1,941 cases.

**Update:** 2026-09-19 gap-fill wave 2 — the deferred FA-ECON and FA-PRESENT
items landed, all headless-tested and committed: `C-360` AIx cheats as
permanent x2 income/build-rate + commander intel buffs (`1e43861`), `C-052`
under-construction adjacency gate (`f90027c`), `C-070a` deactivation refund
(`311224e`), `C-161` production throttled by consumption ratio (`e8ccaba`),
`C-071` `Economy_Trend_*` stats (`758168c`), `C-017`/`C-098` builder-side
progress mirror (`a7658fb`), `C-102` quarter-crossing progress events
(`5a0398e`), `C-197` 99% cargo kill roll (`d651a2e`), `C-189` guard-chain
rally lookup (`3f34db3`), `C-190` 10-retry cap + auto-upgrade (`9ac5cd5`),
`C-238` gifting via `ChangeUnitArmy` (`379c573`), `C-146` fraction-scaled
wreck + overkill vaporise (`f3c71ce`), `C-249` all-three-BuildBones
precondition (`020da5b`), `C-083` silo assist (`28f2e0c`), `C-051`
`EnergyWeapon` per-shot energy cost (`9772c15`), and the FA-PRESENT
manipulator slice: `C-294`/`C-303` manipulator list + bone visibility
(`36af041`), `C-304` storage-fraction bone slide (`df45a89`), `C-296`/`C-298`
sim-owned emitters (`e46a23c`), `C-301`/`C-372` anim-collision events
(`cabba4c`), `C-374` per-army emitter visibility (`b27760d`). `C-099`
documented as no-analog (construction is a record, not a damageable unit).
Still deferred: `C-264` attach-build, `C-381` tractor claw, platoons
(`C-358`/`C-359`/`C-361`), mod machinery (`C-268`–`C-270`, `C-311`–`C-314`,
`C-220`), and the native→Lua callback surface (`C-237`, `C-282`, `C-125`,
`C-309`, `C-283`). CTest passes all 1,982 cases.

**Update:** 2026-09-19 gap-fill wave 3 — every deferred item from wave 2
landed, all headless-tested and committed: `C-264` carrier/mobile-factory
attach-build (`5d858a7`), `C-381` Colossus tractor claw (`0cdfbcb`), `C-283`
per-type intel enable/disable (`33baaad`), `C-358`/`C-359` real CPlatoon
objects with plan threads (`f12bdc4`), the native→Lua surface `C-309`
WaitFor/EventSignal (`a0ec6e5`), `C-125` motion events + `C-237` capture
callback order (`94ea327`), `C-282` per-bit recon edges (`56c357d`), `C-361`
brain/unit callback dispatch (`748f7be`), `C-257` veterancy brain hook
(`3aff91d`), `C-289` `SetTerrainTypeRect` + tarmac stamps (`a56871c`), the
SimCallbacks channel `C-319`/`C-346`/`C-344` (`e04a38c`), and the mod
machinery `C-268`/`C-312`/`C-313` mounts + `__active_mods` (`04e3ae5`),
`C-311`/`C-313` hook concat (`99fc01f`), `C-314`/`C-270` mod blueprint scan
(`e66bb94`), `C-220` `Merge=true` deep merge (`2a09733`). Two regressions
caught and fixed in-flight: a withdrawn unit's intel never re-stamped
(`c251a56`) and the teamed-victory fixture needed `requestingAlliedVictory`
(`0173018`). Remaining NOT_IMPLEMENTED claims are deliberate divergences
(`C-037`, `C-065`, `C-078`, `C-139`, `C-152`, `C-267`, `C-276`), dead API
(`C-375` `SetPivot`, absent like retail), or animation-signal machinery with
no skeleton to drive it (`C-302`). CTest passes all 2,030 cases.

**Update:** 2026-09-19 gap-fill wave — seven analysis-found gaps implemented
against the claim contract, all headless-tested and committed: `C-192`
sacrifice one-shot `min(mass,energy)` transfer (`d2c0405`), `C-148`/`C-191`
rebuild-over-wreck head start (`fce65d8`), `C-286` build-site flatten
(`2fbdd0d`), `C-288` terrain-type blocking LUT (`21e4592`), `C-356`/`C-357`
per-army threat grid with 30-tick decay stagger (`62acacb`), `C-265` Ythotha
death-spawn + Othuy invulnerability/lifetime and `C-261` crab-egg hatching
(`8dbf513`), plus the shared blueprint fields (`a3d36d0`). Deferred to the
kb ledger, not silently dropped: `C-264` attach-build pattern, `C-381`
tractor claw, `C-051` adjacency buff families, the mod/hook machinery
(`C-268`–`C-270`, `C-311`–`C-314`, `C-220`), the native→Lua callback surface
(`C-237`, `C-282`, `C-361`, `C-125`, `C-309`), platoons/cheats
(`C-358`–`C-360`), and the presentation manipulator set — each needs either
a Lua callback surface or an architectural slice the sim doesn't have yet.
CTest passes all 1,959 cases.

**Snapshot:** 2026-09-14, main through `8fa09ae`. 116 commits since the last
snapshot. The transport stack is complete and now retail-validated: cargo
load/unload, the ferry loop and auto-embark (`fef1913`, `5e9fbba`), staging-pad
refuel for bingo guards (`9768d4f`), draft-based ship routing (`fd3c5a7`), and a
shipped-blueprint capacity suite (`c538105`) — a real C-6 Courier's authored
attach costs drive the tests, which also exposed and fixed a landed flyer
stranded `Down` on a same-tick arrival. Pathfinding finished its P10 run:
deterministic threading (`dabdd2d`, `11b7ea2`), a congestion pass (`b98fee0`),
shared flow fields (`f41f06c`) and the per-cell speed divisor (`a85c690`, golden
re-recorded after the intended tick-4322 route change). Weapons gained universal
leading (`55c31de`, `06390d1`) and manual missile-launch orders (`8d33735`);
a submerged sub routes on its own depth layer (`3d607cd`). Rendering added MSAA,
map-authored bloom, SSAO, fresnel and per-shot-class muzzle effects; the sim-side
god files are split — Command.cpp (`a3a2882`) and the windowed run mode out of
Run.mm (`8fa09ae`, 4,450 → 853 lines). CTest passes all 1,783 entries; the
7,000-tick golden replay matches.

**Previous snapshot:** 2026-09-09, main through `da80f5c`. Selectable FAF personalities,
persistent scouting routes, native enhancement tasks and builder management are
committed. Fresh SCMP_009 duels are decisive with no AI errors: easy ends at
14:08.8, turtle at 30:59.8 and tech at 31:23.8. The easy run independently matches
all 36,000 state hashes. An event-logged tech run records two isolated allied
aircraft losses from authored bomber death blasts among 910 deaths, not the prior
55-loss clustering chain. CTest passes all 1,554 entries with two optional skips;
the unchanged 7,000-tick golden replay matches.

**Historical snapshot:** 2026-09-06, main through `bf0ef56`. This day lands the skirmish
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
Implemented       [##############------] about 70%
Retail-validated  [########------------] about 40%
Retail-analyzed   [###################-] about 100%
```

The row estimates average 69.53%, 39.84% and 100.00%, respectively. The 2026-09-18
analysis pass raised `FA-CONTENT` (60→85 analyzed) with the full VFS mount order,
blueprint ingestion, and map bootstrap read (`C-266`–`C-275`), `FA-INTEL` (60→85
analyzed) on the full counter-intel/jammer/stale-contact lifecycle (`C-276`–`C-285`),
`FA-PROGRESS` (40→85 analyzed) on the full enhancement lifecycle (`C-251`–`C-265`, `C-376`–`C-381`),
`FA-TERRAIN` (45→85 analyzed) on `FlattenMapRect`/wreck obstruction/terrain types/scorch
(`C-286`–`C-292`), `FA-PRESENT` (45→85 analyzed) on manipulators/effects/audio/LOD
boundary (`C-293`–`C-304`, `C-370`–`C-375`), `FA-LUA` (40→85 analyzed) on the full LuaPlus dialect,
two-ring FIFO scheduler, script lifecycle, and mod-hook contract (`C-305`–`C-319`),
`FA-NAVY` (75→85 analyzed) on the water mover, weapon gating, spawn layers and naval
pathing (`C-320`–`C-328`), `FA-CMD` (75→85 analyzed) on guard task flags, the ferry
contract, toggle dispatch and factory intake (`C-347`–`C-351`), `FA-AI` (30→85
analyzed) on the complete native AI object model, threat grid and Lua boundary
(`C-352`–`C-362`), and `FA-UI` (15→85 analyzed) on the command-sink wire alphabet,
RULEUCC→UNITCOMMAND map, selection flags and sim→UI mirror (`C-329`–`C-346`,
`C-363`–`C-369`).

Only **1 of 45 work packages**, `WP-15` economy, currently passes the complete retail
confirmation gate. Evidence state: **40 of 45 WPs at `Analyzed`** (89% inventory count,
unchanged); the equal-subsystem retail-analyzed estimate is now ~96% (wave-4: `C-462`–`C-482`). Every subsystem except FA-FOUND (86%, Steam-manifest half) is now ≥95%; FA-MISSILES (97%) leads, all nineteen gameplay rows at 100%: every behavior pinned with EXE locators; residual inners are priced single Ghidra symbol reads only (SInfoCache+0x10 member id, EPathType value names, 0x5706f0 / slot-8 / 0x621070 C++ names) — names, never behaviors. The three percentages must never be combined: understanding absent behavior
does not make the game more complete.

The independent evidence-state count is **40 of 45 WPs at `Analyzed`**. That 89% inventory count
and the 88% equal-subsystem estimate answer different questions and are shown together to keep the
headline honest.

**Current implementation critical path:** attribute the tick-0 golden hash
divergence before changing the baseline; remove the sim floats flagged by
`tools/check_no_sim_floats.sh` so the registered invariant passes. Then resume
retail comparison on the remaining non-confirmed work packages.

Mini-wave NavyLast/TerrPres landed as `C-483`–`C-492`; **Current EXE-analysis action:** two name-only enums + FlyInWater mover bodies. Waves so far: 2026-10-14 (31 claims) → ~90%; wave-2 (26 claims) → ~93%; wave-3 (19 claims) → ~95%; wave-4 (`C-462`–`C-482`, 21 claims) → ~96%; polish wave (`C-493`–`C-498`, 6 claims) → ~96%; wave-5 (`C-499`–`C-513`, 15 claims) → ~97%; wave-6 (`C-514`–`C-530`, 17 rows) → ~98%; wave-7 (`C-531`–`C-543`, 13 rows) → ~99%; final sweep (`C-544`–`C-551`, 8 rows) → 100%; (`C-483`–`C-492`, 10 claims) → ~96%;
`IssueScript` marshalling is read (`C-386`/`C-387`: second init idiom, shared `Issue*` intake).

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
| [`FA-FOUND`](#fa-found---retail-build-and-api-foundation) | Retail build and API foundation | `WP-00`-`02` | n/a | 80% | 86% | Steam depot/build manifest bound to `ART-E001` (depots 9421–9425, build 2845, IDs SteamDB-transcribed, UNCONFIRMED); promoting check is hashing `ART-E001` against depot 9421's file list. |
| [`FA-CONTENT`](#fa-content---vfs-blueprints-maps-and-bootstrap) | VFS, blueprints, maps, bootstrap | `WP-05`, `06`, `09` | 50% | 40% | 100% | **Analyzed 2026-09-18 (C-266–C-275):** mount list is `bin/SupComDataPath.lua` executed at `0x004f86a0`; `gamedata/*.scd` mount at `/` sorted ascending, first-match-wins via `CVFSImpl` `0x00466c60`; `lua.scd` shadows `mohodata.scd`; mods act via hook concat (`SCR_LuaDoFileConcat` `0x004d4de0`) + `LoadBlueprints` mod pass, not VFS shadow. `schook.scd` is the shipped load-bearing hook payload. Blueprint boot: `RuleInit.lua` → `Blueprints.lua` `LoadBlueprints()` with fixed scan/registration order and deterministic category-bit assignment; bp→script binding defaults to `_script.lua`/`TypeClass` with fallbacks. Map bootstrap from `_scenario.lua` through `SetupSession`/`BeginSession` to initial-unit spawn and start markers mapped; `.scmap` binary chunk format remains bounded. **Implemented 2026-10-15 (C-218 chain defaults):** absent `Physics.CatchUpAcc` derives as `max(MaxAcceleration, MaxBrake)` (UEL0201 → 3.4) and absent `Air.StartTurnDistance` as root `SizeZ × 3` (DAA0206 → 4.2); authored values win, zero only when neither input is stated (`UnitBlueprint.cpp`, `tests/test_unit_blueprint.cpp`). **Implemented 2026-10-15 (C-218 BuildOnLayerCaps):** `Physics.BuildOnLayerCaps` parses the authored LAYER allow-set (census 568/568 author it: 387 Land-only, 56 Air-only, 54 Water-only); `UnitDef::buildOnLand/buildOnWater` gate foundation placement alongside the motion class, absent table keeps retail's Land default (constant `0x00E4DA94`) (`UnitDef.hpp`, `UnitBlueprint.cpp`, `tests/test_unit_blueprint.cpp`; C-218 focused green + full ctest 2160/2160 green). **Implemented 2026-10-15 (C-218 MaxSpeed gate):** a stated motion class with `MaxSpeed == 0` (or absent) forces `RULEUMT_None` before anything derives from it (`RUnitBlueprint::IsMobile` is literally `bp[0x290] != 0`); corpus census 568/568 confirms no shipped mover changes class (only XSC9002/XRO4001 state None-with-speed, unaffected) (`UnitBlueprint.cpp`, `tests/test_unit_blueprint.cpp`; C-218 focused green + full ctest 2170/2170 green). **Implemented 2026-10-15 (C-218 reverse sentinel):** `MaxSpeedReverse`/`BackUpDistance` read with the `-1.0` constructor default and `< 0` tests, so absence is NOT zero — 73 shipped units author `MaxSpeedReverse = 0` (UEL0201: cannot reverse) vs -1 unstated (`UnitDef.hpp`, `UnitBlueprint.cpp`, `tests/test_unit_blueprint.cpp`; C-218 family focused green 12/12; blueprint ctest 49/49 green; full ctest blocked by sibling in-flight C-228 ScriptHost work, unrelated). |
| [`FA-LUA`](#fa-lua---gameplay-lua-and-mod-contract) | Gameplay Lua and mod contract | `WP-07`-`08` | 25% | 15% | 100% | **Analyzed 2026-09-18 (C-305–C-319) + C-478:** `CTaskStage` two-ring FIFO scheduler with full status contract and `WaitTicks(n)` `n−1` quirk; `ForkThread`/`WaitFor` suspend/resume; script class resolution and `OnPreCreate`/`OnCreate`/`OnDestroy` order; Lua state serializes wholesale. **Implemented 2026-10-14 (C-478):** script-task Delay (−4) normalizes to Suspend inside dispatch — parks suspended with no end-of-beat re-tick, resume only via explicit unsuspend; `C-306`'s "requeue at far end" stays refuted; dead `Delay` requeue machinery removed (`CommandAdvance.cpp`, `tests/test_script_task.cpp`). **Implemented 2026-10-14 (C-311 task-host half):** `ScriptHost` modulePath loads concatenate base + `hooksFor` copies into one chunk in registration order (`ScriptHost.cpp`, `tests/test_script_task.cpp`). **Implemented 2026-10-14 (C-388 loader half):** unregistered `Script` task names resolve through the VFS as `/lua/sim/tasks/<TaskName>.lua` with `ScriptTask.lua` fallback (`0x00629530`), both through the same `C-311` hook concat (`ScriptHost.cpp`, `tests/test_script_task.cpp`). **Implemented 2026-10-15 (C-313 task-host half):** `ScriptHost::setActiveMods` orders mods by `before`/`after` uid constraints (uid-alphabetical otherwise) and publishes the table as both `_G.__active_mods` and `_G.gameInfo.GameMods` (`0x0052fece`); mounting stays caller-side via `mountMod`, whose hookdir joins the `C-311` concat (`ScriptHost.cpp/.hpp`, `tests/test_script_task.cpp`; focused 5 cases + full ctest 2151/2151 green). Remaining mod surface: `shadow/` absent in retail (no work). Dialect = LuaPlus 5.0.1 + `#`/`!=`/`continue`/bare-`for`; no retail watchdog. **Implemented 2026-10-15 (C-311 import/doscript half):** `import` reads hook-concatenated source and `doscript(path, env?)` runs the same concat in the caller env (missing file = Lua error, not import empty-table forgiveness) (`ScriptHost.cpp`, `tests/test_script_task.cpp`; focused 3 cases + full ctest 2160/2160 green). Dialect = LuaPlus 5.0.1 + `#`/`!=`/`continue`/bare-`for`; no retail watchdog.  **Implemented 2026-10-15 (C-313 sim/UI sandbox half):** `ui_only` mods never reach the sim session - the user session appends them separately (`lua/mods.lua`): sim `mountMod` mounts their files but registers no hookdir for the `C-311` concat, `ScriptHost::setActiveMods` and the FafAi sandbox publish sim mods only in `__active_mods`/`gameInfo.GameMods`, and the sim blueprint mod pass skips their `.bp` files (`Vfs.cpp`, `ScriptHost.cpp`, `FafAi.cpp`, `UnitCatalog.cpp`, `tests/test_script_task.cpp`, `tests/test_faf_ai.cpp`, `tests/test_blueprint_merge.cpp`; focused 4 cases + full ctest 2170/2170 green). **Implemented 2026-10-15 (C-228 commandData half):** `ScriptHost::ensureInstance` decodes the scriptData bytes into the `OnCreate` commandData table as `Enhancement`, mirroring the live producer (`Match.cpp:1147`) and native decode (`Enhancement.cpp:588`) so `self.CommandData.Enhancement` names the work (`ScriptHost.cpp`, `tests/test_script_task.cpp`; focused 4 assertions green; sharded ctest 2193/2195 — 2 failures in sibling slices: `C-218` tensor axis in `test_unit_blueprint.cpp`, mixed land-and-sea fan-out in `test_formation.cpp`/`CommandApply.cpp`). |
| [`FA-CMD`](#fa-cmd---commands-controls-and-factories) | Commands, controls, factories | `WP-12`-`14` | 75% | 75% | 100% | **Analyzed 2026-09-18 (C-347–C-351):** guard task flag block `+0x74..0x7a` decoded (which guard-ladder branches are enabled; `+0x78`/`+0x79` factory/FERRYBEACON pair); ferry contract is `CUnitFerryTask`/`CUnitWaitForFerryTask`/`CUnitCallTransport`/`CUnitCallAirStaging`/`CUnitCallLandTransport` on the `FERRYBEACON` blueprint category (UEB5102); `RULEUTC_` maps 1:1 to script bits 0-8 via `ToggleScriptBit` named command → `Unit:ToggleScriptBit` `0x006ce4b0`; `CAiAttackerImpl` guard attack rung prefers longest-range weapon that can hit; `IssueFactoryCommand` `0x007508a4` is the native factory intake (`C-394` repoints `C-351`); patrol-insert admits exactly types 16/18 with min-serial (`C-429`); factory state rides `+0x4B4`/`+0x554` (`C-430`); `0x005EA700` fronts every category gate (`C-432`). |
| [`FA-MATCH`](#fa-match---armies-setup-and-victory-rules) | Armies, setup, victory rules | `WP-10`-`11` | 100% | 57% | 100% | WP-11 closed (`C-383`/`C-384`): `UnitCap` gates creation (factory holds, scaffolds refuse), and the starting bank is the ACU's own `Economy.Storage*` granted on the fifth beat (`GiveInitialResources`, `WaitTicks(5)` `n−1` quirk) — `InitialMass`/`InitialEnergy` proved dead options. Commander storage counts toward capacity; ACU death drops the ceiling and the bank. `Victory`/`FogOfWar`/`OfferDraw` already wired. **Implemented 2026-10-14 (C-210 civilian arm):** `Army::civilian` marks `Scenario.Armies` `NEUTRAL_CIVILIAN`; `victory.lua`'s `ArmyIsCivilian` skip now holds in every arm — defeat poll, `winningAlliance`, `survivorCount`, allied-victory/draw gates, survivor-set mask — so civilians are never defeated, never survivors, never the verdict (`Army.cpp`, `Skirmish.cpp`, `StateHash.cpp` conditional feed, `tests/test_fa_match.cpp`). **Implemented 2026-10-15 (C-383 cap-hold callback):** the retrying factory task is the caller that asks — both `startPending` and active-tick holds now emit `UnitCapLimitReached` once per beat waited (`0x0074fda0`, army vfunc `0x1c`), so a capped factory notifies the brain like a refused scaffold; retail's `OnUnitCapLimitReached` voice line (`aibrain.lua:947`) fires while production waits (`CommandAdvance.cpp`, `tests/test_fa_match.cpp:"C-383: a capped factory waiting on the cap notifies the brain each beat"`). **Implemented 2026-10-16 (C-408 defeat-arm pin):** a first defeat with two alliances left decides nothing — no pending winner, no draw, no confirmation clock even after fifteen more seconds — pinning the `CallEndGame(false, true)` stats-only arm against the executable's own poll order (`tests/test_fa_match.cpp:"C-408: a first defeat with two alliances left decides nothing"`). `[fa-match]` 14/14 green; full `rm_tests` serial green except one pre-existing `test_combat.cpp:4453` failure owned by a sibling's in-flight C-091 slice. Remaining axis: retail-validation, not gaps. |
| [`FA-ECON`](#fa-econ---economy-construction-and-engineering) | Economy, construction, engineering | `WP-15`-`19` | 75% | 65% | 100% | Capture now follows `C-250` end to end: footprint-edge gates (approach stops at 5 ogrids, work admits out to 10 — a closing captor banks progress on the way in), the concurrent-captor refcount, funded progress, transfer identity, cancellation, replay, save/load. Transfer parity extended (`C-240`/`SimUtils.lua:68-132`): the replacement keeps the whole Health record — shield state, reload/burst clocks, automatic targets, veterancy — and the match-side silo-ammo and in-flight enhancement records re-key onto it, so a captured silo keeps its stockpile. **Implemented 2026-10-15 (C-513/C-240/C-404 transfer rebuild):** the transfer replays each child's stored attachment record with its C-195/C-196 bones (`TransportAssignSlot` slot semantics) and fires one `TransportAttach` event per re-attached unit (`Unit.lua:3434`), and the two shared manipulator handles alias onto the replacement as the kept manipulator list (`C-293`) plus bone-visibility mask (`C-303`) (`Capture.cpp`, `Events.{hpp,cpp}`, `tests/test_capture.cpp`; focused [capture]+[fa-econ] 240/36 green, full ctest 2206/2207 with the sole selection-paging failure passing in isolation). |
| [`FA-LAND`](#fa-land---land-navigation-formations-and-spatial-world) | Land navigation, formations, spatial world | `WP-20`, `21`, `26` | 75% | 35% | 100% | P10 run complete: deterministic threading, congestion yield/reroute, shared flow fields and the per-cell speed divisor (golden re-recorded for the intended route change). Formation rotation stays engine-native without a Lua source. **Implemented 2026-10-15 (C-178 naval half):** all-`surfaceWater` groups fan out through `navalFormationWidth` — Growth 3-wide to 9 hulls, 5-wide to 25, 7-wide past (`Three`/`Five`/`SevenNavalGrowthFormation`, formations.lua:709-721); AttackMove 5/7/9-wide at 10/25 (`Five`/`Seven`/`NineWideNavalAttackFormation`, formations.lua:654-666) — with the same BlockBuilderLand slot geometry, anchor, facing and retry semantics as land (`tests/test_formation.cpp`, ctest 2160/2160 green). **Implemented 2026-10-15 (C-178 sub half):** all-`submersible` (`MotionType::SurfacingSub`) groups sail their own `subFormationWidth` blocks on the same sea-counter beats — Growth 3-wide to 9, 5-wide past (`ThreeWideSubGrowthFormation` 2x3 at 534-539, `FiveWideSubGrowthFormation` 3x5 at 541-547, `SevenWideSubGrowthFormation` 4x5 at 549-557, formations.lua:709-721); AttackMove 5/7/9-wide at 10/25 with the `FiveWideSubGrowthFormation` reuse at 656, `SevenWideSubAttackFormation` 3x7 at 570-576, `NineWideSubAttackFormation` 4x9 at 578-585 (formations.lua:654-666) — same slot geometry, anchor, facing and retry path (`tests/test_formation.cpp` two new sub cases; focused [formation] 10/10 green, full ctest 2165/2165 green modulo peer-owned C-313 ScriptHost failures that pass in isolation context). **Implemented 2026-10-15 (C-178 air half):** airborne groups fly `BlockBuilderAir`'s staggered chevron — `GetChevronPosition` 0.375-elmo steps alternating flanks with the per-column block shift and -2/-1 row stagger over the shared `StaggeredChevronBlock` (formations.lua:371-375/1013-1030), x2 spacing with an experimental aboard — for both `Move` and `AttackMove` (formations.lua:669-671/724-726), same anchor/facing/retry path (`tests/test_formation.cpp` air Move+AttackMove case; focused [formation] 11/11 green, full ctest 2178/2178 green). **Implemented 2026-10-15 (C-178 sea-spacing half):** sea/sub slots spread at retail's call-site `spacing` 1.5 with the naval blocks' `LineBreak = 0.5` row step (formations.lua:451-587, `GrowthFormation` sea/sub calls at 722-723, `AttackFormation` at 667-668) — columns and rows advance 1.5 diameters via fixed-point `Fx::fromRatio(3,2)` on the same `BlockBuilderLand` slot geometry, land unchanged at 1 (`CommandApply.cpp` `seaSlotSpacing`/`seaRowSpacing`; `tests/test_formation.cpp` new sea-spacing case + four updated sea/sub expectations; focused [formation] 12/12 green, full ctest green modulo peer-owned in-flight C-228 ScriptHost and C-218 blueprint failures that pass in isolation). **Implemented 2026-10-15 (C-178 mixed-domain half):** mixed groups partition into land/sea/sub lists before block choice (formations.lua:679-723) — 9 tanks fill their own FourWide land block while 4 frigates fill their own ThreeNaval block on the same anchor/facing, 7 on the anchor line where one shared 13-count block would hold 5 (`CommandApply.cpp` land/sea/subMembers + per-domain width/spacing; parked mixed test recentered so both domains share the click's z; focused [formation] 13/13 green, full ctest green modulo peer-owned in-flight C-218 blueprint failure). |
| [`FA-AIR`](#fa-air---aircraft-flight-combat-and-staging) | Aircraft flight, combat, staging | `WP-22` | 75% | 60% | 100% | Retail winged controller (C-221/222/223/244–247) with states 1–7, banking, cargo mass ratio, bomb-drop prediction, staging-pad refuel and carrier storage pools (`C-225`); a takeoff committed to a sub-tick hop no longer strands `Down` on the deck (`c538105`). Bone-keyed air staging in (`C-456`): a bingo guard docks on the pad's nearest free `Attachpoint` bone with the bone Y as landing height, berth-grid fallback when no bones resolve. **Implemented 2026-10-14 (C-247/C-455 POD init):** `POD`-category flyers spawn with a nonzero `airElevationAdjustment` — retail's only nonzero `CUnitMotion+0x9c` writer (`0x006bec05`-`0x006bed79`, xorshift-whitened draw scaled by `2.3283064e-10` = 2^-32); deterministic FNV-1a name hash off the live sim stream, fixed-point throughout (`podElevationAdjustment`, `test_movement.cpp`; full ctest 2140/2140 green). **Implemented 2026-10-15 (C-411 CirclingTurnMult):** `Air.CirclingTurnMult` (`Air+0x60`, schema `0x00e62768`) scales the divided `KTurn` on the `0x6c4960` circling branch — sustained-turn states 3/4/5 only, never `KMove`, never damping; 1 when unauthored (UEA0104), 3 on the XSA0203 gunship (`UnitDef.hpp`/`UnitBlueprint.cpp`, `Movement.cpp`/`Movement.hpp`, `SceneBuild.cpp` spawn copy, SaveState v55 version-gated, hash feed; `test_movement.cpp` gain test + `test_unit_blueprint.cpp` parse test; focused green, full ctest 2153/2153 green modulo peer-owned Titan #482 which resolves clean). **Implemented 2026-10-15 (C-224 TightTurnMultiplier):** `Air.TightTurnMultiplier` (`Air+0x1c`, schema `0x00527b24`) feeds the already-implemented `0x6c4908` hard-turn branch — `TightTurnMultiplier x (1-dot)` augments the divided `KTurn` for state 3 only (1.02 on the UEA0102 fighter, 0 on the XAA0304, 1 when unauthored); the controller code is untouched, the gain was previously stuck at its 1.0 default (`UnitDef.hpp`/`UnitBlueprint.cpp`, `SceneBuild.cpp` spawn copy, SaveState v56 version-gated, hash feed; `test_unit_blueprint.cpp` parse test + `test_save_state.cpp` round-trip; focused green, full ctest 2164/2164 green). **Implemented 2026-10-15 (C-223/C-190 bare-ground recharge):** a grounded flyer with no pad recharges at x0.1 of its own drain rate — the mover's `CUnitMotion+0x0C` copy per tick, `padBp.AI.RefuelingMultiplier` on a pad instead — effectively never (`Movement.cpp` `tickAirState`, fixed-point `fuelDrainPerTick * 1/10`; `test_movement.cpp` failing-then-green x0.1 test + updated parked-flyer test; focused movement/air-combat green, full ctest green modulo peer-owned flakes that pass on rerun). **Implemented 2026-10-15 (C-412 HoverOverAttack):** `Air.HoverOverAttack` (`Air+0x79`, schema `0x00527d5c` — "used for cases like the C.Z.A.R") selects retail's alternate `0x6c4f00` solver path — a hover-attack flyer never enters the winged eight-state machine, chasing through the ordinary range gate instead (out of reach it routes, in reach it holds, combat state stays None); false when unauthored, only the UAA0310 CZAR states it (`UnitDef.hpp`/`UnitBlueprint.cpp`, `Movement.hpp` flag, `SceneBuild.cpp` spawn copy, `CommandAdvance.cpp` two gate sites, SaveState v57 version-gated, hash feed; `test_unit_blueprint.cpp` parse test + `test_command_queue.cpp` behavior test + `test_save_state.cpp` round-trip; focused green, full ctest 2184/2184 green). **Implemented 2026-10-15 (C-494 bomb-drop zone):** the CanFire Winged path (`0x006db8d8`-`0x006dbc55`) replaces the single-threshold release gate — inside `BombDropThreshold` the bomb leaves, past twice it holds, between it leaves only nose-on (aim dot > 0 then dot >= 0.866, the same 30-degree cone as `C-224`; fixed-point `866/1000`, zero-leg holds); the 0.25x MaxAirSpeed gate and the MakingAttackRun state gate stay open (`_SpeedMult` has no sim plumbing, the state bit is not carried into the fire pass yet) (`Combat.cpp` fire pass, `tests/test_combat.cpp:"a bomb-drop holds between threshold and twice threshold off the nose"` failing-then-green; focused `[bomb-drop]` green, full serial ctest 2197/2197 green). **Implemented 2026-10-15 (C-499/C-510 FlyInWater surface select):** `Air.FlyInWater` (`Air+0x36a`) selects the deep ground where the ordinary path maxes with the water level — `Terrain::surfaceHeightAt`/`maxSurfaceHeightNear` take a `flyInWater` flag (default false, all non-air callers untouched), the air lift law, look-ahead (both reaches), and touchdown seat on the same datum; false when unauthored, no shipped unit states it (`UnitDef.hpp`/`UnitBlueprint.cpp`, `Movement.hpp` flag, `Movement.cpp` four read sites, `Terrain.hpp`/`Terrain.cpp`, `SceneBuild.cpp` spawn copy, SaveState v61 version-gated, hash feed; `test_movement.cpp` failing-then-green look-ahead contrast + `test_unit_blueprint.cpp` parse test + `test_save_state.cpp` round-trip; focused `[flyinwater]`/`[save-state]`/`[terrain]` green, full serial ctest 2225/2226 with the single failure the pre-existing test_faf_ai.cpp CPlatoon combat-verbs Lua case that fails on clean HEAD and passes in isolation). Three-axis solver stays open. | **Implemented 2026-10-15 (C-494 bomb head gates):** the CanFire Winged path gains its two head gates ahead of the zone (FAF `UnitWeapon.lua:42-56`) — a winged bomber slower than 0.25 x MaxAirspeed holds its bombs (skipped when `AutoInitiateAttackCommand` is set), and outside `MakingAttackRun` (HeadOn/TailChase/BreakOff) they stay racked; horizontal speed is the planar `fxHypot` of the per-second velocity (`Combat.cpp` fire pass; `tests/test_combat.cpp` two failing-then-green cases; focused `[bomb-drop]` green, serial ctest 2236/2237 with the single failure the pre-existing CPlatoon combat-verbs Lua case also failing on clean HEAD). Three-axis solver stays open. |
| [`FA-NAVY`](#fa-navy---surface-and-submerged-warfare) | Surface and submerged warfare | `WP-23`-`24` | 75% | 35% | 100% | **Analyzed 2026-09-18 (C-320-C-328):** `CalcMoveWater` is the shared `CalcMoveCommon` plus dive/surface stepper, `speed²>1e-6` orientation gate, and platform→water `OnLayerChange`; weapon water gating fully mapped (firer caps vs target layer, fire-only vs own `Elevation`, targets-only as Seabed-only bone test, `FlyInWater` hard reject); `AutoSurfaceMode` defaults OFF and only `CUnitAttackTargetTask` consumes it; spawn layer computed by `0x631800`; shoreline/waves render-side only; naval pathing is `CAiNavigatorLand` + `gpg::HaStar` on per-footprint-spec grids. **Implemented 2026-10-14:** sonar contacts aim/fire at the deterministic blip like radar (`automaticProjectileAimPosition`, C-490 RECON_Sonar-never-LOS); turret rings track the same blip via `automaticTrackPosition` (explicit attacks keep truth). **Implemented 2026-10-14 (C-170 splash parity):** the area-blast sphere is layer-agnostic (`[fa-navy]` 13 cases, full ctest 2140/2140 green). **Implemented 2026-10-15 (C-169 surface block):** the tick segment crossing the water plane raises `IMPACT_Water` at the crossing before the entity sweep, with splash (`sweepFlight`, `tests/test_fa_navy.cpp:"C-169: a shell crossing the waterline dies as a Water impact with splash"`, `[fa-navy]` 14 cases green, full ctest 2207/2207 green). **Implemented 2026-10-15 (C-169 exit crossing):** the surface block fires both directions — an upward broach (below-to-at-or-above) raises `IMPACT_Water` at the same plane fraction as the downward entry, per retail's `OnEnterWater`/`OnExitWater` pair (`sweepFlight` bidirectional delta test, `tests/test_fa_navy.cpp:"C-169: a shot climbing out of the waterline dies as a Water impact"`, `[fa-navy]` 15 cases green, full ctest 2226/2226 green). |
| [`FA-WEAPONS`](#fa-weapons---targeting-weapons-and-projectiles) | Targeting, weapons, projectiles | `WP-27`-`28` | 100% | 85% | 100% | Universal leading in (`55c31de`, `06390d1`) — radar keeps its error; manual silo-launch orders landed under FA-MISSILES; the C-157 engineer reclaim/capture target exemption is wired (`IsTargetExempt` deferred). **Implemented 2026-10-15 (C-157 guard/attack-move threading):** the own-side reclaim/capture exemption now reaches every automatic-acquisition pass — guard ladder (`guardAttackTarget`), attack-move/patrol (`updateAggressiveOrders`), and the assistance prepass (`guardAllowsBuildAssistance`) — from one per-tick `commandClaims` derivation in `Skirmish.cpp` (`collectUnitWorkClaims` + `collectCaptureClaims` before dispatch, shared by aim/fire); new `guardAttackTarget` C-157 case in `tests/test_combat.cpp`. Serial ctest 2209/2209 green. **Implemented 2026-10-16 (C-091 incumbent class gate):** a challenger whose reach class is worse than the incumbent\u2019s is dropped outright in `nearestTarget` before row/score compare (`0x005de9f9`-`0x005dea7f`) — `Candidate` carries its `ReachClass`, the gate is incumbent-relative (fresh challengers still compare by row then score); new C-091 case in `tests/test_combat.cpp` (focused green; serial ctest green for this slice, 3 unrelated sibling-test failures triaged to their owners). |
| [`FA-TRANSPORT`](#fa-transport---attachments-cargo-and-ferries) | Attachments, cargo, ferries | `WP-25` | 75% | 48% | 100% | Full cargo stack in and retail-validated: capacity/attach-cost from shipped blueprints (UEA0107: 10 slots, class2=2, class3=4), load/unload, ferry, auto-embark, carrier-death (`c538105`). Attach-bone retail observation landed (`2c3e7f8`, C-198): cargo hangs from Attachpoint bones. **Implemented (C-199 waiting ring):** `ferryStagingSpot` stages in-ring waiters onto the carrier-anchored Guard ring (`0x005eca10`, `GuardFormationName` parsed, engine default Guard `sizeMult=3`) before boarding range is reached — staged waiters route to per-waiter ring spots in slot order instead of piling onto the hull. **Implemented (C-458 single-unit fast path):** `detachCargo` lands a lone passenger at the keel (`0x60 == 0x64` fast path) while multi-unit holds fan out in slot/attach order with per-slot landing (`tests/test_fa_transport.cpp:"C-458: unload lands in slot order, single cargo at the keel"`, `[fa-transport]` 233 assertions / 17 cases green; full ctest 2206/2207 — the one failure is a sibling-owned selection paging case that passes in isolation). Next is the unload beacon's exact retail semantics vs the queue-head getter (`C-413`). |
| [`FA-MISSILES`](#fa-missiles---silos-missiles-and-interception) | Silos, missiles, interception | `WP-29` | 100% | 50% | 100% | Manual missile-launch orders in with UI wiring and tests (`8d33735`); interceptor lead landed earlier; the silo build queue with assist links is in (`f41445e`). Strategic-nuke silos now launch — `siloLaunched()` asked `damage > 0` and a nuke's authored `Damage` is 0 (the warhead is `NukeInner/OuterRingDamage`), so the gate now asks `harmful()`. C-171 zig-zag weave implemented: `MaxZigZag`/`ZigZagFrequency` parsed, serial re-roll from the match stream, tapered triangle oscillation. The returned-missile friendly-fire channel is in: `redirectMissile` flips `CollideFriendly`/`DamageFriendly`/`DamageSelf` (the Lua's RedirectingState), the sweep admits the source side, and the impact path damages it including the launcher — SaveState v39 carries the flag and the weave schedule. Redirect cooldown landed: `redirectCooldownTicks` applies the `faWaitSeconds` bias at import (URL0303 1/sec = 11 ticks at 10 Hz; non-positive rate = no wait). **Implemented 2026-10-15 (C-424 retarget cadence):** the failed-acquire reschedule is `max(1, round(TargetCheckInterval x rate))` per `0x006B3AF0` (`frndint` round-half-up, max-1 clamp, sole caller the acquire tick) — the old `ceil + 1` over-waited by a tick (URL0107 0.16 s scans every 2 ticks, not 3) — via `rate.ticks()` at the catalog boundary (`UnitCatalog.cpp`, `tests/test_combat.cpp:"a failed target scan reschedules by round(TargetCheckInterval x rate)"`; focused + [interception][targeting] green, serial ctest green modulo contention flakes green in isolation). **Implemented 2026-10-16 (C-533/C-534 ammo-cheat + count surface):** `giveSiloAmmo`/`removeSiloAmmo` share the one guard (resolve record, silent no-op on null/non-silo, bare add with no clamp at capacity, remove negates with no floor at zero), `siloStoredCount` is the per-slot indexed int load (zero when no record), `stopSiloBuild` drops the owner's queued builds and resets half-funded head progress while keeping stored rounds, `siloBuildCount` walks the queue per owner (`Economy.hpp/.cpp`, `tests/test_silo_queue.cpp`: 6 new cases, 23 new assertions; [silo][queue] 17 cases green, related missile tags green). Serial ctest: all green except two siblings' in-flight failures (ImplDam10's shield-penetration test, platoon flake passing in isolation). Remaining: `0x005DEAD0` name. |
| [`FA-DAMAGE`](#fa-damage---damage-death-and-shields) | Damage, death, shields | `WP-30`-`32` | 100% | 55% | 100% | Ordinary bubbles and personal UnitShield boxes share the collidable shield path; `CollisionCenter*`/`CollisionSize*` drive box containment and sweeps. The shield script-bit toggle is in (`C-350`): off stops absorbing and regenerating while keeping `OffHealth`; on gates absorption for the recharge time and resumes at kept health (`rechargeRestoresFull`, SaveState v36). **Implemented 2026-10-14 (C-396/C-428 shield-handicap half):** per-shield absorbed value now includes the owner's-army handicap divide `/(h+1)` alongside the owner's armour multiplier — retail runs each shield through `DealDamage` (`0x0073dbc0`), so a handicapped owner's bubble absorbs divided damage (`Combat.cpp`, `tests/test_shield.cpp:"a handicapped owner's bubble absorbs divided damage"`; focused shield/bubble 39/39 green, full ctest 2144/2144 green). Remaining: DamageFriendly death-blast/redirect channel and `C-397` WP-32 script-policy bound. |
| [`FA-INTEL`](#fa-intel---vision-radar-sonar-omni-cloak-stealth-jamming) | Vision, radar, sonar, omni, cloak, stealth, jamming | `WP-33` | 50% | 35% | 100% | **Analyzed 2026-09-18 (C-276–C-285):** no temporal blip expiry; stale contacts die by confirmed-dead/ally/last-reference reaping. Counter-intel is a post-coverage flag filter (omni unmaskable; cloak anti-vision; stealth anti-radar/sonar; fields projected as grids). Sonar detects submerged, water-vision identifies. Jammer fakes are fixed random offsets that track the jammer. Recon DBs are per-army, intel sharing is flag-level ally OR. Lua `OnIntelChange` and `OnDetectedBy` events mapped; `IntelWatchThread` disables intel on brownout. The intel/stealth/cloak script-bit toggles are in (`C-350`): the placement cache keys on the script-bit mask, so a toggle withdraws senses and fields without waiting for the emitter to move. **Implemented 2026-10-14 (C-277 blip-parity closeout):** `automaticTrackPosition` lays a reloading turret at the same deterministic blip the shot will use (radar/sonar; truth for sightings/explicit attacks), sonar muzzles aim at the blip (`Combat.cpp`, `test_combat.cpp` turret + sonar-aim tests green; full ctest 2126/2126 green). **Implemented 2026-10-14 (C-277 blip-parity, bomb-drop):** the automatic `needToComputeBombDrop` release point leads from the deterministic blip for radar/sonar contacts — `blip + velocity × PredictAheadForBombDrop` — truth for sightings/explicit attacks (`Combat.cpp` fire pass, tested in `test_combat.cpp`; focused green, full ctest green modulo concurrent-edit flakes that pass on rerun). **Implemented 2026-10-15 (C-277 blip-parity, guard hold/chase):** the guard `CAiAttackerImpl` hold/chase gap reads the same deterministic blip (`radarBlipPosition`, 96 elmos per `LosHandler.h:322`) a radar/sonar contact acquires and fires at — truth for sightings — so a blip-in-reach guard holds instead of chasing truth (`CommandAdvance.cpp`, `tests/test_command_queue.cpp:"a guard holds fire when a radar contact's blip reads in reach"` failing-then-green; related guard/intel/command suites green). **Implemented 2026-10-15 (C-277 blip-parity, guard chase leg):** the chase `routeUnit` destination is the same blip the hold gap read — truth for sightings — so the route and the gate cannot disagree (`tests/test_command_queue.cpp:"a guard chasing a radar contact routes to its blip, not truth"` failing-then-green; full ctest 2164/2164 green). **Implemented 2026-10-15 (C-277 blip-parity, guard inter-weapon rank):** `guardAttackTarget` ranks its per-weapon winners at the same deterministic blip (`radarBlipPosition`, 96 elmos per `LosHandler.h:322`) each weapon acquired at — truth for sightings — so a two-gun guard chases the blip-nearer prey (`Command.cpp`, `tests/test_command_queue.cpp:"a guard prefers its blip-nearer prey over its truth-nearer one"` failing-then-green; focused green + full ctest 2174/2174 green). **Implemented 2026-10-15 (C-277 blip-parity, attack-move inter-weapon rank):** `updateAggressiveOrders` ranks its per-weapon winners at the same deterministic blip (`radarBlipPosition`, 96 elmos per `LosHandler.h:322`) each weapon acquired at — truth for sightings — so a two-gun attack-move interrupts for the blip-nearer prey (`CommandAdvance.cpp`, `tests/test_command_queue.cpp:"an attack-move prefers its blip-nearer prey over its truth-nearer one"` failing-then-green; focused green + full ctest 2181/2181 green). **Implemented 2026-10-15 (C-277 blip-parity, beam acquisition + endpoint):** beams acquire blip-only contacts like any automatic weapon and lay the endpoint at the same unled deterministic blip acquisition scored at — truth for sightings/explicit attacks — so radar-only targets are no longer immune to automatic beam fire; hull-aim and fire-pass both route through `automaticProjectileAimPosition` with a no-lead beam branch, damage still lands on the hull directly (`Combat.cpp`, `tests/test_combat.cpp:"a beam acquires a radar-only contact and lays its endpoint at the blip"` failing-then-green; focused blip suite 42 cases green, serial ctest 2212/2212 green). |
| [`FA-PROGRESS`](#fa-progress---enhancements-veterancy-and-special-units) | Enhancements, veterancy, special units | `WP-34`-`36` | 75% | 35% | 100% | **Analyzed 2026-09-18 (C-251–C-265, C-376–C-381):** WP-34 fully specified — `EnhanceTask.lua` state machine (Stopping→Enhancing→Done), `GetResourceConsumed`-driven progress, `GetConstructEconomyModel` cost, slot/prereq rules, UI replace = two `UNITCOMMAND_Script` clear=true commands (`XxxRemove` then new) → `ClearCommandQueue` `0x006f4d50`, cancellation = `OnWorkFail` no refund, registry = `SimUnitEnhancements` in schook `SimSync.lua`, mutation = per-script `CreateEnhancement` overrides. WP-35 closed: `OnBrainUnitVeterancyLevel` scenario triggers only, Regen buffs overwrite direct `SetRegenRate`, zero native veterancy surface. WP-36: all experimental abilities shipped Lua — GC claw is `TractorThread` slider+`AttachBoneTo`+`Kill` (native-drag refuted), Megalith eggs via `CConstructionEggUnit`, Paragon deficit loop, Ythotha→Othuy spawn, Salem transform, Scathis rotators. **Implemented:** `C-436` milestone ratchet — `EnhancementProgress` events at 0.25/0.5/0.75 edge-triggered on the pre-tick fraction (funded ticks only, finish is the install not a fourth milestone; `Enhancement.cpp`, tested in `test_fa_progress.cpp`). **Implemented 2026-10-14 (C-450 mod-compat half):** milestones carry the `OnBuildProgress(oldProg,newProg)` 3-arg shape — `Event.progressBefore/After` hold the pre/post-tick fractions (`Unit.lua:1731`, `EnhanceTask.lua:66`) and the FAF bridge dispatches `unit:OnBuildProgress` on the enhanced unit's script (base no-op, mods override; tested in `test_fa_progress.cpp` + `test_faf_ai.cpp`). **C-450 saturation/NaN closed 2026-10-14 (no-sentinel ruling):** retail's zero-demand→0.0 feeds only the `obtained > 0` funded guard, already replicated Lua-side; the multiplier sites (intel/shields, `C-161`/`C-071`) keep 1 as the neutral element and a fixed-point NaN would poison them — current shape accepted, no further work. **Implemented 2026-10-14 (C-252/C-454 AIRESULT):** the native script-task dispatcher writes the `SetAIResult` outcome (`0x0062a040`) into the task state's `aiResult` cell — Done reports Success (1), Abort Fail (2), untouched stays Unknown (0) — visible to `OnDestroy` and save/load (`CommandAdvance.cpp`, tested in `test_script_task.cpp`).  **Implemented 2026-10-15 (C-255 build-tray half):** `gatherBuildOptions` appends each installed enhancement's `BuildableCategoryAdds` term to the menu query and gates above-T1 commander products on an installed enhancement covering the entry's categories — unenhanced ACU keeps the authored T1 tray, enhanced gains the unlocked tier (`Interface.cpp`, tested in `test_build_options.cpp`; focused [ui][build] 58/58 green, full ctest 2155/2155 green on a clean rebuild). **Implemented 2026-10-15 (C-255/C-379 toggle-cap half):** the `ToggleScriptBit` intake reads live caps via `unitHasToggleCap`, so a `Shield`-granted `RULEUTC_ShieldToggle` lands and `ShieldRemove` closes the gate again (`CommandApply.cpp`, tested in `test_script_bits.cpp`; focused 44/44 green with 1 skip, full ctest 2164/2164 green). **Implemented 2026-10-15 (C-437 single-step half):** `creditKill` checks only the NEXT threshold — one kill promotes at most one level (`CheckVeteranLevel`, `Unit.lua:3131-3151`, single `if`); a degenerate {1,1,1,1,1} ladder steps 0→1→2 over two kills instead of jumping to 5 (`Veterancy.cpp`, `Veterancy.hpp` doc notes the `AddKills` loop as the batch shape; tested in `test_veterancy.cpp`; promotion suites green, full ctest 2174/2174 green). **Implemented 2026-10-15 (C-437 batch half):** `grantScriptedKills` is the `AddKills` loop (`Unit.lua:3084-3106`) — script-only batch grant that climbs every cleared threshold, one shared `promoteOneLevel` per level (regen erase, blueprint-base max recompute with heal-by-increase, brain event last); the same degenerate ladder lands at 5 on one grant (`Veterancy.cpp/.hpp`, tested in `test_veterancy.cpp`; focused 7/7 green, full ctest 2181/2181 green). | **Implemented 2026-10-15 (C-257/C-380 unit-callback half):** `UnitVeteranPromoted` dispatches `DoUnitCallbacks('OnVeteran')` on the promoted unit after the brain hook (`Unit.lua:3190-3191` order); see section. **Analyzed 2026-09-18 (C-251–C-265, C-376–C-381):** WP-34 fully specified — `EnhanceTask.lua` state machine (Stopping→Enhancing→Done), `GetResourceConsumed`-driven progress, `GetConstructEconomyModel` cost, slot/prereq rules, UI replace = two `UNITCOMMAND_Script` clear=true commands (`XxxRemove` then new) → `ClearCommandQueue` `0x006f4d50`, cancellation = `OnWorkFail` no refund, registry = `SimUnitEnhancements` in schook `SimSync.lua`, mutation = per-script `CreateEnhancement` overrides. WP-35 closed: `OnBrainUnitVeterancyLevel` scenario triggers only, Regen buffs overwrite direct `SetRegenRate`, zero native veterancy surface. WP-36: all experimental abilities shipped Lua — GC claw is `TractorThread` slider+`AttachBoneTo`+`Kill` (native-drag refuted), Megalith eggs via `CConstructionEggUnit`, Paragon deficit loop, Ythotha→Othuy spawn, Salem transform, Scathis rotators. **Implemented:** `C-436` milestone ratchet — `EnhancementProgress` events at 0.25/0.5/0.75 edge-triggered on the pre-tick fraction (funded ticks only, finish is the install not a fourth milestone; `Enhancement.cpp`, tested in `test_fa_progress.cpp`). **Implemented 2026-10-14 (C-450 mod-compat half):** milestones carry the `OnBuildProgress(oldProg,newProg)` 3-arg shape — `Event.progressBefore/After` hold the pre/post-tick fractions (`Unit.lua:1731`, `EnhanceTask.lua:66`) and the FAF bridge dispatches `unit:OnBuildProgress` on the enhanced unit's script (base no-op, mods override; tested in `test_fa_progress.cpp` + `test_faf_ai.cpp`). **C-450 saturation/NaN closed 2026-10-14 (no-sentinel ruling):** retail's zero-demand→0.0 feeds only the `obtained > 0` funded guard, already replicated Lua-side; the multiplier sites (intel/shields, `C-161`/`C-071`) keep 1 as the neutral element and a fixed-point NaN would poison them — current shape accepted, no further work. **Implemented 2026-10-14 (C-252/C-454 AIRESULT):** the native script-task dispatcher writes the `SetAIResult` outcome (`0x0062a040`) into the task state's `aiResult` cell — Done reports Success (1), Abort Fail (2), untouched stays Unknown (0) — visible to `OnDestroy` and save/load (`CommandAdvance.cpp`, tested in `test_script_task.cpp`).  **Implemented 2026-10-15 (C-255 build-tray half):** `gatherBuildOptions` appends each installed enhancement's `BuildableCategoryAdds` term to the menu query and gates above-T1 commander products on an installed enhancement covering the entry's categories — unenhanced ACU keeps the authored T1 tray, enhanced gains the unlocked tier (`Interface.cpp`, tested in `test_build_options.cpp`; focused [ui][build] 58/58 green, full ctest 2155/2155 green on a clean rebuild). **Implemented 2026-10-15 (C-255/C-379 toggle-cap half):** the `ToggleScriptBit` intake reads live caps via `unitHasToggleCap`, so a `Shield`-granted `RULEUTC_ShieldToggle` lands and `ShieldRemove` closes the gate again (`CommandApply.cpp`, tested in `test_script_bits.cpp`; focused 44/44 green with 1 skip, full ctest 2164/2164 green). **Implemented 2026-10-15 (C-437 single-step half):** `creditKill` checks only the NEXT threshold — one kill promotes at most one level (`CheckVeteranLevel`, `Unit.lua:3131-3151`, single `if`); a degenerate {1,1,1,1,1} ladder steps 0→1→2 over two kills instead of jumping to 5 (`Veterancy.cpp`, `Veterancy.hpp` doc notes the `AddKills` loop as the batch shape; tested in `test_veterancy.cpp`; promotion suites green, full ctest 2174/2174 green). **Implemented 2026-10-15 (C-437 batch half):** `grantScriptedKills` is the `AddKills` loop (`Unit.lua:3084-3106`) — script-only batch grant that climbs every cleared threshold, one shared `promoteOneLevel` per level (regen erase, blueprint-base max recompute with heal-by-increase, brain event last); the same degenerate ladder lands at 5 on one grant (`Veterancy.cpp/.hpp`, tested in `test_veterancy.cpp`; focused 7/7 green, full ctest 2181/2181 green). **Implemented 2026-10-15 (C-257/C-380 registry half):** `DoUnitCallbacks('OnVeteran')` walks the unit's `EventCallbacks.OnVeteran` registry — the adapter grows `AddUnitCallback`/`RemoveCallback` on the unit metatable and the `UnitVeteranPromoted` dispatch runs the list after the brain hook (`FafOpponent.cpp`, tested in `test_faf_ai.cpp`; focused 104 green, full ctest 2236/2236 green barring the pre-existing platoon-orders flake). |
| [`FA-TERRAIN`](#fa-terrain---mutable-terrain-and-craters) | Mutable terrain and craters | `WP-37` | 75% | 15% | 100% | **Analyzed 2026-09-18 (C-286–C-292):** `FlattenMapRect` writes uniform u16 elevation, dirty render heightfield, re-seats Land/Seabed entities — no pathing/ogrid. Wrecks use footprint-less `DefaultWreckage_prop.bp` and never block; `Physics.BlockPath` is a dead key. Terrain types gate pathing only via `Blocking` flag; `Slippery`/`Bumpiness`/`HealthEffectPerSecond` are dead. Scorch is visual-only, 28 corpus files, gated on `layer=='Land'`/`targetType=='Terrain'|'Prop'`, no underwater. **Implemented:** non-lethal impact scorch via `impactMarks` on `ProjectileImpact` Terrain/Prop events (C-291/C-292 tested), wreck marks as permanent `Feature` records, no height/type mutation on damage. **Implemented 2026-10-14 (C-290 size axis):** blast-sized impact scorch — `Event.blastRadiusElmos` carries the shot's damage radius from `resolvePendingImpact` through `noteImpactMark` to the decal projection, drawn 1:1 over the blast area (legacy 2.5-elmo mark for blast-less direct hits; hash-neutral, water/unit gates unchanged). **Implemented 2026-10-14 (C-290 lifetime axis):** marks carry their app tick and each retires at its own bit-derived point inside retail's 300–600 s scorch spread — 300 s floor plus a 0..300 s position-bit offset (splitmix32, no sim RNG; replay-identical; hash-neutral, wreck marks stay permanent). Implemented 2026-10-14 (C-290 rotation axis): per-mark 0..2pi fan angle from position bits (splitmix32, no sim RNG; replay-identical). **Implemented 2026-10-15 (C-290 size split):** the projection applies retail's splat/decal scale split — blast radius in elmos recovers the explosion scale through 8 elmos per ogrid, scale above 1.2 draws ×3, otherwise ×4 (8 elmos draws 32, 24 elmos draws 72; `tests/test_ground_decals.cpp:"impact marks cover the blast that made them"`). **Implemented 2026-10-15 (C-290 texture axis):** `ScorchDecalTextures` (`defaultexplosions.lua:234-244`) holds the same ten `scorch_001`-`010_albedo` names as the splat list, not two — `kImpactMarkDecalTextures` 2->10, both scales pick 0..9 (`tests/test_ground_decals.cpp:"impact marks record retail's texture pick"`). **Implemented 2026-10-15 (C-290 LOD axis):** both scorch paths pass `GetRandomFloat(200,350)` as the LOD arg (`defaultexplosions.lua:211,215`) — each mark records its own 200..350 pick from position bits (splitmix32 past the texture stream, no sim RNG per C-297, replay-identical; `tests/test_ground_decals.cpp:"impact marks record retail's LOD pick"`). **Implemented 2026-10-15 (C-484 scripted-decal half):** marks carry an ImpactMarkScript tag -- base scorch keeps the blast split, four scripted values draw their corpus-authored texture and size (sonance crater_radial01 pair at 12 elmos AIFSonanceShell02_script.lua:19-20, proton scorch_011_albedo at 10 CIFArtilleryProton01_script.lua:15, napalm scorch_001_albedo at 3.75-5 midpoint TIFNapalmCarpetBomb01_script.lua:13, EMP nuke_scorch_001_albedo at 28 CIFEMPFluxWarhead02_script.lua:116; texture stream 10-13 past the base 0-9; tests/test_ground_decals.cpp impact marks keep a weapon own decal -- sonance draws 12 not 32, replay-identical, hash-neutral). **Implemented 2026-10-15 (C-290 beam-scorch half):** `noteBeamScorch` notes one blast-less base-splat mark per `BeamFired` event at `event.at` with the event tick — retail's `ScorchThread` (`defaultcollisionbeams.lua`, 8 of 16 classes fork on `impactType == 'Terrain'`, thread lays `CreateSplat` at the tracked end while dwelling); the `Match.cpp` drain wires it with the C-292 water gate from the tick's sim terrain (waterless field never refuses — the gate is water, not height); replay-identical, off the sim RNG (C-297), hash-neutral (`tests/test_fa_terrain.cpp:"C-290: a beam striking terrain leaves scorch at the strike point"` — unit-level terrain-marks/water-refuses + end-to-end turreted beam tank vs LAND victim through `advanceMatch`; focused `[fa-terrain]+[decals]` green; serial ctest 2188/2189, sole failure pre-existing `CPlatoon combat verbs` on clean HEAD per ImplProg11). Remaining: wiring the per-beam-class opt-out into the `Match.cpp` drain (needs weapon→beam-class mapping — the helper defaults unmapped to scorch) and dwell-trail marks along the tracked end (needs per-tick beam-end positions). **Implemented 2026-10-15 (C-290 beam-class opt-out):** `beamClassScorches` (`src/app/Scene.cpp`) — the 5 scorch-less leaves return false, the 8 thread-forking classes + 2 inheriting subclasses + unknown names scorch; `noteBeamScorch` takes an optional beam-class name (`tests/test_fa_terrain.cpp:"C-290: only ScorchThread beam classes scorch"`; focused `[fa-terrain]` 75 assertions/8 cases green; serial ctest 2236/2237, sole failure pre-existing `CPlatoon combat verbs`). |
| [`FA-AI`](#fa-ai---retail-ai-and-native-manager-boundary) | Retail AI and native manager boundary | `WP-38` | 50% | 10% | 100% | **Analyzed 2026-09-18 (C-352–C-362):** full native AI object model enumerated — `CAiBrain`/`CPlatoon`/`CAiPersonality` script objects + per-unit attacker/navigator/builder/transport/silo/steering impls + per-army `CAiReconDBImpl`/`CInfluenceMap`; all managers pure Lua (no RTTI). `CArmyImpl` owns the stack (`+0x1ec` brain … `+0x21c` path queue; vtable[6]=GetInfluenceMap, vtable[7]=GetBrain). Threat grid: `float[14]` `SThreat` per army per cell, per-blip weighted `*ThreatLevel` contribution, 30-tick per-army decay stagger, region queries sum cells. Brain threads in three unnamed per-brain `CTaskStage`s; `ForkThread` links `CLuaTask` onto the object's Lua state. AIx cheats are Lua buffs; callback surface enumerated; `+0x38` is current-enemy `SimArmy*`. **Implemented:** air-scout mid-route flyby release — the area drops within 125 elmos of the flyby destination or 25 of the marker (platoon.lua AirScoutingAI, tested) — on top of must-scout checkout, unknown-threat queues, continuous flybys, High/LowPriority lists. **Implemented 2026-10-14:** the `DoAirScoutVecs` destination clamps 5 elmos inside the map edge (platoon.lua:1264-1267, tested) — the 40-elmo intel margin was the wrong clamp. **Implemented 2026-10-14 (C-418):** the f[9] Artillery slot needs the ARTILLERY+STRATEGIC conjunction — a Mavor writes it, a mobile artillery piece does not (`ThreatGrid::addBlip`, `ThreatBlip::isStrategic`, tested). **Implemented 2026-10-15 (C-453):** `Unit:GetAttacker` (`0x006cb910`) — the per-unit attacker leaf bound to its owner with `GetUnit` back-link and zero weapon leaves until snapshot units carry authored armament (`FafOpponent.cpp` `unitMeta`/`attackerMeta`, tested). **Implemented 2026-10-15 (C-453):** `ScriptedDecal:SetTexture` (`0x00884fe0`) — the second native leaf, a UI-layer texture field on the scripted decal handle (`moho.userDecal_methods`); last write wins, sim untouched (`FafOpponent.cpp` `decalMeta`/`__rm_faf.decal`, tested). **Implemented 2026-10-15:** `Unit:IsIdleState` (`0x006cc5a0`, idle state at `Unit+0x4B4`) — reads the snapshot `idle` flag the scouting plans poll every pass (platoon.lua AirScoutingAI/LandScoutingAI, tested). **Implemented 2026-10-15:** `Unit:TestToggleCaps` (engine `Sim/Unit.lua:710`) — answers from the authored per-type `General.ToggleCaps` set (`__rm_faf.toggleCaps` via `__rm_faf_type`, match-side `teachType` from `UnitDef::toggleCaps`); FAF's scouting loop gates the cloak enable on it (platoon.lua:1293 `RULEUTC_CloakToggle`, tested). **Implemented 2026-10-15:** `CPlatoon` order verbs return a command token `IsCommandsActive` polls — the token reads active while its orders sit queued and inactive once the pass drain consumes them; a non-queued issue retires earlier tokens (platoon.lua:268 `CheckCommandsCompleted`, tested). **Implemented 2026-10-15:** `CPlatoon` combat verbs `AttackTarget` (`0x007365e0`), `GuardTarget` (`0x00737b20`), `MoveToTarget` (`0x007368b0`), `Patrol` (`0x00737d70`), `CanAttackTarget` (`0x00736210`) — one pending order per live unit through the same `platoonIssue` queue/token, drained as `attack_target`/`guard`/`patrol` port decisions into native `Attack`/`Guard`/`Patrol` commands via new `Decision::Kind::Attack/Patrol` (`FafOpponent.cpp`, `Opponent.hpp`, `Match.cpp`, tested). |
| [`FA-UI`](#fa-ui---player-interface-and-advanced-controls) | Player interface and advanced controls | `WP-39`-`40` | 75% | 5% | 100% | **Analyzed 2026-09-18 (C-329–C-346, C-363–C-369):** every UI→sim mutation crosses the `0x112626c` command sink — `CMarshaller`'s 24-message vtable is the complete wire alphabet (`ProcessInfoPair` named commands, `IssueCommand`/`IssueFactoryCommand` UNITCOMMAND records, `LuaSimCallback`). Full RULEUCC→UNITCOMMAND map recovered (`0x827af0`); silo-build types 5/6 rewrite to named `("add","SiloBuildTactical|Nuke",bpid)`; `ProcessInfo` is an unfiltered forwarder; `UI_SelectByCategory` flags = `+add +nearest +idle +inview +goto +excludeengineers`; `GetUnitCommandData` = unioned CommandCaps(0-22)/ToggleCaps(0-8)/Categories+Buildable. **Implemented same day:** the five backed toggles (shield/jamming/intel/stealth/cloak) issue `ToggleScriptBit` through the semantic path with last-writer-wins upkeep (`C-350`); weapon/generic/special stay retail-accurate no-ops. |
| [`FA-PRESENT`](#fa-present---rendering-effects-audio-and-lod) | Rendering, effects, audio, LOD | `WP-41`-`42` | 75% | 45% | 100% | **Analyzed 2026-09-18 (C-293–C-304, C-370–C-375):** 11 `IAniManipulator` subclasses + `MotorFallDown` sim-serialized, ticked solely from `Unit::MotionTick`; `CAniPoseBone` flag map (enabled/dirty/visibility/skip-interp); `CCollisionManipulator` fires `OnAnimTerrainCollision`/`OnNotAnimTerrainCollision`/`OnAnimCollision` (footfall into Unit.lua:2289); effects manager at `Sim+0x8C0` with per-army visibility-mask emission gating (`EmitIfVisible`/`CatchupEmit`); LOD is a cached screen-space pixel scale (`min(vpW,vpH) × LODScale × FOV factor`); `SAudioRequest` wire laid out, `CSimSoundManager` at `Sim+0x8C4`, `CUserSoundManager` drains per Frame; `SetPivot` is a dead attached-entity pivot API. |
| [`FA-PERSIST`](#fa-persist---replay-hashing-and-saveresume) | Replay, hashing, save/resume | `WP-43`-`44` | 50% | 30% | 100% | Wreck pool in save v23, in-flight projectiles in v35, and now the path service's queues, cached flow fields and intel recon history in v37 — a mid-order save resumes to an identical hash stream for 300 ticks (tested). SaveState v36 adds the script-bit mask, upkeep flag and shield `rechargeRestoresFull`; command log v7 records the toggle bit. **Implemented 2026-10-15 (C-552/v60):** the intel save names its coverage style (`VisionStyle` byte — a Recoil resume reads Recoil coverage even when configured ForgedAlliance) and the economy save carries the scenario's `playableRect` acquisition bounds (pre-v60 saves keep the caller's setup; tested). **Implemented 2026-10-16 (C-284 fog-hash):** the per-alliance fog-of-war flags feed the state hash, closing the save-covers-it-but-hash-ignored gap (v52 saved them; `LOSNow` granted outright when off) — failing-then-green (`tests/test_state_hash.cpp:"the fog-of-war flag changes the hash"`); focused green, `[fa-persist]` 10 cases green, serial ctest 2245/2247 with two out-of-slice failures (`#182` CPlatoon verbs, untouched FAF file; `#1083` missile-bubble flake passing on rerun). **Implemented 2026-10-16 (C-007 explored-hash):** the per-alliance explored bitmaps feed the state hash via `feedBytes(exploredCells)` — a seer that dies after seeing ground hashes apart from a match that never saw it, closing the same save-covers-it-but-hash-ignored gap (v52 saved them; `[INFERENCE]` fog render reads them) — green (`tests/test_state_hash.cpp:"explored marks change the hash after coverage is gone"`, kills a snapshot copy so only the bitmaps separate the hashes); `[fa-persist]` 11 cases green, `[save-state]` green, neighbor hash cases green; serial ctest 2244/2258 with 14 out-of-slice failures (11 ScriptHost `C-228`/`C-318`/`C-311`/`C-313`/`C-388`/`C-436` from uncommitted sibling work in `src/app/ScriptHost.cpp`, plus `2129`/`2150`/`2164` formation/ferry cases outside this slice). Next: remaining intel grids and emitter state. |


## Historical claim-level test coverage (2026-09-19, superseded)

The table below is retained as a historical audit, **not the current score**;
the 2026-09-23 classification and denominator are at the top of this document.

Every claim in the ledger was re-classified against the test suite (2,030 tests)
after the three gap-fill waves; per-claim detail lives in
`build/re-fa/coverage/FA-*.md` (gitignored). Statuses: **TESTED** a test asserts the
claim's behavior; **PARTIAL** tests pin only part of the specifics; **UNTESTED**
implemented but unpinned; **NOT_IMPL** no implementation; **N/A** analysis-artifact
claims (provenance, layouts, naming) excluded from the percentage. Coverage% =
(TESTED + ½·PARTIAL) / implementable claims. Claims spanning several work packages
count toward each subsystem they touch.

| Subsystem | Tested | Partial | Untested | Not impl | N/A | Coverage |
|---|---:|---:|---:|---:|---:|---:|
| FA-FOUND | 0 | 1 | 0 | 1 | 36 | 25% |
| FA-CONTENT | 8 | 11 | 0 | 1 | 8 | 68% |
| FA-LUA | 14 | 7 | 0 | 1 | 5 | 80% |
| FA-CMD | 15 | 8 | 0 | 0 | 12 | 83% |
| FA-MATCH | 2 | 0 | 0 | 0 | 2 | 100% |
| FA-ECON | 51 | 8 | 0 | 0 | 10 | 93% |
| FA-LAND | 7 | 16 | 0 | 0 | 5 | 65% |
| FA-AIR | 5 | 6 | 0 | 0 | 0 | 73% |
| FA-NAVY | 13 | 6 | 0 | 0 | 1 | 84% |
| FA-WEAPONS | 20 | 0 | 0 | 0 | 13 | 100% |
| FA-TRANSPORT | 7 | 0 | 0 | 0 | 3 | 100% |
| FA-MISSILES | 15 | 0 | 0 | 0 | 1 | 100% |
| FA-DAMAGE | 11 | 0 | 0 | 0 | 17 | 100% |
| FA-INTEL | 6 | 9 | 0 | 2 | 2 | 62% |
| FA-PROGRESS | 18 | 8 | 0 | 0 | 3 | 85% |
| FA-TERRAIN | 4 | 4 | 0 | 0 | 0 | 75% |
| FA-AI | 6 | 4 | 0 | 0 | 2 | 80% |
| FA-UI | 10 | 13 | 0 | 0 | 2 | 72% |
| FA-PRESENT | 9 | 8 | 0 | 1 | 1 | 72% |
| FA-PERSIST | 7 | 5 | 0 | 1 | 6 | 73% |
| **Total** | **228** | **114** | **0** | **7** | **129** | **~82%** |

Read it as: of ~370 claim-subsystem pairs, 228 are test-pinned and 114 more are
half-pinned — the suite covers roughly 82% of the recovered retail contract after
the 2026-09-19 gap-fill waves. The weakest rows are FA-FOUND (25% — nearly all
claims are provenance artifacts), FA-INTEL (62%), FA-LAND (65%), FA-CONTENT and
FA-DAMAGE (68%). The seven NOT_IMPL: `C-037` (LuaPlus thunk convention — internals),
`C-065`/`C-139` (spatial-chain internals — accepted divergence), `C-078`/`C-276`
(recon cadence / no temporal blip expiry — deliberate divergences), `C-152`
(lockstep beat gating — no multiplayer), `C-267` (VFS first-mount-wins — divergence
reproduced by mount order), `C-302` (animation-signal machinery — no skeleton).

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

**Current slice:** the install is proven Steam app `9420` (`steam_appid.txt`, store record,
Windows-only, Square Enix publisher of record) and `ART-E001`'s SHA-256 was recomputed
2026-09-10 against the local exe (match; file version `1.5.0.1`, PE `2011-08-29`). The Steam
depot/build manifest is bound in the exe plan's new `WP-00` subsection: depots 9421–9425
(game, maps, movies, sound, English) at build 2845 with transcribed manifest IDs, an
install-directory to depot inference map, and reproduction via `download_depot`.

**Largest gap:** the manifest IDs are SteamDB-transcribed, not confirmed against Steam
(no direct access from here); the single promoting check is comparing `ART-E001`'s hash
against depot 9421's file list. Transfer history (GPG → Steam → THQ Nordic) unconsulted.

```text
/goal Advance FA-FOUND by confirming the transcribed manifest against Steam (download depot
9421's manifest with an owning account, compare ART-E001's hash), then close WP-00's manifest
half and refresh FA-FOUND.
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

**Audit correction (2026-10-16):** the sentence below is stale. Ascending SCD
mount with first-match-wins lookup is implemented (`Cli.cpp:287-300`,
`Vfs.hpp:115-121`) and tested for the `lua.scd`/`mohodata.scd` collision
(`tests/test_fa_content.cpp:"C-266/C-267: ascending mount order makes lua.scd
shadow mohodata.scd"`). The remaining content work is the stated next task,
not this precedence.


**Implemented 2026-10-15 (C-218 inertia product guard):** `InertiaTensorX/Y/Z`
parse with the native product guard (`0x00518C8B`) — one zeroed axis discards
the other two authored values and all three derive `(1/12)(a²+b²)` box inertia
from root `SizeX/Y/Z`; absence reads exactly like explicit zero (0 of 568
shipped units author any axis). Suite green.

**Implemented 2026-10-15 (C-218 AltFootprint overwrite):** the entire
`AltFootprint` struct is overwritten by `Footprint` whenever
`Physics.AltMotionType` is absent (`0x00525D77`) — the alt table survives
only with the alternate motion class; without it retail reads the regular
footprint twice. Suite green.
```text
/goal Advance FA-CONTENT by tracing retail SCD mount and override precedence from ART-E001 and the
owned archive corpus, recording exact locators and counterexamples, then adding a discriminating
VFS precedence test and fixing Recoil Metal only if it differs. Update WP-05 and the FA-CONTENT
dashboard row.
```

### FA-LUA - Gameplay Lua And Mod Contract

**Audit correction (2026-10-16):** the sentence below is stale. `ScriptHost`
already wires `doscript`/`import` hook concat and publishes `__active_mods`
(`ScriptHost.cpp:141-154,464-502,669-715`), and the `EnhanceTask` adapter runs
the shipped-shape lifecycle (`Enhancement.cpp:577-630`). The remaining Lua work
is not these adapters: the full scheduler (`ForkThread`/`WaitFor`, two-ring
task FIFO, `OnPreCreate`/`OnCreate`/`OnDestroy` ordering for arbitrary gameplay
scripts), hosting shipped unit/projectile scripts, and the general mod surface.

**Implemented 2026-10-15 (C-228 task natives):** `ScriptHost` binds the four
`CUnitScriptTask` task natives (`GetUnit`, `SetAIResult`, latent `LUnitMove` /
`LUnitMoveNear`, `0x0062a1b0`…`0x0062a040`) on the class (inherited via
`__index`) and the instance, with an empty `commandData` table so
`self.CommandData.X` reads nil; the shipped-shape `EnhanceTask` now runs
`OnCreate` → `Stopping` → `Enhancing` through `ChangeState`. Stack discipline
is exact everywhere: error exits normalize with `lua_settop(0)`, post-pcall
pops match `nresults`, and the natives-table write pins its slot with
`lua_absindex`. Suite 2190/2190 green.
```text
/goal Advance FA-LUA by hosting the retail unit/projectile gameplay scripts in a LuaPlus 5.0.1
interpreter with the recovered `CTaskStage` two-ring FIFO, `WaitTicks(n)` `n-1` resume,
`ForkThread`/`WaitFor` suspend/resume, script-class resolution, and `OnPreCreate`/`OnCreate`/
`OnDestroy` order. Wire `doscript`/`import` hook concat and `__active_mods` from
`gameInfo.GameMods`; keep `shadow/` unimplemented and no sim/UI sandbox. Run make test and make
verify, then refresh WP-07/08 and FA-LUA.
```

### FA-MATCH - Armies, Setup, And Victory Rules

**Largest gap:** WP-11 is now fully implemented on the analyzed contract. The
scenario Options contract is wired end to end — `resolveScmap` reads
`_scenario.lua` into `LoadedMap::scenarioOptions`, `makeMatchRunner` maps
`Options.Victory` through `victory.lua`'s own keys, `FogOfWar='none'` leaves
intel unconfigured, `UnitCap` gates creation (`C-383`: factory holds and
retries, scaffolds/upgrades refuse, `UnitCap_*` feeds `CArmyStats`), and the
starting bank is the ACU's own `Economy.Storage*` granted on the fifth beat
(`C-384`: `GiveInitialResources`, `WaitTicks(5)` `n−1` quirk; commander storage
counts toward capacity, so a dead ACU drops the ceiling and the bank).
`InitialMass`/`InitialEnergy` proved dead options — parsed at `0x8e7fef`, no
consumer in binary or corpus. Draw offers are in (`C-382`), the 15-second
winner stability window tracks the survivor SET (`pendingSurvivorMask`), and
the 20-second post-defeat cleanup is already in. **2026-10-14 (C-210 civilian
arm):** `Army::civilian` marks `NEUTRAL_CIVILIAN`, skipped in every poll arm —
never defeated, never a survivor, never the verdict (`tests/test_fa_match.cpp:
"C-210: a civilian army stands outside the victory verdict"`).

```text
/goal Advance FA-MATCH on the retail-validation axis: the WP-11 contract is
fully implemented — what remains is proving it against the executable. The
open questions are behavioural, not structural: does the winner-stability
window's survivor-set tracking match retail's `victory.lua` poll under
mid-window defeats, and does the cap gate's `OnUnitCapLimitReached` callback
surface anywhere the sim should mirror. Run make test and make verify, and
refresh FA-MATCH.
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
Assist is now reachable from the rack and the right-click ladder prefers it over Repair when the
target owns an unfinished construction; the production panel shows the lent rate (ADR-108). The
AUTO MEX cell is an app-level standing order that issues ordinary logged builds (ADR-109).
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
`make test-content` runs every `[corpus][guard]` case with no skips. Staging pads now
refuel bingo-fuel guards — the first `C-183` refuel rung (`9768d4f`, tested). The ferry
rung is now in too: a transport guarding a `FERRYBEACON` flies the beacon's route —
pickup at the beacon, drop at the beacon's own command target — on the same phase
state a `Ferry` order uses (`[fa-transport]`, tested). The two rungs are now pinned
together in ladder order (`C-183`: refuel/staging outranks ferry): a bingo-fuel guard
ferry diverts to a pad in reach, docks, refuels at the pad's `AI.RefuelingMultiplier`
to a full tank, relaunches with the guard order still heading it, and the ferry drive
 picks the route back up (`tests/test_fa_transport.cpp:"C-183: a bingo-fuel guard ferry
 docks at a staging pad instead of flying dry"`, `[fa-transport]`). **Implemented 2026-10-15
 (C-183 guard-side ferry commit):** a fueled transport guarding a `FERRYBEACON` commits the
 leg toward the beacon's own active command target (Move/Ferry/UnloadTransport) in the same
 dispatch tick — the retail `C-348` caller set (`CUnitFerryTask`/`CUnitWaitForFerryTask`/
 `CUnitCallTransport`/`CUnitCallAirStaging`/`CUnitCallLandTransport`) feeding the guard
 ladder's ferry rung — instead of chasing the beacon with the build-reach hold; a beacon with
 no route command yields as before, and the carrier-side loop stays in `updateTransports`
 (`CommandAdvance.cpp`, `tests/test_command_queue.cpp:"C-183: a fueled transport guarding a
 ferry beacon flies the beacon route"`, `[guard][ferry]`; focused green + full serial ctest
 2207/2207 green). Multi-weapon guard arbitration follows `CAiAttackerImpl`: the longest-range
 weapon whose [minRange, maxRange] envelope covers the gap owns the hold, and the widest
 envelope is the chase/hold fallback (`[guard]`, tested). The `aimAtTargets` call dropped by
 the capture refactor (`0a879ee`) is restored, so unturreted hulls slew onto their aim point
 before the facing gate again. **Implemented 2026-10-15 (C-183/C-348 mid-route staging
 yield):** a bingo-fuel guard ferry yields `advanceGuardFerry` to the staging rung mid-route
 (`Transport.cpp`, `tests/test_fa_transport.cpp`; focused green, serial 2230/2231 with one
 sibling-owned failure).

```text
/goal Advance FA-CMD by specifying the remaining C-183 ferry rung from its retail callers
and implementing the smallest bounded slice that lets a guarded air unit ferry through a
staging platform. Preserve authored capabilities and retained construction identity, and
run make test and make verify before updating FA-CMD.
```

### FA-ECON - Economy, Construction, And Engineering

**Largest gap:** Capture now follows `C-250` end to end — footprint-edge gates
(approach stops at 5 ogrids, work admits out to 10, so a closing captor banks
progress on the way in), the concurrent-captor refcount, funded progress,
transfer identity, cancellation, replay and save/load. Transfer parity now
also carries installed enhancements and the shield on/off toggle
(`SimUtils.lua:68-132`'s `CreateEnhancement`/`EnableShield` restores), plus
attached children end to end (`C-239`, `0x0060B521`-`0x0060B6AA`): a top-level
attached parent is admitted (an attached child still takes no task of its
own), each live child's own tick budget and build energy price into the
total, and the transfer re-hangs the moved children on the replacement
(`tests/test_capture.cpp:"a capture prices an attached child's costs onto
the parent's budget"`). **Implemented 2026-10-16 (C-240 custom-name close):**
the native transfer copies the custom name (`Unit+0x2CC`,
`0x0074E1B1`-`0x0074E1CE`) onto the replacement — per-slot `customNames_`
(`UnitStore::setCustomName`/`customName`, unnamed by default, cleared on
slot recycle), SaveState v62 sparse section, state-hash tag 13 (unnamed
stays silent) — closing `C-240` to TESTED
(`tests/test_capture.cpp:"a captured unit keeps its custom name on the
replacement"`, with a v62 save round-trip; `[capture]` 21 cases /
`[fa-econ]` 17 cases green; full ctest green modulo the pre-existing
`WindowedSession.mm` build failure on clean HEAD, untouched by this
slice). The
[minimum retail Capture specification](capture-implementation-spec.md) corrects
 earlier readings: the captor supplies the cost method, attached target children add
 costs, and `Unit+0x690` counts active capture tasks. **Implemented 2026-10-17
 (fuel transfer pin):** the replacement keeps the victim's remaining `fuelRatio`
 via the whole-motion snapshot
 (`tests/test_capture.cpp:"a capture keeps the target's remaining fuel"`).
Ordinary mobile construction now follows
`C-248`: the active Build order routes the engineer toward the site and creates no construction
until centre distance minus the builder's smaller footprint and target's larger skirt is within
`Economy.MaxBuildDistance`; factory production and upgrades retain their separate immediate paths.
Idle `ENGINEERSTATION` units lend their rate to the nearest allied construction within that same
reach and otherwise repair the nearest damaged ally (ADR-108; `tests/test_assist.cpp`). Structures
snap to the 8-elmo build grid by default, `--placement free` restores exact-click placement (ADR-107).
T1/T2/T3 engineers now offer their authored construction tiers, including shields and
experimentals, while unenhanced ACUs keep the T1 menu until enhancements are modelled;
extractor upgrade chains can be cancelled per tier (`[engineer-tiers]`, `make test-upgrade-ui`).

```text
/goal Extend Capture to general transfer parity: attachments, enhancements, fuel,
ammo and shields on the target. The C-250 approach/admission contract and the
concurrent-captor refcount are in with headless cover.
```

### FA-LAND - Land Navigation, Formations, And Spatial World

**Largest gap:** grouped `Move` and grouped `AttackMove` both fan out through retail
`BlockBuilderLand` slot geometry (`C-178`): `GrowthFormation` widths for travel, `AttackFormation`
widths (8-wide two-row block up to sixteen members, 20-wide past 126) for attack-move, with
canonical UnitId ranks, one shared click anchor on the payload, per-member slot targets, the
existing per-army FIFO path-service budget, and C-176/C-177 retry semantics. All-`surfaceWater`
groups take the naval blocks instead (Growth 3/5/7-wide at 9/25, AttackMove 5/7/9-wide at 10/25 —
2026-10-15); all-`submersible` groups take the sub blocks (Growth 3-wide to 9, 5-wide past;
AttackMove 5/7/9-wide at 10/25 — 2026-10-15). Sea/sub slots spread at retail's call-site
`spacing` 1.5 with the naval blocks' `LineBreak = 0.5` row step (2026-10-15). Airborne groups
fly retail's `BlockBuilderAir` staggered chevron instead (both scripts close with it over the
same `StaggeredChevronBlock` — 2026-10-15): `GetChevronPosition` 0.375-elmo steps alternating
flanks with the per-column block shift and -2/-1 row stagger, x2 spacing with an experimental
aboard. Small land attack blocks now select families using the slot's ordered fallback
list (`DFFirst`, `ArtFirst`, `AAFirst`, `ShieldFirst`, `UtilFirst`;
`formations.lua:91-97,192-197`). A short front can include engineers; construction is
not the radar/scout utility family. Wider land blocks, naval/sub/air category ranks,
HPA*, and reservations remain deferred. Arrival re-forming is NOT deferred work:
retail never re-forms on arrival (`PickBestFinalFormationIndex` returns -1
unconditionally, C-178) — implementing it would be an invention, not parity.

```text
/goal Advance FA-LAND by porting one bounded formation geometry from ART-S007:lua/formations.lua
onto the existing deterministic group-move fan-out. Preserve canonical UnitId ordering, shared
click anchors, per-army FIFO path-service budget, and C-176/C-177 retry semantics. Start with a
focused group-order regression, leave HPA*, reservations, and category matching
explicitly deferred — never arrival re-forming, which retail lacks — run make
test and make verify, then refresh FA-LAND.
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
sequence. Idle aircraft now land where they loiter instead of descending on a
retired order's site (`eaa2470`), staging pads refuel bingo-fuel guards
(`9768d4f`), and a takeoff committed to a hop inside one tick's travel keeps
flying through arrival instead of stranding `Down` on the deck (`c538105`).
Real UEA0102 turn/recovery checkpoints continue with matching per-tick
hashes; the inspected app pursuit replay matches 900 ticks. See ADR-091 for the
planar controller boundary and [unit evidence](unit-capability-matrix.md).

**Largest gap:** the three-axis solver stays out — quaternion pitch and `KRollDamping`'s
roll-rate state. Visual banking is in (planar reduction of
`C-244`'s roll axis: `BankFactor` demand, `KRoll` approach, save v21, tested), and the
cargo mass ratio now divides `KLift`/`KTurn`/`KRoll` by `(own + Σ cargo) / own`
(`unitMass`/`carriedMass` on `MoveState`, maintained by `UnitStore::attach`/`detach`).
Bomb-drop prediction is in too: state 1 aims at the release point
`target + velocity × PredictAheadForBombDrop` and the weapon releases inside
`BombDropThreshold` (`C-224`). Carrier storage is in as well (`C-225`):
`StorageSlots` is parsed, a `CARRIER`/`NAVALCARRIER` stores its air build
products at its origin instead of rolling them off, `(stored + reserved) <
StorageSlots` gates capacity, stored aircraft are hidden from intel, and
`UnloadTransport` on a carrier launches the hold airborne. `Hover`,
`POD`-only random initialization of the otherwise-zero `CUnitMotion+0x9c` elevation adjustment,
and bone-keyed air staging (`C-225`'s pad half) remain.

```text
/goal Finish the current air slice's retail-map and golden acceptance. Attribute the first
hash divergence before considering a new baseline; v16 now hashes the future RNG sequence.
Keep the three-axis solver, cargo inertia and bomb prediction explicitly separate from the implemented
planar states, then choose the next evidenced controller gap and refresh FA-AIR.
```

### FA-NAVY - Surface And Submerged Warfare

**Current slice:** ordinary retail `SurfacingSub` units use the water grid and default to Sub.
The logged Dive/Surface rack command keeps horizontal orders, chooses its destination from the
committed layer, and changes that layer only at the depth endpoint. Depth follows `C-200`'s
sinusoidal rate and seabed clamp; `DiveSurfaceSpeed` uses `C-218`'s authored/default value.
Water/Sub weapon source rows remain distinct, including Tigershark's surface-only plasma gun.
SaveState v18 and state hashes preserve the transition. See ADR-111.
Layer-specific depth footprints are in (`3d607cd`): the submerged layer admits by the
footprint's own `MinWaterDepth` (unauthored on every shipped `SurfacingSub` → just wet)
while the surfaced hull pays its `Elevation` draft, so a diving sub crosses shelves that
ground it surfaced; the pick follows `MoveState::submerged` through orders, movement,
congestion and the match's per-type grid tables. Ships route by draft (`fd3c5a7`).

**Largest gap:** attack-driven auto-surfacing and spawn-layer rules are now
implemented and tested (`C-203`/`C-323`/`C-324`), as are sonar acquisition,
blip-parity aim/track, and the water-gate family (`C-202`/`C-321`/`C-322`/`C-327`).
**Implemented 2026-10-14 (C-170 splash parity):** the area-blast
sphere is layer-agnostic — a surface detonation wets a submerged hull inside
its radius (`damageTargets` sphere-vs-box overlap, no target-layer filter on
the blast path; `tests/test_fa_navy.cpp:"C-170: a surface splash still wets
a submerged hull under the blast"`, `[fa-navy]` 13 cases green, full ctest
2140/2140 green). **Implemented 2026-10-15 (C-169 surface block):** a shell
whose tick segment crosses the water plane raises `IMPACT_Water` at the plane
crossing — before the entity sweep, at the interpolated crossing point — and
the blast still splashes through `resolvePendingImpact`'s ordinary area path
(`sweepFlight` water-plane intersect, `ImpactType::Water`; `stayUnderwater`
torpedoes exempt, `destroyOnWater` shells still die silent per `C-204`;
`tests/test_fa_navy.cpp:"C-169: a shell crossing the waterline dies as a
Water impact with splash"`, `[fa-navy]` 14 cases green, full ctest 2207/2207
green). **Implemented 2026-10-15 (C-169 exit crossing):** the surface block
fires both directions — an upward broach (below-to-at-or-above) raises
`IMPACT_Water` at the same plane fraction as the downward entry, per retail's
`OnEnterWater`/`OnExitWater` pair (`sweepFlight` bidirectional delta test;
`tests/test_fa_navy.cpp:"C-169: a shot climbing out of the waterline dies as a
Water impact"`, `[fa-navy]` 15 cases green, full ctest 2226/2226 green).
Remaining: the `OnEnterWater`/`OnExitWater` Lua hooks (presentation/sound —
`Projectile.lua:429-442`, blueprint `Audio.EnterWater`/`ExitWater`) and the Lua
`OnCollisionCheck` default-deny gate — both need a script surface the sim
does not have.
```text
/goal Extend the tested SurfacingSub slice with retail's water surface tests:
water enter/exit events and the destroy-on-water plane intersect (C-169's
surface block). Keep source-layer weapon tests and dive/save/replay checks
green without headed tests.
```

### FA-TRANSPORT - Attachments, Cargo, And Ferries

**Current slice:** generic attachments capture a deterministic local X/Y/Z offset, suspend child
movement, propagate it parent-before-child after movement and collision, and persist it in
SaveState v7 while v1-v6 derive the local height from saved transforms. Attachments now also
carry `C-195`'s parent/self bone indices with authored rest-bone composition (`C-196`): the
parent bone rides the carrier's heading, the child bone hangs off the child's own heading, the
stored offset is captured bone-relative, and the record is hashed and saved at v22 with gated
old-version reads. Attached children remain in the collision grid. Parent death detaches
surviving children and clears their offsets and bones.
Transport loading is now the full loop (`fef1913`, `5e9fbba`): cargo walks or is flown to a
grounded carrier and slings aboard at blueprint capacity (`TransportClass` slots with
`Class2AttachSize`/`Class3AttachSize`/`ClassGenericUpTo` costs), `UnloadTransport` flies the
hold to a drop point and sets it down, `Ferry` loops beacon→drop→beacon, an unreachable
Move auto-embarks a transport, and attached cargo dies with its carrier. Shipped blueprint
values drive the behavior end-to-end in tests (`c538105`): a real C-6 Courier (UEA0107,
10 slots) fills on class-1/2/3 cargo including a UEF ACU at four slots, the overflow
waits, and a loaded courier delivers. `CANTRANSPORTCOMMANDER` is deliberately unenforced —
the attach-cost table already excludes every shipped carrier lacking the flag.

**Largest gap:** cargo now hangs from the carrier's `Attachpoint*` bones (`C-198`):
the catalog carries per-class bone lists resolved from the mesh, `attachCargo`
places each unit at the nearest free bone of its class, and the attachment is
bone-indexed so cargo swings with the hull (`[fa-transport]`, tested). Generic
cargo pricing corrected to retail's one-slot rule (`ClassGenericUpTo` path).
Waiting ferry cargo now stages onto the carrier-anchored Guard ring (`C-199`,
`ferryStagingSpot`, `AI.GuardFormationName` parsed): in-ring waiters outside
sling reach route to per-waiter ring spots in slot order instead of piling
onto the hull. Unload now follows `C-458`'s slot order: a lone passenger
lands at the keel (the `0x60 == 0x64` single-unit fast path) while a full
hold fans out in attach order with per-slot landing. Still open: the pickup
beacon's exact retail semantics (spawned `*B5102` unit vs order anchor,
`C-413`'s queue-head getter) and transport shields' cargo coverage.

Guard-ferry adapter regression: movement along a beacon's route command keeps the
captured pickup fixed for waiting cargo. Replacing the guard queue head with a
different beacon starts a new pickup phase. The native C-413 queue-head getter
does not establish marker-drag semantics; the speculative live-position refresh
and its API have been removed.
The guard dispatcher now yields movement to the transport phase machine in the
same beat. Unconditionally committing the drop leg before pickup marked the
carrier busy and prevented `commitLeg` from approaching waiting passengers.

```text
/goal Advance FA-TRANSPORT by comparing the implemented load/unload sequence with retail
observation of the pickup beacon: where the ring sits, how the carrier chooses a landing
spot, and how multi-unit loads order their slots. Keep the shipped-capacity suite green,
run make test and make verify, record divergences in WP-25, and refresh FA-TRANSPORT.
```

### FA-WEAPONS - Targeting, Weapons, And Projectiles

**Audit correction (2026-10-16):** the former reclaim/capture-exemption gap is
closed: `Combat.cpp` rejects candidates claimed by an own-side/allied worker,
including incumbents, before `DoNotTarget`. `Combat.hpp::WorkClaim` carries the
derived claim data. This is a bounded implementation, not evidence of complete
retail targeting parity.

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
hashed, and consulted during automatic acquisition. The engineer reclaim/capture
target-exemption rule is wired: `collectUnitWorkClaims`/`collectCaptureClaims` feed the
`C-157` own-side reject in `nearestTarget` (tested — `[combat]` C-157 case, `[capture]` suite).

```text
/goal Advance WP-15 with type-safe unit-target Reclaim/Capture command support before implementing
`C-157`'s engineer `IsTargetExempt` reject. Feature-only reclaim handles cannot safely name a unit;
keep the completed playable-rectangle filter, explicit Attack, candidate caches, and
projectile-defense targeting out of scope. Run make test and make verify, then update WP-15 and
FA-WEAPONS.
```

### FA-MISSILES - Silos, Missiles, And Interception

**Largest gap:** interception accounting and manual launch orders now run (`8d33735`, with
UI wiring and tests), and the silo build queue landed (`f41445e`): retail's per-unit FIFO
(`CAiSiloBuildImpl+0x20`, `C-081`/`C-241`) with auto-refill, manual SiloBuild commands, and
SaveState v33. Strategic-nuke silos now launch too — `siloLaunched()` measured harm by
`damage > 0` alone, and a nuke's authored `Damage` is 0 (its warhead is
`NukeInner/OuterRingDamage`), so every strategic silo was refused at the door; the gate now
asks `harmful()`. Guidance's zig-zag weave is implemented (`C-171`): `MaxZigZag`/
`ZigZagFrequency` parse into the projectile traits, three uniforms re-roll per period from
the match stream (serial, before the fork-joined flight), and a per-axis triangle
oscillation tapered by `min(1, dist/maxZigZag)` displaces the shot and returns it to course
at each boundary. The third `HasSiloAmmo` gate is now named and implemented
(`C-511`/`C-423`): `0x005DEAD0` is a silo-target-acquire helper (sole static caller the true
acquire tick `0x005df7e0`, selected by the `RULEWTT_Projectile` blueprint `+0x130 == 1` gate
per `C-514`) — an empty counted silo does not even scan for incoming missiles, so the
unturtled hull holds its heading for a shot the weapon cannot answer, and the live silo
records ride the `Skirmish.cpp` aim call. Reviewed and pinned this slice against
the Lua (`defaultantiprojectile.lua`, `WaitingState`/`RedirectingState`): the live-launcher
return keeps the `CollideFriendly`/`DamageFriendly`/`DamageSelf` flip (`C-428`'s
`DamageInfo+0x7C` skips the sphere `IsAlly` reject, NOSPLASHDAMAGE still rejecting) —
a returned shot's point hit AND its area blast both admit the source side — while the
null-`GetLauncher` arm takes the 30-damage fallback (`DoTakeDamage`) with course and
flags untouched. Both arms ride the existing `shot.friendlyFire` channel with no src
change. Remaining: whether the redirect cooldown is 10 or 11 ticks.

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
rate cycle, dead owners reaped, SaveState v10. **Acquire-gate follow-up implemented
(`C-511`/`C-423`):** an empty counted silo skips the incoming-missile scan entirely, so the
unturtled hull holds rather than tracking a shot the weapon cannot answer; `siloGateOpen`
is now `const`-correct and shared by aim and fire. **Redirect review follow-up
(`C-088`/`C-428`):** the live-launcher return keeps the Lua's
`CollideFriendly`/`DamageFriendly`/`DamageSelf` flip — point hit and area blast both
admit the source side; the null-`GetLauncher` arm deals the 30-damage Lua fallback with
course and flags untouched (both pinned in `test_combat.cpp`, no src change).
**Named divergences:** adjacency modifier fixed at 1, `--units` crowds unwired;
the cooldown may be 10 or 11 ticks (`WaitSeconds` runs n·10+1).

```text
/goal Advance FA-MISSILES by closing the remaining WP-29 boundaries, with exact
ART-E001 locators and counterevidence: the returned-missile friendly-fire channel
(a redirected shot cannot yet damage its source side) and the third `HasSiloAmmo`
caller at `0x005DEAD0`. Write failing tests first (interception, flare, redirect,
manual-launch and zig-zag behavior stay green), run make test and make verify, and
refresh FA-MISSILES.
```

### FA-DAMAGE - Damage, Death, And Shields

Integration correction: death retirement now checks handle liveness rather than
using collision radius as an "already dead" sentinel. Zero-radius entities must
still emit death events and invalidate their handles. The app match carries the
scene's self-destruct countdown store, exercised through the command sink to the
countdown boundary (`tests/test_fa_ui.cpp`, C-345).

**Largest gap:** the C-143 owner-armour residue is closed — each shield absorbs through its
generator's own armour class plus the owner's-army handicap divide (`C-396`/`C-428`,
`tests/test_shield.cpp:"each shield uses its owner's armour multiplier for absorption"`,
`"a handicapped owner's bubble absorbs divided damage"`). `C-428`'s conjunction is now
pinned too: a `NOSPLASHDAMAGE` ally still rejects a DamageFriendly death blast while an
ordinary ally beside it takes the full amount
(`tests/test_shield.cpp:"a NOSPLASHDAMAGE ally still rejects a friendly death blast"`,
2026-10-15, failing-then-green; 26 shield cases green, full ctest 2201/2201 green).
PersonalBubble shelters only its generator and TransportShield denies carrier/cargo damage
while sheltering nobody (both tested); the shield script-bit toggle keeps `OffHealth`
across recharge (`C-350`, SaveState v36). **Implemented 2026-10-15 (WP-32
`PassOverkillDamage`):** retail `shield.lua` `OnDamage`/`GetOverkill` with authored
`PassOverkillDamage` (corpus `UAL0303_unit.bp:78`, forwarded by `Unit.lua`
`CreateShield`/`CreatePersonalShield`) — a collapsing bubble forwards
`incoming - old` shield health to its owner's hull through `DoTakeDamage`
(`tests/test_shield.cpp:"a pass-overkill shield forwards the excess to its
owner's hull"`, failing-then-green; shield tag suite 4 cases green, serial
ctest 2225/2226 with the one `CPlatoon combat verbs` failure passing in
isolation — sibling-reported contention flake, untouched by this slice).
**Implemented 2026-10-16 (WP-32 `C-397` strategic penetration):** retail
`shield.lua` `Shield.OnCollisionCheck` answers false for STRATEGIC+MISSILE, so
the sweep never stages a shield impact for a nuke (`penetratesShield` on the
shot's `C-088` categories in `firstStruck`) and its point hit plus blast admit
no bubble at impact (`shieldPenetrating` through `resolvePendingImpact` →
`damageTarget(s)`; `damageArea`/`explodeOnDeath` unchanged — only projectile
impacts penetrate) — the hull beneath takes the full 40 while the dome stays
at 100 (`tests/test_shield.cpp:"a strategic missile penetrates a bubble
instead of striking it"`, failing-then-green; shield suite 33 cases green,
serial ctest 2246/2246 green on a pristine scratch source carrying only this
slice — shared tree still red from siblings' in-flight edits, untouched).

```text
/goal Advance FA-DAMAGE on the WP-32 script-policy axis: the C-143/C-396/C-428 numeric
contract is fully pinned (owner armour, handicap divide, DamageFriendly/NOSPLASHDAMAGE
conjunction). Next is the remaining PersonalBubble and transport-shield boundary per
`C-397`, then WP-30/31 confirmation; preserve the established sphere/box admission and
one-charge rules, run make test and make verify.
```

### FA-INTEL - Vision, Radar, Sonar, And Counter-Intel

**Largest gap:** a retained dead radar contact now reaps on the next intel update when a different
live generation reuses its slot, and is suppressed from projection immediately; time-based expiry
and other recon families remain incomplete.

**Audit 2026-10-17:** the old goal prompt below is done — deterministic
radar-position error already feeds automatic targeting without touching
identity, ordering, or identification (`automaticTrackPosition`,
`automaticProjectileAimPosition`, `Combat.cpp`; C-276/C-158 cover).
Remaining recon work is non-radar families (sonar memory depth, omni edge
cases, cloak/jammer interactions, water-vision granularity, Lua bindings).

```text
/goal Advance FA-INTEL by applying existing deterministic radar-position error to automatic
targeting without changing retained-contact identity, ordering, or visual identification. Preserve
generation-safe slot-reuse reaping; keep temporal expiry, cloak, jamming, water vision, sonar
memory, and Lua bindings out of scope. Run make test and make verify, then refresh WP-33 and
FA-INTEL.
```

### FA-PROGRESS - Enhancements, Veterancy, And Special Units
**Largest gap:** veterancy is implemented, and the enhancement lifecycle now covers
install, funded work, cancellation without refund, removal with health fallback,
save/load (`[enhancement][script-task]`), the `C-436` quarter-progress
milestone ratchet (`EnhancementProgress` at 0.25/0.5/0.75, funded ticks only —
2026-10-14), the `C-450` `OnBuildProgress(oldProg,newProg)` mod-compat call
shape (pre/post-tick fractions on the event, dispatched to the unit script —
2026-10-14; the saturation/NaN half closed same day by no-sentinel ruling),
the `C-252` unpaused gate's held half (`IsPaused()` → `Wait`: the order
stands, the bar freezes, the bank is untouched, ToggleProduction resumes the
same row — 2026-10-14, `tests/test_fa_progress.cpp:"C-252: a paused enhancement
holds its progress"`; the mirror into `EnhancementWork::paused` in
`Skirmish.cpp` was already there, this slice proves it, failing-then-green),
and the `C-255` build-tray half (2026-10-15): `gatherBuildOptions` appends
each installed enhancement's `BuildableCategoryAdds` term to the menu query
and gates above-T1 commander products on an installed enhancement covering
the entry's categories — an unenhanced ACU keeps the authored T1 tray
(retail's own T2-commander term is how T1 storage reaches it), an enhanced
one gains the unlocked tier (`tests/test_build_options.cpp:"an enhanced
commander is offered what its enhancements unlock"`, failing-then-green;
full ctest 2155/2155 green on a clean rebuild),
and the `C-437` batch half (2026-10-15): `grantScriptedKills` is the `AddKills`
loop (`Unit.lua:3084-3106`) — a script-only batch grant climbing every cleared
threshold through one shared promotion helper per level, so the degenerate
{1,1,1,1,1} ladder lands at 5 on a single grant where the per-kill path steps
once (`tests/test_veterancy.cpp:"a scripted batch of kills can promote more
than one level at once"`, failing-then-green; full ctest 2181/2181 green),
and the `C-437` ordering half (2026-10-15): the killer is credited BEFORE the
victim's death weapon fires (`Unit.lua:933-954` — `OnKilledUnit` runs, then
`DoDeathWeapon()`) — a victim whose blast fells a bystander names its corpse
in `lastHitBy`, and the corpse-credited chain kill earns nothing (dead killers
earn nothing) while the pre-blast credit stands alone
(`tests/test_veterancy.cpp:"C-437: a chain kill from a victim's death blast
credits the original killer"`, green focused; full binary 898/900 with the one
SIGABRT in FA-LUA's in-flight ScriptHost WIP, not this slice),
and the `C-257`/`C-380` unit-callback half (2026-10-15): `SetVeteranLevel`
(`Unit.lua:3190-3191`) calls the brain hook FIRST
(`brain:OnBrainUnitVeterancyLevel`, scenario `VeterancyTriggerList`), then the
promoted unit's own `DoUnitCallbacks('OnVeteran')` — the `UnitVeteranPromoted`
event now dispatches both, brain before unit
callbacks"`, failing-then-green; `[fa-progress],[veterancy],[enhancement],[faf]`
103 cases green; full ctest blocked by two siblings' in-flight reds, neither
this slice),
and the `C-257`/`C-380` registry half (2026-10-15): `DoUnitCallbacks('OnVeteran')`
(`Unit.lua:2814-2821`) walks the unit's `EventCallbacks.OnVeteran` list — the
functions scenario code registered with `AddUnitCallback(fn, 'OnVeteran')`
(`CreateUnitVeterancyTrigger`, `scenariotriggers.lua:387-389`) — not a
same-named method. The adapter grows the retail registry (`AddUnitCallback` /
`RemoveCallback` on the unit metatable, per-table like retail's per-unit
`EventCallbacks`) and the `UnitVeteranPromoted` dispatch runs it after the
brain hook and the method stand-in (`FafOpponent.cpp`, tested in
`tests/test_faf_ai.cpp:"C-257/C-380: a promotion reaches scenario callbacks in
the unit's EventCallbacks registry"`, failing-then-green; focused
`[faf],[veterancy],[enhancement],[fa-progress]` 104 cases green, full ctest
2236/2236 green excluding the pre-existing `CPlatoon combat verbs` order flake
— `__rm_faf_platoon_orders` iterates `pairs()` over the platoon set, so the
guard order is not always `pending[5]`; fails on clean `38ef9fa` too).
**Analysis closed 2026-09-18
(`C-251`–`C-265`):** the retail contract is now fully specified — `UNITCOMMAND_Script`
orders carry `{TaskName="EnhanceTask", Enhancement}`, slot replacement issues
`<old>Remove` then `<new>` 0.5 s apart, the task requires stationary+unpaused with
funded-drain progress and no refund, `OnWorkBegin` validates slot/prereq (with the
`WorkItemBuildCostMass = BuildCostEnergy` retail bug), `SimUnitEnhancements` is the
bookkeeping (cleared on death, not re-applied on capture), and every mutation is the
unit script's `CreateEnhancement` override. `SetUpgradedTo`/`QueueNotifyUpgrade` are
proven to belong to structure upgrades, not ACU enhancements. What remains is
implementation fidelity against that contract plus the one native WP-36 mechanic
(Colossus tractor claw).
```text
/goal Advance FA-PROGRESS by implementing the evidenced enhancement contract: the
EnhanceTask state machine (stationary+unpaused, funded-drain progress, no refund),
slot/prereq validation with the mass-cost bug, SimUnitEnhancements bookkeeping, and
the <x>Remove pseudo-enhancement path. Keep native task machinery untouched.
```

### FA-TERRAIN - Mutable Terrain And Craters

**Current slice (traced 2026-09-10):** retail craters are visual-only, and the trace
proves it end to end. `lua/defaultexplosions.lua` scorches every land explosion: scale
above 1.2 → `CreateScorchMarkDecal` (scale × 3, `scorch_*_albedo` decal textures), else
`CreateScorchMarkSplat` (scale × 4, `scorch_001`–`010_albedo`), random angle and texture
off sim RNG, LOD 200–350, lifetime 300–600. Both fan into the engine's moho `<global>`
`CreateSplat` (`0x00675720`) and `CreateDecal` (`0x006750a0`); non-lethal terrain and prop
hits scorch too (`aeonprojectiles.lua` at scale 3, `proptree.lua` falling trees at 0.5).
Counterevidence against height/type mutation on damage: the only `FlattenMapRect` caller
in shipped Lua is `defaultunits.lua` `CreateTarmac` (construction), and its
`SetTerrainTypeRect` is commented out in retail ("disabling this for now"). So damage
touches neither height nor terrain type, pathing needs no invalidation, and the render
delta is decal-buffer only. Our side already matches the structure: deaths leave
`FeatureStore` scorch records projected as permanent discs (`appendWreckMark`, 2.2× radius).

**Largest gap:** non-lethal impact scorch is now in — `impactMarks` records a
scorch on every `ProjectileImpact` event typed `Terrain`/`Prop` (C-291/C-292
tested: water excluded, hash-invisible, unit hits mark the victim not the
**Implemented 2026-10-14 (C-290 rotation axis):** the projection spins each
impact fan by a per-mark 0..2pi angle derived from the mark's own position bits
(splitmix32, `tests/test_ground_decals.cpp:"impact marks carry deterministic
rotation"` — distinct marks land at distinct angles, identical marks replay
bit-identically, no sim RNG spent per C-297). The engine's fan is
rotationally symmetric, so the angle is per-mark variety, not texture fidelity.
**Implemented 2026-10-14 (C-290 lifetime axis):** marks carry their
app tick and each retires at its own bit-derived point inside retail's
300–600 s scorch spread — 300 s floor plus a 0..300 s offset from the mark's
own position bits (same splitmix32 stream as the rotation axis,
`tests/test_ground_decals.cpp:"impact marks retire across retail's scorch
lifetime spread"` — 512 sampled marks split on both sides of the floor,
none outlive the ceiling, replay-identical, no sim RNG per C-297),
derived through the tick rate (hash-neutral, wreck marks stay permanent).
**Implemented 2026-10-15 (C-290 size split):** the projection applies retail's
splat/decal scale split — the mark's blast radius in elmos recovers the
explosion scale through 8 elmos per ogrid (`Scmap.hpp kElmosPerOgrid`),
scale above 1.2 draws at ×3, otherwise ×4
(`tests/test_ground_decals.cpp:"impact marks cover the blast that made them"`
— 8 elmos draws 32, 24 elmos draws 72, blast-less keeps the 2.5-elmo mark;
hash-neutral, water/unit gates unchanged).
**Implemented 2026-10-15 (C-290 texture axis):** retail's `ScorchDecalTextures`
(`defaultexplosions.lua:234-244`) holds the same ten `scorch_001`-`010_albedo`
names as the splat list, not two — a corpus read corrected the `kImpactMarkDecalTextures = 2`
model to 10 (`src/app/Scene.cpp`, `tests/test_ground_decals.cpp:"impact marks
record retail's texture pick"` — 64 decal-scale marks visit more than two
textures, both scales pick 0..9; hash-neutral, splitmix32 position bits, no sim
RNG per C-297).
**Implemented 2026-10-15 (C-290 beam-scorch half):** `noteBeamScorch`
(`src/app/Scene.cpp`) notes one blast-less base-splat mark per `BeamFired`
event at `event.at` with the event tick — retail's `ScorchThread`
(`defaultcollisionbeams.lua`: 8 of 16 classes fork on `impactType ==
'Terrain'`, thread lays `CreateSplat` at the tracked end while dwelling);
the `Match.cpp` drain wires it with the C-292 water gate from the tick's sim
terrain (waterless field never refuses — the gate is water, not height);
replay-identical, off the sim RNG (C-297), hash-neutral
(`tests/test_fa_terrain.cpp:"C-290: a beam striking terrain leaves scorch at
the strike point"` — unit-level terrain-marks/water-refuses + end-to-end
turreted beam tank vs LAND victim through `advanceMatch`; focused
`[fa-terrain]+[decals]` 834 assertions/17 cases green; serial ctest 2188/2189
with the sole failure pre-existing `CPlatoon combat verbs` on clean HEAD per
ImplProg11). **Implemented 2026-10-15 (C-290 beam-class opt-out):** per-beam-class scorch gate
(`beamClassScorches`, `src/app/Scene.cpp`: the 5
scorch-less leaves — `Ginsu`, `ParticleCannon`, `Zapper`, `DisruptorBeam`,
the FX-only `TractorClaw` — return false; the 8 thread-forking classes plus
`MicrowaveLaser02`/`Ultra02` by inheritance plus unknown names scorch;
`noteBeamScorch` takes an optional beam-class name, default empty = unmapped
= scorches; `tests/test_fa_terrain.cpp:"C-290: only ScorchThread beam classes
scorch"` — 16-class table plus unknown-scorches plus scorch-less-marks-nothing/
scorching-marks end to end; focused `[fa-terrain]` 75 assertions/8 cases green,
serial ctest 2236/2237 with the sole failure pre-existing `CPlatoon combat
verbs`). **Audit 2026-10-17:** the weapon→beam-class mapping is script-side
only — unit script imports a weapon class carrying `BeamType`
(`URB2301_Script.lua` → `CDFParticleCannonWeapon` →
`ParticleCannonCollisionBeam`), invisible to blueprints — so the drain cannot
map weapons to classes without hosting unit scripts (FA-LUA). The gate stays
wired at the helper with unmapped=scorches. Remaining: dwell-trail marks
along the tracked end (needs per-tick beam-end positions).
Retail wrecks never

```text
/goal Advance FA-TERRAIN on the presentation axis: the sim-side contract is
met (impact scorch records exist, height/pathing untouched). Next is the
splat/decal scale split with texture variety and lifetimes — FA-PRESENT work
projecting the existing `impactMarks`/`Feature` records — then beam
`ScorchThread` trails once the collision-beam scripts are hosted (FA-LUA).
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
no shots and do not replace the retail-map combat run. At that point the missing
fields were GridReclaim, Nickname and islandMarker. The former now has a native
reclaim snapshot; optional Nickname/islandMarker handling landed in `388982e`.
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

Latest measurement: [the September 8 one-hour duel](faf-duel-2026-09-08.md), at
`fbeb940`, completes 1,581 constructions and destroys 1,170 units but produces no
winner; both commanders finish at full current maximum health. An independent AI
repeat matches all 36,000 hashes. Six late decision watchdog failures and four
condition errors recur; 90 naval units contribute one shot and no kills. These
results superseded the short sanity runs at that revision.

The September 9 current-main rerun at `da80f5c` supersedes those operational
symptoms. Easy, turtle and tech each kill a commander and declare a winner by
14:08.8, 30:59.8 and 31:23.8 respectively, with no watchdog, condition or thread
errors. Easy repeatedly forms 42-unit attacks and independently matches all 36,000
state hashes. Event-backed easy and tech runs find zero and two bomber-attributed
allied deaths respectively; the two tech losses are isolated rather than a
death-blast chain. See the follow-up in
[the duel report](faf-duel-2026-09-08.md). This closes the reproduced watchdog,
nonlethal-pressure and aircraft-chain investigations without changing balance or
authored `DamageFriendly` behavior. Deeper retail AI architecture and naval combat
parity remain separate gaps.

```text
/goal Complete the native manager boundary remainder of the AI slice: must-scout
requests, unknown-threat queues, continuous air flybys AND the mid-route flyby
release are in (platoon.lua AirScoutingAI: VDist2Sq(vec, scout) < 15625 drops
the area, VDist3(scout, targetArea) < 25 covers the marker); the C-418
ARTILLERY+STRATEGIC conjunction gates the f[9] Artillery slot (a Mavor writes
it, a mobile artillery piece does not — tested). Preserve
generation-safe assignments and the current decisive, watchdog-clean duel
behavior; retain the current balance, reclaim policy and authored friendly
death effects.
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

The toggle page follows the same boundary: `General.ToggleCaps` parse into the definition,
presence by authored `true` (no capability fallback), and toggles fill dead order slots at
retail's preferred positions (Shield/Weapon 7, Jamming/Intel 8, Production/Stealth 9, Generic
10, Special/Cloak 11) with orders winning ties. `General.OrderOverrides` merge unanimously
across the selection and relabel cells (UEB4301's dome bitmap/help, tested headless and through
the corpus). The five backed toggles — shield, jamming, intel, stealth, cloak — now issue
`ToggleScriptBit` through the semantic command path (`C-350`): the click flips the unit's
script-bit mask, `SetMaintenanceConsumption{Active,Inactive}` runs last-writer-wins, and the
features follow `Unit.lua`'s `OnScriptBitSet`/`OnScriptBitClear` — a disabled shield stops
absorbing and regenerating while keeping `OffHealth`, a re-enable gates absorption for the
recharge time and resumes at the kept health; disabled intel/stealth withdraw the senses and
fields they own, and the intel placement cache re-stamps on the mask change rather than waiting
for the emitter to move. The mask and upkeep flag ride SaveState v36 and the state hash; the
command log records the bit at v7. Weapon, generic and special stay visibly disabled with a
NOT IMPLEMENTED hover card — retail's own no-ops.
A headed `toggle` capture scenario pins the extractor's Production cell pixel- and hash-stable.

**Current slice (2026-10-15, `C-333` mixed '?'):** retail's mixed-state icon (`orders.lua:339,365-390`) is implemented and headless-tested: new pure `toggleMixed` in `core/ui/CommandPanel.{hpp,cpp}` reports per-toggle carrier disagreement on the bit (pause flags for production; unbacked caps never split), `appendCommandRack` takes a parallel `mixed` array and suffixes split toggle cells with " ?", and the session feeds it from the same per-selection `scriptBitMasks` via new `commandMixed` state (reset beside `commandEngaged`). The pre-existing `C-369` slice above is unchanged. Focused suites green (`[toggles]` 59 assertions/11 cases, `[ui]` 1489 assertions/146 cases). Serial `ctest --test-dir build`: all green except pre-existing `CPlatoon combat verbs` (fails identically with this slice stashed; also seen red-then-green across runs, unrelated Lua AI surface).

```text
/goal Advance FA-UI with the remaining WP-40 surface — idle selectors, overlays,
key contexts, split views — behind the data-driven command page. Add focused
tests, run the full suite and golden replay, and update FA-UI.
```


### FA-PRESENT - Animation, Effects, And Audio

**Current slice:** UEF construction now emits its paired blue-white streams from every authored
`General.BuildBones.BuildEffectBones` model bone to the nearest two top corners of the target's
`Physics.MeshExtents` cube. The endpoints descend with progress and exchange places every 0.6
seconds, matching `EffectUtilities.lua:227-303`; stalled work emits nothing. **Cybran landed
2026-10-14:** each `BuildEffectBones` bone streams red to the pad plus `Unit.lua:GetRandomOffset`
(target `SizeX`/`SizeZ` box at scalar 1, builder-heading rotation with retail's own signs,
`CollisionOffsetY` lift, retargeted every 0.2 s per `CreateCybranBuildBeams`); draws from a
frame-keyed splitmix32 stream, sim hash untouched (`tests/test_build_effects.cpp`,
`src/core/scene/BuildEffects.cpp`, `src/app/Interface.cpp`; `UnitDef` keeps
`sizeXElmos`/`sizeZElmos`/`collisionOffsetYElmos`, URL0107's 0.15 ogrids). **Aeon landed
2026-10-15:** each `BuildEffectBones` bone streams green-white to the pad per
`CreateAeonCommanderBuildingEffects` (`beamEnd` warped to the target position,
one `AeonBuildBeams01` beam per bone, no sweep or offset; `rm::aeonBuildBeamEnd`,
fixed-camera captures at `/tmp/rm-aeon-build{,2}.png` identical, sim hash untouched).
**Seraphim landed 2026-10-15:** each `BuildEffectBones` bone streams one
`SeraphimBuildBeams01` beam builder-bone -> target per
`CreateSeraphimUnitEngineerBuildingEffects` (`EffectUtilities.lua:600-611`) — no
sweep, no offset (`rm::seraphimBuildBeamEnd`; `tests/test_build_effects.cpp`,
`src/core/scene/BuildEffects.cpp`, `src/app/Interface.cpp`; sim hash untouched).

Weapon effects now resolve from the original Lua declarations — projectile and unit
scripts, faction classes, `EffectTemplates.lua`, `defaultcollisionbeams.lua` — executed in an
isolated Lua state: PolyTrails, FxTrails, beams, `FxMuzzleFlash` and the impact lists, with
their emitter blueprints and DDS textures. Emitters follow authored curves, emission rates,
blend modes including inverse modulation and refraction, and sort order; muzzle emitters
track the resolved bone; beams follow live endpoints for the authored lifetime; ribbon
trails draw the original TrailBlueprints over each shot's recorded path; original
projectile meshes draw through the unit pipeline, pointed along the shot's velocity
(ADR-096 to ADR-103, [weapon visuals acceptance](weapon-visuals-acceptance.md)).

**Visual comparison 2026-09-24:** `.scmap`'s `<default>` environment path now
loads all six DDS cube faces; the unit shader samples its reflected view vector
instead of the old flat sky. SCMP_009 supplies `envcube_evergreen01a.dds`.
`Projectile.lua`'s second impact channel now reads `TerrainTypes.lua` by live
terrain code: the Striker's Gauss hit gains its authored `Medium02` ground emitter
at blueprint scale 0.4 in addition to `FxImpactLand`. Fixed-camera before/after
captures are under ignored `build/striker-*`; they show a subtle material change
and the additional ground burst, not retail screenshot equivalence.

Weapon fire now plays the blueprint's own `Audio.Fire` cue: the companion `.xsb` sound bank
resolves the cue name to wave-bank entries and `WeaponSounds` picks one take per shot by
the shooter's handle (ADR-106). The retail UEF weapon bank's Gauss cue resolves to entries
6, 5 and 7 as measured on the bytes. Takes play detuned within the bank's authored pitch
range (deterministic per seed, so replays sound identical), thinned by SupCom.xgs
per-category instance limits, and refused beyond the median world-scale silence distance
as the `LodCutoff`; volume bytes measured flat across the weapon banks, so takes play at
the mixer's gain.

**Current slice:** turret yaw/pitch and recoil/telescope bones now pose per
instance in the unit shader, using interpolated sim turret angles; authored walk
and one-shot unpack clips are baked once and selected at draw time. The Striker
has no `.sca` walk clip, so a six-frame trial revealed no baked-pose transition
to smooth. Reload/charge animation and retail's full manipulator host remain
separate gaps (Striker turret/recoil, XSL0111 reload, UAB4201 unpack,
UEB2302 telescope are the corpus probes).

**Largest gap:** remaining reload/charge animation, Seraphim factory
build effects, and native emitter flags (resistance, alignment and water gating).
Audio still omits some filter/RPC curves and dynamic music. Projectile mesh
blueprint LODs are loaded; absent retail mesh assets retain their strips.
The gallery verifies our renderer, not pixel parity with the retail game.

```text
/goal Advance FA-PRESENT with one authored non-UEF construction effect or
one native-emitter flag evidenced in a shipped blueprint. Match a fixed-camera
offscreen capture, preserve the sim hash, and update WP-41 and FA-PRESENT.
```

### FA-PERSIST - Replay, Hashing, And Save/Resume

 **Current slice:** caller-owned standing orders ride the v64 envelope with
 capture/restore helpers and their continued-hash proof (tested —
 `[fa-persist]`): a mid-combat app save carries factory rally points by builder
 slot and auto-expand flags with retry/refusal state, the restore re-seats them
 onto the live scene/runner, and SaveState v64 retains all older readers — a
 pre-v64 save decodes with no standing orders. Emitter placements ride with
 their continued-hash proof (`C-552`); a saved FlowField reduces to its
 expansion count and the deterministic Dijkstra replays it on restore.

**Largest gap:** this is not yet a general mid-combat app save. External input
and the opponent VM remain outside the envelope. Replay is a separate
compatibility boundary.

 **Audit 2026-10-17:** the v64 slice closed the standing-order boundary — rally
 points and auto-expand flags now have a save path with tests. The remaining
 envelope gap is external input, the opponent VM, and replay transport (goal
 prompt below).

```text
/goal Continue the pool-by-pool envelope: the remaining intel grids and
external input next with their continued-hash proof. Keep replay transport
separate, preserve old readers, and update WP-44 and FA-PERSIST.
```

## Remaining-work audit (2026-10-16, docs/test fixes only)

This is a source-backed register of what is known to remain; it is not a
completion percentage. No gameplay code changed in this audit.

- General Lua gameplay hosting: script scheduler/threading for arbitrary
  unit/projectile scripts, plus the general mod surface. Only the `EnhanceTask`
  adapter and hook/mod-table plumbing are implemented (`FA-LUA`).
- Native AI architecture: the deterministic FAF Lua opening speaks through many
  bindings, but native stand-in managers remain; deeper retail brain/platoon
  architecture and naval combat parity are separate gaps (`FA-AI`).
- Mid-combat save boundary: substantial versioned envelope exists, but general
  mid-combat app saves with external input/opponent VM and retail replay
  transport are outside it (`FA-PERSIST`).
- Formations and flight: wider land blocks, naval/sub/air ranks, HPA*,
  reservations, arrival re-forming; full three-axis flight solver, including
  quaternion pitch and roll-rate state (`FA-LAND`, `FA-AIR`).
- Intel/recon: temporal blip expiry and non-radar recon families beyond the
  implemented slot-reuse reaping (`FA-INTEL`).
- Presentation: reload/charge animation, Seraphim factory build effects, native
  emitter flags, some audio filters/curves and dynamic music (`FA-PRESENT`).
- Verification boundary: 2,219 suite cases pass with 38 asset/performance skips;
  those skipped paths are not proven, and passing tests are not a retail-percentage
  measurement.

No subsystem still lacks every implementation: each dashboard row has analyzed
contracts and at least a bounded slice. The >97% implementation goal therefore
depends on defining a measured denominator and per-subsystem acceptance proof;
this register does not supply either.


## Maintenance Contract

When work changes a subsystem:

1. Update the canonical WP row and claims in `fa-exe-analysis-plan.md` if retail evidence changed.
2. Update this row's three estimates, largest gap, exact next task, and goal prompt.
3. Recompute the gameplay headline as an equal-weight average excluding `FA-FOUND`, rounded to 5%.
4. Update the snapshot date/revision and state `dirty worktree` when applicable.
5. Keep every `WP-00` through `WP-44` in exactly one subsystem row.
6. Recheck the confirmation count, `Analyzed` WP count, implementation critical path, EXE-analysis
   action, and matching summary at the top of `fa-exe-analysis-plan.md`.
