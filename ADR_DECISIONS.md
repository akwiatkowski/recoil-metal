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

## ADR-005 — Both content families in one struct, with two recorded differences

**Context.** ADR-004 predicted `.scm`/`.sca` would be "two more loaders filling
the same struct". Implementing them showed the prediction was right about the
shape and wrong to think it was free: two conventions genuinely differ, and both
are *invisible* rather than wrong-looking if guessed. `.s3o` stores vertices
relative to their piece; `.scm` stores them in model space, so adding the bone
offset scatters the model. `.s3o` puts the team-colour mask in tex1's alpha;
Supreme Commander puts it in `_SpecTeam`'s alpha — its albedo is frequently DXT1
and carries no usable alpha at all.

**Decision.** `Model` records which family it came from, and exactly two things
read that field: `restPose` (which transform a bone contributes) and the fragment
shader (which channel the mask is in). Everything else — placement, batching, the
instanced draw, the texture cache — is family-blind. Both differences were
verified against the retail corpora rather than assumed.

**Alternatives considered.** Normalising at load time by baking bone offsets into
`.scm` vertices — rejected: it destroys the rest pose that `.sca` animates away
from. A `Family` field on nothing, with per-format renderers — rejected: it
duplicates the entire draw path to express two booleans. Deriving the family from
the file extension at the call site — rejected for the same reason the map path
sniffs magic: the extension is not load-bearing and the loader already knows.

**Corroboration.** Both layouts were derived byte-by-byte from the retail corpus
before the reference reader ADR-004 cites was located. They then agreed with
`sc_io.py` field for field — `4s11I`, `16f3f4f4i`, `3f3f3f3f2f2f4B` for `.scm`;
`4sIIfIIIII`, a 28-byte root record and `fI` + 7 floats per bone per frame for
`.sca`. Two independent derivations agreeing is worth more than either alone,
and it is the reason the vertex-space and team-mask findings above can be stated
as fact rather than as inference.

**Consequences.** Adding a third family means one enum value and two switches,
both of which the compiler will point at. The cost is that `Family` is a lie
waiting to happen if a future format mixes conventions — at which point it should
become two independent fields, which is a mechanical change.

## ADR-006 — Poses baked per keyframe at upload, played back as a buffer offset

**Context.** `.sca` is the only keyframed animation any format in scope
delivers. Playing it needs a per-bone transform per frame; the obvious
implementation rewrites a GPU buffer every frame, which races the frames already
in flight and needs either rotating buffers or a fence.

**Decision.** Compute every keyframe's pose once, at upload, and concatenate them
into one immutable buffer. Playback selects a keyframe by *offsetting the bind*.
Nothing is written after upload, so there is no race to synchronise and no
per-frame CPU cost. Bone transforms are a quaternion plus a translation (32
bytes), not a 4x4: both families are rigid, and there is no row/column convention
to get backwards between C++ and MSL.

**Alternatives considered.** Per-frame CPU pose evaluation into a triple-buffered
ring — rejected as more moving parts for work that is identical every loop.
Skinning matrices on the GPU from raw keys — rejected: it puts the family-specific
rest-pose inverse into the shader, where ADR-005 works to keep it out.

**Consequences.** Memory is bones x 32 B x keyframes per batch — 45 KB for a
20-bone, 71-keyframe walk cycle, which is nothing. Playback snaps to the source's
own 30 Hz keys instead of interpolating per display frame; interpolating would
cost CPU work every frame to invent detail the file does not contain. Every
instance in a batch shares one clock: enough to see an animation run, and far
short of per-unit state, which belongs with a sim.

## ADR-007 — Start positions come from `_save.lua`, and the Lua reader learns six data constructors

**Context.** `.scmap` carries no start positions. The obvious companion file,
`<map>_scenario.lua`, carries none either — it names the armies and groups them
into teams, and that is all. The coordinates are in `<map>_save.lua`, under
`Scenario.MasterChain._MASTERCHAIN_.Markers`, as markers keyed `ARMY_<n>` mixed
in with dozens of transport, rally and mass markers. That file wraps every leaf
in a constructor call — `VECTOR3( 672.5, 18.7, 346.5 )`, `STRING( 'ff800080' )`,
`GROUP { ... }` — and `core/lua/LuaTable.hpp` exists precisely to refuse calls.

**Decision.** Read `_save.lua`, and teach the Lua reader exactly six constructors
by name: `STRING`, `FLOAT`, `BOOLEAN`, `VECTOR3`, `RECTANGLE`, `GROUP`. Each
takes literal arguments and hands them back, so they are data written in call
syntax rather than computation, and reading them holds the file's stated line
instead of crossing it. Every other call is refused exactly as before, as are
wrong argument counts and wrong argument types. The set is closed by evidence:
those six are the only calls appearing anywhere in the 61 stock maps' `_save.lua`,
which the corpus tests recheck.

**Alternatives considered.** Reading `_scenario.lua` as well, to learn which
armies are playable — rejected on measurement: many stock maps declare an extra
`ARMY_9 NEUTRAL_CIVILIAN`, and it has no marker on any of them, so the marker set
already *is* the playable set. Asserted in the corpus tests rather than assumed.
A regex or ad-hoc scan for `ARMY_<n>` positions — rejected as exactly the
guess-the-format habit the sequential readers exist to avoid. Embedding a real
Lua interpreter — still a dependency this repo does not want.

**Consequences.** The reader is now slightly more than a table-literal parser, and
the header says so. `GROUP` cost a corpus run to find: it is spelled with Lua's
call-with-table sugar, `GROUP { ... }` with no parentheses, so the survey that
found the other five by grepping for `(` missed it entirely. Campaign maps yield
zero start positions, correctly — their armies are named for factions and spawned
by mission script — and the renderer falls back to its scatter.

## ADR-008 — Splat layers bind individually, not as a texture array

**Context.** The staged `.scmap` plan called for the nine ground strata to go
into a `texture2d_array`, which is the textbook shape for "N textures indexed in
a shader" and what the S3 plan assumed.

**Decision.** Bind them as nine separate `texture2d<float>` arguments, gathered
into an MSL `array<texture2d<float>, 9>` at the call site. Thirteen fragment
textures in total with the two masks and the fallback, far inside what Apple
silicon allows.

**Alternatives considered.** The planned `texture2d_array` — rejected by the
corpus. A texture array requires every slice to share one size and one pixel
format, and across the stock maps the albedo strata come in **eight** distinct
combinations: 256², 512² and 1024², in BC1, BC2, BC3 and uncompressed BGRA8.
Fitting them into an array would mean transcoding every stratum to one format and
rescaling to one size at load, which costs a block encoder, loses mip levels, and
buys nothing — the binding limit was never the constraint.

**Consequences.** The shader indexes the array dynamically in its blend loop,
which Apple silicon supports. Adding a tenth layer (the macrotexture) is a
constant and one more binding. The cost measured 1.952 ms GPU against 0.864 for
the single-fetch path on Seton's Clutch — eleven texture reads instead of one —
with the Recoil path unchanged at 0.546 ms CPU.

**Related.** The layers also needed their own *sampler*. The existing one clamps,
correctly, because the ground atlas and both masks cover the map exactly; a
stratum tiles every few dozen elmos, so its uv reaches into the hundreds, and
under clamping every repeat past the first samples the edge texel. That renders
as a smooth smear — which reads as a missing texture rather than as a wrong
sampler, and was found by looking at a close-up rather than by any test.

## ADR-009 — Port the terrain blend from the engine's own shader, once it was found

**Context.** Milestone 6's splat was reconstructed from the map corpus, because
no reference for the blend was known. Two pieces were left deliberately unread:
the macrotexture's combine operator, and whether the masks are used raw. The
macrotexture was recorded as "deferred on principle — a guessed operator renders
as shading rather than as a bug".

**Decision.** The reference existed on disk the whole time. Supreme Commander
ships its HLSL inside `gamedata/effects.scd`; `effects/terrain.fx` contains
`TerrainAlbedoXP`, which is exactly this renderer's path — eight strata through
two masks. Its semantics are now ported rather than inferred.

It confirmed the reconstruction's blend chain (`lerp` per stratum, in order,
lower -> strata 0-7 via `mask0.xyzw` then `mask1.xyzw`) and the `WRAP` addressing
the repeating sampler supplies. It corrected two things: masks are read as
`saturate(m * 2 - 1)`, and the macrotexture is `lerp(albedo, upper.rgb, upper.w)`
— keyed on its own alpha, not multiplied and not mask-driven.

**Alternatives considered.** Continuing to defer the macrotexture — moot once the
source was found. Guessing a multiply — which is what would have been guessed,
and is wrong.

**Consequences.** The mask expansion is not cosmetic: with raw masks every
stratum contributes across the whole map at up to half strength, which reads as a
muddy wash rather than as distinct ground. One more texture fetch takes the splat
to 2.063 ms GPU from 1.952.

The wider consequence is that **the engine's shaders are available locally and
should be consulted first from now on**. `effects/mesh.fx` in the same archive is
the likely authority for the `_SpecTeam` green and blue channels that ADR-005
left unread, and for the per-map normal map convention. The corpus-derived
reconstruction was a good method in the absence of a source, and it got the
structure right — but it cost two wrong details that ten minutes of reading would
have prevented.

## ADR-010 — Instances get a frames-in-flight ring, not a second buffer or a stall

**Context.** Until milestone 8 nothing in a scene changed between frames:
instances were uploaded once at `setUnits`, and animation playback is a buffer
offset into poses baked at upload (ADR-006), so a moving model costs no per-frame
CPU work. Movable units need the instance buffer rewritten every frame. It is
`StorageModeShared` — CPU and GPU see the same memory on Apple silicon, which is
what makes the upload free and also what makes writing it while the GPU may
still be reading last frame's copy a plain data race.

**Decision.** The buffer holds `kMaxFramesInFlight` (3) consecutive copies of the
instances. `beginFrame` acquires a counting semaphore released by the command
buffer's completion handler, then advances the slot; drawing selects the slot by
buffer offset. `setUnits` seeds every slot with the initial instances, so a batch
that is never pushed still draws correctly from whichever slot a frame lands on.

**Alternatives considered.** *Waiting for the GPU each frame* — correct, trivial,
and throws away the pipelining the offscreen benchmark exists to measure.
*Double buffering* — one fewer slot for the same machinery, and the display
link's own drawable pool is three deep, so three is the number that matches what
actually paces the frame loop. *Relying on that drawable pool alone to throttle
the CPU* — true in practice today, undocumented, and silently wrong the moment
the link is moved off the main run loop. *A fresh buffer per frame from a pool* —
allocation in the frame loop, to avoid a memcpy of 36 bytes per unit.

**Consequences.** The pairing is a real footgun: every `beginFrame` must be
followed by exactly one `drawFrame`, including on the path where there is no
drawable at all, or the ring starves after three frames and the app stops
rendering with no error anywhere. `Window` owns the pairing so nothing else has
to. The instance count may shrink per frame but never grow past what was
uploaded, and a batch must be pushed every frame or never — one pushed
intermittently shows a slot three frames stale whenever it is skipped.

## ADR-011 — Picking is a march on the heightfield, not a GPU read-back

**Context.** Click-to-move needs two answers: which unit is under the cursor, and
which point of ground is. The usual RTS answer is to render an ID or depth buffer
and read the pixel back.

**Decision.** Both are computed on the CPU. A screen point becomes a world ray by
inverting the camera's view-projection; the ground hit is found by marching the
ray at half-square steps and bisecting the first crossing; the unit is the
instance nearest the ray within a radius.

**Alternatives considered.** *Depth or ID buffer read-back* — exact, handles the
model's real silhouette, and costs a GPU round-trip plus a frame of latency on
every click. More decisively, it cannot be tested without a device, and this
repo's rule is that anything pure gets a failing test first. *Ray-triangle
intersection against the terrain mesh* — the heightfield has no triangles until
the mesh builder makes some, and a march does not care how the surface was
triangulated.

**Consequences.** Picking is fifteen unit tests instead of a screenshot. Units
are points with a radius rather than silhouettes, so a click near a large model's
edge can miss it and a click in the gap between two small ones can hit. A ray
grazing a thin ridge can tunnel through it — acceptable at once per click, not at
once per frame. The bilinear sampler that picking and the sim share is also why
the nearest-corner sampler from milestone 5 had to go.
