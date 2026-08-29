<!-- Generated and maintained by Claude -->

# PLAN2 — architecture, and the order features get built

`PLAN.md` is the milestone log: what renders next. This file is the other axis — **what this
engine is, how its parts connect, what order to build them in, and how far along it is.**

It is written against `../faf/forged-alliance-reborn/docs/engine-analysis/` (16 reports, 19,871
lines) and `reference/{RecoilEngine,FAF-fa,BAR,Zero-K}`. Every non-obvious claim cites one.
The first version of this file did not, and invented from a blank page what two mature engines
and a 763-line feasibility study had already settled.

---

## Status — 2026-08-20

**P0 done. P1 done except its last item.** 601 tests green, up from 527 at the start of the
day. Tree clean; nine commits.

The determinism harness exists (P0): a per-tick state hash, `--hash-log` /
`--check-hash-log`, first-divergence reporting, and two CTest checks that hold the tick order
and the no-globals rule. Both loops now run the same tick.

P1 has its handle (`UnitId` + `IdPool`), its store (`UnitStore`), its census
(`UnitCensus`), a golden match to refactor against (`make verify` → `MATCH — 5200 ticks
identical`), and an order-independent invariant suite for the part the golden log cannot
cover.

**P0 THROUGH P8 ARE DONE. P9 IS FROZEN.** P7 built the tick-to-render seam and emptied
`main.mm`: the sim publishes an immutable snapshot, the renderer interpolates between the last
two, `Renderer.mm` went 4,267 → 1,440 lines split four ways, there is a minimap, and `main.mm`
went 5,098 → 324 with the rest becoming a library the tests link. P8 measured the scale
criterion: 5,000 units at **12.9x real time**.

**What is left is P9, and it is frozen deliberately** — the Linux build and the
cross-architecture replay hash. §1.3's criterion has two halves and the second one is still
unproven: everything the harness reports is one machine agreeing with itself.

**P5 and P6 before them**: a spatial index that took the quadratic term out of targeting, an
event vocabulary, wrecks as real objects, and the last include from `core/sim` to `core/scene`
gone.

**P3 IS DONE, and so is P4.** Game rules are out of C++. Blueprint categories are parsed and
classified into roles; SupCom's `BuildableCategory` expressions are materialised into a real
build tree; the opponent's opening is `data/opening.lua` and plays all four factions;
passability comes from the motion class rather than from building-placement fields; FA's
`WaitSeconds` bias is corrected at import and burst weapons deliver their bursts; and periodic
work is paced by a mechanism rather than by a modulo in a caller.

**P4 is done with it.** A unit holds a `CommandQueue` with Recoil's cancel-queued semantics,
shift-right-click appends, `advanceOrders` is the tick's first pass, and a registered check
keeps `applyCommand` the only way an order reaches the sim.

**P2 is done.** The sim is fixed point end to end and the float ban is enforced; the tick rate
is a value with `--tick-rate` and a four-rate test behind it; ownership has its three levels;
and orders are data taking one path, with `--command-log` writing the artifact §1.3's criterion
names.

**Engine completion: ~65 %** (§2), computed with a script.

---

**P1 is done.** P1.4 landed: `UnitRef{batch, instance}`, `CombatGroup`, `SkirmishGroup` and
`CollisionGroup` are gone, and every sim pass takes `UnitStore` + `UnitCatalog` and loops once
over slots. As predicted the golden log diverged at tick 0 and was re-recorded in the same
commit; `tests/test_match_invariants.cpp` was the gate and held. It also earned its keep
immediately — it found a real defect in `hashMatch`, which could not see a unit die (§7 P1.4).

---

## 0. Decisions taken

| # | Decision | Date | Consequence |
|---|---|---|---|
| **D1** | **Fixed-point integer sim.** Faster and safe across CPUs | 2026-08-20 | §5.2. Blocks nothing; every tick of float sim written before it lands is a tick to rewrite |
| **D2** | **Tick rate is configuration, not a constant.** Valid range **5–50 Hz, hard rule.** Durations are authored in *seconds*, once; tick counts are always derived from the rate, so changing the rate never means editing a millisecond value | 2026-08-20 | §5.1. If SupCom content needs 10 Hz, 10 Hz becomes a config value, not a rebuild |
| **D3** | **Copy Recoil's architecture** where it is good enough, and record every divergence so a future session knows which choices were ours | 2026-08-20 | §4, §6 |
| **D4** | **Integer type aliases everywhere** in sim state, so widths can be bumped 32→64 without touching logic | 2026-08-20 | §5.3 |
| **D5** | **Progress is measured, periodically, as a percentage of the whole engine** | 2026-08-20 | §2 |
| **D6** | **Unit behaviour lives in C++, engine-owned — BAR/Recoil's side of the conflict.** No script host. Behaviour varies by *data* (unit type, role, motion class), never by script | 2026-08-20 | §3.1. Settles `16 §2`'s conflict for us |
| **D7** | **No global variables. Dependencies are passed.** The sim is a value you construct, not a process-wide singleton set | 2026-08-20 | §5.4. The single largest deliberate divergence from Recoil |
| **D8** | **Copy Recoil's *functionality*, not its structure.** It is a working game with a working asset set; that is what we are borrowing | 2026-08-20 | §6 |
| **D9** | **Default tick rate 10 Hz**, since the content we import is FA-authored. The knob (5–50 Hz) covers everything else | 2026-08-20 | §5.1 |
| **D10** | **Feature work is frozen** until P0–P2 land. Milestones onto the current shape while P1 dismantles it means doing that work twice | 2026-08-20 | §7 |
| **D11** | **P0 is authorized and started** | 2026-08-20 | §7 |
| **D12** | **Damage is per armour class, stored sparsely; classes are names in the data and a dense index in the tick.** Both content families flatten to one runtime shape and differ only in the importer | 2026-08-21 | `ADR-033`, §7 P10.1 |
| **D13** | **A projectile varies by a TAG, not by a subclass.** The flat trivially-copyable struct is load-bearing — it is what the state hash walks — so both reference engines' hierarchies are refused | 2026-08-21 | `ADR-034`, §7 P10.3 |
| **D14** | **Pathfinding is built in layers and this is a STAGING POST.** Cost field, then shared flow fields, then a dynamic blocking overlay. The final architecture is deliberately deferred until the engine can measure its own pathing | 2026-08-21 | `ADR-035`, §7 P10.4 |
| **D15** | **Parallelism may reorder WORK, never RESULTS.** Fork-join, per-slot writes, application in slot order. The tick's pass order is never parallelised | 2026-08-21 | `ADR-036`, §7 P10.6 |

Still open: §9 — Q3 (snapshot copy vs `drawPos + timeOffset`) and Q4 (success criterion).

---

## 1. What this engine is — decided, not open

`16-new-engine-feasibility.md §5` ranks five options. This project is **option 4**:

> *A new engine with its own native format, plus converters from both* ⭐ *the rational
> ambitious option. The crucial move: **you promise import, not compatibility.** No existing
> Lua runs. No community plays your build of their game. You pick your own semantics.*

And `§7` states the shape:

> *Own semantics, compatibility an explicit non-goal, importing already-converted blueprints.
> Success criterion stated in advance and falsifiable: N thousand entities at 30 Hz with
> bit-identical replay hashes across two architectures. Fixed-point integer sim, a Metal or
> wgpu renderer, no Lua API compatibility with anything.*

**1.1 — Compatibility is a non-goal, in writing.** `§6`: *"The moment 'and it should run BAR's
gadgets' appears, §4's unbounded tail attaches and the project is dead."* This retires a live
justification in our own source: `core/sim/Movement.hpp:30-36` argues for 10 Hz so *"Supreme
Commander's own gameplay scripts could later run on this sim unmodified."* Under D2 that is
moot — the rate is a knob, so nothing has to be argued at compile time.

**1.2 — The thesis is determinism.** `§6`: *"a fixed-point integer sim — determinism by
construction, across platforms, forever — something neither Recoil nor FA can claim… If you
want one reason to write an engine, this is it."* Settled as D1.

**1.3 — The success criterion.** Proposed, pending D-open:

> **5,000 units at the configured tick rate on the M4, with bit-identical replay hashes across
> macOS-arm64 and Linux-x86-64, rendering through Metal, importing real blueprints from both
> content families.**

`ADR-032` already commits to an RHI seam for Linux, which is what makes the cross-arch half
reachable rather than aspirational.

---

## 2. Measuring progress

D5. The question "how much of the engine is done" needs a denominator that isn't invented, a
numerator that isn't a feeling, and a procedure that gives the same answer twice.

### 2.1 The denominator

`16-new-engine-feasibility.md §3a` costs a from-scratch engine subsystem by subsystem in
lines-of-code, sized against Recoil's own tree. Those are the best available estimates of
**relative subsystem size**, so they become the weights. Two rows are struck out because they
are explicit non-goals (§1.1): the Lua API bindings (40–55k) and the `LuaOpenGL` compatibility
layer (12–20k). Weights are midpoints, renormalised.

| Subsystem | doc 16 §3a mid | Weight |
|---|---:|---:|
| Sim — units, weapons, projectiles, movetypes, features, misc | 55k | **26 %** |
| Renderer feature layer | 45k | **21 %** |
| System — VFS, archives, config, logging, threading, sound, serialisation | 32.5k | **16 %** |
| Game loop, orders, selection, camera, UI | 27.5k | **13 %** |
| Map — formats, terrain rendering, terrain/resource maps | 20k | **10 %** |
| Pathfinding | 15k | **7 %** |
| Net, lockstep, replay | 11.5k | **5 %** |
| AI | 3.5k | **2 %** |
| | 210k | 100 % |

### 2.2 The reading, 2026-08-21 (after P8)

Weights are sourced; the per-subsystem completion figures are **judgement against a named
capability list**, which is the honest description. Recompute by re-judging each row, not by
re-deriving the weights.

| Subsystem | W | Done | Have | Missing |
|---|---:|---:|---|---|
| Sim | 26 % | **75 %** | movement, collisions, targeting/facing/firing, projectiles, area damage, death, economy, construction, victory, unit handles + flat store + type catalog + census, fixed point END TO END (enforced), CORDIC trig, tick rate as a value at 5–50 Hz, three ownership levels, orders as data through one authorised path, **burst weapons, FA's `WaitSeconds` bias corrected at import, periodic work staggered by a derived period, order queues advanced in the tick, a spatial index every query goes through, an event vocabulary, wrecks as objects, **an immutable snapshot the renderer reads instead of live state, 5,000 units at 12.9x real time**, veterancy and hull regeneration on retail's own per-blueprint thresholds | intel/vision, shields, transports, most weapon classes, air/naval/hover domains, upgrades, adjacency, features that INTERACT (a wreck is a record, not an obstacle or salvage) |
| Renderer | 21 % | **75 %** | instanced units w/ team colour, props + culling, shadows, water + refraction, sky, particles, decals, text/HUD, icons, selection, model LOD, pose playback, DDS, offscreen capture, **an immutable per-tick snapshot, interpolation between the last two, the drawer split, a minimap** | effect taxonomy (muzzle/trail/impact), beams, unit LOD switching |
| System | 16 % | **35 %** | VFS (`.sdz`/`.scd`), asset search, DDS, settings, bench harness, **a data layer: roles, build tree, roster, opening** | **sound (nothing)**, logging framework, job system, serialisation/save, profiling |
| Game/orders/UI | 13 % | **80 %** | orbit camera, picking, selection + modifiers, HUD, order markers, CLI harness, commands as data + a command log + player/army/alliance, build commands applied by the sim AND issued through it, **a per-unit command queue with Recoil's cancel-queued rules, shift-right-click to append, and a registered check that there is one order path, a minimap with click-to-jump, an argv layer with tests** | build menu, control groups, formations, game states, playing a command log back |
| Map | 10 % | **80 %** | SMF/SMT, `.scmap`, tile atlas, heightfield, `mapinfo.lua`, terrain mesh w/ LOD + skirts, chunk culling, splat, water, stratum normals, props, terrain types, start positions, **a minimap with a world round-trip** | resource spots, features as obstacles, the `.scmap`'s embedded preview image |
| Pathfinding | 7 % | **40 %** | coarse grid A* **in fixed point**, **passability per motion class (amphibious, hover, land)**, path following | hierarchical/flow-field, dynamic blocking, formations, avoidance quality, per-motion-class grids |
| Net/replay | 5 % | **40 %** | **a command log — §1.3's criterion now names something that exists** — per-tick state hash over the store (incl. per-slot type, generation and liveness), hash log + first-divergence reporting (P0.2/P0.3) | netcode, lockstep, the cross-architecture proof (P9, frozen) |
| AI | 2 % | **45 %** | scripted build order + one attack wave, **role classification, a materialised build tree, an opening read from data that plays all four factions** | any reaction to what the opponent does; a real AI |

**Weighted total: ~65 %** (script: `0.26*75 + 0.21*75 + 0.16*35 + 0.13*80 + 0.10*80 + 0.07*40
+ 0.05*40 + 0.02*45 = 64.95`). Renderer 55 → 75 % is the largest single move in the project's
history and it is the SEAM rather than the pixels: an immutable snapshot, interpolation between
two of them, and the drawer split were three of that row's four Missing items. Game/orders
65 → 80 % for the minimap and for argv finally being testable at all. Map 70 → 80 % because the
minimap was its longest-standing Missing entry. Sim 73 → 75 % for publishing a snapshot, and
for the scale criterion being measured rather than hoped for.

**System stays at 35 %, and that is now the most conspicuous number on the page.** It is 16 % of
the weight, it has **no sound at all**, and nothing in P0 through P8 touched it.

**The previous reading:** Sim 68 → 73 % for the spatial index, the event queue and features
— one Missing item moving to partly-Have, plus two foundations that make later ones cheap. No
other row moved: P5 and P6 are a refactor and a seam, and neither adds a capability outside the
sim. That the number barely moves for a phase this size is the honest reading rather than a
disappointment — §2 measures how much engine exists, and P5 made the existing engine *fast*
while P6 made it *reachable*.

**The previous reading:** Sim 65 → 68 % for burst firing, the import-time duration correction
and `SlowUpdate`; Game/orders 55 → 65 % because the one thing the previous reading named as
missing — *"what is left there is the QUEUE"* — is the thing this phase built.

**AND A CORRECTION, which is why the total went DOWN from the 58 % published yesterday.** The
weighted sum was being computed wrong, and the error grew: the baseline is exact at 35.02, but
by the P2 row the published figure was 52 against a true 49.20, and the P3.1–P3.4 row said 58
against a true 53.90. Nothing regressed and no row's judgement changed — the arithmetic was
inflated. Every row in §2.3's table is recomputed below and the drift is shown, because §2.3
says never to overwrite the trend and a silently corrected trend is worse than a visible
mistake. Read against the corrected series, this phase moved 54 → 56.

The earlier readings' *reasons* stand as written: AI moved 15 → 45 % because the thing that made
it a stub is gone — it had no build tree, so `07 §4.3`'s "hard blocker for any skirmish" applied
to ours as much as to BAR's. Pathfinding 30 → 40 % because passability finally means what
ADR-027 said it did. System 30 → 35 % for the data layer itself.

**The earlier reading, kept for the trend:** at 52 %, the Sim row moved 42 → 60 % because the phase finished what it
started: the sim is fixed point end to end and a script enforces it, so the determinism claim
rests on something checked rather than intended. Net/replay 25 → 40 % is the command log —
§1.3's criterion was half a sentence about a file that did not exist, and now both halves
exist. Game/orders 30 → 45 % because orders are data with an authorised path, which is most of
what an order system is; what is left there is the QUEUE.

**What is still not proven, and this is the honest limit:** every determinism result here is
one machine agreeing with itself. `check_fx_optimisation.sh` shows the arithmetic survives
`-O0` through `-ffast-math`, which is a real result and is not the claim. The claim needs P9's
Linux build. Fixed point is what makes it *reachable*; it does not make it true.

The shape of that number has changed, and it is worth saying how. For most of this project the
finished slices were the ones a screenshot samples, which is why it looked further along than it
was. At 65 % the four biggest rows — Sim 75, Renderer 75, Game 80, Map 80 — are within five
points of each other and are 70 % of the weight between them. **What is left is no longer "the
parts that do not show"; it is three specific holes**: System at 35 % with no sound,
Pathfinding at 40 % with no hierarchical search, and Net/replay at 40 % with the
cross-architecture proof frozen in P9. Each of those is a phase, not a polish pass.

### 2.3 The procedure

- Recompute at every milestone close. Append a row below — never overwrite; the trend is worth
  more than the number.
- A subsystem's figure may only move when a capability moves between the Have and Missing
  columns. "It feels better" is not a move.
- Keep this separate from **plan progress** (checked boxes in §7). They answer different
  questions: §2 is "how much engine exists", §7 is "how much of the current plan is executed".
  A phase can complete and move §2 by one point.

| Date | Sim | Rend | Sys | Game | Map | Path | Net | AI | **Total** | Note |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2026-08-20 | 22 | 55 | 30 | 30 | 70 | 25 | 0 | 15 | **35 %** | baseline, before P0 |
| 2026-08-20 | 22 | 55 | 30 | 30 | 70 | 25 | 15 | 15 | **36 %** | P0 done — the determinism harness exists |
| 2026-08-20 | 25 | 55 | 30 | 30 | 70 | 25 | 15 | 15 | **37 %** | P1.0–P1.3 — handle, store, census, and the invariant net |
| 2026-08-20 | 32 | 55 | 30 | 30 | 70 | 25 | 20 | 15 | **40 %** | P1 done — the store IS the sim's storage; batch groups gone |
| 2026-08-20 | 42 | 55 | 30 | 30 | 70 | 25 | 25 | 15 | **43 %** | P2.1 + P2.3's mechanism + half of P2.2 — fixed-point arithmetic, trig without libm, tick rate as a value, health/damage/economy migrated |
| 2026-08-20 | 60 | 55 | 30 | 45 | 70 | 30 | 40 | 15 | **52 %** | P2 done — sim fixed point end to end, float ban enforced, three ownership levels, orders as data on one path |
| 2026-08-21 | 65 | 55 | 35 | 55 | 70 | 40 | 40 | 45 | **58 %** | P3.1–P3.4 — roles, the build tree, the opening as data, passability by motion class |
| 2026-08-21 | 68 | 55 | 35 | 65 | 70 | 40 | 40 | 45 | **56 %** | P3.5, P3.6, P4 — burst weapons, the FA duration correction, `SlowUpdate`, the command queue, one checked order path |
| 2026-08-21 | 73 | 55 | 35 | 65 | 70 | 40 | 40 | 45 | **57 %** | P5, P6 — the spatial index (targeting 10x faster at 5,000 units), the event vocabulary, wrecks as objects, the sim's include graph enforced headless |
| 2026-08-21 | 75 | 75 | 35 | 80 | 80 | 40 | 40 | 45 | **65 %** | P7, P8 — the snapshot seam, interpolation, `Renderer.mm` split four ways, a minimap, `main.mm` 5,098 → 324 lines, 5,000 units at 12.9x real time |

**The totals above 37 % were computed wrong, and here is the arithmetic.** The judgement columns
are unchanged; only the weighted sum was off, and the error grew with the numbers. Recomputed
against §2.1's weights:

| Row | Published | True |
|---|---:|---:|
| baseline | 35 | 35.0 |
| P0 | 36 | 35.8 |
| P1.0–P1.3 | 37 | 36.6 |
| P1 | 40 | **38.6** |
| P2.1 + P2.3 + half P2.2 | 43 | **41.5** |
| P2 | 52 | **49.2** |
| P3.1–P3.4 | 58 | **53.9** |
| P3.5 + P3.6 + P4 | — | **56.0** |

So the real trend is 35 → 36 → 37 → 39 → 41 → 49 → 54 → **56**. It is a slightly flatter curve
than the published one and it is the same shape, which is the reassuring outcome: the phases did
move the number, just not by as much as was claimed. Kept here rather than by editing the rows,
because §2.3's rule is that the trend is worth more than the number — including the trend of
getting the number wrong.

---

## 3. Recoil vs Forged Alliance — the difference that decides our shape

Both reference engines are complete RTS engines. They are built on opposite answers to one
question: **who owns a unit?** `11-fa-sim-layer.md §2.3`, `§7.3` and `16 §2` all land on it.

| | Recoil / Spring | Forged Alliance / Moho |
|---|---|---|
| **Who owns a unit** | The **engine**. `CUnit` ticks it — movement via `MoveType`, firing via `CWeapon`, building, resource flow — and calls out to Lua at ~130 defined points. Lua is a *decorator*. Strip all Lua and units still drive and shoot | **Lua**. A unit *is* a Lua `Class()` instance whose metatable chain reaches a C++ base, holding a handle to an entity. `lua/sim/Unit.lua` is **5,670 lines** — lifecycle, veterancy, death, wreckage, buffs, shields, transport. Strip all Lua and there is no game |
| **Global sim tick** | `gadget:GameFrame(n)` — one call per frame into game code | **none.** All periodic work is `ForkThread` + `WaitTicks` coroutines, scheduled by the engine |
| **Per-unit specialisation** | dispatch on `unitDefID` inside a global handler, or the unit's animation script | override a method on the instance |
| **Extension mechanism** | ~130 events plus **17 `Allow*` veto hooks** | override the method; there are no vetoes |
| **Sim ↔ UI** | two Lua states, synced and unsynced, separation enforced by the language environment | a `Sync` table marshalled sim→UI每 tick; `lua/ui/` is a separate world (117,895 lines) |
| **Scale of the content layer** | BAR: 2,186 Lua files / 798k lines *on top of* a 386k-line engine | FA: 332k lines of Lua, of which `lua/sim/` is 51,782 |

`16 §2` calls this **the decisive conflict**, and it is why that report concludes two sim
backends rather than one: *"You cannot have one object model that is simultaneously the
authority for its own tick and a passive service under someone else's tick."*

### 3.1 Which one we take, and why we have no choice

**Recoil's.** Not by preference — by force. `ADR-028`/`ADR-029` decided Lua is parsed as
*data, never evaluated*: there is no VM and there will not be one. An engine with no script
host cannot use the model where scripts own the entities. So: the engine owns the unit, there
is one global tick, and specialisation is data-driven dispatch on unit type.

`11 §2.3` also happens to prefer it on the merits — of `GameFrame` it says *"FA has no global
sim tick hook at all. Everything is coroutines. This is the single biggest architectural
difference, and it is in Recoil's favour."*

### 3.2 What "global tick" buys, concretely

A global tick means there is exactly one place where time advances, and it advances *everything*
in a declared order. Per-unit periodic work is a pass over a store inside that order.

The FA alternative — a coroutine per unit per concern — costs three things:

1. **Scale.** `recoil-engine-map.md §5` names it: *"Coroutine churn is the scaling wall"* —
   one coroutine per weapon per re-aim plus one per shot, all single-threaded.
2. **No cross-unit pass.** Work that wants every unit at once has nowhere to stand.
3. **Ordering is the scheduler's**, not the designer's — so the order passes run in is an
   emergent property rather than a stated one.

That third point is not theoretical for us. `core/sim/Skirmish.hpp` exists *because* the pass
order had no owner: it lived in an anonymous namespace in `main.mm`, so no test could reach it,
and the windowed loop silently ran a smaller subset than the headless one — units in the
interactive game moved perfectly and never fired, with the whole suite green.

**So the rule we take from this: the tick order is part of the architecture and must be a
declared, testable sequence** — not a private method order the way Recoil leaves it
(`CUnitHandler::UpdateUnitPathing / UpdateUnitMoveTypes / UpdateUnitLosStates / UpdateUnits /
UpdateUnitWeapons` are all private and implicit).

### 3.3 The 17 `Allow*` hooks, and why not building them is a decision

Recoil's engine calls a permission hook before it does certain things, and any subscribing
gadget can refuse: `AllowCommand`, `AllowUnitCreation`, `AllowUnitTransfer`,
`AllowUnitBuildStep`, `AllowWeaponTarget`, `AllowFeatureBuildStep`, `AllowResourceTransfer`,
and eleven more (catalogued at `04-lua-game-framework.md §4`). Aggregation is AND — one `false`
blocks — and `04` warns the defaults surprise: `AllowWeaponTarget` defaults to **false**.

They exist to solve a problem created by Recoil's own shape. The engine owns the behaviour, so
content needs a way to say *"not that one"* without editing C++. FA has no equivalent because
it does not need one: content owns the method, so changing behaviour means overriding it.
`11 §7.3` puts it exactly — *"FA has no permission hooks; Recoil has seventeen. FA achieves the
same by overriding methods, which is only possible because Lua lives inside the object."*

**We are the content.** If a rule should be "no structures inside an enemy build radius", that
is an `if` in the placement rule — a hook point with no subscriber is a call site, an
aggregation policy and a surprising default, bought for nothing.

Recorded here so it does not get built by reflex while copying Recoil, and so the name of the
thing we skipped is known: **if we ever want data-driven rule modifiers (mod support), the veto
pattern is what to reach for, and this is the note that says where to look.**

---

## 4. The target layers

Four static libraries, dependency arrows left to right only. Separate CMake targets rather than
directories, because `rm_sim` then has **no link line to Metal or AppKit** and a stray include
is a compile error. `README.md:1141` has claimed *"core/ — pure C++, no Metal"* all along; it
is true, and it did not prevent a single problem in §6, because a directory comment is not a
constraint.

```
rm_core        rm_data                  rm_sim                      rm_render + app
─────────      ───────                  ──────                      ───────────────
formats,       the def layer:           deterministic, headless,    Metal. consumes
math, ids,     UnitCatalog from         fixed-point, no clock       Snapshot + Events.
containers     .bp/.lua + OUR OWN       ├── UnitStore + IdPool      ├── MapRenderer
(HeightField,  data files (roles,       ├── Player/Team/Alliance    ├── UnitRenderer
 Smf, Scm,     build orders, victory)   ├── CommandQueue per unit   ├── FxRenderer
 S3o, Sca,     per-second → per-tick    ├── SpatialGrid             └── UiRenderer
 Dds, Lua,     conversion happens       ├── MoveDefs by name            (HUD, minimap)
 Vfs)          HERE, once               ├── Features, Projectiles
                                        ├── EventQueue
                                        └── tick()  — one declared order
                                              │
                                              └── Snapshot ──► render (one way)
```

**`rm_data` is the layer we have never had, and its absence is the deepest problem in the
codebase** — deeper than `main.mm`. Both reference engines keep game rules in data. Ours keeps:

```cpp
inline constexpr std::size_t kAttackWaveTanks = 20;
inline constexpr std::string_view kFactoryBlueprint = "/units/UEB0101/UEB0101_unit.bp";
```

— `core/sim/BuildOrder.hpp:31,48`: a game-design decision in a C++ header, needing a recompile
to change, hardcoding one faction's path into the engine. Its own comment says *"every choice
in it is OURS — the game names no build order"*, which is true and is exactly the tell: content
went into the engine because there was no content layer to put it in.

### 4.1 The tick → render seam

Sim publishes an immutable snapshot per tick; the renderer interpolates between the last two.
This is the same boundary Recoil enforces through its type system (`System/Sync/SyncedPrimitive.h`,
`SyncedFloat3.h`), and it is what makes the replay-hash criterion mechanically true rather than a
discipline we hope holds.

Recoil itself derives `drawPos = pos + speed * timeOffset` (`Sim/Objects/SolidObject.h:431`,
annotated `unsynced`) instead of copying. That is cheaper, and is question Q4 in §9. The case
for the copy: with D2 the tick rate is now a knob, and at the low end (5 Hz) the interpolation
window is 200 ms, where reading half-updated sim state would be visible; and a per-player
snapshot is where fog of war attaches later.

---

## 5. Sim invariants

The three rules that D1, D2 and D4 turn into code. These are load-bearing: breaking any one
breaks the replay hash, which is the project's success criterion.

### 5.1 Time — D2

**Every duration and rate is authored once, in seconds. The number of ticks is always derived
from the configured rate. No tick-denominated literal exists anywhere in the codebase.**

That is the whole rule, and it is there to kill one specific bug: a constant whose value is
only correct at one rate, with the rate written in a comment instead of in the arithmetic.

We already have two of those:

```cpp
inline constexpr int kProjectileLifetimeTicks = 300;   // "Thirty seconds at 10 Hz"
static constexpr int kMaxTicksPerAdvance = 5;          // "a tenth of a second of catch-up"
```

`core/sim/Combat.hpp:106` and `core/sim/Movement.hpp:246`. Move to 20 Hz and the first silently
becomes a 15-second projectile lifetime and the second a 250 ms catch-up cap. Nothing fails;
the balance just quietly changes. They become:

```cpp
inline constexpr Seconds kProjectileLifetime = 30.0f;      // authored, rate-independent
inline constexpr Seconds kMaxCatchUp          = 0.5f;
// ...used only through the rate:
const TickCount life = rate.ticks(kProjectileLifetime);
```

**And the pattern already exists in our own code, one file over.** `unit/Weapon.hpp:81`
authors *"shots per second, as authored"* and derives `reloadTicks()` from it. That is the
correct shape; §5.1 is that generalised, plus deleting the two constants that do not follow it.

The mechanism:

- **`TickRate`** is one value in the sim's config, validated on construction to
  **5 ≤ rate ≤ 50 Hz**. Hard rule; out of range is a startup error, not a clamp.
- It exposes the only two conversions anyone may use:
  `ticks(Seconds) → TickCount` and `perTick(perSecond) → Fx`.
- **Rounding is part of the contract, because it is determinism-relevant:** round to nearest,
  with a floor of one tick for any positive duration. A duration that rounded to zero ticks
  would fire every tick — which is not "slightly fast", it is a different mechanic.
- Content rates — speeds, turn rates, reload times, income, build rates — convert from
  per-second to per-tick **exactly once, at catalog load** in `rm_data`. `UnitDef` stores
  per-tick values so the sim never divides, and the sim never sees a per-second number.
- Periodic work ("once a second") is `rate.ticks(1.0f)`, never a literal. Recoil hardcodes 16
  frames for `SlowUpdate`; ours is computed.
- `kTickSeconds` leaves sim math. A `secondsPerTick()` accessor survives for the renderer and
  the HUD clock, which are display concerns and unsynced.
- **A replay records its tick rate,** and replaying a log at a different rate is rejected rather
  than rescaled — the hashes cannot match, and pretending otherwise hides a bug.

**The test that proves the rule holds** (P2.3): run the same scripted match at 5, 10, 20 and
50 Hz and assert every observable duration — projectile lifetime, reload cadence, build
completion, income accrued — lands within one tick of its authored value in *wall-clock
seconds*. If a rate change moves any of them by more than a tick, a literal survived somewhere.
This is mechanical and cheap, and it is the only way this rule stays true a year from now.

Why this and not Recoil's approach: `recoil-engine-map.md §2` opens with *"Recoil is internally
inconsistent about units. This table is the highest-value thing in this document; most
conversion bugs are here"* — and shows why. In one def, `speed` is elmos **per second**,
`maxVelocity` is elmos **per frame**, `maxAcc` is elmos **per frame²**, `turnRate` is
65536-units **per frame**. Their unit-of-time leaked into their content format, permanently.
Ours does not leave `rm_data`.

One numerical note: at 50 Hz per-tick deltas are 10× smaller than at 5 Hz. The slowest content
speeds are a few elmos/second, so ~0.1 elmo/tick at the fast end — four orders of magnitude
above Q16.16's resolution. Precision is not a concern across the permitted range.

### 5.2 Numbers — D1

Fixed-point for everything the sim owns: position, velocity, heading, health, resources.
Rendering stays float — it is unsynced and its precision does not affect the hash.

- Storage `FxRaw` = `std::int32_t`, `kFxFractionalBits = 16` (Q16.16). Multiplies widen through
  `FxWide` = `std::int64_t` and shift back.
- Fixed-point `sqrt`, `sin`, `cos`, `atan2` from tables — no libm in the sim, ever, because
  libm is where cross-platform float divergence lives.
- No `float`, no `double` and no `libm` call may appear in `rm_sim`. This is checkable, so it
  should be checked in CI rather than reviewed.

### 5.3 Types — D4

One header, `core/Types.hpp`, and nothing in sim state uses a bare `int`, `size_t` or
`unsigned`. Widths become a one-line change.

```cpp
using UnitIndex     = std::uint32_t;  // 4.3e9 units; bump with the alias if ever needed
using ObjectIndex   = std::uint32_t;  // units + features + projectiles share one space (§6)
using Generation    = std::uint32_t;  // stale-handle detection; wraps harmlessly
using TeamIndex     = std::uint16_t;
using PlayerIndex   = std::uint16_t;
using AllianceIndex = std::uint16_t;
using UnitTypeIndex = std::uint16_t;  // 606 FA blueprints, 969 BAR defs — 65k is ample
using TickIndex     = std::uint64_t;  // 64 from the start: free, and replays concatenate
using TickCount     = std::uint32_t;  // a duration in ticks, always derived — never authored
using FxRaw         = std::int32_t;   // fixed-point storage, Q16.16
using FxWide        = std::int64_t;   // multiply intermediate
using StateHash     = std::uint64_t;

// Authoring-side only: a rate-independent duration, converted through TickRate (§5.1).
// Deliberately a distinct type so `rate.ticks(x)` cannot be handed a tick count by mistake.
enum class Seconds : float {};
```

**Any width change alters the replay hash**, so the replay format carries a version and a
widths fingerprint. Bumping `UnitIndex` to 64-bit invalidates old logs by design, loudly.

### 5.4 Dependencies — D7, and the biggest thing we do not copy

Recoil is built on global singletons. Twelve of them in the sim alone:

```
gs, gsRNG (Sim/Misc/GlobalSynced.h:136)   unitHandler   (Sim/Units/UnitHandler.h:123)
teamHandler   (Misc/TeamHandler.h:189)    quadField     (Misc/QuadField.h:253)
featureHandler, projectileHandler         moveDefHandler (MoveTypes/MoveDefHandler.h:248)
losHandler, pathManager                   plus readMap, globalRendering
```

Measured reach across `rts/` (1,463 files, `lib/` excluded): **`gs->` in 113 files,
`teamHandler` 78, `unitHandler` 69, `readMap` 65, `quadField` 41, `losHandler` 34.**

**Consequence one, and it is the one that matters to us: you cannot construct two sims in one
process.** That is not a purity complaint — it is our success criterion. The strongest form of
the determinism test is two `Sim` instances stepped side by side from the same command log,
comparing hashes each tick and stopping at the first divergence, in one debuggable process.
Recoil cannot do that, which is exactly why its own sync testing is a file diff between
separate runs (`tools/sync-test`, the 47,852-row reference in `08 §4.4`). We get the better
test for free by not taking the globals.

**Consequence two:** every test needs the whole world stood up. `08 §3.3` on the limits of
FAR's headless tests traces straight back to this.

**Consequence three:** a function's dependencies are invisible at its signature. You learn what
`CUnit::Update()` touches by reading it, and by reading everything it calls.

Our rule:

- **`Sim` is a value you construct.** It owns its stores. Two of them can exist at once; that
  is a test, not a hypothetical.
- **Every pass is a free function taking exactly the references it needs** (see ISP in §5.5).
- **No file-scope mutable state in `rm_sim`.** `constexpr` data tables are fine; anything with a
  mutable lifetime is not.
- **The RNG is a member**, seeded from the match setup, threaded through the passes that need
  it. Recoil's `gsRNG` is global, and its own `SimObjectIDPool` comment warns that touching the
  RNG a different number of times between clients desyncs them — a whole class of bug that a
  seeded member on an owned object cannot express.
- **Enforced in CI**, alongside the float ban (P0.4): grep `rm_sim` for file-scope non-`const`.

### 5.5 SOLID, applied — and where applying it would be a mistake

Worth being specific, because "apply SOLID" read naively produces an interface for every class
and a factory for every interface, which for a fixed-point data-oriented sim is pure cost.

**SRP — one pass, one rule.** The tick is a declared composition of small passes (§3.2). A pass
that both moves units and resolves collisions is two passes. This is also what makes the pass
list assertable in a test, which is the specific bug `Skirmish.hpp` was written to fix.

**ISP — the principle that pays most here.** A pass takes narrow views, never a god `World&`:

```cpp
// testable from two structs and a flat field
void tickMovement(std::span<Unit>, std::span<MoveState>, const Map&, TickRate);
// not testable without building a match
void tickMovement(World&);
```

We already have this instinct, arrived at locally: `CombatGroup` and `SkirmishGroup` are exactly
narrow span views over the caller's storage. Generalise it rather than replacing it.

**DIP — apply where something actually varies, and that list is short.** Honest inventory: the
unit catalog (const ref to data), the map (const ref), the tick rate (a value), the RNG (a
member). None needs a virtual. **An interface with one implementation is a cost, not a design.**
The one seam that earns itself in this project is the RHI (`ADR-032`), and it lives outside
`rm_sim` entirely.

**LSP — barely applicable, by construction.** Sim state has no inheritance: no vtables, so it
stays trivially hashable and fixed-size, which the replay hash depends on. Recoil reaches
polymorphism over `CSolidObject` → `CUnit`/`CFeature`; we reach the same capability with an
`ObjectId` and a kind tag (§6.6).

**OCP — the extension points are the pass list and the data, full stop.** Adding a behaviour
should be a data row plus at most one pass. Do **not** build a registration or hook framework;
that is the same argument as §3.3's, and Recoil's 17 `Allow*` hooks are what it looks like when
the answer is yes and you have no subscribers.

---

## 6. Borrowing from Recoil — and where we diverge

D3 and D8. **What we are borrowing is that Recoil is a working game with a working asset set:
its unit model, its economy, its pass order, its targeting rules, its build semantics — the
things that took twenty years to get right.** We are not borrowing its structure, which is
twenty years old in the other sense.

Each row below: what Recoil does, what we do, and why. **The "why we differ" column must be
repeated as a comment at each type's definition when it is written** — that is what lets a
future session (or LLM) tell a deliberate divergence from a mistake, which is the failure mode
this whole file exists to fix.

### 6.1 `SimObjectIDPool` → `IdPool`

*Recoil* (`Sim/Misc/SimObjectIDPool.h`): three `unordered_map`s — `poolIDs`, `freeIDs`,
`tempIDs` — with delayed recycling. Its constructor carries a determinism warning: *"pools are
reused as part of object handlers, internal table sizes must be constant at runtime to prevent
desyncs between fresh and reloaded clients (both must execute Expand since it touches the
RNG)."*

*Ours:* a flat `std::vector<Generation>` plus a `std::vector<UnitIndex>` freelist. An id is
`{index, generation}`, 8 bytes, copyable.

**Why we differ:** a generation counter makes a stale handle detectably stale *forever*, where
delayed recycling only narrows the window. And no hash container means no iteration-order
question at all — the exact class of thing Recoil had to write that warning about. Simpler and
strictly safer; we lose nothing, because the delayed-recycle machinery exists to serve a
problem generations don't have.

### 6.2 `CUnitHandler::unitsByDefs[team][defID]` → a bucketed index

*Recoil* (`Sim/Units/UnitHandler.h`): nested `std::vector<std::vector<std::vector<CUnit*>>>`,
sized MAX_TEAMS × def count, holding raw pointers. Gives O(1) "how many X does team Y have".

*Ours:* one `std::vector<UnitId>` sorted on a packed `(TeamIndex, UnitTypeIndex)` key plus an
offset table, rebuilt when the store changes rather than maintained incrementally.

**Why we differ:** the nested form is a large sparse allocation (teams × defs vectors, mostly
empty), and raw pointers mean every death needs bookkeeping in the index. Ids and a sorted
array give contiguous iteration, no dangling, deterministic order for free, and one allocation.
**Keep the capability, though** — it is what makes the AI's unit census, the HUD's counts and
the victory check O(1) instead of the full scan `main.mm:1085` does every tick today.

### 6.3 `CPlayer` / `CTeam` / `AllyTeam` → `Player` / `Team` / `Alliance`

*Recoil:* three levels. `CPlayer` (`Game/Players/`) is a *participant* — has `spectator`,
`rank`, `cpuUsage`, `desynced`; it is a connection, not a side. `CTeam` (`Sim/Misc/Team.h`)
owns units and resources, has `AddPlayer(playerNum)`, `maxUnits`, `Died()`,
`GiveEverythingTo()`. `AllyTeam` (`Sim/Misc/AllyTeam.h`) holds `std::vector<bool> allies` and
the start rects.

*Ours:* the same three levels, renamed, with `Alliance` for the third.

**Why keep three:** they are not redundant, and our current single `sim::Army` proves the cost
of collapsing them — its own header says *"deliberately not a player"*, which is the design
admitting a missing level. Several players can drive one team (a human plus a helper AI, or
shared control); a team is what owns units and banks resources; an alliance is what wins, and
what shares vision later.

**Why we differ:** (a) *Alliance* rather than *AllyTeam*, which reads like a kind of team;
(b) drop Recoil's `TeamBase`/`PlayerBase` split — those exist only to share fields with the
lobby and start-script path, which we do not have; (c) **naming note for future sessions: FA's
word for Recoil's Team is `Army`** (`ArmyIndex` throughout `lua/sim/`), which is where our
`sim::Army` came from. FA-derived data will say Army; our code says Team. Document the mapping
in one place or the two vocabularies will keep quietly disagreeing.

### 6.4 `CCommandAI` → `CommandQueue` plus a free function

*Recoil* (`Sim/Units/CommandAI/CommandAI.h`): a class hierarchy — `CCommandAI` →
`CMobileCAI` → `CBuilderCAI` / `CFactoryCAI` / `CAirCAI` — with virtual dispatch, a `CObject`
death-dependence graph (`AddDeathDependence`, `DependentDied`, `ClearCommandDependencies`), and
a static command-description cache.

*Ours:* `std::deque<Command>` on the unit; `Command` holds `ObjectId`, never a pointer;
dispatch is a switch on command type plus the unit's capabilities from the catalog.

**Why we differ:** most of that hierarchy's weight exists because commands hold **raw pointers**
to target units and features, so the engine must be told when a target dies. With ids, a dead
target is simply an id that fails to resolve at execution time — the entire dependence graph
evaporates. Domain specialisation (mobile vs factory vs builder) is a data property of the unit
type, not a subclass.

**Keep their semantics, though:** the cancel-queued rules (`WillCancelQueued`, `GetCancelQueued`,
`CancelCommands`) encode real behaviour that players feel — issuing a move while a build is
queued does something specific — and it is worth copying rather than re-deriving.

### 6.5 `QuadField` → `SpatialGrid`

*Recoil* (`Sim/Misc/QuadField.h`): allocates a result vector per query, served from a
`QueryVectorCache` pool that `assert(false)`s when exhausted.

*Ours:* bucket every object into a uniform grid **once per tick** by sorting on cell index, then
a query is a range scan over contiguous cells writing into a caller-provided span.

**Why we differ:** zero per-query allocation, cache-friendly iteration, and deterministic order
without effort. The reason Recoil cannot do this is that its objects move continuously *within*
a frame and it queries mid-update; our tick has a defined pass order, so the grid can be rebuilt
at one point and be authoritative for the rest of the tick.

**Why it matters now:** `nearestTarget` (`core/sim/Combat.hpp:140`) currently scans every group
for every shooter — O(n²). This is the single change that makes thousands of units affordable,
which is half the success criterion.

### 6.6 `CFeatureHandler` → one `ObjectId` space

*Recoil:* `CUnit` and `CFeature` both derive from `CSolidObject`, which is what lets a weapon
target either and lets collision treat them uniformly. Separate handlers, shared base class.

*Ours:* one `ObjectId` space covering units, features and projectiles; separate storage per
kind; a small tag says which. Targeting, collision, area damage and the spatial index all take
`ObjectId` and do not care.

**Why we differ:** same capability, reached by an id tag rather than inheritance — which is what
a fixed-point, data-oriented store wants (no vtables in sim state, no pointer chasing, trivially
hashable for the replay check).

**Why it matters now:** wrecks are currently `std::vector<DecalVertex>` — *GPU vertices* — held
in game state (`main.mm:1070`), because a death needed a scorch mark and there was nowhere else
to put it. Features being real objects is what fixes that, and reclaim later depends on it.

### 6.7 `SlowUpdate` → keep, but derive the period

*Recoil:* periodic per-unit work staggered across frames on a hardcoded 16-frame cycle.

*Ours:* same idea, stagger explicitly on `index % period`, with `period` derived from the tick
rate at load (§5.1) rather than written as a literal.

**Why it matters now:** `BuildOrder.cpp` already hand-rolls *"every second rather than every
tick"*. That is this pattern, discovered locally. Making it a mechanism stops the next four
features from each inventing it again.

---

## 7. The plan

Ordered by **what is expensive to retrofit**, not by what is interesting. Every item names its
automated test and its manual check; an item with neither is not ready to be an item.

### P0 — Harness first — **DONE 2026-08-20**, 557 tests green (from 527)

- [x] **P0.1 One tick order.** Done, and smaller than written: `tickSkirmish` was *already*
      wired into the headless `--march`/`--play` path — only the windowed loop ran the
      reduced set. The caller's side of a tick (opponents -> sim -> wrecks -> finished
      builds become units) is now `MatchRunner`/`advanceMatch` in `main.mm`, called by both.
      *Test:* `tools/check_sim_boundary.sh`, registered in CTest — no file outside
      `core/sim/` may call a mutating pass directly; verified to fail on an injected
      `resolveCollisions` call. *Manual:* `make match SECONDS=500` — the windowed loop now
      runs the opponents (`army 1 ATTACKS with 20 of 20 tanks`) and decides the match
      (`team 1 WINS`); before, it could produce neither line. Headless output unchanged.
- [x] **P0.2 State hash.** `core/sim/StateHash.{hpp,cpp}` — `hashMatch(groups, match)`,
      FNV-1a fed field by field (never a struct memcpy: padding is unspecified), floats by
      bit pattern, `-0.0f` normalised. *Presentation is excluded* — `animationPhase` and
      `teamColour` belong to the caller, and hashing them would make two runs of the same
      match disagree. *Test:* 11 cases in `tests/test_state_hash.cpp` — one-bit
      sensitivity, per-field coverage, order-sensitivity, and 20 ticks of two matches
      agreeing.
- [x] **P0.3 Hash log + `--hash-log` / `--check-hash-log`.** `core/sim/Replay.{hpp,cpp}`,
      text format, one hex hash per line so `diff` names the tick. The header carries the
      tick rate and a fingerprint of `Types.hpp`'s widths; a log recorded at another rate or
      under other widths is **refused, not rescaled** (§5.1). *Test:* 13 cases in
      `tests/test_replay.cpp` — first-divergence reporting, truncation, wrong-file, rate
      refusal. *Manual:* two independent 60s runs -> `determinism: MATCH — 600 ticks
      identical`; one flipped bit in the log -> `DIVERGED at tick 300`, both hashes shown.
      *Scoped honestly:* there are no player commands to log yet (P4), and a `--play` match
      is already a pure function of its setup, so a hash log is a complete record of it
      today. Commands append to this format when they exist.
- [x] **P0.4 `core/Types.hpp`** (§5.3) + `tools/check_no_sim_globals.sh` in CTest, verified
      to catch both a file-scope definition and a mutable `static` local. *Test:*
      `tests/test_types.cpp` pins the widths, which is also what compiles the header.
      **The float ban is NOT registered** — `core/sim` is entirely float until P2.2 migrates
      it, so the check would be red for the wrong reason. It belongs to that commit; the
      deferral is recorded in `CMakeLists.txt` beside the checks that did land.
- [x] **P0.5 `AGENT.md` points at the analysis**, with a routing table for the seven
      questions that recur. Committed separately as `a0f6b72`.
- [~] **P0.6 Divergence detection — working; the in-process form deferred.**
      First-divergence reporting is proven end to end (P0.3's manual check) and the
      no-globals check guarantees two sims *can* coexist. What is **not** built is the
      app-level "two `Sim` instances stepped side by side": that needs two full VFS scenes
      and there is no `Sim` object to construct twice — it arrives with P1/P4. Two
      invocations plus `--check-hash-log` gives the same verdict today; the single-process
      form is a debugging convenience worth having once it is cheap.

**Found while doing it, both fixed:** a fresh `MatchRunner` over an already-decided match
re-announced the winner, and a decorative `--units` crowd (no armies, so `survivorCount`
is 0, which is also `<= 1`) would have declared a DRAW on tick one — `Match::over` is now
seeded from the scene using the same predicate the sim decides on. And `advanceMatch` left
`groups` holding spans into vectors its own spawns had reallocated; harmless while the only
reader was the next tick, an immediate segfault for the first reader that looked straight
after the call. Rebuilt at the end of the call instead of the top of the next.

### P1 — Identity and storage — **done 2026-08-20**

**P0 changed the character of this phase.** This file originally said P1 "cannot be done in
small safe steps" and to "expect the app to be broken in the middle of it". That was true
when the only way to know whether a refactor had changed behaviour was to play the game. It
is no longer: a hash log recorded before the first edit is a statement of exactly what the
match does, and every step can be checked against it. The phase is still large, but it is now
*verifiable* at each step rather than at the end.

The technique, and it is the whole reason to do P1 next rather than P2:

- [x] **P1.0 Record the golden log first.** `--play 300 --hash-log docs/golden-p1.log` on a
      fixed map and army count, committed alongside the code. This is the definition of "P1
      changed nothing", written down before anything moves.
      *Test:* re-running it reports `MATCH`. *Manual:* the file exists and is in the commit.

      Then, after each step below: replay and require `MATCH`. A step that changes the hash
      has changed the match, and the tick number says where to look. **Keep iteration order
      identical** through the whole phase — the hash is order-sensitive on purpose (§6.2), so
      reordering units is a behaviour change even when every value is right. Optimise the
      order afterwards, deliberately, with the log re-baselined in its own commit.

      One caveat to plan for: `hashMatch` currently takes `span<const SkirmishGroup>`, so when
      the store replaces the groups its signature changes and the format version bumps. Keep
      the *sequence of fields fed* identical across that change and old logs stay comparable;
      change both at once and they do not.

- [x] **P1.1 `IdPool`** (§6.1). *Test:* a dead unit's id never resolves to its successor; an
      order held across a death fails cleanly. *Manual:* kill a selected unit, no ghost.
- [x] **P1.2 `UnitStore`.** `Unit` holds type, owner, position, orientation, health, motion,
      build state. Buildings are units with a build state, not a second kind. *Test:* 1,000
      units, kill every third, assert iteration and lookup. *Manual:* `--units 5000` renders.
- [x] **P1.3 Bucketed index by team and type** (§6.2). *Test:* counts match brute force after
      10,000 random spawns and deaths. *Manual:* HUD counts stay right through a battle.
- [x] **P1.4 Delete `UnitRef{batch, instance}`** and the batch-indexed sim spans.
      *Test:* suite passes with the type gone. *Manual:* full `--play`.

      **Done 2026-08-20. It changed the match exactly as predicted: the golden log diverged
      at tick 0, and was re-recorded in the same commit. Everything below was written before
      the work started; it is left as written because the prediction held.**

      Why: `nearestTarget` documents its tie-break as *"ties break on the lower batch then
      the lower instance"* (Combat.hpp), and spawning appends into per-blueprint batches, so
      units of one model are contiguous. A flat store's slot order is pure spawn order,
      interleaved across types. Different order, therefore different winner of a tie,
      therefore a different target, therefore a different match. Collision shoving and damage
      application order shift for the same reason.

      This is a change of *outcome*, not of *property*. The tie-break exists so that "the
      same scene always picks the same target and a screenshot proves something twice" —
      lowest-slot serves that intent exactly as well as lowest-batch. The sim stays
      deterministic; it just plays a different, equally valid match.

      So the verification for this item is not `MATCH`:
      - the suite passes with `UnitRef` gone (the stated test)
      - the match still completes, decides, and reports comparable totals — shots fired,
        units destroyed, HP remaining, the tick someone wins on. Comparable, not identical.
      - `make golden` re-records, **in the same commit**, with the reason in the message.
        That is what `make golden` is for and the only legitimate use of it.

      **What it actually cost, against the estimate above:** 601/601 green, all three
      verifications met. The estimate of ~220 sites was about right in shape and wrong about
      where the work was: the sim passes got *shorter* (every `for group / for instance`
      became one loop over slots) and the test files were the bulk of the churn.

      **The match came out identical, not merely comparable** — 567 shots, 24 destroyed, team
      1 at 502.2s, all three the same as the pre-P1.4 run. Only the per-tick hash moved, which
      is the predicted consequence of slot order plus the three new per-slot fields. The bar
      was "comparable"; the outcome exceeded it, and that is worth knowing: on this map the
      targeting ties that reorder never decided anything.

      **A defect the invariants test found, which the golden log never could.** `hashMatch`
      fed each slot's generation but not its LIVENESS, and `kill` deliberately advances
      neither the arrays nor the generation mirror — a stale mirror is what makes a dead
      unit's handle fail `alive`. So a unit killed at full health changed nothing in the
      hash: the fingerprint could not see a death. It went unnoticed because in a real match
      the values move anyway (`retireDead` zeroes the radius, and health had to reach zero to
      get there). Fixed by feeding `slotAlive(slot)`. This is the case for property tests
      over regression logs: a log records what the code did, including what it failed to
      notice.

      **Two more defects found in the process, both of the same kind — a count taken by
      scanning storage, which slot reuse invalidates:**

      - The end-of-match summary counted units destroyed with `deadUnits(store)`, a scan for
        slots whose health is zero. A corpse's slot is reused by the next spawn, so the scan
        undercounted every recycled death: 21 reported against 24 scorch marks, and the marks
        were right because they accumulate from the tick's death reports. `MatchRunner` now
        totals deaths from the reports. **A total that only goes up cannot be undone by
        storage reusing a slot** — the rule to apply to every other running total.
      - `test_match_invariants` keyed "a death is reported exactly once" by SLOT, which would
        false-positive on the first recycled slot. Two reports of one corpse carry the same
        handle; two occupants of one slot do not. Keyed by the whole handle now.

      **And one defect introduced and caught before commit:** `Renderer::setUnits` sizes a
      batch's instance buffer from `batch.instances.size()`, and `spawnUnit` used to re-point
      that span on every spawn — so a batch created mid-match was never empty by the time
      `setUnits` saw it. With no per-batch vector to re-point, a newly built unit type got a
      capacity of zero and would never have drawn. `gatherForDrawing` now re-points every
      batch, so the invariant holds wherever the gather is called rather than at four call
      sites that must remember.

      **One structural change beyond the item:** `applyClick` is now a template over the
      identity type, so a selection holds `UnitId` and `SelectionEntry{batch, instance}`
      survives only as the renderer's draw-time projection (`UnitScene::drawnAt`). And the
      three test files that each had their own copy of the same unit fixture now share
      `tests/support/TestRoster.hpp` — three copies of one fixture was three chances to
      migrate it differently.

      **Measured, not estimated: ~220 use sites.** ~130 across 15 files in the sim and its
      tests, plus 91 more in `main.mm` alone touching the parallel arrays. `UnitScene`'s
      members are referenced from `main.mm` 39 times for `instances`, 26 for `batches`, 23
      for `motion`, 18 for `defs`, 14 for `health` — and the renderer's per-batch instance
      upload has to become a gather, which is P7's snapshot arriving early.

      **Why it was not started rather than half-started:** the item is atomic — flat store or
      batch groups, no coherent middle — so a migration that runs out of runway leaves the
      repo worse than either end. Combined with the golden log being unable to verify it,
      starting without finishing is the one outcome to avoid.

      **What now exists for it, which did not before:** `tests/test_match_invariants.cpp` —
      twelve properties true of any correct match, none of which mention an order, a batch or
      a slot. A migration that reorders units keeps all twelve; one that loses a unit,
      resurrects a corpse, double-reports a death or leaks a projectile breaks one at once.
      That plus a comparable match summary is the gate, and it is now in place.

      Scope of the original estimate: ~130 use sites across 15 files. Heaviest are
      `tests/test_combat.cpp` (29), `Combat.cpp` (25), `Skirmish.cpp` (22). The whole
      per-batch grouping exists because `def` is per batch, so the enabling move is putting
      the type on the unit and a `UnitCatalog` behind it — at which point `SkirmishGroup` and
      `CombatGroup` collapse into "the store" and every nested `for group / for instance`
      becomes one loop over slots. The passes get shorter, not longer.

### P2 — Fixed-point (D1) and ownership

- [x] **P2.1 `Fx` type + fixed-point trig/sqrt tables** (§5.2) — **done 2026-08-20.**
      *Test:* golden values, plus `tools/check_fx_optimisation.sh`, which is the real gate —
      §7's "identical at `-O0` and `-O2`" is a claim about the COMPILER that no test linked
      into one binary can make, so a probe over ~200,000 results is built at `-O0`, `-O1`,
      `-O2`, `-O3` and `-O3 -ffast-math` and required to agree. All five do.

      **NOT tables, and not Q16.16.** Two deliberate divergences, both recorded in
      `core/Types.hpp` and `core/sim/Fx.hpp`:

      - **Layout is Q18.14 plus a 64-bit `Mag`, not Q16.16.** Forced by measurement, not
        taste. Three shipped maps are **32,768 × 32,768 elmos** (SCMP_029, SCMP_030,
        X1MP_012 — the 81 × 81 km size), which is *exactly* Q16.16's ceiling, so a unit in
        the far corner is unrepresentable; and `BuildCostEnergy` reaches **10,008,000**
        (XSB2401) with `MaxHealth` at **5,000,000** (XSC9010), which no 32-bit fixed-point
        layout holds. Both types share `kFxFractionalBits`, so converting between them is a
        widening rather than a rescale — the mistake `recoil-engine-map.md §2` records in
        Recoil's own content format.
      - **CORDIC, not lookup tables.** One algorithm gives sin, cos, atan2 and hypot; a
        table's accuracy is fixed at authoring time, while CORDIC converges below the
        output's last bit (worst error against `libm` over the whole circle: 1.2e-8, about
        1/5000th of a step). Angles are `Brad` — turn/65,536 — because wrapping fixed-point
        radians needs a modulo by 2π, an irrational no fixed-point type holds, so the wrap
        itself would accumulate error.

- [x] **P2.2 Migrate sim state to fixed-point** — **done 2026-08-20.** *Test:* sim tests
      pass with re-based constants; the P0.2 hash is identical across `-O0`/`-O2`/arm64/x86-64.
      *Manual:* `--play` unchanged.

      Done in two commits, each with the whole suite green and the match unchanged in every
      reported total (567 shots, 24 destroyed, 20600/38900 hp, 37 of 37 builds, team 1 at
      502.2s). The golden log was re-recorded both times because the FINGERPRINT changed —
      `hashMatch` feeds raw integers where it fed float bit patterns — which is a different
      description of the same 5200 ticks and exactly what §11 predicted P2 would do to it.

      - [x] **Health, damage and costs** — `Health`, `Weapon::damage`, `UnitDef::health`,
            `Projectile::damage`, `TickReport::deathBlastDamage`. Content converts at parse
            time; the blast falloff stays `Fx` because a fraction of a radius is geometry, and
            `Mag * Fx` is the one operation that mixes the two.
      - [x] **The economy** — `Resources`, `Economy`, `Construction`, and `UnitDef`'s cost
            fields. Rates are now **per tick, derived once** in `UnitCatalog::Rates`;
            `tickEconomy` mentioned `kTickSeconds` four times and now mentions it nowhere.
            One invariant got *stronger*: the "store never exceeds its cap" bound lost its
            `+ 0.001f` of slack, because fixed point cannot leave a value a hair above a clamp.
      - [x] **Position, orientation and velocity.** THE ATOMIC ONE. `UnitInstance` is the GPU's layout — pinned by a `static_assert`,
            read verbatim by the vertex shader — so it cannot hold `Fx`. The sim needs its own
            `Transform{Fx x, y, z; Brad heading, pitch, roll;}` as the authority, with
            `UnitInstance` becoming a draw-time projection built by `gatherForDrawing` from
            the transform plus the type's scale and the army's colour. That is **P7's snapshot
            arriving early**, forced here the same way P1.4 forced the gather.

            Scope, measured: `Movement.cpp` (54 float mentions), `Combat.cpp` (49), plus every
            `instances[slot].position` read in the passes and the whole of `main.mm`'s spawn
            and draw path. Estimate it in sessions.

            **It was atomic in the way P1.4 was** — flat store or batch groups, no coherent
            middle — and it landed in one pass. `sim::Transform` is the authority;
            `UnitScene::instanceFor` is the whole projection; `paceAnimationByDistance` and
            `paceSceneAnimations` are gone as a consequence, because the phase was always
            derived and there was nowhere left for a separate pass to write it.

            **`Brad` angles paid for themselves immediately.** `shortestAngleTo` was six lines
            of `fmod`; `headingError` was eight. Both are a subtraction and a cast now, because
            the difference of two `Brad` in 16 bits IS the shortest way round.

            **The match went from identical to comparable, as predicted:** 566 shots against
            567, same 24 destroyed, team 1 at 502.5s against 502.2s. Quantising positions moved
            one shot by three ticks.
      - [x] **Pathfinding, build placement and heightfield sampling.** `sim::Terrain` samples
            the same grid with the same bilinear arithmetic, so the sim's ground and the
            renderer's cannot disagree. Pathfinding split along the line that was always there:
            grid CONSTRUCTION reads float heights at load and stays float; the queries that run
            inside a tick are fixed point.

            **One number needed measuring, and the first answer was wrong.** Holding the
            vertical scale as an `Fx` looks obviously right and is not: a real `.smf` states
            ~0.01 elmos per raw unit, which quantises to one part in a thousand in Q18.14 and is
            then multiplied by a raw of up to 65,535 — 0.17 elmos of error, seven hundred times
            the type's resolution. The scale is kept to 2^-30. **A small factor multiplied by a
            large operand needs more fractional bits than the product does**, and that lesson is
            in the header because it recurs.
      - [x] **Register the float ban** (`tools/check_no_sim_floats.sh`). Registered. It bans
            float variables, parameters, returns and casts, and permits `constexpr float`
            constants (a constant cannot vary between platforms, so it cannot diverge) plus a
            named list of boundary files, each with its reason in the script. Verified against
            an injected violation.

- [x] **P2.3 Tick rate as configuration** (§5.1) — **mechanism done 2026-08-20**, landed early
      because P2.2's conversions need it. `core/sim/TickRate.hpp` is a validated value (5–50 Hz,
      throws rather than clamps), rounding to nearest with a floor of one tick, and it is
      *passed* to the passes that need it rather than read from a global.

      **Both constants the rule was written against are gone.**
      `kProjectileLifetimeTicks = 300` → `kProjectileLifetime = Seconds{30}`;
      `TickClock::kMaxTicksPerAdvance = 5` → `kMaxCatchUp = Seconds{0.5}`.
      Worth recording: the comment on the second said *"a tenth of a second of catch-up"* and
      was wrong about its own value — five ticks at 10 Hz is half a second. The rate was in a
      comment, and the comment was mistaken. That is the failure mode the rule exists to
      remove, caught in the act. `tools/check_no_tick_literals.sh` is registered as a test.

      `Seconds` is a **struct**, not §5.3's `enum class Seconds : float` — an enumeration's
      underlying type must be integral, so that declaration is not legal C++. The plan is
      wrong on that detail; a one-member struct with an explicit constructor gives the same
      property at the same cost.

      **Done 2026-08-20.** `--tick-rate N` sets it, and `tests/test_tick_rate_invariance.cpp`
      is §5.1's stated test: the same work at 5, 10, 20 and 50 Hz, with every observable
      duration inside one tick of its authored value in seconds. All five cases pass at all
      four rates. Measured end to end on a real match: first extractor at 5.8 / 5.9 / 6.0 /
      6.0s against an authored 6, tank cadence 14.0s at every rate, 24 units destroyed at every
      rate.

      **AND RUNNING IT FOUND THREE REAL BUGS**, all the same shape, none of which any test or
      check had caught — which is precisely why §5.1 calls this "the only way this rule stays
      true a year from now":

      - `--play 520` converted seconds to ticks with `kTicksPerSecond`, so it ran 5200 ticks at
        every rate. At 20 Hz that is 260 seconds of match, and the first `--tick-rate 20` run
        produced a match in which nothing was built and nobody fired.
      - `kDecisionTicks = rm::sim::kTicksPerSecond` — the opponents' thinking cadence. It LOOKS
        derived and is not: it names the default rate. `check_no_tick_literals.sh` now catches
        that shape too, which cost a real bug to learn.
      - Three `catalog.add(def)` calls defaulted to 10 Hz, so a type's reload and income were
        derived at one rate while its units moved at another.

      `gAppTickRate` is a mutable file-scope value in `main.mm`, and its comment says so rather
      than hiding it: `core/sim` holds no such thing, which is what lets the test run four rates
      in one process. Twenty-one sites there convert a content rate; threading a parameter to
      all of them is work P7.5 throws away.
- [x] **P2.4 `Player` / `Army` / `AllianceIndex`** (§6.3) — **done 2026-08-20.** *Test:*
      `tests/test_players.cpp` — victory fires on alliance elimination, not army elimination;
      two players sharing an army both command its units. *Manual:* `--armies 4 --alliances 2`,
      which gives 1249 shots and 78 destroyed against the free-for-all's 1706 and 93, because
      allies do not shoot each other.

      `Player` is the level §6.3 called out as missing, and it was right: `Army.hpp` said
      "deliberately not a player", which is the design admitting the gap. `winningTeam` is
      `winningAlliance`, and a free-for-all HIDES whether that reads the right level — one army
      is one alliance there — so the test needs a 2v2.

      **THE NAMING DIVERGES FROM §6.3, DELIBERATELY.** The plan says call the middle level
      `Team`, following Recoil. We keep `Army`, following Forged Alliance, because FA is the
      content we read: `ArmyIndex` runs through all of `lua/sim/`, and `mapinfo.lua` uses `team`
      for what we call an ALLIANCE. Renaming would align 477 call sites with an engine we load
      no content from, misalign them with the one we do, and make our `team` mean the opposite
      of the map file's. The three-way mapping table in `Army.hpp` is the "one place" §6.3 asks
      for; if a future session prefers Recoil's spelling, the cost is in the table.

      `AllianceIndex` stays an index rather than a struct with one member. It becomes one when
      it holds shared vision.

- [x] **P2.5 Human input becomes a command source** — **done 2026-08-20.** *Test:*
      `tests/test_command.cpp`. *Manual:* `--command-log`, which a 520-second match fills with
      41 orders over 5029 ticks.

      `orderRouted` is deleted and all four order sites — right-click, scripted attack wave,
      factory rolloff, `--march` — go through `applyCommand`. "The same path" is structural
      now: there is no other path.

      Three rules the single path makes enforceable, each tested: a player cannot order another
      army's units; a defeated army takes no more orders; a STALE HANDLE is refused rather than
      resolved to whoever inherited the slot. That last is the case that makes
      generation-tagged handles worth having.

      §1.3's criterion — "the same command log produces the same match" — named something that
      did not exist until this item. Both halves exist now.

      One gap, named: `Build` commands are recorded but not applied, because a construction
      needs a blueprint the sim cannot reach. It moves into `applyCommand` in P3.

### P3 — The data layer: game rules out of C++

- [x] **P3.1 Role classification from categories,** using `07-ai-and-gamesetup.md §4.4`'s
      vocabulary — `commander, builder, factory, extractor, energy, storage, defence, raider,
      assault, artillery, antiair, air, naval, scout, transport, shield, radar, experimental` —
      chosen there as a superset of BAR's `ai_simpleai.lua` inference and a subset of
      CircuitAI's enum. *Test:* real blueprints — `URL0001`→commander, `URB1103`→extractor,
      `URL0105`→builder, `UEB0101`→factory. *Manual:* `--dump-roles`.
- [x] **P3.2 `BuildableCategory` expression evaluator.** SupCom ships no build lists, it ships
      expressions: `{"BUILTBYTIER1ENGINEER CYBRAN"}`, **space = AND, list = OR** (`07 §4.3`).
      That report calls materialising them *"a hard blocker for any skirmish, not just for the
      AI"*. *Test:* hand-written expressions, then a real-corpus test that a T1 engineer's set
      holds its own faction's mex, pgen and land factory and nothing of another's.
      *Manual:* `--dump-buildtree URL0105`.
- [x] **P3.3 Delete the hardcoded blueprint paths and wave constants.** `BuildOrder.hpp`'s four
      `k*Blueprint` paths and `kAttackWaveTanks` become a data file selecting by *role*.
      *Test:* one build order drives UEF and Cybran from data alone. *Manual:* `--play` as each.
- [x] **P3.4 MoveDefs by name, from motion class.** Two corrections from `01 §3.4` and
      `recoil-engine-map.md §5`: mobile units ignore the unitdef's `maxSlope`/`maxWaterDepth`
      entirely — those govern *building placement* — and **all four factions' ACUs are
      `RULEUMT_Amphibious`** (`14 §9.1`), so amphibious is a fixed cost, not a later feature.
      `ADR-027` says passability comes from motion class; this makes it true. *Test:* water that
      blocks a land unit and passes an amphibious one, same map. *Manual:* walk the commander
      across a bay, then a tank.
- [x] **P3.5 FA duration correction** — **done 2026-08-21.** `11 §2.1`: FA's `WaitSeconds(n)` is `WaitTicks(n*10+1)`,
      so every Lua-timed duration is quantised to 100 ms **and inflated by 100 ms** —
      `WaitSeconds(1.0)` waits 1.1 s. The transform is
      `t' = (t <= 0.1) ? 0.1 : (floor(t*10) + 1)/10`, and without it *"every weapon's DPS is
      wrong by up to 50 % on burst weapons"*. Applies when importing FA content at any tick
      rate — it is a fact about how FA's numbers were authored, not about our rate.
      *Test:* table-driven: `0.05→0.1`, `0.2→0.3`, `1.0→1.1`. *Manual:* `--dump-weapon URL0202`.
- [x] **P3.6 `SlowUpdate`** (§6.7) — **done 2026-08-21.** *Test:* every unit visited exactly once per period, even
      per-tick load. *Manual:* `--bench` shows no periodic spike.

### P4 — Orders

- [x] **P4.1 `CommandQueue`** (§6.4) — **done 2026-08-21**: queue, shift-append, cancel-queued rules, finish.
      *Test:* three queued moves run in order; a plain order replaces, a shift-order appends.
      *Manual:* shift-click a route.
- [x] **P4.2 Every input path goes through commands** — **done 2026-08-21.** *Test:* the scripted opponent's order and
      a synthetic click produce identical hashes. *Manual:* replay divergence check.

### P5 — Spatial queries

- [x] **P5.1 `SpatialGrid`** (§6.5) — **done 2026-08-21.** *Test:* results match brute force on 10,000 randomised
      layouts — the ideal test shape, since the slow version is the oracle.
      *Manual:* `--bench --units 5000`.
- [x] **P5.2 Route targeting, collisions and area damage through it** — **done 2026-08-21.** *Test:* combat tests pass
      unchanged. *Manual:* frame time at 2,000 units drops; targeting behaves the same.

### P6 — Events, and effects out of the sim

- [x] **P6.1 `EventQueue`** — **done 2026-08-21** — our subset of the ~130-call-in vocabulary (`04 §4`), sized to what
      exists: `UnitCreated, UnitFinished, UnitDamaged, UnitDestroyed, UnitTaken, WeaponFired,
      ProjectileImpact, ConstructionStarted, ConstructionFinished, TeamDefeated, GameOver`.
      *Test:* a kill emits exactly one `UnitDestroyed` with the right instigator.
      *Manual:* `--print-events`.
- [x] **P6.2 `FeatureStore`** (§6.6) — **done 2026-08-21**; `wreckDecals` deleted; the renderer makes decals from
      `UnitDestroyed`. *Test:* a death creates one feature at the right position; the sim links
      no GPU type. *Manual:* scorch marks still appear.
- [x] **P6.3 Particles, dust and death blasts move behind events** — **done 2026-08-21**, with the effect half smaller than the item implies (see §11). *Test:* `rm_sim` compiles
      with `core/scene/` off its include path — the real assertion. *Manual:* unchanged.

### P7 — The render side

- [x] **P7.1 `Snapshot` + `snapshot(const Sim&)`** — **done 2026-08-21.** *Test:* pure; same sim, same bytes; never
      mutates. *Manual:* none.
- [x] **P7.2 Interpolation; `UnitInstance` becomes render-only** — **done 2026-08-21.** *Test:* alpha 0 and 1
      reproduce the endpoints exactly. *Manual:* visibly smoother; `--no-interpolate` for
      captures.
- [x] **P7.3 Split `Renderer.mm` (4,267 lines)** — **done 2026-08-21**, into `MapRenderer`, `UnitRenderer`,
      `FxRenderer`, `UiRenderer` — Recoil's own split (`CBaseGroundDrawer`, `CUnitDrawer`,
      `CProjectileDrawer`, `CMiniMap`). *Test:* unchanged golden screenshots.
- [x] **P7.4 Minimap** — **done 2026-08-21.** Nearly free once a `Map` object and a snapshot exist; today there is
      nothing to read from. *Test:* world→minimap projection round-trips. *Manual:* click it.
- [x] **P7.5 `main.mm` to argv plus wiring** — **done 2026-08-21**, under 400 lines from 3,948. *Test:* the suite
      covers what moved out. *Manual:* every flag still works.

### P8 — Scale

- [x] **P8.1 5,000 units at the configured rate** — **done 2026-08-21**, measured at 12.9x real time. *Test:* perf regression gate.
      *Manual:* `--bench`.

**RENUMBERED, and the reason is a decision rather than tidying.** This was P8.3, behind a
Linux build and a cross-architecture hash. Those two are now **P9, and P9 is frozen** — see
below. What is left in P8 is the half of the success criterion that can be met on one machine,
and it goes first because it is reachable: P5 took 5,008 units from 73.7 s of headless match to
7.0 s, so the gate is a measurement rather than a project.

### P9 — The cross-architecture proof — **FROZEN**

- [ ] **P9.1 Linux-x86-64 headless sim build** through `ADR-032`'s RHI seam — sim and tests
      only. *Test:* CI on both. *Manual:* it builds.
- [ ] **P9.2 Cross-architecture replay hash.** Same log, both platforms, identical per-tick
      hashes. *Test:* this *is* the test. *Manual:* the number goes in the README.

**FROZEN, deliberately, and it is worth being exact about what that costs.** §1.3's success
criterion has two halves — "the same command log produces the same match" and "on any
machine" — and freezing this phase means the second half stays unproven. Everything the
determinism harness reports is still ONE MACHINE AGREEING WITH ITSELF.
`check_fx_optimisation` shows the arithmetic survives `-O0` through `-ffast-math`, which is a
real result and is not the claim. **Nothing in the README or the about page may say otherwise
while this phase is frozen.**

Why freeze it rather than do it. The work is not sim work: it is a build system, a second
toolchain, CI, and `ADR-032`'s RHI seam — which is a rendering abstraction, so a headless
Linux build needs the renderer separated first (that is P7). Fixed point is what made the
claim *reachable*, and it is reachable whenever this thaws; nothing in P1–P8 has to be redone
to get there. The phase is a day of infrastructure standing between here and a sentence in a
README, and the sentence is not worth the day yet.

What would thaw it: a second machine to run it on, or a reason to trust the number more than
the design — a networked game, a shared replay, anyone else's hardware.

### P10 — Depth: what an RTS needs that this sim cannot yet express

**WHY THIS PHASE EXISTS.** §2's Sim row reads 73 %, and that number is honest about
*foundations* and quietly optimistic about *gameplay*. A review against Recoil on 2026-08-21
(`REVIEW.md`) separated the two: the foundations — fixed point, no globals, generational
handles, a declared pass order, a spatial index — are at or above 73 % and in four places
beat the reference engine outright. The gameplay surface is nearer 25 %, and the gap is not
spread evenly. It is four specific things, each of which makes the next one cheaper, and
three of which get **much more expensive to retrofit the longer they wait**.

**THE ORDER IS RETROFIT COST, not value.** Damage goes first not because armour classes are
the most exciting feature but because every combat call site grows an argument, and there are
fewer of those today than there will ever be again.

- [x] **P10.0 The float ban's blind spot** — **done 2026-08-21.** `check_no_sim_floats.sh` finds floats by grepping
      for the *tokens* `float` and `double` in declaring position, so it cannot see a
      *conversion call* — and there is one, in the sim, on the determinism-critical path:
      `Command.cpp` builds `Construction::position` with `fxToFloat`, and `StateHash.cpp`
      then feeds that triple as raw IEEE bits. Not a live desync (an `int32`-to-`float`
      conversion is IEEE-defined and reproducible), but §2 says the sim is "fixed point END
      TO END (enforced)" and it is not quite: one float triple survives, and the guard
      structurally cannot see the code that fills it. Widen the pattern to catch
      `fxToFloat`/`static_cast<float>`, then finish P2.5 and make the field `array<Fx, 3>`.
      *Test:* the check itself, with a deliberately added conversion as a negative case.
      *Manual:* the screenshot hash is unchanged across the change.

      **THE ACCEPTANCE CRITERION AS FIRST WRITTEN WAS WRONG, and is kept rather than edited
      away.** It said "`make verify` MATCHes — this must move no hashes", which is impossible:
      the state hash *is* a walk over the bytes of sim state, so changing a field from three
      floats to three `Fx` moves it by construction, at tick 0. The criterion confused *the
      match plays the same* with *the fingerprint is the same* — precisely the distinction the
      harness exists to keep apart, written by someone who had just spent a day reading it.

      What was used instead is P7.3's technique and it is the stronger one: **hash the
      screenshot.** `0a430d44ee89ea9a3868f17d5caa0cdb6b7bd9d650cce465592c02fb5ff37a97` before
      and after, so 7,000 ticks of a decided match render pixel-for-pixel identically while the
      state hash moves. That is what "a representation change, not a behaviour change" looks
      like when it is proven rather than asserted, and it is the right shape for every future
      width change — including the `UnitIndex` widening `core/Types.hpp` warns about.

      **A second guard came free.** With the field fixed point, `StateHash.cpp`'s
      `feed(StateHash&, float)` had no callers, and `-Werror=unused-function` refused to build
      until it was deleted. So re-introducing a float into hashed state now fails to compile
      until someone puts that function back — a deliberate act rather than an oversight, and a
      stronger guarantee than the grep gives.

- [x] **P10.1 Armour classes and the damage profile** — **done 2026-08-21** (`cc930ce` the type, `c46f603` wired; `make verify` MATCHed because a catalog with no armour context reproduces the pre-P10.1 engine byte for byte).
      Original item: (`ADR-033`, D12). `ArmorRegistry`
      interning sorted case-folded names to a dense `uint8`; `DamageProfile` as a base `Mag`
      plus a short inline override list; `damageArea` taking a profile with the existing
      `Mag` signature kept as a wrapper so the two dozen falloff tests are untouched.
      Importers: BAR's `damage = {class = n}` reads directly, FA's
      `[ArmorType][DamageType] -> multiplier` is transposed and flattened at parse.
      *Test:* the FA matrix round-trips — an Overcharge shot does quarter damage to a
      `Structure` and full damage to `Normal`; an unknown class name falls back to `default`
      rather than to zero. *Manual:* `--dump-weapons` shows a commander's Overcharge with its
      overrides listed.

- [ ] **P10.2 Shields, as an armour class.** Nearly free once P10.1 lands, because that is
      how Recoil does it — `PlasmaRepulser.cpp:201-202` reads
      `damageArray.Get(weaponDef->shieldArmorType)`, so a shield is not a special case in the
      damage path at all. *Test:* a weapon with a shield-class override is absorbed at a
      different rate than the same weapon against the hull. *Manual:* a shielded unit under
      fire.

- [ ] **P10.3 Projectile kinds** (`ADR-034`, D13). A `ProjectileKind` tag and a flags
      bitmask on the same flat struct: tracking, torpedo and semi-ballistic motion, a
      target-layer mask, `targetable`/`interceptor` bitmasks, and a proximity fuse — which is
      one field here and does not exist in Recoil at all (`13 §1.5`). *Test:* an
      anti-missile intercepts a tactical and ignores a torpedo; a proximity-fused AA shell
      detonates above its target. *Manual:* the state hash still walks a `Projectile` with no
      visitor — if it needs one, D13 has been broken.

- [ ] **P10.4 A cost field, not binary passability** (`ADR-035` layer 1, D14). One byte per
      cell per motion class; 0 impassable, otherwise a speed divisor. Fixes the current rule
      on its own: `buildPassability` marks a 64-elmo cell impassable if *any* of its 64
      squares is. *Test:* a route through a gap that the binary grid refuses. *Manual:* a
      unit crosses a bridge one square wide.

- [ ] **P10.5 Shared flow fields and a dynamic blocking overlay** (`ADR-035` layers 2-3).
      Integer Dijkstra from the goal, goals snapped to a coarse cell so nearby clicks share
      one field, LRU cached; static terrain cost and dynamic blocking kept as separate layers
      so a placed building dirties only the fields whose region it touches. Removes the
      per-unit stored path, and with it a variable-length run from the state hash.
      *Test:* fifty units to one point compute **one** field, and a building placed across a
      route makes the units go round rather than through. *Manual:* `--bench` — this should
      show up as a drop, not a wash.

- [ ] **P10.6 Threading, path work first** (`ADR-036`, D15). Fork-join, per-slot writes,
      application in slot order, pool sized to the *performance*-core count. *Test:* the
      replay hash is identical single-threaded and multi-threaded, and identical across two
      different pool sizes — that equality **is** the test, and if it ever fails the rule in
      D15 has been broken somewhere. *Manual:* `--bench` at 5,000 units.

- [x] **P10.7 The event queue's frame boundary** — **done 2026-08-21.** `REVIEW.md` §5.7. The
      queue's lifetime was a convention described in prose across two headers: `Events.hpp` said
      it is cleared every tick, `Skirmish.hpp` said the CALLER clears and not the tick. Both
      accurate, and together a trap — the thing they described had no name in the code, so the
      only way to know where the boundary was is to read both notes and believe them. It had
      already gone wrong once, and the cost was two event kinds declared, emitted and never
      observable, with nothing failing: a lost notification looks exactly like one nobody sent.

      `clear()` is replaced by `beginFrame(TickIndex)` and there is no second way to advance.
      Two properties, and **the second is the one that matters**: the frame carries the tick it
      belongs to, so a consumer can ask `frame()` instead of trusting that somebody advanced it;
      and **advancing is idempotent**, so a second caller beginning the frame it is already in
      destroys nothing. Had the tick called `beginFrame` rather than `clear`, the original
      defect would have been a no-op. The first advance always clears whatever tick it names, so
      a queue reused for a second match does not carry the first one's events in — two matches
      both starting at tick 0 is ordinary, not a corner case.

      *Test:* four, and the load-bearing one is "beginning the frame you are already in destroys
      nothing"; the end-to-end one emits before `tickSkirmish` and after it and asserts both
      survive alongside the sim's own. *Manual:* `make verify` MATCHes — events are not hashed,
      so this must move nothing. It does not.

**WHAT IS DELIBERATELY NOT IN THIS PHASE.** *Intel — LOS, radar, fog of war.* It is the next
largest sim gap and the one that makes scouting exist, but it is genuinely phase-sized on its
own: it gates targeting, it filters the event stream (`Events.hpp` already documents that
Recoil nils an attacker triple the receiver cannot see), it is per-player so it lives in the
snapshot (`Snapshot.hpp` already predicted this), and it wants P10.6 to exist first because
an LOS bitmap per team restamped on movement is exactly the shape fork-join is for. It gets
its own ADR and its own phase.

**AND THE PATHFINDING HERE IS A STAGING POST, on purpose** (D14). P10.4 and P10.5 are chosen
to be individually useful and individually replaceable. The hierarchical layer for sparse
single-unit queries — HPA*, or Recoil's HAPFS shape — is the intended next layer and is
*not* being designed now: `16 §3a` warns that pathing has the worst ratio of lines to months
in the genre, and the honest position is that this engine cannot yet measure its own pathing
well enough to choose. Revisit with numbers, not with a preference.

---

## 8. Content targets — what "playable" means concretely

From `14-blueprint-census.md §5`/`§9`, so this is chosen rather than drifted into.

**Faction first: Cybran.** `§9.2` — best mesh coverage (41 of 118) and **101 units ship
`_SpecTeam`** against 5/5/1 for UEF/Aeon/Seraphim, so team colouring works for Cybran and
almost nobody else. Zero hover units, the only faction with none, so its minimum set needs
three MoveDef rows in one `speedModClass`. `§9.1`: *"Aeon is the most expensive first
faction"* — its whole T1 land line hovers.

**The eight roles that make a match** (`§5.2`): ACU `URL0001` · Engineer `URL0105` · Mass
extractor `URB1103` · Power generator `URB1101` · Land factory `URB0101` · T1 tank `URL0107` ·
Scout `URL0101` · Arty/AA `URL0103`.

**Projectiles: 6 visual classes, not 307.** `13 §7.3`: *"the minimum projectile set for a
playable faction: 6 CEGs, 4 colour ramps, and ~120 generated weapondefs. Not 53–66 projectiles,
and certainly not 307."* Faction colour from the projectile id's first letter is *"the cheapest
fidelity win available"*.

**One FA semantic to adopt or reject deliberately** (`11 §10`): FA area damage has **no distance
falloff** — binary in/out, with graduated damage done as concentric rings. Ours has falloff. Own
semantics makes this a choice; it should be a recorded one.

### 8.1 The substrate lesson

`11 §8.1` is the most useful planning sentence in the whole analysis:

> *Build #11 (the buff system) first. It is 4–6 days and it is the substrate for adjacency,
> veterancy, enhancements, regen auras and cheat buffs — five of the top twelve items. Doing it
> first turns each of those into a data table plus a hook.*

Applied here: **P3.1 (roles) and P3.2 (build trees) are our buff system.** Once a unit type has
a role and a buildable list from data, the opponent script, the build menu, the AI's census, the
victory condition and the HUD all become a table plus a hook. Every one is a hardcoded blueprint
path today.

---

## 9. Still open

Answered: tick-rate default → D9 (10 Hz). Freeze → D10. P0 → D11.

**Q3 — Snapshot copy, or Recoil's `drawPos + timeOffset`?** (§4.1.) The copy buys per-player
snapshots later; the alternative is what a shipping engine does and allocates nothing. Not
blocking — it lands at P7.

**Q4 — Success criterion (§1.3):** accept, or set your own numbers? It goes in the README and
becomes what the project is measured against. Not blocking — and half the proof is now FROZEN
(P9), so the README has to be careful about which half it claims.

This should become ADR-033 as P0 lands.

---

## 10. What will not go smoothly

Kept because being wrong about this once already cost a phase's worth of surprise. Updated
for what P0 actually taught.

- **P1 is large, but no longer blind.** Downgraded from "cannot be done in small safe steps".
  The hash log turns each step into a checkable claim (§7, P1.0). What remains genuinely hard
  is the breadth: `Renderer::setInstances(batchIndex, span)` and the whole instanced-draw path
  assume batch-indexed storage, and every sim pass takes `[batch][instance]` spans. Estimate
  it in sessions, not hours.
- **P0 found three bugs in its own wiring, and that is the honest rate.** A re-announced
  winner, a DRAW declared for a crowd with no armies, and a dangling span that segfaulted the
  moment something read it straight after the call. All three were invisible until a new
  reader appeared. Expect the same density in P1, where the readers are every pass.
- **Interpolation will move every golden screenshot** (P7.2). The benchmark numbers in
  `README.md` shift too. Re-baseline and keep `--no-interpolate` for captures; the decision is
  cheap, discovering it late is not.
- **Cross-machine determinism is still unproven, and the harness does not prove it.** What P0
  built shows two runs of *the same binary on the same machine* agree. That is the useful
  daily tool and it is not the claim in §1.3. The claim needs P2's fixed-point sim and P9's
  Linux build; float sim agreeing with itself locally says nothing about arm64 vs x86-64.
  Do not let `MATCH` in a local log be read as the criterion being met.
- **`main.mm` holds real knowledge, not just mess.** The spawn logic, the extractor ordering,
  the passability cache keying — those encode decisions that took work. P7.5 moves them; the
  move has to be read rather than mechanical.
- **P1 through P6 have no player-facing payoff.** P7.4's minimap is the first visible thing.
  Four or five sessions of no screenshots is worth knowing before starting.

---

## 11. The next goal

**P0 through P8 are done (2026-08-21). PLAN2 has one phase left and it is frozen.**

What the last two phases leave behind:

- **The tick-to-render seam exists and is a type, not a discipline.** The sim publishes an
  immutable `sim::Snapshot` per tick; the renderer interpolates between the last two. `make
  verify` MATCHes unchanged across the whole of P7, which is the result to want: interpolation
  is presentation and never reaches the sim, so a seam done right moves no hashes.
- **`main.mm` is 324 lines and the rest is a library the tests link.** Seven `.cpp` files under
  `src/app/`, one `.mm` for the run modes. Writing the first tests for argv found three bugs in
  code that had shipped for weeks — `argv[1]` is the map so parsers start at 2, `--look` needs
  three values, and `AssetSearch::addRoot` silently drops a directory that does not exist.
- **`Renderer.mm` is 1,440 lines from 4,267**, split four ways plus six shader headers. One
  class across five translation units, not four objects — the ownership question (who holds the
  device, the queue, the camera, the depth textures, the shadow map) is deliberately unanswered,
  because splitting the code first is what made this step provable by hashing a screenshot.
- **The scale criterion is met and measured: 12.9x real time at 5,000 units.** Before P5 the
  same match ran at 0.8x. The gate is 2x, wide on purpose — it exists to catch a pass going
  quadratic, which is a factor of ten.

**Three lessons worth carrying out of this phase**, all of them about tests rather than code:

1. **A test can pass because it never runs the code it is about.** P5.1's oracle test was
   answered almost entirely by the grid's brute-force fallback; a deliberately broken cell key
   passed all 40,000 assertions. Any code with an adaptive path needs the test to assert that
   both paths carried traffic.
2. **A Catch test name starting with `--` is unrunnable through CTest.** It passes by hand and
   fails as "Unrecognised token", because the name is passed as the filter.
3. **A refactor's test can be an equality.** Every step of P7.3 and P7.5 was checked by hashing
   the same screenshot — `60f805cd915ad3f4e7c362bb2581746bd12205e82867e0eaa4f7a5f4eeb9914f`
   throughout. That is stronger than a suite and cheaper than reading the diff twice.

---

**P9 is frozen, and the freeze is the honest state of the project's headline claim.** §1.3 says
"the same command log produces the same match, on any machine". The first half is built and
checked every commit. **The second half is not proven and nothing may say otherwise** —
`check_fx_optimisation` shows the arithmetic survives `-O0` through `-ffast-math`, which is real
and is a different statement. Fixed point made the claim reachable; nothing in P1–P8 has to be
redone to get there.

What would thaw it: a second machine, or a reason to trust the number more than the design — a
networked game, a shared replay, anyone else's hardware.

**What is next IS a phase of PLAN2 now: §7 P10**, written 2026-08-21 after a review against
Recoil (`REVIEW.md`) which separated two things §2's Sim row had been averaging together. The
foundations are at or above their 73 % and beat the reference engine in four places — fixed
point, no globals, generational handles, the pass order. The gameplay surface is nearer 25 %,
and P10 is the four things that close it: armour classes, projectile kinds, a pathing cost
field with shared flow fields, and the first threading. **Ordered by retrofit cost rather than
by value** — damage is first because every combat call site grows an argument and there are
fewer of those today than there ever will be again.

The items below are still real and still unscheduled; they are what P10 is *not*:

1. **Sound — System is 16 % of the weight at 35 %, with nothing at all.** No mixer, no
   listener, no per-unit cue. Both games ship their audio in formats the VFS already mounts. It
   is the largest untouched slice in the project and the one a player notices first.
2. ~~**The two index spaces (`#28343`).**~~ **CLOSED.** `#3090` unified them and the build
   path is routed: both `applyDecisions` and `orderFirstExtractors` issue `CommandKind::Build`
   through `applyCommand`, so a build is authorised, recorded in the command log, and has its
   `ConstructionStarted` raised by the sim. P4.2's "one order path" is now true of construction
   as well as movement, and `check_one_order_path.sh` watches both — **which is what found the
   second bypass.** Routing only the scripted opponent's builds would have left the property
   half true for a second time, and a guard scoped to half a property is exactly how the first
   one survived unnoticed for months with a green check beside it.

   The change moved the golden and moved nothing a player can see: `Construction::position`'s
   `y` had been carrying a mass marker's terrain height, which `spawnUnit` overwrites with
   `terrain.heightAt(x, z)` — derived data that reached only the state hash. The screenshot
   hash was bit-identical across the change while the fingerprint moved, which is P10.0's
   technique and the right proof for a representation change.
3. **Formations, and the hierarchical pathing layer.** The rest of pathfinding's 40 % moved
   into P10.4 and P10.5 — the cost field and shared flow fields. What stays out of scope
   there is deliberate: a rally order still walks twenty units into each other and lets
   collision sort it out, and D14 says the hierarchical layer is chosen with measurements
   rather than now.
4. **The rest of the UI:** a build tray, control groups, a selection roster, order queues drawn
   in the world. The minimap was the hard one because it needed the snapshot; these need only
   the primitives the HUD already has.

**Model: Opus 5**, `effort: xhigh`. Sound has the most design freedom and the least prior art
in this repo — nothing here has ever made a noise, so ADR-0xx for the mixer's shape is the first
deliverable rather than an afterthought.

The goal prompt, for whichever is chosen:

```
Read PLAN2.md first — §0 has the settled decisions, §2 the progress reading and what it
says is missing, §11 what P7 and P8 left behind, §10 the risks.

PLAN2's phases P0-P8 are done and P9 is FROZEN (the Linux build and the cross-architecture
hash). Do not thaw it without being asked: the freeze is deliberate and §1.3's second half
must keep being described as unproven.

Pick up <the item>. It is phase-sized, so plan it before writing code: an ADR for any
non-trivial design choice (ADR_DECISIONS.md), then items with the automated test and the
manual check named per item the way §7 does.

Constraints:
- Do not add a verification step, a self-review pass, or a verifier subagent. Run the tests
  and the replay, and report what they output.
- Do not delegate to subagents.
- `make verify` is a strict check for anything that is meant to be a refactor. If a change is
  meant to alter the match, use `test_match_invariants` and `test_tick_rate_invariance` as
  the gates and re-record the golden deliberately, saying in the commit what moved and why.
- For a refactor, prefer an EQUALITY as the test: hash the same screenshot before and after.
  P7.3 and P7.5 were both checked that way and it caught more than reading would have.
- Report the §2 progress reading at the end, recomputed WITH A SCRIPT.
```
