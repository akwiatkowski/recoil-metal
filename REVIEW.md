<!-- Generated and maintained by Claude -->

# REVIEW — `recoil-metal` measured against the Recoil engine

**Date:** 2026-08-21
**Reviewed tree:** `main` @ `8631950`, plus an uncommitted renderer split in the working tree
**Reference engine:** Recoil at `~/projects/llm/games/faf/recoil-macos/rts` — 1,518 source files
outside `rts/lib`, ~351k lines across Sim/Game/Rendering/Lua/System/Map/Net
**This tree:** 31,706 lines of `src/`, 19,759 lines of `tests/` across 76 test files

Every claim below cites a file and line on at least one side. Where this review disagrees with a
comment in the tree, both are quoted.

---

## Resolution — updated 2026-08-21

**Every §5 finding is closed.** §5 was the defect list; §3 is a capability comparison, and its
items are phase-sized work rather than things to fix in a session — they are scheduled, not
outstanding, and the distinction is kept honest below rather than blurred.

| Finding | Status |
|---|---|
| §5.1 The float ban's blind spot | **fixed** — `68c7e73`, P10.0 |
| §5.2 `Movement.hpp` stale 10 Hz rationale + duplicate include | **fixed** — `68c7e73` |
| §5.3 `SlowUpdate.hpp` cites 16 frames; Recoil's constant is 15 | **fixed** — `68c7e73` |
| §5.4 `check_no_sim_globals.sh` file counts drifted | **fixed** — `68c7e73` |
| §5.5 The app target does not build | **fixed** — the renderer split landed (P7.3/P7.5) |
| §5.6 `main.mm` at 4,996 lines | **fixed** — 324 lines; the rest is a library the tests link (P7.5) |
| §5.7 One-tick event lifetime is a convention | **fixed** — P10.7, `beginFrame(tick)` replaces `clear()` |

Two of those (§5.5, §5.6) were already being fixed while this review was being written — the
work was uncommitted in the tree at the time and is noted as such in each section.

**§5.1 was worse than described.** The review found a float *tolerated* in sim state; it was
actually a **round trip in both directions** — `Command.cpp` rounded an `Fx` the caller already
held into a float, `Skirmish.cpp` converted it back to raise an event, and `app/Match.cpp`
converted it back again to measure a distance. All are now the identity. Fixing it moved the
replay hash, which is correct and was proven harmless by P7.3's technique: the screenshot hash
is `0a430d44ee89ea9a3868f17d5caa0cdb6b7bd9d650cce465592c02fb5ff37a97` before and after, so
7,000 ticks render pixel-for-pixel identically while the fingerprint moves. A second guard came
free — `feed(StateHash&, float)` lost its last caller and `-Werror=unused-function` now refuses
to build until it is deleted, so re-introducing a float into hashed state is a compile error.

**Of §3, one item is addressed.** §3.1 (scalar damage) is P10.1: armour classes and a sparse
`DamageProfile`, with Forged Alliance's multiplier matrix transposed at import. The rest —
projectiles (§3.2), pathfinding (§3.3), motion types (§3.4), sound/threading/serialisation
(§3.5), intel and shields (§3.6) — are **scheduled in `PLAN2.md` §7 P10 and its stated
non-goals**, ordered by retrofit cost rather than by value, and governed by `ADR-033` through
`ADR-036`. None of them is a defect; all of them are engine that does not exist yet.

**§6's two suggestions are both taken**, with one deliberately deferred: `ADR-035` adopts the
layered pathfinding seam (§6.1) and explicitly defers choosing the final architecture until the
engine can measure its own pathing; `ADR-034` takes §6.2's data-driven weapon variation but
**refuses the class hierarchy**, because a polymorphic projectile is not hashable without a
visitor per subclass and the flat trivially-copyable struct is what the state hash walks.

Everything above was re-verified against the tree rather than recalled.

---

## 0. Verification performed for this review

Facts, not impressions, because most of what follows rests on them:

| Check | Result |
|---|---|
| `ctest` | **771/771 passed**, 8 skipped (all asset-gated: BAR/FA corpus not present) |
| `cmake --build build` | **FAILS** — see §5.5. Library and tests build; the app target does not |
| Recoil global-symbol reach | recounted; the numbers in `check_no_sim_globals.sh` need a small correction (§5.4) |
| `UNIT_SLOWUPDATE_RATE` | **15**, not the 16 quoted in `SlowUpdate.hpp:15` (§5.3) |
| `COMMAND_CANCEL_DIST = 17.0f` | confirmed at `CommandAI.cpp:49`, as cited |
| `DegreesToMaxSlope` = `1 - cos(clamp(deg,0,60) * 1.5 * DEG_TO_RAD)` | confirmed at `MoveDefHandler.cpp:84-95`, as cited |
| `SimObjectIDPool::Expand` touches the RNG | confirmed at `SimObjectIDPool.cpp:29-30` — two `random_shuffle` calls on `gsRNG` |
| `QueryVectorCache` asserts when exhausted | confirmed at `QuadField.h:38`, as cited |

The citation discipline in this codebase is unusually good. Of roughly twenty specific Recoil
line references spot-checked, **eighteen were exactly right**. The two that were not are §5.3
and §5.4, and both are off by a rounding rather than wrong in substance.

---

## 1. Verdict in one table

| Area | vs Recoil | Why |
|---|---|---|
| Determinism model | **Better** | Fixed point by construction beats `SYNCCHECK`-by-instrumentation |
| Testability of the sim | **Much better** | No globals ⇒ two sims in one process; Recoil structurally cannot |
| Handle safety | **Better** | Generational `UnitId` vs raw pointers + a death-dependence graph |
| Unit storage layout | **Better for now** | SoA + stable slots; Recoil is AoS with pointer indirection |
| Spatial index | **Better for this tick model** | One rebuild/tick, zero per-query allocation, brute-force floor |
| Order semantics | **Comparable, far smaller** | Cancel rules copied faithfully; 4 command kinds against Recoil's ~40 |
| Pacing (`SlowUpdate`) | **Better** | Rate-derived period vs a hardcoded frame count |
| Documentation of intent | **Far better** | Recoil's rationale is mostly lost; here it is in the headers |
| Pathfinding | **Much worse** | One coarse A* grid vs QTPFS + HAPFS, 16.3k lines, multithreaded |
| Weapons/projectiles | **Much worse** | 1 scalar damage vs `DamageArray` per armour class; 1 projectile vs 15 classes |
| Movement types | **Much worse** | One ground mover vs Ground/Hover/Strafe/Static/Script |
| Vision/intel, shields, transports | **Absent** | Recoil has all three |
| Netcode, save/load, sound | **Absent** | Recoil has `rts/Net` (7k), creg serialisation, a sound layer |
| Concurrency | **Absent** | No `std::thread` anywhere in `src/`; Recoil multithreads pathing and rendering |
| App/renderer structure | **Worse** | `main.mm` is 4,996 lines; `Renderer` is one class with 118 methods |

---

## 2. Where this engine is genuinely better

### 2.1 Determinism is structural, not instrumented — **the strongest thing here**

Recoil's approach is to *detect* desync after the fact. `SyncedPrimitive<T>`
(`System/Sync/SyncedPrimitive.h:37-60`) wraps every primitive and calls `Sync()` on each write,
maintaining a running checksum in `CSyncChecker` (`SyncChecker.h:33-49`). Crucially it is
**compiled out unless `SYNCDEBUG` or `SYNCCHECK` is defined** (`SyncedPrimitive.h:6`), so a
release build carries none of it. Underneath, the sim still runs on floats — `SyncedFloat3`
(`SyncedFloat3.h`) is three of them — with `streflop` (`rts/lib/streflop`) pinning the FPU and
`good_fpu_control_registers()` (`FPUCheck.h:14`) asserting the control word. That is a large,
fragile apparatus whose job is to make floating point *behave*.

This tree removes the problem instead. `core/Types.hpp:92-132` defines Q18.14 geometry, Q50.14
magnitudes, and 16-bit binary-radian angles, and `tools/check_no_sim_floats.sh` mechanically
forbids float variables, parameters, returns and casts anywhere in `src/core/sim` outside eight
named boundary files. `tools/check_fx_optimisation.sh` compiles a probe at `-O0`, `-O1`, `-O2`,
`-O3` and `-O3 -ffast-math` and compares output bytes — a test Recoil cannot write, because
`-ffast-math` would destroy its sim.

Three design choices here are better than merely *different*:

- **Binary radians, not fixed-point radians** (`Types.hpp:121-132`). Wrapping by unsigned
  overflow is exact; a modulo by an irrational 2π accumulates error over a match. Recoil reaches
  the same conclusion for `turnRate` but not for its internal angle math.
- **Two fixed-point widths with the same fractional count** (`Types.hpp:99-119`). Conversion
  between `Fx` and `Mag` is a widen or a narrow, never a rescale. The header names the failure
  mode it avoids and it is a real one in Recoil's own content format.
- **The width choice is measured, not assumed.** `Types.hpp:74-90` records that Q16.16 was
  specified in PLAN2 §5.2 and *rejected on evidence*: three shipped maps are 32,768 elmos across,
  which is exactly Q16.16's ceiling, and `BuildCostEnergy` reaches 10,008,000. The plan was
  overruled by measurement and the overrule was written down. That is rare.

`StateHash.cpp:18-26` also feeds bytes low-to-high explicitly so that arm64 and x86-64 agree —
the endianness hazard is handled at the one place it could bite.

**The honest limit, which the tree already states** (`PLAN2.md §2.2`): every determinism result
so far is one machine agreeing with itself. Fixed point makes the cross-architecture claim
*reachable*; it does not make it *true*. Recoil, for all its float fragility, has the far
stronger position of having actually shipped cross-platform lockstep to thousands of players.

### 2.2 No globals — and the payoff is concrete

`tools/check_no_sim_globals.sh` bans file-scope mutable state and `static` locals in
`src/core/sim`. Recoil's counterpart: `extern CUnitHandler unitHandler;` at `UnitHandler.h:124`,
and eleven siblings. Recounted for this review across the 1,518 non-`lib` source files:

| Symbol | Files referencing |
|---|---:|
| `globalRendering` | 135 |
| `gs->` | 119 |
| `teamHandler` | 78 |
| `unitHandler`, `readMap` | 69 each |
| `gsRNG` | 43 |
| `quadField` | 41 |

The argument in the script's header is the right one and worth restating: because Recoil's sim
*is* a set of process-wide singletons, **you cannot construct two of them in one process** — and
the strongest determinism test available is exactly that. Step two sims from one command log,
compare hashes each tick, stop at the first divergence, all inside one debugger. Recoil's own
sync testing is reduced to diffing files between separate runs. This tree gets the better test
as a side effect of the rule.

That property is visible in the test suite: 771 tests over a 7,410-line sim is a density Recoil
does not approach (its `test/` directory is a fraction of the size of its `Sim/`).

### 2.3 Generational handles collapse an entire Recoil subsystem

`IdPool.hpp:36-45` gives `UnitId{index, generation}` with generation 0 reserved as never-live.
`UnitStore::kill` leaves the arrays intact and does not advance the mirror, so a stale handle
fails `alive()` forever.

Recoil instead stores raw `CUnit*` in queued commands, which forces `CObject`'s death-dependence
graph — `AddDeathDependence` / `DependentDied` / `ClearCommandDependencies` — to exist purely to
stop pointers dangling. `CommandQueue.hpp:13-24` identifies this correctly: most of `CCommandAI`'s
weight (1,864 lines in `CommandAI.cpp` alone, 7,323 across the directory) is bookkeeping that a
handle makes unnecessary.

The `IdPool.hpp:25-33` comparison against `SimObjectIDPool` is also correct and verified:
Recoil keeps three `unordered_map`s, recycles on a delay, and carries a determinism warning in
its constructor because `Expand` calls `spring::random_shuffle(..., gsRNG)` twice
(`SimObjectIDPool.cpp:29-30`) — so table growth perturbs the synced RNG. Recoil accepted a
*determinism hazard in its ID allocator* to keep unit IDs unguessable by Lua widgets. This tree
has no Lua, so the trade does not apply, and the generational counter removes the hazard entirely.

### 2.4 The spatial index is better *for this tick model*, and the header says why

`SpatialGrid.hpp:29-32` makes the sharpest observation in the codebase:

> The reason we can do that and Recoil cannot is not cleverness: its objects move continuously
> *within* a frame and it queries mid-update, so a grid built at one point would be stale by the
> next reader. Our tick has a declared pass order, so the grid can be rebuilt at one named point.

That is exactly right, and it is why `CQuadField` needs a per-query result vector from a pool of
three (`QuadField.h:70-72`) that `assert(false)`s on exhaustion (`:38`). Two properties here beat
it outright:

- **Ascending slot order guaranteed** (`SpatialGrid.hpp:36-41`) — so the index is byte-identical
  to the brute-force scan it replaced, *including tie-breaks*. That is what let the migration be
  verified against a golden log rather than eyeballed.
- **Never worse than brute force** (`:43-46`) — a long-range weapon scans the array instead of
  4,096 cells, so cell size stops being a number that has to be right.

The cost is stated rather than hidden: the returned span is invalidated by the next query, and
`reindex` is manual. Both are honest trades.

### 2.5 Rate-derived pacing beats a hardcoded frame count

Recoil staggers `SlowUpdate` across `UNIT_SLOWUPDATE_RATE` frames
(`UnitHandler.cpp:348-364`), a constant of 15 at `GAME_SPEED 30` — i.e. 0.5 s, but expressed as
*frames*. `SlowUpdate.hpp:26-31` refuses to copy the number, taking a period in seconds and
deriving ticks. Given D2's 5–50 Hz range, Recoil's constant would mean 3.0 s at 5 Hz and 0.3 s at
50 Hz. `tools/check_no_tick_literals.sh` enforces the rule, and its header records that the check
was written *after* a real bug found by running `--tick-rate 20`.

One structural advantage falls out of §2.3: Recoil staggers by position in `activeUnits`, which
compacts on death, so the rotation reshuffles and a unit can be skipped or double-visited.
`SlowUpdate.hpp:40-44` staggers by slot index, which never moves — so the "exactly once per
period" guarantee actually holds. This is a real correctness win Recoil does not have.

### 2.6 The economy is the right model, expressed better than the original

`Economy.hpp:133-152` gets the two-pass flow model right — every construction states its want,
then all advance by one shared `fundedFraction` — and states *why* the alternative is wrong:
funding in list order makes build progress depend on array order. The ordering of upkeep before
construction (`:65-68`) is a genuine game mechanic ("a base short of power stops building rather
than stopping running") and it is documented as one. Recoil's team resource code has no comparable
explanation anywhere.

### 2.7 Documentation quality

This is the largest gap and it runs the other way. Recoil's headers document *what*; this tree's
document *why, with the alternative that was rejected and the measurement that decided it*.
`SpatialGrid.hpp`, `IdPool.hpp`, `Types.hpp`, `SlowUpdate.hpp` and `Snapshot.hpp` each explain a
decision better than any comparable file in `rts/`. `Skirmish.hpp:191-206` states the tick order
*and the failure mode of each alternative ordering* — Recoil's equivalent (`CUnitHandler::Update`,
`UnitHandler.cpp:440-455`) is six bare calls.

The habit of recording *corrections* is rarer still — `StateHash.cpp:206-213` documents a real
defect where a death hashed identically to a survival, `PLAN2.md §2.2` publishes a downward
revision of the project's own progress number rather than quietly fixing the arithmetic.

---

## 3. Where Recoil is better, and by how much

### 3.1 Damage is a scalar; Recoil's is a vector — the biggest gameplay gap

`Weapon.hpp:73` is `sim::Mag damage{}`. Recoil's `DamageArray` (`Sim/Misc/DamageArray.h`) holds
one damage figure **per armour class**, plus `paralyzeDamageTime`, `impulseFactor`,
`impulseBoost`, `craterMult` and `craterBoost`.

This is not a polish item. Armour classes are how an RTS makes rock-paper-scissors work: an
anti-air missile that does 10% to ground is the same weapon table entry with a different vector.
With a scalar there is no way to express it, so every unit is a damage-per-second number and unit
composition stops mattering. The two-ring model (`Weapon.hpp:134-153`) is a good, correctly
motivated special case for commander detonations, but it is a special case.

Also missing and present in Recoil: `paralyzeDamage` (EMP), impulse (knockback), crater
deformation.

### 3.2 Projectiles: one struct vs fifteen classes

`Combat.hpp:38-68` has a single `Projectile` — position, velocity, damage, radius, arc, lifetime —
advanced by a single `advanceProjectiles`. Recoil has `Sim/Projectiles/WeaponProjectiles/` plus a
`CWeapon` hierarchy of fifteen: `BeamLaser`, `Cannon`, `MissileLauncher`, `StarburstLauncher`,
`TorpedoLauncher`, `FlameThrower`, `LightningCannon`, `MeleeWeapon`, `PlasmaRepulser`,
`BombDropper`, `DGunWeapon`, `EmgCannon`, `LaserCannon`, `Rifle`, `NoWeapon` — 4,757 lines of
weapons and 6,655 of projectiles.

Two capability gaps follow directly:

- **Beams.** A beam weapon is instantaneous and continuous; it cannot be modelled as a travelling
  point. `PLAN2 §2.2` lists this under Renderer's Missing, but it is a sim gap first.
- **Collision.** `Combat.hpp:191-192` is explicit: "A shot lands when it reaches its target's
  ground position or its height falls to the ground — not when it collides with a model". Recoil
  tests against real collision volumes via `CQuadField`. Point-vs-point impact means projectiles
  pass through everything they were not aimed at, so there is no such thing as a shot blocked by
  a building or a unit body-blocking for another.

### 3.3 Pathfinding: 40% is generous

`Pathfinding.hpp` is 110 lines of header over a 275-line implementation: one uniform grid at
64-elmo cells, 8-connected A*, octile heuristic. Recoil ships **two complete pathfinders** in
16,325 lines — HAPFS (hierarchical, with `PathEstimator`, `PathCache`, `PathHeatMap`,
`PathFlowMap`) and QTPFS (quadtree, with incremental node-layer updates) — behind an
`IPathManager` interface, and runs path requests **multithreaded**
(`UnitHandler.h:88-90`: `MultiThreadPathRequests`).

Missing here and material to gameplay:

- **No dynamic blocking.** A building placed across a route does not invalidate it.
  `Movement.hpp:24-27` admits it: "a unit whose route is occupied leans on whoever is in the way
  rather than re-routing."
- **No path cache.** Fifty units ordered to one point run fifty full searches.
- **No flow field or formation.** Recoil has `PathFlowMap` and `SelectedUnitsAI` for exactly this.
- **Cell granularity.** 64-elmo cells against Recoil's 8-elmo squares. `Pathfinding.hpp:20-23`
  defends this correctly as a search-graph-vs-cost-map distinction, but the consequence is that a
  cell is impassable if *any* of its 64 squares is (`:76-79`) — a conservative rule that will
  refuse routes through legitimately walkable gaps as soon as maps get tight.

### 3.4 One motion type

`Sim/MoveTypes/` is 9,049 lines across `GroundMoveType` (3,731 alone), `HoverAirMoveType`,
`StrafeAirMoveType`, `StaticMoveType`, `ScriptMoveType`, behind `AMoveType` with a
`MoveTypeFactory`. This tree has `Movement.cpp` at 370 lines, ground only.

`data/MoveDef.hpp` is genuinely good work — it correctly identifies (with three citations) that
`UnitDef::maxSlope`/`maxWaterDepth` are **building-placement** fields and mobile units take theirs
from the MoveDef, a mistake that would have silently produced plausible-looking wrong routing.
But `MoveDef::usesGroundGrid` (`:54`) already flags air, ships and submarines as unsupported. Air
is not a "later feature" in an RTS; it is a third of the roster.

### 3.5 Everything the system layer does not have

`PLAN2 §2.2` scores System at 35% and that is if anything charitable. Verified absences:

- **Sound: nothing.** No `AVAudio`, `OpenAL` or `AudioToolbox` reference anywhere in `src/`.
- **Threading: nothing.** No `std::thread`, `std::async`, `dispatch_async`, or thread pool
  anywhere in `src/`. On an M4 Pro this leaves most of the machine idle. Recoil multithreads
  pathing and parts of rendering, and `CSyncChecker::InSyncedCode` exists precisely to police
  the boundary.
- **Serialisation.** Recoil's `creg` reflects every sim class for save/load and for reconnect.
  This tree has `ScenarioSave` (208 lines) for map scenarios only. Note that a save system and
  the replay-hash claim interact: `SimObjectIDPool`'s constructor comment
  (`SimObjectIDPool.h:17-19`) exists *because* Recoil learned that fresh and reloaded clients
  must desync if allocation differs. Building save/load later, onto a deterministic core, is the
  right order — but the deterministic core has to survive it.
- **Netcode.** `rts/Net` is 7,009 lines. §1.3's success criterion is a *replay* claim, not a
  *lockstep* claim, which is the honest scoping — but it means the hardest half is untouched.
- **Logging.** Recoil has a levelled, sectioned logging framework. This tree prints.

### 3.6 No vision, no shields, no transports, no veterancy

All four exist in Recoil (`losHandler` reaches 34 files). `Events.hpp:37-41` handles the
consequence gracefully — it documents that Recoil nils an attacker triple when the attacker is
not visible, and states that filtering will happen on the way out of the queue rather than by
making the queue lossy. That is the right architecture for a feature that does not exist yet.

---

## 4. Where the two are close, and the copying was done well

### 4.1 Cancel-queued semantics — faithfully ported, correctly attributed

`CommandQueue.cpp:41-73` reproduces `CCommandAI::GetCancelQueued`
(`CommandAI.cpp:1311-1361`) accurately: scan from the back, one match only for non-build orders
(`CommandAI.cpp:1400`), positional match within `COMMAND_CANCEL_DIST`, build orders additionally
matched on type.

The transferability argument at `CommandQueue.hpp:32-40` deserves calling out as a model of how
to port a constant across games: 17.0f is kept **not because both games use elmos**, but because
it describes *one unit's own width*, and the FA corpus's median `SizeX` of 2.3 ogrids is 18.4
elmos — within 8%. That reasoning is more careful than the original, which has no comment at all.

Two divergences, both defensible:

- Recoil matches `CMD_ATTACK` against a 1-param `CMD_FIGHT`; there is no fight order here.
- Build matching is stricter (same type required) where Recoil compares footprint overlap.
  `CommandQueue.hpp:53-56` argues correctly that this is stricter in the right direction.

### 4.2 Events as data vs Recoil's call-in table

`System/Events.def` declares 173 events consumed by `CEventHandler` (797-line header) and
dispatched to registered `CEventClient`s. `Events.hpp:48-73` declares ten, as a flat POD in a
per-tick queue.

Different in kind rather than in quality. Recoil's is a *dispatch* mechanism for scripts; this is
a *record* of what happened. The flat-struct choice (`Events.hpp:79-83`) has a specific payoff
Recoil's does not: an `Event` is trivially copyable, so the event stream can be hashed and logged
alongside the command log. Given that determinism is the thesis, that is the right shape.

The `UnitTaken` omission is well-reasoned (`:31-35`): declaring a kind nothing can emit puts a
branch in every consumer for an unreachable case.

### 4.3 The snapshot vs `drawPos`

`Snapshot.hpp:35-45` chooses a per-tick copy over Recoil's derived
`drawPos = pos + speed * timeOffset` (`SolidObject.h:431`), and argues it on two grounds: at 5 Hz
the interpolation window is 200 ms, long enough that reading half-updated sim state is visible;
and a per-player snapshot is where fog of war attaches later, which a derived `drawPos` has
nowhere to put. Both are right. The cost is measured rather than guessed — 56 bytes × 5,000 units
= 280 KB/tick.

The observation at `:26-29` is the sharpest framing of the trade in the codebase: Recoil enforces
the sim/render seam through its *type system*; a copy the renderer cannot write to is the cheap
version of the same guarantee. Note the irony — Recoil's type-level enforcement
(`SyncedPrimitive.h:6`) is compiled out of release builds, so in shipping Recoil the seam is a
discipline too.

---

## 5. Findings — things worth fixing

Ordered by consequence. None is fatal; several are small.

### 5.1 The float ban has a hole on the path it most needs to cover

`tools/check_no_sim_floats.sh:44-46` detects floats by grepping for the **tokens** `float` and
`double` in declaring position. It cannot see a *conversion call*. And there is one, in the sim,
on the determinism-critical path:

```
src/core/sim/Command.cpp:275
    .position = {fxToFloat(command.targetX), 0.0f, fxToFloat(command.targetZ)},
```

`Command.cpp` is **not** in the exemption list, yet the conversion passes the check because
`fxToFloat` does not contain the token `float` at a word boundary. The resulting float triple is
then fed to the state hash as raw IEEE bits (`StateHash.cpp:80-84`).

This is not currently a desync risk — `int32 → float` is IEEE-defined and reproducible — but two
things are true and worth saying plainly:

1. `PLAN2 §2.2` claims the sim is "fixed point END TO END (enforced)". It is not quite: there is
   one float triple in sim state and the enforcement mechanism structurally cannot see the code
   that fills it. The tree half-knows this — `Economy.hpp` is exempted with the note "declares
   `Construction::position`, same reason" — but the *conversion site* is unlisted and unguarded.
2. The next `fxToFloat` in the sim will also pass silently, and it may not be benign.

**Fix:** add `fxToFloat|fxToDouble|static_cast<float>|static_cast<double>` to the pattern, or
better, invert the check — allow `Fx`/`Mag`/`Brad`/integer types and flag everything else.
Then finish P2.5 and make `Construction::position` a `std::array<Fx, 3>`.

### 5.2 `Movement.hpp:29-51` is a stale rationale the plan has already retired

The header still argues at length for 10 Hz on the grounds that *"Supreme Commander's own
gameplay scripts could later run on this sim unmodified"* and that *"this is not a constant to
change later"*. `PLAN2.md §1.1` explicitly retires both:

> This retires a live justification in our own source: `core/sim/Movement.hpp:30-36` argues for
> 10 Hz so *"Supreme Commander's own gameplay scripts could later run on this sim unmodified."*
> Under D2 that is moot — the rate is a knob.

Compatibility is a stated non-goal (D-non-goal, §1.1), and D2 made the rate configurable at
5–50 Hz. A reader arriving at `Movement.hpp` first is told the opposite of what the project
decided. In a codebase whose main asset is that its comments are trustworthy, this is a
disproportionate cost. Same file, line 51: `kTicksPerSecond` survives as a bare constant whose
comment has to explain why it is not the thing `check_no_tick_literals.sh` forbids.

Also `Movement.hpp` includes `core/map/HeightField.hpp` twice (lines 3 and 9).

### 5.3 `SlowUpdate.hpp:15` quotes 16 frames; Recoil's constant is 15

```
GlobalConstants.h:60:  static constexpr int UNIT_SLOWUPDATE_RATE = 15;
UnitHandler.cpp:355:   if ((gs->frameNum % UNIT_SLOWUPDATE_RATE) == 0)
```

The header says "a hardcoded **16**-frame cycle" and derives "0.533 s at `GAME_SPEED 30`". It is
15 frames and 0.5 s. The argument is unaffected — the point is that it is a *frame count* — but
the number is quoted as a citation and one-line fixes to citations are cheap.

### 5.4 `check_no_sim_globals.sh` quotes a file total that does not match the tree

The header states `gs->` appears in *"113 of its 1,463 files"*. Recounted against this checkout:
119 files, and the non-`lib` source total is 1,518. The other figures (`teamHandler` 78,
`unitHandler` 69) match exactly. Likely measured against a slightly older checkout; worth a
refresh, since the whole point of the file is that its numbers are checkable.

### 5.5 The app target does not currently build

```
src/render/FxRenderer.mm:40: error: cannot define or redeclare 'setGroundDecals' here
                             because namespace 'rm' does not enclose namespace 'Renderer'
```

This is **uncommitted work in progress** — `FxRenderer.mm`, `MapRenderer.mm`, `UiRenderer.mm`,
`UnitRenderer.mm`, `RendererInternal.hpp` and `shaders/` are all untracked, with `CMakeLists.txt`
and `Renderer.mm` modified. The committed tree at `8631950` is presumably fine, and `librm_core.a`
and `rm_tests` build and pass. Recorded here only so the review's build result is not misread as
a regression in committed code: the new `.mm` files define members outside the `rm` namespace.

### 5.6 `main.mm` at 4,996 lines is the worst structural problem in the tree

It contains map loading, texture registry, prop loading, model loading, roster building, unit
spawning, blueprint resolution, passability set construction, the scripted opponent's opening,
CLI parsing for a dozen flags, and the frame loop. `Renderer.hpp` is 801 lines declaring one class
with 118 public methods.

Recoil, for all its faults, splits this: `PreGame`, `GameSetup`, `LoadScreen`, `Game`,
`WorldDrawer`, plus a `Drawer` per concern (`CommandDrawer`, `HUDDrawer`, `IconHandler`,
`ShadowHandler`, `LuaObjectDrawer`, …). `PLAN2 §2.2` lists "drawer split" under Renderer's
Missing, and the uncommitted work in §5.5 is evidently starting it — this is the right next move.

The cost is not aesthetic. `Skirmish.hpp:24-31` records that the last time logic lived in
`main.mm`, **two tick loops diverged and the whole suite stayed green** because
executable-only translation units cannot be linked by tests. Every one of the 4,996 lines still
there is untestable for the same reason, and it contains real logic — `spawnCommanders`,
`orderFirstExtractors`, `resolveBuildable`, `refreshWreckDecals`.

### 5.7 One-tick event lifetime is a constraint worth stating louder

`Events.hpp:110-114` clears the queue every tick and says a consumer that needs history keeps its
own. Correct, and matching Recoil's call-in semantics. But combined with `Skirmish.hpp:76-82`'s
note that *the caller* owns clearing, the contract is now split across two files: the sim appends,
the caller clears, and getting the clear point wrong silently loses events — which the header
records already happened once, to two event kinds that "were declared, emitted, and never once
observable". Consider making the frame boundary explicit (a `beginTick`/`endTick` pair, or a
frame token) rather than a convention.

---

## 6. Two things Recoil does that are worth stealing later

1. **`IPathManager` as an interface with two implementations.** Recoil ships HAPFS and QTPFS
   behind one interface and selects at runtime. When this engine's coarse A* becomes the
   bottleneck — and it will, well before 5,000 units — having introduced the seam first means the
   replacement is a new file rather than a rewrite. The seam costs almost nothing today.

2. **A weapon *class* hierarchy driven by data, not a weapon *struct*.** `unitdef::Weapon` already
   carries `role` and `arc` as enums it mostly ignores. The step Recoil took — `WeaponLoader`
   constructing a `CWeapon` subclass from the def — is compatible with D6 (behaviour in C++,
   varying by data) and is what makes beams, torpedoes and AA expressible at all. Doing it before
   the fifth weapon type is added is much cheaper than after.

---

## 7. Assessment

Judged as what it is — a from-scratch engine at roughly 32k lines against a mature one at 351k —
this compares extremely well on the axes it chose to compete on, and honestly reports the axes it
has not started.

The core thesis is sound and the execution of it is better than the reference. Fixed-point
determinism, no globals, generational handles, and a tick with a declared pass order are four
decisions that each individually beat Recoil's equivalent, and they compound: the no-globals rule
is what makes the two-sims-one-process determinism test possible, the stable slots are what make
the `SlowUpdate` rotation correct, and the declared pass order is what makes the spatial index
rebuildable once per tick. That is a coherent architecture rather than four good ideas.

Against that: the sim is thin where an RTS is thickest. Scalar damage, one projectile type, one
motion type, one pathfinder with no dynamic blocking, and no vision. `PLAN2 §2.2`'s 73% for the
Sim row is measuring foundations, not gameplay — the foundations are genuinely 73% or better, and
the gameplay surface is closer to 25%. The plan's own Missing column is accurate; it is the
weighting that reads optimistic.

The three things worth doing next, in order: **(1)** close the `fxToFloat` hole in §5.1, because
the determinism claim is the project's whole reason to exist and the check that guards it has a
gap; **(2)** finish the renderer/`main.mm` split already in flight, because 4,996 untestable lines
is how the last silent divergence happened; **(3)** make damage a vector before more weapons are
added, because retrofitting armour classes touches every combat call site.

> **All three are done as of 2026-08-21** — (1) P10.0, (2) P7.3/P7.5, (3) P10.1. See Resolution
> at the top. What comes next is P10.3 onward: projectile kinds, the pathing cost field, shared
> flow fields, and the first threading, in that order and for the same reason this list was
> ordered — retrofit cost, not value.

The documentation is the project's second-best asset after the determinism model, and the two are
related — the reason the fixed-point width could be overruled on evidence is that the evidence was
written down where the decision was. Keep §5.2, §5.3 and §5.4 tidy; a codebase that earns trust
through citation loses more from a stale one than a codebase that never cited anything.
