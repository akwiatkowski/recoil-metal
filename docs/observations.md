<!-- Generated and maintained by Claude -->
# Observations — what the retail game actually does

Notes from playing Supreme Commander: Forged Alliance, written down before they
decay into "I remember it working like…". This is the intake; confirmed findings
graduate into the claim ledger in
[`fa-exe-analysis-plan.md`](fa-exe-analysis-plan.md) as `OBS` evidence.

## What an observation is worth

Two powers, and they are not the same one:

1. **On priorities, absolute.** A note here re-orders the work queue. No gate, no
   argument, no evidence required — it is a preference about what matters, and
   preferences do not need proof. If a note says the vision circles look wrong,
   vision moves up, whatever the dashboard's critical path said.
2. **On evidence, `OBS` — authoritative about the symptom, silent about the
   mechanism.** Nobody outranks direct observation on *what happened on screen*.
   But "units stop before reaching the flag" is a symptom; whether that is an
   arrival radius, a deceleration curve or a path-cell snap is a question for the
   disassembly. An observation that contradicts an `EXE` claim is a **finding** —
   usually the wrong function was read, or the session was FAF rather than retail
   1.6.6 — and never a licence to overwrite the claim.

So: an observation opens an investigation and sets its priority. It closes an
investigation only when the question was itself observational ("does the wreck
survive an overkill?").

## Recording one

The environment line is the part that makes it usable a month later. Retail and
FAF differ in patched areas, and a note that does not say which was running
cannot be reconciled against the executable.

```markdown
### OBS-nn — one-line summary

- **Build:** retail 1.6.6 / FAF <version> — which one, always
- **Setup:** map, factions, unit(s), what was ordered
- **Observed:** what happened on screen, in the order it happened
- **Expected / ours:** what Recoil Metal does in the same setup, if known
- **Question it raises:** the one thing to go and read
- **Media:** screenshot or clip path, if any

**Status:** open | investigating | folded into `C-nnn` | closed as ours-is-right
```

Screenshots and clips live outside the repo (`reference/observations/`,
gitignored) — the note carries the path, not the bytes.

## Open observations

*(none recorded yet — this file was created 2026-09-03 with the protocol only)*

## Folded into claims

*(an observation moves here once a claim cites it, with the claim id)*
