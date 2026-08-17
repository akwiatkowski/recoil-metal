<!-- Generated and maintained by Claude -->
# Architecture decisions

Short records of non-obvious design choices, newest last. Each answers "why is it
done this way" for someone who arrives later with a reasonable alternative in mind.

Decisions already recorded in [`AGENT.md`](AGENT.md) under *Settled decisions*
(C++23, metal-cpp, macOS-only, runtime shader compilation, AppKit, Catch2,
vertical slices, GPL-2.0) are not repeated here.

---

## ADR-001 — Decoded map data lands in a format-agnostic `HeightField`

**Context.** Milestone 2 had to load Recoil SMF maps, and the project is expected
to also read Supreme Commander `.scmap` eventually. The obvious move is a
format interface with an `SmfLoader` and a `ScmapLoader` behind it.

**Decision.** No interface. Both formats decode into a plain `HeightField`
struct holding the raw `uint16` grid plus `baseHeight` and `heightScale`, because
both formats *are* `h = base + raw * scale` over a corner-sampled grid, and
report 06 §8.6.2 confirms the two grids are byte-identical in layout.

**Alternatives considered.** An `IMapFormat` abstract base — rejected as
speculative generality forbidden by AGENT.md, and it would have bought nothing:
there is no polymorphic call site. Loading straight into GPU buffers — rejected
because it makes the loader untestable without a GPU.

**Consequences.** Adding `.scmap` later is one free function filling the same
struct (a `memcpy` plus two floats). Nothing downstream — mesh builder, camera,
renderer — knows which format it came from. Cost today: zero, since this is the
natural output of the SMF loader anyway.

---

## ADR-002 — The `.smf` header is not trusted for vertical scale

**Context.** Heights decode as `min + raw * (max - min) / 65536` from the binary
header. BAR's Angel Crossing ships that header **inverted** (`min=850, max=-150`)
and corrects it in `mapinfo.lua`, which the engine treats as authoritative
(`MapInfo.cpp:405-418`).

**Decision.** The loader reports the header's values as-is and never second-
guesses them. Applying the override is the caller's job, because it needs a
second file. `min > max` is passed through unchanged.

**Alternatives considered.** Detecting `min > max` and swapping — rejected: that
is guessing at intent, and an inverted range is legal (the raw domain runs
downhill). Refusing to load such maps — rejected: real content depends on it.

**Consequences.** Callers must consult `mapinfo.lua` or render maps upside down.
Documented in AGENT.md's gotchas and enforced by a real-map test, because a
synthetic fixture cannot catch it.

---

## ADR-003 — Lua is parsed as data, never evaluated

**Context.** `mapinfo.lua` carries the authoritative map metadata. Reading it
properly means handling a Lua file.

**Decision.** `core/lua` parses Lua *table literals* — comments, nested tables,
strings, numbers, booleans, positional and bracketed keys — and **refuses**
anything requiring evaluation. A computed value yields a `ParseError` and the
caller falls back to the binary header.

**Alternatives considered.** Embedding a real Lua interpreter — rejected as a
dependency the project does not want for one metadata file. Regex-scraping the
two keys we need — that was the first implementation, and it was replaced because
it could not see structure (it would have matched an unrelated `height` key).
Silently skipping computed values — rejected: that reintroduces exactly the
silent-wrongness failure mode of ADR-002.

**Consequences.** A `mapinfo.lua` that computes its heights is not understood,
and we say so rather than guess. Good enough for real BAR content; a full
interpreter can replace the reader behind the same API if that ever changes.

---

## ADR-004 — Model formats: Recoil-native now, Forged Alliance not foreclosed

**Context.** Milestone 5 renders units, and the engine must eventually handle
both Recoil (Beyond All Reason) content and Forged Alliance content. Three
candidate formats exist, and the README originally said "glTF/S3O".

Measured, not assumed:

| format | whose native | reference spec | vertex | files on disk | needs |
|---|---|---|---|---|---|
| `.s3o` | Recoil / BAR | 98-line header, 259-line parser | 32 B fixed | 127 | nothing |
| `.scm` + `.sca` | Supreme Commander | 228-line `scstudio/sc_io.py` | 68 B fixed | 608 + 139 | nothing |
| glTF | **nobody's** | 621 lines glue over ~17k-line `fastgltf` + 14 MB `simdjson` | accessor indirection | 1 | a JSON parser **and** Blender to produce it |

**Decision.** Recoil first: implement `.s3o`. Do **not** implement glTF now, and
do not design in a way that forecloses it or `.scm`/`.sca`.

The seam is a shared decoded model, following ADR-001's pattern rather than a
format interface: **a bone/piece hierarchy, one vertex array in which each vertex
names its bone, and one index array.** Both native formats map onto that
naturally — S3O partitions geometry per rigid piece, so each piece becomes a bone
and its vertices all carry that index; `.scm` is already one vertex array with
per-vertex bone indices. This is justified by S3O alone, since the piece
hierarchy is needed regardless.

**Alternatives considered.** glTF alongside S3O — rejected: it is an
*intermediate*, not a native format, and the most expensive of the three. Worse,
FAR's report 05 records that Recoil parses glTF animation tracks and then never
reads them, so the glTF route **discards animation** while `.sca` carries
keyframed bone motion directly. `.scm`/`.sca` first, skipping S3O — rejected:
Recoil renders BAR's `.s3o` natively, so unit benchmarking against it would stop
being like-for-like, and milestone 4's comparison is the project's main result.
Vendoring `cgltf` as a lighter glTF path — kept in reserve, not needed yet.

**Consequences.** Milestone 5 adds no dependencies. Supreme Commander support
becomes two more loaders filling the same struct, and gains real skeletal
animation rather than losing it. If glTF is ever genuinely wanted — to consume
FAR's existing converted output as-is — the JSON dependency question is deferred,
not answered, and nothing built here has to change to accommodate it.
