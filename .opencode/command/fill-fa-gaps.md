---
description: Fill many Forged Alliance gameplay gaps through bounded parallel Terra work waves.
agent: build
model: openai/gpt-5.6-terra
---

Run a breadth-first Forged Alliance implementation campaign. The objective is to land as many
small, coherent gameplay foundations as the available time permits. Retail-perfect polishing,
full confirmation-gate work, and broad EXE archaeology are deferred unless a concrete
implementation decision is otherwise impossible.

User constraints for this run: `$ARGUMENTS`

If no constraints were supplied, work autonomously for up to one hour, running as many complete
waves as that budget permits with at most three parallel implementation workers per wave. Ask only
when an unexpected existing edit directly conflicts with a selected slice or no safe implementation
candidate remains. Stop early only when no safe implementation candidate remains.

## Campaign rules

1. Read `AGENT.md` and the subsystem table plus Starting Work section of
   `docs/fa-gameplay-progress.md`. Do not read `PLAN.md`, `README.md`, or the full evidence ledger
   up front.
2. Prefer high `Retail-analyzed` and low `Implemented` rows. Skip `FA-FOUND`, validation-only work,
   visual baseline capture, dashboard polish, and confirmation-gate closure by default.
3. Treat existing WP claims, shipped Lua, and blueprints as the current contract. Do not recheck
   the original executable merely to increase confidence. If one exact unknown blocks a slice,
   record it and choose another slice before starting EXE analysis.
4. Prefer foundations that unlock later work and can be implemented in existing files. Never
   invent balance values or retail behavior. Keep uncertain behavior explicit and out of scope.
5. The coordinator is the only writer to shared campaign documents. Subagents must not edit
   `docs/fa-gameplay-progress.md`, `docs/fa-exe-analysis-plan.md`, `PLAN.md`, `README.md`,
   `ADR_DECISIONS.md`, build files, or shared registries unless the coordinator serializes that
   edit after the parallel wave.
6. Do not commit unless `$ARGUMENTS` explicitly requests commits.

## Wave 1: scout in parallel

Rank the likely implementation candidates from the dashboard. Start with subsystems such as
`FA-TRANSPORT`, `FA-MISSILES`, `FA-AIR`, `FA-NAVY`, `FA-PERSIST`, and `FA-LAND`, but let current
dependencies and file overlap decide the actual order.

Launch up to six `fa-gap-scout` subagents in one parallel batch, one subsystem per scout. Give each
only its dashboard row, exact next task, source WP identifiers, and this campaign objective. Require
this compact result:

```text
ID and proposed slice
Why this is a high-leverage gap
Current contract: claim IDs, Lua/blueprint evidence, and only essential EXE facts already recorded
Files to read or edit
Shared-file collision risk
One focused regression or acceptance check
Hard stop boundary
Blockers and confidence
```

Scouts investigate and report; they do not edit. They must read only the relevant WP/claim sections
of `docs/fa-exe-analysis-plan.md`, not the whole ledger, and only then inspect named code paths.

## Wave 2: choose disjoint slices

Select at most three packets that:

- touch disjoint implementation and test files;
- do not depend on each other;
- create a usable vertical behavior or reusable foundation;
- have no unresolved semantic choice requiring fresh EXE work;
- are small enough for one focused worker.

Reserve each packet's file set. If packets need the same central file, select one and defer the
other. Launch the selected `fa-gap-worker` agents together in **test-only stage**. Pass each worker
only its packet, reserved paths, and relevant source WP/claim excerpts. Each worker adds the
smallest focused regression and returns without implementation. Do not ask workers to rediscover
the repo.

After all test-only workers return, the coordinator builds once and runs their focused tests
sequentially to record the expected failures. Reject tests that fail for setup or compilation
rather than the missing behavior. Then resume the same `fa-gap-worker` sessions together in
**implementation stage**, passing each its own red result. Workers implement only their packet and
do not run commands against the shared build tree. A worker must stop and report rather than edit
outside its reservation.

## Integration gate

After the parallel workers return:

1. Inspect their summaries and the combined diff. Reject or revert only edits made by this command;
   never disturb pre-existing user or agent work.
2. Serialize any necessary shared build-file or registry edits.
3. Build once, then run the workers' focused tests sequentially. Use `mise exec --` for every tool.
4. Resume the responsible `fa-gap-worker` for concrete build/test failures. Give it the exact
   failure and keep its original path reservation; do not let it broaden scope. Rebuild after each
   repair before rerunning its focused test.
5. Run the complete CTest suite once for the whole successful wave, not once per slice. Run the
   repository's simulation verification once if the wave changed authoritative simulation state,
   hashing, replay, serialization, timing, or ordering.
6. Launch one `fa-gap-reviewer` over the combined wave. Fix high-confidence correctness,
   determinism, ownership, or integration findings; defer parity polish and speculative concerns.
7. After any reviewer-driven edit, rebuild before rerunning its focused checks and the complete
   CTest suite. Repeat simulation verification too when the edit affects authoritative simulation
   behavior.

If the integration gate cannot be made green quickly, remove only the exact edits attributed to
the affected worker before continuing and exclude that slice from dashboard progress. If those
edits share a file with pre-existing or concurrent work, stop and ask rather than reverting the
whole file. Continue only after the combined worktree is green again.

## Record and continue

The coordinator updates shared documents once per green wave:

- update only affected dashboard rows, scores, exact next tasks, headline arithmetic, and snapshot;
- update WP claims only when new retail evidence was actually learned;
- do not raise `Retail-validated` for implementation-only work;
- preserve every deferred residue as a bounded next slice rather than solving it immediately.

Report a compact wave table with subsystem, landed behavior, focused check, deferred residue, and
verification status. If time and context remain, compact the completed wave into those packets and
start another scout wave without rereading the repository broadly.
