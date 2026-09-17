# Minimum retail Capture implementation

Status: minimum slice implemented 2026-09-09 (`core/sim/Capture.hpp`, `CommandKind::Capture`,
`CaptureWork` with save v20). One engineer captures one completed, unattached ordinary
enemy land unit or structure with funded progress and replacement-entity transfer, under
the progress contract below. Approach and admission were read from the executable
2026-09-17 (`C-250`, "Approach and admission" below). General transfer parity
(attachments, enhancements, fuel, ammo, shields) remains open, as do concurrent-captor
races beyond one bar each.

## Source identity and corrections

The executable is preserved `retail-fa/bin/SupremeCommander.exe`, `ART-E001`,
SHA-256 `c6783580c0b7a408ec2ad3bfe5eb1fdbef31a60d92c1007ff9b90c33bb960aa0`.
Addresses below are absolute PE virtual addresses. This investigation used
`tools/re/disasm_annotated.py`, without running retail or opening a window.

The Lua below is **ART-S007, `gamedata/lua.scd`**, not ART-S010
(`mohodata.scd`). Both extracted files in `build/re-fa/lua/` were compared
byte-for-byte with ART-S007 and match. ART-S010 contains different, much shorter
files. Existing C-236–C-240 citations to these long Lua files need that correction.
The FAR `11-fa-sim-layer.md` report is useful orientation, but its FAF capture-cost
helper is not the authority for this retail slice.

| Existing claim | Correction established by this read |
|---|---|
| C-236: target's `GetCaptureCosts` | The **captor's** method receives the target. `Unit.lua:2734-2744` reads `self:GetBuildRate()` and the argument's blueprint. EXE `0x0060B437–0x0060B477` packages the target while retaining the captor receiver. |
| C-239: summing multiple captors' costs/work | `0x0060B4FF–0x0060B666` iterates the **target's attached entities**, filters live units, and calls the same captor's method for each child. The getter is Entity vtable `+0x58` → `0x005C46E0`, returning `Entity+0x17C`; independently identified in C-195. This is capture cost for attached units, not summed captor BuildRates. |
| C-243: no writer for `Unit+0x690` | The writer is already inside capture activation: `0x0060B93B` increments it; deactivation at `0x0060B9EA` calls `0x0060A9E0`, which decrements only above zero. The field belongs to the **target**, and counts active capture participation. |
| C-240: no native gameplay-state preservation evidenced | The native path copies health and current layer, as well as custom name and two shared handles from the manipulator manager. Lua subsequently restores selected state too. Native preservation cannot be dismissed from Lua restoration alone. |

These are corrections to the evidence, not a claim that WP-17 is implemented or
that every capture branch has passed independent parity review.

## Approach and admission

Read 2026-09-17 from `CUnitCaptureTask::TaskTick` `0x0060AEB0`–`0x0060B8CB`
(`.\sim\AiUnitCapture.cpp`) and its ctor/factory/dispatcher neighbourhood; full
evidence is claim `C-250` in `fa-exe-analysis-plan.md`.

**The task is a five-state machine** on `task+0x24`, dispatched through the jump
table at `0x0060B8CC`: state 0 approach (`0x0060B0D6`), state 1 admission
(`0x0060B271`), state 2 cost setup (`0x0060B3BC`), state 3 funded progress
(`0x0060B759`), state 4 completion (`0x0060B822`). States advance with `ret 0`
(same-beat re-run under the `C-232` contract); `ret -1` ends the task.

**All legality is a per-tick preamble; there is no issue-time validation.** The
command-issue path (`0x0060F9C0` case `0x00610379` → factory `0x0060AE30`)
performs no legality check beyond a dead weak target (`cmd+0x2C = 2`). Every tick
re-checks, in order:

| Gate | Check | Exit |
|---|---|---|
| Target alive | weak cell `task+0x44` resolves | `OnStopCapture` on captor, `*cmd+0x2C = 1`, `ret -1` |
| Capturable | `vt[0x40]` object `+0x69` byte (written by `Unit::SetCapturable` `0x006D00B0`) | same exit |
| Has army | target `Entity+0x14C` non-null | `*cmd+0x2C = 2`, `ret -1` |
| Not airborne | target `Entity+0x118` layer != `0x10` (`LAYER_Air`) | status 2 |
| Not friendly | `0x0057FF00(targetArmy+8, captorArmy+8) != 1` (self/ally) | status 2 |
| Concurrency | `IsUnitState(27)` `BeingCaptured` AND edge distance > 10.0 | status 2 |

The distance is **footprint-edge distance**, 2D:
`sqrt(dx² + dz²) − max(captor FootprintSizeX,Z) − max(target FootprintSizeX,Z)`.
Captor position via `vt[0x14]`, target position at `Entity+0xAC/+0xB4`, both
footprints as u8 pairs via `0x0067F310` (`Entity::GetFootprint`).

**State 0 — approach.** If `edgeDist <= 5.0` (`0x00E4D960`), the task goes
straight to state 1 in the same tick — no move is issued. Otherwise it snapshots
the target position, builds a 16-byte approach block via `0x006AE090` on the
target's `vt[0x10]` Unit, resolves a pathable goal through `0x00632250` (args:
captor, target pos, block, `captor+0x154->vt[0xB8]` service, flag 1), and
enqueues a move task once via `0x0061FB70`. Captor byte `Unit+0x68B` set skips
the goal recompute; its writer is outside this function and unnamed. The helper
semantics (approach-block geometry vs pathable-goal resolution) are inferred
from use — recorded, not named.

**State 1 — admission.** `edgeDist > 10.0` (`0x00E4E150`) → `*cmd+0x2C = 2`,
`ret -1`: the task ends while the enqueued move task keeps walking. The retry is
**dispatcher-level**: `IAiCommandDispatchImpl::TaskTick` reads `+0x2C == 2` at
`0x0059F98C` and re-issues the command through `0x006F4A40`. There is no in-task
wait state. In range, a dead (`+0x99`) or dying (`+0x1B9`) target ends the task
quietly; otherwise `captor+0x554` (navigator/rally object) receives `vt[0x3C]`
goal = target `vt[0x48](-1)` position, the captor gains `UNITSTATE_Capturing`
(bit 26, `orl $0x4000000, Unit+0x4A0`), and the task advances to state 2.

**The status cell.** `task+0x28` = `&cmd+0x2C`, zeroed at task spawn
(`0x005F7423`–`0x005F742F`). Semantics: `0` = running or success (state-4
completion leaves it 0), `1` = target invalidated (fires `OnStopCapture`), `2` =
legality/range abort (dispatcher re-issues). Whether status 1 also re-issues is
unread — the observed read checks `== 2` only.

**Concurrency is a refcount, not mutual exclusion.** `SetActive` `0x0060B8E0`
(gated on `task+0x4C`, called from state 2 after cost setup, the command-event
listener `0x0060BA90`, and teardown `0x0060BCC1`) sets target
`UNITSTATE_BeingCaptured` (bit 27) and increments `Unit+0x690` on activation;
deactivation guarded-decrements and clears the bit only when the count reaches
0. Multiple near captors are therefore permitted — each funded beat adds the
live count to each task's progress (`C-243`). The only exclusion is the
preamble's `BeingCaptured` + >10-elmo early exit for captors still far away.

**Deactivation reports failure, not stop.** On a live target the deactivate arm
fires `OnFailedBeingCaptured(target,captor)` /
`OnFailedCapture(captor,target)`; on a dead/dying target (`+0x99`/`+0x1B9`) the
whole arm is a no-op — which is why a completed transfer's target destruction
does not produce failure callbacks. Task teardown additionally clears captor
`Capturing`, zeroes `Unit+0x2AC` (shared work-progress display field), resets
the `+0x554` goal, and calls `0x006B21F0`.

**Corner case recorded verbatim:** the preamble's dead-target exit fires
`OnStopCapture` on the captor passing the **captor** cell as arg
(`0x0060B896`–`0x0060B8AD`), unlike state 4 which passes the target cell — Lua
receives the captor itself on that path.

## Progress contract

For a single captor and a target with no attached units:

```text
seconds = ((target.BuildTime or 10) / captor.GetBuildRate()) / 2
          * (captor.CaptureTimeMultiplier or 1)
workTicks = truncate(float32(max(1, seconds * 10)))
energyCost = target.BuildCostEnergy or 100
massCost = 0
energyPerBeat = max(0, energyCost) / workTicks
massPerBeat = 0
```

The Lua formula is `Unit.lua:2734-2744`; EXE `0x0060B4A8–0x0060B4DC`
multiplies by 10, clamps to at least one, and performs a truncating integer
conversion. This is a 10 Hz simulation work budget, not a construction-rate
healing fraction. Do not substitute ceiling or invent a fallback BuildRate.
Lua's `or` applies to absent/false values, not numeric zero.

The cost setup requires exactly three Lua returns. Attached live units add their
individual costs and tick budgets at `0x0060B521–0x0060B666`; each addition
passes through float32 and truncation. Demands are divided by the total work
budget at `0x0060B66C–0x0060B6AA`.

`task+0x44` is the weak target used by activation and by the progress reader.
The activation boolean at `task+0x4C` makes repeat activation/deactivation a
no-op (`0x0060B8EA–0x0060B8FD`). Entering increments target `+0x690` before
`OnStartBeingCaptured`; leaving decrements it on the live-target branch.
When it reaches zero, the capture-state clearing branch becomes eligible
(`0x0060BA03–0x0060BA59`). It is an integer participation counter, not an
engineer's BuildRate or a float speed multiplier. Its serialization as a
32-bit integer is independently visible in Unit's writer `0x006B9E20`,
`build/re-fa/exports/serializer_members.tsv`, offset `0x0690`.

Only a beat with both allocations meeting the demands advances progress
(`0x0060B759–0x0060B7FB`). The task consumes funded demand and computes:

```text
task.progress = min(task.progress + target.activeCaptureCount, task.workTicks)
```

The add-and-cap is `0x0060B7BC–0x0060B7E2`. A lone active captor therefore
adds one per funded beat. Partial allocation does not proportionally advance
capture. Count changes are observed live, rather than cached at cost setup.
This does **not** establish a complete mixed-captor wall-clock formula: each
task has its own costs, progress and funding, and scheduling/ownership races
remain outside this slice. In particular, never sum the captors' times as C-239
previously suggested.

## Completion and transfer inventory

Start order is target `OnStartBeingCaptured`, then captor `OnStartCapture`
(`0x0060B942–0x0060B968`). Completion order is captor `OnStopCapture`, target
`OnStopBeingCaptured`, then target `OnCaptured` (`0x0060B822–0x0060B870`).
The Lua callback bodies are `Unit.lua:471-518,555-619`.

`OnCaptured` checks both units are alive and have different brains. A target
that has become noncapturable is killed; transport cargo has an additional
noncapturable-unit kill pass. It snapshots new-unit callbacks, calls
`TransferUnitsOwnership`, then invokes the saved callbacks with the replacement.
Campaign army-cap overrides and arbitrary script callbacks are separate scope.

| State / operation | Evidence and implementation consequence |
|---|---|
| Entity identity | `Sim::TransferUnit` `0x0074E0BF` creates a replacement, returns it in `eax`; `0x0074E409–0x0074E413` destroys the old entity. An in-place army-ID rewrite is not this contract. |
| Blueprint and transform | Old Unit vtable `+0x1C` returns blueprint (`0x006AF5B0`, Unit `+0x74`); `+0x18` returns transform (`0x006AB450`, Unit `+0xA4`). `0x0074E022–0x0074E0BF` copies the seven-float transform into creation arguments. |
| Current layer | Old Unit `+0x120` is copied into the creation descriptor at `0x0074E02E,0x0074E0A9`. `Unit:GetCurrentLayer` independently names this field in `field_offsets.layout.tsv`. |
| Health | `0x0074E184–0x0074E1AC` compares old/new Unit `+0x98` and sets old health on replacement via `0x006803D0` with the Entity subobject at `Unit+8`. `Unit:GetHealth` reads the same field at `0x006CB839`. Lua later restores health again, after veterancy/enhancements, capped to the new maximum. |
| Custom name | `0x0074E1B1–0x0074E1CE` copies through Unit virtual `+0x6C` → `+0x68`. Unit vtable `0x00E7A024` resolves these to `0x006AB540`/`0x006AB4E0`, getter/setter of the string at `Unit+0x2CC`. `Unit:SetCustomName` independently calls `+0x68` at `0x006D46E8–0x006D46EF`. |
| Manipulator-related shared state | `0x0074E0DC–0x0074E109` reads two shared-pointer pairs from old `Unit+0x540` via `0x005C4840`/`0x005C4820`. `0x006B2620–0x006B26AE` installs them at replacement `+0x30C/+0x314` and in its manager via `0x006414F0`. Pointer preservation is proved; the exact semantic contents are still unnamed. Do not claim all animation state is copied or reset. |
| Kills, enhancements, fuel, silo ammo, shield | `SimUtils.lua:68-132` snapshots these, calls `ChangeUnitArmy`, then restores kills first, enhancements, clamped health, fuel, ammo deltas, shield health and on/off state. These are explicit Lua responsibilities, not evidence of a whole-unit clone. |
| Attachments | Native recursive transfer/re-attachment occupies `0x0074DD15–0x0074E022` and `0x0074E1D0–0x0074E3DD`; `OnTransportAttach` is conditional. Exclude attached parents/children from the initial slice. |
| Failure | Replacement-creation failure branches to `0x0074E421`; the target brain's `OnFailedUnitTransfer` is conditional there. Success destroys the old entity; that destruction block is skipped on creation failure. Earlier transport recursion means general failure is not proved atomic. |
| Other reset/copy behavior | The old unit's byte `+0x560` is conditionally cleared at `0x0074E402` before destruction; its purpose remains unnamed. No source read here proves command queues, cooldowns, arbitrary Lua fields, shield recharge timers or all modifiers survive. |

The native health copy disproves the earlier broad negative in C-240. The two
unnamed shared handles and conditional destruction flag also prevent calling
the entire native copy/reset inventory closed.

## Implementation boundary

Build one semantic, queueable Capture command using the existing entity-command
dispatch and replay path. Use a normal engineer with authored capture capability,
one completed hostile land target, no attached children/parent, no enhancements,
fuel, shield or silo ammunition, and no concurrent captor. A T1 engineer and a
T1 economy structure provide a useful first acceptance pair. Reject unsupported
targets explicitly during this first slice; do not silently approximate them.

Reuse movement-to-work and army funding infrastructure, but give Capture its own
integer budget and funded-only advancement. Do not call Repair's fractional
healing or use its range constants without capture-specific evidence. Add a
small capture task record only where existing command state cannot represent
progress, budget, target and activation. Persist/hash the state actually used;
save/load and replay must preserve activation without incrementing twice.

On success, snapshot supported restoration state, create the same blueprint for
the new owner at the old transform/layer, restore supported health/name state,
then retire the old entity and capture order. Reuse normal spawn/destruction
bookkeeping for army counts, economy, collision, visibility and stale handles;
do not run combat death, grant kills, or spawn a wreck for a transfer. The
replacement's initial order policy must be documented as a slice decision until
native queue retention is established.

Approach and admission are now read (section above, claim `C-250`): the capture
task approaches within the edge-distance gates and admits once inside 10 elmos,
ending with status 2 — not waiting — when the target stays out of reach; the
command dispatcher's re-issue is retail's retry. Implement the gate and the
legality preamble, not a repair-style range hysteresis. Remaining open reads
before full parity claims: `OnCaptured`'s Lua body and `Sim::TransferUnit`'s
unnamed details, plus the approach helpers (`0x006AE090`, `0x00632250`,
`0x00511B10`, `Unit+0x68B`/`+0x554`) whose semantics are inferred rather than
named. General gifting, mixed captors, transport cargo and rich-unit transfers
remain later work. They must not block specifying or testing the single-captor
formula.

## Headless acceptance contract

Write these behavior checks before the implementation; use real retail engineer
and target blueprints for the integration case. No headed test is required for
the simulation contract.

1. Fully funded single capture takes exactly `workTicks` **funded progress beats**;
   setup/approach/completion beats are counted separately. Cover a fractional
   time and the one-tick minimum to distinguish truncation from ceiling.
2. Insufficient energy causes no progress; funding resumes without lost progress
   or double charge. Competing construction shares the existing army allocator.
3. Activation once, repeated activation, cancellation and target death leave the
   target participation count consistent; repeated deactivation never underflows.
4. Completion creates a new entity owned by the captor's army, preserves supported
   transform/layer/health/name, invalidates old generation-safe handles, updates
   both armies, creates no wreck and awards no combat kill.
5. Record start/stop/completion ordering at the engine event boundary. This proves
   the native event contract, not arbitrary retail Lua callback execution.
6. Reject attached, unfinished and other unsupported targets at admission. Cancel
   safely if eligibility changes while working. Verify queued next orders run.
7. Replay and a mid-capture save/load yield identical remaining progress,
   replacement identity sequence, funding and state hashes.
8. Inject replacement-creation failure: leave the ordinary unattached old target
   intact, clean capture funding/activation, and report failure explicitly.

An extended transfer suite must separately cover kills/veterancy, enhancements,
fuel, shield and ammo restoration in Lua order, and transport partial failure.
A multi-captor suite must distinguish independent per-task budgets from the
shared target counter, including mixed BuildRates and partial funding.

Verification performed for this specification: executable checksum, exact Lua
archive-member comparisons, disassembly of the cited paths, vtable resolution,
and cross-checks against the existing field/serializer exports. No implementation,
retail observation, simulation test run or visual acceptance is claimed here.
