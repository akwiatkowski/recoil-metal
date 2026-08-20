<!-- Generated and maintained by Claude -->

# PLAN2 — architecture, and the order features get built

`PLAN.md` is the milestone log: what renders next. This file is the other axis — **what this
engine is, how its parts connect, what order to build them in, and how far along it is.**

It is written against `../forged-alliance-reborn/docs/engine-analysis/` (16 reports, 19,871
lines) and `reference/{RecoilEngine,FAF-fa,BAR,Zero-K}`. Every non-obvious claim cites one.
The first version of this file did not, and invented from a blank page what two mature engines
and a 763-line feasibility study had already settled.

---

## Status — 2026-08-20

**P0 is done.** 557 tests green, up from 527. The determinism harness exists: a per-tick state
hash, a hash log with `--hash-log` / `--check-hash-log`, and first-divergence reporting. Two
independent 60-second runs report `MATCH — 600 ticks identical`; one flipped bit reports
`DIVERGED at tick 300`. Both loops now run the same tick, enforced by a CTest check rather
than remembered.

**Engine completion: ~36 %** (§2). **Next: P1** — identity and storage, and §11 says why it is
now a far safer phase than this file originally claimed.

**Uncommitted:** everything except `a0f6b72` (P0.5). The working tree already held an
uncommitted milestone-20 changeset when P0 started, interleaved with P0's edits in `main.mm`
and `CMakeLists.txt`; see §11's last paragraph for the split.

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

### 2.2 The reading, 2026-08-20

Weights are sourced; the per-subsystem completion figures are **judgement against a named
capability list**, which is the honest description. Recompute by re-judging each row, not by
re-deriving the weights.

| Subsystem | W | Done | Have | Missing |
|---|---:|---:|---|---|
| Sim | 26 % | **22 %** | movement, collisions, targeting/facing/firing, projectiles, area damage, death, economy, construction, victory | entity store + ids, features, fixed-point, intel/vision, shields, transports, most weapon classes, air/naval/hover domains, veterancy, upgrades, adjacency |
| Renderer | 21 % | **55 %** | instanced units w/ team colour, props + culling, shadows, water + refraction, sky, particles, decals, text/HUD, icons, selection, model LOD, pose playback, DDS, offscreen capture | drawer split, minimap, effect taxonomy (muzzle/trail/impact), beams |
| System | 16 % | **30 %** | VFS (`.sdz`/`.scd`), asset search, DDS, settings, bench harness | **sound (nothing)**, logging framework, job system, serialisation/save, profiling |
| Game/orders/UI | 13 % | **30 %** | orbit camera, picking, selection + modifiers, HUD, order markers, CLI harness | command queue, build menu, control groups, minimap interaction, formations, game states |
| Map | 10 % | **70 %** | SMF/SMT, `.scmap`, tile atlas, heightfield, `mapinfo.lua`, terrain mesh w/ LOD + skirts, chunk culling, splat, water, stratum normals, props, terrain types, start positions | minimap, resource spots, features as objects |
| Pathfinding | 7 % | **25 %** | coarse grid A*, passability, path following | hierarchical/flow-field, dynamic blocking, formations, avoidance quality, per-motion-class grids |
| Net/replay | 5 % | **15 %** | per-tick state hash, hash log + first-divergence reporting (P0.2/P0.3) | netcode, lockstep, the cross-architecture proof (P8) |
| AI | 2 % | **15 %** | scripted build order + one attack wave | role classification, build tree, any reaction |

**Weighted total: ~36 %.**

The shape of that number is the important part: **the two most-complete slices are Map (70 %)
and Renderer (55 %), which together are 31 % of the weight — and the Sim, at 26 % of the
weight, is 22 % done.** That is precisely why the project looks further along than it is. A
screenshot samples the finished third.

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

### P1 — Identity and storage (load-bearing; everything is easier after and impossible before)

**P0 changed the character of this phase.** This file originally said P1 "cannot be done in
small safe steps" and to "expect the app to be broken in the middle of it". That was true
when the only way to know whether a refactor had changed behaviour was to play the game. It
is no longer: a hash log recorded before the first edit is a statement of exactly what the
match does, and every step can be checked against it. The phase is still large, but it is now
*verifiable* at each step rather than at the end.

The technique, and it is the whole reason to do P1 next rather than P2:

- [ ] **P1.0 Record the golden log first.** `--play 300 --hash-log docs/golden-p1.log` on a
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

- [ ] **P1.1 `IdPool`** (§6.1). *Test:* a dead unit's id never resolves to its successor; an
      order held across a death fails cleanly. *Manual:* kill a selected unit, no ghost.
- [ ] **P1.2 `UnitStore`.** `Unit` holds type, owner, position, orientation, health, motion,
      build state. Buildings are units with a build state, not a second kind. *Test:* 1,000
      units, kill every third, assert iteration and lookup. *Manual:* `--units 5000` renders.
- [ ] **P1.3 Bucketed index by team and type** (§6.2). *Test:* counts match brute force after
      10,000 random spawns and deaths. *Manual:* HUD counts stay right through a battle.
- [ ] **P1.4 Delete `UnitRef{batch, instance}`** and the batch-indexed sim spans. *Test:* suite
      passes with the type gone. *Manual:* full `--play`.

### P2 — Fixed-point (D1) and ownership

- [ ] **P2.1 `Fx` type + fixed-point trig/sqrt tables** (§5.2). *Test:* golden tables; identical
      results at `-O0` and `-O2`. *Manual:* none.
- [ ] **P2.2 Migrate sim state to fixed-point.** *Test:* sim tests pass with re-based constants;
      the P0.2 hash is identical across `-O0`/`-O2`/arm64/x86-64. *Manual:* `--play` unchanged.
- [ ] **P2.3 Tick rate as configuration** (§5.1): per-second → per-tick conversion moves into
      `rm_data`; `kTickSeconds` leaves sim math. *Test:* the same match at 10 Hz and 20 Hz
      produces the same *outcome* (not the same hash); a rate of 4 or 51 is rejected at startup.
      *Manual:* `--tick-rate 20 --play`.
- [ ] **P2.4 `Player` / `Team` / `Alliance`** (§6.3). *Test:* victory fires on alliance
      elimination, not team elimination; two players sharing a team both command its units.
      *Manual:* `--play` 2v2.
- [ ] **P2.5 Human input becomes a command source.** *Test:* a recorded human log and a
      synthetic script log replay through the same path. *Manual:* play, then replay.

### P3 — The data layer: game rules out of C++

- [ ] **P3.1 Role classification from categories,** using `07-ai-and-gamesetup.md §4.4`'s
      vocabulary — `commander, builder, factory, extractor, energy, storage, defence, raider,
      assault, artillery, antiair, air, naval, scout, transport, shield, radar, experimental` —
      chosen there as a superset of BAR's `ai_simpleai.lua` inference and a subset of
      CircuitAI's enum. *Test:* real blueprints — `URL0001`→commander, `URB1103`→extractor,
      `URL0105`→builder, `UEB0101`→factory. *Manual:* `--dump-roles`.
- [ ] **P3.2 `BuildableCategory` expression evaluator.** SupCom ships no build lists, it ships
      expressions: `{"BUILTBYTIER1ENGINEER CYBRAN"}`, **space = AND, list = OR** (`07 §4.3`).
      That report calls materialising them *"a hard blocker for any skirmish, not just for the
      AI"*. *Test:* hand-written expressions, then a real-corpus test that a T1 engineer's set
      holds its own faction's mex, pgen and land factory and nothing of another's.
      *Manual:* `--dump-buildtree URL0105`.
- [ ] **P3.3 Delete the hardcoded blueprint paths and wave constants.** `BuildOrder.hpp`'s four
      `k*Blueprint` paths and `kAttackWaveTanks` become a data file selecting by *role*.
      *Test:* one build order drives UEF and Cybran from data alone. *Manual:* `--play` as each.
- [ ] **P3.4 MoveDefs by name, from motion class.** Two corrections from `01 §3.4` and
      `recoil-engine-map.md §5`: mobile units ignore the unitdef's `maxSlope`/`maxWaterDepth`
      entirely — those govern *building placement* — and **all four factions' ACUs are
      `RULEUMT_Amphibious`** (`14 §9.1`), so amphibious is a fixed cost, not a later feature.
      `ADR-027` says passability comes from motion class; this makes it true. *Test:* water that
      blocks a land unit and passes an amphibious one, same map. *Manual:* walk the commander
      across a bay, then a tank.
- [ ] **P3.5 FA duration correction.** `11 §2.1`: FA's `WaitSeconds(n)` is `WaitTicks(n*10+1)`,
      so every Lua-timed duration is quantised to 100 ms **and inflated by 100 ms** —
      `WaitSeconds(1.0)` waits 1.1 s. The transform is
      `t' = (t <= 0.1) ? 0.1 : (floor(t*10) + 1)/10`, and without it *"every weapon's DPS is
      wrong by up to 50 % on burst weapons"*. Applies when importing FA content at any tick
      rate — it is a fact about how FA's numbers were authored, not about our rate.
      *Test:* table-driven: `0.05→0.1`, `0.2→0.3`, `1.0→1.1`. *Manual:* `--dump-weapon URL0202`.
- [ ] **P3.6 `SlowUpdate`** (§6.7). *Test:* every unit visited exactly once per period, even
      per-tick load. *Manual:* `--bench` shows no periodic spike.

### P4 — Orders

- [ ] **P4.1 `CommandQueue`** (§6.4): queue, shift-append, cancel-queued rules, finish.
      *Test:* three queued moves run in order; a plain order replaces, a shift-order appends.
      *Manual:* shift-click a route.
- [ ] **P4.2 Every input path goes through commands.** *Test:* the scripted opponent's order and
      a synthetic click produce identical hashes. *Manual:* replay divergence check.

### P5 — Spatial queries

- [ ] **P5.1 `SpatialGrid`** (§6.5). *Test:* results match brute force on 10,000 randomised
      layouts — the ideal test shape, since the slow version is the oracle.
      *Manual:* `--bench --units 5000`.
- [ ] **P5.2 Route targeting, collisions and area damage through it.** *Test:* combat tests pass
      unchanged. *Manual:* frame time at 2,000 units drops; targeting behaves the same.

### P6 — Events, and effects out of the sim

- [ ] **P6.1 `EventQueue`** — our subset of the ~130-call-in vocabulary (`04 §4`), sized to what
      exists: `UnitCreated, UnitFinished, UnitDamaged, UnitDestroyed, UnitTaken, WeaponFired,
      ProjectileImpact, ConstructionStarted, ConstructionFinished, TeamDefeated, GameOver`.
      *Test:* a kill emits exactly one `UnitDestroyed` with the right instigator.
      *Manual:* `--print-events`.
- [ ] **P6.2 `FeatureStore`** (§6.6); `wreckDecals` deleted; the renderer makes decals from
      `UnitDestroyed`. *Test:* a death creates one feature at the right position; the sim links
      no GPU type. *Manual:* scorch marks still appear.
- [ ] **P6.3 Particles, dust and death blasts move behind events.** *Test:* `rm_sim` compiles
      with `core/scene/` off its include path — the real assertion. *Manual:* unchanged.

### P7 — The render side

- [ ] **P7.1 `Snapshot` + `snapshot(const Sim&)`.** *Test:* pure; same sim, same bytes; never
      mutates. *Manual:* none.
- [ ] **P7.2 Interpolation; `UnitInstance` becomes render-only.** *Test:* alpha 0 and 1
      reproduce the endpoints exactly. *Manual:* visibly smoother; `--no-interpolate` for
      captures.
- [ ] **P7.3 Split `Renderer.mm` (4,267 lines)** into `MapRenderer`, `UnitRenderer`,
      `FxRenderer`, `UiRenderer` — Recoil's own split (`CBaseGroundDrawer`, `CUnitDrawer`,
      `CProjectileDrawer`, `CMiniMap`). *Test:* unchanged golden screenshots.
- [ ] **P7.4 Minimap.** Nearly free once a `Map` object and a snapshot exist; today there is
      nothing to read from. *Test:* world→minimap projection round-trips. *Manual:* click it.
- [ ] **P7.5 `main.mm` to argv plus wiring,** under 400 lines from 3,948. *Test:* the suite
      covers what moved out. *Manual:* every flag still works.

### P8 — The proof

- [ ] **P8.1 Linux-x86-64 headless sim build** through `ADR-032`'s RHI seam — sim and tests
      only. *Test:* CI on both. *Manual:* it builds.
- [ ] **P8.2 Cross-architecture replay hash.** Same log, both platforms, identical per-tick
      hashes. *Test:* this *is* the test. *Manual:* the number goes in the README.
- [ ] **P8.3 5,000 units at the configured rate.** *Test:* perf regression gate.
      *Manual:* `--bench`.

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
becomes what the project is measured against. Not blocking — the proof is P8.

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
  daily tool and it is not the claim in §1.3. The claim needs P2's fixed-point sim and P8's
  Linux build; float sim agreeing with itself locally says nothing about arm64 vs x86-64.
  Do not let `MATCH` in a local log be read as the criterion being met.
- **`main.mm` holds real knowledge, not just mess.** The spawn logic, the extractor ordering,
  the passability cache keying — those encode decisions that took work. P7.5 moves them; the
  move has to be read rather than mechanical.
- **P1 through P6 have no player-facing payoff.** P7.4's minimap is the first visible thing.
  Four or five sessions of no screenshots is worth knowing before starting.

---

## 11. The next goal

**P1 — identity and storage.** The reasons it goes next rather than P2:

1. It is the load-bearing change. §6.1–6.3 all depend on a unit having an identity that is not
   a draw-call address, and P3's role/build-tree work needs to count units by team and type,
   which is P1.3.
2. P0 just made it safe (P1.0). That safety expires: the longer the sim grows against
   `[batch][instance]`, the more code the refactor touches.
3. It does not change numerics, so the golden log is a *strict* check — unlike P2, where every
   constant is re-based and the log must be re-baselined by construction. Doing the phase whose
   correctness is exactly checkable before the phase whose isn't is the cheaper order.

**Model: Opus 5 again**, `effort: xhigh`, `max_tokens` ≥ 64K. Same reasoning as P0 and it
applies more strongly here — *"strongest on difficult tasks: multi-file features, larger
refactors"* is a description of P1. Consider Fable 5 only if P1.2 stalls; that is the one item
in the phase with genuine design freedom left in it.

The goal prompt:

```
Execute phase P1 of PLAN2.md in the recoil-metal repo. Read PLAN2.md first —
§0 has the settled decisions, §6.1–6.3 the designs, §7 the items, §10 the risks.

Start with P1.0: record docs/golden-p1.log and commit it. Then P1.1 → P1.4,
replaying against that log after each and requiring MATCH before moving on.
Keep iteration order identical for the whole phase; a hash change is a
behaviour change, and the tick number says where to look.

Every item ships with the automated test and the manual check named in §7.
Commit each item separately, conventional messages, co-author trailer
"Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>".

Constraints:
- Deliver P1 at the scope §7 states. Don't start P2 — fixed-point re-bases
  every constant and would invalidate the golden log this phase depends on.
- Do not add a verification step, a self-review pass, or a verifier subagent.
  Run the tests and the replay, and report what they output.
- Do not delegate to subagents.
- Feature work is frozen (D10).
- If a step turns out to be blocked, finish the others and say plainly which
  one you left and why. Don't silently narrow the scope.
- Report the §2 progress reading at the end, recomputed, with a new history row.
```

**Before that goal starts, the commits need resolving.** The working tree holds an
uncommitted milestone-20 changeset that predates P0, interleaved with P0's edits in `main.mm`
and `CMakeLists.txt`; the pre-session `main.mm` is not recoverable, so the two cannot be
separated. Proposed history, executable in one step: (1) milestone 20's sim files plus their
`CMakeLists` entries, (2) P0's harness files plus theirs, (3) `main.mm`, described honestly as
both. P1 should start from a clean tree — its whole method depends on knowing what changed.
