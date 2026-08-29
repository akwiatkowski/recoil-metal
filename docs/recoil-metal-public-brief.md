# Recoil Metal public briefing and artifact handoff

This document lets a fresh LLM session generate a public-facing artifact without the retail disk,
the previous chat, or undocumented project knowledge. It is separate from the long-running
[executable analysis plan](fa-exe-analysis-plan.md) so communication work does not consume analysis
context.

## Project brief

Recoil Metal is an experiment: can a clean-room, open-source engine recreate enough of *Supreme
Commander: Forged Alliance* to provide a modern foundation for the game when the original engine
source is unavailable?

It is a new engine, not a patch. It already runs a playable skirmish vertical slice with retail maps
and content on a native Metal renderer: mass/energy economy, construction, command queues, weapons,
ordinary shields, simplified aircraft and surface naval movement, fog/radar/sonar/counter-intel,
reclaim, adjacency, overcharge, sound, a playable HUD, victory, command replay, and per-tick state
hashes. This is not a feature-parity claim.

The next major fidelity phase will analyze Olek's owned retail executable, DLLs, Lua, blueprints,
and archives before implementing more systems. Findings will become concise behavioral evidence,
failing tests, and independent clean-room code. Decompiled source or proprietary assets will not be
copied into or distributed with the project.

## Current status snapshot

| Readiness | Count |
|---|---:|
| **Not ready** | 26 |
| **Ready but not confirmed** | 19 |
| **Confirmed with EXE analysis** | 0 |

These are the 45 work packages in `fa-exe-analysis-plan.md`. Readiness measures the current Recoil
Metal implementation; EXE knowledge is tracked separately. The retail disk is disconnected, so the
EXE campaign has not started.

## Community motivation

The FAF community has spent years working around a closed engine beneath an open Lua ecosystem. The
April Fools thread
["The day has come, we have the source code"](https://forum.faforever.com/topic/8990/april-fools-the-day-has-come-we-have-the-source-code)
made the wish explicit. It claimed that, after nearly a decade of trying, the original source had
been obtained, cleaned up, ported to current Windows and Linux, and published. It then described the
engine bugs and new capabilities that source access would make possible. The announcement was a
joke. Its premise was compelling because the constraint is real.

Recoil Metal could be an epilogue to that joke. It does not recover the source code the post
pretended had been found. It tests whether the hoped-for outcome can be reached another way: build a
new engine from owned retail artifacts, shipped Lua and data, public documentation, bounded
executable analysis, and independent implementation.

Treat the forum thread as community motivation, not technical evidence. The current research tool
received HTTP 403 from the site on 2026-08-29; the summary above comes from the post text supplied by
Olek, not an independent fetch. The project is not affiliated with or endorsed by FAF, Gas Powered
Games, Square Enix, or THQ Nordic.

## Facts safe to state

- The project is a new engine, not a modification or binary patch of the retail executable.
- The renderer is native Metal on macOS. The simulation is original C++ and uses fixed-point match
  state where determinism matters.
- The same binary reads Supreme Commander map/model/content formats and Recoil/BAR content formats.
- A vertical-slice skirmish progresses from commanders and economy through construction, combat,
  commander elimination, and a victory banner.
- Human and AI commands use one authoritative path and can be recorded and replayed.
- At commit `60b1525` on 2026-08-29, the repository had 1,129 registered tests. The full run passed,
  with 26 expected retail-content-dependent skips while the disk was disconnected.
- FAF's Lua builder data and conditions run through a partial adapter. The full manager stack and
  full native API semantics are not implemented.
- The planned executable campaign is static-analysis-first, clean-room, artifact-hashed, and
  hypothesis-driven.
- No subsystem is currently labelled Confirmed with EXE analysis because that campaign has not
  begun.

## Claims the artifact must not make

- Do not call the engine a complete remake, emulator, drop-in replacement, or feature-complete port.
- Do not claim binary, replay-file, network-protocol, or multiplayer compatibility with retail/FAF.
- Do not imply simplified aircraft, naval, shield, AI, or UI behavior matches retail.
- Do not say retail source code was recovered. Decompilation provides behavioral evidence, not
  maintainable original source.
- Do not imply executable analysis has started or that Ghidra findings already exist.
- Do not present FAF behavior as retail behavior unless the distinction is explicit.
- Do not include or offer proprietary executable code, decompiler listings, game assets, music, or
  archive contents.
- Do not describe the project as affiliated with or endorsed by the original developers,
  publishers, or FAF.

## Story the artifact should tell

1. **The community problem:** the game has an open Lua ecosystem on top of a closed, old engine.
2. **The experiment:** test whether a new clean-room engine is possible without original source.
3. **What exists:** a tested, playable, deterministic native engine and renderer, not a paper plan.
4. **What remains:** formations, transports, missiles/interception, enhancements, veterancy,
   submerged warfare, factory controls, full unit scripts, and full AI behavior.
5. **Why analyze the EXE:** Lua and blueprints describe intent but not native movement, targeting,
   collision, rounding, update order, and state ownership.
6. **How analysis stays efficient:** bound each question from Lua/data/docs first, enumerate the
   remaining mechanisms, then use one decisive native call path to distinguish them.
7. **How results become code:** finding, failing behavioral test, independent implementation.
8. **Why the work is resumable:** artifact hashes, stable work-package IDs, symbol/claim/hypothesis
   ledgers, and one exact next action at every session boundary.

## Suggested visual pipeline

```text
Community need: a maintainable engine foundation
                        |
                        v
Playable clean-room vertical slice
    original C++ simulation + native Metal renderer
                        |
                        v
Owned retail artifacts
    EXE + DLLs + SCD archives + Lua + blueprints
                        |
                        v
Bounded clean-room analysis
    known evidence -> candidate mechanisms -> decisive EXE trace
                        |
                        v
Durable knowledge
    symbols + claims + counterevidence + session handoff
                        |
                        v
Independent implementation
    failing behavioral test -> C++ behavior -> deterministic verification
```

## Minimum source bundle for another LLM

| File | Why provide it |
|---|---|
| `docs/recoil-metal-public-brief.md` | Project story, safe claims, source prompt, and review checklist. |
| `docs/fa-exe-analysis-plan.md` | Canonical readiness dashboard and planned research method. |
| `README.md` | Public overview, commands, screenshots, and concise gameplay gaps. |
| `docs/milestones.md` | Detailed evidence of implemented and tested behavior. |
| `PLAN.md` | Long-term direction and explicit non-goals. |
| `ADR_DECISIONS.md` | Design rationale for architecture-focused artifacts. |

This document is sufficient for a short overview. The other files improve detail. Historical prose
must not override the current dashboard in `fa-exe-analysis-plan.md`.

## Ready-to-paste prompt for another session

```markdown
Create a standalone public-facing artifact explaining Recoil Metal and its next major phase. The
audience is technically curious game-engine developers, Supreme Commander/FAF community members,
and potential collaborators who have not seen the repository.

Use `docs/recoil-metal-public-brief.md` as the primary source and
`docs/fa-exe-analysis-plan.md` as the canonical status source. You may use `README.md`,
`docs/milestones.md`, `PLAN.md`, and `ADR_DECISIONS.md` for supporting detail. Do not invent facts.

Explain:

1. The community motivation: the April Fools post pretended the long-wanted source had finally been
   recovered. Present Recoil Metal as a possible epilogue: not recovered source, but an attempt to
   reach the hoped-for result through a new clean-room engine. Use the thread as context, not
   technical evidence.
2. The experiment: determine whether a new clean-room, open-source engine can recreate enough
   Forged Alliance behavior without original source.
3. What already works, with concrete examples and current test evidence.
4. What remains incomplete and why this is not a feature-parity claim.
5. The disk-backed workflow: hash owned artifacts, map Lua/Moho registrations, bound questions with
   Lua/blueprints/docs, distinguish native mechanisms in Ghidra, preserve evidence across sessions,
   then derive tests and independent code.
6. The three labels exactly: Not ready; Ready but not confirmed; Confirmed with EXE analysis. State
   that no package is currently EXE-confirmed.
7. Why this is a long, multi-session research effort and how its ledgers make it resumable.
8. Clean-room and copyright boundaries. No proprietary assets, executable code, or decompiler
   listings are redistributed.

Include one simple pipeline diagram and a compact status summary. Make current achievements,
planned analysis, and long-term ambition visually distinct. Avoid hype, invented timelines, and
claims of retail/FAF compatibility. If producing slides or a web artifact, keep important facts in
selectable text and support desktop and mobile. Otherwise produce a polished one-page Markdown
brief.
```

## Artifact review checklist

- Current and planned behavior are visually distinct.
- The three readiness labels and counts match `fa-exe-analysis-plan.md`.
- The test count matches current repository evidence.
- The retail disk is described as disconnected.
- The forum thread is motivation, not evidence or endorsement.
- The artifact says clean-room reimplementation and includes the non-affiliation boundary.
- It does not reproduce proprietary images or imply decompiled code will be copied.
- Every technical claim traces to a supplied file.
