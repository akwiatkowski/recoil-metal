<!-- Generated and maintained by Claude -->
# Forged Alliance parity — the orchestration prompt

A self-contained brief for a fresh session, in any harness. Paste it whole. It
assumes no memory of previous conversations and names every file it depends on.

---

## Mission

You are working in `~/projects/llm/games/recoil-metal`, a Mac-native C++23 RTS
engine that reads Supreme Commander: Forged Alliance content off disk and plays
a match out of it. Twenty milestones are done; a headless skirmish runs from
spawn to a victory banner, deterministically.

**Your job: raise its behavioural parity with retail Forged Alliance toward
1:1, across the 20 `FA-*` gameplay subsystems.** The bar is the retail
executable, not a game that feels similar.

## Read before you touch anything

| File | What it gives you |
|---|---|
| `AGENT.md` | Hard rules, settled decisions, C++ style, fleet protocol, gotchas. Non-negotiable. |
| `docs/fa-gameplay-progress.md` | **The dashboard.** 20 subsystems, three score axes, 45 work packages, the exact next task for each. This is the scheduler and the status file. |
| `docs/fa-exe-analysis-plan.md` | The evidence ledger: claims `C-nnn`, hypotheses, possibility envelopes `PE-01..PE-30`, address/symbol ledger, evidence vocabulary, the confirmation gate, session protocol. |
| `docs/observations.md` | The owner's notes from playing retail. Read this first each session. |
| `ADR_DECISIONS.md` | 69 decisions with their rejected alternatives. Do not re-litigate them. |
| `PLAN.md`, `PLAN2.md` | Milestone narrative and the numeric design (§5.2 fixed point). |
| `reference/supcom-wiki/INDEX.md` | Local mirror of the Supreme Commander wiki — unit roles, weapon behaviour, controls, strategy. Gitignored, `WEB` tier. |
| `../faf/forged-alliance-reborn/docs/engine-analysis/01..16` | A prior study of the Recoil engine and the SupCom corpus, ~20k lines, every claim cited. **Read the analysis before the source.** |

Both `../faf/forged-alliance-reborn/` and the Recoil reference tree it owns are
**read-only**. Never edit anything under them.

## What 1:1 means here

Retail is `binary32` float end to end — health is a `float` at `Entity+0x98`
(`C-038`), economy fields are floats (`C-068`), the storage cap is rounded twice
(`C-160`), and construction completes on an exact `== 1.0f` compare (`C-187`).
Recoil Metal is fixed point by design: `Fx` is Q18.14 in `int32`, `Mag` is Q50.14
in `int64`, angles are `Brad` (turn/65536, wrapping by unsigned overflow). See
`src/core/sim/Fx.hpp`, which is the one file in the sim allowed to name `float`.

**So numeric equality is structurally impossible, not merely hard.** The gate is
*structural equivalence*, all five of:

1. **Same algorithm** — the same decision procedure, not a different one tuned to agree.
2. **Same ordering** — iteration order, tie-breaks, which pass sees which state.
3. **Same quantisation points** — where retail rounds or truncates, we do.
4. **Same observable staleness** — a value retail reads a beat late is read a beat late.
5. **Divergences enumerated** as claims, never left implicit.

Bit-identical results, matching replay digests and reproducing retail's float
rounding are explicitly **not** required. Do not chase them; do not widen a
tolerance to fake them. But note the sharp edge: some float artefacts are
*behaviour*. That `== 1.0f` means "did this beat land exactly on full", and the
fixed-point equivalent must stay exact rather than gain an epsilon.

## Never plausible behaviour

The parity analogue of "never programmer art". Programmer art renders fine and
nobody chose the look; plausible behaviour runs fine and nobody read the retail
path. Every gameplay constant, threshold, tie-break and update-order decision
carries an exact locator:

| Tier | Locator required |
|---|---|
| `EXE` | Artifact id + RVA + function name |
| `LUA-R` | Archive id, VFS path, line/table key |
| `BP-R` | Archive id, VFS path, exact field path |
| `OBS` | Experiment id, environment, setup, input, observed output |
| `FAF` | Commit, path, and whether behaviour is inherited or changed |
| `WEB` | URL, title, access date — includes the wiki mirror |

Specifically forbidden: a constant picked so a test goes green; "FAF does X so
retail does X"; widening a tolerance until it passes; choosing the mechanism
that is cleaner rather than the one that is there. Never promote `FAF` or `WEB`
to retail fact without corpus or EXE support. One plausible decompile is not
confirmation — check callers, callees, contradictory evidence.

If the retail mechanism is unknown, **say so and stop**. An honest "blocked on
evidence" is worth more than a guess with a test wrapped around it.

*The cautionary tale:* `C-187`. The clean design is "sum the assisters'
contributions" — and it is wrong. Retail has no assister enumeration; each
builder owns a helper and calls `Materialize` independently, so a late assister
on the completing beat adds full health and zero progress. Taste never finds
that.

## Two modes, chosen per subsystem

- **Fill mode** — subsystems under ~40% implemented. Many agents, breadth first,
  integration smoke only. The goal is that the behaviour *exists and can be
  played*. Constants still need locators, but a documented gap beats a blocked
  round.
- **Parity mode** — subsystems above it. Full contract, critic, confirmation gate.

The dashboard says which is which, and when it disagrees with anything in this
document, **the dashboard wins**.

## What a merge is

**A finished feature: a difference visible in a match.** Aircraft that fly, a
shield that blocks, a silo that fires. A parser with no caller, a refactor with
no behavioural change, or slice 2 of 4 is not a merge — it waits and lands with
the visible thing it serves.

Run the suite **per merge, not per edit**. Measured on an M4 Pro, 2026-09-03:

```
mise exec -- cmake --build build -j8     #  9 s incremental
mise exec -- ctest --test-dir build -j8  #  6 s, 1305 tests
mise exec -- make verify                 # 19 s, the whole 700 s golden match
```

About 35 seconds for the lot. TDD still applies per change; it is the *suite*
that batches, not the failing test that starts the work. If something is red
when you begin, record it and say so — do not silently fix or silently ignore.

## The human oracle

The owner plays retail Forged Alliance and writes what he sees into
`docs/observations.md`. Those notes have two distinct powers:

- **On priorities, absolute.** A note re-orders the queue. No gate, no argument,
  no evidence required. It is a preference, and preferences need no proof.
- **On evidence, `OBS` — authoritative about the symptom, silent about the
  mechanism.** Nobody outranks direct observation on what happened on screen.
  But "units stop before the flag" is a symptom; whether that is an arrival
  radius, a deceleration curve or a path-cell snap is a question for the
  disassembly. An observation contradicting an `EXE` claim is a *finding* —
  usually the wrong function was read, or the session was FAF rather than retail
  1.6.6 — never a licence to overwrite the claim.

Every note needs its environment line: **retail or FAF**, map, units.

## The fleet

If your harness supports subagents, fan out. Two classes, different rules:

- **Analysis agents — read-only, parallelise freely.** Ghidra, `objdump`,
  `tools/re/*`, the retail Lua and blueprint corpus, the wiki mirror, FAF source.
  They edit nothing under `src/`. Output is claims, counterevidence and narrowed
  envelopes in `docs/fa-exe-analysis-plan.md`. One agent per open `PE-nn` or WP.
- **Implementer agents — serialised on shared sim state.** They own a work
  package, not a folder. `src/core/sim/` is one fixed-point machine with one tick
  order, so at most one agent per wave touches `UnitStore`, `Command`,
  `Movement`, `SaveState` or `StateHash`.
- **One integrator.** The only role that changes tick order, `SaveState` version
  or `StateHash` coverage. It merges, re-runs the suite and `make verify` against
  `main`'s golden log, and re-blesses that log only with an explanation.

Worktree setup — `git worktree` materialises tracked files only, and nothing
tracked here is game content, but three things a build needs are gitignored:

```sh
git worktree add ../rm-wt-<name> -b wt/<name>
ln -s "$PWD/third_party" ../rm-wt-<name>/third_party     # metal-cpp + miniz
mkdir -p ../rm-wt-<name>/vendor
ln -s "$PWD/vendor/ai" ../rm-wt-<name>/vendor/ai         # pinned AI corpora
cp local.mk ../rm-wt-<name>/ 2>/dev/null || true         # content paths
```

Each worktree needs its own `build/` (~560 MB; CMake bakes absolute paths).
Retail content lives outside the repo behind `$FA`, so it is inherited free.

Waves, in dependency order — but defer to the dashboard's critical path:

1. `FA-SIM`, `FA-CONTENT`, `FA-PERSIST` — kernel, lifecycle, VFS, save/replay.
2. `FA-CMD`, `FA-ECON`, `FA-DAMAGE`, `FA-INTEL`, `FA-WEAPONS`.
3. `FA-LAND`, `FA-AIR`, `FA-NAVY`, `FA-TRANSPORT`, `FA-MISSILES`, `FA-PROGRESS`, `FA-TERRAIN`.
4. `FA-MATCH`, `FA-AI`, `FA-UI`, `FA-PRESENT`.

## Verification — three instruments, and what each can prove

| Instrument | Question it answers | Ground truth |
|---|---|---|
| Focused test | Is this value, order or edge case what retail does? | An `EXE`/`LUA-R`/`BP-R` locator |
| `make verify` | Did this edit change simulation behaviour, and from which tick? | Our own previous run |
| `OBS` experiment | What does retail actually do when you run it? | The retail game, recorded |

`docs/golden-p1.log` is **7000 hashes of our own sim**, not of retail's —
`C-154` found retail's replay checksum is a bounded change-set ring, so the
digests are not comparable even in principle. It is a change detector with a
blast radius, never a parity oracle. An unexplained golden diff is a bug.

Useful commands: `make match SECONDS=<n>` (scrub one deterministic match),
`make battle-shots` (screenshots at fixed times), `make skirmish`, `make
ai-sanity`, and the boundary checks `tools/check_no_sim_floats.sh`,
`check_no_sim_globals.sh`, `check_no_tick_literals.sh`, `check_sim_boundary.sh`,
`check_one_order_path.sh`. Anything visual is verified with `--screenshot`,
which is offscreen and deterministic — never by capturing the focused window.

**No agent may claim a behaviour it has not run and looked at.**

## Critics — two, and they must stay separate

**The parity critic gates.** It writes no code, assumes the builder is wrong, and
checks:

- every new constant has an exact locator, and the locator *actually says* what
  the builder claims — spot-check it in the artifact;
- the test asserts a retail-derived value, not the implementation's own output,
  and a failing test existed first;
- suite green, `make verify` green, all five boundary scripts clean;
- `SaveState` versioned and round-tripped if state was added;
- the dashboard row moved for the right reason, and the three axes were not
  conflated — understanding absent behaviour does not make the game complete.

Pass is the confirmation gate in `fa-exe-analysis-plan.md` plus zero
regressions. Fail returns a ranked issue list; at most 3 rounds, then the work
package goes back to analysis with what was learned recorded.

**The feel critic advises and gates nothing.** `FA-FEEL` scores 0–10 on battle
legibility at each zoom, sense of scale and mass, spectacle, and headroom at
1000+ units, from `make battle-shots` on a fixed match. It exists so the project
can tell whether it is heading somewhere worth playing. It may **never** propose
a change to a simulation constant, its findings become tickets for `FA-UI`,
`FA-PRESENT` and performance only, and its score stays out of the parity
dashboard.

## Final gate per wave

A whole-game run to a victory banner, hash-log stable, no console errors, frame
budget held. Then a **blind trace test**: a judge is given two behaviour traces
of the same scenario — one from a retail observation, one from ours — labelled A
and B in shuffled order, and must say which is retail and why. If it can tell,
the "why" is the next issue list. Where no retail trace exists for that scenario,
say so rather than substituting a plausible one.

## Loop and persistence

After every round, write to `docs/fa-gameplay-progress.md` (scores, exact next
task, snapshot line) and to the claim/hypothesis/WP ledgers in
`docs/fa-exe-analysis-plan.md`. Each iteration resumes from the weakest subsystem
on the critical path, never from scratch. Cheap analysis runs continuously; full
match runs happen at wave boundaries.

## Hard rules

- **Never inflate.** Report real numbers, failed rounds, what is missing.
  Implemented / Retail-validated / Retail-analyzed are never averaged together.
- **Never invent a constant.** No locator, no merge.
- **Never edit shared sim core outside the integrator role**, and never edit
  another agent's work package.
- **Never edit the reference trees.**
- **`mise exec --` for every tool invocation.** Never call `cmake`/`ctest`
  directly.
- **Sim hygiene:** fixed point only, no floats outside `Fx.hpp`, no globals, no
  tick literals, one order path.
- **Never commit game assets**, `third_party/`, `vendor/ai/` or `reference/`.
- **Keep `main` buildable and the golden log green** — other agents run matches
  against it.
- **Do not ask questions.** Make routine decisions, state assumptions, keep going.

## Start here

Read `docs/observations.md`, then the dashboard headline's current critical path,
and take that subsystem's exact next task.
