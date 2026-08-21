<!-- Generated and maintained by Claude -->
# NOTICE — what `vendor/ai/` contains, and on whose terms

Nothing here is committed. `tools/fetch_ai.sh` holds the pins and the path lists — it is the
single source of both, so this file deliberately does not repeat them. What this file records is
what each tree *is*, what licence it carries, and **how far its provenance can be trusted**,
which turned out to differ between them.

---

## `vendor/ai/faf/` — Forged Alliance Forever's game AI

| | |
|---|---|
| **Upstream** | https://github.com/FAForever/fa.git, branch `develop` |
| **Pin** | `82a2612` (2026-08-14, *"Changelog 3837 (#7221)"*) |
| **Provenance** | **Verified.** The source checkout's HEAD is contained in `origin/develop`, so the hash is genuinely upstream's. |
| **Licence** | **None stated.** No `LICENSE` file, no copyright notice. |

**On the licence, plainly rather than glossed.** FAForever/fa is community-maintained Lua for
*Supreme Commander: Forged Alliance*, whose original code is proprietary; the project's README
requires players to prove ownership of a retail copy. It declares no licence of its own. This
tree is therefore used **by explicit decision, with provenance recorded rather than rights
asserted** — the question was asked and answered deliberately (ADR-039), not overlooked. Because
nothing is committed here, this repository redistributes none of it; a checkout fetches it from
FAF directly, on FAF's terms.

`engine/` is fetched alongside the AI and is not AI code: 12,154 lines of typed LuaLS
annotations reconstructing the closed Moho API. It is the specification the adapter's binding
layer is generated from, and the only written account of that API that exists.

**Measured on this pin** — the numbers ADR-039 rests on. 83,006 lines across 174 files, calling
222 distinct engine entry points (84 globals, 138 methods). Of 548 distinct methods called, 377
are defined inside the corpus and 33 leak into game Lua outside it. Dialect: zero files use `#`
line comments, zero use `arg[]`, 49 sites use the Lua 5.1 `#x` operator, against 115
`table.getn` and one `math.mod` — it runs on stock Lua 5.1 with three shims.

---

## `vendor/ai/circuit/` — CircuitAI, which BAR ships as "BARb"

| | |
|---|---|
| **Upstream** | https://github.com/rlcevg/CircuitAI.git |
| **Pin** | `0ef3626` (2025-12-17, *"Add move_state tag. Add PatrolTask for static constructors…"*) |
| **Provenance** | **Verified.** The pinned fetch succeeded — git rejects a sha upstream does not have — and the result is byte-identical to the tree the measurements were taken on. |
| **Licence** | **GPL-2.0**, `LICENSE` in the tree. Compatible with this project. |

**On the name.** Recoil checks this out as `AI/Skirmish/BARb`, which reads as though BARb were a
fork. It is not: it is `rlcevg/CircuitAI` itself, pinned by Beyond All Reason at a newer commit
and named for how the AI appears in that game. The same Recoil tree also carries
`AI/Skirmish/CircuitAI` at `edc7414` (2024-04-14) — the same repository, eighteen months older.
The newer pin is the one taken. Its own `VERSION` file reads `stable`.

53,648 lines of C++ in `src/circuit`, plus vendored AngelScript, lemon, kdtree, json and
triangulate, plus `data/` configs and AngelScript behaviour scripts. It builds its own threat
map, influence map, pathfinder and metal manager, and wants none of the engine's.

---

## `vendor/ai/recoil/` — the C ABI CircuitAI is compiled against

| | |
|---|---|
| **Upstream** | https://github.com/beyond-all-reason/RecoilEngine.git |
| **Pin** | `bcdca85` (master as of 2026-08-21) |
| **Provenance** | **Verified for the part that matters.** `rts/ExternalAI/Interface/` is byte-identical to the tree the interface was measured on. |
| **Licence** | GPL-2.0-or-later, as the Recoil engine. |

`AI/Wrappers/` and `rts/ExternalAI/Interface/` — `SSkirmishAICallback.h` (596 function
pointers), `AISEvents.h` (27 events), `AISCommands.h` (109 command topics). Engine code rather
than AI code, fetched because CircuitAI does not compile without it.

## How the pins were checked, and one difference worth knowing

All three trees were first measured on local checkouts, then re-fetched from upstream at these
pins and compared. That comparison is what turns a plausible hash into a verified one, and it
found one real difference:

- **FAF** — the source checkout's HEAD was contained in `origin/develop`, and the fetch returns
  the same 257 files. Verified twice over.
- **CircuitAI** — the local checkout was a single commit on no remote-tracking branch, so the
  hash was plausible but unproven. The fetch proves it: `git fetch --depth 1 origin 0ef3626`
  fails outright if upstream lacks that commit, and what came back is byte-identical to the
  tree the 53,648-line measurement was taken on.
- **Recoil** — originally unpinned, because the local checkout's entire history was four commits
  with no upstream ancestry and its HEAD named nothing upstream. Resolved by fetching `master`
  and comparing: **`rts/ExternalAI/Interface/` is byte-identical**, so this pin names the same
  596 callbacks, 27 events and 109 command topics that ADR-039 counts.

  `AI/Wrappers/` is **not** identical — three files differ and upstream no longer carries
  `JavaOO`. That difference is in the *local* checkout, not upstream: it sits on four macOS
  build commits on top of an unversioned import, and those commits touch `Cpp/src/AIFloat3.*`
  and `CUtils/Util.c`. Upstream is the clean tree, and upstream is what is fetched. Worth
  remembering if a Circuit adapter ever behaves differently here than the local Recoil build
  does — the local build is the modified one.

## Referenced, not fetched

`rts/ExternalAI/SSkirmishAICallbackImpl.cpp` in the Recoil tree is 5,554 lines implementing all
596 callbacks, with the complete name-to-implementation map at line 5263. It is the reference
*semantics* for a future Circuit adapter, and the reason ADR-039 records that the BAR side has
ground truth where the FA side does not. Add it to `RECOIL_PATHS` when that adapter starts.
