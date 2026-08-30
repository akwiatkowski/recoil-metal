# Retail Forged Alliance executable analysis plan

This is the durable control document for a long, multi-session clean-room analysis of the
owned retail *Supreme Commander: Forged Alliance* executable and its relationship to the
shipped Lua, blueprints, archives, maps, and data.

The purpose is not to decompile the entire program. The purpose is to answer bounded questions
about native engine behavior before Recoil Metal implements more Forged Alliance parity.

## What is ready

**Campaign state:** the map is built and the campaign has moved from finding things to reading
them. The owned, hash-identified installation (Steam app `9420`) is preserved and hash-verified;
Ghidra 12.1.2 holds an analyzed project whose database now carries **2,979 named functions** after
the recovered knowledge was written back into it. The Lua API is fully recovered — **1,182
callables** with signatures, documentation and native addresses (`C-032`), 549 of them
machine-verified against the engine's own arity checks. **Eight work packages are at `Analyzed`**
and the object-layout table holds forty-odd offsets, each naming its base pointer.

Two accelerators from session 02 still underpin everything: the executable ships **complete MSVC
RTTI** (2,728 `Moho::` descriptors, 471 concrete classes), and `MohoEngine.dll` carries 4,875
mangled symbols from the same Perforce tree. **But see `C-075`: the DLL is a naming oracle only.**
It is a different build and its *behaviour* diverges — one intel function is live there and dead in
retail, and `Unit` offsets differ by `0x10`. Transfer names from it, never offsets and never
behaviour.

An authoritative Steam depot manifest is still needed to prove the exact retail build.

**Next exact action:** find the engine's **default `Footprint` size** for a unit blueprint that
omits the field. It is the single unknown blocking `C-073`(a) — Recoil Metal centres skirt
rectangles on the unit instead of computing `pos − Footprint.Size/2 + SkirtOffset`, which misplaces
adjacency for **31 structures including every naval factory**. Many small structures omit
`Footprint` entirely, so the fix cannot be written until the default is known, and it is set in
native code rather than in the shipped Lua. Start from `Unit:GetSkirtRect` and the blueprint
loader's footprint field registration.

| Readiness | Count | Meaning |
|---|---:|---|
| **Not ready** | 24 | Recoil Metal is absent or materially incomplete for this work package. |
| **Ready but not confirmed** | 21 | Recoil Metal has a tested implementation, but native retail semantics have not been confirmed from the executable. |
| **Confirmed with EXE analysis** | 0 | The implementation is ready and the relevant retail native behavior has been traced, recorded, and compared. |

Counts as of session 04. `WP-35` moved to ready when veterancy was implemented; `WP-02` moved when the
registration map was completed. Nothing is Confirmed yet, because confirmation additionally requires a
traced native path with at least one caller and one downstream effect — see the confirmation gate.

### How much of the executable is actually analyzed

Measured, not estimated — regenerate with `tools/re/ReportCoverage.java`. Kept here because a
campaign like this accumulates a *feeling* of progress that runs far ahead of the real number,
and because the honest denominators are what stop the next session over-claiming.

| Measure | Value | Share |
|---|---:|---:|
| Executable bytes in `ART-E001` | 9,101,312 | — |
| Code bytes Ghidra places inside a function | 5,855,603 | 64% of executable |
| Functions Ghidra identified | 32,268 | — |
| Functions carrying a meaningful name (RTTI, demangled export, or analyst) | 2,979 | 9.2% |
| **Function starts recovered from vtables that Ghidra never found** (`C-117`) | **4,514** | — |
| Lua callables mapped name → address (`C-032`) | 1,182 | — |
| …of those, resolved to the real native method (`C-035`) | 797 | 2.6% of all functions |
| …of those, arity machine-verified against the code (`C-039`) | 549 | 90.3% of the 608 checkable |
| **Vtables recovered / slot rows** (`C-117`) | **4,049 / 32,678** | 3,861 classes |
| **Object fields recovered, base pointer read not guessed** (`C-128`, `C-133`, `C-137`, `C-143`) | **~500** | 51+ classes |
| **Enum (name, value) pairs recovered** (`C-122`, `C-125`) | **541** | 51 enums + 4 motion tables |
| **Functions attributed to a source file and line** (`C-114`) | **560** | 1.8% — a measured ceiling |
| **Mutable globals reachable from the sim frontier** (`C-136`) | **282**, 11 shared | the whole surface |
| Functions read instruction by instruction by an analyst | **~180** | ~0.56% |
| Work packages at **Confirmed with EXE analysis** | 0 of 45 | **0%** |

Read that table as three different senses of the word "analyzed", which are worth keeping apart:

- **Inventoried** is now largely done, and session 13 is why. The type system, the complete
  script API, every vtable, ~500 field offsets, 541 enum constants and the whole mutable global
  surface are in hand. The binary describes itself in more places than the campaign first
  assumed, and four separate self-description veins have now been mined out.
- **Understood** has moved from "barely started" to "started". Around 180 functions have been
  read as instructions, and several have been traced through a caller to a downstream effect —
  the tick's stage order (`C-142`), the grid query's iteration order (`C-138`), the reclaim
  rate loop (`C-147`). That is the work the confirmation gate actually needs.
- **Bounded** is where most packages sit: a named mechanism plus at least one refuted
  alternative. Eight packages moved up a level in session 13, including `WP-26`, which the plan
  had called the weakest link in the map.

**The `Confirmed` column is still zero, and that number is the honest one.** Nothing has yet
been taken through the full gate of a traced native path matched against a tested
implementation. Until one package does, the cost of that gate is unknown.

The 36% of executable bytes outside any recognised function is mostly padding, jump tables,
switch data, and the statically linked C/C++ runtime — not hidden game logic. It has not been
examined and there is currently no reason to.

## Analysis strategy: how to not analyze 31,086 functions

The executable cannot be read. It does not need to be. This section is the campaign's method,
and it is derived from measurement rather than from preference — the numbers below are
reproducible with `tools/re/DumpCallGraph.java` plus the reachability snippet in session 06.

### 1. The Lua boundary is the natural cut, and it is small

Everything the simulation does is reachable from the Lua API, because that is the boundary the
game's own scripts talk through. Measuring the transitive closure of the call graph from the
recovered entry points bounds the work:

| Root set | Functions | Share of binary | Code bytes |
|---|---:|---:|---:|
| Whole binary | 31,086 | 100% | 5,718 KB |
| Reachable from the entire Lua API (794 roots) | 4,929 | **15.9%** | 1,010 KB |
| Reachable from **simulation** subsystems only (303 roots) | 2,562 | **8.2%** | 484 KB |
| **Depth 1 from the simulation roots** | **618** | **2.0%** | — |

So **84% of the binary is unreachable from any script-visible behavior** — renderer, audio
mixer, netcode, wxWidgets, the statically linked CRT. It cannot affect simulation parity and
is out of scope permanently, not merely deprioritised.

Per subsystem, the frontier is smaller still. These are the depth-1 counts — the functions the
Lua API calls *directly*, where engine semantics actually live:

| Subsystem | Roots | Depth 1 | Depth 2 | Full closure |
|---|---:|---:|---:|---:|
| combat | 94 | 199 | 362 | 1,494 |
| lifecycle | 80 | 180 | 304 | 1,266 |
| movement | 24 | 75 | 130 | 946 |
| intel | 19 | 73 | 151 | 986 |
| orders | 14 | 70 | 154 | 908 |
| build | 16 | 66 | 158 | 942 |
| transport | 13 | 66 | 144 | 1,046 |
| **economy** | 18 | **51** | 109 | 771 |
| shields | 3 | 19 | 50 | 665 |
| ordnance | 7 | 14 | 30 | 525 |

Read that as: *confirming the economy means understanding on the order of 51 functions, not
31,086.* The closures overlap heavily because they share containers, maths and allocators —
which is a feature, since that infrastructure gets understood once and then stops mattering.

**Work the depth-1 frontier, not the closure.** Depth 1 is engine semantics. Depth 3+ is
almost entirely `std::vector`, string handling, allocators and maths.

### 2. Four tiers of method, cheapest first

Measured against this campaign's own effort. Do not reach for a lower tier until the one above
it is exhausted.

| Tier | Method | Cost | Yield here |
|---|---|---|---|
| **0** | **Answer from shipped Lua/blueprints instead** | minutes | `WP-35` fully specified with **no executable work at all** (`C-024`) |
| **1** | **Whole-binary pattern extraction** | seconds | RTTI 471 classes; exports 4,083 members; registration scan 1,182 callables with signatures; arity check 549 verified |
| **2** | **Index lookup** — jump to a known address | minutes | every damage entry point located once `C-032` existed |
| **3** | **Read disassembly** | hours | 9 functions total, but 2 of them produced `C-037` and `C-038` |

Tier 0 deserves emphasis because it is counter-intuitive: Gas Powered Games put most *rules*
in Lua. The executable owns ordering, numeric representation, data structures and timing —
and little else that matters. Reaching for Ghidra to answer a question the shipped scripts
state outright is the most common way to waste a session.

Tier 1 is where the leverage is. Every large result in this campaign came from noticing that
the binary describes itself somewhere and then scanning for that description across all of it
at once. Sources already exploited: RTTI type descriptors, MSVC export names, per-method
static initialisers with doc strings, native arity checks.

**Session 13 ran the remaining four in parallel, and the yields differed by two orders of
magnitude — which is the reusable lesson.** Ranked by what they actually returned:

| Source | Yield | Verdict |
|---|---|---|
| **Enum registration tables** (`C-122`) | **525 (name, value) pairs across 51 enums**, including all 45 bits of the unit-state mask | The best Tier-1 vein found to date |
| **Field-offset access patterns** (`C-128`) | **484 fields across 51 classes**, up from ~40, with the base pointer *read* from the receiver's type descriptor | Second best; validated 12/12 |
| **Vtable slot tables** (`C-117`) | **32,678 slots over 4,049 vtables**, validated 217/217 against exported `??_7` symbols | Third; also found 4,514 functions Ghidra missed |
| **Assertion strings** (`C-114`) | 72 source files, 560 functions — **1.8% of the binary**, and none of it damage, shields, economy or weapons | Largely a negative result |

The pattern: **a source is worth its cost in proportion to how systematically the compiler or
the engine emitted it.** Enum registration and vtables are emitted by construction for every
instance; assertion strings are emitted only where a programmer chose to write one, which
turns out to be almost nowhere in the subsystems this campaign cares about. Prefer veins the
toolchain produces mechanically over veins that depend on authorial habit.

### 3. Prefer questions whose answers are one line

A good question has an answer that fits in a table cell and changes code:

- *good*: "what numeric type is health, and at what offset?" → `float`, `Entity+0x98` (`C-038`)
- *good*: "how many arguments does `Damage` take?" → five, not four (`C-040`)
- *good*: "what order do the sim stages run in?"
- *bad*: "how does damage work?" — unbounded, and no single answer changes anything

If a question cannot be phrased so that a wrong answer would change a line of Recoil Metal,
it is not worth executable time.

### 4. `ART-D001` as a name oracle — tested, and the answer is "yes, but narrowly"

Session 07 ran this. The prediction was that it would transfer "thousands of names" and be
the campaign's biggest lever. **That was wrong, and how it was wrong is the useful part.**

`MohoEngine.dll` has 21,152 named functions against the executable's 943, and both come from
the same Perforce tree (`C-002`), so bulk matching via BSim looked like a Tier 1 operation
with Tier 3 value. Measured (`C-041`):

| Region queried | Matched | With a real name | Accuracy |
|---|---:|---:|---|
| The executable's 870 already-named functions (mostly CRT/MFC) | 164 | 54 | 84% peak, **degrading above sig 30** |
| The 626-function depth-1 engine frontier | 471 | **98** | **78%** corroborated at sig ≥ 40, truly higher |

Three lessons, all of which generalise beyond BSim:

- **Small functions match everything.** `_sscanf` → `Moho::PLAT_SetRegistryValueDword` at
  similarity **1.000**. A short function's feature vector is degenerate. **Use significance,
  never similarity, and distrust any match on a function under a few dozen instructions.**
- **The oracle only helps where the oracle itself has names.** Querying the Lua *bindings*
  returned 472 matches and **zero** named ones — because those bindings are generated glue in
  both binaries, and Ghidra named them in neither. Aim at the layer below the glue.
- **Yield is modest but not uniform in value.** 98 candidate names is not "thousands". But one
  of them is `SIM_MetaImpactArea` (`C-042`), the damage anchor that two sessions of working
  forwards from Lua had failed to reach. A lever that produces one decisive address is worth
  running even when its bulk yield disappoints.

So the technique earns a permanent place — as a **frontier tool at Tier 1 cost**, not as a
bulk renamer. Its output is `maybe_` names, never `fa_` names.

**And a harder limit, found in session 11 (`C-075`): the DLL is a NAMING oracle, never a
BEHAVIOURAL one.** The two binaries are different builds of the same tree and they differ in
what they do — one intel function is live in the DLL and has zero callers in retail, and
another does terrain occlusion in the DLL and none in retail. A name transferred from
`ART-D001` tells you what a function is *called*. Reading the DLL's *body* and attributing
that behaviour to retail is a mistake this campaign has now made once and recorded.

### 5. Triage by parity impact, and say no

Rendering, audio, UI internals and netcode cannot change a replay hash. They are `P3` in the
dashboard and should stay unexamined unless a specific user-visible defect demands it. The
campaign's purpose is simulation parity; a beautiful map of the renderer is a cost with no
return.

### 6. Stop rules

- **Timebox each question.** If a Tier 3 read has not answered a one-line question within a
  session, record it as unresolved with the exact next address and move on.
- **Irreducible ambiguity is a result.** The confirmation gate explicitly allows "the
  executable cannot distinguish these candidates" as an answer, provided the reason is stated.
- **A negative result is a result.** `veterancy` having zero native API (`C-024`) and
  `luaL_register` being absent (`F-008`) both saved more time than they cost.

### 7. Two failure modes this campaign has already hit

Both produced confident wrong answers, and both are cheap to avoid:

- **Inferring a mechanism from agreeing examples.** `C-036` read a shared thunk as "resolve
  the receiver" from two examples that were both methods. 112 global functions with no
  receiver run the same code. *Before generalising, find the case that should differ and
  check it.*
- **Differential analysis with a bad control.** `H-001` intersected four damage functions'
  callees and subtracted one unrelated binding's — but the control (`Unit::GetHealth`) was
  trivial and called no script machinery, so all the script plumbing survived and looked
  damage-specific. *A control must be at least as rich as the subject.*

## Multi-session hunting kit

The campaign is now long enough that the binding constraint is no longer *finding* things, it
is **not losing what was found between sessions** — including sessions run by a different
analyst or a different model. This section is the handover.

### The database is now labelled — start there, not in a text file

Until session 08 every Ghidra session opened 31,086 functions called `FUN_xxxxxxxx` and had to
re-derive context from TSVs in another window. `tools/re/ApplyKnownNames.java` has now written
the recovered knowledge into the program itself: **2,036 functions named and plate-commented**,
taking the named total from 943 to **2,979** and named code from 151 KB to 457 KB. Call sites in
the decompiler now read `fa_lua_Unit_GetHealth(...)` rather than `FUN_006cb7a0(...)`.

**Read the name prefix before trusting a name.** The prefix *is* the confidence:

| Prefix | Meaning | Trust |
|---|---|---|
| `fa_<subsystem>_<Scope><Name>` | Recovered from the registration scan (`C-032`), arity-verified for 549 of them (`C-039`) | Fact |
| `fa_luathunk_<Scope><Name>` | The generated 20-byte Lua wrapper for that method (`C-035`) | Fact |
| `maybe_Moho__<Class>__<Method>` | BSim candidate at significance ≥ 40 — **~80% accurate** (`C-041`) | Lead only |
| `Moho::...`, `FID_conflict:...` | Ghidra's own RTTI/demangler/FunctionID output — never overwritten by us | Fact |
| `FUN_...` | Untouched | Nothing |

Every applied name carries a plate comment with the Lua signature, the engine's own
documentation where it ships one, the subsystem, and the claim IDs behind it. A future session
can therefore trace any name back to its evidence without opening this document.

### Starting a deep session

```bash
# 0. Everything below assumes these two lines; Ghidra 12 needs JDK 21 and it is NOT on PATH.
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
export PATH="$JAVA_HOME/bin:$PATH"
HL=/opt/homebrew/Cellar/ghidra/12.1.2/libexec/support/analyzeHeadless

# 1. Confirm the artifact still matches the ledger before trusting any address.
shasum -a 256 ~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe
#   expect c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0

# 2. If the project is missing, rebuild it and re-apply knowledge (about 10 minutes).
build/re-fa/run-import.sh
python3 tools/re/extract_moho_methods.py --tsv build/re-fa/exports/moho.methods.tsv
python3 tools/re/annotate_moho_methods.py
$HL build/re-fa/project fa -process SupremeCommander.exe -noanalysis \
   -scriptPath tools/re -postScript ApplyKnownNames.java \
   build/re-fa/exports/moho.methods.annotated.tsv build/re-fa/exports/bsim.frontier.tsv

# 3. Read code. Two ways, and prefer the first — it needs no project lock, so it works
#    while other work is running.
objdump -d --start-address=0xAAAA --stop-address=0xBBBB <exe>
$HL build/re-fa/project fa -process SupremeCommander.exe -noanalysis \
   -scriptPath tools/re -postScript DumpRefsAndCode.java 0xADDR -- at
```

**Only one Ghidra process may hold the project at a time.** Parallel work must use `objdump`
and `tools/re/pe_reader.py`, which read the file directly and take no lock. All the derived
tables (`moho.methods.tsv`, `callgraph.tsv`, the RTTI list) exist precisely so that parallel
analysts never need the project at all.

### The hunting queue

Ordered by (implementation impact) x (1 / measured frontier size). Each entry is a question with a
starting address, because "continue analysis" is not a next action. The first six entries were
cleared in sessions 09–11, and the former entry 1 (the default `Footprint`) was **answered by
`C-109`** — `ceil(SizeX)` from the top-level key, read at `0x00518B60`. What remains is the
question of *applying* it, which is an implementation task (`C-073`(a)), not an analysis one.
What follows is what is left to analyse.

| # | WP | Question | Start at |
|---|---|---|---|
| 3 | `WP-12` | What `GetQueueClearingCmdType` (`CUnitCommandQueue+0x1c`) actually does — accessors are inlined everywhere with zero call sites. **Narrowed by `C-121`: it cannot be a `CCommandTask` virtual, and `CUnitCommandQueue` has no vtable, so it is inside a specific `TaskTick` or a static method** | `C-096`'s open item; the 32 `TaskTick` addresses in `vtables.siblings.CCommandTask.tsv` |
| 4 | `WP-11` | Victory and defeat predicates. **Executable half closed by `C-126` — there is no native predicate; Lua pushes the verdict in via `SetArmyOutOfGame`. This is now a Tier-0 scenario-Lua question** | `ART-S007` scenario scripts; `SetArmyOutOfGame` `0x007115e0` for the boundary |
| 5 | `WP-22` | Aircraft mover states: takeoff, landing, fuel, staging. **`C-125` recovered the four motion-event enums and their fields; what is left is `CalcMoveAir` itself** | `CUnitMotion::CalcMoveAir`, `ProcessFuelLevels`, `AttemptingToLand`; `EAirCombatState` type info at `0x006be170`, vtable slot `0x00e7a724` |
| 6 | `WP-43` | Replay command payload and how the client abstraction records it | `CReplayClient`, `CCommandDB`, `CMessageStream`, `Sim::GetCommandSources` |
| 7 | `WP-27` | Two unopened per-candidate rejects in target acquisition. **Second one is answered twice over — read as behaviour (`C-119`) and named `IsTargetExempt` (`C-130`). All that remains is pinning its CALL SITE**, since the slot is only ever loaded into a register | `0x006de340`; `CAiAttackerImpl` slot `+0x58` = `0x005dde10` |
| 8 | `WP-27` | Whether a weapon with an empty priority table acquires **nothing** — if so it is load-bearing | `0x006e2930`, the suspected default-priority seeder |

### Object layout, the most reusable artifact

Field offsets outlive every other kind of finding: they are small, they are exact, and they are
what parity questions actually turn on. Only one is recovered so far — `Entity+0x98` is health
as a `float` (`C-038`) — and it immediately produced a hard constraint on replay parity. **A
session that recovers ten offsets has done more for the campaign than one that reads one
function very carefully.** The cheap way is the small getters: each Lua accessor is a handful
of instructions and reveals one offset.

**Always name the base pointer.** `Moho::Unit` has three complete-object locators and its
`Entity` subobject sits at `Unit+0x08`, so `Unit+0x98` and `Entity+0x90` are the *same field*
(`C-053`). An offset without a base is not a fact.

| Base | Offset | Field | Type | Evidence |
|---|---|---|---|---|
| `Moho::Entity` | `+0x4C` | spatial-grid record (cell rect, grid*, dedup flag, layer flags) | struct | `C-064` |
| `Moho::Entity` | `+0x68` | entity id (as hashed by the checksum) | `int` | `C-057` |
| `Moho::Entity` | `+0x90` | **health** | `float` | `C-053` |
| `Moho::Entity` | `+0x94` | **max health** | `float` | `C-053` |
| `Moho::Entity` | `+0x99` | dead/destroyed flag | `byte` | `C-060` (Medium) |
| `Moho::Entity` | `+0xAC`–`+0xB4` | position (float3); 28 bytes hashed as transform | `float[3]` | `C-057`, `C-060` |
| `Moho::Entity` | `+0x148` | owning `Sim*` | ptr | `C-060` |
| `Moho::Entity` | `+0x178` | collision shape (virtual exact test at slot `+0x20`) | ptr | `C-066` |
| `Moho::Entity` | `+0x1b9` | queued-for-destruction flag | `byte` | `C-046` |
| `Moho::Unit` | `+0x08` | the `Entity` subobject | — | `C-053` |
| `Moho::Entity` | `+0x14C` | owning army (`CArmyImpl*`) — **restated from `Unit+0x154` by `C-132`**; it is an `Entity` field, so props and projectiles have it too | ptr | `C-068`, `C-132` |
| `Moho::Unit` | `+0x2e8`/`+0x2ec` | produced this tick [E, M] | `float` | `C-068` |
| `Moho::Unit` | `+0x2f0`/`+0x2f4` | consumed this tick [E, M] | `float` | `C-068` |
| `Moho::Unit` | `+0x470`/`+0x474` | consumption per second [E, M] | `float` | `C-068` |
| `Moho::Unit` | `+0x478`/`+0x47c` | production per second [E, M] | `float` | `C-068` |
| `Moho::Unit` | `+0x52c` / `+0x534` | `CEconStorage*` / `CEconRequest*` | ptr | `C-068` |
| `Moho::Unit` | `+0x538` / `+0x539` | consumption active / production active | `byte` | `C-068` |
| `Moho::Unit` | `+0x53c` | resource-consumed ratio | `float` | `C-071` |
| `Moho::Unit` | `+0x568` | armour multiplier map (`std::map`, value `float` at node `+0x28`) | map | `C-060` |
| `Moho::CEconomy` | `+0x18`/`+0x20`/`+0x30`/`+0x38` | stored / income / requested / usage, each [E, M] | `float` | `C-068` |
| `Moho::CEconomy` | `+0x40 + 8i` | max storage — **`uint64`, the only rounded quantity** | `int64` | `C-069` |
| `Moho::CEconomy` | `+0x58` | intrusive `CEconRequest` list head | node | `C-068` |
| `Moho::CEconRequest` | `+0x08` / `+0x10` | demand[2] / allocated[2] | `float` | `C-068` |
| `Moho::Unit` | `+0x558` | `IAiSiloBuild*` (ammo lives here, not on `Unit`) | ptr | `C-081` |
| `Moho::Unit` | `+0x544` | attacker/weapon **manager** (`IAiAttacker`), not a bare container — `C-132` | ptr | `C-081`, `C-132` |
| `Moho::Unit` | `+0x4A0` | 64-bit unit-state mask — **all 45 bits named, `C-123`**; `+0x4A0` low dword, `+0x4A4` high | `u64` | `C-081`, `C-123`, `C-131` |
| `Moho::Unit` | `+0x2AC` | **generic work progress** 0..1 — silo build is one consumer, `C-132` | `float` | `C-081`, `C-132` |
| `Moho::Unit` | `+0x298` / `+0x294` | shield ratio / fuel ratio (**no `Moho::Shield` receiver exists**) | `float` | `C-131` |
| `Moho::Unit` | `+0x120` | current layer (range-checked `<= 0x20`, per `LAYER_` in `C-124`) | `int32` | `C-128` |
| `Moho::Unit` | `+0x498` | script-bit mask | `u32` | `C-128` |
| `Moho::Unit` | `+0x4B4` | task/command queue — `+0x10`/`+0x14` are `begin`/`end` of 8-byte entries | ptr | `C-131` |
| `Moho::Entity` | `+0x9C`–`+0xA8` | orientation quaternion, immediately before position | `float[4]` | `C-133` |
| `Moho::Entity` | `+0xD8` | fraction complete | `float` | `C-098`, `C-133` |
| `Moho::Entity` | `+0x1DC`/`+0x1E0`/`+0x1E4`/`+0x1E8` | visibility to focus player / allies / enemies / neutrals | — | `C-133` |
| `Moho::UnitWeapon` | `+0x94` / `+0x90` | damage / damage radius | `float` | `C-129` |
| `Moho::UnitWeapon` | `+0xA0` / `+0xD0` | owning `Moho::Unit*` / current target | ptr | `C-129` |
| `Moho::Projectile` | `+0x2C8` / `+0x330` | damage / **absolute expiry tick**, not a countdown | `float`/`int32` | `C-129` |
| `Moho::Entity` | — | **`sizeof` = `0x270` (624 bytes)** | — | `C-144` |
| `Moho::Shield` | — | **`sizeof` = `0x270`; adds NO members over `Entity`** | — | `C-144` |
| `Moho::Entity` | `+0x90` / `+0x94` | shield current / max — **the same fields as hull health** on a `Shield` entity | `float` | `C-144` |
| `Moho::Entity` | `+0x6C` | blueprint pointer | ptr | `C-148` |
| `Moho::Entity` | `+0x4C` | `CollisionDBRect`: `u16 cellX0, cellZ0, cellCountX, cellCountZ`, then `CollisionDB*` `+0x54`, dedup flag `+0x58`, kind flags `+0x5C` | struct | `C-137` |
| `Moho::Sim` | `+0x908` / `+0xA3C` | `COGrid*` (broad phase at `COGrid+0x04`) / world shield list | ptr | `C-137`, `C-144` |
| `Moho::CollisionDB` | `+0x00`..`+0x30` | `cellsX`, `cellsZ`, `cellCount−1` mask, `shift`, buckets A/B/C, free list, node blocks | — | `C-137` |
| `CDamage` | `+0x34`/`+0x38`/`+0x40`/`+0x48`/`+0x4C`/`+0x50`/`+0x5C`/`+0x60`/`+0x7C`/`+0x80` | kind / instigator / target / outer radius / inner radius / centre / amount / type / damage-friendly / direction | — | `C-143` |
| `CUnitReclaimTask` | `+0x1C`/`+0x24`/`+0x44`/`+0x50` | engineer `Unit*` / state / target weak-ref / position | — | `C-147` |
| build task | `+0x5C`/`+0x88`/`+0x8C`/`+0xC0` | rebuild head start / blueprint / position / wreck weak-ref | — | `C-148` |
| `CArmyImpl` | `+0xE8` | allegiance bit vector (base index at `+0xE0`) | vec | `C-143` |
| blueprint | `+0x5C` / `+0xD8`,`+0xD9` / `+0x518` | category-bit index / footprint SizeX, SizeZ (`u8`) / `RebuildBonusIds` vector | — | `C-148`, `C-109` |
| `Moho::ReconBlip` | `+0x4C4` | per-army array, stride `0x34` (52 bytes) | ptr | `C-133` |
| `CAiSiloBuildImpl` | `+0x0C`/`+0x18` | stored ammo, tactical / nuke | `int` | `C-081` |
| `CAiSiloBuildImpl` | `+0x44` / `+0x48` | total ticks / elapsed ticks for one missile | — | `C-081` |
| entity layer mask | — | `0x100` unit, `0x200` prop, `0x400` projectile, `0x800` beam | bits | `C-064`, `C-086` |
| `Moho::Sim` | `+0x50` / `+0xB0` | running MD5 context / 128-entry digest ring | — | `C-057` |
| `Moho::Sim` | `+0x8F8` / `+0x900` | beat number / tick — **two counters** | `u32` | `C-055` |
| `Moho::Sim` | `+0x904` / `+0x908` | `CRandomStream*` / `COGrid*` | ptr | `C-056`, `C-064` |
| `Moho::Sim` | `+0x930`/`+0x944`/`+0x958` | Motion / Script / CommandDispatch stages | `CTaskStage` | `C-054` |
| `Moho::Sim` | `+0xA5C` | dirty-entity list | list | `C-055`, `C-063` |
| `CRandomStream` | `+0x000`/`+0x9C0`/`+0x9C4`/`+0x9C8` | MT state[624] / `mti` / cached gaussian / flag | — | `C-056` |
| Lua `_c_object` payload | `+0x00` | native object pointer — **nulled on destroy** | `void*` | `C-044`, `C-045` |
| Lua `_c_object` payload | `+0x04` | type/class pointer, checked on cast | `Class*` | `C-044` |
| Lua binding context | `+0x44` | `lua_State*` | `void*` | `C-037`, `C-044` |
| `Moho::WeakPtr` node | `+0x00`, `+0x04` | target, next | `void*` | `C-045` |

### Two evidence traps found the hard way

Both cost real time in session 02. Any analyst — human or model — resuming this campaign should
read these before trusting a negative result.

1. **`grep` silently drops matches in the retail corpus, and `LC_ALL=C` alone does not fix it.**
   The shipped Lua is ISO-8859-1 with CRLF line endings. Under a UTF-8 locale, `grep` classifies
   files containing bytes like `0xA9` (`©`, present in every GPG copyright header) as binary and
   suppresses matching lines. A search for `Veteran` across `lua/sim/` returned *nothing* and
   briefly looked like proof that retail FA has no Lua veterancy. It has a complete implementation.
   **Correction, session 09:** `LC_ALL=C` was recorded here as the fix and **it is not sufficient**
   on this machine — `grep` is `ugrep` invoked with `-I` (skip binary), which skips these files
   regardless of locale. A whole-tree sweep for `adjac` returned a false negative for exactly this
   reason, in a session that had already been warned about the first version of this trap.
   **Use `LC_ALL=C grep -a`, or read the files in Python with `encoding='latin-1'`.** Three separate agents hit this independently in one session; one found that `lua/sim/Unit.lua` contains an actual NUL byte, so even a locale-correct `grep` treats it as binary. Treat `LC_ALL=C grep -a` as the standing incantation for this corpus.
   Treat any all-empty result over the corpus as unproven until re-run that way.
2. **`MohoEngine.dll`'s export table is not the engine's full surface.** Its 4,875 exports omit
   whole subsystems. Searching them for `Reclaim`, `Capture` and `Repair` returned zero hits, which
   read as "retail implements these in Lua". The executable's RTTI lists
   `Moho::CUnitReclaimTask`, `Moho::CUnitCaptureTask` and `Moho::CUnitRepairTask` as native classes.
   **The executable's RTTI is the authority on what exists; the DLL exports are only a signature
   dictionary for the subset they cover.**
3. **A parser that skips what it cannot classify reports a clean negative — the same shape as
   trap 1, found again in session 13 in a different tool.** A line parser matched `objdump`'s
   byte column as "hex pairs each followed by a space". llvm-objdump pads that column to a fixed
   width, **but a 10-byte instruction fills it exactly and gets no padding space before the tab**,
   so every 10-byte instruction was silently dropped. Ten bytes is precisely the encoding of
   `orl $imm32, disp32(%reg)` — so the tool was discarding the entire unit-state bit-mask table
   (`C-131`) while reporting success. Fixed by splitting on tab. **Generalise the rule rather than
   the instance: any parser over tool output must fail loudly on an unrecognised line, never skip
   it.** Both traps share one signature — a silent skip that looks exactly like "there is nothing
   there".

### Ready but not confirmed

`WP-01` binary map, `WP-02` Moho registration map, `WP-04` tick/update determinism, `WP-05` VFS, `WP-06` blueprint
ingestion, `WP-09` map loading, `WP-10` skirmish setup/factions, `WP-12` command queues,
`WP-15` economy, `WP-16` construction/upgrades/assist, `WP-19` adjacency, `WP-20` ordinary
ground movement, `WP-23` surface naval movement, `WP-26` spatial collision, `WP-27` target
acquisition, `WP-28` ordinary weapons/projectiles, `WP-30` damage/armor/death, `WP-31` ordinary
shields, `WP-33` intel/counter-intel, `WP-35` veterancy, `WP-39` core player UI, `WP-43` command
replay/state hashing.

### Not ready

`WP-00` artifact provenance, `WP-03` native object lifecycle, `WP-07` retail
Lua host, `WP-08` mod hooks, `WP-11` match rules/caps, `WP-13` persistent unit controls,
`WP-14` factory automation, `WP-17` repair/capture/gifting, `WP-18` complete wreck semantics,
`WP-21` formations/dynamic blockage, `WP-22` aircraft, `WP-24` submerged warfare, `WP-25`
transports/attachments, `WP-29` missiles/interception, `WP-32` shield variants, `WP-34`
enhancements, `WP-36` experimentals, `WP-37` terrain deformation, `WP-38` retail AI behavior,
`WP-40` advanced UI/controls, `WP-41` rendering/animation fidelity, `WP-42` audio/music
behavior, `WP-44` save/resume.

## Separate public briefing

Public explanation and artifact-generation instructions live in
[`recoil-metal-public-brief.md`](recoil-metal-public-brief.md). Keep this analysis ledger focused on
the much larger executable-research campaign.

## How to read the states

Readiness and knowledge are separate axes.

- **Not ready** means the Recoil Metal behavior is absent or materially incomplete. It remains
  Not ready even after the retail behavior is understood, until implementation and tests exist.
- **Ready but not confirmed** means our implementation works and is tested against its own
  contract, but might still differ from retail in ordering, rounding, edge cases, or architecture.
- **Confirmed with EXE analysis** requires both a ready implementation and executable evidence.
- **Confidence** measures how tightly non-EXE evidence already bounds retail behavior.
- **EXE evidence** measures reverse-engineering progress. It may become `Analyzed` while readiness
  remains Not ready.

`WP-00` through `WP-03` are research prerequisites rather than Recoil Metal behavior packages. For
those four packages, readiness instead measures whether the durable artifact, binary, registration,
or object-lifecycle map is complete enough to support downstream analysis. Their confirmation gate
uses that package-specific map and validation in place of an implementation and focused tests.

Allowed EXE evidence values:

| EXE evidence | Meaning |
|---|---|
| `Blocked` | Retail artifacts are unavailable. |
| `Unexamined` | Artifacts exist, but no relevant native path has been traced. |
| `Anchored` | At least one reliable string, registration, import, RTTI, vtable, or caller anchors the subsystem. |
| `Bounded` | Candidate mechanisms and discriminating observations are documented. |
| `Analyzed` | The decisive native path is traced and recorded with addresses and counterevidence. |

Confidence is about the current claim, not confidence in the analyst:

- **High:** several independent sources agree or shipped Lua/blueprints state the contract directly.
- **Medium:** documentation and observed data agree, but native ordering or edge cases are unknown.
- **Low:** several mechanisms remain plausible and materially change implementation.

## Scope and rules

### Included

- Final retail Forged Alliance skirmish behavior.
- Native engine semantics behind Moho Lua methods.
- Simulation, game rules, content loading, UI, rendering, audio, replay, and save behavior where
  they affect a skirmish or mod compatibility.
- Shipped executable, DLLs, SCD archives, Lua, blueprints, maps, and configuration files.

### Excluded unless separately approved

- Campaign scripting and cinematics except where they expose a shared engine contract.
- Retail network-protocol compatibility.
- DRM removal, protection bypass, or redistribution of proprietary code or assets.
- Copying decompiled implementation into this GPL project. We recover behavior and write an
  independent implementation from findings and tests.
- FAF additions unless clearly labelled as post-retail evidence.

### Safety and provenance

- Analyze only Olek's owned copy.
- Static analysis is the default, matching the Traffic Giant workflow.
- Never execute an unknown binary on the host. Any dynamic experiment needs a separately approved,
  isolated environment and a written question that static analysis could not answer cheaply.
- Keep executables, DLLs, archives, Ghidra projects, exports, and generated databases out of git.
- Store large input artifacts under `~/projects/llm/input/recoil-metal/retail-fa/`; treat that tree
  as append-only.
- Proposed Ghidra project directory: `build/re-fa/project` (not a dotted project directory).
- Durable conclusions belong in this document. Large decompiler output does not.

## Evidence vocabulary

Every durable claim gets an evidence code and a confidence.

| Code | Evidence |
|---|---|
| `EXE` | Retail executable or DLL disassembly/decompilation. |
| `LUA-R` | Shipped retail Lua from the owned disk. |
| `BP-R` | Shipped retail blueprints or other data. |
| `DOC-R` | Retail manual or publisher documentation. |
| `FAF` | FAF source that inherits or changes retail behavior. Version and retail/FAF distinction required. |
| `WEB` | Secondary internet documentation. Source URL required. |
| `OBS` | A narrow reference-game experiment. Environment and procedure required. |
| `RM` | Current Recoil Metal source or tests. File and line/test required. |

Every citation needs an exact locator. A bare evidence code is not enough:

| Code | Required locator |
|---|---|
| `EXE` | Artifact ID plus RVA/address and canonical or original function name. |
| `LUA-R` | Archive artifact ID, VFS path, and line/function/table key. |
| `BP-R` | Archive artifact ID, VFS path, and exact table/field path. |
| `DOC-R` | Document URL/edition plus page and section. |
| `FAF` | Repository commit, path, line/symbol, and whether behavior is inherited or changed. |
| `WEB` | URL, title, and access date. |
| `OBS` | Experiment ID, artifact IDs, environment, setup, input, and observed output. |
| `RM` | Recoil Metal commit, path and line/symbol, or exact test name. |

Do not promote `FAF` or `WEB` evidence to retail fact without either retail corpus support or EXE
confirmation. Do not use one plausible decompile as confirmation without checking callers,
callees, and contradictory evidence.

## Range of possibilities

Executable analysis is most efficient when each question has a known envelope before Ghidra is
opened. A **possibility envelope** has five parts:

1. **Known bounds:** what Lua, blueprints, documentation, internet research, and Recoil Metal tests
   already require or rule out.
2. **Candidate mechanisms:** the smallest set of materially different native implementations still
   consistent with those bounds.
3. **Excluded mechanisms:** tempting interpretations already contradicted by evidence.
4. **Discriminating observation:** the branch, data owner, call shape, update order, or constant that
   tells the candidates apart.
5. **Search anchors:** strings, Moho method names, Lua registrations, categories, blueprint field
   names, imports, RTTI names, vtables, or known values that lead into the native path.

Use qualitative candidates, not false numeric probabilities. If the candidate list is still
"anything," do more corpus research before decompiling.

### Possibility-envelope register

| ID | Subsystem | Known bounds | Remaining candidates | Decisive EXE observation / anchors |
|---|---|---|---|---|
| `PE-01` | Tick and update order | Retail scripts assume 10 Hz; callbacks and `WaitTicks` expose phase boundaries. Recoil Metal uses fixed ticks and an explicit pass order. | One central phased loop; per-object virtual updates; hybrid manager phases plus object callbacks. | Trace the simulation-frame entry point and calls around `OnCreate`, weapon, economy, motion, intel, and death callbacks. Anchors: `WaitTicks`, `ForkThread`, sim-rate constants, Lua callback strings. |
| `PE-02` | Native object model | Lua sees units, projectiles, props, armies, and manipulators as typed handles. Many Moho methods imply native receiver classes. | C++ class hierarchy with vtables; manager-owned records behind opaque handles; hybrid objects plus component managers. | Recover Lua userdata constructors/metatables and receiver registration tables, then follow one `Unit` method to its object layout. Anchors: `moho.unit_methods`, RTTI, vtables, registration names. |
| `PE-03` | Identity and lifecycle | Lua callbacks distinguish create, built, damaged, killed, destroyed, captured, and ownership transfer. Stale references must fail safely. | Stable integer IDs resolved by managers; pointers wrapped by userdata; generation/versioned handles; deferred destruction queue. | Trace userdata resolution and unit destruction. Look for ID tables, nulling, generation checks, deferred free lists, callback order. |
| `PE-04` | Ground paths | Units accept goals, obey motion classes, and move at large scale. FAF includes higher-level navigation because native pathing is insufficient for AI planning. | Per-unit A*; cached shared paths; hierarchical sectors plus local pathing; flow/heat fields; hybrid request service. | Trace `SetGoal`/move-order registration into path request allocation and waypoint production. Identify cache keys and whether multiple units share route objects. |
| `PE-05` | Formations | Retail UI can preserve/cycle formations and coordinate attacks. Units do not all receive the same final point. | Destination offsets assigned once at command issue; persistent group controller; shared path with per-unit slots; arrival-only spreading. | Trace multi-unit issue-command path and compare per-unit goals. Anchors: formation UI strings, order serialization, group/platoon classes. |
| `PE-06` | Dynamic blockage | Retail crowds eventually route around obstructions; exact replanning cadence and occupancy rules are unknown. | Static terrain path plus steering only; path invalidation on stuck timer; dynamic cost overlay; reservation grid. | Find movement stuck counters, path invalidation calls, occupancy writes, and timers after velocity falls below expected progress. |
| `PE-07` | Collision/spatial queries | Weapons, shields, movement, intel, reclaim, and selection all query spatial neighborhoods. Ordering matters for determinism. | One global spatial database; per-layer grids/trees; subsystem-specific indices; broad-phase plus exact shape tests. | Trace a radius-damage query and a movement collision query back to their index owners. Compare iteration/sort order. |
| `PE-08` | Aircraft | Retail aircraft take off, land, bank, use fuel, stage, and have role-specific attack runs. Blueprints provide air-motion parameters. | Full native flight state machine; generic steering plus script-controlled phases; mover class per aircraft family; hybrid mover and weapon-run controller. | Follow aircraft motion-type construction and state transitions. Anchors: fuel fields, `RULEUMT_Air`, staging methods, takeoff/landing animation names. |
| `PE-09` | Surface and submerged naval | Surface ships and submarines occupy different domains; sonar and water vision distinguish contacts; torpedoes use water behavior. | Discrete surface/sub layers; continuous depth with layer thresholds; mover subclasses with depth bands; script-controlled dive state. | Trace `RULEUMT_Water` and `RULEUMT_SurfacingSub` mover creation, layer-change callbacks, and torpedo target checks. |
| `PE-10` | Transports and attachments | Cargo uses attachment bones/clamps, capacity classes, load/unload, ferry routes, carrier death, and transport shields. | Generic parent-child attachment graph; transport-specific cargo slots; bone manipulators plus hidden cargo manager; hybrid. | Trace `AttachUnitTo`, `DetachFrom`, transport load commands, and attachment-bone lookup. Determine who owns position, collision, visibility, and death propagation. |
| `PE-11` | Economy allocation | Mass/energy flow is continuous; shortages slow work; storage caps and upkeep matter. The exact rounding and competing-demand order are native concerns. | Global proportional throttle; priority buckets then proportional allocation; sequential deterministic consumers; economy events with cached requested/actual values. | Trace one economy tick from income to requested drain and construction progress. Identify numeric type, rounding point, priority order, and stall callbacks. |
| `PE-12` | Construction state | Builders and factories contribute build power; unfinished units exist; assists add rate; upgrades replace units. | Progress stored on target unit; separate construction task object; builder-owned task with target backlink; hybrid factory queue plus target fraction. | Follow `SetWorkProgress`, build start/stop callbacks, and completion. Identify authoritative progress owner and cancellation/refund path. |
| `PE-13` | Repair/capture/reclaim | The same engineering rate influences several actions, but costs, legality, and completion differ. | One generalized work engine parameterized by action; separate native action state machines; common rate helper with separate passes. | Compare entry points and inner loops for repair, capture, reclaim, and build. Look for shared progress/cost functions versus duplicated state. |
| `PE-14` | Factory automation | Retail supports repeat, pause, rally chains, pre-queued upgrades, and factory assist/mirroring. | Persistent flags on factory; command-queue sentinel entries; UI-managed reissue; factory manager object. | Trace repeat/pause UI commands into sim state and serialization. Follow product completion to rally/factory-assist dispatch. |
| `PE-15` | Target acquisition | Blueprints specify target layers, categories, priorities, ranges, arcs, and weapon roles. Lua can influence targeting. | Per-weapon scans; per-unit candidate scan shared by weapons; centralized target manager; native broad phase with Lua priority hooks. | Trace `SetTargetingPriorities`, weapon acquire calls, and first hostile spatial query. Record deterministic tie-break order and retarget cadence. |
| `PE-16` | Projectile architecture | Retail has beams, ballistic shells, bombs, torpedoes, missiles, collisions, terrain impact, and script callbacks. | Native subclass per projectile family; one data-driven projectile with flags; Lua projectile state around native motion; hybrid. | Start from projectile blueprint/class creation and impact callback. Recover base layout, virtual methods, guidance fields, and collision query. |
| `PE-17` | Missiles, ammunition, interception | Tactical/strategic launchers build ammo; defenses build interceptors and select incoming missiles by class/range. | Ammo as hidden build queue/resource counter; ammo as child entities; projectile targetability through normal object IDs; dedicated interceptor manager. | Trace silo build command, ammo count UI getter, launch order, and defense acquisition. Anchors: nuke/tactical command strings, ammo methods, anti-missile categories. |
| `PE-18` | Damage and death | Damage profiles, armor, area falloff, death weapons, overkill, friendly fire, and callbacks interact. Ordering decides wreck value and chain reactions. | Shared damage gate for all sources; projectile-specific paths converging late; script-mediated damage modification; native armor lookup plus callbacks. | Trace `DamageArea` and direct projectile hit to health mutation. Record armor multiplication, shield interception, callbacks, death queue, and blast timing. |
| `PE-19` | Shields | Area, personal, transport, enhancement, and overlapping shields differ. Friendly shots pass outward; enemies can enter area bubbles. | Geometric projectile interception; damage-boundary containment test; shield entities in spatial index; hybrid projectile collision plus beam/damage gate. | Trace shield creation and one projectile crossing. Determine collision ownership, overlap selection, inside/outside rules, beam handling, energy-stall shutdown. |
| `PE-20` | Intel | Vision, water vision, radar, sonar, omni, stealth, cloak, fields, and jamming are range-based and alliance-specific. | Incremental stamped/refcount grids; periodic complete rebuild; per-query spatial tests; mixed grids for contacts plus direct vision tests. | Trace intel update cadence and one contact transition. Identify grid storage, stamp removal, remembered identity, jammer generation, and omni override. |
| `PE-21` | Enhancements | ACU/SCU upgrades occupy slots, consume resources/time, can replace prior upgrades, and mutate weapons/build categories/intel/economy. | Buff/capability overlay on immutable blueprint; per-unit cloned blueprint; component add/remove; script-owned state calling native setters. | Trace enhancement start/completion and one weapon-adding enhancement. Determine persistence owner, slot replacement, rollback, serialization, and UI query. |
| `PE-22` | Veterancy | Retail FA has five levels and changes health/regeneration rather than weapon damage. Threshold source and promotion ordering need confirmation. | Native kill counter and fixed table; blueprint-driven thresholds; Lua buff application; hybrid native event plus Lua buffs. | Trace kill credit into veteran-level mutation and health adjustment. Anchors: veteran strings, kill callbacks, blueprint veterancy fields. |
| `PE-23` | Wrecks/features | Wreck value depends on unit cost and state; wrecks can be reclaimed, damaged, obstruct movement, and sometimes support rebuild. | Wreck as unit-like object; feature subclass in shared object manager; lightweight prop record with separate collision; script-created entity. | Follow unit death through wreck creation and object registration. Record ID type, health/collision, value scaling, overkill, submerged modifier, and rebuild link. |
| `PE-24` | Terrain deformation | Explosions can change terrain appearance/height and must affect rendering, collision, and possibly pathing. | Visual decal only for most weapons; mutable heightfield patches; GPU-only deformation plus separate sim height; queued terrain operations. | Trace crater/deformation blueprint fields and impact path to heightmap/mesh writes and pathing invalidation. |
| `PE-25` | Match rules and caps | Retail offers assassination, supremacy, annihilation, sandbox, alliances, and configurable unit caps. | Rule strategy object; scenario Lua owns predicates; native enum branches; category counters plus callbacks. | Trace lobby option ingestion into defeat checks and build authorization. Find cap-cost accounting and transfer behavior. |
| `PE-26` | Replay and save | Retail replays commands in a deterministic simulation; save/resume may serialize more complete state. Exact formats and compatibility boundaries are unknown. | Command stream plus initial setup; periodic snapshots plus commands; full object serialization; script-driven save tables around native state. | Trace replay record write/read and save menu entry points. Identify header/version, command payloads, RNG state, script state, and object identity restoration. |
| `PE-27` | Lua and mod contract | Shipped Lua calls a broad native Moho API; hook files and archive precedence modify behavior. | Direct C functions registered per class; generated registration tables; generic dispatcher by method ID; hand-written wrappers over native virtual methods. | Recover all Lua registration arrays and map names to addresses before feature tracing. Compare retail registration set with FAF annotation stubs. |
| `PE-28` | AI/native boundary | Retail AI is Lua but depends on native threat, path, economy, platoon, and unit queries. | Native world-query service with Lua managers; native platoon objects; mostly Lua plans over native unit commands; hybrid. | Map brain/platoon method registrations, then trace threat and path query implementations and returned data ownership. |
| `PE-29` | Rendering and animation | Lua creates rotators/sliders/emitters; blueprints name meshes, bones, LODs, trails, and effects. Native render timing may differ from sim timing. | Scene graph manipulators attached to model bones; component animation system; render-thread proxies fed by sim objects; hybrid. | Trace one `CreateRotator` registration through object allocation and frame update. Keep visual fidelity lower priority than gameplay semantics. |
| `PE-30` | Audio and music | Blueprints and Lua name cues; retail has dynamic music and event priorities. | Native cue/event graph; Lua-triggered named playback; state-driven music manager; middleware bank playback. | Trace one `PlaySound` method and music-state transition. Identify bank lookup, event priority, concurrency, and listener ownership. |

## Work-package dashboard

This table is the canonical inventory. Update the row in the same session that evidence changes.
Do not change readiness merely because analysis began.

| ID | Area | Readiness | Confidence before EXE | EXE evidence | Envelope | Priority | Current basis / main uncertainty |
|---|---|---|---|---|---|---|---|
| `WP-00` | Artifact provenance and version identity | Not ready | High | Anchored | n/a | P0 | Steam app `9420`; executable version `1.5.0.1`, complete `bin` set, and all SCDs are hashed. `C-025` proves the corpus is Forged Alliance. Original Steam depot/build manifest and transfer history remain unknown. |
| `WP-01` | PE architecture, sections, imports, RTTI, symbols, global map | **Ready but not confirmed** | High | Analyzed | `PE-02` | P0 | `C-001`–`C-003`: statically linked, no engine DLL loaded, full RTTI recovered, Ghidra project analyzed. **Session 13 closed the address-ledger gap that kept this Not ready.** `C-117`: 4,049 vtables / 32,678 slots, validated 217/217 against exported `??_7` symbols, surfacing **4,514 function starts Ghidra never found**. `C-128`: 484 field offsets across 51 classes with the base pointer read from each receiver's type descriptor. `C-122`: 525 enum (name, value) pairs. `C-114`: 72 source paths (a measured ceiling, not a partial result). Globals remain unsurveyed — that is what stops it being Confirmed. |
| `WP-02` | Moho/Lua native registration map | **Ready but not confirmed** | High | **Analyzed** | `PE-27` | P0 | `C-032`: the complete map is recovered — 1,182 Lua callables with name, signature, documentation and native wrapper address, cross-validated by `C-033` against shipped Lua and DLL exports. Regenerate with `tools/re/extract_moho_methods.py`. Readiness is "ready" in this package's own terms (the map is complete enough to drive downstream analysis) but not confirmed: no individual wrapper has yet been decompiled and checked against its signature. |
| `WP-03` | Native object identity, ownership, lifecycle, destruction | Not ready | High | **Analyzed** | `PE-02`, `PE-03` | P0 | **Answered** by `C-044`–`C-047`. Sim identity is a pooled integer id via `EntityDB`/`IdPool` with deferred destruction (`C-004`); the Lua boundary resolves its receiver fresh per call from `_c_object`, whose native pointer is nulled in `~CScriptObject` (`0x004cdeab`, 91 callers) and checked by 73 per-class resolvers — 54 raising `"Game object has been destroyed"`, 19 returning NULL for lifecycle queries. `Entity+0x1b9` flags queued destruction so scripts see death *before* the pointer dies. Readiness stays Not ready only because Recoil Metal has no equivalent script boundary to test. |
| `WP-04` | Simulation tick, phase order, RNG, determinism | Ready but not confirmed | High | **Analyzed** | `PE-01` | P0 | `C-005`, `C-006`: beats with named stages, per-entity `MotionTick`/`TaskTick`, Mersenne-Twister RNG behind `Sim::GetRandom`, pervasive `UpdateChecksum`. **`C-142` answers the stage-order question: the per-tick order is CommandDispatch → Script → Motion — the REVERSE of declaration order**, read from the tick driver at `0x007513a3` and verified by confirming `0x00409ac0` is `CTaskStage::Tick` rather than assuming it. So a command issued on a tick reaches motion within the *same* tick. The MT variant and its seeding remain the open question. |
| `WP-05` | VFS, SCD mounting, override precedence | Ready but not confirmed | High | Anchored | `PE-27` | P2 | `Moho::CVFSImpl`/`CVirtualFileSystem` located in RTTI; `.scd` confirmed to be plain ZIP. Exact retail mount precedence still needs tracing. |
| `WP-06` | Blueprint loading, merge/default rules, categories | Ready but not confirmed | High | Bounded | `PE-27` | P2 | `C-010`: blueprints deserialise into native per-section structs; defaults and merge order live in `RRuleGameRulesImpl::InitBlueprint`, not in Lua. |
| `WP-07` | Retail Lua dialect, scheduler, callbacks, script objects | Not ready | High | Bounded | `PE-01`, `PE-27` | P2 | `C-022`, `C-023`: native `CTaskThread`/`CTaskStage`/`CLuaTask` scheduler behind `ForkThread`/`WaitTicks`; `#` line comments and ISO-8859-1/CRLF source. Wake ordering is an engine property and is untraced. |
| `WP-08` | Mod manager, hooks, UI/sim mod boundaries | Not ready | Medium | Unexamined | `PE-27` | P3 | Manual and mod conventions bound capabilities; exact retail load and sandbox rules unknown. |
| `WP-09` | Map/scenario loading and start markers | Ready but not confirmed | High | Anchored | `PE-27` | P2 | `C-019`: `STIMap` over `CHeightField` with five distinct elevation queries and a playable rect. Scenario-rule integration is still incomplete. |
| `WP-10` | Skirmish setup, four factions, armies/alliances | Ready but not confirmed | High | Anchored | `PE-25` | P2 | `Moho::CLobby`, `SimArmy`, `CArmyImpl`, `IArmy`, `CArmyStats` located; `Sim::CreateArmies`/`GetArmies`/`GetFocusArmy`. Retail lobby option translation is not confirmed. |
| `WP-11` | Victory modes, sandbox, unit caps, rule state | Not ready | High | **Bounded** | `PE-25` | P2 | `C-009`: `RRuleGameRules` is the *blueprint/category* database, not victory logic. **`C-126` settles the executable half: there is NO native victory predicate — Lua decides and pushes the verdict in**, per the engine's own doc strings (`SetArmyOutOfGame` `0x007115e0` *"indicate that the supplied army has been defeated"*, `EndGame` `0x00752bf0`, `IsGameOver` `0x00752cc0`, `GetArmyScore` `0x008499a0`, `SetAlliedVictory` `0x00710320`). This is now a **Tier-0 scenario-Lua question**, not an executable one. |
| `WP-12` | Command authorization, queues, cancellation, patrol | Ready but not confirmed | High | **Analyzed** | `PE-14`, `PE-25` | P1 | `C-096`, `C-097`: one and only one site advances the head (`TaskTick`), in the command-dispatch stage; patrol rotates only while another command follows; factory repeat-build restores the count and rotates; the 40-entry `EUnitCommandType` enum is recovered. Lua edits interrupt sub-tasks but never rotate. |
| `WP-13` | Persistent fire state, toggles, priorities, mutable capabilities | Not ready | High | **Analyzed** | `PE-14`, `PE-21` | P1 | **The storage is now fully legible.** `C-123`: `Unit+0x4A0` is a 64-bit mask whose **45 bits are all named**, bit index == enum value, proven by reading `IsUnitState` (`0x006ab580`) and verifying `__allshl` so states ≥32 reach the high dword at `+0x4A4`. `C-124` adds `FIRESTATE_ (0 ReturnFire, 1 HoldFire, 2 HoldGround, 255 Mix)` and the `RULEUTC_` 9-toggle mask. `C-128`/`C-131` add `Unit+0x498` script-bit mask, `+0x2A0` paused, `+0x2A8` fire state, `+0x29C` stunned, `+0x2A1` valid target. Readiness stays Not ready because Recoil Metal implements almost none of these states. |
| `WP-14` | Factory repeat, pause, rally, queue editing, mirroring | Not ready | High | Anchored | `PE-14` | P1 | Sim ownership confirmed: `Unit::IsRepeatQueue`/`SetRepeatQueue`, `IsPaused`/`SetPaused`, `CUnitCommand::GetRally`/`FactoryRepeatable`/`AlwaysRepeatable`/`GetIsFactoryOrder`, `Sim::IssueFactoryCommand`, `CFactoryBuildTask`. Serialization detail still open. |
| `WP-15` | Mass/energy income, storage, upkeep, stalls, allocation | Ready but not confirmed | High | **Analyzed** | `PE-11` | P1 | **The decisive native path is traced end to end (`C-159`–`C-163`): allocator `0x007790e0`, called per army per beat, two ratios over two buckets keyed on nonzero OUTSTANDING, energy winning `k*` ties, FIFO request order.** Rounding, staleness, clamp order and ally sharing are all read. **This package satisfies the EXE half of the confirmation gate in full and is the first to do so.** It stays `Ready but not confirmed` on the READINESS axis because our implementation demonstrably differs — `C-103` and `C-104`(a)–(e) are open, and `C-160` shows bit-exact parity is structurally impossible for fixed point. **To reach `Confirmed`, fix in this order: upkeep becomes a request line (`C-161`), the single throttle becomes two bucketed ratios (`C-159`), clamp after allocation (`C-163`), reproduce the one-beat ratio cache (`C-162`).** |
| `WP-16` | Construction, upgrades, assist, build progress | Ready but not confirmed | High | **Analyzed** | `PE-12`, `PE-13` | P1 | `C-098`–`C-102`: progress on the target at `Entity+0xd8` with the builder's `Unit+0x2ac` a mirror; `delta = BuildRate/BuildTime × economyRatio × 0.1f` in float32 with no accumulator; the ratio is **one beat stale**; assist clamps per contribution so order matters at completion; `UpdateFractionComplete` floors on health only when increasing. |
| `WP-17` | Explicit repair, guard, capture, gifting, ownership transfer | Not ready | High | Bounded | `PE-13` | P1 | `C-008`: `CUnitRepairTask`, `CUnitCaptureTask`, `CUnitReclaimTask`, `CUnitGuardTask` are **separate native state machines**. Ownership transfer is `Sim::TransferUnit`; capture refcounts via `Unit::IncCaptors`/`DecCaptors`/`GetCaptors`. |
| `WP-18` | Wreck creation, damage, collision, rebuild, reclaim value | Not ready | Medium | **Analyzed** | `PE-18`, `PE-23` | P2 | **Answered by `C-146`–`C-148`.** Reclaim values are Lua table fields (which is why no native sweep could find them), scaled by `GetFractionComplete()` on mass and energy but never on time or the `Max` baseline. The reclaim rate is `wreckMaxHealth / max(mtime, etime)` ticks and **the wreck's health IS the progress bar** (`0x00625ad0`). Rebuild matches `prop.AssociatedBP` against `RebuildBonusIds` on the nearest `RECLAIMABLE` prop, and the bonus is a **build-progress head start** (hardcoded `0.5`), not a mass refund. Two shipped quirks must be reproduced, not fixed — see `C-146`. **Open: the mass/energy credit itself was not located**; next addresses recorded in `C-147`. |
| `WP-19` | Adjacency geometry and economy effects | Ready but not confirmed | High | Anchored | `PE-11` | P2 | **Fully specified** by `C-048`–`C-052` from shipped Lua and a blueprint census: counting stacks, `AdjMod = 1 + Σ(Add×Count)`, three participation gates, per-size magnitudes with a constant-product invariant, three retail defects, and the lifecycle. Only the *geometry test* is engine-side (`Moho::UI_DetectAdjacencyBonus` is UI-only); skirt rects come from `Unit:GetSkirtRect` and touch edge-to-edge with no tolerance. Ready to verify Recoil Metal against — ten regression tests are specified in the session-09 record. |
| `WP-20` | Ground movement, motion classes, ordinary pathing | Ready but not confirmed | High | Bounded | `PE-04`, `PE-06` | P1 | `C-012`, `C-013`: shared `PathQueue` request service over precomputed `PathTables`, spline routes, one `CUnitMotion` mover, and a real occupancy grid (`COGrid`, `OCCUPY_*`, `ReserveOgridRect`). Recoil Metal's per-unit A* is a structural difference. |
| `WP-21` | Formations, coordinated movement, congestion, dynamic blockage | Not ready | Medium | Bounded | `PE-05`, `PE-06` | P1 | `C-015`: formation generated on the command object at issue, persistent `CFormationInstance`, Lua formation scripts via `FORMATION_RunScript`, explicit multi-unit coordination handshake. Replanning cadence still unknown. |
| `WP-22` | Aircraft flight, bombing runs, fuel, staging, landing | Not ready | Medium | **Bounded→Anchored** | `PE-08` | P1 | `C-013`: one mover with `CalcMoveAir`/`CalcWingedLift`/`ComputeAirControl`/`ComputeAirCombatTactics`, fuel in `ProcessFuelLevels`/`GetFuelUseTime`/`Unit::GetFuelRatio`, staging via `CUnitCallAirStagingPlatform`/`CUnitRefuel`/`CUnitCarrier*`. **`C-125` adds the four motion-event enums with their field offsets** — `EUnitMotionHorzEvent` at `CUnitMotion+0x7C`, `Vert` at `+0x80`, `Turn` at `+0x84`, `EUnitMotionState` at `+0x78` — corroborated against shipped `Unit.lua`, plus the finding that `OnMotionStateChange` is emitted and **never handled in Lua**. `C-128` adds `Unit+0x294` fuel ratio and `+0x4B0` fuel-use object. **`C-134` settles the two missing enums: `EAirCombatState` and `EUnitMotionCarrierEvent` have NO value names anywhere in the binary** — they are serialization-reflected only — so those must be read from code, not looked up. Three fuel callbacks recovered incidentally at `0x00e7a5a4`–`0x00e7a5c4`: `OnStartRefueling`, `OnGotFuel`, `OnRunOutOfFuel`. |
| `WP-23` | Surface naval movement and ordinary combat | Ready but not confirmed | High | Bounded | `PE-09` | P2 | `C-013`, `C-019`: `CalcMoveWater`, discrete layers, and separate water/surface/deep/abyss elevations. `Moho::Shoreline`/`ShoreCell`/`WaveSystem` cover the shore boundary. |
| `WP-24` | Submarines, depth, surfacing, torpedoes, water vision | Not ready | Medium | Bounded | `PE-09`, `PE-16` | P1 | `C-013`: diving is a **layer transition** (`HandleDivingAndSurfacing`, `SetNewTargetLayer`, `TransitionBetweenLayers`), not a depth scalar; `Unit::SetAutoSurfaceMode`/`IsAutoSurfaceMode`. Continuous-depth candidate refuted. |
| `WP-25` | Transports, cargo, attachments, ferry, staging reuse | Not ready | Medium | Bounded | `PE-10` | P1 | `C-016`: generic `Entity` attachment graph plus `CAiTransportImpl`; ferry via `CUnitFerryTask`/`CUnitWaitForFerryTask`/`GetTransportFerryBeacon`. Death propagation is callback-ordered. |
| `WP-26` | Spatial index, collision layers, query ordering | Ready but not confirmed | Low | **Analyzed** | `PE-07` | P1 | **Closed by `C-137`–`C-141`: the container is `Moho::CollisionDB<Moho::Entity>` at `COGrid+0x04`** — a flat uniform grid of 4-unit cells with three intrusive head-inserted bucket lists, not a tree. Query order is **z ascending → x ascending → bucket A → B → C → chain head to tail**, first-touch dedup via a transient per-record flag, and **nothing sorts**; the ray query deliberately differs (C then A, skips props). Order is history-dependent but stable under removal, and relinking happens only when the quantised cell rect changes. The old basis for this row — *"`COGrid` is occupancy, not broad phase; the structure is unidentified"* — was **half right and misleading**: `COGrid` holds both, the broad phase at `+0x04` and the occupancy bitmaps at `+0x38`/`+0x48`/`+0x58`. |
| `WP-27` | Weapon target acquisition, priorities, arcs, retargeting | Ready but not confirmed | High | **Analyzed** | `PE-15` | P1 | `C-090`–`C-095` plus **`C-156`/`C-157`, which close the last two questions**. An **empty priority table acquires NOTHING** and no default seed exists — verified against all 494 weapons in the corpus, whose 157 exceptions are every one on a separate path. The full 14-step pre-scoring reject order is recovered, with `IsTargetExempt` at step 11, **before** range/arc classification; its mystery constants are `UNITCOMMAND_Reclaim` and `UNITCOMMAND_Capture`, i.e. *do not shoot what my own side is reclaiming or capturing*. Still open: the contents of the list `0x006de340` walks (next address `0x006de370`), and a latched bit at `0x005de98e` that gates whether priorities apply at all. |
| `WP-28` | Direct fire, beams, ballistic projectiles, impact | Ready but not confirmed | High | Bounded | `PE-16`, `PE-18` | P1 | `C-021`: one `Moho::Projectile` class configured by ~20 setters (tracking, ballistic acceleration, lifetime, stay-underwater, velocity-align) with a single `Impact` and `CheckCollision`; beams are a separate `CollisionBeamEntity`. Per-family subclassing is refuted. |
| `WP-29` | Tactical/strategic missiles, ammo, interception | Not ready | High | **Analyzed** | `PE-17` | P1 | `C-081`–`C-088`: ammo is an `int` on `CAiSiloBuildImpl` at `Unit+0x558` with build progress as separate state; production runs on `CEconomyEvent` and **slows rather than stalls** under scarcity (`C-084`); launch is decrement-then-fire; **interception is the ordinary collision path** gated by Lua `OnCollisionCheck`, and it **damages** rather than force-destroys — nukes have 25 HP. Three distinct counter-missile mechanisms exist, including Aeon flares that divert without damaging. |
| `WP-30` | Damage, armor, area falloff, death blasts, friendly fire | Ready but not confirmed | High | **Anchored→Bounded** | `PE-18` | P1 | `C-021` plus addresses: the four scripted entry points are `Damage 0x0073f6d0` (arity 5, `C-040`), `DamageArea 0x0073fa40` (6–7), `DamageRing 0x0073fdc0`, `MetaImpact 0x007401e0`; `Entity::AdjustHealth 0x00693bb0`, `Entity::Kill 0x006987a0`, `Unit::GetArmorMult 0x006cac80`, `Unit::AlterArmor 0x006caa80`. `C-038`: health is a **float at `Entity+0x98`**, so exact damage totals cannot match Recoil Metal's fixed point. Next: follow these four into the shared gate. |
| `WP-31` | Ordinary area shields | Ready but not confirmed | Medium | **Analyzed** | `PE-19` | P1 | **`C-143`/`C-144`/`C-145`.** Absorption is per target against the target's own position, all covering bubbles stack, and each is charged once — **confirming the `C-110` rewrite**. `Moho::Shield` adds **zero data members** over `Entity`: strength lives in `Entity+0x90`/`+0x94`, the same fields as hull health, and everything else is a Lua table field. Regen and recharge are Lua, in seconds; recharge blocks absorption by `SetCollisionShape('None')`. **Three open divergences (`C-143` a–c)**: a blast centred inside a dome is not absorbed by it; every admitted shield is charged whether or not it sheltered anyone; and coverage is a shape containment test, so personal shields are boxes. |
| `WP-32` | Personal, transport, enhancement, overlapping shield variants | Not ready | Low | Anchored | `PE-19`, `PE-21` | P2 | `C-020` plus `RUnitBlueprintDefenseShield`; `ART-S007` `lua/shield.lua` defines `Shield`, `UnitShield`, `AntiArtilleryShield`. Overlap selection rules untraced. |
| `WP-33` | Vision, radar, sonar, omni, cloak, stealth, jamming | Ready but not confirmed | High | **Analyzed** | `PE-20` | P1 | `C-076`–`C-079`: nine signed-byte **coverage** grids (vision scale 2, the rest scale 4, explored a real bitmask), filled-circle rasterisation, **removal is immediate — the deferred path is dead code**, recon staggered one army per beat, and **stale contacts are retained** with a Lua-visible seen-now / seen-ever / maybe-dead identity. Recoil Metal models no remembered contacts. |
| `WP-34` | ACU/SCU enhancements and mutable abilities | Not ready | Low | Unexamined | `PE-21` | P2 | No native enhancement symbol found in RTTI or exports; `Sim::QueueNotifyUpgrade`/`SNotifyUpgradeParams` and `Unit::SetUpgradedTo` are the only hooks. Likely Lua-owned — verify against `ART-S007` before EXE work. |
| `WP-35` | Veterancy, kill credit, promotion, regeneration | Ready but not confirmed | High | Unexamined | `PE-22` | P1 | **Implemented** in session 03 against `C-024`, `C-028`, `C-029`: `src/core/sim/Veterancy.{hpp,cpp}`, kill credit from `retireDead`, hull regeneration in the tick, covered by `tests/test_veterancy.cpp`. Readiness stops at "not confirmed" because the native `KILLS` store and `OnKilledUnit` ordering have not been traced in the executable, and blueprint `Veteran` threshold overrides are not implemented. |
| `WP-36` | Experimentals and unit-specific special abilities | Not ready | Low | Unexamined | `PE-10`, `PE-16`, `PE-21` | P3 | Generic units can load; bespoke script/native interactions vary by unit and remain largely unknown. |
| `WP-37` | Mutable terrain, craters, path/render invalidation | Not ready | Medium | Anchored | `PE-24` | P3 | `C-019`: `Sim::FlattenMapRect` and `STIMap::SetTerrainType`/`SetTerrainTypeRect` prove the **sim-side** map is mutable, so deformation is not purely visual. Crater path is untraced. |
| `WP-38` | Retail AI native boundary, threat, platoons, managers | Not ready | Medium | Anchored | `PE-28` | P3 | Native threat extraction found: `IntelExtractor`, `RadarExtractor`, `SonarExtractor`, `OmniExtractor`, `RangeExtractor`, `WeaponExtractor`, `CounterIntelExtractor`, `CountermeasureExtractor`, `CombinedMilitaryExtractor`, `MiscellaneousExtractor`, over `CInfluenceMap`, with `CAiBrain`, `CPlatoon`, `CAiPersonality`. |
| `WP-39` | Core selection, camera, minimap, build/command UI | Ready but not confirmed | High | Unexamined | `PE-25` | P3 | Recoil Metal has a usable native HUD; this is functional readiness, not pixel or retail UI parity. |
| `WP-40` | Advanced controls, overlays, split views, key behavior | Not ready | High | Anchored | `PE-05`, `PE-14` | P3 | `Moho::CUIKeyHandler`, `UICommandModeData`, `UIBuildDragger`, `UICommandDragger`, `SelectionDragger2D`/`3D`, `CUIWorldView`, `IdleUnitSelector`, `RDebugOverlay*` located. Command encoding untraced. |
| `WP-41` | Rendering, model graph, manipulators, effects, LOD | Not ready | Medium | Anchored | `PE-29` | P3 | Manipulator classes enumerated (`CAnimationManipulator`, `CAimManipulator`, `CBoneEntityManipulator`, `CBuilderArmManipulator`, `CCollisionManipulator`, `CFootPlantManipulator`, `CRotateManipulator`, `CSlaveManipulator`, `CSlideManipulator`, `CStorageManipulator`, `CThrustManipulator`) over `IAniManipulator`, plus `CAniActor`/`CAniPose`/`CAniSkel` and `MeshLOD`. |
| `WP-42` | Audio cues, voice priority, dynamic music | Not ready | Medium | Anchored | `PE-30` | P3 | `Moho::CUserSoundManager`, `ISoundManager`, `CSimSoundManager`, `HSound`, `CSndParams`, `CSndVar`, `SAudioRequest`, `RWaveBankResource`/`ResInMemory`/`ResStreaming` located. Event/music logic untraced. |
| `WP-43` | Command replay, state hash equivalence, divergence | Ready but not confirmed | High | **Analyzed** | `PE-01`, `PE-26` | P2 | **Answered by `C-150`–`C-155`.** A 24-opcode message stream with `Moho::Sim` as the `ICommandSink`; one command is a 14-field value with **two** targets, a formation quaternion and a cell list; beats gate on `mAvailableBeat` (everyone acked), not on the queued counters; command ids are `[source:8][counter:24]`, unique among *live* commands and not monotonic; and application order is **ascending client index → FIFO within client → one AdvanceBeat**, with units inside one command visited in **ascending entity id**. **`C-154` is the sting: retail's checksum is a bounded 128-beat change-set ring, not a full-state hash, so digest parity is not a usable oracle for us** — a perfectly matching sim would still disagree. |
| `WP-44` | Save/resume and full simulation serialization | Not ready | Medium | Anchored | `PE-26` | P3 | `Moho::CSavedGame`, `CSaveGameRequestImpl`, `ISaveRequest`, `Sim::SaveState`/`SerArmies`/`SerDirtyEnts`/`SerMapData`/`SerVars`, `EntityDB::SerEntities`/`SerSets`, and a per-class `MemberSaveConstructArgs` convention. Full object serialization, not a command replay. |

## Analysis order

The campaign starts with the least-known behavior, but executable mapping prerequisites come first.
Do not skip Phase 0 to chase an interesting feature: every later session becomes cheaper when the
registration and object maps are stable.

> **Revised after session 06.** Phases 0–4 below remain the right *subject* ordering, but the
> measured reachability in `Analysis strategy` changes the *method* ordering. Concretely, the
> next four moves, in this order:
>
> 1. ~~Bulk-transfer names from `ART-D001`.~~ **Done in session 07, and it is not the lever it
>    looked like** — 98 candidate names, not thousands. See strategy §4 for the measured
>    result. It stays in the toolbox as a frontier tool, and it did hand us
>    `SIM_MetaImpactArea` (`C-042`), which unblocks `WP-30`.
> 2. **Mine field offsets for `Moho::Unit` and `Moho::Entity`** from the depth-1 frontier.
>    Object layout is what parity questions actually need — `C-038` is the template.
> 3. **Take one subsystem to `Confirmed`** to prove the gate is passable at all. `economy`
>    is the right pick: 51 depth-1 functions, a `Ready but not confirmed` implementation to
>    compare against, and `PE-11`'s open question (rounding point and priority order) is
>    exactly a "one-line answer" question.
> 4. **Then `WP-30` damage**, which is higher value but four times the frontier.

### Phase 0: make the binary searchable

1. `WP-00` artifact provenance and exact version.
2. `WP-01` PE/compiler/import/RTTI/string/global survey.
3. `WP-02` complete Lua/Moho registration address map.
4. `WP-03` one unit userdata from Lua receiver to native object and destruction.
5. `WP-04` central simulation tick and phase call graph.

### Phase 1: least-known shared native foundations

1. `WP-26` spatial query and collision ownership.
2. `WP-20` path request and route representation.
3. `WP-21` formation assignment and dynamic blockage.
4. `WP-25` attachment/containment graph.
5. `WP-22` aircraft mover state machine.
6. `WP-24` submerged movement and target layers.
7. `WP-27` target acquisition and deterministic tie breaks.
8. `WP-28` projectile base architecture.
9. `WP-29` guidance, ammunition, and interception.

### Phase 2: mutable gameplay state

1. `WP-13` persistent controls and mutable capabilities.
2. `WP-34` enhancements.
3. `WP-35` veterancy.
4. `WP-16` construction state owner.
5. `WP-17` repair/capture/reclaim relationship.
6. `WP-14` factory automation.
7. `WP-18` features and wreck lifecycle.
8. `WP-11` match rules and unit caps.

### Phase 3: confirm currently ready systems

1. `WP-15` economy precision and allocation.
2. `WP-30` damage and death ordering.
3. `WP-31` ordinary shield interception.
4. `WP-33` intel storage and update cadence.
5. `WP-19` adjacency stacking.
6. `WP-12` queue semantics.
7. `WP-43` replay ordering and RNG state.

### Phase 4: compatibility and presentation

1. `WP-07`, `WP-08` retail scripting and mod contract.
2. `WP-38` retail AI/native boundary.
3. `WP-40` advanced UI and command encoding.
4. `WP-41` rendering/manipulators/effects.
5. `WP-42` audio and music.
6. `WP-44` save/resume.

## Artifact manifest

No analysis result is valid without an exact artifact identity. Fill this before Ghidra import.

Inventory date: 2026-08-29. The connected volume label is `Samsung_T5`. `steam_appid.txt` and
`installscript.vdf` associate the installation with Steam app `9420`; the executable resource names
the product *Supreme Commander Forged Alliance*. The files were already present on the external
disk with filesystem modification times of 2025-06-28. The original Steam download date, depot
manifest/build ID, and method used to transfer the installation to this disk are not currently
known. Do not infer them from those filesystem timestamps.

Path abbreviations used below:

- `SOURCE_ROOT`: `/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance`
- `PRESERVED_ROOT`: `~/projects/llm/input/recoil-metal/retail-fa`

| Artifact ID | Source path | Preserved path | Size | SHA-256 | Version / PE timestamp | Notes |
|---|---|---|---:|---|---|---|
| `ART-E001` | `$SOURCE_ROOT/bin/SupremeCommander.exe` | `$PRESERVED_ROOT/bin/SupremeCommander.exe` | 13,213,696 | `c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0` | File/product `1.5.0.1`; PE `2011-08-29 23:48:30 +0200` | Main game executable in this installation; PE32 x86 GUI, linker-version field 8.0, relocations stripped, large-address aware. |
| `ART-E002` | `$SOURCE_ROOT/bin/BsSndRpt.exe` | `$PRESERVED_ROOT/bin/BsSndRpt.exe` | 180,224 | `f914ca869e52fc4a57665ef21a17ab482d47c5aa32baca8aa72db94b45e209b0` | `3.1.0.1`; PE `2006-04-19 02:05:41 +0200` | BugSplat crash-report helper, not a game-engine analysis target. |
| `ART-D001` | `$SOURCE_ROOT/bin/MohoEngine.dll` | `$PRESERVED_ROOT/bin/MohoEngine.dll` | 9,827,584 | `3e6e1a698a57d051e8ee6120d81d4af373321b9bf5f10071e291c96c08a7c3b5` | PE `2007-07-09 21:01:12 +0200` | Engine DLL; PE32 x86. |
| `ART-D002` | `$SOURCE_ROOT/bin/LuaPlus_1081.dll` | `$PRESERVED_ROOT/bin/LuaPlus_1081.dll` | 365,824 | `54167a0d72df75a06109879a9e3eb95bd5e83d1e07dd7c0d6156f05ca0835e28` | `1.0.0.1`; PE `2007-07-09 20:46:42 +0200` | LuaPlus runtime. |
| `ART-D003` | `$SOURCE_ROOT/bin/gpgcore.dll` | `$PRESERVED_ROOT/bin/gpgcore.dll` | 533,760 | `6d0b7cc22711bf86f223fb8225458fc0aa1e99147efcc79f955ffc181f660699` | `1.0.0.1`; PE `2007-07-09 20:46:41 +0200` | Gas Powered Games core runtime. |
| `ART-D004` | `$SOURCE_ROOT/bin/gpggal.dll` | `$PRESERVED_ROOT/bin/gpggal.dll` | 1,246,464 | `c9b7d90199e963a9d7683f74cd50dd6977d6cf193fe172c3a792b6d7cf1754a3` | `1.0.0.1`; PE `2007-07-09 20:46:47 +0200` | Gas Powered Games abstraction layer. |
| `ART-D005` | `$SOURCE_ROOT/bin/GDFBinary.dll` | `$PRESERVED_ROOT/bin/GDFBinary.dll` | 390,400 | `f9c5d50ae7117749d7c2a3585518688ada4108535e5548d93357f54b7a52deb0` | `1.0.0.1`; PE `2007-10-12 20:45:30 +0200` | Game Definition File support. |
| `ART-D006` | `$SOURCE_ROOT/bin/steam_api.dll` | `$PRESERVED_ROOT/bin/steam_api.dll` | 121,984 | `59ed854645eaa237463eb22f3c5a25f726d7cb2f29440f00a1ec0a4d73d0207a` | PE `2010-01-27 21:59:55 +0100` | Steamworks runtime. |
| `ART-D007` | `$SOURCE_ROOT/bin/zlibwapi.dll` | `$PRESERVED_ROOT/bin/zlibwapi.dll` | 72,704 | `0d38360003865e84a2842c337d7c440c8ab4c41809cc87b8758df6d852c02afc` | PE `2004-10-07 12:50:49 +0200` | zlib runtime. |
| `ART-D008` | `$SOURCE_ROOT/bin/wxmsw24u-vs80.dll` | `$PRESERVED_ROOT/bin/wxmsw24u-vs80.dll` | 3,575,808 | `665b9a94cb3aa6a4bd7009bece6370e4673f8c695678af2269c5d36eda2b18c1` | PE `2007-06-29 01:48:01 +0200` | wxWidgets runtime. |
| `ART-D009` | `$SOURCE_ROOT/bin/sx32w.dll` | `$PRESERVED_ROOT/bin/sx32w.dll` | 100,864 | `da3fb6c95af39cda6a67a6024bb70bf0d19bcf271b055e0c468cd8426b1bff13` | PE `2003-02-26 08:43:43 +0100` | Third-party runtime. |
| `ART-D010` | `$SOURCE_ROOT/bin/SHW32d.DLL` | `$PRESERVED_ROOT/bin/SHW32d.DLL` | 283,648 | `757458f9c1fb0a1842d98b396c43c065d1811594407d1c42f0e44c31d2b36cf1` | PE `2006-03-27 21:35:03 +0200` | Third-party runtime. |
| `ART-D011` | `$SOURCE_ROOT/bin/SHSMP.DLL` | `$PRESERVED_ROOT/bin/SHSMP.DLL` | 115,200 | `82b9c2dcf5bbe21522b4d0b841de0bd3ac90c8d96e400bde24aa1353244a76ff` | PE `2006-03-27 19:50:53 +0200` | Third-party runtime. |
| `ART-D012` | `$SOURCE_ROOT/bin/msvcr80.dll` | `$PRESERVED_ROOT/bin/msvcr80.dll` | 626,688 | `120cd25f5d6002ffd9069cf9550bc16c682bcd3323053b95146e7cd3ba2215ac` | PE `2005-09-23 08:44:37 +0200` | Microsoft Visual C runtime. |
| `ART-D013` | `$SOURCE_ROOT/bin/msvcp80.dll` | `$PRESERVED_ROOT/bin/msvcp80.dll` | 548,864 | `9fc2e85ba84cf0459aab0dc2efac734ad7b5b4c99ba19871fe8f6e35d0191838` | PE `2005-09-23 08:46:56 +0200` | Microsoft Visual C++ runtime. |
| `ART-D014` | `$SOURCE_ROOT/bin/msvcm80.dll` | `$PRESERVED_ROOT/bin/msvcm80.dll` | 479,232 | `c75e2f91a7b2032d3757eeac12502112381e0cb6f0e6e308adc74ac30c8a7ec7` | PE `2005-09-23 08:46:13 +0200` | Microsoft managed C++ runtime. |
| `ART-D015` | `$SOURCE_ROOT/bin/DbgHelp.dll` | `$PRESERVED_ROOT/bin/DbgHelp.dll` | 986,112 | `cf2647be9233f4a7248514cbd2541d5f7bebd61005bde1dca79c8e4234f53794` | PE `2005-01-12 20:23:59 +0100` | Microsoft debugging runtime. |
| `ART-D016` | `$SOURCE_ROOT/bin/d3dx9_31.dll` | `$PRESERVED_ROOT/bin/d3dx9_31.dll` | 2,413,064 | `010473c709db48fb72e3ea3af174ee023a9b291463e43bf7c8a9172a043594e5` | PE `2006-09-29 00:13:05 +0200` | DirectX helper runtime; `ART-E001` also imports system `d3dx9_35.dll`, which is not shipped in `bin`. |
| `ART-D017` | `$SOURCE_ROOT/bin/BugSplat.dll` | `$PRESERVED_ROOT/bin/BugSplat.dll` | 106,555 | `ad79278f441fb4c9cb4ffeb0fb2a196e8e9fdaa44a9e831302d7eecd0d633cd8` | PE `2006-04-19 02:05:12 +0200` | Crash-report runtime. |
| `ART-D018` | `$SOURCE_ROOT/bin/BugSplatRc.dll` | `$PRESERVED_ROOT/bin/BugSplatRc.dll` | 65,597 | `be777e26779d780de20cd92fa4779fa503cf7d8a09ed4e2f5782b6bb2076ea53` | PE `2006-04-19 02:04:57 +0200` | Crash-report resources. |
| `ART-S001` | `$SOURCE_ROOT/gamedata/units.scd` | `$PRESERVED_ROOT/gamedata/units.scd` | 1,114,843,505 | `c23a48d6b47043144baafc4c7f47b5a470392264f1bbaf962896923d58899c26` | n/a | Units and blueprints archive. |
| `ART-S002` | `$SOURCE_ROOT/gamedata/textures.scd` | `$PRESERVED_ROOT/gamedata/textures.scd` | 530,031,023 | `5f48e3e283759cb71b65e6501063d0d4bd586fe67fc1cfa27672250d1a4f8ffd` | n/a | Texture archive. |
| `ART-S003` | `$SOURCE_ROOT/gamedata/env.scd` | `$PRESERVED_ROOT/gamedata/env.scd` | 1,355,035,520 | `30110a7314fadcfbd05b69c626d87dfd5cd23f309cfe19490421828809e95ac7` | n/a | Environment archive. |
| `ART-S004` | `$SOURCE_ROOT/gamedata/editor.scd` | `$PRESERVED_ROOT/gamedata/editor.scd` | 57,563,657 | `bd9d4866b78194cafe87562927b765a3557ec3a4c2a20567bd5bf9d5a9abbd9d` | n/a | Editor archive. |
| `ART-S005` | `$SOURCE_ROOT/gamedata/effects.scd` | `$PRESERVED_ROOT/gamedata/effects.scd` | 27,224,110 | `f0dcbf76b2c3381e7d9ebbd7e09613e046875f6ba3a788406e5635768e28a300` | n/a | Effects archive. |
| `ART-S006` | `$SOURCE_ROOT/gamedata/loc_PL.scd` | `$PRESERVED_ROOT/gamedata/loc_PL.scd` | 608,377 | `4323e70411dfd486abe75ce1598c13746cf3390c2795e94a97e9c764db8e135d` | n/a | Polish localization archive. |
| `ART-S007` | `$SOURCE_ROOT/gamedata/lua.scd` | `$PRESERVED_ROOT/gamedata/lua.scd` | 7,671,907 | `3632a3294fc01a07ebe017dfd12ce4ff655c2d019934f1d7a784fbc1d4f314bb` | n/a | Retail Lua archive. |
| `ART-S008` | `$SOURCE_ROOT/gamedata/meshes.scd` | `$PRESERVED_ROOT/gamedata/meshes.scd` | 36,067,607 | `24e2e6f8febf16c2f190675f5a0e8b14bb19abfa2040b180bcb53f49fdf0db1d` | n/a | Mesh archive. |
| `ART-S009` | `$SOURCE_ROOT/gamedata/mods.scd` | `$PRESERVED_ROOT/gamedata/mods.scd` | 1,239,705 | `4b0fcf88079453034fbaeb06a333dadfc40b0cde7712c430fe285e180ed354d9` | n/a | Mod-support archive. |
| `ART-S010` | `$SOURCE_ROOT/gamedata/mohodata.scd` | `$PRESERVED_ROOT/gamedata/mohodata.scd` | 526,529 | `f57a18a1756632d6c125f806f50dbe28a6d7adbdd85d2fc8ae99e23000253b04` | n/a | Moho data archive. |
| `ART-S011` | `$SOURCE_ROOT/gamedata/moholua.scd` | `$PRESERVED_ROOT/gamedata/moholua.scd` | 267 | `c3b7e4ae210f26e733d066baefb99ddeac1b1a5dd16002f8d2abdc4b6dd0835b` | n/a | Moho Lua mount/archive placeholder. |
| `ART-S012` | `$SOURCE_ROOT/gamedata/objects.scd` | `$PRESERVED_ROOT/gamedata/objects.scd` | 655,296 | `711dfbf8c8ee13f8d8ab4ab3cca08a59e394de8c9d598fe6780dfa0d9e7541e9` | n/a | Objects archive. |
| `ART-S013` | `$SOURCE_ROOT/gamedata/projectiles.scd` | `$PRESERVED_ROOT/gamedata/projectiles.scd` | 12,430,259 | `60d2732597cde9ef046a663c8d439e60844f967147e14737c7ad5a08c641be12` | n/a | Projectile archive. |
| `ART-S014` | `$SOURCE_ROOT/gamedata/props.scd` | `$PRESERVED_ROOT/gamedata/props.scd` | 2,382,020 | `d323f6ca205482d825b562add1ea03f941aa69ac0cdfb0f969549d13e154b82d` | n/a | Props archive. |
| `ART-S015` | `$SOURCE_ROOT/gamedata/schook.scd` | `$PRESERVED_ROOT/gamedata/schook.scd` | 25,568 | `c9716a24421e005c496ce36afa734119342dc1288fde05c75b77d67bf766f334` | n/a | Script-hook archive. |
| `ART-S016` | `$SOURCE_ROOT/gamedata/skins.scd` | `$PRESERVED_ROOT/gamedata/skins.scd` | 271 | `cdb019bcc7ab2ebc264945e4507fa17b1a3d16bda40de465062111a58710dd41` | n/a | Skin mount/archive placeholder. |
| `ART-S017` | `$SOURCE_ROOT/gamedata/ambience.scd` | `$PRESERVED_ROOT/gamedata/ambience.scd` | 277 | `e8fd733928877232694933a88344eef91bc9b8404ebe162a0bdc0e161680173b` | n/a | Ambience mount/archive placeholder. |
| `ART-M001` | `$SOURCE_ROOT/bin/steam_appid.txt` | `$PRESERVED_ROOT/bin/steam_appid.txt` | 6 | `ce59ed427d6d19ee1eb4363ecc40eb6fb3ccac6cc213ba4e92ba1cb6351fd218` | Steam app `9420` | Steam application identity. |
| `ART-M002` | `$SOURCE_ROOT/installscript.vdf` | `$PRESERVED_ROOT/installscript.vdf` | 334 | `39bc6b304ecb3aa4c65f80e100fd0369d63d675156bfdbd1bfb7beb6cdde5c22` | n/a | Steam install script; identifies app `9420` registry key and August 2009 DirectX redistributable. |
| `ART-M003` | `$SOURCE_ROOT/bin/SupComDataPath.lua` | `$PRESERVED_ROOT/bin/SupComDataPath.lua` | 1,048 | `2d96a60f9d214182cd7246c9572fef807a25710e67009a86f3caf7ccf1119dbe` | n/a | Retail VFS mount and hook configuration. |
| `ART-M004` | `$SOURCE_ROOT/bin/game.dat` | `$PRESERVED_ROOT/bin/game.dat` | 173,576 | `379aa5c867cb9b22310f7dd080e261dabe806a283516bec63ed75a90ea2b3ce6` | n/a | Binary game data/configuration; format not yet identified. |

Record the acquisition date, disk label, install provenance, and whether Steam/GOG/disc patching
changed any binary. Never silently replace an artifact: add a new ID. Use `ART-E` for executables,
`ART-D` for DLLs, `ART-S` for archives, and `ART-M` for manifests or configuration. Every file gets
its own ID; never assign one ID to a group of DLLs or archives.

## Tool and project manifest

| Tool | Version | Path | Project/output | Notes |
|---|---|---|---|---|
| Ghidra | 12.1.2 (Homebrew Cellar, pinned) | `/opt/homebrew/Cellar/ghidra/12.1.2/libexec/support/analyzeHeadless` | `build/re-fa/project`, project name `fa` | The canonical database. Driven headless only; the GUI has never been opened on this project. |
| OpenJDK | 21.0.11 (Homebrew, keg-only) | `/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home` | n/a | Ghidra 12 needs JDK 21+. It is **not** on `PATH`; `build/re-fa/run-import.sh` exports `JAVA_HOME` explicitly. Without that, `analyzeHeadless` fails with "Unable to locate a Java Runtime". |
| `objdump` | Apple LLVM 17.0.0 | `/usr/bin/objdump` | Scratch only | Used for PE headers, timestamps, imports, and export tables; not the primary database. |
| `strings` | Apple system tool | `/usr/bin/strings` | Scratch only | Preserve offsets and encoding when exporting strings. |
| ExifTool | 13.10 | `/opt/homebrew/bin/exiftool` | Scratch only | Used to read PE version resources and timestamps. |
| `shasum` | 6.02 | `/usr/bin/shasum` | Artifact verification | Used with SHA-256 for source and preserved-copy identity. |
| `tools/re/parse_msvc_exports.py` | This repo | `tools/re/parse_msvc_exports.py` | `build/re-fa/exports/*.classes.tsv` | Original tooling, committed. Heuristic MSVC name parser: turns a PE export table into a class → member-name inventory. Not a full demangler; Ghidra demangles properly once a binary is imported. |
| `tools/re/pe_reader.py` | This repo | — | — | Minimal read-only PE32 accessor (sections, VA→bytes, C strings). No `pefile` dependency. |
| `tools/re/extract_moho_methods.py` | This repo | — | `moho.methods.tsv` | Recovers the whole Lua API by scanning `.text` for registration static initialisers (`C-032`). |
| `tools/re/verify_arity.py` | This repo | — | stdout | Checks each recovered signature against the engine's own arity check (`C-039`). |
| `tools/re/annotate_moho_methods.py` | This repo | — | `moho.methods.annotated.tsv` | Adds subsystem, canonical name and purpose to each callable. |
| `tools/re/ReportCoverage.java` | This repo | — | stdout | Ghidra script: the coverage numbers at the top of this document. |
| `tools/re/DumpRefsAndCode.java` | This repo | — | stdout | Ghidra script: references to an address, and decompilation of the functions involved. |
| `tools/re/DumpMohoRegistrations.java`, `DumpMohoRegLayout.java`, `DumpAddrs.java` | This repo | — | stdout | Ghidra scripts from the `WP-02` data-scan phase. Superseded by `extract_moho_methods.py` but kept: they record how the registry layout was found, and `DumpAddrs` is a general pointer-chain walker. |

If another tool is added, record its version and role. Do not let two unlabeled analysis databases
become competing sources of truth.

### Reproducing the session-02 derived data

Everything below is regenerable and therefore **not** committed; it lives under the gitignored
`build/re-fa/`. Regenerate with:

```bash
build/re-fa/run-import.sh                      # Ghidra headless import + analysis, all five binaries
objdump -p <dll> | sed -n '/Export Table/,/Import Table/p' \
  | grep -E '^ *[0-9]+ +0x' > build/re-fa/exports/<name>.exports.txt
python3 tools/re/parse_msvc_exports.py build/re-fa/exports/MohoEngine.exports.txt \
  > build/re-fa/exports/moho.classes.tsv
strings -a <exe> | grep -E '^\.\?A[VU]' | sort -u > build/re-fa/exports/exe.rtti.raw.txt
python3 -c "import zipfile;zipfile.ZipFile('<gamedata>/lua.scd').extractall('build/re-fa/lua')"
```

The `.scd` archives are ordinary ZIP files; Python's `zipfile` reads them directly.

## Address and symbol ledger

This is the compact map that lets a new session resume. One address may have several hypotheses,
but only one canonical analyst name. Addresses are always relative to the exact artifact ID.

Naming convention:

- `fa_<subsystem>_<verb>` for high-confidence behavior, for example `fa_economy_allocate`.
- `maybe_<subsystem>_<verb>` while evidence is incomplete.
- Keep the original Ghidra name in the `Original` column forever.

Image base of `ART-E001` is `0x00400000` and relocations are stripped, so the addresses below are
absolute and stable for this hash. Section map:

| Section | Virtual range | Raw offset | Role |
|---|---|---|---|
| `.text` | `0x00401000`–`0x00c4f8fe` | `0x00001000` | code |
| `PSFD00` | `0x00c50000`–`0x00c52f50` | `0x00850000` | small non-standard section, unexamined |
| `.rdata` | `0x00c53000`–`0x00f87bce` | `0x00853000` | read-only data, RTTI, binding descriptors |
| `.data` | `0x00f88000`–`0x0129cea8` | `0x00b88000` | writable data, registration tables |
| `.tls` | `0x0129d000`–`0x0129f08d` | `0x00bef000` | thread-local storage |
| `.rsrc` | `0x012a0000`–`0x012eb8ec` | `0x00bf2000` | version resource, icons |
| `.bind` | `0x012ec000`–`0x01348000` | `0x00c3e000` | import binding |

| Artifact | Address / RVA | Original | Canonical name | Signature hypothesis | Confidence | Evidence citations | Callers/callees | Last reviewed |
|---|---|---|---|---|---|---|---|---|
| `ART-E001` | `0x00fb9400`–`0x00fbb800` (`.data`) | n/a | `fa_lua_class_registry` | Array of 24-byte class-registration records | High | [EXE] all 60 `moho.*` strings are referenced from inside this range and from nowhere else | Consumed by the `CScrLuaBinder` machinery of `C-011` | 2026-08-29 |
| `ART-E001` | `0x00fba008` (`.data`) | n/a | `fa_luareg_Unit` | Class record for Lua `Unit` | High | [EXE] see `C-026` for the field layout | → `0x00fee88c` method list | 2026-08-29 |
| `ART-E001` | `0x00fee874`, `0x00fee88c` (`.data`) | n/a | `maybe_luamethods_Entity`, `maybe_luamethods_Unit` | Array of `{descriptor*, NULL}` 8-byte entries | Medium | [EXE] reached from the `+16` field of the `Unit` and `Entity` class records | → per-method `.rdata` descriptors | 2026-08-29 |
| `ART-E001` | `0x00e6f0a4` (`.rdata`) | n/a | `maybe_class_CUnitAssistMoveTask` | `{void* fn; char name[];}` with the name stored **inline** | Medium | [EXE] bytes at `+4` decode little-endian to `CUnitAssistMoveTask`, a class from `C-008` | referenced from `0x00fee874` list | 2026-08-29 |
| `ART-E001` | `0x00975c60` (`.text`) | `thunk_FUN_0098f640` | `fa_lua_resolve_receiver_thunk` | 5-byte `jmp` to `0x0098f640` | High | [EXE] `C-035`, `C-036` | called by all 797 `(self)` thunks | 2026-08-29 |
| `ART-E001` | `0x0098f640` (`.text`) | `FUN_0098f640` | `fa_lua_call_context` | `void* __cdecl(void *luaStateWrapper)` — returns `*(void**)(w + 0x44)`, the per-call context | High | [EXE] `C-037`; used by class methods and `<global>` functions alike | sole callee of every binding thunk; result becomes `ecx` | 2026-08-29 |
| `ART-E001` | `0x00977ce0` (`.text`) | `FUN_00977ce0` | `fa_lua_argcount` | `int __cdecl(lua_State*)` — the number of arguments on the Lua stack | High | [EXE] `C-037`, `C-039`: called in the prologue of 608 verified bindings | called by every binding before its arity compare | 2026-08-29 |
| `ART-E001` | `0x00977920` (`.text`) | `FUN_00977920` | `fa_lua_arity_error` | raises using `"%s\n  expected %d args, but got %d"` (`0x00e59e38`) | High | [EXE] `C-037` | called on a failed arity compare | 2026-08-29 |
| `ART-E001` | `0x0059a190` (`.text`) | `FUN_0059a190` | `maybe_lua_fetch_receiver` | returns the native object for a Lua argument | Medium | [EXE] `C-037`: called by `Unit::GetHealth` before reading `+0x98`; not yet read | called by class-method bindings | 2026-08-29 |
| `ART-E001` | `0x0073f6d0` / `0x0073fa40` / `0x0073fdc0` / `0x007401e0` | — | `fa_lua_Damage`, `fa_lua_DamageArea`, `fa_lua_DamageRing`, `fa_lua_MetaImpact` | `int __thiscall(LuaCallContext*)`; arities 5, 6–7, 7, 6 | High | [EXE] `C-032`, `C-039`, `C-040` | the four scripted damage entry points; expected to converge on `SIM_Damage` (`C-021`) | 2026-08-29 |
| `ART-E001` | `0x004ced90` | `FUN_004ced90` | `fa_lua_fetch_c_object` | `{void*,Class*}* __thiscall(LuaObject*)` — `rawget(self,"_c_object")` and return its payload | High | [EXE] `C-044` | called by all 73 per-class resolvers | 2026-08-29 |
| `ART-E001` | `0x0059a190` | `FUN_0059a190` | `fa_lua_resolve_Unit` | receiver cast to `Moho::Unit`; **raises** on null at `0x0059a1c6` | High | [EXE] `C-044`; RTTI `0x00fca61c` | one of 54 raising resolvers | 2026-08-29 |
| `ART-E001` | `0x006273a0` | `FUN_006273a0` | `fa_lua_resolve_Entity_nullable` | receiver cast to `Moho::Entity`; **returns NULL** on null at `0x006273d6` | High | [EXE] `C-044`; RTTI `0x00fcbc48` | one of 19 tolerant resolvers, used by `Destroy`/`BeenDestroyed` | 2026-08-29 |
| `ART-E001` | `0x004cde60` (null write at `0x004cdeab`) | `FUN_004cde60` | `fa_CScriptObject_dtor` | nulls the Lua-visible native pointer, then unlinks the weak-pointer list | High | [EXE] `C-045`; 91 direct callers; pattern unique in `.text` | called by every scriptable class destructor | 2026-08-29 |
| `ART-E001` | `0x004ce630` | `FUN_004ce630` | `fa_lua_IsDestroyed` | `resolver()==0 \|\| *(int*)resolver()==0` | High | [EXE] `C-046` | the `<global> IsDestroyed` binding | 2026-08-29 |
| `ART-E001` | `0x00698650` | `FUN_00698650` | `fa_lua_Entity_BeenDestroyed` | null receiver **or** `Entity+0x1b9 != 0` | High | [EXE] `C-046` | the `Entity:BeenDestroyed` binding | 2026-08-29 |
| `ART-E001` | `Entity + 0x98` | — | `fa_Entity_health` | `float` | High | [EXE] `C-038`: `flds 0x98(%esi)` in `Unit::GetHealth` | read by `Unit::GetHealth`, `Entity::GetHealth` | 2026-08-29 |
| `ART-E001` | `0x0073e950` | `FUN_0073e950` | `maybe_damage_SIM_MetaImpactArea` | area damage — the `C-021` entry | Medium-High | [EXE] `C-041`/`C-042`: BSim sig 201, called by the `MetaImpact` Lua binding | ← `fa_lua_MetaImpact 0x007401e0` | 2026-08-29 |
| `ART-E001` | `0x0074dc40` | `FUN_0074dc40` | `maybe_session_Sim_TransferUnit` | changes a unit's owning army | Medium-High | [EXE] `C-042`: BSim sig 489, called by `ChangeUnitArmy` | ← `ChangeUnitArmy` binding | 2026-08-29 |
| `ART-E001` | `0x006b1390` | `FUN_006b1390` | `maybe_economy_Unit_SetConsumptionActive` | toggles a unit's resource draw | Medium-High | [EXE] `C-042`: BSim sig 100; the binding's own name matches exactly | ← `Unit:SetConsumptionActive` | 2026-08-29 |
| `ART-E001` | `0x00681ae0`, `0x008bf570` | — | `maybe_blueprint_Entity_IsInCategory`, `..._UserEntity_IsInCategory` | category test | Medium-High | [EXE] `C-042` | ← `Entity:Kill`, `UserUnit:IsInCategory` | 2026-08-29 |
| `ART-E001` | `0x006adf20`, `0x006ae7f0` | — | `maybe_orders_Unit_ToggleScriptBit`, `maybe_orders_Unit_IsIdleState` | per-unit state bits | Medium | [EXE] `C-042` | ← the same-named bindings | 2026-08-29 |
| `ART-E001` | `0x0067f3c0`–`0x0067f4b0` | — | `maybe_intel_Entity_SetVizTo{Allies,Enemies,Neutrals,FocusPlayer}` | four visibility setters, **order unknown** | Low | [EXE] `C-043`: BSim returns the same name for all four | — | 2026-08-29 |

Names prefixed `maybe_` are BSim candidates at roughly 80% accuracy (`C-041`). They are leads, not
facts, and **must not be promoted to `fa_*` without a second source** — a corroborating caller, a
matching signature, or a read of the code.
| `ART-E001` | `0x006ca530` → `0x006ca5b0` | `FUN_006ca530` | `fa_lua_thunk_Unit_GetUnitId` → `fa_Unit_GetUnitId` | thunk `int(lua_State*)`; method `int __thiscall(lua_State*)` | High | [EXE] `C-032`, `C-035`; signature string `GetUnitId(self)` | thunk → `fa_lua_resolve_receiver`, then tail-call | 2026-08-29 |
| `ART-E001` | `0x006cb720` → `0x006cb7a0` | `FUN_006cb720` | `fa_lua_thunk_Unit_GetHealth` → `fa_Unit_GetHealth` | as above | High | [EXE] `C-032`, `C-035`; signature string `GetHealth(self)` | thunk → `fa_lua_resolve_receiver`, then tail-call | 2026-08-29 |
| `ART-E001` | `0x00fee88c`, `0x00fee874` (`.data`) | n/a | `fa_luamethods_Unit`, `fa_luamethods_Entity` | the class method-list globals named by descriptor field `+20` | High | [EXE] `C-032`; 125 and 65 registrations respectively | written by the per-method static initialisers | 2026-08-29 |

The full 1,182-row table is **not committed** — it is derived metadata from a proprietary binary and
is regenerable in about a second. Produce it, verify it, and annotate it with:

```bash
python3 tools/re/extract_moho_methods.py --tsv build/re-fa/exports/moho.methods.tsv
python3 tools/re/verify_arity.py                     # 549/608 signatures machine-checked
python3 tools/re/verify_arity.py --mismatches        # the 59 that disagree
python3 tools/re/annotate_moho_methods.py            # adds subsystem / canonical / purpose
```

`moho.methods.tsv` columns: `scope`, `name`, `signature`, `method_va`, `wrapper_va`,
`method_list_va`, `init_site_va`, `doc`.

`moho.methods.annotated.tsv` prepends four more:

| Column | Meaning |
|---|---|
| `subsystem` | Which part of the engine the callable belongs to (see the census below). |
| `canonical` | `fa_<subsystem>_<Scope><Name>` — this ledger's naming convention, ready to paste. |
| `purpose` | One line of prose describing what it does. |
| `purpose_source` | `engine` where the binary ships the text, `derived` where it was generated from the name. **Always check this before quoting a purpose as fact.** |

305 of the 1,182 purposes are the engine's own words; the other 877 are derived from the verb
and noun in the name and are a navigation aid, not evidence.

#### Subsystem census of the retail Lua API

| Subsystem | Count | | Subsystem | Count |
|---|---:|---|---|---:|
| `ui` | 247 | | `world` | 22 |
| `ai` | 165 | | `intel` | 21 |
| `misc` | 113 | | `transport` | 16 |
| `combat` | 101 | | `audio` | 16 |
| `lifecycle` | 98 | | `debug` | 12 |
| `session` | 67 | | `platform` | 10 |
| `animation` | 50 | | `selection` | 9 |
| `fx` | 49 | | `filesystem` | 9 |
| `movement` | 32 | | `persistence` | 8 |
| `blueprint` | 29 | | `ordnance` | 7 |
| `build` | 28 | | `shields` | 3 |
| `runtime` | 24 | | `resources` | 2 |
| `economy` | 22 | | **`veterancy`** | **0** |
| `orders` | 22 | | | |

Two rows of that census are evidence in their own right, because a zero is a finding:

- **`veterancy` is 0.** The retail engine exposes *no* veterancy API to Lua at all —
  independent corroboration of `C-024`, which concluded from the shipped scripts that
  veterancy is entirely Lua-owned. Two unrelated methods agreeing is worth more than either.
- **`shields` is 3**, and all three are `Unit::GetShieldRatio`/`SetShieldRatio` plus a global
  `_c_CreateShield` — no shield object methods. That is what `C-034` predicted from the empty
  `moho.shield_methods` table, now visible from the other direction.

## Claim ledger

Every conclusion that may affect implementation gets a stable ID. Do not rewrite history when a
claim changes: mark it superseded and add the replacement.

| Claim ID | Work package | Claim | Evidence | Confidence | Status | Implementation impact |
|---|---|---|---|---|---|---|
| `C-001` | `WP-01` | `ART-E001` statically links the entire engine. Its import table names only Windows, D3D9/`d3dx9_35`, DirectSound/`X3DAudio1_2`, WS2_32, Steam and BugSplat DLLs — no `MohoEngine.dll`, `gpgcore.dll`, `gpggal.dll` or `LuaPlus_1081.dll`. Those four shipped DLLs are **not loaded by the game**. | [EXE] `ART-E001` PE import directory | High | Confirmed | All executable analysis targets one binary. Do not attribute retail behavior to `ART-D001`–`ART-D004` without proving the exe contains the same code. |
| `C-002` | `WP-00`, `WP-01` | `ART-E001` and `ART-D001` were built from the same source tree: their PE debug directories name `c:\work\rts\main\code\bin8\SupremeCommander.pdb` and `...\MohoEngine.pdb`. | [EXE] `ART-E001`, `ART-D001` debug directory | High | Confirmed | Legitimises using `ART-D001`'s exported symbol names as a naming dictionary for functions found in `ART-E001`. Version drift (2007 DLL vs 2011 exe) means names, not addresses, transfer. |
| `C-003` | `WP-01`, `WP-03` | The engine object model is an ordinary C++ class hierarchy with vtables and full RTTI, rooted at `Moho::Entity`, with `Moho::Unit`, `Moho::Projectile`, `Moho::Prop`, `Moho::Shield` and `Moho::ReconBlip` as sibling concrete entity classes. 2,728 `Moho::` RTTI type descriptors are present, 471 of them concrete non-template classes. | [EXE] `ART-E001` `.?AV*@Moho@@` type descriptors; [EXE] `ART-D001` exports | High | Confirmed | `PE-02` resolved. Recoil Metal's typed-entity approach matches. The "manager-owned records behind opaque handles" candidate is refuted. |
| `C-004` | `WP-03` | Entity identity is an engine-assigned integer id from a pool, owned by `Moho::EntityDB` (`AssignId`, `ReserveId`, `DoReserveId`, `ReleaseId`, `LookupId`), with classes `Moho::IdPool` and `Moho::EntId`. Destruction is deferred: `Entity::DestroyQueued`, `Entity::IsReadyForDelete`, `EntityDB::ReadyForDelete`, `EntityDB::Purge`, `Sim::GetOnDestroyedQueue`. | [EXE] `ART-D001` exports; [EXE] `ART-E001` RTTI | High | Confirmed | `PE-03`: "stable integer IDs resolved by managers" **and** "deferred destruction queue" both hold. Ids are recycled via a pool, so a stale id can be reused — generation/versioned handles are refuted, meaning retail relies on the deferred-purge window for safety. |
| `C-005` | `WP-04` | The simulation advances in named "beats" with explicit engine-level stages, not one monolithic loop and not pure per-object virtual updates: `Sim::AdvanceBeat`, `Sim::GetCurrentTick`, `Sim::AdvanceCommandClock`, and stage accessors `Sim::GetMotionUpdateStage`, `Sim::GetScriptStage`, `Sim::GetCommandDispatchStage`. Per-entity work hangs off virtual `Entity::MotionTick` and `Entity::TaskTick` (overridden by `Unit`, `Projectile`, `CUnitMotion`). | [EXE] `ART-D001` exports `Sim`, `Entity`, `Unit`, `CUnitMotion` | High | Confirmed | `PE-01`: "hybrid manager phases plus object callbacks" confirmed; "per-object virtual updates" alone refuted. The stage *order* is still untraced — that is the open `WP-04` question. |
| `C-006` | `WP-04`, `WP-43` | Determinism is a first-class native concern. `Sim` exposes `GetBeatChecksum`, `UpdateChecksum`, `VerifyChecksum`, `GetChecksumContext`, `GetChecksumDigest`; many classes implement their own `UpdateChecksum`; `Moho::SDesyncInfo` exists. The RNG is a Mersenne Twister: `Moho::CMersenneTwister` (`Seed`, `IRand`, `ShuffleState`, `Checksum`, `N`) wrapped by `Moho::CRandomStream` (`SetSeed`, `IRand`, `FRand`, `DRand`, `FRandGaussian`, `Checksum`), reached through `Sim::GetRandom`. | [EXE] `ART-D001` exports; [EXE] `ART-E001` RTTI | High | Confirmed | `WP-04` RNG ownership answered: retail is MT19937-family, per-sim, checksummed, seeded once. Recoil Metal must match the generator family and draw order to claim replay parity. The exact MT variant and seeding are not yet read. |
| `C-007` | `WP-33` | Intel is stored in rasterised grids, not recomputed per query. `Moho::CIntelGrid` has `AddCircle`, `SubtractCircle`, `DelayedSubtractCircle`, `Raster`, `GetCoverage`, `IsVisible`, `Tick`, `UpdateChecksum`; `Moho::CIntelXGrid` is the explored map (`AddCircle`, `IsExplored`, `ExploreAll`). Separate grids exist per intel type: `CAiReconDBImpl::ReconGetRadarGrid`/`ReconGetSonarGrid`/`ReconGetOmniGrid`/`ReconGetJamingBlips`. `Moho::VisionDB` and `CWldSession::VisionDB` own vision. | [EXE] `ART-D001` exports; [EXE] `ART-E001` RTTI (`CIntel`, `CIntelGrid`, `CIntelCounterHandle`, `CIntelPosHandle`, `VisionDB`) | High | Confirmed | `PE-20`: incremental grids confirmed; "periodic complete rebuild" and "per-query spatial tests" refuted. `GetCoverage` plus `CIntelCounterHandle` implies a **refcount** grid, and `DelayedSubtractCircle` with `Moho::SDelayedSubVizInfo` suggested removal was deferred. **`C-076` refuted this for retail**: that function has zero callers in `ART-E001` and every removal path is immediate. The inference was drawn from a symbol's existence rather than from a caller, which is the error `C-075` generalises. |
| `C-008` | `WP-12`, `WP-16`, `WP-17`, `WP-25` | Unit orders are executed by **separate native task classes**, not one generalised work engine. RTTI names at least: `CUnitMoveTask`, `CUnitFormAndMoveTask`, `CUnitAssistMoveTask`, `CUnitPatrolTask`, `CUnitGuardTask`, `CUnitAttackTargetTask`, `CUnitMeleeAttackTargetTask`, `CUnitFireAtTask`, `CFireWeaponTask`, `CAcquireTargetTask`, `CUnitMobileBuildTask`, `CFactoryBuildTask`, `CUnitGetBuiltTask`, `CUnitUpgradeTask`, `CUnitRepairTask`, `CUnitCaptureTask`, `CUnitReclaimTask`, `CUnitSacrificeTask`, `CUnitTeleportTask`, `CUnitCallTeleport`, `CUnitLoadUnits`, `CUnitUnloadUnits`, `CUnitFerryTask`, `CUnitWaitForFerryTask`, `CUnitCallTransport`, `CUnitCallLandTransport`, `CUnitCallAirStagingPlatform`, `CUnitCarrierLand`, `CUnitCarrierLaunch`, `CUnitCarrierRetrieve`, `CUnitRefuel`, `CUnitPodAssist`, `CUnitScriptTask`, `CWaitForTask`, `CCommandTask`, over the shared bases `CTask`/`CBuildTaskHelper`. | [EXE] `ART-E001` RTTI | High | Confirmed | `PE-13` resolved against candidate 1: repair, capture and reclaim are **separate native state machines** sharing a base and a build-rate helper, not one parameterised work engine. Also refutes the session-02 interim reading drawn from DLL exports alone (see evidence trap 2). |
| `C-009` | `WP-11` | `Moho::RRuleGameRules`/`RRuleGameRulesImpl` is **not** the victory-condition system despite the name. Its entire method surface is blueprint and category management: `GetUnitBlueprint`, `GetProjectileBlueprint`, `GetPropBlueprint`, `GetMeshBlueprint`, `InitBlueprint`, `InitCategories`, `ParseEntityCategory`, `ResolveCategoryReferences`, `SetupEntityCategories`, `GetEntityCategory`, `GetUnitCount`, `FindFootprint`, `ExportToLuaState`, `UpdateLuaState`, `UpdateChecksum`. | [EXE] `ART-D001` exports | High | Confirmed | Prevents a costly wrong turn on `WP-11`. Victory/defeat predicates are *not* here; no native symbol matches `Victory`, `Defeat` or `Score`. `WP-11` should be pursued through scenario Lua first. |
| `C-010` | `WP-06` | Blueprints are parsed into native typed structs, one per section: `RUnitBlueprint` with `RUnitBlueprintGeneral`, `Physics`, `Economy`, `Defense`, `DefenseShield`, `Intel`, `Air`, `Transport`, `Weapon`, `AI`, `Display`; likewise `RProjectileBlueprint` (`Display`/`Economy`/`Physics`) and `RPropBlueprint` (`Defense`/`Display`/`Economy`), plus `RMeshBlueprint`, `REmitterBlueprint`, `RTrailBlueprint`, `RBeamBlueprint`, `REffectBlueprint`, `REntityBlueprint`. | [EXE] `ART-E001` RTTI; [EXE] `ART-D001` exports | High | Confirmed | Blueprint defaults and coercion live in native struct initialisation, not in Lua. `WP-06` merge-order questions must be answered at `RRuleGameRulesImpl::InitBlueprint`. |
| `C-011` | `WP-02` | Lua binding is generated per bound class through the template `Moho::CScrLuaMetatableFactory<T>`, supported by `CScrLuaBinder`, `CScrLuaClassBinder`, `CScrLuaObjectFactory`, `CScrLuaBaseClassSpec`, `CScrLuaInitForm`/`CScrLuaInitFormSet`. The bound classes are enumerable from the instantiated factory symbols. 60 registration-table name strings of the form `moho.<name>_methods` are present in `ART-E001`. | [EXE] `ART-D001` exports; [EXE] `ART-E001` strings | High | Confirmed | `PE-27`: "generated registration tables" confirmed; "generic dispatcher by method ID" refuted. Gives `WP-02` a mechanical path: each `moho.*_methods` string is referenced by exactly the registration site that builds that table. |
| `C-012` | `WP-20`, `WP-21` | Pathing is native and service-shaped, not per-unit ad-hoc: `Moho::CAiPathFinder`, `CAiPathNavigator`, `CAiNavigatorLand`, `CAiNavigatorAir`, `CAiPathSpline`, `CPathPoint`, `PathQueue`, `PathTables`, `PathPreviewFinder`, `IPathTraveler`, plus `Sim::GetPathTables` and `AI_ClearPathData`/`AI_TestForTerrainBlockage`. Routes are **splines** (`CUnitMotion::SetSplineData`). Occupancy is a separate grid: `Moho::COGrid` with `Unit::ReserveOgridRect`, `FreeOgridRect`, `CanReserveOgridRect`, `GetReservedOgridRect`, and free functions `OCCUPY_Check`, `OCCUPY_MobileCheck`, `OCCUPY_MobileCheck_1x1`, `OCCUPY_CheckAreaFlatness`, `OCCUPY_CheckEdgeFlatness`. | [EXE] `ART-E001` RTTI; [EXE] `ART-D001` exports | High | Confirmed | `PE-04`: a shared path **request service** (`PathQueue`) with precomputed `PathTables` is confirmed over naive per-unit A*. `PE-06`: an explicit occupancy/reservation grid exists, so retail blockage is not steering-only. Recoil Metal's per-unit A* over a plain grid is a structural difference worth measuring. |
| `C-013` | `WP-22`, `WP-23`, `WP-24` | All movement domains share **one** mover class, `Moho::CUnitMotion`, which branches per layer: `CalcMoveCommon` plus `CalcMoveLand`, `CalcMoveWater`, `CalcMoveAir`, `CalcMoveHover`, `CalcMoveBallistic`. Air specifics are methods on the same class (`CalcWingedLift`, `CalcWingedOrientation`, `CalcCirclingOrientation`, `CalcAirMovementDampingFactor`, `ComputeAirControl`, `ComputeAirCombatTactics`, `AttemptingToLand`, `ShouldHoverInsteadOfLand`, `ProcessFuelLevels`, `GetFuelUseTime`). Layers are discrete and transitions explicit: `Entity::GetCurrentLayer`/`SetCurrentLayer`, `CUnitMotion::UpdateCurrentLayer`, `SetNewTargetLayer`, `TransitionBetweenLayers`, `HandleDivingAndSurfacing`, `IsOnValidLayer`, and `COORDS_LayerToString`/`COORDS_StringToLayer`. | [EXE] `ART-D001` exports (`CUnitMotion`, 74 methods) | High | Confirmed | `PE-08`: "mover class per aircraft family" refuted; one data-driven mover with per-layer branches confirmed. `PE-09`: discrete layers with explicit transition confirmed over continuous depth. Submarine diving is a layer transition, not a depth scalar. |
| `C-014` | `WP-15` | Economy is a per-unit request/consume cycle feeding army-level pools: `Unit::UpdateResourceRequest`, `UpdateResourceConsumption`, `UpdateResourceProduction`, `GetConsumptionRequest`, `GetResourceConsumed`, `HandleResourceManagement`, `ResetEconValues`, `SetConsumptionActive`, `SetProductionActive`, `IsConsumptionActive`, `IsProductionActive`, with `Moho::CEconomy`, `CEconRequest`, `CEconStorage`, `CEconomyEvent`, `CSimResources`/`ISimResources` and `Sim::GetResources`. Events are serialised (`Unit::SerEconomyEvents`, `AddEconomyEvent`). | [EXE] `ART-E001` RTTI; [EXE] `ART-D001` exports | High | Confirmed | `PE-11`: separate request objects (`CEconRequest`) and storage (`CEconStorage`) exist, so allocation is a two-phase request-then-satisfy, not a single sequential drain. Rounding point and priority order remain untraced — that is the `WP-15` question. |
| `C-015` | `WP-21` | Formation is assigned **on the command object at issue time**, not by a persistent per-unit controller: `CUnitCommand::GenerateFormation`, `HasFormation`, `GetFormation`, `RemoveUnitFromFormation`, alongside `AddUnit`/`RemoveUnit`/`GetUnits`/`GetTotalCount`/`GetCurrentCount`. Formation selection is scripted: `FORMATION_PickBestFormation`, `FORMATION_PickTravelFormation`, `FORMATION_RunScript`, `FORMATION_GetScriptName`/`GetScriptIndex`/`GetNumScripts`, with `Sim::GetFormationDB`, `Moho::CAiFormationDBImpl`, `CAiFormationInstance`, `CFormationInstance`, `CSquad`. Multi-unit coordination is explicit: `CUnitCommand::CoordinateWith`, `IsCoordinating`, `CheckForCoordinationSuccess`, `SatisfiyCoordination` (sic). | [EXE] `ART-D001` exports; [EXE] `ART-E001` RTTI | High | Confirmed | `PE-05`: "destination offsets assigned once at command issue" confirmed, but with a persistent `CFormationInstance` object rather than plain offsets, and with Lua-scripted formation shapes. |
| `C-016` | `WP-25` | Attachment is a generic parent-child graph on `Entity` (`AttachTo`, `DetachFrom`, `GetAttachedEntities`, `CalculateAttachedTransform`, `SetParentOffset`, `GetParentInfo`, `Moho::SEntAttachInfo`) with explicit death propagation in both directions (`AttachedEntityDestroyed`, `AttachedEntityKilled`, `ParentEntityDestroyed`, `ParentEntityKilled`). Transport logic sits above it in `IAiTransport`/`CAiTransportImpl` with `Unit::GetAssignedTransport`, `SetTransportedBy`, `CalcTransportLoadFactor`, `GetTransportFerryBeacon`. | [EXE] `ART-D001` exports | High | Confirmed | `PE-10`: generic attachment graph plus a transport-specific controller — the hybrid candidate. Death propagation is callback-driven, so ordering is observable and must be matched. |
| `C-017` | `WP-16` | Build progress is stored on the **target** unit, with a separate builder-side work value: target-side `Entity::GetBuildProgress`, `GetFractionComplete`, `UpdateFractionComplete`, `IsBeingBuilt`; builder-side `Unit::GetWorkProgress`/`SetWorkProgress` and `Unit::GetBuilder`. Native builder controller is `IAiBuilder`/`CAiBuilderImpl`, with `Moho::CBuildTaskHelper` shared by the build tasks. | [EXE] `ART-D001` exports | High | Confirmed | `PE-12`: "progress stored on target unit" confirmed, combined with a builder-owned task holding a backlink. The two values are distinct — conflating them is a parity bug. |
| `C-018` | `WP-29` | Silo ammunition is a **counter**, not child entities: `Moho::IAiSiloBuild`/`CAiSiloBuildImpl`, `Unit::GetSiloBuild`, `GetSiloBuildCount`, `GetSiloStorageCount`, `GetSiloMaxStorageCount`, plus `Unit::GetCountedProjectileWeapon` and `AI_CreateSiloBuilder`. | [EXE] `ART-D001` exports | High | Confirmed | `PE-17`: "ammo as child entities" refuted. Ammo is a build-then-store counter on the launching unit. |
| `C-019` | `WP-09`, `WP-37` | The terrain model is `Moho::STIMap` over a `Moho::CHeightField`, with **four distinct elevation queries** — `GetTerrainElevation`, `GetSurfaceElevation`, `GetWaterElevation`, `GetDeepElevation`, `GetAbyssElevation` — plus `GetTerrainType`/`GetTerrainTypeCode`/`SetTerrainTypeRect`, `IsBlockingTerrain`, `IsPlayable`, `GetPlayableMapRect`, `SurfaceIntersection`, `TerrainIntersection`. `Sim::FlattenMapRect` mutates it. | [EXE] `ART-D001` exports | High | Confirmed | Surface elevation (what a unit stands on) and terrain elevation (the heightfield) are **separate** queries — water and abyss thresholds sit between them. `Sim::FlattenMapRect` proves the sim-side heightfield is mutable, which bears on `WP-37`. |
| `C-020` | `WP-31`, `WP-32` | Shields are first-class entities in the entity database, not a damage-gate flag: `Moho::Shield` is an `Entity` subclass (`Entity::IsShield`), registered via `Sim::AddShield`/`RemoveShield`/`GetShields` and iterated by `EntityDB::ShieldsBegin`/`ShieldsEnd`/`AllShieldsBegin`/`AllShieldsEnd`. `Unit::GetShieldRatio`/`SetShieldRatio` carry the display value. | [EXE] `ART-D001` exports; [EXE] `ART-E001` RTTI | High | Confirmed | `PE-19`: "shield entities in spatial index" is the favoured candidate; a pure damage-boundary containment test is refuted, because shields have entity identity and collision. Recoil Metal's damage-gate architecture is a real structural difference. |
| `C-021` | `WP-30` | The damage entry points are the free functions `Moho::SIM_Damage` and `Moho::SIM_MetaImpactArea`, with armor handled by `Moho::ARMOR_GetArmorDefinations` (sic) and `Unit::InitializeArmor`, `GetArmorMult`, `AlterArmor`, `ProcessArmorOnDamage`. Projectiles carry `GetDamageAmount`/`GetDamageRadius`/`GetDamageType` and a single `Impact` plus `CheckCollision`. `Moho::CDamage` is the Lua-bound damage object. | [EXE] `ART-D001` exports | High | Confirmed | `PE-18`: a **shared damage gate** is confirmed by the existence of two free entry points that all sources funnel through. `SIM_Damage` is the single highest-value address to recover for `WP-30`; ordering of armor, shield and callback all resolve there. |
| `C-022` | `WP-07` | The Lua scheduler is native and thread-shaped: `Moho::CTask`, `CTaskThread`, `CTaskStage`, `CTaskEvent`, `CLuaTask`, `Moho::PausedThread`/`PausedMainThread`/`PausedChildThread`, with `Moho::STaskEventLinkage` and `Sim::GetScriptStage`. `Sim::ExecuteLuaInSim` and `Sim::LuaSimCallback` bridge in. | [EXE] `ART-E001` RTTI; [EXE] `ART-D001` exports | High | Confirmed | `ForkThread`/`WaitTicks` are backed by native task threads with stages, not plain Lua coroutines the VM schedules. Wake ordering is therefore an engine property and matters for `WP-04` determinism. |
| `C-023` | `WP-07` | The retail Lua dialect uses `#` for line comments in shipped sim/UI scripts, not `--`, and shipped files are ISO-8859-1 with CRLF endings. | [LUA-R] `ART-S007` `lua/sim/Unit.lua` lines 1–9, 3108; every shipped `lua/**.lua` | High | Confirmed | Any retail-Lua ingestion in Recoil Metal must accept `#` comments and 8-bit encodings. Also the root cause of evidence trap 1. |
| `C-024` | `WP-35` | Retail FA veterancy is entirely Lua-owned and blueprint-driven. Thresholds are cumulative unit `KILLS` from `blueprint.Veteran`, defaulting to `Game.VeteranDefault = {Level1=25, Level2=100, Level3=250, Level4=500, Level5=1000}`. Kill credit flows `Unit:OnKilledUnit(victim)` → `CheckVeteranLevel()`, which reads `GetStat('KILLS',0).Value + 1` (it runs *before* the stat is written) and promotes at most **one** level; `AddKills(n)` is the multi-level path and loops. Promotion applies buffs `VeterancyHealth<L>` and `VeterancyRegen<L>`, both `Duration = -1`, `Stacks = 'REPLACE'`: health is `MaxHealth` **Mult** 1.1/1.2/1.3/1.4/1.5, regen is `Regen` **Add** 2/4/6/8/10. Blueprint `Buffs[<type>].Level<N>` can add overrides via `CreateVeterancyBuff`. Then `AIBrain:OnBrainUnitVeterancyLevel` and the `OnVeteran` unit callback fire. Per-weapon damage buffs are present but **commented out** in retail with `# TODO: Enable per weapon buffs again`. | [LUA-R] `ART-S007` `lua/sim/Unit.lua` `AddKills`/`SetVeterancy`/`CheckVeteranLevel`/`SetVeteranLevel`/`OnKilledUnit`/`BuffTypes`; `lua/game.lua:11-17`; `lua/sim/BuffDefinitions.lua` `VeterancyHealth1-5`, `VeterancyRegen1-5` | High | Confirmed | `PE-22` resolved: "Lua buff application" confirmed, "native kill counter and fixed table" refuted. Confirms retail veterancy does **not** change weapon damage, and pins every constant. `WP-35` is now fully specified without any EXE work; the only native dependency is the `KILLS` stat store and callback timing. |
| `C-032` | `WP-02`, `WP-03` | The complete retail Moho Lua API is recoverable **from code, not data**. Every Lua-callable native function has a compiler-generated static initialiser that fills a descriptor with `mov dword ptr [absolute], immediate` stores (`C7 05`), laid out `+0` name, `+4` scope, `+8` signature-with-documentation, `+16` native wrapper, `+20` the class's method-list global (0 for a free function). Scanning `.text` for runs of those stores recovers **1,182 callables across 54 method-list globals**: 776 class-bound methods and 406 `<global>` free functions, each with its Lua name, its authored signature *including parameter names*, its documentation string where the engine ships one, and the address of its native wrapper. Example: `Unit::GetUnitId`, signature `GetUnitId(self)`, wrapper `0x006ca530`, list `0x00fee88c`, initialiser `0x006ca560`. | [EXE] `ART-E001` `.text` static-initialiser scan; reproduce with `tools/re/extract_moho_methods.py` | High | Confirmed | This is `WP-02`'s deliverable and the key that unlocks every later subsystem: any Moho method named in shipped Lua now resolves directly to a native address to decompile. Largest scopes: `Unit` 125, `Entity` 65, `CAiBrain` 64, `CPlatoon` 49, `UserUnit` 36, `CAiPersonality` 35, `UnitWeapon` 32, `Projectile` 30. |
| `C-035` | `WP-02`, `WP-03` | Every Lua-callable `(self)` method is reached through a **20-byte generated thunk** of identical shape: `mov eax,[esp+4]` (the `lua_State*`), `push eax`, `call 0x00975c60`, `add esp,4`, `mov ecx,eax` (receiver becomes `this`), `jmp <real method>`. Following the tail call resolves **797 of the 1,182 callables to their real native method, all 797 distinct** — no aliasing. The remaining 385 have wrappers of other shapes (more marshalling). **All 797 call the same resolver, `0x00975c60`**, which converts a Lua userdata into a native object pointer. | [EXE] `ART-E001` disassembly at `0x006ca530` (`Unit::GetUnitId`) and `0x006cb720` (`Unit::GetHealth`), plus the thunk scan | High | Confirmed | Two things. It verifies `C-032`: a method declared `GetUnitId(self)` really does consume exactly one argument and use it as the receiver, and the calling convention is `__thiscall`. And it hands `WP-03`/`PE-03` its decisive anchor for free — `0x00975c60` is *the* userdata-to-object resolution point, so the stale-reference rules live inside one function. |
| `C-036` | `WP-02` | ~~The thunk's callee resolves the Lua receiver, unchecked.~~ **SUPERSEDED — twice, and the second correction matters more than the first.** `C-037` fixed *what* `0x0098f640` returns (a call context, not a receiver). `C-044` then refuted the damaging half of the original claim: the sentence "the engine performs no validity check" was extrapolated from that one function to the whole binding path, and it is **false**. The receiver is resolved separately, per call, and checked every time. The function at `0x0098f640` (reached via the `jmp` thunk at `0x00975c60`) is `mov eax,[esp+4]; mov eax,[eax+0x44]; ret`. That much is correct. The *interpretation* was not: it was read as userdata → native receiver resolution. | [EXE] `ART-E001` `0x0098f640` | High | **Superseded** | Refuted by counting: **112 of the 797 thunk-resolved callables are `<global>` free functions with no receiver at all**, and their thunks are byte-identical. A step every global also takes cannot be receiver resolution. The error was inferring a mechanism from one plausible reading of two examples, both of which happened to be methods. |
| `C-109` | `WP-19`, `WP-06` | **`C-073`(a) is unblocked. The engine's default footprint is `ceil(SizeX)` / `ceil(SizeZ)` from the TOP-LEVEL blueprint keys — not a constant 1.** Read from `REntityBlueprint::OnInitBlueprint` at `0x00518B60`: when `Footprint.SizeX` (a **byte** at `+0xD8`) is zero it computes `rint(SizeX) + (SizeX > rint(SizeX))`, which is exactly `ceil`, from the float at `+0xAC`. Three identical blocks follow for `Footprint.SizeZ` and both `AltFootprint` fields. The constructor zeroes those bytes, so **an explicit `Footprint.SizeX = 0` is indistinguishable from an absent one**. Two bonus facts from the same pass: `RUnitBlueprintPhysics::ComputeDerivedQuantities` (`0x00525D60`) then **clamps `SkirtSize = max(SkirtSize, Footprint)` and `SkirtOffset = min(SkirtOffset, 0)`** — a positive skirt offset is silently discarded — and the twelve naval factories *do* carry a `Footprint` block, containing only `MinWaterDepth`, so their footprint comes from `SizeX=4`/`SizeZ=10`. | [EXE] `ART-E001` `0x00518B60`–`0x00518C6D`, `0x00525D60`; schema tables `0x00519370`, `0x00513040`, `0x00527540`; [BP-R] `ART-S001` census | High | Confirmed | **The discriminator the earlier audit lacked is UEB4302**: `SizeX = 1.75`, no `Footprint`, `SkirtOffset = −0.5`, `SkirtSize = 3`. Every FA structure obeys `SkirtOffset = −(SkirtSize − Footprint)/2`, which solves to footprint 2 = `ceil(1.75)`; a default of 1 leaves it off-centre. Corpus-wide, **84 of 102 structures centre correctly under `ceil` against 80 under `1`**. The fix is now writable: `footprint = bp.Footprint.SizeX or ceil(bp.SizeX)`, treating an explicit 0 as absent, then apply the two clamps. |
| `C-110` | `WP-30`, `WP-31` | **Our shield absorption is applied once to the whole blast and tested against the blast centre — the most consequential defect found in the campaign.** An agent built a probe against `librm_core.a` and measured it: one 100-point shield near a blast centre absorbed 40 and thereby saved **three separate units**, including one 150 elmos away and entirely outside the 80-elmo dome. Retail subtracts a per-shield amount inside the **per-entity** call, so each of those three takes its own damage less whatever shield actually covers *it*. Two further errors in the same block: coverage is computed from `centre − shieldCentre` rather than from the target, so a blast outside a dome damages a unit inside it at full strength; and a `break` after the first absorbing shield means **only one shield ever absorbs**, where retail walks the whole world shield list. | [RM] `src/core/sim/Combat.cpp:801-851`, probe-verified; [EXE] `C-060`, `C-062` | High | **Fixed** | It had been **silently cancelling most of what the falloff fix restored**. Rewritten: bubbles are gathered once with an absorption budget each, charged **per target** against the target's own position, **all** covering bubbles stack rather than the first winning, and each bubble pays once however many targets it sheltered — retail's shape per `C-062`. **One part of the audit was wrong and was not applied:** it reported the proportional rescale as a defect against retail's flat subtraction, but `DamageProfile` stores damage pre-resolved per armour class (`ADR-033`), so `against(c) = A·M(c)` and scaling by `(A−S)/A` yields `(A−S)·M(c)` — the same arithmetic in different coordinates. A literal flat subtraction on a resolved profile would have been the bug. Four regression tests now encode the measured scenarios. |
| `C-111` | `WP-30` | Four smaller damage-path divergences from the same probe-backed audit. (a) **No `amount == 0` or `amount <= 0` guard** — a zero-damage blast emits a `UnitDamaged` event per unit in radius, reachable from shipped content via an armour multiplier of exactly `0.0`; and negative damage **heals**. (b) **A point hit is a spatial query, not a named entity** — three units within 4 elmos of an impact each took the full amount, so a radius-0 weapon deals N× its authored damage against a crowd, even though `advanceProjectiles` has already resolved the struck slot and then discards it. (c) **A point hit consults shields**, which retail's kind-0 path never does. (d) **`UnitDamaged.amount` reports clipped damage**, so overkill is invisible to event consumers. | [RM] `src/core/sim/Combat.cpp:855-856,890-897,927-946,983,1010`; [EXE] `C-060` | High | **Open** | (c) must be **sequenced after** shield collision exists: removing the point-path consult on the engine claim alone would make bubbles stop working against direct fire entirely, which is not retail behaviour end to end. `ADR-048` deliberately replaced retail's shield entities with a damage gate. |
| `C-112` | `WP-12`, `WP-16` | Audit of orders and construction. **Assist is a summed rate with the clamp applied to the sum** (`Assist.cpp:54`, `Economy.hpp:179`), where retail makes N independent calls each clamped separately in task order — so our result is order-*independent* where retail's is order-dependent, and a retail replay in which two engineers finish a structure on the same beat cannot be reproduced. **A second site advances the queue head**: `finishBuildOrder` pops from inside the economy write-back at the end of the beat, violating `C-096`'s single-site invariant. **`advanceOrders` can pop several commands in one beat** where retail does one per beat per unit, so a queue of N dead waypoints clears in 1 beat here and N in retail. A queued patrol is always **appended** rather than inserted before the smallest-clock entry, so a waypoint added to an already-rotating patrol splices into a different place. And the economy ratio is **current, not one beat stale**. | [RM] `Assist.cpp`, `Economy.{hpp,cpp}`, `Command.cpp:548-577`, `CommandQueue.cpp:90`; [EXE] `C-096`–`C-102` | High | **Open** | The assist divergence was *deliberate at the level of the sum* — the header argues against per-builder sequencing — but it was chosen against a guess about retail, and the guess is now refuted. |
| `C-150` | `WP-43`, `WP-12` | **The command stream is a 24-opcode message stream and `Moho::Sim` IS the `ICommandSink`.** Framing read at `0x006eabfa`–`0x006eac35` and written identically at `0x006ed15e`: `[u8 opcode][u16 totalLength LE]` then `totalLength − 3` payload bytes, with `CMessageStream` (vtable `0x00e52a1c`) constructed over the payload and `+0x04/+0x08/+0x0c` as begin/cur/end. `CDecoder::ProcessMessage` dispatches through a 24-entry jump table at `0x006eadfc`. **`Moho::Sim`'s primary vtable is the sink** — slots 0–23 at `0x00e841c4` — and `Moho::CMarshaller` (`0x00e7e2ac`) is the encoder implementing the same interface. Alphabet: `0 AdvanceBeat, 1 SetCommandSource, 2 CommandSourceTerminated, 3 VerifyChecksum, 4-6 RequestPause/Resume/SingleStep, 7 CreateUnit, 8 CreateProp, 9 DestroyEntity, 10 WarpEntity, 11 ProcessInfoPair, 12 IssueCommand, 13 IssueFactoryCommand, 14/15 Increase/DecreaseCommandCount, 16 SetCommandTarget, 17 SetCommandType, 18 SetCommandCells, 19 RemoveCommandFromUnitQueue, 20 ExecuteDebugCommand, 21 ExecuteLuaInSim, 22 LuaSimCallback, 23 EndGame`; opcode order ≠ slot order at the tail. | [EXE] `ART-E001` `0x006eadfc`, `0x00e841c4`, `0x006ed110`; `vtables.tsv` | High | Confirmed | **Independent cross-confirmation that this is the right interface:** `Sim::AdvanceBeat = 0x00751300` and `Sim::VerifyChecksum = 0x0074fb80` are exactly the addresses `C-055` and `C-057` reached by unrelated routes. And the encoder witnesses the decoder — `CMarshaller::IssueCommand` (`0x006ed110`) allocates with `pushl $0xc` and encodes EntIdSet → CommandData → clearQueue in that order. **`C-127`'s own caveat was vindicated**: `0x006eb250` and `0x006eb360` are *not* two copies of `DecodeIssueCommand` as the self-naming strings suggest — they emit the same string from a copy-paste in the original source, but call sink slots **11 and 12**, so the second is `DecodeIssueFactoryCommand`. Identity was established from behaviour, exactly as `C-127` warned it must be. |
| `C-151` | `WP-43`, `WP-12` | **One issued command, field by field, from `CDecoder::DecodeCommandData` `0x006eba90`.** Wire and destination: `+0x00 i32` command id (validated); `+0x04 i32` and `+0x08 i32`, ctor default `−1`, **semantics unresolved**; `+0x0C u32` command type read as **one byte, fatal if ≥ 40** — exactly `C-124`'s `UNITCOMMAND_` alphabet; `+0x10..+0x20` **and** `+0x24..+0x34` — **two Targets, not one**; `+0x38 i32` formation index (`−1` = none, gating the next two); `+0x3c..+0x48` formation orientation **quaternion** (ctor default `(1,0,0,0)`); `+0x4c float` formation scale; `+0x50 Blueprint*` from a string dictionary, fatal if unresolved; `+0x58..+0x6c` cell vector as `u32 count` then `count × {i16 x, i16 y}`; `+0x70`/`+0x74` two counts, both defaulting to 1; `+0x78` string `LuaParams`. **Target is a 20-byte tagged union** `{u32 type, u32 entityId, float x,y,z}` decoded at `0x006ebf30`: type 0 → none, 1 → entity, 2 → position, else fatal — **confirming `C-124`'s `STITARGET_` from behaviour** and giving `0xF0000000` as the null-entity sentinel. **Entity id is `u32` = 4-bit kind tag `|` 28-bit index**, proven by `DecodeEntIdSet` (`0x006eb910`) rejecting a set whose members differ in the top 4 bits (`shrl $0x1c`). | [EXE] `ART-E001` `0x006eba90`, `0x00559020` (ctor), `0x00843cd0` (UI dumper), `0x006ebf30`, `0x006eb910` | High for the named offsets | Confirmed | **Three independent witnesses agree** on `+0x00/+0x0C/+0x10/+0x24/+0x38/+0x3c/+0x4c/+0x50/+0x58/+0x70/+0x74/+0x78`: the decoder, the constructor `0x00559020`, and the UI dumper `0x00843cd0` which builds the Lua table `{Units, Blueprint, Target, LuaParams, CommandType, Clear}`. The formation triple is Medium-high — inferred from the setter `0x005590e0`, the identity-quaternion default, and Lua's `IssueFormMove(units, pos, formation, degrees)` signature. **A unit set is a dynamic BITSET, so `Sim::IssueCommand` applies to units in ascending entity-id order — never selection order.** That matters because `C-112` found an order-dependent rule (the assist clamp) where we are order-independent. |
| `C-152` | `WP-43`, `WP-04` | **Beats are gated by `mAvailableBeat` — everyone has acknowledged receipt — not by the queued-beat counters.** Four counters at two levels: per client (`CClientBase` `+0xA0` `mQueuedBeat`, `+0xA4` `mDispatchedBeat`, `+0xA8` `mAvailableBeatRemote`, `+0xB0` `mLatestAckReceived[]`) and per manager (`CClientManagerImpl` `+0x438` `mDispatchedBeat`, `+0x43C` `mAvailableBeat`, `+0x440` `mFullyQueuedBeat`, `+0x444` `mPartiallyQueuedBeat`). `CClientManagerImpl::Update` (`0x00545500`) advances each by a `while` over its predicate: partial = **at least one** live client queued through the beat (`0x005458f0`), full = **every** non-ejected client (`0x00545970`), available = every peer's `mLatestAckReceived[i] >= beat` (`0x005459e0`/`0x00542e90`), broadcasting opcode `0x34` when it moves. The blocking-status routine (`0x005460de`–`0x00546259`) returns "not blocked" iff **`mAvailableBeat − mDispatchedBeat > 0`**; the queued counters only name *which* client is stalling. **`AdvanceBeat`'s `u32` is a delta and retail always sends 1** — `CClientBase::DispatchBeat` does `mDispatchedBeat += payload` (`0x005432b2`) while `Sim::AdvanceBeat` clobbers `ebp` with `this` at `0x00751322` and therefore **cannot read its argument at all**. | [EXE] `ART-E001` `0x00545500`, `0x005458f0`, `0x00545970`, `0x005459e0`, `0x005460de`, `0x00751300` | High; the AdvanceBeat-ignores-its-argument point Medium-high (structural) | Confirmed | **Corrects a lead I passed to the agent**: `mFullyQueuedBeat`/`mPartiallyQueuedBeat` are **not** `CClientBase` fields — `0x005438a0` is `CClientBase`'s dump and `0x005463d0` is `CClientManagerImpl::Dump`, a different object, so the two counters are manager-level aggregates. Falsifiers stated: a `DispatchBeat` guarded on `mFullyQueuedBeat`, or any `[ebp+8]` read in `0x00751300`–`0x007519ce`. |
| `C-153` | `WP-43`, `WP-12` | **`Sim::ValidateNewCommandId` is `0x00750580` and enforces three rules — and command ids are NOT monotonic.** Identified by behaviour, not by name: exactly two callers, `Sim::IssueCommand` (`0x00750684`) and `Sim::IssueFactoryCommand` (`0x007508a4`), passing the literals `"IssueCommand"`/`"IssueFactoryCommand"` matching its own log strings. (1) `Sim+0x92C`, the current command source, must not be `0xFF`. (2) **`commandId >> 24` must equal the current command source** — so a command id is `[u8 source][u24 per-source counter]`. (3) The id must not already be live in the map at `Sim+0x988`. **The rule is source-tagged high byte plus uniqueness among LIVE commands; ids may be reused once a command leaves the map.** `Sim::SetCommandSource` (`0x0074fa10`) accepts `source < commandSourceCount` (records at `Sim+0x920`, **stride 36**, name at `+0x04`) or literal `0xFF`; anything else is forced to `0xFF`, after which every subsequent command fails rule 1. | [EXE] `ART-E001` `0x00750580`, `0x0074fa10`, `0x007505b2` | High | Confirmed | Also read in `Sim::IssueCommand` (`0x00750650`): per unit, the **command source must have its bit set in the army's `authorizedCommandSources` bitset** (`army+0x130/+0x138/+0x13c`) unless cheats are on, and **command type 31 `DestroySelf` is dropped entirely unless cheats**. We have no concept of a command source at all, which is the largest structural gap in this package. |
| `C-154` | `WP-43`, `WP-04` | **`Sim::GetBeatChecksum` is `0x0074F9C0` and confirms `C-057`'s ring exactly — but our checksum can never match retail's, and that is structural rather than numeric.** The function reads: reject if `beat` is older than 128 beats or not yet computed, else copy 16 bytes from **`Sim + 0xB0 + 16 × (beat & 0x7F)`**. `Sim::VerifyChecksum` (`0x0074FB80`) applies the identical bounds test, compares, and on mismatch appends to a desync list at `Sim+0x8B0` and zeroes `Sim+0x4C`. Its three refusal messages name the policy outright: no active command source → silently ignored; too old → *"we only have data for %d beats"*; future → *"because it is in the future"*. The outgoing side is **inlined** at `0x00744690`–`0x0074470f`, skips all-zero digests, and sends via sink slot 2 — so **`GetBeatChecksum` has zero direct callers in retail**. | [EXE] `ART-E001` `0x0074f9c0`, `0x0074fb80`, `0x00744690`, `0x0074fa70` | High | Confirmed | **The implementation consequence is the important part: retail's digest is a bounded-history change-set with an explicit too-old/future policy, where ours is a full-state hash. Even a perfectly matching simulation would produce different digests, so hash parity is NOT a usable oracle** without reimplementing `Sim::UpdateChecksum`'s exact feed order. Also read: `Sim::CommandSourceTerminated` (`0x0074fa70`) feeds the source id and the tick into the running MD5 at `Sim+0x50`, so **a player disconnecting is itself hashed into the state**. |
| `C-155` | `WP-43`, `WP-12`, `WP-04` | **Command application order within a beat is fully determined: ascending client index, then FIFO within a client, then exactly one `AdvanceBeat`; and within one `IssueCommand`, ascending entity id.** `CClientManagerImpl::DispatchBeat` (`0x00545AC0`, vtable slot 22, called from the session at `0x00744403`) locks, loops `i = 0..mClients.size()` calling each client's `DispatchBeat` into a **shared queue** at `mgr+0x18470`, broadcasts opcode `0x33`, increments `mDispatchedBeat`, then drains the shared queue FIFO dispatching each message to the per-opcode receiver table at `mgr+0x0C + op*4`, and finally synthesises `[opcode 0][len 3][u32 1]`. `CClientBase::DispatchBeat` (`0x00543000`) drains that client's `mPipe` until it hits an `AdvanceBeat`, and emits a `SetCommandSource` **only when the value differs from the last one emitted** — where "last" is **manager-scoped and persists across clients and across beats**. | [EXE] `ART-E001` `0x00545ac0`, `0x00543000`, `0x00542f60`, `0x00744403` | High | Confirmed, with **one stated loose end** | **That statefulness is a replay-format property, not just an ordering one**: a stateless "each command carries its source" model produces a different byte stream for identical play. An unauthorised source logs and sets `mCommandSource = 0xFF`, after which **every subsequent message from that client is dropped outright**, not merely rejected downstream. **The loose end the agent flagged rather than glossed is now CLOSED, and the ordering claim survives.** It warned that `sharedQueue`'s vtable slot 8, called between filling and draining at `0x00545bfd`, would change everything if it sorted. Followed up: the object is constructed at `0x009711c0` (called from `0x00544ab9`, which also initialises `mgr+0x184b8` to `0xFF` — the last-emitted source, confirming that reading too), and its constructor stores vtable `0x00d955c0`, which `vtables.tsv` identifies as **`gpg::PipeStream`**. Slot 8 is `0x009718b0`: it takes a mutex on `this+0x1c`, tests a byte flag at `+0x24`, and allocates a 0x28-byte node — buffer management, with no comparison and no swap anywhere. **A byte pipe cannot reorder messages: there is no element boundary to sort by, only a stream.** The structural argument is the stronger half — even without reading the body, `PipeStream` has nothing to sort. |
| `C-156` | `WP-27` | **Hunting-queue #8 answered: a weapon with an empty priority table acquires NOTHING, and there is no default seed anywhere.** In `CAiAttackerImpl::FindBestEnemy` (`0x005de4e0`), at `0x005de9dc` the candidate loop calls `vector::size()` on `weapon+0x150` and `jbe` skips the candidate entirely when it is zero; the result slot `[esp+0x28]` is initialised to 0 and written only on acceptance, so **an empty table rejects every candidate and the function returns NULL**. `0x006e2930` — the plan's suspected "default-priority seeder" — is **not one**: it is the reflection deserialiser for `std::vector<EntityCategory>`, reached only from the type descriptor initialiser `0x006e1da0`. The vector's only three writers image-wide are `UnitWeapon::SetTargetingPriorities` (`0x006e0fe0`, which **erases before pushing**, so an empty Lua table clears to empty), that deserialiser, and `CPlatoon::SetPrioritizedTargetList` (AI platoons, not weapons). Tier-0 closes it: `Weapon.lua:364` only calls the native setter `if bp.TargetPriorities`, and no shipped subclass overrides `SetWeaponPriorities`. | [EXE] `ART-E001` `0x005de4e0`, `0x005de9dc`, `0x006e0fe0`, `0x006e1da0`; [LUA-R] `Weapon.lua`; [BP-R] 568-blueprint census | High | Confirmed | **The falsifier was run, not just stated** — and this is the model of how to check a rule like this. Of 494 weapon entries across 568 blueprints, **157 have no `TargetPriorities`**, and every one is on a path that does not use priority acquisition: 99 `Death` weapons, 43 `Defense` weapons (all `RULEWTT_Projectile`, i.e. `C-095`'s separate algorithm), 10 explicit-order missiles, 4 `ManualFire`, and **one residual** — `URL0303`'s `EMP`, whose script defines it as an `EMPDeathWeapon` disabled at creation. Zero genuine counterexamples. **Implementation warning that follows: "empty ⇒ acquire nothing" is only survivable because death, manual-fire and anti-projectile weapons are on separate paths, so those paths must exist BEFORE priorities land, or ~157 of our equivalents go silent.** |
| `C-157` | `WP-27` | **Hunting-queue #7 fully closed: `IsTargetExempt` is called from `FindBestEnemy` at `0x005de78b`, it is reject step 11 of 14 — before range/arc classification — and the mystery constants 19/21 are `UNITCOMMAND_Reclaim` and `UNITCOMMAND_Capture`.** The call is a two-step `mov eax,[ecx]; mov edx,[eax+0x58]; push edi; call edx`, which is why no literal `call [reg+0x58]` existed to find (`C-119`). The scan was exhaustive: of 172 two-step `+0x58` sequences image-wide, four are on an `IAiAttacker`. **The full pre-scoring reject order is recovered** — null slot, null weak-deref, `Entity+0x99` dead, `Entity+0x1b9` queued-for-destruction, resolve-to-unit + visibility, 2-D squared distance, enemy-by-army, `BENIGN` category, `LAYER_Air` special cases, `0x006de340`, **`IsTargetExempt`**, state bit 34 `DoNotTarget`, playable rect, per-weapon restrict, **then** range/arc class, then score, then the priority row loop. So an exempt candidate is dropped **before it is classed or scored** and can neither become nor remain the incumbent. | [EXE] `ART-E001` `0x005de4e0`, `0x005de78b`, `0x005ddef3`, `0x006f4800`; `enum_registrations.tsv` | High | Confirmed | **The 19/21 identification was verified three ways rather than assumed from `C-124`'s table.** (1) The same field `[task+0x98]` is compared against 2, 7, 8, 15, 16, 17, 18, 22, 27, 28 elsewhere — all valid `UNITCOMMAND_` values in matching code regions — the clincher being `0x006f4800` testing **16 `Patrol` OR 18 `FormPatrol` as an adjacent pair**, which no other registered enum makes natural. (2) The function's second pass gates on `IsUnitState(27 = BeingCaptured)` and then enumerates the `"ENGINEER"` category. (3) **20 `Repair` is absent** — Reclaim and Capture are exactly the two engineer orders that consume an *enemy* target. In plain language: *"do not shoot what my own side is reclaiming or capturing."* We have no equivalent, so our units shoot the wreck their own engineer is reclaiming. Refines `C-129` (`weapon+0x150` is the vector object; begin `+0x154`, end `+0x158`, cap `+0x15C`; element = 40-byte `EntityCategory`) and `C-092` (the class penalty is `4.0`, **multiplied** on the distance path but **added** on the aim-direction path). |
| `C-164` | `WP-15`, `WP-06` | **`RUnitBlueprint+0x500` is `Economy.NaturalProducer` — and the engine ships its own one-line definition of it.** The blueprint schema stores each key next to its documentation, so the `.rdata` block reads in declaration order: `StorageEnergy` at `0x00e63820` (*"Energy storage capacity provided by this unit"*), `StorageMass` at `0x00e63860` (*"Mass storage capacity provided by this unit"*), then **`NaturalProducer` at `0x00e63898` — *"Produces resouce naturally and does not consume anything"*** (the typo is the engine's). Those three keys are consecutive and map to the three consecutive struct offsets `+0x4f8`, `+0x4fc`, `+0x500`, which the economy trace had already assigned to storage-energy cap, storage-mass cap, and the byte flag that exempts production from ratio scaling. | [EXE] `ART-E001` schema strings `0x00e63820`–`0x00e638b0`; behaviour at `0x006b1592` (`cmpb $0x0, 0x500(%eax)`) | High | Confirmed | **Three independent lines agree**: declaration order in the schema matches struct order; the read is a `cmpb`, i.e. a boolean, which is what a flag key must be; and the documented meaning — *does not consume anything* — is exactly the behaviour `C-161` read, since a unit that consumes nothing has no consumption ratio to be throttled by. This closes the one open item the economy trace left with a next address, and it cost one lookup because the binary documents its own blueprint schema. |
| `C-159` | `WP-15` | **The economy allocator is fully traced: `0x007790e0`, a `__stdcall` free function taking `CEconomy*`, called once per army per beat from `CArmyImpl` vtable slot 15 (`0x007067f0`).** `C-067`'s two-ratio scheme is **confirmed by reading**, with three refinements that change code. Pass 1 computes `out[k] = max(0, demand[k] − allocated[k])` per request; requests are bucketed by how many resources have **nonzero OUTSTANDING** — two → `T[]`, one or zero → `B[]`. Income is handicap-scaled (`CArmyImpl+0x1dc` gate, `+0x1e0` value), `S[k] = stored[k] + income[k]`, and then **`r1 = min(1.0, min_k S[k]/(T[k]+B[k]))` with `k*` the argmin**, `Rem[k] = max(0, S[k] − T[k]·r1)`, `r2 = min(1.0, min_{k≠k*} Rem[k]/B[k])`. A request is granted at `r1` if `out[k*] != 0`, else `r2`. **The three refinements:** (1) the r1 divisor is `T+B`, not `T` as `C-067` recorded; (2) the bucket key is outstanding, not demand, so a two-resource request already satisfied on its binding resource drops to the single bucket and is funded at `r2`; (3) **on an exact tie `k*` stays at index 0 = ENERGY**, because the `comiss/jbe` at `0x007792e4` updates only on strict `>`. Iteration is **FIFO in creation order** — `CEconRequest::Init` (`0x0077abc0`) inserts at the tail. | [EXE] `ART-E001` `0x007790e0`–`0x0077a30f` read instruction by instruction; `0x00706844`, `0x0077abc0` | High | Confirmed | **This is the decisive native path `WP-15` needed, and it refutes two envelope alternatives from the instructions rather than by argument**: `PE-11`'s "global proportional throttle" (there are two precomputed ratios) and "sequential deterministic consumers" (there is no supply drain inside the grant loop). **`CEconomyEvent`s share the same `CEconomy+0x58` list** — silo builds and enhancements compete at exactly the same r1/r2 and differ only in how they spend (`C-084`). Grants are order-independent, but the running `S[k] -= grant[k]` is not, so the final `stored` carries order-dependent float32 rounding. |
| `C-160` | `WP-15` | **`C-069` is INCOMPLETE as written: storage capacity is rounded TWICE, not once.** The truncation to `int64` in `CEconStorage::Apply` is real (`__ftol2` at `0x0077a87a`, truncating toward zero, then `__allmul` by ±1 into `CEconomy+0x40+8i`) — but that `uint64` is **converted back to binary32 on every read**, via x87 `fildll` + sign-bit split + `fstps` at `0x00779679`, `0x00779852`, `0x00779975` and `0x00779a28`. So the cap the engine actually compares and clamps against is a `float`, quantised above 2²⁴. Everything else in the allocator is SSE scalar single — 16 `mulss`/`divss`, **zero `cvttss2si`**, and one `cvtsi2ssl` for the ally count. `0x006b1550` contains no conversion instructions at all. | [EXE] `ART-E001` `0x0077a850`, `0x0077a87a`, and the four `fstps` sites | High | Confirmed; **supersedes `C-069`'s "the only rounded quantity"** | The phrase should read *"the only INTEGER quantity, which is then re-rounded to binary32 on every read"*. **Implementation consequence, stated plainly: this is unreachable for us.** Recoil Metal is fixed-point (`Fx`/`Mag`) throughout, so bit-exact retail parity here is impossible by construction. The reachable target is `C-104`(e) — truncate storage capacity per contributing structure and nothing else. |
| `C-161` | `WP-15` | **There is no priority between upkeep and construction because they are LITERALLY THE SAME NUMBER — the engine never sees two components.** `Unit::SetConsumptionPerSecondEnergy` (`0x006d08a0`) clamps negatives to zero and writes one field; `SetConsumptionActive` (`0x006b1390`) turns it into `demand[k] = consumptionPerSecond[k] × 0.1f` in the unit's **single** `CEconRequest`. `lua/sim/Unit.lua:700-752` computes `energy_rate = buildEnergy/time + maintenanceEnergy` and pushes that **sum**. So `C-103` is confirmed and reinforced from the Lua side. **Two behaviours a reimplementation would not guess.** (a) **Production is throttled by the consumption ratio natively** — `0x006b1769`–`0x006b17c6` computes `produced[k] = productionPerSecond[k] × ratio × 0.1f` unless `RUnitBlueprint+0x500` is set, so an energy-starved mass fabricator produces proportionally less in the *engine*, not merely in `defaultunits.lua`. (b) **`Unit+0x53c` is zeroed first** (`0x006b1577`) and written only when `!IsDead && consumptionActive && request != NULL`, so **`GetResourceConsumed()` returns 0, not 1**, for a unit with consumption inactive — and `shield.lua:288` and `RemoteViewing.lua:137` use it as a raw rate multiplier. | [EXE] `ART-E001` `0x006d08a0`, `0x006b1390`, `0x006b1550`, `0x006b1577`, `0x006b1769`; [LUA-R] `Unit.lua:700-752` | High | Confirmed | Refines `C-071`. **What Recoil Metal must do: make upkeep a request line summed into the same per-unit demand, delete the pre-subtraction lump at `Economy.cpp:66-69`, and gate shield and intel work on the resulting per-unit ratio.** **The open item is now closed: `RUnitBlueprint+0x500` is `Economy.NaturalProducer`** (`C-164`). |
| `C-162` | `WP-15`, `WP-16` | **The one-beat staleness `C-100` found is a property of the CONSUMER's cache, not of the allocator — and two more carry-forward effects come with it.** The allocator is not stale: it reads demand and allocated live and recomputes both ratios every beat. The staleness is `Unit+0x53c`, **written in the LAST stage of beat N** (motion, `Sim+0x930`, at `0x006b15e3`) and **read in the FIRST stage of beat N+1** (command dispatch, `Sim+0x958`, at `0x005fc76e`) — so a builder applies the previous beat's funding fraction. The cache exists for a reason worth reproducing: `Consume` (`0x0077ac30`) drains `allocated` immediately after `GetRatio()` is read, so any later in-beat read would return ~0. **Two consequences of that drain.** (a) `want[k] = demand[k]·ratio ≤ allocated[k]`, so the binding resource lands at exactly 0 but the non-binding one keeps a residue — **unspent allocation carries forward** and next beat's outstanding is smaller. (b) **Income is one beat delayed too**: `CEconomy+0x08/+0x0c` are filled by MotionTick in the last stage and consumed and zeroed by the allocator at the start of the next (`0x0077a2ad`). | [EXE] `ART-E001` `0x006b15e3`, `0x005fc76e`, `0x0077ac30`, `0x0077a2ad` | High | Confirmed | **Depends on `C-142`'s stage order and independently corroborates it** — the write is in the stage that runs last and the read in the stage that runs first, which only makes sense under CommandDispatch → Script → Motion. Verified that nothing between the beat start and the allocator touches the economy. Recoil Metal must either reproduce the cache (write at end of tick, read at start of next) or accept a documented one-tick divergence in build progress. |
| `C-163` | `WP-15` | **`C-104`(a) confirmed by direct read, and ally sharing is NOT the flat 1/n `C-070` recorded.** Order in `0x007790e0` is: `S = stored + income` (`0x00779267`) → grants subtract from `S` → `excess[k] = max(0, S[k] − (float)cap[k])` (`0x00779640`) → optional sharing (gated on `CEconomy+0x54`) → **finally** `stored[k] = max(0, min(S[k], (float)cap[k]))` at `0x0077992c`. **So a full-storage army CAN spend the tick's income**, which our clamp at `Economy.cpp:38-39` prevents. Sharing is **progressive, not flat**: per recipient `b` of `n`, `share = excess/remaining` with `remaining` counting down, capped by that ally's headroom, then `excess -= share`. And **the share is credited to the recipient's INCOME accumulator (`+0x08`/`+0x10`), not to its `stored`** — so it arrives as the ally's income next beat. Published stats, all from this function: `requested = T+B`, `usage = T·r1 + B·r2`, `income` = the **raw, un-handicapped** accumulator, `Trend = (income − usage) × 10.0f`, `Ratio = stored / (float)cap`. | [EXE] `ART-E001` `0x00779267`, `0x00779640`, `0x007797c0`, `0x00779850`, `0x0077992c`, `0x00779501`, `0x007595b3` | High | Confirmed | Confirms `C-104`(a) and (b) and `C-071`; **refines `C-070`(c)** from "shares the excess `1/n`" to the progressive scheme above. Note the asymmetry the stats expose: the income the army actually *spends* is handicap-scaled but the income it *reports* is not — so an AI reading `GetEconomyIncome` on a handicapped army reads a number the allocator never used. Also refines `C-070`(a): on deactivation the refund goes to the income accumulator and the request's `allocated[]` is **not** zeroed (only `demand` is), so a re-activated consumer starts with a pre-credited balance. |
| `C-158` | `WP-27`, `WP-33` | **Category priorities apply ONLY to a candidate the attacker's army has ever had line of sight on. The bit is `RECON_LOSEver`, now read rather than guessed.** At `0x005de992`–`0x005de9b7` in `FindBestEnemy`: take the attacker's owning `Unit*` from `this+0x40`, its army from `Unit+0x154`, the army index from `army+0x08`, multiply by `0x34`, index the candidate's per-army array at `candidate+0x4C4`, read the dword at `+4`, and `shrl $0x4; andb $0x1` — **bit index 4, i.e. mask `0x10`, which is `RECON_LOSEver` in `C-124`'s recovered table**. The matched priority row is stored to `[esp+0x38]` only when that bit is set; otherwise the candidate keeps the sentinel `0x270f` (9999) and **competes at the worst priority rank, on score alone**. | [EXE] `ART-E001` `0x005de992`–`0x005de9b7`; `enum_registrations.tsv` `RECON_` | High | Confirmed | The `WP-27` agent found this latch and explicitly recorded it as *"plausibly `RECON_LOSEver` — unverified"*, giving the next address. Followed up and the guess is right; it is now read. **Behavioural consequence: a radar-only contact is never matched against `TargetPriorities` at all**, so a unit will not preferentially engage a high-priority target it has only ever seen on radar — priorities are a visual-identification mechanic. Combined with `C-156` (empty table ⇒ acquire nothing) this is the second place where our "priorities are a preference" model is structurally wrong. **One caution recorded rather than smoothed over:** this per-army stride-`0x34` array sits at `+0x4C4` on a `Unit` here and `C-133` found the same shape at `+0x4C4` on a `ReconBlip` — but `sizeof(Entity)` is `0x270` (`C-144`), so it **cannot** be an inherited `Entity` field. Either two classes coincide in layout or there is a common intel-bearing base not yet identified; do not assume they are one field. |
| `C-142` | `WP-04`, `WP-12` | **`WP-04`'s open question is answered: the per-tick stage order is CommandDispatch → Script → Motion, which is the REVERSE of the declaration order.** The per-tick driver is the function containing `0x007513a3`, which increments `Sim+0x900` (the tick counter, `C-055`), and at `0x007514ca`/`0x007514d5`/`0x007514e0` it calls `0x00409ac0` with `ecx` = `Sim+0x958`, then `Sim+0x944`, then `Sim+0x930` — CommandDispatch, Script, Motion per `C-054`. **`0x00409ac0` was verified to be `CTaskStage::Tick` rather than assumed**: it walks the stage's intrusive task list from `[stage+0x04]`, calls `0x004091f0` per task, and compares the result against `-2` and `-1` — exactly the `ETaskStatus` values `C-118` independently found `TaskTick` returning — unlinking and freeing the task on `-1`. | [EXE] `ART-E001` `0x007513a3`, `0x007514ca`–`0x007514e6`, `0x00409ac0`, `0x004091f0` | High | Confirmed | **The reversal is the whole point, and it is a trap for anyone reading member order.** Declaration order is Motion, Script, CommandDispatch — that is what `Sim`'s constructor and its serializer at `0x0075c321` both use (930, 944, 958), and assuming the tick followed suit would invert the entire beat. Executing commands *first* means an order issued on a tick reaches motion within the **same** tick, not the next one. Two other orderings appear in the binary and are neither the tick nor the declaration: a destructor at `0x0074cc66` (944, 930, 958) and construction at `0x0074a95b` (930, 944, 958). |
| `C-143` | `WP-31`, `WP-30` | **The shield rewrite shipped in `C-110` is CONFIRMED on its central question — all covering bubbles stack, charged per target against the target's own position, each bubble paying once.** Read from `0x0073d8c0`, the per-entity absorption applier: it walks the whole shield list with `mov esi,[esi]` and **no early exit**, each node subtracting its budget at `[esi+0xc]`; coverage is tested by calling the shield's collision primitive with **`lea edx,[ebx+0xac]` — the TARGET's own `Entity+0xAC` position**; and after the target loop `0x0073e100` at `0x0073e3d7` walks the same list calling `DealDamage` once per shield. **Three divergences remain, ranked.** (a) **A blast whose centre is inside a dome is not absorbed by that dome at all** — `0x0073d9dc`–`0x0073da38` fetches the shield's cached bounding sphere, shrinks the radius by `0.1f`, and if the blast centre is inside, **skips that shield entirely**; we have no such rule, so we shelter units from shots fired from under their own dome. (b) **Our `bubble.used` flag should not exist** — retail admits a shield on shield-shape ∩ blast-sphere alone (`0x0073daa0`) and charges every admitted shield whether or not it sheltered anyone, so artillery landing on an empty shielded pad drains nothing for us and drains retail. (c) **Coverage is a collision-shape containment test, not a sphere test** — `shield.lua:264` sets `'Sphere'` for bubbles but `:477` sets **`'Box'`** for `UnitShield`, so our sphere-only test is wrong for every personal shield. | [EXE] `ART-E001` `0x0073d8c0`, `0x0073d930`, `0x0073e100`, `0x0073e3d7`, `0x00a880b0`; [LUA-R] `shield.lua` | High | (a)–(c) **Open**; the stacking question **Confirmed** | The polarity of (a) was checked rather than assumed — `0x00a880b0` reads `fld dist; fld radius; fcompp; test ah,1`, returning true iff `radius ≥ dist`, so it is genuinely "inside". Also read: allied shields are skipped via a per-army bit vector at `CArmyImpl+0xE8` unless `DamageInfo+0x7C` is set, and **`0x0073d8c0` has exactly two callers, so the kind-0 point path never consults shields** — re-confirming `C-060` and bearing directly on `C-111`(c). One quantity we get wrong: absorption uses **the owner unit's armour multiplier for the damage type** (`shield.lua:100-108`), not a "Shield" armour class as our `damage.against(classFor("Shield"))` assumes. |
| `C-144` | `WP-31`, `WP-32` | **`Moho::Shield` adds ZERO data members over `Moho::Entity`, and shield strength is stored in the same two fields as hull health.** `_c_CreateShield` (`0x0077e030`) allocates `0x270` bytes at `0x0077e12a` (and identically at `0x0077de2c`), and **`sizeof(Entity) = 0x270` established three independent ways** from `rtti.bases.tsv`: `InstanceCounter<Unit>` at `Unit+0x278` with `Entity` at `Unit+0x08`; `Projectile`'s `ManyToOneBroadcaster` at `+0x270` with `Entity` at `+0x00`; `Shield`'s `InstanceCounter<Shield>` at `+0x270`. The constructor `0x0077da20` writes the `Shield` vtable `0x00e86adc`, its `CTask` vtable at `+0x34`, calls `Entity::Entity` with layer flags `0x800`, and links into the world shield list at **`Sim+0xa3c`** — and **reads no blueprint key at all, not one `fstps`**. Therefore current and maximum shield strength are **`Entity+0x90`/`+0x94`, `float32`, the very fields `C-053` identified as hull health**, corroborated Tier-0 by `shield.lua:36-37` `SetMaxHealth`/`SetHealth` and `:164` `GetHealth() <= 0`. `Moho::Unit+0x298` (`C-131`) is a display-only mirror written by `SetShieldRatio`. | [EXE] `ART-E001` `0x0077e030`, `0x0077e12a`, `0x0077da20`; `rtti.bases.tsv`; [LUA-R] `shield.lua` | High | Confirmed | **Everything else about a shield is a Lua table field with no native backing** — `RegenRate`, `RegenStartTime`, `ShieldRechargeTime`, `Size`, `OffHealth`, `PassOverkillDamage`. That reframes `WP-32`: shield *variants* are Lua constructs over one native entity, not native subclasses. `shield.lua` allows **exactly one shield per unit** (`self.MyShield` is a scalar and every constructor calls `DestroyShield()` first), so nothing iterates a per-unit shield collection. |
| `C-145` | `WP-31` | **Shield regeneration and recharge are entirely Lua, in SECONDS, and recharge blocks absorption by removing the collision shape.** No native timer exists. `shield.lua:181` waits `RegenStartTime` **seconds**; `:182-189` applies `RegenRate` flat **per second**, not per tick; `:160-163` any damage kills the regen thread. Depleted recharge (`:383-394`) runs `ChargingUp(0, ShieldRechargeTime)` then `SetHealth(max)` — **a full restore, never partial**. Brownout recharge (`:398-411`) uses `ShieldEnergyDrainRechargeTime`. Charge progress (`:286-297`) accumulates `GetResourceConsumed()/10` per `WaitTicks(1)`, so the quoted time holds **only at full power and a brownout stretches it**. Both recharge states begin with `RemoveShield()` → `SetCollisionShape('None')`, and `0x0073d930` skips a shield with an absent primitive at `0x0073d97a`/`0x0073d988` **silently**. | [LUA-R] `ART-S007` `shield.lua`; [EXE] `0x0073d930` | High | Confirmed | The diagnostic *"invalid shield or missing collision primitive!"* (`0x00e82644`) fires only in `0x0073d8c0`/`0x0073d860`, which operate on the **already-filtered** list — it is a consistency check, not the hot-path gate. A shipped inconsistency worth carrying: `Unit.lua` defaults both recharge times to `10` while `shield.lua:42` falls back to `5`. Shield energy drain is not a shield number at all — it is the unit's ordinary `Economy.MaintenanceConsumptionPerSecondEnergy`. |
| `C-146` | `WP-18` | **Wreck reclaim values are Lua table fields, not native ones — which is why no sweep could find them — and they scale by `GetFractionComplete()`.** `Moho::Prop` inherits all of `Entity` and its own members start only at `+0x27C` (bounded-prop bookkeeping). `Unit.lua:1090-1146` computes the wreck: `mass = BuildCostMass × (Wreckage.MassMult or 0)`, likewise energy, `time = Wreckage.ReclaimTimeMultiplier or 1`; `SetMaxReclaimValues` takes the **unscaled** baseline, then `SetReclaimValues` takes `(value − value×overkillRatio) × GetFractionComplete()` for mass and energy — **never for time, and never for the `Max` baseline**. Overkill above `1.0` vaporises the wreck (`:1079-1081`), gated first by `Wreckage.WreckageLayers[currentLayer]`. | [LUA-R] `ART-S007` `Unit.lua`, `Prop.lua`, `wreckage.lua`; [EXE] blueprint key strings `0x00e60ab0`/`0x00e60ae0` | High | Confirmed | **Two shipped quirks that a faithful implementation must reproduce rather than fix.** `overkillRatio or 1` means a **nil** overkill yields a **zero-value** wreck. And `wreckage.lua:24-33` later re-derives value from `MaxMassReclaim × healthRatio`, so **the first scratch on such a wreck restores its value**. Damaging a wreck scales mass, energy and both time multipliers linearly by `health/maxHealth`; health ≤ 0 destroys it with no residual. |
| `C-147` | `WP-18`, `WP-17` | **The reclaim rate is read end to end, and the wreck's HEALTH is the progress bar.** `CUnitReclaimTask::TaskTick` `0x00625ad0` (`task+0x1c` engineer, `task+0x24` state via a 5-way jump table at `0x00626bc0`, `task+0x44` target weak-ref) calls Lua **`GetReclaimCosts`** on the target at `0x0062626c`, requires **exactly 3** return values, sets unit-state **bit 28 `Reclaiming`**, converts value 0 to ticks with `× 10.0` and clamps to a minimum of 1 tick, then computes `rate = target->Entity+0x94 (MAX health) / ticks`, adds the target's regen (`bp[+0x5c] × 0.1`) so the drain must out-pace it, and calls `Entity::AdjustHealth(0, −rate)` through vtable `+0x78`, setting **bit 15 `BeingReclaimed`** on the target. When `Entity+0x90` (current health) falls below the drain, the task completes. **The two halves close exactly:** `Prop.lua:153-162` returns `time/10` where `time = max(ReclaimTimeMassMult × mass/buildRate, ReclaimTimeEnergyMult × energy/buildRate)`, and the engine multiplies by `10.0` — **inverses, so the native budget is `max(mtime, etime)` in ticks.** | [EXE] `ART-E001` `0x00625ad0`, `0x0062626c`, `0x006262bb`, `0x0062631b`, `0x006263d4`, `0x00626410`; [LUA-R] `Prop.lua`, `Unit.lua` | High | Confirmed | **Bits 28 and 15 are read as literal `orl` immediates and match `C-123`'s table exactly** — `28 Reclaiming`, `15 BeingReclaimed` — a third independent confirmation of the state-mask numbering after `C-131`. **One gap stated honestly: the mass/energy CREDIT was not located.** Values 2 and 3 of the triple are consumed somewhere unreached. Next addresses in order: `0x00626600`–`0x006268d0`, then `0x00627230` (called at `0x00626491` with `ecx = targetUnit+8`), then `Sim+0x8d8` at `0x006264c6`. The stat keys `Economy_Reclaimed_Mass`/`_Energy` have **zero code xrefs** — reached by table, not by `push imm32` — so that lead is dead. |
| `C-148` | `WP-18`, `WP-16` | **Rebuild matching resolved: `prop.AssociatedBP` ∈ `newBlueprint.RebuildBonusIds`, case-insensitive, on the NEAREST `RECLAIMABLE` prop inside the new building's footprint — and the bonus is a head start on build progress, not a mass refund.** `LookForStructureRebuilder` `0x005fd970` parses the `"RECLAIMABLE"` category to a bit vector, builds a rect from blueprint footprint bytes `bp[0xd8]`/`bp[0xd9]`, queries the grid with layer mask **`0x200`** (props — consistent with `C-137`'s bucket B), keeps the nearest candidate by squared distance, reads the winner's Lua string field `"AssociatedBP"` and `stricmp`s it against every entry of the 28-byte `std::string` vector at `bp+0x518..0x51c`, then calls Lua `unit:GetRebuildBonus(bp)` requiring exactly one return value. It stores `task+0x5C = wreck's Entity+0xD8 × bonus` and a weak ref to the wreck at `task+0xC0`. `Entity+0xD8` is fraction complete (`C-133`). | [EXE] `ART-E001` `0x005fd970`, `0x005fdb82`, `0x005fdc86`, `0x005fdea7`; [LUA-R] `Unit.lua:2723-2726`, `:1137` | High | Confirmed | `GetRebuildBonus` is a hardcoded `return 0.5` with the parameter ignored, and its own comment says *"everything re-built is 50% complete to begin with"* — so the bonus is **build progress scaled by how complete the wreck's originating unit was**. The engine's own blueprint doc string for `RebuildBonusIds`: *"You will get bonus if you rebuild this unit over the wreckage of these wreckages"*. Falsifier stated: what `task+0x5C` feeds into on the build-task tick was not opened. |
| `C-149` | `WP-01` | **`C-132`'s inferred note is settled by reading, not inference: `Moho::Prop`, `Moho::Shield` and `Moho::Projectile` embed `Entity` at `+0x00`; only `Moho::Unit` places it at `+0x08`.** From `rtti.bases.tsv` directly, and corroborated in code — the `Shield` constructor writes its `CTask` vtable at `[this+0x34]`, matching `Shield → CTask @ 0x34` where `Unit`'s is `0x3c`. | [EXE] `rtti.bases.tsv`; `0x0077da20` | High | Confirmed | `C-132` recorded this as resting on the single coincidence that `Prop+0x148` and `Projectile+0x148` both resolve to `Sim*`, and explicitly flagged it as inferred. It is now read. **Every `Prop` offset recorded under that assumption stands.** |
| `C-137` | `WP-26` | **The broad phase is identified: `Moho::CollisionDB<Moho::Entity>`, a flat uniform grid of 4-world-unit cells with three intrusive singly-linked bucket arrays.** Not a tree, not a sorted list. The name is read twice over, not guessed: the DLL export `??0?$CollisionShape@VEntity@Moho@@@Moho@@QAE@PAV?$CollisionDB@VEntity@Moho@@@1@I@Z` declares `CollisionShape<Entity>(CollisionDB<Entity>*, unsigned)`, matching the record constructor at `0x00503ee0` byte for byte; and `rtti.bases.tsv` shows `Entity` inherits `CollisionDBRect → CollisionShapeBase → CollisionShape<Entity>` all at `+0x4C` (and `Unit` at `+0x54` = `0x08+0x4C`, consistent with `C-053`). It lives at **`COGrid+0x04`**, read verbatim from all three `Entity` constructors at `0x0067e49e`: `mov eax,[ebp+0x908]` (the `Sim`'s `COGrid*`), `add eax,4`, `mov [esi+0x54],eax`. `CollisionDB` layout: `+0x00` `cellsX` = `mapSizeX>>2` (also the row stride), `+0x04` `cellsZ`, `+0x08` `cellsX*cellsZ − 1` used as an **AND mask** so the cell count is power-of-two, `+0x0c` `shift = bsr(cellsX)`, `+0x10`/`+0x14`/`+0x18` bucket arrays A/B/C, `+0x1c`/`+0x20` free-node LIFO and count, `+0x24..0x30` a vector of 64 KB node blocks (8192 × 8-byte nodes). Node = `{next, CollisionDBRect*}`. `index = ((z << shift) + x) & [db+0x08]`. `CollisionDBRect` at `Entity+0x4C`: `u16 cellX0, cellZ0, cellCountX, cellCountZ`, `CollisionDB*` at `+0x54`, **dedup flag** at `+0x58`, **kind flags** at `+0x5C`. | [EXE] `ART-E001` `0x00503820`, `0x00503990`, `0x00503ee0`, `0x0067e49e`; [ART-D001] export as naming only, per `C-075` | High | Confirmed | **This resolves the plan's own "weakest link in the map".** It also dissolves the apparent contradiction between `C-064` and the old dashboard row: `COGrid` holds **both** — the entity broad phase at `+0x04` *and* the movement occupancy bitmaps at `+0x38`/`+0x48`/`+0x58`. They were describing different members of one object. Bucket assignment read at each subclass's ctor: **A (`0x100`) = `Unit` AND `ReconBlip`**, B (`0x200`) = `Prop`, C (`0x400`/`0x800`) = `Projectile`, `Shield`, collision beams, plain `Entity`. Insert (`0x00503a20`) picks **exactly one** array, priority `0x100 → 0x200 → 0xC00`, and does nothing if `flags & 0xF00 == 0`. |
| `C-138` | `WP-26`, `WP-27`, `WP-30` | **The query iteration order — the determinism input `C-090` and `C-061` both depend on — read end to end from `0x00503b00`.** Rect query emits: **cell row-major (z ascending outer, x ascending inner) → bucket A → B → C → within each, chain head to tail.** Base cell `(rect.z0 << shift) + rect.x0` at `0x00503b2b`, `+= cellsX` per row at `0x00503cbf`, `++x` at `0x00503cb1`, row extents clamped to `min(count, cells − start)`. The three bucket blocks are at `0x00503bb4`/`0x00503c08`/`0x00503c58` in that fixed source order. Chain walk is `edi = [edi]`, head first, and **head is most-recently-linked**. The wrapper `0x00729870` converts `CollisionDBRect*` to `Entity*` with `add edi,-0x4c` in an index-preserving loop. **Nothing sorts.** Dedup: `0x00503bd3` tests the record's `+0x0c` flag, sets it and appends, else skips; a trailing pass at `0x00503cd1`–`0x00503cf0` clears it over the collected results — so it is a **transient per-query mark owned by the query**, read by nobody else. Net rule: a multi-cell entity appears **once, at its first visited cell** (lowest z, then lowest x), at its chain position within it. | [EXE] `ART-E001` `0x00503b00`–`0x00503cf5` decoded branch by branch, `0x00729870` | High | Confirmed | **The plan's "find the case that should differ" discipline was applied and the case DOES differ**, which is what makes the rect-query reading trustworthy: the segment/ray query at `0x00503d00` is a deliberate second implementation that walks **C first, then A** — the opposite order — **never touches bucket B at all** (props are unreachable by ray), and uses a **hardcoded `0xD00`** mask rather than a parameter. Cell scale confirmed independently there by the constant `0x00ea2ab8` = `0.25f`. |
| `C-139` | `WP-26`, `WP-43` | **Query order is history-dependent but has no swap-and-pop, and relinking is conditional.** Insert (`0x00503a20`) is **head-insert** — `node->next = array[cell]; array[cell] = node` — so newest is first. Remove (`0x00503a90`) is a linear scan by record pointer and an unlink, so **surviving nodes keep their relative order** (a critical difference from a swap-and-pop vector). `Entity::UpdateCollision` (vtable `+0x84`; `Entity` `0x0067fc60`, `Unit` override `0x006b1fb0`) calls `Record::Update` at `0x00503ff0`, which quantises the bounds and **only relinks if one of the four `u16` cell values changed** — `UnlinkAll` (`0x00503f90`) then `LinkAll` (`0x00503f20`). So a unit moving *within* its current cell footprint keeps its chain position everywhere; the frame it crosses a boundary it jumps to the **front** of every cell it now occupies. Order is a pure function of the spawn / cell-crossing / death event sequence — identical across lockstep clients, therefore deterministic in retail, but **not reconstructible from entity id, spawn index, or distance**. | [EXE] `ART-E001` `0x00503a20`, `0x00503a90`, `0x00503ff0`, `0x00503f20`, `0x00503f90`, `0x006b1fb0` | High | Confirmed | Two extras. **`Unit::UpdateCollision` uses the explicit-rect variant `0x00504090`, building the rect from blueprint fields `+0x27c/0x280/0x284/0x288` scaled by `0.5f` — structures occupy their SKIRT/FOOTPRINT rect, not their collision primitive's bounds**, which also bears on `WP-19` and `C-073`(a). And `LinkAll` masks the cell index with `& (cellCount−1)` and **never clamps**, so an entity whose rect runs past the east edge **aliases into the next row** (Medium, inferred from the missing clamp; falsifier stated: place an entity at x > mapSizeX and query at (0, z+1)). |
| `C-140` | `WP-26`, `WP-27` | **Layer filtering is asymmetric between buckets, and the asymmetry has a direct consequence for target acquisition.** Read at `0x00503b84`–`0x00503ba1`, hoisted once per row: **buckets A and B are pre-filtered wholesale** — `mask & 0x100` / `mask & 0x200` gates whether the array is walked at all, and **individual records in A and B are never re-tested**. **Bucket C is pre-filtered to enter (`mask & 0xC00`) and then genuinely post-filtered per candidate** at `0x00503c77` (`test [record+0x10], fullMask`), because C is shared — a `0x500` query walks C but keeps only projectiles, dropping beams and shields. | [EXE] `ART-E001` `0x00503b84`–`0x00503ba1`, `0x00503c77` | High | Confirmed | **Consequence, and it reframes an earlier reading:** since `ReconBlip` shares bucket A with `Unit` (`C-137`) and A is never per-record filtered, a mask-`0xD00` query (projectile `CheckCollision`, `C-086`) or `0xF00` (`DealDamage`) **returns `ReconBlip`s alongside `Unit`s**. Retail rejects them downstream. So `C-090`'s "resolves to a unit" filter in the acquisition survey **is not defensive coding — it is load-bearing**, and an implementation that omits it will acquire ghost radar contacts. |
| `C-141` | `WP-26`, `WP-30` | **`C-120`'s guess about `CColPrimitiveBase` slot `+0x1c` is REFUTED — it is strictly more expensive than `+0x20`, not cheaper.** The pair is `Test`-with-approximate-contact versus `Find`-with-exact-contact, same signature `bool(const Wm3primitive&, CollisionResult*)`, same output contract. `+0x20` (sphere `0x005052a0`) is hand-rolled SSE: `|Δc|²` vs `(r₁+r₂)²`, then normalise Δ via `0x0044f7e0` to get the contact normal and `(r₁+r₂) − |Δ|` as penetration depth. `+0x1c` (sphere `0x00505360`) constructs a `Wm3::IntrSphere3Sphere3` (`0x00a80b10`), calls `Test()` (`0x00a80b40`) for the same boolean, then calls **`Find(tmax, ZERO, ZERO)`** (`0x00a82a00`, with `0x00f97234` verified as `Vector3::ZERO`), retrieves the exact contact point (`0x00a80aa0`) and recomputes depth with a real `sqrt` — carrying SEH frames and a Wm3 object lifetime. Slot `+0x00` is `GetBounds` (`0x005064a0`/`0x00506720`), called by `Record::Update`. **`CollisionResult` layout recovered:** `+0x00` `Entity*` (written by the `+0x18` adapter), `+0x04` `Entity*`, `+0x08..0x10` `float3` normal, `+0x14` `float` depth — the name confirmed by the export `?Intersects@Entity@Moho@@QAE_NPAVCColPrimitiveBase@2@PAUCollisionResult@2@@Z`. | [EXE] `ART-E001` `0x005052a0`, `0x00505360`, `0x00505c50`, `0x00505d60`, `0x005064a0`, `0x00681440`/`0x00681460`/`0x00681480` | High for the algebra and contract; Medium for the `Test`/`Find` naming | Confirmed; **supersedes `C-120`'s speculation** | The three `Entity` adapters are `0x00681440` (`+0x20`), `0x00681460` (`+0x1c`), `0x00681480` (`+0x18`). **Only `0x00681460` has direct callers** — `0x0061caf0` and `0x00727dc0`, the latter being build placement, which constructs an oriented `Box3` from footprint bytes via `0x00474170` and tests per candidate. One open discrepancy, honestly flagged: the export declares the argument as `CColPrimitiveBase*`, but the sphere implementation reads offset `+0x00` as a **float**, which cannot be a vtable pointer — so either callers pass `primitive+4` or these adapters are different overloads. Resolve by decoding `0x00727dc0`'s frame at `esp+0x64` vs `esp+0x17c`. |
| `C-136` | `WP-01`, `WP-04` | **The globals survey that `WP-01` was waiting on: the simulation's mutable global surface is 282 addresses, and only 11 are shared by three or more frontier functions.** Method: disassemble `.text` once, parse real operands, and keep only those landing in a data section, restricted to the measured 629-function depth-1 simulation frontier. Result: **691 distinct globals referenced, 41 shared by 3+, and of the mutable `.data` ones, 282 reachable with just 11 shared.** Separately, a whole-binary scan for the MSVC global-accessor idiom `B8 <imm32> C3` (`mov eax, <global>; ret`) finds **1,881 accessors exposing 1,459 distinct globals** — the engine's deliberately-exposed global surface, most of it reflection type-info singletons. | [EXE] `tools/re` scan over `objdump` output; `build/re-fa/exports/sim_globals.tsv`, `global_accessors.tsv` | High | Confirmed | **The number is the finding, and it is good news for determinism**: the sim path keeps almost no shared mutable global state — state lives on objects, which is consistent with `C-060` putting the owning `Sim*` at `Entity+0x148` rather than in a global. **No dominant singleton appears in the sim frontier** (the top is 7 references), so there is no global `Sim` pointer on that path. Two concrete leads left open: `0x01107480` is a genuine singleton — 234 references image-wide, **only 3 writers**, all inside `0x0089259b`–`0x0089d067`, and read-only from the sim frontier; and `0x011292ec`/`f0`/`f4`, which looked like a **global float3 scratch** written from 70 sites — the shape that would carry state between calls — **turns out on reading it to be benign, and was checked rather than left flagged.** `0x004725c0` is a lazy-initialisation cache: `0x01129428` is a one-time-init bitmask tested with `testb %cl, flag; jne skip`, `0x011292f8` a second guard, and the floats are filled once from the constant at `0x0110ab0c` and thereafter only read. Deterministic by construction — no cross-call state, no determinism hazard. **A first version of this survey was discarded rather than reported:** an unaligned byte-scan returned 1,328 globals, but its top hits were an artifact family (`0x01xx2444`) produced by `44 24`, the SIB byte for `[esp+disp]`, appearing inside unrelated instructions. Parsing real operands removed that entire class and halved the count. |
| `C-135` | `WP-17`, `WP-12` | **`Listener<ECommandEvent>::OnEvent` is a NULLARY notification — it carries no event value — so `ECommandEvent`'s constants cannot be recovered from the listeners, and the four subscribing tasks each have their own single-slot handler.** From `rtti.bases.tsv`, `Listener<ECommandEvent>` sits at `+0x34` on exactly `CUnitRepairTask`, `CUnitCaptureTask`, `CUnitReclaimTask` and `CUnitGuardTask` (`C-121`), each with a distinct one-slot vtable: `0x00600830`, `0x0060ba90`, `0x0061b280`, `0x00626c30` respectively, **no target shared between them**. All four were disassembled: every one uses `this` immediately and **none reads a stack argument as a discriminator** — `CUnitCaptureTask`'s does `leal -0x34(%edi)` to recover the task base, confirming the subobject offset from the other direction. Behaviour of the repair handler: it weak-derefs a unit from listener `+0x5c` (the `ptr−4` idiom, `C-121`) and **clears 64-bit state bit 40 = `NoReclaim`** via `andl $0xfffffeff, 0x4a4(%eax)`, then zeroes `Unit+0x2AC` work progress (`C-132`) on a *different* unit reached from listener `−0x18`. | [EXE] `ART-E001` `0x00600830`, `0x0060ba90`, `0x0061b280`, `0x00626c30`; vtables `0x00e6f634`, `0x00e6fa8c`, `0x00e70018`, `0x00e7043c` | High for structure, Medium for the repair-handler reading | Confirmed (structure); interpretation partly inferred | Combined with `C-134` this **closes the `ECommandEvent` question rather than leaving it open**: the names are absent from the binary *and* the one place they would naturally be consumed does not consume them. The bit-40 identification follows `C-123`'s proven bit-index-equals-value rule (`0xfffffeff` clears bit 8 of the HIGH dword = state 32+8). **What the two unit references are is inferred, not read** — plausibly the repair target and the repairer — and a session wanting `WP-17` should pin them before building on it. |
| `C-134` | `WP-01`, `WP-12`, `WP-28`, `WP-29` | **A complete census of the engine's reflected enums, and which ones still lack a value table.** The serialization reflection emits a mangled helper name per enum — `PrimitiveSerHelper@W4<Type>@Moho@@H` and `SerSaveLoadHelper@W4<Type>@Moho@@` — so grepping those yields the **authoritative list of 46 reflected enum types**, independent of how (or whether) their values were registered. Cross-referencing against `C-122`'s 51 registered enums and `C-125`'s four motion tables leaves **about twelve with no recovered values**, of which the sim-relevant ones are: **`ECommandEvent`**, `EProjectileImpactEvent`, `ESiloBuildStage`, `ESiloType`, `EPathType`, `EPathPointState`, `EAirCombatState`, `EAiResult`, `EUnitMotionCarrierEvent`, `State@CAiPathNavigator`. | [EXE] `build/re-fa/exports/strings.all.tsv`; RTTI descriptors e.g. `.?AW4EAirCombatState@Moho@@` at `0x00fd91e0`, `EAirCombatStateTypeInfo` at `0x00fd8d24` | High | Confirmed (as a census); values **Open** | **`ECommandEvent` is the highest-value gap**: `C-121` found `Listener<ECommandEvent>` at `+0x34` on exactly four task classes — `Repair`, `Capture`, `Reclaim`, `Guard` — and *not* on `GetBuilt` or `FactoryBuild`. Its values would say what those four listen for, which is a `WP-12`/`WP-17` question. **Two approaches were tried and both are dead ends, recorded so they are not repeated:** the type-info blocks (0x40-byte records at `0x00e7a600`+, e.g. `EAirCombatState` at `0x00e7a700`) have **zero literal references image-wide**, and so do the tables they point at (`0x00ed77e4`, `0x00ed784c`, …) — they are reached by index from a descriptor array, not by literal address, and their fields are zero in the file because they are constructed at startup. **RESOLVED, and the answer is that the names do not exist in the binary.** The registration path was traced: `0x006c1120` and `0x006c11c0` are the per-enum registrars, and the assert strings they carry say exactly what they register — `!type->mSerLoadFunc` and `!type->mSerSaveFunc`, from `gpgcore/reflection/serialization.h` lines 84 and 87. **That path installs save/load functions and never carries a value name.** Value names come from one of only two other places: `C-122`'s enum registration, or `C-125`'s pointer array — and that array holds exactly 16 strings, all four motion enums and nothing else (a 17th, `ArmyPool`, is scanner overrun past the end). A hypothesis was refuted along the way rather than reported: "serialization-reflected implies unnamed" is false as a *rule*, because `EUnitMotionHorzEvent` has both a `PrimitiveSerHelper` and a name table — the two mechanisms are independent, and these twelve enums simply have only the first. **Consequence: stop looking for a table. Their values can only come from reading switch statements and comparisons in code**, one enum at a time, and are worth that cost only where a specific question needs them. |
| `C-128` | `WP-01` | **Object layout goes from ~40 hand-won offsets to 484 fields across 51 classes, and the BASE POINTER IS READ RATHER THAN GUESSED.** Each Lua binding calls a per-type receiver resolver, and that resolver contains a literal pointer to the MSVC `TypeDescriptor` for its type — `push $0xfcbc48; call 0x94ef10` → `0xfcbc48+8` = `".?AVEntity@Moho@@"`, `push $0xfca61c` → `".?AVUnit@Moho@@"`. So every `base_class` in the output was read out of the executable, and inheritance resolves mechanically: `Unit:GetHealth` resolves via the *Unit* descriptor and reads `+0x98`, `Entity:GetHealth` via the *Entity* descriptor and reads `+0x90` — the same field with correctly-named bases, which is exactly what `C-053` demands. Enum descriptors (`.?AW4…`) are rejected as value converters. Yield: 484 (class, offset) fields (156 high / 285 medium / 43 low, 338 simulation-side), plus 105 vtable slots and 31 native `thiscall` addresses. | [EXE] `tools/re/mine_field_offsets.py`, `tools/re/verify_field_offsets.py`; `build/re-fa/exports/field_offsets{,.layout,.chains}.tsv` | High (structure) | Confirmed | **Validation is 12/12 on known answers**, re-derived independently: entity id, health, max health, owning `Sim*`, collision shape, `Unit+0x98`, owning army, work progress, the state mask, resource-consumed ratio, weapon container, `IAiSiloBuild*`. It also **corroborated two Medium entries** — `Entity+0x99` dead flag (via `Entity:Kill`) and `Entity+0x1B9` queued-for-destruction (via `Entity:BeenDestroyed`). Offsets with no Lua accessor (`Sim`, `CEconomy`, `CRandomStream`) are correctly ABSENT rather than hallucinated, which is the more important half of the test. **`field_meaning` is inferred throughout** (method name minus accessor prefix) — trust `base_class`, `offset`, `type`, `access`; treat the meaning as a hint. |
| `C-129` | `WP-28`, `WP-27` | **`Moho::UnitWeapon` (28 fields) and `Moho::Projectile` (35 fields) recovered from zero.** Weapon: `+0x5C` rate of fire, `+0x60`/`+0x68` min radius and `+0x64`/`+0x6C` max radius (**each `ChangeMinRadius`/`ChangeMaxRadius` writes TWO slots** — likely current + blueprint default, which is which unresolved), `+0x70` max height difference, `+0x90` damage radius, `+0x94` damage `float`, `+0xA0` owning `Moho::Unit*`, `+0xCC` current target position, `+0xD0` current target, `+0x148` fire target layer caps, `+0x14C` firing randomness, `+0x150`/`+0x154`/`+0x158` targeting priorities. Projectile: `+0x278` launcher, `+0x280` velocity, `+0x2A8`/`+0x2A9` collide surface/entity, `+0x2AB` velocity align, `+0x2AE` stay underwater, `+0x2B0` turn rate, `+0x2B4` max speed, `+0x2B8` acceleration, `+0x2BC` ballistic acceleration, `+0x2C8` damage, `+0x2F0` tracking target (`WeakPtr`, deref at `−0x4`), `+0x36C` max zig-zag, `+0x370` zig-zag frequency, `+0x374`/`+0x378` detonate above/below height. **`Projectile:SetLifetime` loads `Entity+0x148` → `Sim+0x900` (the tick counter, `C-055`) and stores an `int32` at `+0x330`: lifetime is an ABSOLUTE EXPIRY TICK, not a countdown.** | [EXE] `build/re-fa/exports/field_offsets.layout.tsv`; four rows hand-checked against raw disassembly | High | Confirmed | `UnitWeapon+0xA0` as the owning `Unit*` was **cross-checked rather than asserted**: chained accesses from it land on `Unit+0x540` and `Unit+0x68E`, both independently established from `Unit`-resolved methods. The absolute-expiry-tick representation is directly implementable and is the kind of fact `WP-28` needed. |
| `C-130` | `WP-27` | **Hunting-queue #7 fully answered: `CAiAttackerImpl` vtable `+0x58` is `IsTargetExempt`.** The whole `IAiAttacker` interface is now named: `+0x08 GetUnit, +0x0C AttackerWeaponsBusy, +0x18 GetWeaponCount, +0x20 SetDesiredTarget, +0x24 GetDesiredTarget, +0x2C CanAttackTarget, +0x34 FindBestEnemy, +0x38 GetTargetWeapon, +0x3C GetPrimaryWeapon, +0x40 GetMaxWeaponRange, +0x54 IsTooClose, +0x58 IsTargetExempt, +0x5C HasSlavedTarget, +0x60 ResetReportingState, +0x6C ForceEngage`. `CAiNavigatorImpl` is likewise mapped (`+0x04`…`+0x38`). `CAiAttackerImpl`'s own bindings do no field work — each forwards through the vtable — so its layout is unreachable this way, and that is itself the finding. | [EXE] `build/re-fa/exports/vtable_slots_from_lua.tsv` | High | Confirmed | **Independently corroborates `C-119` by a different route and a different agent.** `C-119` read `0x005dde10` as an "am I already ordered at this thing?" per-candidate reject **from the disassembly, without a name**; this recovers the engine's own name for the same slot — `IsTargetExempt` — from the interface. Read behaviour and transferred name agree. The call site remains unpinned (`C-119`). |
| `C-131` | `WP-13`, `WP-12`, `WP-31` | **Five state-mask bits read as literal immediates, and a command-queue container opened.** From `orl` instructions against `Unit+0x4A0`/`+0x4A4`: bit 1 `Immobile` (`orl $0x2, 0x4a0`), bit 13 `Busy` (`$0x2000`), bit 33 `UnSelectable` (`orl $0x2, 0x4a4`), bit 34 `DoNotTarget` (`$0x4`), bit 42 `BlockCommandQueue` (`$0x400`). **`Moho::Unit+0x4B4` is the task/command queue**: `+0x10`/`+0x14` are `begin`/`end` of a vector of 8-byte entries (`sub; sar $3` → count), `+0x0C` a second container, and `IsIdleState` returns true when that vector is empty. `Unit+0x554` is a related object gating build orders, reached by virtual slot `+0x30`. Shields: there is **no `Moho::Shield` receiver** — they are exposed through `Unit`, and `Moho::Unit+0x298` is the shield ratio (`float`, witnessed by both `GetShieldRatio`/`flds` and `SetShieldRatio`/`fstps`). | [EXE] `build/re-fa/exports/field_offsets.tsv`, `field_offsets.chains.tsv` | High | Confirmed | **The five bits match `C-123`'s enum table exactly — and the two were derived by different agents from different evidence**, one reading `orl` immediates out of code, the other reading the startup enum registration. Five-for-five agreement across independent methods. `Unit+0x4B4` is also the list `C-119` walks for its exempt check, which ties `WP-12` and `WP-27` to one container. |
| `C-132` | `WP-01`, `WP-15` | **Five corrections to the recorded object-layout table.** (a) **`Unit+0x154` owning army is really `Entity+0x14C`** — `Entity:GetArmy` resolves an *Entity* and reads `+0x14C`, and with the `Entity` subobject at `Unit+0x08` these are one address; recording it under `Moho::Unit` wrongly implies props and projectiles lack it. (b) **`Unit+0x2AC` is generic *work progress*, not "silo build progress"** — witnessed by `Unit:GetWorkProgress`/`SetWorkProgress`; silo build is one consumer. (c) **`Unit+0x544` is the attacker/weapon-MANAGER pointer**, not a bare container — `GetWeapon`, `GetWeaponCount`, `GetAttacker` and `GetTargetEntity` all route through it, so it is the `IAiAttacker` that owns the weapons. (d) **`Moho::Unit` is at least `0x690` = 1,680 bytes**, not "~1.4 KB": `Unit+0x68E` is written by three `Unit`-resolved methods and reached independently from `UnitWeapon+0xA0`. (e) `Moho::Prop` yields only 4 fields because retail exposes exactly **one** `Prop` Lua method — `WP-18`'s wreck/reclaim fields are not script-visible and need a Tier-3 read from `CUnitReclaimTask`. | [EXE] `build/re-fa/exports/field_offsets.layout.tsv` | High | Confirmed | **No recorded offset was contradicted outright — every overlap agreed.** These are naming and scope corrections, which is the failure mode `C-053` established as the dangerous one. One further item is explicitly marked *inferred, not read*: `Prop+0x148` and `Projectile+0x148` both resolve to `Sim*`, the same slot as `Entity+0x148`, implying those two classes embed `Entity` at `+0x00` rather than `+0x08` — but it rests on that single coincidence and no second independent field was found to confirm it. |
| `C-133` | `WP-30`, `WP-33` | **New `Moho::Entity` fields, 64 in total.** `+0x9C`–`+0xA8` is a four-float orientation quaternion (read by `GetHeading`) sitting immediately before the known `+0xAC`–`+0xB4` position, so the 28 bytes `C-057` hashes as "transform" is position plus part of this block. `+0xD8` fraction complete (matching `C-098`). `+0x128`–`+0x144` intel block; `+0x1D8` intel-enabled object. `+0x1DC`/`+0x1E0`/`+0x1E4`/`+0x1E8` are visibility to focus-player / allies / enemies / neutrals — **four methods, four consecutive slots, in semantic order**. `+0x218`/`+0x220`/`+0x224` shooter tracking. Bonus for `WP-33`: **`Moho::ReconBlip+0x4C4` points to a per-army array with stride `0x34` (52 bytes)** — `imull $0x34, %eax` then `0x4(%eax,%ecx)`. | [EXE] `build/re-fa/exports/field_offsets.layout.tsv` | High | Confirmed | The four consecutive visibility slots corroborate each other mutually — a wrong base would have to be wrong four times in semantic order. The `ReconBlip` stride is exact and reusable for intel parity work under `C-113`, where we currently model no remembered contacts at all. |
| `C-122` | `WP-01`, `WP-13` | **The engine registers every named enum at startup through one helper pair, and each entry pushes its integer NEXT TO its own name — so values are read, never inferred.** `0x00947000` interns the name, `0x0094def0` binds; each entry is 22 bytes (`push <value>; push offset "<NAME>"`). Sweeping all call sites recovers **525 (name, value) pairs across 51 enums** from 51 registration functions. Self-check: decoded entry count equals actual bind-call count for **51/51**. | [EXE] `ART-E001` `0x00947000`, `0x0094def0`; `tools/re/extract_enum_registrations.py`; `build/re-fa/exports/enum_registrations.tsv` | High | Confirmed | **The method is validated by the case that should differ, and the agent went looking for it:** 5 of the 51 enums register OUT of ascending order — `FIRESTATE_` starts at 255, `THREATTYPE_` runs 0,1,3,2,4,6,5, and `RECON_` is a bitmask with composite values. Position-counting would give wrong answers on exactly those and reading the immediates does not. This is the third Tier-1 self-description vein found (after RTTI and the registration scan) and the highest-yield one per unit of effort. |
| `C-123` | `WP-13` | **`Unit+0x4A0`'s 64-bit state mask is fully named: 45 states, bit index == enum value, proven end to end.** From `0x005626a0`: `1 Immobile, 2 Moving, 3 Attacking, 4 Guarding, 5 Building, 6 Upgrading, 7 WaitingForTransport, 8 TransportLoading, 9 TransportUnloading, 10 MovingDown, 11 MovingUp, 12 Patrolling, 13 Busy, 14 Attached, 15 BeingReclaimed, 16 Repairing, 17 Diving, 18 Surfacing, 19 Teleporting, 20 Ferrying, 21 WaitForFerry, 22 AssistMoving, 23 PathFinding, 24 ProblemGettingToGoal, 25 NeedToTerminateTask, 26 Capturing, 27 BeingCaptured, 28 Reclaiming, 29 AssistingCommander, 30 Refueling, 31 GuardBusy, 32 ForceSpeedThrough, 33 UnSelectable, 34 DoNotTarget, 35 LandingOnPlatform, 36 CannotFindPlaceToLand, 37 BeingUpgraded, 38 Enhancing, 39 BeingBuilt, 40 NoReclaim, 41 NoCost, 42 BlockCommandQueue, 43 MakingAttackRun, 44 HoldingPattern, 45 SiloBuildingAmmo`. Bit 0 and bits 46–63 are unused. The bit-index identity is **read, not assumed**: `Moho::Unit` vtable `0x00e7a024` (COL at `−4` resolves to `.?AVUnit@Moho@@`) slot `+0x3C` = `0x006ab580` = `mov eax,1; xor edx,edx; call __allshl; and eax,[esi+0x4A0]; and edx,[esi+0x4A4]; or eax,edx`; `0x00b963c0` verified as genuine `__allshl` so states ≥32 reach the high dword; call site `0x005de7af` passes `0x22` (34). | [EXE] `ART-E001` `0x005626a0`, `0x006ab580`, `0x00b963c0`, `0x005de7af` | High | Confirmed | **45 facts from one table, and it validates two earlier claims from the other direction**: `C-081`'s bit 45 = `SiloBuildingAmmo` ✓ and `C-094`'s bit 34 = `DoNotTarget` ✓, both originally derived by reading code. `WP-13`'s authoritative storage is now fully legible. Note `39 BeingBuilt` and `31 GuardBusy` — `C-118`'s `IsBeingBuilt` is this bit. |
| `C-124` | `WP-30`, `WP-33`, `WP-12`, `WP-26` | **Sim-relevant enum tables recovered in full**, all by `C-122`'s mechanism. `UNITCOMMAND_` 40 entries 0–39 (`0 None, 1 Stop, 2 Move, 3 Dive, 4 FormMove, 5 BuildSiloTactical, 6 BuildSiloNuke, 7 BuildFactory, 8 BuildMobile, 9 BuildAssist, 10 Attack, 11 FormAttack, 12 Nuke, 13 Tactical, 14 Teleport, 15 Guard, 16 Patrol, 17 Ferry, 18 FormPatrol, 19 Reclaim, 20 Repair, 21 Capture, 22–26 Transport*, 27 Upgrade, 28 Script, 29 AssistCommander, 30 KillSelf, 31 DestroySelf, 32 Sacrifice, 33 Pause, 34 OverCharge, 35 AggressiveMove, 36 FormAggressiveMove, 37 AssistMove, 38 SpecialAction, 39 Dock`) — **confirming `C-097` from the executable rather than the DLL**. `LAYER_` bitmask `1 Land, 2 Seabed, 4 Sub, 8 Water, 16 Air, 32 Orbit, 127 All` (`All`=0x7F implies an unnamed bit 64). `IMPACT_` 12 values incl. `8 Shield, 9 Unit, 10 UnitAir, 11 UnitUnderwater`. `INTEL_` 14 values. `RECON_` bitmask `1 Radar, 2 Sonar, 4 Omni, 8 LOSNow, 16 LOSEver, 32 KnownFake, 64 MaybeDead` + composites. `CDamage:: 0 SINGLE_TARGET, 1 AREA_EFFECT, 2 RING_EFFECT`. `COLSHAPE_ 0 None, 1 Box, 2 Sphere`. `FIRESTATE_ 0 ReturnFire, 1 HoldFire, 2 HoldGround, 255 Mix`. Plus `ALLIANCE_`, `RESTYPE_`, `ECON_`, `JOB_`, `THREATTYPE_` (15), `SQUADCLASS_`, and the Lua-facing `RULEUMT_` (10 motion types), `RULEUCC_` (25-bit command-capability mask), `RULEUTC_` (9 toggles), `RULEUBA_`, `UWRC_` weapon roles. | [EXE] `build/re-fa/exports/enum_registrations.tsv` | High | Confirmed | **Two independent cross-confirmations landed in the same session.** `CDamage::` 0/1/2 confirms `C-059`. `COLSHAPE_ 1 Box, 2 Sphere` independently confirms `C-120`'s finding — from vtables, by a different agent — that `CColPrimitiveBase` has **exactly two** implementations, box and sphere. And `RECON_`'s `LOSNow`/`LOSEver`/`MaybeDead` bits are `C-078`'s seen-now / seen-ever / maybe-dead identity, named by the engine itself. |
| `C-125` | `WP-22`, `WP-07` | **The four `CUnitMotion` event enums, by a DIFFERENT mechanism — a 16-pointer `const char*` array indexed by `lea [base+reg*4]`, replicated 29 times in `.data`.** Read from `0x006bff20` (resets all four and fires each callback): `EUnitMotionHorzEvent` at `CUnitMotion+0x7C` (`0 Cruise, 1 TopSpeed, 2 Stopping, 3 Stopped`, idle 3); `EUnitMotionVertEvent` at `+0x80` (`0 Top, 1 Bottom, 2 Up, 3 Down, 4 Hover`, idle 1); `EUnitMotionTurnEvent` at `+0x84` (`0 Straight, 1 Turn, 2 SharpTurn`, idle 0); `EUnitMotionState` at `+0x78` (`0 None, 1 Attached, 2 Ballistic, 3 Crashed`, idle 1). Type names are the engine's own, from reflection `GetTypeName` stubs at `0x006bdb80`/`0x006bdde0`/`0x006bdf10`/`0x006be040`. Setters at `0x006bf9b0`/`0x006bf9f0`/`0x006bfa30`/`0x006bfa70`, each firing its callback. | [EXE] `ART-E001` as listed; [LUA-R] `ART-S007` `lua/sim/Unit.lua` L2140, L2167, L2213–2219, L2272–2277 | High | Confirmed | Corroborated for free: **all 16 names appear in shipped `Unit.lua`** as the `new`/`old` arguments of the three `OnMotion*EventChange` callbacks — including `Hover` grouped with `Bottom`, matching the recovered split. **Negative result worth keeping:** the engine emits a fourth callback, `OnMotionStateChange` (`0x00e7a53c`), which **does not exist anywhere in the shipped Lua** — `Attached`/`Ballistic`/`Crashed` transitions are fired and dropped. Caveat stated by the agent: `units.scd`/`mods.scd` were not searched, so a unit-side override is not excluded. |
| `C-126` | `WP-11` | **No engine-side victory predicate exists; Lua decides and pushes the verdict in.** Every relevant native is a `<global>` and their own shipped doc strings state the direction of control: `SetArmyOutOfGame(army)` — *"indicate that the supplied army has been defeated"* (`0x007115e0`); `ArmyIsOutOfGame` (`0x007114e0`); `EndGame` — *"Signal the end of the game. Acts like a permanent pause"* (`0x00752bf0`); `IsGameOver` — *"true if the game is over (i.e. EndGame() has been called)"* (`0x00752cc0`); plus `GetArmyScore` (`0x008499a0`) and `SetAlliedVictory` (`0x00710320`). | [EXE] addresses as listed; engine-shipped doc strings via `C-032` | Medium-High | **Lead, not traced** | Consistent with `C-009` (which ruled out `RRuleGameRules`) and with `C-060`'s finding that death is decided in Lua. Rests on doc strings plus the *absence* of a native predicate, not on a traced path — falsified by finding native code that calls `SetArmyOutOfGame`'s underlying setter with no Lua caller. **This closes hunting-queue #4's executable half: the answer is "look in scenario Lua", which the queue entry already suspected.** |
| `C-127` | `WP-43`, `WP-27`, `WP-20` | **57 functions name themselves in their own format strings**, an unambiguous label per row. Highest-value: `0x006eb250`+`0x006eb360` `CDecoder::DecodeIssueCommand` and `0x0074fa10` `Sim::SetCommandSource` (**`WP-43`**); `0x006dc860` `UnitWeapon::PickNewTargetAimSpot` (**`WP-27`**); `0x00595ab0` `CAiBrain::PickBestAttackVector`; the `CAiPathNavigator` cluster `0x005b5980`/`0x005b61a0`/`0x005b4380`/`0x005b4ce0` and `CAiNavigatorImpl::NavigatorAbortMove` `0x005aa220` (**`WP-20`/`WP-21`**). **`WP-43` bonus:** `0x005438a0` is a per-client debug dump printing member names in order — `mNickname, mIndex, mValidCommandSources, mCommandSource, mReady, mPipe.GetLength(), mQueuedBeat, mDispatchedBeat, mAvailableBeatRemote, mLatestAckReceived, mLatestBeatDispatchedRemote, mEjectPending, mEjected, mEjectRequests[].{mRequester, mAfterBeat}`, with `0x005463d0` adding `mFullyQueuedBeat`, `mPartiallyQueuedBeat` — a free field map of the client/replay object. **Source-tree bonus:** 74 paths root at `C:\work\rts\main\code\src\` with a `.\hastar\` module (`Cluster.cpp`, `ClusterCache.cpp`, `ClusterMap.cpp`) — **hierarchical A\***, which names `C-012`'s `PathTables` mechanism. | [EXE] `build/re-fa/exports/strings.function_names.tsv`, `strings.sourcefiles.tsv` | Medium per row | Lead | **Deliberately rated Medium and stated as such by the agent**: a string proves a function *can emit* that message, not that it *is* that method — a wrapper or an inlined helper could carry a callee's label. Use as `maybe_` names. The 74-path source tree overlaps `C-114`'s 72 by construction; the `hastar` naming is the new fact. |
| `C-117` | `WP-01`, `WP-03` | **Every vtable in the binary is recovered, validated against the linker's own statement of the answer.** Walking RTTI type descriptors → 4,049 Complete Object Locators → the `.rdata` word pointing at each COL → the vtable 4 bytes later yields **4,049 vtables over 3,861 classes (403 of the 471 concrete `Moho::` ones) and 32,678 slot rows**, naming 1,030 slots across 188 classes. Validation is unusually strong: the DLL exports 217 `??_7<Class>@@6B@` vftable symbols — the linker saying where each vtable begins — and **217 of 217 land exactly on a scanned vtable, zero misses**; slot counts agree across the two builds for **2,048 of 2,089 (98.0%)** `(class, offset)` pairs, with the 41 disagreements skipped rather than half-named; and 7,270 derived-at-least-as-long-as-base checks pass with **0 violations**. Two structural facts fall out. **The 68 concrete `Moho::` classes with NO vtable are non-polymorphic by construction** — `CEconomy`, `CEconRequest`, `CEconStorage`, `COGrid`, `CMersenneTwister`, `CIntelGrid`, `CCommandDB`, `CAiTarget` — so the whole economy layer dispatches statically and has no virtual hook to look for. And **not one of the 797 Lua-bound natives is a virtual method**: the intersection of `moho.methods` `method_va` with all 8,397 slot targets is empty, so the Lua boundary never enters through a vtable. **4,514 slot targets are function starts Ghidra never found.** | [EXE] `tools/re/extract_vtables.py`; `build/re-fa/exports/vtables{,.classes}.tsv`, `rtti.bases.tsv`; [ART-D001] used strictly as a naming key `(mangled class, subobject offset, slot) → name`, no address or behaviour transferred, per `C-075` | High | Confirmed | Semantics were then checked **exe-side against offsets three earlier sessions derived by unrelated means, 4 of 4 exact**: `Unit::GetEntityId` reads `[this+0x70]` = `C-057`'s `Entity+0x68` plus `C-053`'s `Unit+0x08`; `GetPosition` is `lea [this+0xB4]` = `Entity+0xAC`; `IsDead` reads byte `[this+0xA1]` = `Entity+0x99`, **which upgrades `C-060`'s dead flag from Medium to confirmed**; `IsUnit` is `mov eax,ecx; ret`. Also `0x00ac123f` = `__purecall`. |
| `C-118` | `WP-16` | **Hunting-queue #2 answered: `CUnitGetBuiltTask::TaskTick` is `0x00610fa0`, a two-state machine keyed on `this+0x24`.** State 0: if `owner->IsBeingBuilt()` return Running; else if `!owner->IsMobile()` return Done; else advance to state 1. State 1: weak-deref `owner->[0x194]` and return Done when it drops, Running otherwise. So on the **target** side the task holds the unit while it is under construction, and the instant construction finishes an **immobile** target (a structure) ends immediately while a **mobile** target stays in `GetBuilt` through a second phase until a weak reference clears — the factory roll-off. The default case asserts, naming `.\sim\AiUnitCommands.cpp` line 557. | [EXE] `ART-E001` `0x00610fa0`, vtable `0x00e6fe00` slot 1; assert string per `C-114` | High | Confirmed | **Two agents reached `0x00610fa0` / `AiUnitCommands.cpp:557` independently in the same session** — one from source-path strings (`C-115`), one from RTTI vtables — having shared no intermediate result. Structure-vs-mobile asymmetry after completion is the implementable fact. `Unit+0x194` is a `WeakPtr` to a unit; which unit is **unverified** (creator is the obvious candidate, not traced), and `IUnit` slots `+0x30`/`+0x34` are oracle-named `IsMobile`/`IsBeingBuilt` rather than read. |
| `C-119` | `WP-27` | **Hunting-queue #7, first half: `CAiAttackerImpl` vtable `+0x58` is slot 22 = `0x005dde10`, a per-candidate "am I already ordered at this thing?" reject.** A `__thiscall` predicate taking one pointer and returning `bool`. It reads the owning `Unit*` from `this+0x40` (corroborated internally by its use of `[unit+0x154]`, the owning army from `C-068`), snapshots the list at `unit+0x4b4` into a local vector, and weak-derefs each 8-byte entry; for entries whose type field `[e+0x98]` is `0x13` or `0x15` it compares the entry's target at `[e+0x120]` against the candidate and returns true on a match. A second pass calls the candidate's virtuals `+0x10` then `+0x3c` with argument `0x1B` (the shape of `IsUnitState(27)`) and walks another list from `[unit+0x150]->[0x8c8]`. | [EXE] `ART-E001` `0x005dde10`, primary vtable `0x00e6e59c` (29 slots, all overriding pure-virtual base slots) | High for the address, Medium for the description | **Partly open** | **The call site is unpinned**: the slot is never reached by a literal `call [reg+0x58]` — exactly one such instruction exists image-wide and is unrelated — so the caller loads the slot into a register first, and 176 candidates were not disambiguated within the timebox. Command-type constants 19 and 21 are unidentified. Falsifier: find the caller and discover the argument is not an acquisition candidate. |
| `C-120` | `WP-26`, `WP-30` | **`C-066`'s "virtual exact test at slot `+0x20`" now has a class, two implementations and a signature.** `Entity+0x178` holds a `Moho::CColPrimitiveBase*` — a pure interface, all 10 slots `__purecall` — with exactly two implementations: `CColPrimitive<Wm3::Sphere3<float>>` (vtable `0x00e5d068`, slot `+0x20` = `0x005052a0`) and `CColPrimitive<Wm3::Box3<float>>` (vtable `0x00e5d0f4`, slot `+0x20` = `0x00505c50`). Slot `+0x20` is `bool Intersect(const Sphere3& probe, ContactInfo* out)`. Read at the use site `0x00681440`: null-check the field, call the slot with (probe, out), and on success write the entity into `out+0x04` so the contact records what was hit. The sphere version compares `|Δcentre|²` against `(r₁+r₂)²` with SSE; the box version calls the Wm3 point-box distance routine at `0x00a84d10`, compares squared distance against `r²`, and writes the closest point to `out+0x08..0x10`. | [EXE] `ART-E001` `0x00681440`, `0x005052a0`, `0x00505c50`, `0x00a84d10` | High | Confirmed | **Two shapes only — sphere and box.** `0x00681460` immediately below calls slot `+0x1c` on the same field: a second, presumably cheaper test, unread and worth a look. Falsifier: an `Entity` subclass storing a different class at `+0x178`; one call site was checked, not all. |
| `C-121` | `WP-12`, `WP-16`, `WP-17` | **The whole unit-command-task surface is one function per task, and it is now completely indexed.** All 32 `Moho::CCommandTask` subclasses have an **identical 2-slot vtable — `[scalar deleting dtor, TaskTick]` — and every one overrides both slots.** There is no shared implementation in the family and no inherited plumbing to filter out. `TaskTick` addresses: `CUnitGetBuiltTask 0x00610fa0`, `CUnitRepairTask 0x005ffe40`, `CUnitCaptureTask 0x0060aeb0`, `CUnitReclaimTask 0x00625ad0`, `CUnitGuardTask 0x0061ac70`, `CFactoryBuildTask 0x00601260`; all 32 in `vtables.siblings.CCommandTask.tsv`. `Moho::Entity` and `Moho::Unit` reach `TaskTick` through their `CTask` subobject at `Entity+0x34` / `Unit+0x3c`, both dispatching to one `Entity::TaskTick` at `0x006806d0`. | [EXE] `build/re-fa/exports/vtables.siblings.{CCommandTask,CTask}.tsv`, `rtti.bases.tsv` | High | Confirmed | **This kills a search step for `WP-16` and `WP-17` outright.** It also constrains `WP-12`: queue-clearing behaviour **cannot** live in a `CCommandTask` virtual, because the class has none besides `TaskTick` — it is either inside a specific `TaskTick` or on `CUnitCommandQueue`, which per `C-117` has no vtable at all. Bonus artifact: `rtti.bases.tsv` gives the byte offset of every base subobject of every class, cross-checked against `C-053` (`Entity` at `Unit+0x08`) and `C-064` (collision record at `Entity+0x4C`); it also shows `WeakObject` at `Unit+0x04`, which is what makes the pervasive `ptr−4` weak-deref idiom legible, and `InstanceCounter<Unit>` at `Unit+0x278` bounding `sizeof(Unit)`. `Repair`/`Capture`/`Reclaim`/`Guard` carry `Listener<ECommandEvent>` at `+0x34`; `GetBuilt` and `FactoryBuild` do not. |
| `C-114` | `WP-01` | **`__FILE__` survives in this build, but the source-path oracle is small and lands almost nowhere useful.** Retail is an `NDEBUG` build — zero hits for `Assertion failed`, `_wassert`, `Debug Assertion` — yet GPG's own release-mode panic macro survived: `push <file>; push <line>; push <msg>; call 0x009728a0; int3`, canonically at `0x0067f584` (`.\sim\Entity.cpp` line 354, *"Reached the supposably unreachable."*). 1,178 direct calls to that reporter. Of 21,028 data strings, **72 are source paths**, 66 referenced from code across **1,528 literal `push imm32` sites** (93% carrying a recoverable line number), attributing **560 functions — 1.8% of 31,086.** The build root is `c:\work\rts\main\code\`, output `bin8`, from the recovered PDB path. Accuracy where it fires is **98.5% and effectively 100%** against RTTI-derived ground truth (the one `.cpp` "miss" is a tokenizer artifact), corroborated two independent ways: line numbers rise monotonically with function address within every file, and `.rdata` string order = `.text` code order = alphabetical filename order. | [EXE] `ART-E001` `0x0067f584`, panic reporter `0x009728a0`; `tools/re/extract_source_paths.py`; `build/re-fa/exports/source_paths{,.rollup}.tsv` | High | Confirmed | **This is a negative result for its stated purpose, and the distribution is why**: 72% of sites are the reflection/serialization template headers, 24% the D3D renderer (P3, out of scope), and **0.9% — 14 sites — are in `src/sim/`**. There is **no `Damage.cpp`, `Shield.cpp`, `Economy.cpp`, `Weapon.cpp`, `OGrid.cpp` or `Motion.cpp` string at all**; those subsystems left no `__FILE__` and this oracle cannot reach them. A deliberately loose re-sweep found exactly one string the heuristic missed (the PDB name), so the ceiling is real rather than an artifact of the search. |
| `C-115` | `WP-16`, `WP-12`, `WP-17` | **Recovered source layout for the unit-task classes, with line numbers.** `.\sim\AiUnitBuild.cpp` defines five task classes in source order, each pinned by its own panic line: `CUnitMobileBuildTask` (856), `CUnitUpgradeTask` (1032), `CUnitRepairTask` (1388), `CFactoryBuildTask` (1713), `CUnitSacrificeTask` (1896), at strictly increasing addresses `0x5fdf10 < 0x5ff360 < 0x5ffe40 < 0x601260 < 0x601c50`. Separately, **hunting-queue #2 is now anchored**: `CUnitGetBuiltTask` is `.\sim\AiUnitCommands.cpp` around line 557, function `0x00610fa0`, sharing that translation unit with `CUnitTeleportTask`, `CUnitFireAtTask` (line 910, `0x00611e50`), `CUnitFerryTask`, `CUnitWaitForFerryTask` and `CUnitGuardTask`. Fifteen `sim/` files are named in total, all of them `AiUnit*`/`AiPath*` plus `Entity`, `PathQueue`, `PathTables`. | [EXE] `ART-E001`; `build/re-fa/exports/source_paths.tsv` | High | Confirmed | Turns "find `CUnitGetBuiltTask`" into a known address, and gives the *sibling set* that shares its file — which is the right comparison group for `WP-12`/`WP-16`/`WP-17`, since classes in one TU tend to share a base and differ only in overridden slots. |
| `C-116` | `WP-01` | **Bracketing unreferenced file strings is a lead, not a fact — and the agent found the case that refutes the strong reading.** Because string order = code order = alphabetical filename order (`C-114`), a source file with *zero* code references can still be bracketed into a `.text` span between its alphabetical neighbours. It predicts cleanly twice: `AiUnitCallTransport.cpp` brackets to `[0x602300, 0x60abf0)` containing exactly `CUnitCallTransport`/`CUnitCallLandTransport`/`CUnitCallTeleport`/`CUnitCallAirStagingPlatform`, and `AiUnitCarrier.cpp` to a span containing exactly the three `CUnitCarrier*` classes. **But the `[AiUnitMeleeAttack, AiUnitPodAssist)` bracket contains `CUnitMoveTask`, `CUnitFormAndMoveTask` and `CUnitPatrolTask`, which must come from an `AiUnitMove*.cpp` that left no marker.** So a bracket bounds a *run* of translation units, not one file. | [EXE] `build/re-fa/exports/source_paths.rollup.tsv` | Medium | Confirmed (as a lead-generator) | This is the campaign's failure mode #1 — *"inferring a mechanism from agreeing examples"* — caught **before** it was recorded as fact, by deliberately hunting the case that should differ. The two agreeing examples would have been enough to state the strong version. Bracketed attributions get `maybe_`; only direct references are fact. |
| `C-113` | `WP-33` | Intel audit: **most of it is already right, by a different derivation.** Coverage is refcounted not boolean; vision cells are 16 elmos and radar/sonar 32, which is exactly retail's scale-2 and scale-4 — reached independently from Recoil's `losMipLevel`/`radarMipLevel` rather than from the executable, so the agreement is genuine corroboration of `C-077`. Radius-to-cells truncates, rasterisation is a filled circle, and removal is immediate. **One defect: omni was rasterised at the vision mip**, where retail puts it with the scale-4 family — fixed, with the source comment's precision argument recorded as reasoning about a choice the original had already made. Gaps rather than defects: no explored bitmask, no water-vision grid (parsed and deliberately unused), 6 grids against retail's 9, no recon staggering, and **no remembered contacts at all** — a blip vanishes the beat its source leaves radar, so retail's ghost dots simply do not occur. | [RM] `src/core/sim/Intel.{hpp,cpp}`; [EXE] `C-076`–`C-079` | High | Confirmed (omni fixed) | The agent also flagged that `C-099`'s asymmetric health floor is **non-applicable** here, not wrong — there is no fraction-decrease path — and warned explicitly against adding a *symmetric* floor when one is built. Worth honouring. |
| `C-103` | `WP-15` | **Recoil Metal's scarcity allocator is the single global throttle `C-067` refutes.** `src/core/sim/Economy.cpp:68-83` computes one `funded = min(stored.mass/wanted.mass, stored.energy/wanted.energy)` and applies it to every consumer identically. There are no buckets, no binding resource, no second ratio. Worse, **upkeep is not a request at all**: it is summed into a per-army lump and subtracted from `stored` *before* demand is measured, with the shortfall absorbed by a `max(0, …)`. So retail's single-resource bucket is **not representable**, and an unpayable upkeep bill has **no effect on anything** — shields regen at full rate, intel is never gated, and the funded fraction stays at 1.0 while the energy bar sits at zero. | [RM] `src/core/sim/Economy.{hpp,cpp}`, `Skirmish.cpp:173`; [EXE] `C-067` | High | **Open** | `D1` cannot be fixed by changing the ratio alone — upkeep has to become demand first. The header comment claiming "upkeep before construction *is the mechanic*" was an **invention presented as a rule**; retail has no priority between them and would fund both at the same fraction. Comment corrected; behaviour not. |
| `C-104` | `WP-15` | Five further economy divergences from the same audit. (a) **Storage is clamped before allocation** (`Economy.cpp:38-39`), so a full-storage army cannot spend the tick's income; retail allocates out of `stored + income` and clamps afterwards. (b) **`requested` and `usage` are conflated** and the *wrong* one is published — retail keeps total outstanding demand and total *granted* separately, and defines trend as `income − usage`, so our trend goes sharply negative during a stall where retail's approaches zero. (c) **Excess above storage is destroyed**; retail shares it `1/n` among allies with free storage. (d) **No per-unit consumed ratio** exists — ours is per army, which stops being equivalent the moment (a) in `C-103` is fixed. (e) **No per-structure truncation** of storage capacity, the one place retail rounds. | [RM] `Economy.cpp`, `Skirmish.cpp:174-175`, `app/FafOpponent.cpp:138,1034`; [EXE] `C-068`–`C-070` | High | **Open** | (b) is the cheapest and has the clearest consequence: **AI economy gates read a systematically wrong number precisely when it matters**. (c)'s source comment asserted the opposite of the recovered behaviour and is corrected. |
| `C-105` | `WP-27` | **The targeting audit found the largest single behavioural gap in the campaign: we have no priority concept at all.** `Combat.cpp:275-337` ranks candidates by ground distance and nothing else, and the header states this as deliberate ("a target priority list is a game design decision and this milestone does not make one"). Retail's priority index wins **unconditionally** before any score comparison, and a candidate matching no priority row is **never selectable**. Four more absences compound it: **no stickiness** (targets are re-derived every firing beat, so a burst can spray across three units where retail's stays on one); **no aim-direction score**, though 284 of 399 corpus weapons are turreted; **candidate sets are per weapon rather than one cached list per unit**, so two weapons on one unit legitimately disagree about ordering; and **out-of-range candidates are dropped rather than classed**, so a unit only begins turning once the enemy is already in range where retail is lined up before contact. | [RM] `src/core/sim/Combat.{hpp,cpp}`, `Weapon.hpp:229`, `Health.hpp`; [EXE] `C-090`–`C-093` | High | **Open** | The range-class defect is the one with a direct DPS consequence. Stickiness and the aim-direction term are **one change, not two** — both need per-weapon aim state, and both alter the state-hash layout. |
| `C-106` | `WP-27` | **We reproduced, in our own code, the exact trap `C-094` documented in retail.** `Unit:SetDoNotTarget` is bound in `src/app/FafApi.inc:228` and **read by nothing in the simulation** — a plausibly-named binding that does nothing, which is precisely what `C-094`(b) found on the retail side with `IsValidTarget`. Two further rejects are also missing: the **`BENIGN`** category (our `Role::Civilian` documents it and the targeting path never consults it) and the playable-map-rect test. `TargetRestrictOnlyAllow`/`Disallow` are read but collapsed to a single `AIR` bit, so a weapon restricted to `NAVAL` fires at anything on the permitted layer. | [RM] `src/app/FafApi.inc:228`, `Combat.cpp:36-48`, `Weapon.cpp:131-140`; [EXE] `C-094` | High | **Open** | Worth stating plainly: documenting a failure mode does not prevent committing it. A binding that no consumer reads should fail a test, not merely a review. |
| `C-107` | `WP-26`, `WP-43` | **Our tie set is materially LARGER than retail's, which widens the `C-065` divergence beyond what the claim implies.** Retail compares float32 squared distance; we compare a fixed-point hypotenuse quantised to 1/16384 elmo, so two candidates at genuinely different distances collapse to an exact tie far more often here. Since selection requires a *strictly* better score, every such collapse lands on our slot-ordered tie-break where retail would have separated them on distance. | [RM] `src/core/sim/Combat.cpp:271`, `SpatialGrid.cpp:51-53,112`; [EXE] `C-065` | High | Confirmed | The actionable part is not the ordering — that is an accepted divergence — but the **tie set**: comparing squared distance where it fits would shrink it and narrow the gap without touching determinism. A genuinely useful insight the claim did not anticipate. |
| `C-108` | campaign method | **Green tests are evidence of nothing when they were written from the same model as the code.** Two audits identified **ten tests** that pin behaviour retail does not have — three asserting one shared economy ratio, one asserting that unpaid upkeep is invisible, one asserting a minimum-range hole makes a unit target-less, one asserting nearest-first as *the* selection rule, and others. All are green. All would stay green through the entire class of defects above. | [RM] `tests/test_economy.cpp`, `test_combat.cpp`, `test_overcharge.cpp`, `test_assist.cpp`, `test_spatial_grid.cpp` | High | Confirmed | This is the third independent demonstration in one campaign (after the adjacency magnitudes and the damage falloff). **A test suite validates internal consistency, never external fidelity** — only a recovered specification can do that. |
| `C-096` | `WP-12` | **Exactly one place advances an order queue.** `IAiCommandDispatchImpl::TaskTick` (`0x0059f950`) holds the only five call sites in the whole executable of `RemoveFirstCommandFromQueue` and `MoveFirstCommandToBackOfQueue` — a negative check over the full image, not a sample. Its logic: on a hard-stop result, Attack and everything else pop while Patrol rotates; otherwise a repeat count `>1` decrements and stays on the command; a factory with **repeat-build** (`Unit+0x2a2`) set on a `BuildFactory` command restores the original count and rotates; Patrol, FormPatrol and cycling-Attack **rotate only while another command exists behind them** and pop when alone. Dispatch is then gated on not being built, not dead, not in states `0x0e`/`0x2a`, and a non-empty queue. | [EXE] `ART-E001` `0x0059f950`; full-image call-site scan | High | Confirmed | Queue editing from Lua (`ClearCommandQueue`, `AbortActiveTask`) and on issue (`TrimCommandQueue`) can *interrupt* the running sub-task but **never rotates the head**. `AbortActiveTask` notably does not pop. |
| `C-097` | `WP-12`, `WP-04` | The queue advances in the **command-dispatch stage**, confirmed structurally rather than by inference: `IAiCommandDispatchImpl`'s constructor registers its task thread into `Sim+0x958` (the stage the DLL names `GetCommandDispatchStage`), while `Entity::StandardInit` registers into the motion stage — the only two such registration sites. This independently confirms the stage identities of `C-054`. The full `EUnitCommandType` enum was also recovered (40 entries, `0 None` … `39 Dock`), cross-validated two ways: `GetNumBuildCmds` counts `type == 7` and sums their repeat counts, and `TaskTick`'s factory-repeat branch tests the same value, so `BuildFactory == 7`. | [EXE] `ART-E001`; [EXE] `ART-D001` `0x10189210`, `0x1025cee3`, string block `0x10736b94` | High | Confirmed | Command-queue status is broadcast as five events; `2` (cleared) and `3` (aborted) are what actually kill a running sub-task, via `TaskInterruptSubtasks`. |
| `C-098` | `WP-16` | **`C-017` is confirmed, with addresses.** Construction progress lives on the **target** at **`Entity+0xd8`**, written only through `Entity::UpdateFractionComplete`, itself called only from `Unit::Materialize` (`0x006b08e0`). The builder-side `Unit::GetWorkProgress` (**`Unit+0x2ac`**) is a **separate field that, for ordinary construction, is merely assigned the target's fraction at the end of every helper tick** — so `GetWorkProgress` on an assisting engineer reports the *shared* target progress, not that engineer's own share. It carries independent meaning only on the Lua work-item and silo-assist paths. | [EXE] `ART-E001` `0x00693af8`, `0x006b08e0`, `0x006d3838`, helper tail of `0x005fc6c0` | High | Confirmed | `Unit::Materialize` also drives health linearly off the same delta (`maxHealth × delta`), not off the resulting fraction — and `Materialize(0.0f)` is a deliberate **heartbeat** stamping "worked on this beat" when no progress is possible. |
| `C-099` | `WP-16` | `Entity::UpdateFractionComplete` is **asymmetric**, and this is the case that should differ: on an increase it clamps to `[0,1]` and then takes `max(n, health/maxHealth)` as a floor; on a **decrease there is no health floor**, only the clamp. | [EXE] `ART-E001`, inlined at `0x006b0a3x` | High | Confirmed | A reimplementation that applies the health floor symmetrically cannot reproduce a unit losing fraction (reclaim, capture-in-progress) correctly. |
| `C-100` | `WP-16`, `WP-15` | **The build-rate arithmetic, in one site** (`0x005fc9ea`–`0x005fca1f`): `delta = (1 / (targetBp.BuildTime / builderAttrs.BuildRate)) × builder.resourceConsumed × 0.1f`, with `resourceConsumed` being `Unit+0x53c` — the economy ratio of `C-071` — and `0.1f` the beat length. **All float32; no accumulator on the builder, no rounding, and the float returned by `Materialize` is discarded**, so the engine never reconciles "asked for" against "applied". `BuildTime` is `RUnitBlueprint+0x4f4`, `BuildRate` is `UnitAttributes+0x58`. | [EXE] `ART-E001` at the addresses given | High | Confirmed | **Construction is not all-or-nothing** — it scales smoothly with the economy ratio, unlike `CEconomyEvent` (`C-084`). Two different funding behaviours coexist in one engine, so "how does retail handle scarcity" has no single answer. |
| `C-101` | `WP-16`, `WP-04` | **The economy ratio a builder uses is one beat stale.** `Unit::HandleResourceManagement`, which writes `Unit+0x53c`, is called from `Unit::MotionTick` — the **motion** stage, which runs **third**. The build helper runs in the **command-dispatch** stage, which runs **first**. So a builder multiplies by the ratio computed at the end of the previous beat. | [EXE] `ART-E001` `0x006b1550`, call site `0x006afca5` in `0x006afaa0`; stage order from `C-054` | High (Medium-High that no earlier phase refreshes it — not exhaustively checked) | Confirmed | A one-beat lag between economy and construction, arising purely from stage order. Exactly the class of ordering fact that is invisible in a unit test and decisive in a replay. |
| `C-102` | `WP-16` | **Assist is independent contributions applied sequentially, not a summed rate.** Each assisting builder owns its own task and helper, computes its own delta from its own `BuildRate` and its own economy ratio, and calls `target->Materialize(delta)`. `UpdateFractionComplete` clamps **per call**, in task order — not on the sum. So on the final beat the first builder to tick can take the fraction to `1.0` and every later assister's contribution that beat is silently absorbed. **Order matters at the completion boundary.** Progress callbacks are not per tick: `OnBuildProgress`/`OnBeingBuiltProgress` fire only when the fraction crosses `0.25`, `0.5` or `0.75` relative to the helper's last report. | [EXE] `ART-E001` `0x005fc6c0` body | High | Confirmed | Three sub-paths diverge from the common one: Lua work items (enhancements) write `WorkProgress`/`WorkItemBuildTime` on the script table and never touch `Entity+0xd8`; silo assist hands **resources** rather than fraction; shield repair halves the materialize delta and doubles the health rate. |
| `C-090` | `WP-27`, `WP-26` | **A weapon never queries the spatial grid.** Each *unit* builds one cached candidate list at **`Unit+0x5b8`** (`0x006b36f0`), from a single layer-`0x100` grid query whose radius is `max(Sim+0x460, the unit's longest weapon range)`, filtered while building to: not dead, not queued-for-destruction, resolves to a unit, **enemy by the army bitset**, and **visible to my army**. Every weapon on the unit then re-scores that same unsorted list. The cache is rebuilt lazily on the acquisition clock (`0x006b3af0`: rebuild when `beat − Unit+0x668 >= ticks`). | [EXE] `ART-E001` `0x006b36f0`, `0x006b3af0`, `0x005de4e0` | High | Confirmed | One survey per unit, not per weapon — a meaningful cost structure, and it means all of a unit's weapons see an identical, identically-ordered candidate set. |
| `C-091` | `WP-27` | **The selection order is `(range/arc class, priority index, score, incumbency)` — not "first acceptable", and not nearest.** Read verbatim from `0x005de9f9`–`0x005dea7f`: a candidate whose range/arc class is worse than the incumbent's is dropped outright (class from `0x006dc5c0`: 0 in range and arc, 1 inside MinRadius, 2 out of arc/height, 3 beyond MaxRadius); a **lower priority index wins unconditionally**; only at equal priority is the score compared, and it must be **strictly** better. A candidate matching no priority row is never selected. Stickiness is explicit: the weapon's current target wins a tie against a challenger, and is re-accepted even when its score is worse. | [EXE] `ART-E001` `0x005de4e0` body | High | Confirmed | Because step 3 demands a *strictly* better score, **an exact tie is won by whichever candidate sits earlier in the cached list** — i.e. by the unsorted grid chain order of `C-065`. The ultimate tie-break in retail combat is arbitrary insertion history. |
| `C-092` | `WP-27` | **The "score" is not always distance.** By default it is 2-D squared distance with **Y ignored** (`0x005de693`). But when the weapon's blueprint sets `Turreted` (`+0x139`) or `SlavedToBody` (`+0x47`), the score becomes a weighted dot product against the weapon's **current aim direction** (`0x0044f7e0`) — that is *least turret slew*, not least distance. A candidate in a worse range/arc class is additionally penalised ×4 unless `AutoInitiateAttackCommand` is set. **There is no threat or value term anywhere** — no health, no cost, no DPS. | [EXE] `ART-E001` `0x005de693`, `0x005de91e`, `0x005dfe27`, `0x005de966` | High | Confirmed | A turreted weapon prefers the target it is already pointing at over the nearer one. That is a visible behaviour and nothing in Recoil Metal reproduces it. |
| `C-093` | `WP-27` | Re-acquisition cadence is per weapon: `ticks = max(1, ceil(TargetCheckInterval × 10)) + 1`, with `TargetCheckInterval` at blueprint `+0x50` (proven from the field registration at `0x00528e90`). After a successful acquire the task reschedules at **1** beat; a blueprint with **`ManualFire`** returns **−1** and therefore **never auto-acquires**. There is no explicit stagger — a weapon's phase is whichever beat its task was first scheduled on. | [EXE] `ART-E001` `0x005df807`–`0x005df84c`, `0x005df924` | High (Medium that the return value is a beat delay) | Confirmed | Contrast with the AI (`tick % 30`) and recon (`tick % armyCount`) staggers of `C-055`/`C-078`: targeting is *not* staggered, it is self-paced from content. |
| `C-094` | `WP-27` | Two negative results, both verified by exhaustive search. (a) **`TargetPriorities` is not an engine blueprint field** — the string does not occur anywhere in `ART-E001`, while `TargetRestrictOnlyAllow`, `TargetRestrictDisallow`, `TrackingRadius`, `TargetCheckInterval`, `TargetType` and `Turreted` all do. Lua reads the blueprint and pushes the list in through `SetTargetingPriorities` (`0x006e0fe0`), which refills `weapon+0x150`. (b) **`Unit:IsValidTarget`/`SetIsValidTarget` are inert to the engine**: they read and write `byte [Unit+0x2a1]`, and a scan of all `.text` for that displacement found 17 hits of which every one outside the two Lua wrappers is a jump displacement, not a field access. The accessors have **zero** call sites. The real untargetable flag is `SetDoNotTarget` → unit-state bit **34**, checked at `0x005de7b2`. | [EXE] `ART-E001` full-image string scan; `0x006cdb53`, `0x006ca4d0`, `0x005de7b2` | High | Confirmed | (b) is the kind of thing that would waste a day: a plausibly-named binding that does nothing the engine reads. Also a hard reject on the **`BENIGN`** category (`0x005de711`) and on being outside the playable map rect. |
| `C-095` | `WP-27`, `WP-29` | **The anti-projectile path is a different algorithm** — the case that should differ, and does. When a weapon's `TargetType` is `RULEWTT_Projectile` (blueprint `+0x130 == 1`), acquisition goes to `0x005dead0`, which issues **its own grid query each time** with layer mask `0x400` and radius `max(R, R × TrackingRadius)` into a 20-slot inline buffer. It has **no priority table, no range/arc class and no stickiness** — pure nearest in 2-D, with an exact tie again won by grid-walk order. It does have something the unit path lacks: an anti-overkill gate skipping targets already engaged by `DesiredShooterCap` shooters (blueprint `+0xd4`). | [EXE] `ART-E001` `0x005dead0`, `0x005dfd83` | High | Confirmed | Missile defence does not share the unit-targeting logic at all. Implementing one and reusing it for the other would be wrong in both directions. |
| `C-081` | `WP-29` | **Ammo is not on `Unit` at all.** All six Lua ammo bindings load an `IAiSiloBuild*` from **`Unit+0x558`** and dispatch through its vtable (`0x00e6d9a4`). `CAiSiloBuildImpl` holds, per silo type (stride 12 from `+0x08`): the owning `UnitWeapon*`, **stored ammo as a plain `int`**, and max storage — plus a build queue at `+0x20`, the current `CEconomyEvent*` at `+0x2C`, a 4-state machine at `+0x30`, per-tick mass/energy at `+0x34`/`+0x38`, **total ticks at `+0x44` and elapsed ticks at `+0x48`**. `ESiloType` is **0 = tactical, 1 = nuke**. The count is an `int` converted to a Lua float only at the boundary. | [EXE] `ART-E001` `0x005d5910`–`0x005d6061`, bindings `0x006d5aec` etc. | High | Confirmed | `PE-17` resolved: ammo is a counter and **build progress is entirely separate state**, not a fractional ammo value. `SiloIsFull` counts queued builds against capacity, so a silo never over-queues. |
| `C-082` | `WP-29` | **An SMD's anti-nuke interceptors occupy the *tactical* silo slot.** Slot assignment (`0x005d596c`–`0x005d5988`) takes weapons with `CountedProjectile != 0` and a projectile blueprint, then routes on the **`NukeWeapon`** flag alone: slot 1 for nuke weapons, slot 0 otherwise. An SMD's interceptor weapon sets `CountedProjectile = true, MaxProjectileStorage = 7` but **not** `NukeWeapon`, so `GetTacticalSiloAmmoCount()` on a UEB4302 returns its anti-nuke stock. | [EXE] `ART-E001` `0x005d596c`; [BP-R] `ART-S001` UEB4302 | High | Confirmed | Exactly the "case that should differ" — an implementation keying ammo off the *unit's* role rather than the *weapon's* `NukeWeapon` flag would put those interceptors in the wrong slot and silently break the Lua ammo bindings for every strategic missile defence. |
| `C-083` | `WP-29`, `WP-15` | **Silo production runs on `CEconomyEvent` against the army-level list, not the unit's `CEconRequest`** — the event is constructed at `0x005d5e44` and linked into `(Unit+0x154)->vt[9]()+0x58`. Per-tick costs derive from the **projectile** blueprint: `totalTicks = BuildTime × 10 / max(GetEconomyBuildRate(), 0.1)`, with mass and energy per tick scaled by the unit's build-adjacency modifiers. A separate, different funding path exists for assistance: `SiloAssistWithResource` (`0x005d5b00`) is driven from the *assistant's* `CEconRequest` and **loops**, so one assist call can advance several ticks. | [EXE] `ART-E001` `0x005d5cb0`–`0x005d6061`, assist site `0x005fc856` | High | Confirmed | Two funding routes with different shapes for one subsystem. Building your own missile and having an engineer assist it are not the same code path. |
| `C-084` | `WP-15`, `WP-16` | **Refines `C-070`(b), which was right but incomplete and read as more punitive than it is.** `CEconomyEvent` does require a whole tick's cost before advancing — but the delivered accumulator at `event+0x10`/`+0x14` **is only zeroed when the threshold is met** (`0x0077acd0`; the generic consumer `0x0077c800` exits at `0x0077c9bb` without consuming). So underfunding **neither wastes resources nor deadlocks**: partial deliveries accumulate and production simply proceeds in whole-tick granules at a proportionally slower rate. | [EXE] `ART-E001` `0x0077acd0`, `0x0077c800`, `0x005d5efa` | High (Medium on the end-to-end rate — the army-side distributor that writes the delivered pair was not disassembled) | Confirmed | "Skips the tick entirely" was accurate about the *tick* and misleading about the *resources*. The corrected reading matters: a starved silo is slow, not stalled, and nothing is lost. |
| `C-085` | `WP-29` | **Launch is decrement-then-fire with no reservation.** `UnitWeapon::ConsumeSiloAmmo` (`0x006dbfc0`) is a single unconditional `−1` guarded only by `count > 0`; `HasSiloAmmo` (`0x006dbf80`) returns **true** for any weapon whose blueprint lacks `CountedProjectile` or whose unit has no silo object. There is no pending or reserved field in the class. In practice the shipped Lua fires first and decrements after — `defaultweapons.lua:582-591` calls `CreateProjectileAtMuzzle` then `RemoveNukeSiloAmmo(1)`, and those are the **only two call sites** of the Remove bindings in the whole corpus. | [EXE] `ART-E001` `0x006dbf80`, `0x006dbfc0`; [LUA-R] `ART-S010` `lua/sim/defaultweapons.lua:582-591` | High | Confirmed | `Unit::GetCountedProjectileWeapon` (`0x006b1fb0`) returns the **first** weapon with `CountedProjectile`, ignoring `NukeWeapon` — so on a unit carrying both it is order-dependent. |
| `C-086` | `WP-29`, `WP-26` | **Interception is the ordinary shared collision path; there is no interceptor manager.** Projectiles are ordinary entities registered with layer mask **`0x400`** (`0x006a16e1`), and `Projectile::CheckCollision` (`0x006a3c60`) issues **one** swept query with mask **`0xD00`** — units, projectiles and collision beams, with **props deliberately excluded**. The accept gate is **Lua on the target**: `0x006a6140` looks up `"OnCollisionCheck"` on the candidate's script object and **returns false when no script exists**. `DealDamage` separately queries `0xF00`, which includes projectiles, so area damage genuinely reaches missiles in flight. | [EXE] `ART-E001` `0x006a3c60`, `0x006a434e`, `0x006a6140`; [LUA-R] `ART-S007` `lua/sim/Projectile.lua:89` | High | Confirmed | The entity-layer table is now complete: `0x100` unit, `0x200` prop, `0x400` projectile, `0x800` collision beam — matching `C-064`. `PE-17`'s "dedicated interceptor manager" candidate is **refuted**. |
| `C-087` | `WP-29` | **Interception damages; it does not force-destroy.** Projectiles inherit `Entity` and do not override `AdjustHealth`/`Kill`, and 34 projectile blueprints declare `Defense.MaxHealth` — tacticals 1–3, nukes **25**, the Seraphim experimental missile 60. An SMD interceptor deals 30 and one-shots a nuke; the UEF TMD "Phalanx" deals 1 and needs several hits. `Projectile.lua:358`'s `OnImpactDestroy` explicitly **keeps the struck missile alive** when the thing it hit is `ANTIMISSILE * ALLPROJECTILES` — the missile dies only from losing health. A projectile with no `MaxHealth` dies to any damage. | [EXE] `ART-E001`; [BP-R] `ART-S013` projectile blueprints; [LUA-R] `ART-S007` `lua/sim/Projectile.lua:128,358` | High | Confirmed | Missile interception is an ordinary combat exchange with hit points, not a special-cased destroy. That is implementable with machinery Recoil Metal already has. |
| `C-088` | `WP-29` | Retail ships **three distinct counter-missile mechanisms**, not one. (a) **TMD** (`U?B4201`): no ammo at all, free-firing, `TargetRestrictOnlyAllow = 'TACTICAL MISSILE'`. (b) **SMD** (`U?B4302`): counted ammo of 7, `'STRATEGIC MISSILE'`. Both use `TargetType = 'RULEWTT_Projectile'`, `WeaponCategory = 'Defense'`, `RangeCategory = 'UWRC_Countermeasure'`. (c) **Aeon flares** — `Weapon.Flare = {Category='MISSILE', Radius}` on exactly 3 blueprints (UAB4201, UAS0202, UAS0302): a `Flare` entity attaches to the incoming projectile, its `OnCollisionCheck` **returns false**, and it calls `other:SetNewTarget(self.Owner)`. It **diverts, never damages**. Plus the Cybran Loyalist (URL0303, the only unit with `Defense.AntiMissile` in all 568 blueprints), which turns a missile back on its own launcher. Corpus counts: `NUKE` 10, `ANTIMISSILE` 22, `SILO` 26, `STRATEGIC` 23; the tokens `TACTICAL`, `MISSILEPLATFORM` and `OVERLAYANTIMISSILE` **do not exist** in retail. | [BP-R] `ART-S001`; [LUA-R] `ART-S007` `lua/sim/defaultweapons.lua:121`, `defaultantiprojectile.lua` | High | Confirmed | Aeon TMD is a decoy system wearing an interceptor's clothes. Implementing "anti-missile" as one mechanism would get one of the four factions wrong. |
| `C-089` | campaign method, `WP-00` | **Further evidence for `C-075`, and a concrete measurement of the drift.** In the silo/unit region, `Moho::Unit` field offsets in `ART-D001` run **0x10 lower** than in `ART-E001` (e.g. the silo pointer is `+0x548` in the DLL and `+0x558` in the executable; the spatial DB is `Sim+0x918` vs `Sim+0x908`). `CAiSiloBuildImpl`'s own layout is byte-identical between the two. | [EXE] `ART-E001` vs `ART-D001`, offsets in the session-12 record | High | Confirmed | The two builds differ by an inserted member in `Unit` and in `Sim`. **Never transfer an offset from the DLL** — only names. A third independent confirmation, after the intel divergences, that the DLL is a naming oracle only. |
| `C-075` | campaign method, `WP-00` | **`ART-D001` and `ART-E001` are DIFFERENT BUILDS and their behaviour differs. The DLL is a naming oracle, not a behavioural one.** Two independent divergences found in one subsystem: (a) `CIntelGrid::DelayedSubtractCircle` exists in retail at `0x0050E1B0` but has **zero callers** — proven by a full `.text` relative-call and relative-jump scan, a full-image dword-reference scan, and a scan for an inlined `mov [mem],0x1E` — while the DLL calls it from the `CVizHandle` destructor; (b) the DLL's `Raster` samples the heightmap and skips terrain-occluded cells, and **retail's `Raster` at `0x0050E040` has none of that**. | [EXE] `ART-E001` `0x0050E1B0`, `0x0050E040`, `0x00774DF0`; [EXE] `ART-D001` `0x100F4220`, `0x100F3E30`, `0x10338B05` | High | Confirmed | **This bounds `C-002`.** Same Perforce tree, yes — but a symbol name transferred from the DLL says what a function *is called*, never what retail *does*. The direction of drift is unsettled: the agent inferred the DLL is later, whereas the PE timestamps in the artifact manifest say the DLL is 2007 and the executable 2011, which would make the DLL **earlier** and these features *removed* before retail. Either reading supports the same rule; do not assert a direction without more evidence. |
| `C-076` | `WP-33` | **Retail has no deferred vision removal and therefore no ghost-vision window.** Every removal path is immediate: the `CVizHandle` destructor (`0x00774DF0`) divides the radius by the grid scale and calls `Raster(..., add=false)` directly, as do `CVizHandle::Remove` and the counter-intel teardown loop. `CIntelGrid::Tick` (`0x0050E2B0`) exits at its first branch every time because the pending list is always empty — the only other writer of that list is the savegame **deserializer**. The dead `DelayedSubtractCircle` would have used a countdown of **30** (`movl $0x1e` at `0x0050E1F4`). | [EXE] `ART-E001` at the addresses given | High | Confirmed | **Reverses the expectation set in `C-007`**, which read `DelayedSubtractCircle`'s existence as evidence of deferred removal and called the delay "an observable ordering difference". It is not: a unit that dies or moves stops contributing in the same beat. Recoil Metal's immediate removal already matches retail. |
| `C-077` | `WP-33` | Intel is **nine separate grids** on the recon DB, each a `CIntelGrid` of **signed bytes** — a refcount/coverage grid, not a bitmask: `Raster` (`0x0050E040`) computes `dl = add ? +1 : -1` and does `addb %dl,(cell)`, so overlapping sources stack, `GetCoverage` returns the raw count and `IsVisible` is `GetCoverage != 0`. Rasterisation is a **true filled circle** (`dz = (int)sqrtf(r² − dx²)`), and radius is converted to cells by **truncating unsigned division** `radius / scale`. **One cell is exactly `scale` world units.** Vision ("fog") is scale **2**; radar, sonar, omni, water and the three counter-intel grids are scale **4**; the explored map is a genuine bitmask (`CIntelXGrid`) at scale 2. | [EXE] `ART-E001` `0x0050E040`, `0x0050E220`, `0x0050D920`, accessors `0x005C84E0`–`0x005C8630` | High | Confirmed | Radar/sonar/omni cells are **exactly the 4-unit spatial-grid cell size**; vision is 2× finer per axis, so 4× the cells. Coverage semantics mean two overlapping radars leave coverage when one dies — a bitmask would not. |
| `C-078` | `WP-33`, `WP-04` | Recon cadence: **every army is touched every beat, but only one gets the expensive pass.** In `Sim::AdvanceBeat` the code computes `beat % armyCount` and, per army, calls `ReconTick(armyCount)` (`0x005C7710`) for the selected index and the cheap `ReconRefresh` (`0x005C7FB0`) for all others. So each army's heavy pass runs once every `armyCount` beats, and `CIntelGrid::Tick` is called only from `ReconTick`, once per each of the eight grids. `ReconRefresh` walks the army's known-blip tree and resyncs each blip against its source unit. | [EXE] `ART-E001` `0x007514ED`–`0x0075155E`, vtable `0x00E6D4A4` | High | Confirmed | A second staggering mechanism alongside the AI's `tick % 30` (`C-055`), and it explains why the delayed-subtract countdown was decremented by `armyCount` rather than 1 — the deferral was designed around this stagger. |
| `C-079` | `WP-33` | **Retail keeps stale contacts.** `ReconBlip` exposes `IsSeenNow` (`0x005C9950`), `IsSeenEver` (`0x005C97E0`), `IsMaybeDead` (`0x005C9AC0`) and `IsKnownFake` (`0x005CA080`) — a distinction that only means anything if a blip outlives its contact. Shipped Lua relies on it: `lua/platoon.lua:1365,1512` loop `until allIdle or blip:BeenDestroyed() or blip:IsKnownFake(index) or blip:IsMaybeDead(index)`. Blips are reaped explicitly, not implicitly: the recon DB parks blips whose source is gone on a separate pending list, re-evaluates them on the staggered `ReconTick`, and only then calls `DestroyIfUnused`. | [EXE] `ART-E001` at the addresses given; [LUA-R] `ART-S007` `lua/platoon.lua` | High (Medium on the exact re-detect predicate) | Confirmed | Ghost radar dots are a real retail feature with a Lua-visible three-state identity, not an artefact. `WP-33` parity needs remembered contacts, which Recoil Metal does not model. |
| `C-080` | `WP-02` | A data-quality correction to `C-032`'s table, and a vindication of `C-039`'s precedence rule. Several `ReconBlip` methods are documented in the recovered signatures as taking **no arguments**, but their native arity checks demand **2** (`cmpl $0x2,%eax` at `0x005C9AF3` for `IsMaybeDead`) and shipped Lua calls them as `blip:IsMaybeDead(index)`. | [EXE] `ART-E001` `0x005C9AF3`; [LUA-R] `ART-S007` `lua/platoon.lua:1365` | High | Confirmed | These are instances of the 53 empty-parenthesis doc stubs counted in `C-039`. **The arity check is authoritative; the shipped doc string is not.** Anything consuming `moho.methods.tsv` must prefer the verified arity. |
| `C-072` | `WP-19` | **Two transcription defects found in Recoil Metal's own adjacency tables by auditing against `C-050`'s invariant, and fixed.** `T1EnergyStorage`'s energy-production row was **every value doubled** — `{0.25, 0.125, 0.083334, 0.0625, 0.05}` against retail's `{0.125, 0.0625, 0.041667, 0.03125, 0.025}` — so a full ring paid **+100% where retail pays +50%**. `T1MassStorage` was correct in four rows and had `0.03` at SIZE12 where the file says `0.041667`, making a twelve-ring pay **+36% instead of +50%**. | [LUA-R] `ART-S007` `lua/sim/AdjacencyBuffs.lua`; [RM] `src/core/unit/Adjacency.cpp` | High | Confirmed and fixed | The **`Add × n` invariant is what made both visible** — a value that breaks the constant product is a transcription slip. Neither defect is findable by playtesting: an energy ring twice as strong as retail reads as a balance opinion, not a bug. This is the strongest argument yet for recovering a design invariant alongside the numbers. |
| `C-073` | `WP-19` | Three further divergences found by the same audit, **not yet fixed**, recorded so they are decisions rather than oversights. (a) **Skirt rectangles are centred on the unit**; `Physics.SkirtOffsetX/Z` is never parsed and `Footprint` is never read, whereas retail computes `pos − Footprint.Size/2 + SkirtOffset` extending by `SkirtSize`. A census of all 568 blueprints found **31 structures with a genuinely off-centre skirt**, including all twelve naval factories (12×14 skirt displaced by up to 44 elmos in z) — so naval-factory adjacency is placed wrongly. (b) A structure with **no SIZE category has its size derived from the skirt**, an invention: retail gates every buff on `EntityCategory = 'STRUCTURE SIZEn'`, making all **174** such structures permanently inert. (c) Modifiers are **clamped at zero**; retail applies no `Ceil`/`Floor` to any adjacency buff, so the modifier can legitimately go negative. | [BP-R] `ART-S001` census; [LUA-R] `ART-S007` `lua/sim/Buff.lua`; [RM] `src/core/unit/UnitBlueprint.cpp`, `src/core/sim/UnitCatalog.cpp`, `src/core/sim/Adjacency.cpp` | High | **Open** | (a) is the consequential one. **The blocking unknown is now answered — `C-109`: the default is `ceil(SizeX)` from the top-level key, plus two skirt clamps. The fix is writable.** (b) and (c) are latent until build-cost adjacency lands. |
| `C-074` | `WP-19` | Two deliberate deviations in Recoil Metal, confirmed as deviations rather than defects and left alone: adjacency tolerates a **half-ogrid gap** (`kAdjacencyGapElmos = 4`) and treats **overlapping** skirts as adjacent, where retail requires exact edge contact with zero gap and zero overlap. Both are documented in the source as concessions to free placement — retail can demand exactness because it snaps structures to a build grid. | [RM] `src/core/sim/Adjacency.hpp:28-36`, `Adjacency.cpp:16-19`; [LUA-R] `ART-S007` `EffectUtilities.lua` beam placement compares skirt edges with `==` | High | Confirmed as intentional | Worth re-affirming rather than silently keeping: they exist because this engine has no build-grid snap. If snapping ever lands, both should go. |
| `C-067` | `WP-15` | **Retail's scarcity allocator is a TWO-RATIO scheme, not a global throttle and not sequential consumers.** The allocator (`0x007790e0`, ~4.6 KB) walks the intrusive request list at `CEconomy+0x58`, computes `outstanding = max(0, demand - allocated)` per request, and splits demand into two buckets by **how many resources a request needs** (multi-resource vs single). Then: `S[k] = stored[k] + income[k]`; **`r1 = min_k(S[k]/T[k])` capped at 1.0**, recording `k*`, the *binding* (scarcest) resource; `Rem[k] = max(0, S[k] − A[k]·r1)`; **`r2 = min over k ≠ k* of (Rem[k]/B[k])`** capped at 1.0. In the second pass each request is granted `outstanding × r1` **if it needs the binding resource at all**, else `outstanding × r2`. | [EXE] `ART-E001` `0x007790e0`, branch at `0x7793f8`–`0x77940b` | High | Confirmed | **The discriminating case:** while mass-stalled, a pure-energy consumer (shield, radar) is throttled by the *separate, more generous* `r2` from leftover energy rather than by the mass ratio. A single global throttle would use one ratio for everything; sequential consumers would drain supply inside the loop. Both are refuted — `PE-11` resolved. |
| `C-068` | `WP-15` | Economy object layouts, with **resource index `[0] = ENERGY`, `[1] = MASS`** (established three independent ways from paired stat names). `CEconomy`: `+0x08/+0x0c` production accumulator consumed as income, `+0x10/+0x14` a second accumulator used only for the trend stat, `+0x18/+0x1c` **stored**, `+0x20/+0x24` **income**, `+0x30/+0x34` **requested** (total outstanding), `+0x38/+0x3c` **usage** (total granted), `+0x40+8i` **max storage as an unsigned 64-bit integer**, `+0x54` resource-sharing flag, `+0x58` request-list head. `CEconRequest` is 24 bytes: `{prev, next, demand[2], allocated[2]}` with `GetRatio()` = `min_k(allocated[k]/demand[k])` capped at 1. `CEconStorage` is 12 bytes `{CEconomy*, capE, capM}`. On `Unit`: `+0x470/+0x474` consumption per second E/M, `+0x478/+0x47c` production per second E/M, `+0x52c` storage, `+0x534` request, `+0x538` consumption-active, `+0x539` production-active, `+0x53c` the consumed ratio, `+0x154` the army. `CArmyImpl+0x1f4` is the `CEconomy*`. | [EXE] `ART-E001` at the addresses in the session-10 record | High | Confirmed | The largest single addition to the object-layout table. `Unit::SetConsumptionActive 0x006b1390` is now **confirmed**, upgrading it from the BSim candidate in `C-042`. |
| `C-069` | `WP-15` | **There is no integer rounding anywhere in the allocation path.** Per-second rates become per-tick by `× 0.1f` (constant `0x00e4cea4`), ratios are `divss` capped at `1.0f`, grants are `mulss`, and stored is `max(0, min(S, cap))` — all IEEE-754 **binary32**. The *only* truncation in the whole economy is storage **capacity**: each contributing structure's blueprint float is converted with `__ftol2` (truncate toward zero) and accumulated into a **64-bit integer** at `CEconomy+0x40+8i`. | [EXE] `ART-E001` `0x0077a850`, `0x6b13d1`, `0x7792e9`, `0x779383` | High | Confirmed | Answers `PE-11`'s rounding question: the rounding point is storage capacity and nothing else. Determinism therefore rests on float32 operation order, and the request-list iteration order matters for the running `S[k] -= grant` bookkeeping — though not for the grants themselves, since both ratios are precomputed. |
| `C-070` | `WP-15`, `WP-16` | Three economy behaviours a reimplementation would not guess. (a) **Deactivating a consumer refunds it**: `SetConsumptionActive(false)` adds the request's unspent `allocated[]` back into the army income pool and zeroes it (`0x6b1466`–`0x6b14b5`). (b) **`CEconomyEvent` is all-or-nothing** (`0x0077c800`): if `allocated < cost` for *either* resource it skips the tick entirely rather than progressing partially, unlike ordinary consumers. (c) **Excess above storage is shared equally**: when `CEconomy+0x54` is set, overflow is split `1/n` among allies that have free storage, each capped by its own headroom (`0x7797c0`). | [EXE] `ART-E001` at the addresses given | High | Confirmed | (b) is a real gameplay difference — enhancements and similar timed events stall completely under partial funding instead of slowing down. |
| `C-071` | `WP-15`, `WP-38` | `GetResourceConsumed` (`0x006d1440`) is a plain read of `Unit+0x53c`, written each tick from `CEconRequest::GetRatio()` — so it is **`min over {energy, mass} of granted/demanded`** for that unit this tick. Separately, "trend" has **two incompatible definitions**: `CAiBrain::GetEconomyTrend` (`0x005967c0`) returns `income[i] − usage[i]`, while the `Economy_Trend_*` army *stat* is `(CEconomy+0x10+4i − CEconomy+0x30+4i) × 10.0`. | [EXE] `ART-E001` `0x006d1440`, `0x006b1550`, `0x005967c0`, `0x00779fe3` | High | Confirmed | The two trends will disagree; AI code and UI code are reading different numbers. Worth knowing before matching either. |
| `C-058` | `WP-30` | **`C-042` was wrong about `SIM_MetaImpactArea`: `0x0073e950` is a physics IMPULSE routine, not damage.** It is called only by the `MetaImpact` binding; it normalises a target-to-centre vector, scales it, and calls a unit-motion routine — it never consults armour, never calls the damage applier, and contains no store to health. It does consult the shield list (`0x0073d860`), so shields block the *impulse*. The BSim name was right; my reading of what the name meant was not. | [EXE] `ART-E001` `0x0073e950`, callee set and body `0x73eb23`–`0x73ebef` | High | **Refutes `C-042`'s damage attribution** | A `maybe_` name was treated as a lead, which is exactly right — but the *interpretation* ("MetaImpactArea must be area damage") was assumed rather than read. The name told us the function; it did not tell us the subsystem. |
| `C-059` | `WP-30` | **The real damage funnel is `Moho::DealDamage` at `0x0073dbc0`**, identified by its own format string `"DealDamage(target=0x%08x, amt=%.1f)\n"` (`0x00e826ec`). All three scripted damage entries converge on it through a dispatcher `0x0073e8e0` that switches on `DamageInfo+0x34`: kind 0 point → direct, kind 1 sphere → `0x0073e100`, kind 2 ring → `0x0073e5b0`, each calling `DealDamage` per entity. Its complete caller set is `{0x006ddd80, 0x0073e100, 0x0073e5b0, 0x0073e8e0}`. | [EXE] `ART-E001`; `callgraph.tsv` convergence | High | Confirmed | This is what `C-021` predicted from the DLL symbol `SIM_Damage`, finally located. |
| `C-060` | `WP-30` | **The engine never writes health on the damage path — it computes a final amount and hands it to Lua.** `DealDamage`'s order is: return if `amount == 0`; self-damage guard; **shield interception** (area/ring only, `0x0073d8c0`, and therefore **before armour**); **armour multiply** (`0x006b07f0`, a `std::map` lookup keyed by damage-type string, multiplier is a `float` at node `+0x28`, **default 1.0 on miss**); **handicap divide** by `(h+1)` using the **target's** army; stats and the `OnDamageBy`/`OnExtraDamageDealt` events; return if `amount <= 0`; then `0x00740f70` dispatches Lua `Unit:OnDamage`. Lua's `DoTakeDamage` calls `AdjustHealth`, and only then does `Entity::SetHealth` (`0x006803d0`) store to `+0x90`. **If a target has no Lua script object, `0x00740f70` returns immediately and no damage is applied at all** — there is no engine fallback. **Death is decided in Lua**, not the engine: `AdjustHealth` clamps at 0 and returns. | [EXE] `ART-E001` `0x0073dbc0`, `0x006b07f0`, `0x00740f70`, `0x006802f0`, `0x006803d0`; [LUA-R] `ART-S007` `lua/sim/Unit.lua:787,794` | High | Confirmed | Shields before armour, and a **direct scripted `Damage()` call bypasses shields entirely** because the point path never consults them. Both are ordering facts a reimplementation will get wrong by default. |
| `C-061` | `WP-30` | **Retail area damage has NO distance falloff.** Every entity inside the radius takes the full amount. The agent looked specifically for the discriminating case: in `0x0073e100` the only thing that modifies `amount` between the spatial query and `DealDamage` is `0x0073d8c0`, which is shield absorption — a **flat subtraction** of a per-shield value with no distance term. The centre and radius parameterise the spatial query only; the target-to-centre delta is written into `DamageInfo+0x80` as a *direction* and never multiplied into the amount. Ring damage (`0x0073e5b0`) is structurally identical with min/max radii. Per-target filters are an allegiance test and a `NOSPLASHDAMAGE` category test. | [EXE] `ART-E001` `0x0073e100`, `0x0073e5b0`, `0x0073d8c0` | High | Confirmed | **A direct, live parity defect in Recoil Metal**, which applies `share = 1 - distance/radius` in `damageArea` (`src/core/sim/Combat.cpp`). Retail is uniform. This changes every blast's totals and therefore every downstream death, wreck and hash. |
| `C-062` | `WP-30` | Shield absorption is **scripted, not hardcoded**. `0x0073d930` builds a per-shield table up front from the world shield list at `Sim+0xa3c`, calling `0x00740db0` per shield — a Lua dispatcher for `"OnGetDamageAbsorption"`, matching `lua/shield.lua:100`. The returned float is stored per entry and subtracted flat; afterwards each shield is itself run through `DealDamage` with that amount. | [EXE] `ART-E001` `0x0073d930`, `0x00740db0`; [LUA-R] `ART-S007` `lua/shield.lua:100` | High | Confirmed | Shield policy is a script decision, so `WP-31` parity depends on the Lua, not on engine geometry. |
| `C-063` | `WP-30` | `Entity::SetHealth` (`0x006803d0`) quantises for its callback: it computes `floor(fraction * 4) * 0.25` for old and new health and fires the script event `"OnHealthChanged"` **only when the quarter-of-maximum bucket changes**. It then relinks the entity into the sim's dirty list at `[entity+0x148]+0xa5c`. Corroborated by shipped Lua — `sim/Unit.lua`'s `ManageDamageEffects` comments that "Health values come in at fixed 25% intervals". | [EXE] `ART-E001` `0x006803d0`; [LUA-R] `ART-S007` `lua/sim/Unit.lua` | High | Confirmed | Ties `C-057` together: the dirty list the checksum walks is maintained by the health write itself. |
| `C-064` | `WP-26` | **Retail's broad phase is one global uniform grid**, living inside `Moho::COGrid` at `Sim+0x908` (field `+0x04`; the occupancy bitmaps are separate members at `+0x38/+0x48/+0x58` of the same object). Cells are **4 world units**; the cell array is flat and row-major with `index = (z << shift) + x`, `shift = bsr(cellsX)`. Entities carry an embedded record at **`Entity+0x4C`** (cached cell rect, grid pointer, dedup flag, layer flags) and every consumer converts a result back with **`-0x4C`**. Insertion is **head-insert** into a per-cell singly-linked chain, with nodes from a LIFO free list; an entity occupies **every cell its box overlaps**, and moving re-inserts it at the **front** of each new chain. Three bucket arrays are selected by layer: `0x100` units and recon blips, `0x200` props, `0x400|0x800` projectiles, shields, beams and plain entities. | [EXE] `ART-E001` `0x00727030`, `0x00503820`, `0x00503a20`, `0x00503ee0`; RTTI `.?AVCOGrid@Moho@@` at `0x00fcbff8` | High | Confirmed | Trees, quadtrees and BVHs are **ruled out** — no recursion, no stack, no split logic anywhere in `0x00503640`–`0x00504110`. `EntityDB` is ruled out as the spatial index; it never appears on any path to the query. |
| `C-065` | `WP-26`, `WP-43` | **The query order is: cell row-major (z outer ascending, x inner ascending) → layer array A, B, C → chain position (LIFO by most-recent insertion or cell change). Nothing is ever sorted — not by distance, not by id, not by insertion order.** Read directly from the rect query `0x00503b00`, which also sets a per-record dedup flag on first hit and clears it in a final pass. The segment/ray query `0x00503d00` is a DDA walk whose **per-cell order is the opposite** (array C filtered, then array A) and which **never visits props at all**. | [EXE] `ART-E001` `0x00503b00`, `0x00503d00` | High | Confirmed | **The most fragile ordering fact in the campaign.** Retail's tie-break in area damage and target acquisition depends on entity creation and movement *history*. It is deterministic in lockstep because all clients replay the same operations — but it is **not reproducible by any implementation that iterates by id, by insertion order, or by distance**, which is what Recoil Metal does. |
| `C-066` | `WP-26` | One index, shared. Damage (`0x0073e100`, `0x0073e5b0`, and the impulse `0x0073e950`), projectile collision (`0x006a3c60`), beam weapons (`0x006dd580`), build placement (`0x00727810`), melee spacing and unit recon (`0x006b36f0`) all reach `0x00503b00` or `0x00503d00` through wrappers `0x00729870`/`0x007298b0`, always on the same instance `[Sim+0x908]+4`. Movement and pathing instead reach the COGrid **occupancy bitmaps** and never the entity grid; build placement uses **both**. The Lua `*InRect` bindings apply **no narrow phase at all** (`0x00762410`), so they return a cell-granular superset quantised to 4 units. | [EXE] `ART-E001`; `callgraph.tsv` BFS paths | High | Confirmed | "Per-subsystem indices" is refuted for every sim consumer checked. Recoil Metal's separate indices are a structural divergence, though a defensible one. |
| `C-053` | `WP-30`, `WP-03` | **`C-038` was mislabelled, and the mistake is instructive.** Health is a `float` at **`Entity+0x90`**, max health at **`Entity+0x94`**. `C-038` recorded it as "Entity+0x98" from `Unit::GetHealth` (`flds 0x98(%esi)`), but that `esi` is the pointer the **`Unit` resolver** returns, which sits 8 bytes below the `Entity` base. Proved by comparing resolvers: `Entity::GetHealth` (`0x00693e48`) reads `flds 0x90(%esi)`, `Entity::GetMaxHealth` (`0x00693f88`) reads `+0x94`, while `Unit::GetHealth` (`0x006cb839`) reads `+0x98`; and `Sim::UpdateChecksum` walks the dirty list, adjusts the intrusive node with `add edi,-0x60` to reach the entity, then reads id at `+0x68` and health at `+0x90`. `UserUnit::GetHealth` (`0x008c9d48`) reads `+0x68` of a different object entirely — the UI-side mirror. | [EXE] `ART-E001` at the five addresses given | High | Confirmed | **An offset is meaningless without naming the base pointer it is relative to.** With multiple inheritance a `Unit*` and its `Entity*` differ by a constant, so "the entity pointer" is ambiguous. Every row of the object-layout table now names its base. The *fact* in `C-038` — health is a 32-bit float — stands, and so does its parity consequence. |
| `C-054` | `WP-04` | **The phase order inside one beat is command dispatch → script (Lua) → motion.** `Sim::AdvanceBeat` is at `0x00751300` (identified three ways: vtable slot 22 at `.rdata 0x00E841C4` = offset 0x58, the slot the per-beat driver calls; the `"********** beat %d **********"` log at `0x0075132B`; and the tick increment at `0x007513A3`). At `0x007514CA`–`0x007514E6` it makes three consecutive calls to the `CTaskStage` runner `0x00409AC0` on `sim+0x958`, `sim+0x944`, `sim+0x930`, which the DLL's one-instruction exported accessors identify as `CommandDispatchStage`, `ScriptStage` and `MotionUpdateStage` respectively. | [EXE] `ART-E001`; stage identities via [EXE] `ART-D001` exported accessors `0x101336C0/B0/A0` | High | Confirmed | The single most consequential ordering fact for `WP-04`. Commands are dispatched **before** Lua runs, and motion happens **after** Lua — so a script reacting on beat *n* sees the results of that beat's commands but not yet its movement. |
| `C-055` | `WP-04` | The full beat sequence, read from `Sim::AdvanceBeat`: log beat number; **early-out if game over** (`sim+0x8DD`, skipping most of the beat); `++tick` (`sim+0x900`); per-unit accumulator reset; **per-army update** (`CArmyImpl` `0x007067F0` — economy, unit-cap stats, three more army-level task stages, and AI staggered by `tick % 30 == armyIndex`); the three stages of `C-054`; **intel** (`ReconBlip::Refresh` per blip, `0x005CA1E0`); **recon DB staggered** by `i == tick % armyCount`; effect manager and formation DB; **death cleanup** (`Unit::KillCleanup 0x006AF220`); dirty-entity list → `Entity::AdvanceCoords 0x0067F9A0`; a pose/extra-data gather; a double-buffer swap. Then, after the join point, the **OnDestroyedQueue** drain, effects/decals, the periodic checksum, a per-beat observer list, and a 70-tick log flush. | [EXE] `ART-E001` `0x00751300`–`0x007519BA` | High (steps 14–15 Low, undecoded) | Confirmed | Two independent staggering mechanisms — AI by `tick % 30`, recon by `tick % armyCount` — are the kind of load-spreading that changes *when* things happen without changing *what*. Recoil Metal's `SlowUpdate` is the same idea, independently arrived at. |
| `C-056` | `WP-04`, `WP-43` | **The RNG is bit-exact stock MT19937**, and the obvious way to test for it fails. `CMersenneTwister::IRand` is at `0x0040E9F0`, the twist at `0x0040EBB0` with `mag01` at `.data 0x00FBCAE4 = {0, 0x9908B0DF}`, `Seed` at `0x0040EB60` using the standard `1812433253` recurrence, and `CRandomStream`'s default seed is **5489** — the MT19937 reference default. Ranged draws are half-open with a 64-bit multiply-high (`(u64)r * n >> 32`), so **no modulo bias**. The sim's stream is one `CRandomStream` allocated in `Sim::Setup` (`0x0074B3C0`) seeded from `LaunchInfoNew+0xB0`, stored at `Sim+0x904`. | [EXE] `ART-E001` at the addresses given | High | Confirmed | **The trap:** scanning `.text` for the four classic MT constants finds `0x9908B0DF` and `0x6C078965` but **zero hits for `0x9D2C5680` and `0xEFC60000`** — the compiler hoisted the tempering masks before the shifts (`and edx,0xFF3A58AD; shl edx,7`), which is algebraically identical. A constant scan would have wrongly concluded "not MT19937". |
| `C-057` | `WP-43` | The per-beat checksum is a running `gpg::MD5Context` at `Sim+0x50`, **never reset**, snapshotted into a 128-entry ring at `Sim+0xB0` indexed `beat & 0x7F`. `Sim::UpdateChecksum` (`0x00751A00`) feeds, in order: per army 56 bytes of economy state; per army the recon DB's own checksum; then per **dirty** entity — id (`+0x68`), health (`+0x90`), blueprint id string, transform (`+0xAC`, 28 bytes), velocity (12 bytes); then the RNG — the 624-word MT state and the cached-Gaussian flag and value. **`mti` is deliberately not hashed**, and neither is max health. Cadence is the convar `sim_ChecksumPeriod`: `if (beat % period == 0)`. Mismatches report through `Sim::VerifyChecksum` (`0x0074FB80`). | [EXE] `ART-E001` `0x00751A00`–`0x0075215D` | High (default period value Low — not recovered statically) | Confirmed | Only **dirty** entities are hashed, not all of them, so retail's checksum is a change-set digest rather than a full state hash. That is a materially different guarantee from Recoil Metal's `StateHash`, which walks everything. |
| `C-048` | `WP-19` | **Adjacency stacks by COUNT, not by replacement** — the opposite of veterancy, and the single highest-risk detail in the subsystem. All 95 adjacency buffs declare `Stacks = 'ALWAYS'`, which `Buff.ApplyBuff` does *not* special-case (it handles only `'REPLACE'` and `'IGNORE'`), so application falls through to the counting path: `Affects[type][buffName].Count += 1`. `BuffCalculate` then sums `Add × Count` per buff name. Every adjacency buff has `Duration = -1`, `Mult = 1.0` (inert) and a single `Add`, and is computed from `initialVal = 1`, so **`AdjMod = 1 + Σ(Add_b × Count_b)`**. Count is keyed by **buff name, not giver**, so three adjacent T1 power generators on a SIZE16 factory give `Count == 3`. Different producer families stack additively. **No `Ceil`/`Floor` on any adjacency buff — the value is unclamped and can go negative.** | [LUA-R] `ART-S007` `lua/sim/Buff.lua:61-65,73-75,77-91,102-115,452-499`; `lua/sim/AdjacencyBuffs.lua` (95 definitions) | High | Confirmed | Contrast with `C-028`: veterancy uses `'REPLACE'` and does **not** stack. Two buff families in the same engine with opposite stacking rules — generalising from either would be wrong, which is exactly the failure mode this campaign keeps hitting. |
| `C-049` | `WP-19` | Adjacency participation is gated three times, and most structures fail. **Giver:** must be a `StructureUnit` subclass (the `OnAdjacentTo` hook exists nowhere else, so mobile units never give) *and* carry a top-level blueprint `Adjacency` string naming a table in `AdjacencyBuffs.lua` — **44 of 568 units**, being 11 families × 4 factions. **Receiver:** every adjacency buff sets `EntityCategory = 'STRUCTURE SIZEn'`, so the receiver must be a `STRUCTURE` carrying one of `SIZE4/8/12/16/20` — **187 of 568 qualify, and 174 STRUCTUREs are permanently inert** for lacking a SIZE category (walls, civilian props). **Effect:** a per-buff `BuffCheckFunction` then tests eligibility (has a buildable category, has energy upkeep, has an energy weapon, has non-zero production, etc.). | [LUA-R] `ART-S007` `lua/defaultunits.lua:357-369`; `lua/sim/Buff.lua:46-57`; `lua/sim/AdjacencyBuffFunctions.lua:24-157`; [BP-R] `ART-S001` census | High | Confirmed | `SIZE_n_` is **hand-authored, not derived** — it usually equals `SkirtSizeX + SkirtSizeZ` but there are ~20 mismatches (XEB2306 is a 2×2 skirt tagged SIZE12; XAB2307 is 10×10 tagged SIZE4). **Read it from the blueprint; never compute it.** |
| `C-050` | `WP-19` | Adjacency magnitudes are per-neighbour `Add` values that scale inversely with receiver size, and the design invariant is that **`Add × n` is constant** for a 2×2 giver, where `n` is how many 2×2 structures fit around a receiver of that size bucket. Fully-surrounded totals: T1 power `−0.25` build energy and upkeep, `−0.10` weapon energy; T1/T2/T3 mass extractor `−0.40`/`−0.60`/`−0.80` build mass; T1 fabricator `−0.10`; **T1 energy and mass storage `+0.50`**. Givers with a 6×6 or 8×8 skirt use a *flat* `Add` across buckets because they can only occupy one side (max 4): T2 power `−0.5`, T3 power `−0.75`, T3 fabricator `−0.3`. **Adjacency reduces resource cost, never build time** — `Game.GetConstructEconomyModel` has no adjacency term; the modifier is applied afterwards in `UpdateConsumptionValues`. | [LUA-R] `ART-S007` `lua/sim/AdjacencyBuffs.lua` (line refs per family in the agent report); `lua/sim/Unit.lua:690,716-723,734,755-759`; `lua/game.lua:24-51` | High | Confirmed | The invariant is a ready-made test oracle: four mass storages around a T1 extractor give `MassProdAdjMod = 1.5`, i.e. 2.0 → 3.0 mass/s, which is the well-known retail result. |
| `C-051` | `WP-19` | Three retail defects to reproduce deliberately rather than silently fix. (a) **`T2PowerEnergyBuildBonusSize20` is `Add = -0.0125`** where every sibling is `-0.125` — a dropped digit, so T2 power generators give a tenth of the intended build-energy discount to SIZE20 receivers, while the maintenance buff at the same size is correct. (b) **`RateOfFire` adjacency is a penalty, not a bonus**, despite the name: `Buff.lua` computes `wep:ChangeRateOfFire(1/(val*delay))` so the new rate is `val × bpRateOfFire`, and `Add` is negative — the weapon fires *slower*. Only 5 units can ever receive it (`STRUCTURE + SIZE4 + ARTILLERY`). (c) **12 of the 95 buffs are dead code** — the `RateOfFire` Size8/12/16/20 sets are defined but referenced by no producer list. | [LUA-R] `ART-S007` `lua/sim/AdjacencyBuffs.lua:537`; `lua/sim/Buff.lua:410-423`; producer lists at `:14,:441,:845` | High | Confirmed | A parity implementation must decide explicitly whether to reproduce (a) and (b). Silently "fixing" either changes balance away from retail. |
| `C-052` | `WP-19` | Adjacency lifecycle: **no bonus flows while either party is under construction** — `OnAdjacentTo` returns early if `self:IsBeingBuilt()` or `adjacentUnit:IsBeingBuilt()`. Removal via `OnNotAdjacentTo` has **no such guard** and is unconditional. Both parties fire independently, so a generator beside an extractor produces two separate applications. A captured structure loses all adjacency buffs, because `TransferUnitsOwnership` destroys and recreates the entity and `OnCreate` re-initialises `self.Buffs` to empty. | [LUA-R] `ART-S007` `lua/defaultunits.lua:357-385`; `lua/sim/Unit.lua:178-181,555-620` | High | Confirmed | Two consequences are **engine-side and unproven from Lua**: nothing in the shipped scripts re-fires `OnAdjacentTo` on construction completion, and nothing releases a dead giver's buff — the engine must do both. Also unresolved: no army/ally check appears anywhere in Lua. |
| `C-044` | `WP-03`, `WP-02` | **Retail Lua handles are safe by construction, and the earlier reading of this was wrong in both directions.** The receiver is *not* a pointer cached in the thunk path — it is resolved **fresh on every call** from Lua argument slot 1: `0x009741c0` builds a LuaObject from argument *n* (`0x00977b20` computes `stackbase + 8n`), then `Moho::CScriptObject`'s fetch at `0x004ced90` does `rawget(self, "_c_object")` (string `0x00e5053c`), checks it is userdata, and returns its payload — a `{void *object, Class *type}` pair whose **first dword is the native pointer**. Every per-class resolver then tests that dword, in one of two variants: **54 resolvers raise** `lua_error` with `"Game object has been destroyed"` (`0x00e594b8`) — e.g. the `Unit` resolver `0x0059a190`, check at `0x0059a1c6` — and **19 return NULL** instead, used by lifecycle queries that must survive a dead receiver, e.g. the `Entity` resolver `0x006273a0`, check at `0x006273d6`. | [EXE] `ART-E001` disassembly at the addresses given; agent-verified 2026-08-29 | High | Confirmed | **Supersedes the "no validity check" reading in `C-036`.** A call on a dead unit raises a *caught* Lua error and unwinds the coroutine; it is never a wild dereference. |
| `C-045` | `WP-03` | The single invalidation site is `Moho::CScriptObject::~CScriptObject` at `0x004cde60`, which nulls the cached pointer with `movl $0x0,(%eax)` at **`0x004cdeab`** after resolving `_c_object`. That destructor has **91 direct callers** — every derived scriptable class — and the byte pattern occurs exactly once in `.text`. The same destructor separately unlinks the native `Moho::WeakPtr` list at `0x004cdef6`/`0x004cdefc` (node layout `{+0 target, +4 next}`; stubs `0x004ce720`/`0x004ce750`/`0x004ce770`). So there are **two independent invalidation mechanisms**: the Lua-visible pointer and the native weak-pointer graph. No refcount holds the object alive for Lua. | [EXE] `ART-E001` `0x004cde60`–`0x004cdf08` | High | Confirmed | Resolves hypothesis `H-002` in favour of "nulled on destruction, checked on use". Candidates "refcount keeps it alive" and "unsafe, script discipline" are both refuted. |
| `C-046` | `WP-03`, `WP-18` | "Destroyed" has **two distinct meanings** in retail, and they do not coincide. The global `IsDestroyed` (`0x004ce630`) is exactly `resolver() == 0 \|\| *(int*)resolver() == 0` — pointer nulled. But `Entity:BeenDestroyed` (`0x00698650`) is true if *either* the resolver returned NULL **or** the byte at **`Entity+0x1b9`** is non-zero. `Entity::Destroy` only *queues* destruction; the object stays allocated until `EntityDB::Purge`. So during the deferred window a script sees "destroyed" while the pointer is still live. | [EXE] `ART-E001` `0x004ce630`, `0x00698650` | High | Confirmed | A new object offset — `Entity+0x1b9`, the queued-for-destruction flag. Confirms the deferred-destruction model of `C-004` and shows scripts are told about death **earlier** than the pointer dies, which closes the window in which a script could observe a live-looking but doomed entity. |
| `C-047` | `WP-03`, `WP-07` | Shipped retail Lua **almost never guards a stored reference**: across all 369 `.lua` files, `IsDestroyed` appears **0 times** and `BeenDestroyed` **twice** (`lua/objectiveArrow.lua:46`, `lua/proptree.lua:220`) — yet scripts routinely hold unit references across ticks. | [LUA-R] `ART-S007`, full-corpus count with `LC_ALL=C` | High | Confirmed | Independent corroboration of `C-044`: scripts do not need to guard because the engine raises on their behalf. Had the engine been unsafe, a corpus this size would be littered with guards. A rare case where an *absence* in the scripts is strong evidence about the engine. |
| `C-041` | campaign method | BSim name transfer from `ART-D001` to `ART-E001` **works on large engine functions and fails on small ones**, and the difference is stark enough that a single accuracy figure would be misleading. Two controls: (a) on the executable's 870 already-named functions — overwhelmingly CRT and MFC stubs — peak agreement is **84% at significance ≥ 30** and it *degrades* above that, with confident errors such as `__alldvrm` → `__alldiv` and `_sscanf` → `Moho::PLAT_SetRegistryValueDword` both at similarity 1.000; (b) on the 626-function depth-1 engine frontier, 471 matched and **98 matched a *named* DLL function, of which 78% are corroborated at significance ≥ 40** by an independent signal — which Lua binding calls them — and manual inspection shows several of the remaining 22% are correct synonyms the metric cannot see (`CreateAnimator` → the `CAnimationManipulator` constructor, `PauseSound` → `AudioEngine::SetPaused`). | [EXE] `ART-E001` vs `ART-D001` via BSim `medium_32`; reproduce with `tools/re/BSimTransferNames.java` | High | Confirmed | Small functions have degenerate feature vectors and match everything, so **similarity 1.0 on a short function is worthless**. Use significance, not similarity, and only on the frontier. Yield is modest — 98 candidate names from 626 — but it includes the `WP-30` anchor. Never auto-apply: at ~80% these names must be marked `maybe_` per the naming convention. |
| `C-042` | `WP-30`, `WP-15`, `WP-17` | Addresses recovered by `C-041`, each corroborated by its calling Lua binding: **`Moho::SIM_MetaImpactArea 0x0073e950`** (sig 201, called by the `MetaImpact` binding) — the area-damage entry `C-021` predicted; `Moho::Sim::TransferUnit 0x0074dc40` (sig 489, called by `ChangeUnitArmy`); `Moho::Unit::SetConsumptionActive 0x006b1390` (sig 100, exact name agreement with its binding); `Moho::Entity::IsInCategory 0x00681ae0`; `Moho::Unit::ToggleScriptBit 0x006adf20`; `Moho::Unit::IsIdleState 0x006ae7f0`; `Moho::CollisionBeamEntity::CheckCollision 0x00679d60`; `Moho::UserEntity::IsInCategory 0x008bf570`. | [EXE] `C-041` plus call-graph corroboration | Medium–High | Confirmed | `SIM_MetaImpactArea` is the first sim-side damage function located, after two sessions of failing to reach it from the Lua side. The damage neighbourhood is `0x0073e000`–`0x00740000`. |
| `C-043` | campaign method | BSim **cannot distinguish identical sibling functions**. `Moho::Entity::SetVizToAllies` is returned as the best match for four consecutive addresses (`0x0067f3c0`, `0x0067f410`, `0x0067f460`, `0x0067f4b0`), all at similarity 1.000 and significance 41.68. `Moho::Entity` really has four such setters — `SetVizToAllies`, `SetVizToEnemies`, `SetVizToNeutrals`, `SetVizToFocusPlayer` (`ART-D001` exports) — with, evidently, identical code differing only in a constant. | [EXE] `ART-E001` BSim results; [EXE] `ART-D001` export list | High | Confirmed | Where a class has a family of near-identical accessors, BSim assigns them all the same name and the *order* must be recovered another way. Treat a repeated match name across consecutive addresses as an ordering problem, not as a set of duplicates. |
| `C-037` | `WP-02`, `WP-07` | The retail Lua binding convention, read from `Unit::GetHealth` at `0x006cb7a0`. The 20-byte thunk unwraps a **call context** from the LuaPlus state object (`+0x44`, the step `C-036` misread) and tail-calls the real function with it in `ecx`. The real function is `int __thiscall F(LuaCallContext*)`: it dereferences `[ecx]` to get the raw `lua_State*`, calls the argument-count helper at `0x00977ce0`, compares the result against each accepted arity, and on mismatch raises through `0x00977920` with the format string `"%s\n  expected %d args, but got %d"`. The receiver, where there is one, is fetched separately at `0x0059a190`. Return value is the Lua result count (`mov eax,1; ret` for a one-value getter). | [EXE] `ART-E001` `0x006cb7a0` full disassembly; string at `0x00e59e38` | High | Confirmed | Corrects `C-036`. Also means arity is checked *natively* on every Moho call, which is what makes `C-039` possible. |
| `C-038` | `WP-30`, `WP-03` | **Entity health is a 32-bit `float` at object offset `+0x98`.** `Unit::GetHealth` reads it with a single `flds 0x98(%esi)` after fetching the receiver, then pushes it as a Lua number. | [EXE] `ART-E001` `0x006cb839` | High | Confirmed | A direct, load-bearing divergence from Recoil Metal, which stores health as fixed-point `Mag` (Q50.14) *on purpose* — see `src/core/sim/Health.hpp`. Retail accumulates float rounding error across a blast's linear falloff; Recoil Metal does not. Neither is "wrong", but exact damage totals **cannot** match retail bit-for-bit, and any future replay-parity claim must say so. This is the first concrete number-level parity constraint the campaign has produced. |
| `C-039` | `WP-02` | The recovered signatures are **machine-verified against the engine's own arity checks**. Of the 608 rows having both a parseable parameter list and a detectable arity check, **549 agree (90.3%)** and 59 do not. Of the 59: 53 are doc strings that declare empty parentheses for a method that really takes arguments (`CAiBrain:GetEconomyStored()` accepts 2), and only **6** are substantive — `Damage`, `PlayLoop`, `StopLoop`, `_c_CreateShield`, `CMauiItemList::SetNewColors`, `Entity::SetCollisionShape`. | [EXE] `ART-E001`; reproduce with `tools/re/verify_arity.py` | High | Confirmed | Independently corroborates `C-032` at scale, and establishes the precedence rule: **where the shipped signature and the arity check disagree, the code is right and the documentation is stale.** |
| `C-040` | `WP-30` | The global `Damage` really takes **five** arguments, not the four its shipped signature declares: the declared `Damage(instigator, target, amount, damageType)` omits `location`. The native implementation at `0x0073f6d0` checks `cmp eax, 5`, and the only retail script that calls it passes five — `Damage(self, {0,0,0}, TargetEntity, self.Data, 'Normal')` in `ADFShieldDisruptor01_script.lua`. | [EXE] `ART-E001` `0x0073f707`; [LUA-R] `ART-S013` `projectiles/ADFShieldDisruptor01/ADFShieldDisruptor01_script.lua` | High | Confirmed | Three independent sources agree against the engine's own documentation. A worked example of why `C-039`'s precedence rule matters, and a caution for anyone treating the recovered signatures as authoritative. |
| `C-033` | `WP-02` | Cross-validation of `C-032` against two independent sources. Of the 125 recovered `Unit` method names, **81 appear verbatim as method calls in the shipped retail Lua** (`ART-S007`) and 40 match exported `Moho::Unit` members in `ART-D001`; for `Entity`, 53 of 65 and 27 of 65. Every one of the 44 `Unit` names *not* called by shipped Lua is a recognisable engine method — `ToggleScriptBit`, `AlterArmor`, `RemoveNukeSiloAmmo`, `SetFireState`, `RevertRegenRate`, `SetBreakOffDistanceMult` — several of which are independently confirmed by `ART-D001` exports. No recovered name is implausible. | [EXE] `ART-E001`; [LUA-R] `ART-S007` call-site scan; [EXE] `ART-D001` export table | High | Confirmed | The extraction has no observed false positives. The 44 uncalled names are API the shipped scripts simply do not use, which is expected — mods and FAF use many of them. |
| `C-034` | `WP-02`, `WP-31` | `moho.shield_methods` and `moho.sound_methods` are registered as metatables (`C-027`) but **no method is bound to them** through the mechanism of `C-032`: the strings `Shield` and `HSound` never appear as a descriptor scope anywhere in `ART-E001`. | [EXE] `ART-E001` static-initialiser scan, negative result | Medium | Open | Suggests Lua's `Shield` object gets its behavior from `Entity` plus the Lua-side `lua/shield.lua` class rather than from shield-specific native methods — consistent with `C-020`, where shields are ordinary entities. Worth confirming before relying on it for `WP-31`. |
| `C-030` | `WP-35`, `WP-06` | `Game.VeteranDefault` is the **fallback, not the norm**. 193 of the 568 shipped unit blueprints in `ART-S001` state their own top-level `Veteran = { Level1..Level5 }` table, and the overrides are far shorter than the default: UAA0102 (interceptor) promotes at 2/4/6/8/10 kills, UEL0001 (UEF commander) at 20/40/60/80/100, UAA0310 (strategic bomber) at 40/80/120/160/200, against the default's 25/100/250/500/1000. Every override observed is an evenly spaced arithmetic series. 374 units state neither `Veteran` nor `Buffs` and take the default. | [BP-R] `ART-S001` `units/<id>/<id>_unit.bp`, `Veteran` table, counted over all 568 unit blueprints | High | Confirmed | Implementing veterancy against the default alone would make it roughly an order of magnitude too rare across most of the combat roster. `UnitDef` therefore carries per-type thresholds. Also a caution for `WP-06`: `Veteran` is a **top-level** blueprint section, not part of `Defense`. |
| `C-031` | `WP-35`, `WP-06` | 192 of the 568 unit blueprints also state a top-level `Buffs = { Regen = { Level1..Level5 } }` table, which **replaces** the default `VeterancyRegen<L>` ladder rather than adding to it — both the engine buff and the blueprint-derived one carry `BuffType = 'VETERANCYREGEN'` with `Stacks = 'REPLACE'`, so the later application wins. Values differ from the default 2/4/6/8/10: UAA0102 states 1/2/3/4/5 and UEL0001 states 3/6/9/12/15. **`Regen` is the only buff sub-table any shipped unit overrides** — a census of every `Buffs` sub-table across all 568 blueprints returns `{'Regen': 192}` and nothing else, so the health multiplier is genuinely universal. | [BP-R] `ART-S001` `Buffs` sub-table census over all 568 unit blueprints; [LUA-R] `ART-S007` `lua/sim/Unit.lua` `SetVeteranLevel`/`BuffTypes`/`CreateVeterancyBuff` | High | Confirmed | The negative half is as useful as the positive: it licenses keeping the health multiplier a constant while making regeneration per-type, which is what the implementation does. |
| `C-028` | `WP-35`, `WP-34` | Retail buffs are **recomputed from the blueprint base value**, never applied incrementally. `Buff.ApplyBuff` calls `BuffCalculate(unit, buffName, affectType, initialVal)` with `initialVal` taken fresh from the blueprint (`Defense.MaxHealth`, `Defense.RegenRate`), and the formula is `result = (base + Σ(Add × Count)) × Π(Mult repeated Count times)` — **adds first, then multiplies**. Because the veterancy buffs declare `Stacks = 'REPLACE'`, only the current level's buff is ever present, so levels **do not compound**. | [LUA-R] `ART-S007` `lua/sim/Buff.lua:206-231` and `BuffCalculate` at `:452-492` | High | Confirmed | Decisive. Level 3 health is `base × 1.3`, **not** `base × 1.1 × 1.2 × 1.3`. Implementing veterancy as successive multiplications is the obvious wrong answer and gives 1.716× instead of 1.3× at level 3. |
| `C-029` | `WP-35` | Raising `MaxHealth` by buff also **heals** the unit. `Buff.lua` does `SetMaxHealth(val)` then, unless the buff sets `DoNoFill`, `AdjustHealth(unit, val - oldmax)` when the maximum grew, or clamps current health to the new maximum when it shrank. The veterancy health buffs do not set `DoNoFill`. Regeneration is set, not added: `SetRegenRate((bpRegenRate + adds) × mults)`, so at level `L` regen is `Defense.RegenRate + 2L` and max health is `Defense.MaxHealth × (1 + 0.1L)`. | [LUA-R] `ART-S007` `lua/sim/Buff.lua:206-231`; `lua/sim/BuffDefinitions.lua` | High | Confirmed | A promoted unit is healed by exactly the max-health increase, so veterancy is a mid-combat survivability spike, not just a larger bar. Both facts are directly testable. |
| `C-026` | `WP-02` | The Lua class registry is a `.data` array at `0x00fb9400`–`0x00fbb800` of 24-byte records laid out `{const char *metatable_name, const char *class_name, const char *doc, NULL, void *method_list, void *type_desc}`. Inheritance is a second record of the same shape whose name field is the literal `"base"` and whose doc field reads `"derived from <Parent>"`. All 60 `moho.*` strings are referenced from inside this range and from nowhere else in the image. | [EXE] `ART-E001` `.data 0x00fba008` (the `Unit` record) and 59 siblings | High | Confirmed | Gives `WP-02` its skeleton and the exact `PE-27` mechanism. The method **names** are not in this table. |
| `C-027` | `WP-02`, `WP-03` | The retail Lua object model exposes exactly 36 method tables. Mapping metatable → native class: `unit_methods`→`Unit`, `entity_methods`→`Entity`, `projectile_methods`→`Projectile`, `prop_methods`→`Prop`, `shield_methods`→`Shield`, `blip_methods`→`ReconBlip`, `weapon_methods`→`UnitWeapon`, `userDecal_methods`→`ScriptedDecal`, `aibrain_methods`→`CAiBrain`, `platoon_methods`→`CPlatoon`, `navigator_methods`→`CAiNavigatorImpl`, `aipersonality_methods`→`CAiPersonality`, `CAiAttackerImpl_methods`→`CAiAttackerImpl`, `manipulator_methods`→`IAniManipulator`, `sound_methods`→`HSound`, `lobby_methods`→`CLobby`, `discovery_service_methods`→`CDiscoveryService`, `steam_discovery_service_methods`→`CSteamDiscoveryService`, `PathDebugger_methods`→`CPathDebugger`, `ui_map_preview_methods`→`CUIMapPreview`, `WldUIProvider_methods`→`CLuaWldUIProvider`, `world_mesh_methods`→`CUIWorldMesh`, and 14 `CMaui*` UI classes (`control`, `bitmap`, `border`, `cursor`, `dragger`, `edit`, `frame`, `group`, `histogram`, `item_list`, `mesh`, `movie`, `scrollbar`, `text`). Declared inheritance counts: 13 classes `derived from Entity`, 33 `derived from CMauiControl`, 3 `derived from IAniManipulator`, 1 `derived from CScriptEvent`. | [EXE] `ART-E001` `.data` class registry, field `+4` and the `"base"` records | High | Confirmed | The authoritative list of what retail Lua can touch. Note `Shield` and `ReconBlip` are Lua-visible entity classes in their own right, reinforcing `C-020`. |
| `C-025` | `WP-00` | The preserved corpus is Forged Alliance, not vanilla Supreme Commander: `ART-S007` contains Seraphim faction data (`lua/factions.lua`, `lua/ui/lobby/restrictedUnitsData.lua`, and others). The exe version resource reads product `Supreme Commander Forged Alliance`, internal name `SupCom`, original filename `SupremeCommander.exe`. | [LUA-R] `ART-S007`; [EXE] `ART-E001` version resource | High | Confirmed | Removes the ambiguity created by the executable being named `SupremeCommander.exe` and by `MohoEngine.dll` (a vanilla-era artifact) being present in `bin`. |

## Hypothesis ledger

Use this for decisions not yet settled by the executable.

| Hypothesis ID | Envelope | Candidate | Supporting evidence | Counterevidence | Discriminating observation | State |
|---|---|---|---|---|---|---|
| `H-001` | `PE-18` | `0x0073ef40` is the shared damage gate that `C-021` calls `SIM_Damage`. | All four scripted damage entry points call it; only 6 call sites image-wide; adjacent to the four callers. | **Refuted by reading it.** `0x0073ef40` unpacks Lua values (`0x009729f0` repeatedly), reads a field at `+0x8d8` of a context object, and calls `0x004cdb30` — which is itself Lua machinery (argument counting at `0x00977ce0`, stack access at `0x00972a40`/`0x00975090`, 16 callers spread across unrelated subsystems). No armour lookup, no write to `Entity+0x98`. | Done: disassembled. It only touches the Lua stack, which was the stated discriminator. | **Refuted** |
| `H-002` | `PE-03` | The Lua object's cached native pointer is nulled on destruction, rather than the handle being validated on use. | Resolved: `C-044`–`C-047`. **Both halves turned out to be true** — the pointer is nulled at `0x004cdeab`, *and* it is validated on every use by 73 per-class resolvers. The question posed them as alternatives; they are a pair. | The competing candidates are refuted: no refcount holds the object alive for Lua, and scripts do not guard (0 uses of `IsDestroyed` in 369 files). | Done. | **Confirmed** |

## Work-package record template

Create one subsection only when a package becomes active. This avoids 45 empty prose sections while
keeping every active investigation resumable.

```markdown
### WP-NN: Name

**Readiness:** Not ready
**Confidence before EXE:** Low
**EXE evidence:** Anchored
**Possibility envelopes:** PE-NN
**Artifacts:** ART-E001
**Current analyst:**
**Last touched:** YYYY-MM-DD

#### Exact question

One behavior question narrow enough to answer from a call path.

#### Known bounds

- [LUA-R] ...
- [BP-R] ...
- [DOC-R] ...
- [RM] ...

#### Candidate mechanisms

1. Candidate A.
2. Candidate B.

#### Search anchors

- String / registration / RTTI / field / constant.

#### Native path

`RVA -> function -> function -> decisive branch`

#### Findings

- `F-NNN` finding with evidence and confidence.

#### Counterevidence and unresolved questions

- What still could make the current interpretation wrong.

#### Completion criteria

- Decisive branch and state owner identified.
- At least one caller and one downstream effect traced.
- Candidate mechanisms reduced to one, or ambiguity documented as irreducible.
- Claim ledger and dashboard updated.
- Exact next action written below.

#### Next exact action

One address, function, xref, or experiment. Never "continue analysis."
```

## Active work-package records

### WP-01: PE architecture, sections, imports, RTTI, symbols, global map

**Readiness:** Not ready
**Confidence before EXE:** High
**EXE evidence:** Analyzed
**Possibility envelopes:** `PE-02`
**Artifacts:** `ART-E001`, `ART-D001`
**Last touched:** 2026-08-29

#### Exact question

Is the retail engine reachable in one binary, and does that binary carry enough type information to
name functions without a PDB?

#### Findings

- `F-001` Yes to both. `C-001` (single statically linked binary) and `C-003` (full RTTI) mean the
  campaign never needs the shipped DLLs at runtime, and Ghidra's RTTI analyzer can name vtables
  directly. `C-002` additionally supplies 4,875 mangled names from the same source tree.
- `F-002` The executable is PE32 x86, GUI subsystem, linker-version field 8.0, relocations
  stripped, large-address aware, imports `d3dx9_35.dll` which is **not** shipped in `bin` (only
  `d3dx9_31.dll` is), so the retail install depends on a system DirectX redistributable — consistent
  with `ART-M002` referencing the August 2009 DirectX package.

#### Counterevidence and unresolved questions

- Relocations are stripped, so the image base is fixed; addresses recorded from Ghidra are directly
  comparable across sessions, but only for this exact hash.
- No function RVA has been recorded in the symbol ledger yet. Until it is, `WP-01` stays Not ready.
- The global/static data map is entirely unsurveyed.

#### Next exact action

Run a headless script that lists Ghidra's RTTI-derived vtable symbols with addresses, and seed the
symbol ledger with `Moho::Sim`, `Moho::Entity`, `Moho::Unit` and `Moho::EntityDB` vtable RVAs.

---

### WP-02: Moho/Lua native registration map

**Readiness:** Ready but not confirmed
**Confidence before EXE:** High
**EXE evidence:** Analyzed
**Possibility envelopes:** `PE-27`
**Artifacts:** `ART-E001`, cross-checked against `ART-D001` and `ART-S007`
**Last touched:** 2026-08-29 (session 04)

#### Exact question

Which native function implements each Lua-visible Moho method, and what is the complete retail
registration set?

#### Known bounds

- [EXE] `ART-E001` contains exactly 60 registration-table name strings matching `moho.*`. The
  method-table ones are: `aibrain_methods`, `aipersonality_methods`, `bitmap_methods`,
  `blip_methods`, `border_methods`, `CAiAttackerImpl_methods`, `control_methods`, `cursor_methods`,
  `discovery_service_methods`, `dragger_methods`, `edit_methods`, `entity_methods`, `frame_methods`,
  `group_methods`, `histogram_methods`, `item_list_methods`, `lobby_methods`,
  `manipulator_methods`, `mesh_methods`, `movie_methods`, `navigator_methods`,
  `PathDebugger_methods`, `platoon_methods`, `projectile_methods`, `prop_methods`,
  `ScriptTask_Methods`, `scrollbar_methods`, `shield_methods`, `sound_methods`,
  `steam_discovery_service_methods`, `text_methods`, `ui_map_preview_methods`, `unit_methods`,
  `userDecal_methods`, `weapon_methods`, `WldUIProvider_methods`, `world_mesh_methods`.
  The remainder are constructor/type names: `AimManipulator`, `AnimationManipulator`,
  `BoneEntityManipulator`, `BuilderArmManipulator`, `CDamage`, `CDecalHandle`,
  `CollisionBeamEntity`, `CollisionManipulator`, `CPrefetchSet`, `EconomyEvent`, `EntityCategory`,
  `FootPlantManipulator`, `IEffect`, `MotorFallDown`, `RotateManipulator`, `SlaveManipulator`,
  `SlideManipulator`, `StorageManipulator`, `ThrustManipulator`, `UIWorldView`.
- [EXE] `C-011`: registration is generated per class via `CScrLuaMetatableFactory<T>`.

#### Candidate mechanisms

Settled by `C-011`. What remains is purely mechanical recovery, not a choice between mechanisms.

#### Native path

`.data 0x00fb9400..0x00fbb800` class registry → record `+16` → `.data` list of
`{descriptor*, NULL}` → `.rdata` per-method descriptor whose word 0 is the native function.

#### Findings

- `F-006` `C-026`: the class-registry record layout, recovered by dumping around every
  `moho.*_methods` string reference rather than by pattern-scanning.
- `F-007` `C-027`: the complete 36-entry metatable → native class map plus declared inheritance.
- `F-008` **Negative result, recorded so it is not retried.** A generic scan for `luaL_reg`-shaped
  `{const char *name, lua_CFunction fn}` arrays finds LuaPlus's own standard libraries
  (`base`, `math`, `table`, `debug` — for example the `math` table at `0x00d96f48` and the `table`
  library at `0x00d980a0`) but finds **no Moho method table**. Moho does not use `luaL_register`.
  The per-method `.rdata` descriptors instead begin with the function pointer and continue with a
  LuaPlus type/signature structure (a `{0,0,0, ptr, ptr, 0, 0, argcount, ...}` shape), so the
  method *name* is not adjacent to the pointer. That scan is a dead end; do not repeat it.

- `F-009` **Solved, and the hypothesis in `F-008` was right.** The names are immediate operands in
  code. Each Lua callable has a compiler-generated static initialiser that fills a descriptor with
  `mov dword ptr [absolute], immediate` stores; scanning `.text` for runs of them recovers the
  whole API without decompiling anything. `C-032` states the layout and the result; `C-033` the
  cross-validation. The extractor is `tools/re/extract_moho_methods.py`, and the table lands at
  `build/re-fa/exports/moho.methods.tsv` (gitignored, regenerable in about a second).
- `F-010` The descriptor carries a **documentation string** as well as a signature, which the
  engine splits on `" - "`. So the recovered table includes the engine authors' own one-line
  description of many methods — for example `Unit::GetConsumptionPerSecondEnergy`, "Get the
  consumption of energy of the unit". That is a better specification of intended behavior than
  anything reconstructed from call sites.

- `F-011` The wrapper check passed. `0x006ca530` and `0x006cb720` are byte-identical 20-byte
  thunks that take one argument, resolve it to a receiver through `0x00975c60`, and tail-call
  the real method with the receiver in `ecx` — exactly what a signature of `GetUnitId(self)`
  claims. Following that tail call resolves 797 of the 1,182 callables to distinct real method
  addresses. See `C-035`.

#### Counterevidence and unresolved questions

- 385 callables still name only their generated wrapper, not the real method: their thunks
  marshal more than a receiver and so have other shapes. Each shape is a small, separate
  decoding job.
- `C-034`: `Shield` and `HSound` have metatables but no methods bound by this mechanism. Either
  they genuinely have none, or those two use a second registration path this scan does not see.
  Distinguishing the two matters for `WP-31`.
- 406 of the 1,182 callables have scope `<global>`; which Lua table they end up in (`moho.`,
  `_G`, a UI namespace) has not been determined.

#### Next exact action

Decompile `fa_lua_resolve_receiver` at `0x00975c60`. It is the single point where a Lua handle
becomes a native object, so it decides `WP-03`'s open question — what happens on a stale
reference, and whether the pooled id is checked or the pointer trusted. One registration
function should yield the whole `unit_methods` table; the other 35 follow the same shape.

---

### WP-35: Veterancy, kill credit, promotion, regeneration

**Readiness:** Not ready
**Confidence before EXE:** High
**EXE evidence:** Unexamined
**Possibility envelopes:** `PE-22`
**Artifacts:** `ART-S007`
**Last touched:** 2026-08-29

#### Exact question

What exactly changes when a retail Forged Alliance unit gains a veterancy level, and what triggers
it?

#### Findings

- `F-003` See `C-024` for the complete specification. This package is **answered without EXE work**;
  the whole mechanism is in shipped Lua and blueprint data.
- `F-004` A subtle detail worth preserving: `CheckVeteranLevel` reads `GetStat('KILLS',0).Value + 1`
  because it runs *before* the kill stat is written, and it promotes at most one level per call.
  `AddKills(n)` is the separate multi-level path and loops. Implementing only one of the two paths
  produces off-by-one veterancy.
- `F-005` `Unit:OnKilledUnit(victim)` is called on the *instigator* from the victim's death handling,
  immediately before the death weapon fires and the death thread forks. Kill credit therefore lands
  before wreck creation.

#### Counterevidence and unresolved questions

- Whether the `KILLS` stat is stored natively (`Unit::GetStat`/`SetStat` are native) or in Lua
  affects save/replay but not gameplay values.
- Whether an instigator that dies in the same beat still receives credit depends on native death
  ordering, which is untraced.

#### Implementation, session 03

Done. `src/core/sim/Veterancy.{hpp,cpp}` holds the rules; `Health` carries the per-unit
`Veterancy` state; kill credit is awarded from `retireDead` (retail's own placement, before the
death weapon and the wreck); hull regeneration runs as `tickRegeneration` next to `tickShields`;
`Defense.RegenRate` is now parsed and converted per tick in `UnitCatalog`. Covered by
`tests/test_veterancy.cpp` — all 1,129 tests and all seven sim-discipline guards pass.

Three implementation notes worth keeping:

- Max health is scaled as an **exact integer ratio** on the raw fixed-point value rather than
  by an `Fx` multiplier. `Fx::fromRatio(11, 10)` is 18022/16384, which turns 1,000 health into
  1,099.976 rather than 1,100 — invisible in play, but it feeds the state hash.
- The veteran regen bonus is the one rate in the sim that **cannot** be converted once at
  content load, because it depends on the unit's level rather than its type. It is derived in
  whole numbers inside the tick (`veteranRegenPerTick`) precisely so that deriving it there
  does not mean a float in the tick.
- `creditKill` refuses a killer at zero health that has not yet been retired. Without that
  guard, promoting it would heal it by the max-health increase and resurrect a unit the same
  loop was about to bury — an outcome that would depend on nothing but slot order.

- Per-type thresholds are read from the blueprint's top-level `Veteran` table (`C-030`), which
  193 of 568 units state. This was found by checking rather than assumed: implementing only
  `Game.VeteranDefault` would have been wrong for a third of the roster, and wrong by roughly
  a factor of ten on the units that fight most.

- Per-type regeneration ladders come from `Buffs.Regen` (`C-031`), replacing the default.
  `tests/test_veterancy.cpp` checks all of this against the **real shipped blueprints**
  (UAA0102, UEL0001, DAA0206) as well as against hand-built fixtures, because the hand-built
  ones cannot catch the fact that `Veteran` and `Buffs` are top-level sections.

#### Counterevidence and unresolved questions, still open

- Whether the `KILLS` stat is native or Lua-side, and whether an instigator that dies in the
  same beat still receives credit, both remain untraced.
- Veterancy is not surfaced in the UI, so a promotion is currently invisible except through
  the event and the health change.
- `Unit.lua` also calls `AIBrain:OnBrainUnitVeterancyLevel`. Recoil Metal's AI is not notified.

#### Next exact action

Surface veteran level in the HUD (chevrons on the selected unit's panel), which is the only
part of `C-024` a player can currently not see.

## Session protocol

### Starting a session

1. Read `What is ready`, `Artifact manifest`, and the latest session log.
2. Confirm the artifact hash matches the Ghidra project.
3. Open the active work-package record and its possibility envelope.
4. Read its `Next exact action`; do not start by browsing unrelated functions.
5. Check whether another session changed the canonical names or claims you depend on.

### During a session

1. Work on one exact question at a time.
2. Rename a function only after recording its original name/address in the symbol ledger.
3. Record candidate mechanisms before deciding among them.
4. Capture decisive pseudocode as a short behavioral summary, not copied decompiler output.
5. Follow at least one caller and one callee before declaring a function understood.
6. Record counterevidence and negative searches; future sessions must know what was already tried.
7. Keep generated exports and scripts outside git unless they are original reusable tooling.

### Ending a session

1. Update the active work package's evidence and exact next action.
2. Update symbol, claim, and hypothesis ledgers.
3. Update the dashboard row if confidence or EXE evidence changed.
4. Append one session record below.
5. Save durable findings to the project KB handoff as well as this document.
6. Never leave the only useful result inside a Ghidra comment or chat transcript.

## Session log

Append sessions newest-last. Keep each record concise enough to scan but complete enough to resume.

```markdown
### YYYY-MM-DD / Session NN

**Artifact:** ART-E001 SHA-256 prefix
**Tool/project:** Ghidra version, project path
**Work package:** WP-NN
**Question:** exact question

**Accomplished**
- Functions/addresses traced.
- Claims confirmed/refuted.

**Possibility envelope changes**
- Candidate eliminated or favored, with reason.

**Files and durable outputs**
- Paths to scripts, exports, screenshots, or notes.

**Blocked by**
- Concrete missing evidence, if any.

**Next exact action**
- Open function at RVA X, inspect xrefs from string Y, or trace return value into Z.
```

### 2026-08-29 / Session 01

**Artifact:** `ART-E001` SHA-256 `c6783580c0b7a408...`
**Tool/project:** Apple LLVM `objdump` 17.0.0 and ExifTool 13.10; no Ghidra project yet
**Work package:** `WP-00`
**Question:** Which exact retail executable, DLLs, configuration files, and SCD archives are on the
owned connected disk, and are they sufficient to begin static analysis?

**Accomplished**
- Identified the Steam app `9420` executable as PE32 x86 version `1.5.0.1`, reporting a PE
  linker-version field of 8.0.
- Hashed and recorded the main executable, BugSplat helper, all 18 DLLs, all 17 SCD archives, and
  four identity/configuration files.
- Preserved the complete `bin` and 2.93 GiB `gamedata` directories under the append-only input tree.
  Each source and preserved file was independently hashed with `/usr/bin/shasum -a 256`; comparing
  the normalized hash streams with `diff` returned no differences.
- Confirmed the executable imports and PE metadata statically; no retail binary was executed.

**Possibility envelope changes**
- None. This session established artifact identity only.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`
- `~/projects/llm/input/recoil-metal/retail-fa/bin/`
- `~/projects/llm/input/recoil-metal/retail-fa/gamedata/`

**Blocked by**
- Full `WP-00` provenance still lacks the original Steam depot/build manifest and installation
  transfer history. This does not block static analysis of the hash-identified artifact.

**Next exact action**
- Install and pin Ghidra, create `build/re-fa/project`, import preserved `ART-E001`, and record its
  section map, imports, compiler indicators, RTTI evidence, debug-directory contents, and image base
  for `WP-01`.

### 2026-08-29 / Session 02

**Artifact:** `ART-E001` SHA-256 `c6783580c0b7a408...`, `ART-D001` SHA-256 `3e6e1a698a57d051...`,
`ART-S007` SHA-256 `3632a3294fc01a07...`
**Tool/project:** Ghidra 12.1.2 headless with OpenJDK 21.0.11, project `build/re-fa/project` name `fa`
**Work package:** `WP-01`, opportunistically `WP-02` and `WP-35`
**Question:** Is the retail engine reachable in one binary, and does it carry enough type information
to proceed without a PDB?

**Accomplished**
- Established that Ghidra was already installed (12.1.2) but unusable because no JVM was on `PATH`;
  wired `JAVA_HOME` to the keg-only OpenJDK 21 in `build/re-fa/run-import.sh`.
- Imported and auto-analyzed `ART-E001` (224 s), `ART-D001`, `ART-D003`, `ART-D004`, `ART-D002` into
  project `fa`. No binary was executed at any point.
- `C-001`: proved from the PE import directory that the executable statically links the engine and
  loads none of the four shipped engine/runtime DLLs. This reorients the whole campaign onto one
  binary.
- `C-002`: matched the debug-directory PDB paths of `ART-E001` and `ART-D001` to the same Perforce
  tree, legitimising the DLL export table as a naming dictionary.
- Recovered 2,728 `Moho::` RTTI type descriptors from `ART-E001` (471 concrete non-template classes)
  and 4,875 mangled exports from `ART-D001`, parsed into 352 classes / 4,083 members.
- Recorded claims `C-001` through `C-025`, updating 38 of 45 dashboard rows from `Unexamined`.
- `C-024`: fully specified retail veterancy from shipped Lua and data, with every constant.
- `C-025`: confirmed the corpus is Forged Alliance, not vanilla Supreme Commander.
- Fixed a structural defect in the dashboard: rows `WP-06` through `WP-44` were missing the
  `Priority` column, silently shifting every later cell one column left.

**Possibility envelope changes**
- `PE-01` hybrid stages plus object callbacks — **favored**, alternatives refuted (`C-005`).
- `PE-02` C++ hierarchy with vtables — **confirmed**, opaque-handle candidate refuted (`C-003`).
- `PE-03` pooled integer ids plus deferred destruction — **confirmed**; generation/versioned handles
  refuted, which *weakens* stale-reference safety versus what was assumed (`C-004`).
- `PE-04` shared path request service with precomputed tables — **favored** over per-unit A* (`C-012`).
- `PE-05` formation assigned at command issue — **confirmed**, via a persistent instance (`C-015`).
- `PE-06` an explicit occupancy/reservation grid exists — steering-only refuted (`C-012`).
- `PE-08` one parameterised mover — per-family mover classes **refuted** (`C-013`).
- `PE-09` discrete layers — continuous depth **refuted** (`C-013`).
- `PE-10` generic attachment graph plus transport controller — **confirmed** (`C-016`).
- `PE-11` two-phase request/satisfy — sequential single drain **refuted** (`C-014`).
- `PE-12` progress on target plus builder-owned task — **confirmed** (`C-017`).
- `PE-13` separate native task state machines — one generalised work engine **refuted** (`C-008`).
- `PE-16` one data-driven projectile configured by setters — per-family native subclassing
  **refuted**: `ART-E001` RTTI contains a single `Moho::Projectile` plus a separate
  `Moho::CollisionBeamEntity`, and no per-family projectile subclass. See the `WP-28` dashboard row.
- `PE-17` ammo as a counter — ammo as child entities **refuted** (`C-018`).
- `PE-18` shared damage gate — **confirmed** by `SIM_Damage`/`SIM_MetaImpactArea` (`C-021`).
- `PE-19` shields as entities — pure containment test **refuted** (`C-020`).
- `PE-20` incremental refcounted grids with deferred removal — rebuild and per-query **refuted**
  (`C-007`).
- `PE-22` Lua buff application — native kill counter with fixed table **refuted** (`C-024`).
- `PE-25` native rule strategy object — **refuted** for victory conditions specifically (`C-009`).
- `PE-27` generated registration tables — method-id dispatcher **refuted** (`C-011`).

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md` (this file)
- `tools/re/parse_msvc_exports.py` (committed, reusable)
- `build/re-fa/run-import.sh`, `build/re-fa/project/` (gitignored)
- `build/re-fa/exports/{MohoEngine,gpgcore,gpggal,LuaPlus_1081}.exports.txt`,
  `moho.classes.tsv`, `exe.rtti.raw.txt`, `exe.moho.classes.txt` (gitignored, regenerable)
- `build/re-fa/lua/` extracted `ART-S007` (gitignored, regenerable)

**Blocked by**
- Nothing. `WP-00` still lacks the Steam depot manifest, which does not block analysis.

**Next exact action**
- Write and run a Ghidra headless script that, for every string beginning `moho.` in `ART-E001`,
  lists xrefs and dumps the adjacent `(name, function-pointer)` data array; write
  `build/re-fa/exports/moho.registration.tsv` and seed the symbol ledger from `moho.unit_methods`.

### 2026-08-29 / Session 03

**Artifact:** `ART-E001`, `ART-S001` SHA-256 `c23a48d6b4704314...`, `ART-S007`
**Tool/project:** Ghidra 12.1.2 headless, project `fa`; Python `zipfile` for the SCDs
**Work package:** `WP-02`, then `WP-35`
**Question:** Which native function implements each Lua Moho method — and, once that stalled,
what exactly does retail veterancy do and can Recoil Metal do it?

**Accomplished**
- `WP-02`: wrote `tools/re/DumpMohoRegistrations.java`, `DumpMohoRegLayout.java` and
  `DumpAddrs.java` (all read-only against the Ghidra database). Recovered `C-026`, the layout
  of the class-registration record, and `C-027`, the complete 36-entry metatable → native class
  map with declared inheritance.
- `WP-02` negative result `F-008`: Moho does **not** use `luaL_register`. A generic scan for
  `{name, fn}` arrays finds only LuaPlus's own standard libraries. Recorded so it is not
  retried; the method names are most likely immediates inside the registration code.
- `WP-35`: `C-024`, `C-028`, `C-029` from the shipped Lua, then `C-030` and `C-031` from a
  census over all 568 shipped unit blueprints.
- Implemented veterancy and hull regeneration. All 1,133 tests and all seven sim-discipline
  guards pass; the full app builds.

**What went wrong, and what it cost**
- A `grep` under a UTF-8 locale reported that retail has no Lua veterancy. It has a complete
  implementation; the shipped files are ISO-8859-1 and `grep` was silently treating them as
  binary. Recorded as evidence trap 1 at the top of this document.
- The `MohoEngine.dll` export table was briefly taken as the engine's full surface, which made
  repair, capture and reclaim look Lua-side. The executable's RTTI shows all three as native
  classes. Evidence trap 2.
- Veterancy was first implemented against `Game.VeteranDefault` alone. A check of the corpus
  showed 193 of 568 units override the thresholds and 192 override the regeneration ladder,
  most of them by large factors — so the first implementation would have been wrong for a
  third of the roster. Both are now per-type and tested against the real blueprints.
- The assumption that the UEF commander states no `Veteran` table was wrong; it states
  20/40/60/80/100. The test now pins the real values.

**Possibility envelope changes**
- `PE-27`: `C-026`/`C-027` confirm generated per-class registration. The remaining unknown is
  where the method *names* live, which is a decompilation question rather than a design one.
- `PE-22`: closed. Retail veterancy is Lua-owned, blueprint-parameterised, and does not touch
  weapon damage.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`
- `tools/re/{parse_msvc_exports.py,DumpMohoRegistrations.java,DumpMohoRegLayout.java,DumpAddrs.java}`
- `src/core/sim/Veterancy.{hpp,cpp}`, `tests/test_veterancy.cpp`, plus edits to `Health.hpp`,
  `UnitDef.hpp`, `UnitBlueprint.cpp`, `UnitCatalog.{hpp,cpp}`, `Skirmish.cpp`, `StateHash.cpp`,
  `Events.{hpp,cpp}`
- `build/re-fa/exports/{moho.registration.raw.tsv,moho.reglayout.txt}` (gitignored)

**Blocked by**
- Nothing. `WP-02`'s remaining step needs decompilation time, not new evidence.

**Next exact action**
- Find the function that writes `0x00fee88c` with a write-xref search, decompile it, and read
  the `unit_methods` names from its immediate operands.

### 2026-08-29 / Session 04

**Artifact:** `ART-E001` SHA-256 `c6783580c0b7a408...`, cross-checked against `ART-D001`, `ART-S007`
**Tool/project:** Ghidra 12.1.2 headless project `fa`; `tools/re/extract_moho_methods.py`
**Work package:** `WP-02`, spilling into `WP-03`
**Question:** Which native function implements each Lua-visible Moho method?

**Accomplished**
- **`WP-02` solved.** `C-032`: the method names are not in data at all — each Lua callable has a
  compiler-generated static initialiser that fills a descriptor with `mov [abs], imm` stores.
  Scanning `.text` for runs of those recovers **1,182 callables across 54 method-list globals**,
  each with its Lua name, authored signature *with parameter names*, the engine's own
  documentation string, and its native address. No decompiler needed; the scan runs in a second.
- `C-033`: cross-validated. 81 of 125 recovered `Unit` names are called verbatim in shipped
  retail Lua; every one of the 44 that are not is a recognisable engine method, several
  independently confirmed by `ART-D001` exports. No false positives observed.
- `C-035`: verified the mapping by disassembling two wrappers. Both are byte-identical 20-byte
  thunks that resolve `self` and tail-call the real method as `__thiscall` — exactly what a
  `(self)` signature claims. Following the tail call resolved **797 callables to distinct real
  native methods**, with zero address collisions.
- `C-036`, a `WP-03` bonus: the shared receiver resolver is four instructions —
  `mov eax,[esp+4]; mov eax,[eax+0x44]; ret`. Lua holds a **raw pointer** at userdata `+0x44`
  and the engine checks nothing on the way in.
- Dashboard readiness recounted from the table: 24 Not ready, 21 Ready but not confirmed.
  `WP-02` and `WP-35` both moved this session.

**Possibility envelope changes**
- `PE-27` closed. Generated per-class registration confirmed, and the whole surface enumerated.
- `PE-03` refined rather than settled: the sim identifies entities by pooled id, but the script
  boundary is a raw cached pointer. Both candidates were true, at different layers — a reminder
  that "which mechanism" questions need to name the layer they are about.

**What this changes for Recoil Metal**
- Recoil Metal's generational `UnitId` is **stricter than retail**, which resolves Lua receivers
  with no validity check whatsoever. That is a deliberate divergence and should stay.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`
- `tools/re/{pe_reader.py,extract_moho_methods.py,DumpRefsAndCode.java}` (committed, reusable)
- `build/re-fa/exports/moho.methods.tsv`, 1,182 rows (gitignored, regenerable)

**Blocked by**
- Nothing.

**Next exact action**
- Find what writes userdata `+0x44` on entity destruction — search for stores of `0` to
  `[reg+0x44]` inside `Entity::Destroy`/`OnDestroy`, or establish that the userdata holds a
  `Moho::WeakPtrBase` and the field is a weak-pointer slot. That settles `WP-03`'s last question.

### 2026-08-29 / Session 05

**Artifact:** `ART-E001`, cross-checked against `ART-S013`
**Tool/project:** Ghidra 12.1.2 project `fa`; `objdump`; `tools/re/verify_arity.py`
**Work package:** `WP-02` verification, `WP-30` anchoring
**Question:** Push the executable analysis from a map into read code.

**Accomplished**
- `C-037`: read `Unit::GetHealth` end to end and recovered the **binding calling convention** —
  `int __thiscall F(LuaCallContext*)`, native arity check via `0x00977ce0`, error through
  `0x00977920`, receiver fetched separately at `0x0059a190`, return value is the Lua result count.
- `C-038`: **health is a `float` at `Entity+0x98`** — the campaign's first number-level parity
  constraint against Recoil Metal's fixed-point `Mag`.
- `C-039`: machine-verified 549 of 608 checkable signatures against the engine's own arity
  checks (90.3%). Only 6 substantive disagreements; 53 more are empty-paren doc stubs.
- `C-040`: the global `Damage` takes **five** arguments, not the four it documents. Confirmed
  three ways: the arity check, the doc string, and the one retail script that calls it.
- `WP-30` anchored with addresses for all four scripted damage entry points.

**Corrections**
- **`C-036` superseded.** I read `0x0098f640` as "resolve the Lua receiver" from two examples,
  both methods. It is not: **112 `<global>` functions with no receiver run the identical thunk**.
  It unwraps a per-call context. The lesson is the one already at the top of this document in
  another form — a mechanism inferred from agreeing examples is not confirmed until something
  that *should* differ is checked.
- The first arity run reported 74 disagreements. 21 of them were **the parser's fault**: doc
  strings name the receiver after the class (`IsUnitState(unit, stateName)`), not `self`, so a
  receiver was double-counted. Fixed by admitting both readings before calling anything a
  mismatch. Reporting those 21 as engine bugs would have been a fabricated finding.

**Possibility envelope changes**
- `PE-18` unchanged but now addressable: the entry points are located, the gate is not yet read.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`; `tools/re/{verify_arity.py,ReportCoverage.java}`

**Also accomplished (after the session's first write-up)**
- Intersected the call targets of all four damage entry points and subtracted an unrelated
  binding's calls to strip the Lua plumbing. One candidate survives with only 6 callers in the
  whole image: `0x0073ef40`. Recorded as **`H-001`**, not as a claim — it has not been read.
- Annotated all 1,182 callables with a subsystem, a canonical `fa_*` name and a purpose line
  (`tools/re/annotate_moho_methods.py`). 305 purposes are the engine's own text; the rest are
  derived and marked as such in a `purpose_source` column so the two are never confused.
- The subsystem census produced two corroborating zeros: **no native veterancy API** (supports
  `C-024`) and only three shield-related callables (supports `C-034`).

**Negative result worth keeping**
- `H-001` **refuted**, and the method that produced it was flawed. Intersecting the four damage
  bindings' call targets and subtracting one unrelated binding's calls does *not* isolate
  domain logic: the binding used for subtraction, `Unit::GetHealth`, is trivial and calls no
  script machinery, so everything script-related survived the subtraction and looked
  damage-specific. `0x0073ef40` and `0x004cdb30` are both Lua plumbing. **Subtract a rich
  binding, not a simple one** — or better, do not subtract at all and work from the other end.

**Next exact action**
- Work backwards from armour instead of forwards from Lua. `Unit::GetArmorMult` is at
  `0x006cac80` and `Unit::AlterArmor` at `0x006caa80`; both must read the same per-unit armour
  table that the damage gate consults. Find the field offset they use, then find the other
  functions that read that offset. The damage gate is among them, and unlike a call-graph
  intersection this cannot be fooled by shared script plumbing.

### 2026-08-29 / Session 06

**Artifact:** `ART-E001`
**Tool/project:** Ghidra 12.1.2 project `fa`; `tools/re/DumpCallGraph.java`
**Work package:** campaign method
**Question:** How do we analyze this efficiently, given that we cannot analyze all of it?

**Accomplished**
- Exported the full call graph (31,086 functions) and measured reachability from the recovered
  Lua API. **84% of the binary is unreachable from any script-visible behavior**; the
  simulation-relevant closure is 2,562 functions (8.2%), and its **depth-1 frontier is 618**.
- Per-subsystem frontiers are small enough to plan against: economy 51, build 66, orders 70,
  intel 73, movement 75, combat 199.
- Wrote the `Analysis strategy` section: the Lua boundary as the natural cut, four cost tiers,
  the one-line-answer rule, `ART-D001` as the unused name oracle, triage, stop rules, and the
  two failure modes already hit.
- Revised the analysis order: bulk name transfer first, then field-offset mining, then take
  **economy** to `Confirmed` before attempting damage.

**Why economy before damage**
Damage is the more valuable answer, but economy is the better *first* one: a quarter of the
frontier, an implementation already built to compare against, and `PE-11`'s open question
(rounding point, priority order) has a one-line answer. Passing the confirmation gate once
matters more than passing it on the most interesting subsystem — nothing has passed it yet.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`; `tools/re/DumpCallGraph.java`;
  `build/re-fa/exports/callgraph.tsv` (gitignored, regenerable)

**Next exact action**
- Run Ghidra BSim or Version Tracking with `ART-D001` as the source and `ART-E001` as the
  destination, and measure how many of the 4,875 exported names transfer. Record the match
  count and the false-positive rate before trusting any transferred name.

### 2026-08-29 / Session 07

**Artifact:** `ART-E001` against `ART-D001`, `ART-D003`, `ART-D004`
**Tool/project:** Ghidra 12.1.2 BSim, H2 database at `build/re-fa/bsim/fadb`, template `medium_32`
**Work package:** campaign method, then `WP-30`
**Question:** Does bulk name transfer from the named DLL actually work?

**Accomplished**
- Stood up a local BSim database and committed signatures for `MohoEngine.dll`, `gpgcore.dll`
  and `gpggal.dll` (`build/re-fa/run-bsim-sigs.sh`).
- Wrote `tools/re/BSimTransferNames.java`, **report-only by default** — it will not rename
  without `--apply`, which is what made the following measurements possible before any damage
  was done to the database.
- `C-041`: measured accuracy in two regions and found them completely different — 84% peak and
  degrading on the CRT-heavy control, ~78%+ on the engine frontier.
- `C-042`: recovered 98 candidate names including **`Moho::SIM_MetaImpactArea` at
  `0x0073e950`**, the sim-side damage entry that `C-021` predicted and that sessions 05 and 06
  failed to reach from the Lua side.
- `C-043`: found a real limitation — four identical `Entity::SetVizTo*` siblings all match the
  same name, so BSim cannot order a family of near-identical accessors.

**Corrections to my own strategy**
- Session 06 called DLL name transfer "the biggest unused lever" and predicted thousands of
  names. **It yielded 98.** The strategy section now records the measured result in place of
  the prediction. The technique keeps a place as a frontier tool at Tier 1 cost, not as a bulk
  renamer, and its output is `maybe_` names only.
- The first control was unrepresentative — the executable's "already-named" functions are
  almost all CRT and MFC stubs, nothing like the engine code the campaign cares about.
  Generalising from it would have killed a technique that works. **A control has to resemble
  the target.** This is the second time in three sessions that a bad control produced a wrong
  conclusion (`H-001` was the first).
- The second attempt was also unfair in the opposite direction: querying the Lua *bindings*
  returned zero named matches, because generated glue is unnamed in both binaries. The
  meaningful query is the layer below.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`; `tools/re/BSimTransferNames.java`;
  `build/re-fa/bsim/`, `build/re-fa/exports/bsim.*.tsv`, `frontier_d1.clean.txt` (gitignored)

**Next exact action**
- Disassemble `0x0073e950` (`SIM_MetaImpactArea`). Confirm it by the two independent markers
  already established: it must reach a write to `Entity+0x98` (`C-038`) and consult armour
  (`Unit::GetArmorMult`, `0x006cac80`). That converts `C-042` from a BSim candidate into a
  read fact and gives `WP-30` its damage-ordering answer.

### 2026-08-29 / Session 08

**Artifact:** `ART-E001`
**Tool/project:** Ghidra 12.1.2 project `fa`; `tools/re/ApplyKnownNames.java`
**Work package:** campaign infrastructure
**Question:** Make the campaign survivable across sessions and analysts.

**Accomplished**
- Wrote the recovered knowledge **into the Ghidra database**: 2,036 functions named and
  plate-commented. Named functions went 943 → **2,979**; named code 151 KB → **457 KB**. A
  session now opens a partly labelled binary instead of 31,086 `FUN_` placeholders.
- Encoded confidence in the name prefix — `fa_` is recovered fact, `maybe_` is an ~80% BSim
  lead, Ghidra's own RTTI names are never overwritten. Two existing names were correctly
  skipped. The prefix is the safeguard: a future session will read a name and not this ledger.
- Added the **multi-session hunting kit**: the prefix table, a start-of-session command
  sequence including artifact-hash verification and full project rebuild, the one-Ghidra-process
  rule, the ordered hunting queue with a start address per entry, and an object-layout table.
- Six analysis agents dispatched in parallel on `WP-30`, `WP-15`, `WP-04`, `WP-03`, `WP-26`
  and `WP-19`, working through `objdump` and the exported tables so none of them needs the
  Ghidra project lock. Their findings are integrated by the parent session, which owns this
  document — six writers on one file would corrupt it.

**Why the object-layout table matters more than it looks**
Only one offset is recorded (`Entity+0x98`, health, `float`), and it alone produced a hard
parity constraint. Offsets are small, exact, and durable in a way that prose about a subsystem
is not. A session that recovers ten of them advances the campaign more than one that reads a
single function exhaustively.

**Files and durable outputs**
- `docs/fa-exe-analysis-plan.md`; `tools/re/ApplyKnownNames.java`, `DumpCallGraph.java`,
  `ReportCoverage.java`, `BSimTransferNames.java`
- `ADR-065` in `ADR_DECISIONS.md`; `README.md` and `PLAN2.md` corrected — veterancy was still
  listed as absent in both.

**Next exact action**
- Integrate the six agents' reports into the claim ledger, then take the top hunting-queue
  entry (`WP-30`, `SIM_MetaImpactArea 0x0073e950`) to a traced path with a caller and a
  downstream effect. That would be the first package through the confirmation gate.

### 2026-08-29 / Session 09

**Artifact:** `ART-E001`, `ART-S001`, `ART-S007`
**Tool/project:** six parallel analysis agents on `objdump` + the exported tables (no Ghidra lock)
**Work package:** `WP-03`, `WP-19` reported first
**Question:** Can parallel agents deepen the analysis without colliding?

**Accomplished**
- `WP-03` **answered**: `C-044`–`C-047`, hypothesis `H-002` confirmed. Lua handles are safe by
  construction — resolved fresh per call, nulled at one site, checked by 73 resolvers.
- `WP-19` **fully specified**: `C-048`–`C-052`. Stacking, gates, magnitudes, defects, lifecycle.
- The object-layout table grew from 1 entry to 7.

**Corrections forced by the results**
- **The "no validity check" half of `C-036` was false** and had been repeated to the user as
  "Recoil Metal's handles are strictly safer than retail". They are not; retail is safe too.
  The error was generalising from one four-instruction function to a whole subsystem.
- **Evidence trap 1 was itself wrong.** It said `LC_ALL=C` fixes the corpus-grep problem. On this
  machine `grep` is `ugrep -I`, which skips those files regardless of locale; `grep -a` or a
  Python read with `encoding='latin-1'` is required. An agent hit a false negative on `adjac`
  *after* being warned by the older, incomplete version of the trap. Now corrected in place.
- `C-048` vs `C-028`: adjacency stacks by count (`Stacks='ALWAYS'`), veterancy replaces
  (`Stacks='REPLACE'`). Two buff families, opposite rules, same engine. Any statement of the form
  "retail buffs work like X" is wrong without naming the family.

**Ten regression tests specified for `WP-19`** (from the agent report, worth keeping):
`SkirtRect(UEB1101 @ 10,10) == (9,9,11,11)`; touching adjacent but gap/overlap/corner not;
one T1 pgen on a SIZE16 factory ⇒ `EnergyBuildAdjMod == 0.984375`; sixteen ⇒ exactly `0.75`;
four mass storages on UEB1103 ⇒ `MassProdAdjMod == 1.5` (2.0 → 3.0 mass/s), kill one ⇒ `1.375`;
two T1 + one T2 pgen ⇒ `0.84375` (cross-family additive); twelve T1 pgens on a T3 fabricator ⇒
`EnergyMaintAdjMod == 0.75`, 3500 → 2625 E/s; a giver beside a half-built structure grants
nothing; `T2PowerEnergyBuildBonusSize20 == -0.0125` (assert the retail bug, or document the
deviation); a SIZE-less STRUCTURE receives nothing.

**Next exact action**
- Verify Recoil Metal's `src/core/sim/Adjacency.cpp` against those ten tests; each disagreement
  is a parity bug with a known correct value.

### 2026-08-29 / Session 10

**Artifact:** `ART-E001`, `ART-S007`
**Tool/project:** six parallel agents, `objdump` + exported tables (no Ghidra lock)
**Work package:** `WP-04`, `WP-15`, `WP-26`, `WP-30`
**Question:** Can parallel agents take the hard subsystems, not just the easy ones?

**Accomplished — all six agents returned, all with usable results**
- `WP-04`: beat order is **command dispatch → script → motion** (`C-054`), full 21-step beat
  (`C-055`), RNG is **bit-exact MT19937** seeded 5489 by default (`C-056`), checksum covers only
  **dirty** entities plus the MT state, and deliberately not `mti` (`C-057`).
- `WP-30`: the real funnel is `DealDamage 0x0073dbc0` (`C-059`); order is shield → armour →
  handicap → **Lua** → health write (`C-060`); **retail area damage has no distance falloff**
  (`C-061`); shield absorption is scripted (`C-062`).
- `WP-26`: one global uniform 4-unit grid inside `COGrid` (`C-064`); query order is cell
  row-major → layer array → **chain LIFO**, never sorted (`C-065`); one index shared by damage,
  projectiles, beams, build placement and recon (`C-066`).
- `WP-15`: a **two-ratio** scarcity allocator, refuting both a global throttle and sequential
  consumers (`C-067`); full economy layouts (`C-068`); the only rounding in the economy is
  storage capacity as `int64` (`C-069`).
- The object-layout table went from 7 entries to **35**, now with an explicit base pointer per
  row after `C-053` showed an unnamed base makes an offset meaningless.

**Corrections the agents forced on me**
- `C-053`: my `C-038` "health at Entity+0x98" was mislabelled. It is `Unit+0x98` = `Entity+0x90`,
  because `Unit`'s `Entity` subobject sits at `+0x08`. Two agents caught this independently.
- `C-058`: my `C-042` said `SIM_MetaImpactArea 0x0073e950` was area damage. It is a physics
  **impulse** routine that never touches health or armour. The BSim *name* was right; my
  *interpretation* of what the name implied was an assumption I never checked.
- The corpus-grep trap needed a third correction. `LC_ALL=C` is not enough, `-a` is required, and
  `lua/sim/Unit.lua` contains a real NUL byte. Three agents hit this independently **in one
  session**, after two previous versions of the warning. The standing incantation is now
  `LC_ALL=C grep -a`.

**Two live parity defects in Recoil Metal, with known-correct values**
1. **Area damage falloff.** `src/core/sim/Combat.cpp` applies `share = 1 − distance/radius`.
   Retail applies the **full amount to everything inside the radius** (`C-061`). This changes
   every blast total and therefore every downstream death, wreck and hash.
2. **Query iteration order.** Recoil Metal iterates its spatial index deterministically by slot.
   Retail returns cell row-major, then layer array, then **chain order — LIFO by most recent
   insertion or cell change** (`C-065`). Retail's tie-breaks depend on entity history, so no
   id-ordered or distance-ordered implementation can reproduce them.

**Next exact action**
- Fix the falloff defect: make `damageArea` uniform within the radius, behind a test that pins
  `C-061`. It is a small change with large consequences, so land it alone.

### 2026-08-29 / Session 11

**Artifact:** `ART-E001`, `ART-S001`, `ART-S007`, `ART-S013`
**Tool/project:** five parallel agents on `objdump` + the exported tables; one auditing our own source
**Work package:** `WP-12`, `WP-16`, `WP-19`, `WP-27`, `WP-29`, `WP-33`
**Question:** Can a second wave take the subsystems the first wave did not reach, and can an agent
audit our implementation rather than the binary?

**Accomplished**
- `WP-27` **Analyzed**: selection is `(range/arc class, priority index, score, incumbency)`; the
  score is **turret slew, not distance**, for turreted weapons; anti-projectile acquisition is a
  **separate algorithm**; `TargetPriorities` is not an engine field and `IsValidTarget` is inert
  (`C-090`–`C-095`).
- `WP-29` **Analyzed**: ammo is an `int` on `CAiSiloBuildImpl`; an SMD's interceptors live in the
  **tactical** slot; interception is the ordinary collision path and **damages** rather than
  destroys; three distinct counter-missile mechanisms exist (`C-081`–`C-088`).
- `WP-33` **Analyzed**: nine signed-byte **coverage** grids; **removal is immediate — the deferred
  path is dead code**; stale contacts are retained (`C-076`–`C-079`).
- `WP-12`/`WP-16` **Analyzed**: one site advances a queue; progress is on the target; the economy
  ratio a builder uses is **one beat stale**; assist clamps per contribution (`C-096`–`C-102`).
- The adjacency **audit found two real defects in our own tables** and both are fixed (`C-072`).

**The audit agent was the most valuable per token**
It read our source against a recovered specification rather than reading the binary, and it found a
doubled magnitude row that no playtest would ever surface. Worth repeating for every subsystem that
reaches `Analyzed`: **recovering a spec and then auditing against it catches a different class of
bug than either activity alone.** The invariant did the work — `Add × n` must be constant, so a row
that breaks the product is a transcription slip.

**Corrections forced this session**
- `C-007` refuted: deferred vision removal does not exist in retail. The inference had been drawn
  from a symbol's *existence* rather than from a caller.
- `C-070` refined: `CEconomyEvent` is all-or-nothing about the *tick*, not about the *resources* —
  partial deliveries accumulate, so a starved silo is slow rather than stalled.
- `C-075`/`C-089`: the DLL is a **naming oracle only**. Different build, divergent behaviour,
  `Unit` offsets shifted by `0x10`. Folded into the strategy section.

**Two defects fixed in Recoil Metal, both with known-correct retail values**
1. Area damage had a linear falloff; retail has none (`C-061`, `ADR-066`, commit `aa65554`).
2. Two adjacency magnitude rows were wrong, one of them doubled (`C-072`, commit `bc59bdc`).

**Next exact action**
- Recover the engine's default `Footprint` for blueprints that omit the field. It is the only
  unknown blocking `C-073`(a), which misplaces adjacency for 31 structures including every naval
  factory.

### 2026-08-29 / Session 12

**Artifact:** `ART-E001`, `ART-S001`; and Recoil Metal's own source
**Tool/project:** five agents — one binary analysis, four auditing our implementation
**Work package:** `WP-15`, `WP-19`, `WP-27`, `WP-30`, `WP-33`, `WP-12`/`WP-16`
**Question:** Turn recovered specifications into a defect list.

**Why the mix changed.** Eight packages were `Analyzed` and none `Confirmed`. Reading a ninth
subsystem does not move that; comparing our code against specs we already hold does. The single
analysis agent went at the one unknown blocking a known defect.

**Accomplished**
- `C-109`: the engine's default footprint is **`ceil(SizeX)`** from the top-level key, read at
  `0x00518B60`, with `SkirtSize` clamped up to the footprint and positive offsets discarded.
  **`C-073`(a) is unblocked** and the adjacency fix is writable.
- Four audits produced roughly forty defects across economy, targeting, damage and
  orders/construction/intel — recorded as `C-103`–`C-108` and `C-110`–`C-113`.
- **Fixed:** omni rasterised at the vision mip instead of radar's (`C-113`); two source comments
  asserting the opposite of recovered fact (`C-103`, `C-104`); and the falloff prose that
  outlived the `aa65554` behaviour fix by a session.

**The finding that matters most**
`C-110`. Shield absorption is applied once to the whole blast and tested against the blast
centre, so one small shield near an impact nullifies the area effect for arbitrarily many
targets — including units entirely outside the dome. It **silently cancels most of what the
falloff fix restored**. The agent proved it by building a probe against `librm_core.a` and
measuring, rather than by reading, which is why the claim carries numbers.

**Three things this session says about method**
- **Green tests are evidence of nothing when written from the code's own model.** Ten more were
  identified as pinning behaviour retail does not have. That is now three independent
  demonstrations (adjacency magnitudes, damage falloff, and these).
- **We reproduced a trap we had documented.** `C-094` recorded that retail's `IsValidTarget` is a
  binding the engine ignores; our `SetDoNotTarget` is bound and read by nothing (`C-106`).
- **A behaviour fix is not done until its prose is.** `aa65554` corrected the falloff in code and
  tests and left ten comments teaching the old model, including on the public declaration.

**Next exact action**
- Rewrite the shield block in `Combat.cpp:801-851` so absorption is per target and coverage is
  tested against the target, not the blast centre (`C-110`). Then apply `C-109` to the skirt
  rectangle. Hold the point-path shield question until shield collision exists.

## Confirmation gate

A work package may move to **Confirmed with EXE analysis** only when all are true:

1. Recoil Metal behavior is implemented and covered by focused tests.
2. The exact retail artifact is identified by hash.
3. Relevant native entry points and state owner are in the symbol ledger.
4. The decisive behavior is traced through at least one caller and downstream effect.
5. Existing Lua, blueprint, documentation, and EXE evidence agree, or disagreement is explained.
6. At least one plausible alternative from the possibility envelope is explicitly ruled out.
7. Ordering, numeric precision/rounding, deterministic tie breaks, and failure behavior are recorded
   where they can affect simulation results.
8. The result is summarized in the claim ledger and latest session log.

If the executable cannot distinguish candidates because behavior is delegated entirely to Lua or
data, record that result. Confirmation may then rest on the retail script/data path plus the native
registration bridge, but the reason must be explicit.

## Current sources before disk analysis

- Official *Supreme Commander* PC manual, inherited retail mechanics:
  <https://manuals.thqnordic.com/SupremeCommander/SupremeCommander_PC_Manual_EN.pdf>
- Steam publisher description for Forged Alliance's faction, roster, navy, orbital weapon, and
  counter-intelligence scope:
  <https://store.steampowered.com/api/appdetails?appids=9420&l=english>
- Current implementation and known gaps: `README.md`, `PLAN.md`, `docs/milestones.md`,
  `ADR_DECISIONS.md`.
- Recoil Metal source and tests, especially `src/core/sim`, `src/core/unit`, `src/app`, and `tests`.
- FAF source is supporting evidence only until compared against the owned retail scripts. Every FAF
  citation must state whether the behavior is inherited, changed, or unknown relative to retail.

## Document maintenance rules

- The dashboard is a shortcut, not a second source of truth. Its counts must match the work-package
  table whenever a row changes.
- Readiness never changes merely because confidence increased.
- Confidence never increases merely because a function was renamed.
- Preserve refuted hypotheses and superseded claims; they prevent repeated dead ends.
- Split detailed subsystem findings into a dedicated `docs/fa-exe-<subsystem>.md` only when this
  file becomes difficult to navigate. Leave the dashboard, result summary, and link here.
- Keep one active work-package record per analyst/session unless two questions are genuinely
  independent.
- Every session ends with one exact next action.
