# Lua ↔ EXE contract: reimplementation guide

How the retail Forged Alliance executable uses its shipped Lua — and what a C++
port must preserve. Scope: **sim-side Lua only** (`lua.scd` 369 files,
`mohodata.scd` 91 files, `units.scd` 419 `*_script.lua`, `schook.scd` 8 files;
2,091 `.lua` files total in `build/re-fa/corpus/`). UI-side Lua (`/lua/ui/…`)
is out of scope except where it issues sim orders.

Purpose, per the goal: run the shipped Lua as-is, but document the contract so
(i) we can check we call it correctly, (ii) hot paths can move to C++ later
without behavior change. Every section ends with **Port notes** — the exact
semantics a native reimplementation must preserve.

Conventions: `[LUA-R] path:line` = shipped-Lua locator (1-based, decoded
ISO-8859 view; on-disk files are CRLF + `#`-comment LuaPlus dialect). `[EXE]`
= native locator in preserved `ART-E001`
(`c6783580…960aa0`, PE32 x86, base `0x00400000`). `N→L` = engine calls script;
`L→N` = script calls engine binding (`moho.methods.tsv` row / address).
`L→L` = script-to-script (no engine hop — port freely, keep ordering).

Ledger cross-refs: `C-xxx` = `docs/fa-exe-analysis-plan.md` claim.

---

## 1. Corpus map

| Archive | Files | Role |
|---|---|---|
| `lua.scd` (`corpus/lua/lua/`) | 369 | Sim core (`sim/Unit.lua` 3,715 lines, `sim/Projectile.lua`, `aibrain.lua`, `SimCallbacks.lua`, `SimUtils.lua`), tasks (`sim/tasks/`), default weapon/unit bases via mohodata fallthrough |
| `mohodata.scd` (`corpus/mohodata/lua/`) | 91 | Engine-adjacent bases: `sim/Unit.lua` stub, `sim/weapon.lua` (519 lines), `sim/defaultweapons.lua` (~760), `sim/ScriptTask.lua`, `sim/Entity.lua` (the ONLY `Entity.lua` — `lua.scd` has none) |
| `units.scd` (`corpus/units/units/`) | 419 `*_script.lua` | Per-unit subclasses (ACU scripts, `defaultunits.lua` hierarchy) |
| `schook.scd` (`corpus/schook/`) | 8 | Load-bearing hook payload (`SimSync.lua` hosts `SimUnitEnhancements` registry) |
| `mods.scd` | 12 | Mod-support scaffolding |

Dialect: LuaPlus 5.0.1 + `#` line comments, `!=`, `continue`, bare-table `for`
(`C-023`, `C-305`–`C-319`). No retail watchdog: error path is WARN-and-Abort-thread
(`C-452`). Recoil Metal deliberately ADDS one (fuelHook/LUA_MASKCOUNT) — the one
documented FA-LUA divergence.

---

## 2. Class hierarchy

Native inheritance (`moho.classes.tsv`): `Moho::Entity` (#2, 130 fns) ←
`Moho::Unit` (#1, 178 fns), `Moho::Projectile` (#15, 42 fns), `Moho::Prop`,
`Moho::Shield`, `Moho::ReconBlip` (siblings, `C-003`).

Lua inheritance is FLAT method tables, not native mirroring:

```text
Entity = Class(moho.entity_methods)        # mohodata/lua/sim/Entity.lua:8 ONLY
Unit   = Class(moho.unit_methods)          # lua/lua/sim/Unit.lua:50 (single base!)
  borrows Entity via import, NOT via Class() — calls Entity.OnCreate explicitly (Unit.lua:125)
  ├─ DummyUnit = Class(Unit)                # defaultunits.lua:27 (OnStopBeingBuilt→Destroy)
  ├─ StructureUnit = Class(Unit)            # :36 (+OnCreate/OnStopBeingBuilt/adjacency/build/kill)
  │    ├─ FactoryUnit (+Land/Air/Sea/QuantumGate) — :422–1181
  │    └─ 15 econ/structure classes (:701–1180), ShieldStructureUnit.OnDamage (:1160, ONLY OnDamage override in file)
  └─ MobileUnit = Class(Unit)               # :1194
       ├─ WalkingLandUnit (:1280), SubUnit (:1319), AirUnit (:1384), … (SeaUnit, Amphibious…, Transport* similar)
```

Per-unit scripts subclass these (`UEL0001_script.lua: ACU = Class(MobileUnit)`-family;
`XSL0402` Othuy = 27-line `SEnergyBallUnit` subclass — all bespoke ability is shipped
Lua, `C-260`/`C-381`). Method tables: `moho.entity_methods` (`00e7743c`),
`moho.unit_methods` (`00e7a800`), `moho.projectile_methods` (`00e79290`).

**Port notes:** (1) Lua `Class()` is single-inheritance method tables — a C++ port
models them as interfaces over one entity handle, NOT as the C++ class tree.
(2) `Entity.lua` exists ONLY in mohodata — searching `lua.scd` alone is a
false-negative trap (same trap as `schook/SimSync.lua`, `C-379`). (3) Retail bug
compatibility: `WorkItemBuildCostMass = BuildCostEnergy` copy-paste (`Unit.lua:2006`,
`C-376`) and `#`-comments/`!=` dialect MUST be preserved by any reimplementation.

---

## 3. Lifecycle: construction → death

### 3.1 Construction preamble (all sim entities; `C-318`: OnPreCreate → native setup → OnCreate)

| # | Step | Dir | Evidence |
|---|---|---|---|
| 1 | Script-class resolution: `<bpid>_script.lua` `TypeClass`, else bp `ScriptModule/ScriptClass`, fallbacks `Unit/Projectile/Prop` | N | `C-318` (EXE `0x0067ddf0`,`0x006a1a70`,`0x00680610`) |
| 2 | `OnPreCreate()` — EntityId + army ALREADY assigned (`Sync.id/army` read them); must create `Trash`/`TrashOnKilled`, `Sync+SyncMeta`, `EventCallbacks` table (21 keys incl. `OnKilled/OnCaptured/OnDamaged/OnVeteran`); MUST NOT call `Entity.OnCreate` (commented out, `Unit.lua:99`) | N (pre-setup) | [LUA-R] `sim/Unit.lua:85-122`; stub `:37-54` |
| 3 | Native setup (blueprint attach, army, transform, `MemberConstruct`) | N | `Moho::Unit` #1 / `Entity` #2 |
| 4 | `OnCreate(spec)` — blueprint/army/position/layer valid; health ALREADY set (never assign); `HideLandBones`, volume-based damage FX, `SetupIntel`, state inits | N (post-setup) | [LUA-R] `sim/Unit.lua:124-…`; 8 push sites (unit/prop/projectile/shield/…): `0x00629380`,`0x00679a10`,`0x006a1a70`,`0x006abe80`,`0x006dad90`,`0x00700810`,`0x0072b740`,`0x0072bbd0` |

### 3.2 Death pipeline (multi-beat, NOT an event)

`OnKilled(self,instigator,type,overkillRatio)` [LUA-R] `sim/Unit.lua:896-943`:

1. `Dead=true` FIRST. Factory `Kill()`s unfinished `UnitBeingBuilt` (reentrant Kill).
2. Death anim / `OnKilledVO` / `DoUnitCallbacks('OnKilled')` / `DestroyTopSpeedEffects`.
3. `instigator:OnKilledUnit(self)` (L→L) → `CheckVeteranLevel` — single-step `if`
   (max +1); `AddKills(n)` while-loop is the ONLY multi-level path (`:3084`).
4. `DoDeathWeapon()` (BEFORE `DeathThread` fork — credit precedes detonation).
5. `DisableShield/DisableUnitIntel`; fork `DeathThread` (async `Destroy` after
   `DeathThreadDestructionWaitTime`).
6. NATIVE `KILLS++` happens only after return (lock-xadd writer `0x00698aef`,
   `C-437`/`C-449`) — the veterancy +1 quirk falls out of this ordering.

`OnDestroy` [LUA-R] `sim/Unit.lua:1244-1290` teardown order is load-bearing:
sync-table nulling → factory `Destroy()`s (NOT `Kill`) unfinished product →
`Trash`/effect-bag cascade (idempotent — Destroy-during-Destroy reentrancy) →
`RemoveEconomyEvent`/`RemoveAllUnitEnhancements` → `ChangeState(DeadState)`.

**Port notes:** death spans beats (kill → anim → destroy); never collapse to one
call. Callbacks may `Kill`/`Destroy` self mid-chain — post-adjust code must handle
dead self. `OnHealthChanged(new,old)` fires ONLY on quarter-bucket change
(`floor(f*4)*0.25`, `C-063`); `SetHealth` also relinks the checksum dirty list
(`C-057`) — quantise-then-compare or checksums diverge.

---

## 4. Script tasks (`CUnitScriptTask` bridge, `C-228`/`C-388`)

Loader [EXE] `0x00629530`: reads `TaskName` from the order table →
sprintf `/lua/sim/tasks/%s.lua` → load via `0x004056b0`; miss logs and falls back
to `/lua/sim/ScriptTask.lua`. Base `TaskTick` ERRORS LOUD on missing TaskName
(mohodata `ScriptTask.lua:67-72`) — never silent.

Sequence: N→L `OnCreate(commandData:TABLE, 1 arg)` once → N→L `TaskTick()` (0 args,
polled every tick, returns int TASKSTATUS scalar: Done=−1/Suspend=−2/Abort=−3/
Delay=−4/Repeat=0/Wait=1, N>1 = N-tick wait) → N→L `OnDestroy()` (0 args).

Native surface (only 4 bindings): `GetUnit()→unit` (`0x0062a1b0`… rows 832),
`SetAIResult(int 0..3)` (`0x0062a040`), latent helpers `LUnitMove(self,target)` /
`LUnitMoveNear(self,target,range)` — both DEAD surface (zero shipped callers,
comment-only mention; `C-437`).

Live entries: `EnhanceTask` (`corpus/lua/lua/sim/tasks/EnhanceTask.lua:11-80`;
dispatch: UI `construction.lua` / `platoon.lua:290` / ability RULEUCC_Script →
`IssueScript 0x006fd240` / ctor `0x00629380` / serializer `0x0062af20`).
Enhancing loop: `Stopping` admits immobile immediately else `AbortMove`+Wait;
`Enhancing` advances ONLY when `GetResourceConsumed()>0` (funded-only);
0.25/0.50/0.75 milestones edge-triggered via LastProgress ratchet; completion at
≥1.0 via `OnWorkEnd`; cancel = `OnWorkFail`, no refund. Registry write
(`AddUnitEnhancement`) lives in the UNIT's `WorkingState.OnWorkEnd`, NOT in the
task (`C-436`). `TargetLocation` has NO shipped issuer (dormant/engine path).

**Port notes:** preserve funded-only progress, ratchet edges, record-after-loop,
`WorkItemBuildCostMass=BuildCostEnergy` bug, and the 3-arg `OnBuildProgress`
shape (`Unit.lua:1731`, base no-op, zero overrides — keep call for mod compat).
`OnFire` has 3 per-state variants (Idle-advance / Charge-swallow / Reload-swallow)
+ FiringState has NO handler — one global `OnFire` breaks counted/unpacking flows.

---

## 5. Weapon fire-state machine

Classes: `Weapon` (mohodata `weapon.lua`, 519 lines — turret/anim/sound/buff base);
`DefaultProjectileWeapon` (`defaultweapons.lua:22-~700` — rack/muzzle machine);
`Kamikaze/BareBones/Beam` variants. Full state graph in auditor output; key shape:

- IDLE →[N→L `OnGotTarget()`]→ counted? require `CanFire()` (Lua gate
  `defaultweapons.lua:416`) else RETURN → unpack/charge/fire-ready branches.
- `RackSalvoFiringState.Main` (`:510-658`): per rack/muzzle — Show/HideBone →
  charge → `PlayFxMuzzleSequence` → `CreateProjectileAtMuzzle` → native
  `CreateProjectile` + `PassDamageData(DAMAGETABLE)` → counted? decrement silo
  (`:584-591`, AFTER creation at `:582` — order load-bearing, `C-085`) → buffs +
  `OnWeaponFired()` → exit branch (reload/charge/pack/idle matrix).
- N→L `OnLostTarget` / `OnHaltFire` (`HaltFireOrdered=true`, mid-loop `continue`).
- Native double gate: Lua `CanFire()` + `UnitWeapon::CanFire 0x006e0130`
  (arming + `HasSiloAmmo 0x006dbf80` + water/bomb gates `C-327`/`C-492`/`C-494`).

Native rows: `CanFire→bool` (`0x006e0130`), `WeaponHasTarget` (binding `HasTarget`,
`0x006df870`), `CreateProjectile(muzzle:string)→projectile|nil` (`0x006def90`,
nil-checked EVERY site), `FireWeapon→bool` (`0x006df9b0`, one corpus caller),
`SetTargetingPriorities(ARRAY of parsed-category userdata)` (`0x006e0fe0`).
**Correction:** `RedirectingState` is the missile-redirect ENTITY
(`defaultantiprojectile.lua:110-148`), not a weapon state — always-false collision
return + DamageData mutation-before-impact + rate-wait-before-untrack are all
load-bearing.

**Port notes:** `DamageFriendly` nil→true default; Buffs shallow-copied per shot
minus Disabled entries; `SetWorkProgress` is one float channel shared by enhance
progress + `RenderFireClock` (last-writer-wins); weapon-index is 1-based in Lua;
`PlaySound(weapon, paramsTABLE)` is TWO args in order; `GetCurrentLayer()` returns
the layer STRING (authoritative sim state, read per event).

---

## 6. Intel contract

- Refcount wrapper: Lua `IntelDisables[string]→int`; nil = broadcast-all; edge-only
  native calls (`EnableIntel`/`DisableIntel (type:STRING)`, `0x00694b60`/`0x00694e00`).
  WaterVision forced on water layers; layer toggle swaps Vision↔WaterVision.
- `IntelWatchThread`: `WaitSeconds(0.5)` poll, EXACT-equality `fraction==1` inner
  loop (do NOT epsilon-ize — `C-284`); brownout → disable-all → wait
  `Reactivatetime|10` → re-enable. Forked once (`if not self.IntelThread`),
  self-clears on exit.
- N→L `OnDetectedBy(index = armyIndex+1)` (`Unit.lua:1940`): `DetectedByHooks`
  fan-out, then faction-switched brain call. Add/Remove hook = linear table ops.
- N→L brain `OnIntelChange(blip, reconType ∈ {LOSNow,Radar,Sonar,Omni}, val:bool)`
  (`aibrain.lua:688`): IntelTriggerList scan with category+type+blip+value+brain
  match; OnceOnly nils the entry. Native fires per changed BIT (`C-282`).

---

## 7. AI / brain / platoon

- Brain tables init (`aibrain.lua:428-432` CreateBrainShared): TriggerList,
  IntelTriggerList, VeterancyTriggerList, PingCallbackList, UnitBuiltTriggerList.
  InterestList lazy (`:3779-3789`): High/LowPriority + MustScout
  (`{Position, LastScouted, TaggedBy}`); `ParseIntelThread` sweeps every 5s
  (dedup VDist2Sq<10000, re-sort by LastScouted).
- Veterancy ordering (`Unit.lua:3154-3191`): level → Regen+Health buffs →
  blueprint buffs → `brain:OnBrainUnitVeterancyLevel` (`:3190`) →
  `DoUnitCallbacks('OnVeteran')` (`:3191`). Brain triggers ALWAYS before unit
  callbacks. Trigger spec `{CallbackFunction, Category, Level}`, CallCount
  defaults 1 (single-shot); callback gets `(unit)` ONLY, level not passed.
- Platoon: `Platoon = Class(moho.platoon_methods)` (`platoon.lua:23`); 49 native
  methods, 31 live (`C-476`). Top shapes: `GetPlatoonUnits()→{units}` (124 sites),
  `GetBrain()`, `Stop()`, `MoveToLocation(pos,useTransports[,squad])` (bool =
  transport flag), `AggressiveMoveToLocation(vec3)→cmd`, `Patrol` (polymorphic),
  `IsCommandsActive(cmd)→bool` (THE poll predicate), `GetPlatoonPosition()`
  (falsy when empty). Factory trio: `MakePlatoon(name,plan)` /
  `AssignUnitsToPlatoon(plat,{units},squad,formation)` / `BuildPlatoon(template,…)`.
  Ferry chain (`ScenarioPlatoonAI.lua:1136-1151`, sole `UseFerryBeacon` caller):
  VarTable gate → 3s poll for beacon → `UseFerryBeacon(ALLUNITS, beaconTABLE)` →
  3s `IsCommandsActive` poll → return; platoon death mid-ferry is NORMAL exit.
  Poll idiom: `cmd=native(...) ; while IsCommandsActive(cmd) do WaitSeconds(3)`
  (+`PlatoonExists` guard, 30-cap at `platoon.lua:2662`).
- SimCallback wire (20 entries, `SimCallbacks.lua`): N→L dispatch reads
  `.Func`/`.Args`, arity 1–2, linear whitelist scan (`0x008c06b0`), vtable
  `+0x88` into sink `0x112626c` (`C-434`). Refusal ("No callback named") is
  LUA-side (`DoCallback`). Per-entry arg shapes enumerated in auditor output
  (fractions for resources, `{From,To}` ints, WIRE `units=[entityIds]`,
  `OkayToMessWithArmy` NATIVE auth gate `0x007113a0`). Engine-sourced entries
  (`OnPlayerQuery`, `OnCameraFinish`) live in mohodata (`SimPlayerQuery.lua`,
  `SimCamera.lua`) — a port must include mohodata, not just `lua.scd`.
- Victory: 3s poll, 15s stable window (re-arm, not cancel), win-before-draw,
  defeat arm never ends the match (`C-408`/`C-422`); transfer snapshot/restore
  field ORDER in `SimUtils.lua:46-134` (`C-385`).

---

## 8. Native→Lua callback inventory (hot paths)

Conventions: arity = handler params AFTER self. Push sites = string-push VA in
function VA (proves engine dispatch — a C++ port MUST implement each).

| Callback | Push sites | Handler | C++ hazard |
|---|---|---|---|
| `OnPreCreate(self)` | `0x006a1f98`, `0x006ac2a3` | `Unit.lua` | Pre-setup: no native reads valid |
| `OnCreate(self[,spec])` | 8 sites (unit/prop/projectile/shield/…​) | `Unit.lua`, `Projectile.lua:64`, `Prop.lua:24`, `shield.lua` | Post-setup (`C-318`); every creatable class needs the hook |
| `OnDestroy(self)` | 6 sim + 1 UI (`Entity::OnDestroy` vtable `0x80`) | `Unit.lua:1244` | Teardown order load-bearing; double-destroy idempotency |
| `OnDamage(self,instigator,amount,vector,type)` | 2 invokers (weapon-hit + DealDamage) | `Unit.lua:787` | Arbitrary-Lua-before-AdjustHealth; snapshot pre-health; self may die mid-call |
| `OnKilled(self,instigator,type,overkillRatio)` | `0x006b7530`, `0x00701680` | `Unit.lua:896` | Multi-beat pipeline (§3.2); NATIVE KILLS++ after return |
| `OnCollisionCheck(self,other,weapon)→bool` | `0x006a6140`, `0x006e4050` | `Unit.lua:972,1005` | FALSE vetoes mid-query; NO world mutation inside veto |
| `OnImpact(self,type,entity)` | 3 sites | `Projectile.lua:259` | Reentrant damage; instigator fallback rule |
| `OnLayerChange(new,old)` | 8 sites | `Unit.lua:2101` | Idempotent (duplicate fires); starts/stops motion FX |
| Motion events ×4 (+fan-out to weapons) | fan-out `0x006bff20` + setters | `Unit.lua:2133,2206,2267` | Change-detect only (`C-328` tables); unit→weapons order |
| Anim-collision trio | `0x0063e760` via `0x0063f440` | `Unit.lua:2289` | Only effect→sim feedback; mid-update reentrancy |
| `OnGetDamageAbsorption(…​)→float` | `0x00740db0` ← `0x0073d930` | `shield.lua:100` | PER SHIELD in list order, store floats, then apply (`C-396`); Lua already includes armor+handicap |
| Capture sextet | `0x0060aeb0` + `0x0060b8e0` | `Unit.lua:472-513,555` | Native order (`C-237`); transfer REPLACES unit identity |
| Build progress family | 8 sites | `Unit.lua` | 0.25/0.5/0.75 crossing only (`C-102`); Stop on ALL exits |
| Transport family (9 cbs) | attach/detach/beam/load/full sites | `Unit.lua` | Kill-path detaches ALL cargo first (0.99 roll, `C-197`); transfer path re-dispatches |
| Weapon trio | fire/got/lost/halt sites | faction `*weapons.lua` | Per-shot script cost is real |
| Enable/disable ×4 pairs | unit + weapon + pause + scriptbit sites | `Unit.lua` | Separate pairs — do not merge; repeat-queue has 2 sites |
| `OnHealthChanged(new,old)` | `Entity::SetHealth` | `Unit.lua` | Quarter-bucket only + checksum relink |
| Intel: `OnDetectedBy`, `OnFerryPointSet`, `OnTransportReject` | intel/ferry/intake sites | `Unit.lua` | Intake-time (pre-queue), not mid-task |
| `OnAssignedFocusEntity` | **17 sites, NO Tier-0 handler** | ⚠️ unclaimed | Most-pushed callback in the image — locate consumer or preserve blindly |
| Brain/economy/scenario | stats/cap/transport/army-brain sites | `aibrain.lua` | One-shot removal-before-delivery (`C-227`); gate-at-creation (`C-383`) |

Two pushes have NO Tier-0 handler (`OnSetTeleportDest`, `OnTarget`) and one
(`OnAssignedFocusEntity`, 17 sites) has no located consumer — verify before
porting; do not drop the dispatch.

---

## 9. Top Lua→native calls (hot paths for optimization)

By corpus frequency: `GetBlueprint()` ~158 (mutable TABLE alias — cache ref,
never deep-copy); `PlayUnitSound(string)` ~55 (Lua-defined, nil-safe);
`GetCurrentLayer()→STRING` ~34 (authoritative sim state, read per event);
`ForkThread(fn,owner,…)→thread` ~24 (TrashBag-owned); intel wrappers ~19
(two-tier refcount→single edge call); `SetBusy(bool)` ~16; `PlaySound(weapon,TABLE)`
~15 (TWO args, order matters); `GetArmy()→INT` ~14 (0-based; +1 at boundary);
`GetUnitId()→STRING` ~13; `DoUnitCallbacks(type,param)` ~13 (Lua-defined);
`GetPosition([bone])→vec3 TABLE` ~12 (1-indexed table, never tuple);
`GetAIBrain()` ~12; `IsDead()→bool` ~11 (⚠️ NO sim-Unit TSV row — predicate
unclaimed, do NOT merge with `BeenDestroyed`); `CUnitScriptTask.GetUnit` ~10
(the ONLY unit handle tasks hold); `GetEntityId()→INT` ~10; bone validators ~7;
`GetWeaponCount/GetWeapon(1-based)/GetWeaponByLabel` ~7; `CreateProjectile` ~6
(nil-check EVERY site); `SetWorkProgress(0..1)` ~6 (shared float channel).

**Optimization guidance:** cache `GetBlueprint` refs; batch per-shot weapon Lua
(`OnFire` runs per shot per weapon — real cost); `OnIntelChange` must not
allocate per fire (high frequency); motion-event fan-out is unit→weapons ordered;
`IsDead` needs a disasm follow-up before inlining any predicate.

---

## 10. Optimization notes (what moves to C++ safely)

1. **Pure-Lua, zero native surface — port freely:** veterancy promotion
   (`CheckVeteranLevel`/`AddKills`), adjacency stacking counts, victory polling,
   platoon Lua wrappers (`GetUnitsAroundPoint`, `GatherUnits`), SimCallback
   refusal path. Keep exact ordering + retail bugs (`WorkItemBuildCostMass`,
   exact-float `fraction==1`, 1-based indices, `DamageFriendly` nil→true).
2. **Thin native bridges — port the Lua half, keep the call:** intel refcount
   tier, `OnBuildProgress` dead hook (mod-compat call), `TargetLocation`
   (dormant), `LUnitMove/Near` (dead surface — do NOT port callers that don't exist).
3. **Hot native↔Lua boundaries — batch, don't inline:** per-shot `OnFire`,
   per-shield absorption loop (order + stored floats), per-bit intel dispatch,
   motion-event fan-out. Inlining changes tick timing; batching preserves it.
4. **Reentrancy hazards (port MUST handle):** damage-callbacks-kill-self,
   Destroy-during-Destroy, Kill-during-Kill (transport cargo), veto-mutates-world
   (forbidden), transfer-replaces-identity, factory-kills-unfinished-product.
5. **Do-not-port list:** `OnAssignedFocusEntity` (consumer unknown),
   `OnSetTeleportDest`/`OnTarget` (handlers unlocated), sim-`IsDead` predicate
   (unclaimed — use `BeenDestroyed` semantics only where proven).

Audit basis: 4 parallel auditors (class/tasks/AI/native) over 2,091-file corpus +
`moho.methods.tsv` + `strings.xrefs.tsv` (281 On* rows / 157 names / 159 invokers) +
`callgraph.tsv`; all addresses re-verified with `disasm_annotated.py` against
`ART-E001`. Doc claims spot-verified by author (`Unit.lua` lifecycle reads,
`ScriptTask.lua` base contract); auditor outputs retained at
`agent://LuaClass`, `agent://LuaTasks`, `agent://LuaAIBrain`, `agent://LuaNative`.
