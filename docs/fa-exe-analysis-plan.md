# Retail Forged Alliance executable analysis plan

This is the durable control document for a long, multi-session clean-room analysis of the
owned retail *Supreme Commander: Forged Alliance* executable and its relationship to the
shipped Lua, blueprints, archives, maps, and data.

The purpose is not to decompile the entire program. The purpose is to answer bounded questions
about native engine behavior before Recoil Metal implements more Forged Alliance parity.

## What is ready

**Campaign state:** unblocked for static executable analysis. The owned, hash-identified
installation associated with Steam app `9420` is connected, and its complete `bin` and `gamedata`
directories have been preserved and hash-verified. An authoritative Steam depot manifest is still
needed to prove the exact retail build and completeness.

**Next exact action:** install and pin Ghidra, create `build/re-fa/project`, import the preserved
`ART-E001`, and complete the initial PE section/import/compiler survey for `WP-01` without executing
the binary.

| Readiness | Count | Meaning |
|---|---:|---|
| **Not ready** | 26 | Recoil Metal is absent or materially incomplete for this work package. |
| **Ready but not confirmed** | 19 | Recoil Metal has a tested implementation, but native retail semantics have not been confirmed from the executable. |
| **Confirmed with EXE analysis** | 0 | The implementation is ready and the relevant retail native behavior has been traced, recorded, and compared. |

### Ready but not confirmed

`WP-04` tick/update determinism, `WP-05` VFS, `WP-06` blueprint ingestion, `WP-09`
map loading, `WP-10` skirmish setup/factions, `WP-12` command queues, `WP-15` economy,
`WP-16` construction/upgrades/assist, `WP-19` adjacency, `WP-20` ordinary ground movement,
`WP-23` surface naval movement, `WP-26` spatial collision, `WP-27` target acquisition,
`WP-28` ordinary weapons/projectiles, `WP-30` damage/armor/death, `WP-31` ordinary shields,
`WP-33` intel/counter-intel, `WP-39` core player UI, `WP-43` command replay/state hashing.

### Not ready

`WP-00` artifact provenance, `WP-01` binary map, `WP-02` Moho registration map, `WP-03`
native object lifecycle, `WP-07` retail Lua host, `WP-08` mod hooks, `WP-11` match rules/caps,
`WP-13` persistent unit controls, `WP-14` factory automation, `WP-17` repair/capture/gifting,
`WP-18` complete wreck semantics, `WP-21` formations/dynamic blockage, `WP-22` aircraft,
`WP-24` submerged warfare, `WP-25` transports/attachments, `WP-29` missiles/interception,
`WP-32` shield variants, `WP-34` enhancements, `WP-35` veterancy, `WP-36` experimentals,
`WP-37` terrain deformation, `WP-38` retail AI behavior, `WP-40` advanced UI/controls,
`WP-41` rendering/animation fidelity, `WP-42` audio/music behavior, `WP-44` save/resume.

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
| `WP-00` | Artifact provenance and version identity | Not ready | High | Anchored | n/a | P0 | Steam app `9420`; executable version `1.5.0.1`, complete `bin` set, and all SCDs are hashed. Original Steam depot/build manifest and transfer history remain unknown. |
| `WP-01` | PE architecture, sections, imports, RTTI, symbols, global map | Not ready | Low | Unexamined | `PE-02` | P0 | PE32 x86, linker-version field 8.0, stripped relocations, large-address awareness, timestamp, and imports observed; sections, compiler attribution, symbols, RTTI, and globals remain unsurveyed. |
| `WP-02` | Moho/Lua native registration map | Not ready | Medium | Unexamined | `PE-27` | P0 | FAF annotation stubs enumerate names, but retail addresses and exact registration set are unknown. |
| `WP-03` | Native object identity, ownership, lifecycle, destruction | Not ready | Low | Unexamined | `PE-02`, `PE-03` | P0 | Lua callback surface bounds lifecycle; native storage and stale-reference rules unknown. |
| `WP-04` | Simulation tick, phase order, RNG, determinism | Ready but not confirmed | Medium | Unexamined | `PE-01` | P0 | 10 Hz and callback clues are strong; native phase order, numeric types, and RNG ownership unknown. |
| `WP-05` | VFS, SCD mounting, override precedence | Ready but not confirmed | High | Unexamined | `PE-27` | P2 | Archive behavior is well bounded by shipped layout and mods; exact retail precedence still needs tracing. |
| `WP-06` | Blueprint loading, merge/default rules, categories | Ready but not confirmed | High | Unexamined | `PE-27` | Corpus gives strong output evidence; native defaults and merge ordering may differ. |
| `WP-07` | Retail Lua dialect, scheduler, callbacks, script objects | Not ready | Medium | Unexamined | `PE-01`, `PE-27` | Shipped scripts bound syntax/API; VM integration and coroutine/event ordering need analysis. |
| `WP-08` | Mod manager, hooks, UI/sim mod boundaries | Not ready | Medium | Unexamined | `PE-27` | Manual and mod conventions bound capabilities; exact retail load and sandbox rules unknown. |
| `WP-09` | Map/scenario loading and start markers | Ready but not confirmed | High | Unexamined | `PE-27` | Binary readers and corpus tests are strong; scenario-rule integration is incomplete. |
| `WP-10` | Skirmish setup, four factions, armies/alliances | Ready but not confirmed | High | Unexamined | `PE-25` | Core seating and faction identity work; retail lobby option translation is not confirmed. |
| `WP-11` | Victory modes, sandbox, unit caps, rule state | Not ready | High | Unexamined | `PE-25` | Manual names expected modes; native predicates, categories, timing, and cap accounting unknown. |
| `WP-12` | Command authorization, queues, cancellation, patrol | Ready but not confirmed | Medium | Unexamined | `PE-14`, `PE-25` | Deterministic command path exists; retail queue mutation and edge cases need tracing. |
| `WP-13` | Persistent fire state, toggles, priorities, mutable capabilities | Not ready | Medium | Unexamined | `PE-14`, `PE-21` | UI/Lua expose controls; authoritative storage and update effects unknown. |
| `WP-14` | Factory repeat, pause, rally, queue editing, mirroring | Not ready | High | Unexamined | `PE-14` | Manual bounds user behavior; sim ownership and serialization remain unknown. |
| `WP-15` | Mass/energy income, storage, upkeep, stalls, allocation | Ready but not confirmed | High | Unexamined | `PE-11` | Lua/data and current tests bound outcomes; native precision, ordering, and priorities unknown. |
| `WP-16` | Construction, upgrades, assist, build progress | Ready but not confirmed | High | Unexamined | `PE-12`, `PE-13` | Common gameplay is implemented; unfinished-unit lifecycle and exact cancel/refund semantics unknown. |
| `WP-17` | Explicit repair, guard, capture, gifting, ownership transfer | Not ready | Medium | Unexamined | `PE-13` | Lua/docs bound commands; common-work versus separate native mechanisms is unknown. |
| `WP-18` | Wreck creation, damage, collision, rebuild, reclaim value | Not ready | Medium | Unexamined | `PE-18`, `PE-23` | Reclaimable records exist; world-object and retail value semantics are incomplete. |
| `WP-19` | Adjacency geometry and economy effects | Ready but not confirmed | High | Unexamined | `PE-11` | Buff tables and geometry strongly bound behavior; stacking/rounding/order need confirmation. |
| `WP-20` | Ground movement, motion classes, ordinary pathing | Ready but not confirmed | Medium | Unexamined | `PE-04`, `PE-06` | Deterministic A* works; retail route representation, caching, steering, and stuck behavior unknown. |
| `WP-21` | Formations, coordinated movement, congestion, dynamic blockage | Not ready | Low | Unexamined | `PE-05`, `PE-06` | User-visible formation controls are known; native grouping and replanning are poorly known. |
| `WP-22` | Aircraft flight, bombing runs, fuel, staging, landing | Not ready | Low | Unexamined | `PE-08` | Blueprint parameters and visible behavior bound states; native mover architecture is unknown. |
| `WP-23` | Surface naval movement and ordinary combat | Ready but not confirmed | Medium | Unexamined | `PE-09` | Water routing and surface targeting exist; draft, turning, beaching, and weapon-domain details unknown. |
| `WP-24` | Submarines, depth, surfacing, torpedoes, water vision | Not ready | Low | Unexamined | `PE-09`, `PE-16` | Motion classes and Lua names bound capabilities; depth/layer representation is unknown. |
| `WP-25` | Transports, cargo, attachments, ferry, staging reuse | Not ready | Low | Unexamined | `PE-10` | Manual and attachment API bound lifecycle; native ownership graph is unknown. |
| `WP-26` | Spatial index, collision layers, query ordering | Ready but not confirmed | Low | Unexamined | `PE-07` | Recoil Metal has deterministic indices; retail data structure and tie ordering are unknown. |
| `WP-27` | Weapon target acquisition, priorities, arcs, retargeting | Ready but not confirmed | Low | Unexamined | `PE-15` | Blueprint constraints are known; native candidate ownership, cadence, and tie breaks are not. |
| `WP-28` | Direct fire, beams, ballistic projectiles, impact | Ready but not confirmed | Medium | Unexamined | `PE-16`, `PE-18` | Generic physical combat works; retail projectile class/state details need tracing. |
| `WP-29` | Tactical/strategic missiles, ammo, interception | Not ready | Low | Unexamined | `PE-17` | User behavior and blueprint categories are known; native ammo/projectile/defense model is unknown. |
| `WP-30` | Damage, armor, area falloff, death blasts, friendly fire | Ready but not confirmed | Medium | Unexamined | `PE-18` | Damage matrices and current behavior exist; callback, shield, overkill, and death ordering need tracing. |
| `WP-31` | Ordinary area shields | Ready but not confirmed | Medium | Unexamined | `PE-19` | Bubble data and visible rules are bounded; current damage-gate architecture may differ from retail collision. |
| `WP-32` | Personal, transport, enhancement, overlapping shield variants | Not ready | Low | Unexamined | `PE-19`, `PE-21` | Variant distinctions are known; native representation and pass-through rules are not. |
| `WP-33` | Vision, radar, sonar, omni, cloak, stealth, jamming | Ready but not confirmed | Medium | Unexamined | `PE-20` | Most authored fields and outcomes are implemented; memory, update cadence, and contact identity need confirmation. |
| `WP-34` | ACU/SCU enhancements and mutable abilities | Not ready | Low | Unexamined | `PE-21` | Slots/costs/effects are visible in Lua/data; authoritative mutation architecture is unknown. |
| `WP-35` | Veterancy, kill credit, promotion, regeneration | Not ready | Medium | Unexamined | `PE-22` | Retail outcomes are documented; threshold ownership and native/Lua split need tracing. |
| `WP-36` | Experimentals and unit-specific special abilities | Not ready | Low | Unexamined | `PE-10`, `PE-16`, `PE-21` | Generic units can load; bespoke script/native interactions vary by unit and remain largely unknown. |
| `WP-37` | Mutable terrain, craters, path/render invalidation | Not ready | Low | Unexamined | `PE-24` | Visible deformation exists; whether gameplay height changes and how systems invalidate are unknown. |
| `WP-38` | Retail AI native boundary, threat, platoons, managers | Not ready | Medium | Unexamined | `PE-28` | Lua AI is visible; native world-query semantics and manager integration are incomplete. |
| `WP-39` | Core selection, camera, minimap, build/command UI | Ready but not confirmed | High | Unexamined | `PE-25` | Recoil Metal has a usable native HUD; this is functional readiness, not pixel or retail UI parity. |
| `WP-40` | Advanced controls, overlays, split views, key behavior | Not ready | High | Unexamined | `PE-05`, `PE-14` | Manual documents controls; exact command encoding and UI/sim split need tracing. |
| `WP-41` | Rendering, model graph, manipulators, effects, LOD | Not ready | Medium | Unexamined | `PE-29` | Formats render; native manipulator/effect lifecycle and broad visual coverage are incomplete. |
| `WP-42` | Audio cues, voice priority, dynamic music | Not ready | Medium | Unexamined | `PE-30` | XWB PCM and basic playback are understood; retail event/music logic is not. |
| `WP-43` | Command replay, state hash equivalence, divergence | Ready but not confirmed | Medium | Unexamined | `PE-01`, `PE-26` | Recoil Metal has its own deterministic logs; retail command payload/RNG/replay ordering are unknown. |
| `WP-44` | Save/resume and full simulation serialization | Not ready | Low | Unexamined | `PE-26` | No Recoil Metal save state; retail format and script/native restoration boundaries are unknown. |

## Analysis order

The campaign starts with the least-known behavior, but executable mapping prerequisites come first.
Do not skip Phase 0 to chase an interesting feature: every later session becomes cheaper when the
registration and object maps are stable.

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
| Ghidra | Not installed or not on `PATH` as of 2026-08-29 | Pending | `build/re-fa/project` proposed | Pin the version for reproducible decompiler output. |
| `objdump` | Apple LLVM 17.0.0 | `/usr/bin/objdump` | Scratch only | Used for PE headers, timestamps, imports, and disassembly checks; not the primary database. |
| `strings` | Apple system tool | `/usr/bin/strings` | Scratch only | Preserve offsets and encoding when exporting strings. |
| ExifTool | 13.10 | `/opt/homebrew/bin/exiftool` | Scratch only | Used to read PE version resources and timestamps. |
| `shasum` | 6.02 | `/usr/bin/shasum` | Artifact verification | Used with SHA-256 for source and preserved-copy identity. |

If another tool is added, record its version and role. Do not let two unlabeled analysis databases
become competing sources of truth.

## Address and symbol ledger

This is the compact map that lets a new session resume. One address may have several hypotheses,
but only one canonical analyst name. Addresses are always relative to the exact artifact ID.

Naming convention:

- `fa_<subsystem>_<verb>` for high-confidence behavior, for example `fa_economy_allocate`.
- `maybe_<subsystem>_<verb>` while evidence is incomplete.
- Keep the original Ghidra name in the `Original` column forever.

| Artifact | Address / RVA | Original | Canonical name | Signature hypothesis | Confidence | Evidence citations | Callers/callees | Last reviewed |
|---|---|---|---|---|---|---|---|---|
| | | | | | | | | |

## Claim ledger

Every conclusion that may affect implementation gets a stable ID. Do not rewrite history when a
claim changes: mark it superseded and add the replacement.

| Claim ID | Work package | Claim | Evidence | Confidence | Status | Implementation impact |
|---|---|---|---|---|---|---|
| | | | | | Open / Confirmed / Refuted / Superseded | |

## Hypothesis ledger

Use this for decisions not yet settled by the executable.

| Hypothesis ID | Envelope | Candidate | Supporting evidence | Counterevidence | Discriminating observation | State |
|---|---|---|---|---|---|---|
| | | | | | | Open / Favored / Refuted / Confirmed |

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
