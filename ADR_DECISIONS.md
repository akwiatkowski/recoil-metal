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

## ADR-012 — `_SpecTeam`'s four channels, from `mesh.fx` rather than from inference

**Context.** ADR-005 established that Supreme Commander's team-colour mask lives in
`_SpecTeam`'s alpha, and read its red channel as specular strength. Green and blue
were deliberately left unused: nothing verified said what they held, and a guessed
operator renders as *shading* rather than as a bug. ADR-009 then found the game
ships its HLSL uncompiled in `gamedata/effects.scd` and concluded that
`effects/mesh.fx` was the likely authority. This is that file, read.

**Decision.** `NormalMappedPS` (mesh.fx:2184-2201) settles all four channels:

    albedo.rgb          = lerp(teamColour, albedo.rgb, 1 - spec.a)
    phongAdditive       = NormalMappedPhongCoeff * pow(phongAmount, 2) * spec.g
    phongMultiplicative = 2 * environment * spec.r
    emissive            = glowMultiplier * spec.b
    colour              = albedo * (emissive + light + phongMultiplicative) + phongAdditive

So **red multiplies the environment reflection, green scales an additive Phong
highlight, blue is emissive, alpha is the team mask** — with
`NormalMappedPhongCoeff = float3(0.6, 0.80, 0.90)` (a *colour*, so highlights are
faintly blue) and `glowMultiplier = 2.0`. The SupCom branch of the model fragment
shader now follows this exactly, and the Recoil branch is untouched; each family is
a port of its own engine's shader.

**Alternatives considered.** Keeping red as specular and leaving green and blue
unused — which is what ADR-005 chose in the absence of a source, and which conflated
two independent channels: it drove both the highlight and the reflection from red,
and rendered every emissive surface unlit. Also considered keeping the shared
Blinn-Phong lobe for both families for simplicity, and rejected: having the exact
expression and not using it is the mistake ADR-009 exists to prevent.

**Consequences.** Emissive and reflection join the light sum and are therefore
tinted by the albedo, while only the highlight is added on top. That grouping is
the non-obvious part — adding emissive after the albedo multiply washes glowing
panels toward white instead of letting them burn in their own colour.

Two normal-map conventions came out of the same read, and they differ, so neither
could have been assumed from the other. **Model** normal maps are DXT5nm-style
swizzled — `2 * tex2D(source, uv).gaa - 1` with Z reconstructed as
`sqrt(1 - x^2 - y^2)` (mesh.fx:565-571), i.e. X in green and Y in alpha, the two
channels DXT5 stores well. The **per-map** normal map is not: `terrain.fx` reads it
`.xyz * 2 - 1` straight. Decals use a third spelling, `.ag`.

## ADR-013 — Pathfinding is a coarse grid A*, and passability comes from slope and depth

**Context.** Milestone 8's units walked straight lines through cliffs and water.
The obvious source for "where can a unit walk" is the `.scmap` terrain-type
array, which the ground already reads for colour — but `Scmap.hpp` records its
semantics as undocumented, and guessing it would repeat exactly the mistake
ADR-009 exists to record.

**Decision.** Passability is derived from the heightfield instead, by the two
rules the engine itself uses. Slope: a face's steepness is `1 - normal.y`
(`ReadMap.cpp:765-778`), compared against
`1 - cos(clamp(degrees, 0, 60) * 1.5)` (`MoveDefHandler.cpp:84-95`). Depth:
ground under more than `maxWaterDepth` elmos is out, since Recoil's rule is a
depth limit rather than a water line. Defaults are BAR's Pawn — `maxslope 17`,
`maxwaterdepth 12`. The search is A* over an 8-connected grid of 8x8-square
cells with an octile heuristic.

**Alternatives considered.** *The terrain-type array* — undocumented, and unit
movement definitions live in `.bp` blueprints inside `units.scd`, a separate
archive and out of scope. *One node per heightmap square* — a million nodes on a
1024-square map, for a search whose answer is a route around an island. *Flow
fields* — the right structure once fifty units share one destination, and
premature while a single selected unit is all that can be ordered.

**Consequences.** A cell is passable only when every square in it is, so one
cliff face blocks a 64-elmo cell. That errs toward routing around things, which
is the right direction to be wrong in at this resolution, but it will refuse
gaps a unit could actually fit through. Waypoints sit at cell centres and
intermediate ones use a loose arrival radius, so units round corners rather than
driving into each centre and pivoting.

The visible consequence is that an unreachable destination now does nothing.
On an island map most units cannot reach most places, so the app reports how
many found a route — without that line, 27 of 120 units standing still reads as
a broken sim rather than as a correct answer.

Still absent, and deliberately: unit-unit collision. Units mass at a rally point
by standing inside one another. Collision changes what a path *means* — it stops
being a property of the terrain alone — and is a milestone rather than a detail.

## ADR-014 — A walk cycle is paced by distance, not by a clock

**Context.** Animation playback was a clock: `animationTime` advanced with wall
time and every unit read it. That is correct for a scene being inspected and
wrong for one being simulated, because feet slide whenever the cycle and the
ground disagree — and they disagree constantly. A unit standing still keeps
striding; one pivoting on the spot keeps striding; one that has arrived strides
forever.

**Decision.** The sim accumulates the horizontal ground distance each unit has
walked, and the phase is `distance / (speed * duration)` — stride being the
distance one cycle covers at full speed. Each batch declares whether its
instances' phases are the whole answer or an offset added to the renderer's
clock.

**Alternatives considered.** *A playback-rate multiplier per unit* — expresses
"walk slower" but not "stop", and needs rewriting the moment a unit turns.
*Deriving distance from displacement* — wrong for a unit that goes out and comes
back, which has covered twice what its displacement says. *Always driving from
the CPU* — would freeze the animation in headless captures, which have no sim
stepping them and where `--time` is the whole point.

**Consequences.** Stride length is an assumption, not data: it says the
animation was authored for the unit's top speed. Real content carries no such
field for this engine to read, and the assumption is exactly right at full speed
and gracefully wrong below it. A batch pushed intermittently holds its pose,
which is the honest result of nothing driving it.

The pairing also made the milestone verifiable. A marched scene is now
independent of `--time` — two renders half a cycle apart are pixel-identical
when distance-driven and differ over the model when clock-driven, which is a
deterministic offscreen test of the whole path from sim to shader.

## ADR-015 — The shadow map follows the camera, and the bias lives in the shader

**Context.** Units and terrain were lit but cast nothing, which reads as models
pasted onto a picture rather than standing on ground. One directional light and
one shadow map is the whole requirement: the sun does not move, and an RTS
camera looks at the same ground from a fairly constant height.

**Decision.** A depth-only pass from the sun into a 2048² depth texture,
sampled with a comparison sampler, four taps. The light's orthographic box is
sized and centred on the **camera**, not the map. The depth bias is applied in
the fragment shader, scaled by how obliquely the sun strikes the surface.

**Alternatives considered.** Both rejected alternatives were what the first
implementation actually did, and it rendered no shadows at all.

*A light box covering the whole map* is the tidier-sounding option and is much
worse. On an 8192-elmo map a 2048-texel shadow map is 4 elmos per texel and the
depth range spans the map's diagonal — so the bias needed to stop the ground
shadowing itself is several elmos, which is most of a tank's height. The shadow
lifts clean off its caster and nothing appears.

*`setDepthBias` on the encoder* takes its constant term in units of the depth
format's smallest resolvable step, which for a 32-bit float over a map-sized
orthographic range is not a quantity worth guessing at. Applying the bias in the
shader puts it in units this code chose.

*Cascades* would be the answer if the camera could be both close and far in one
frame. It cannot: the box is sized from the orbit distance, and one level is
enough at every zoom the camera allows.

**Consequences.** The light matrix is rebuilt every frame, which is a handful of
matrix operations against a pass that draws the whole scene. Shadows exist only
within the box, so a shadow cast from outside it is missing — invisible in
practice because the box is sized to what the camera can see.

The pass costs 1.060 → 1.668 ms GPU on a 200-unit scene at 1920×1080. It
re-submits all 2.1M terrain triangles even though the box now covers a fraction
of the map, so culling to the box is the obvious next saving and was not taken.

Shadow attenuates the sun only. Ambient stands in for sky light, which reaches
into shade; dimming it too makes shadowed ground black, which is a different
wrongness from no shadows at all.

## ADR-016 — Water carries its depth in its vertices

**Context.** The water was a four-vertex translucent quad, with a comment saying
depth-based tinting "would need the depth buffer as an input attachment". Flat
water is the single most obviously fake thing left on screen: it ends at the
shore in a hard line, and a puddle looks exactly like an ocean.

**Decision.** The surface is a 256-span grid whose every vertex carries how deep
the water is there, sampled from the terrain when it is uploaded. Depth drives
the colour ramp and the alpha; Schlick's approximation drives reflectance.

**Alternatives considered.** *Reading the depth buffer* — the option the old
comment assumed was necessary. It needs either a second pass or framebuffer
fetch, and it answers a question the CPU already knows the answer to: the
terrain height under a point on the water plane does not change.

*Sampling a height texture per fragment* would work and costs an upload sized by
the map — 134 MB on an 8192-square one — to compute something that varies
slowly enough to interpolate across a triangle.

*Keeping the quad and fading by distance from shore* has no shore to measure
from without the same data.

**Consequences.** The depths are cached at the grid's own resolution rather than
holding the terrain mesh, because `setWater` arrives *after* `setTerrain` and a
surface built only in the latter bakes the default level of zero into every map.

Two numbers came from looking rather than reasoning. At 128 spans the depth ramp
interpolates over 64-elmo triangles on a large map and the shoreline visibly
facets, so the grid is 256 — 131k triangles against the terrain's two million,
and free within measurement noise. And the two wave trains run at oblique,
incommensurable angles: aligned to X and Z, the obvious choice, their crests
intersect on a regular lattice and open water reads as tiled graph paper.

The waves perturb the normal only. Displacing the surface would need a mesh fine
enough to show it and a shoreline that moved with the waves, and the depth ramp
assumes a flat plane.

## ADR-017 — Cull the shadow pass by chunk, and merge the survivors

**Context.** The shadow pass re-submitted the whole terrain — 2.1M triangles —
even though ADR-015 had already shrunk the light's box to what the camera can
see. It was the only measured regression in the project: shadows took a
200-unit scene from 1492 to 783 fps.

**Decision.** `buildTerrainMesh` emits the terrain in 64-square chunks, each a
contiguous range of the index buffer carrying its own bounding box. The shadow
pass tests each chunk's bounding sphere against the light's box *in the light's
own axes* and draws only those that survive, merging consecutive survivors into
single draws.

**Alternatives considered.** *A world-space axis-aligned test* — wrong, because
the box is oriented along the sun and only happens to be axis-aligned when the
sun is straight overhead. *Per-triangle or per-square culling* — the CPU cost
would exceed the GPU saving. *Frustum culling the visible pass too* — worth
doing and not done here; this ADR is about the pass that was measurably wrong.

**Consequences.** Close camera: 1.082 → 0.705 ms GPU. Whole-map camera: 1.792 →
1.747 ms, which is about noise — every chunk survives, as it should.

The run-merging is not an optimisation, it is what makes the change a win at
all. Without it, culling replaces one draw of the terrain with one per chunk,
and at a camera distance that keeps them all it is *slower than not culling* —
which is what the first version measured, and what nearly had the whole idea
discarded as a mistake.

Chunk order matters as much: emitting chunk-by-chunk rather than row-by-row is
what makes a chunk's triangles contiguous. Row-major order interleaves every
chunk in a row and makes ranges impossible.

A chunk's bounding sphere is deliberately generous. Too generous costs a draw
that renders nothing visible; too tight drops shadows, and only from certain
camera angles.

## ADR-018 — Port water and sky from the game's shaders, with named stand-ins

**Context.** ADR-009 established that Supreme Commander ships its HLSL
uncompiled and that guessing at rendering behaviour has cost this project twice.
Every shader here is now a port with a `file:line` citation — except the water,
which was written from first principles, and the sky, which did not exist.

**Decision.** Port the composition and constants of `effects/water2.fx`'s
`HighFidelityPS` and `effects/sky.fx`'s `AtmospherePS`, substituting named
stand-ins for the three inputs this renderer does not have: the refraction
target becomes the depth ramp, the sky cubemap becomes the sky function, and
four scrolling normal maps become two oblique wave trains.

**Alternatives considered.** *Implementing refraction and planar reflection
first* — two more passes, and the engine's own low-fidelity path proves the
composition stands without them. *Keeping the hand-written water* — it looked
plausible, which is exactly the failure mode ADR-009 exists to prevent. *Waiting
for per-map colours* — the skybox block is parsed but not exposed, and the
structure is worth having before the values.

**Consequences.** Three of the engine's constants contradict what a physically
minded implementation would choose, and would not have been guessed:

- `waterLerp` is `clamp(waterDepth, 0.3, 0.3)` — a **constant** 0.3 of
  `waterColor`, not a depth ramp. The depth dependence lives in
  `skyreflectionAmount * saturate(waterDepth * 10)` instead.
- Fresnel comes from a lookup texture built from bias 0.1 and power **1.5**,
  far softer than a physical Schlick 5. That is why the engine's water reflects
  noticeably even viewed straight down.
- `waterColor` is `float3(0, 0.7, 1.5)` — a value above 1, so it brightens as
  well as tints.

One deviation is deliberate and marked: the wave-crest term is rarer and fainter
than the engine's, because two sine trains crest on a regular grid and at full
strength that reads as polka dots. The engine avoids it by summing four scrolling
normal-map textures, which is the thing being stood in for.

The sky function is shared by the sky pass and the water's reflection, so the sea
reflects the sky that is actually above it rather than a constant chosen to look
similar.

## ADR-019 — LOD cracks are closed with skirts, and the index buffer is level-major

**Context.** Milestone 12 shipped per-chunk LOD unstitched: neighbouring chunks
drawn at different levels disagree along their shared edge, because the coarse
one draws a chord where the fine one follows every vertex. No crack was ever
observed — the transition sits eight chunks out where a one-level difference is
subpixel — so this was a known hole rather than a reported bug.

Testing the *other* invariant found something worse. The renderer's cull-and-
merge (ADR-017) merges a chunk's draw into the previous one only while their
index ranges stay adjacent. Milestone 12 emitted each chunk's three levels back
to back, which puts chunk N's coarse ranges between chunk N's fine range and
chunk N+1's. No two chunks were ever adjacent at the level they were drawn at.
The merge had not fired for a whole milestone, and terrain was one draw per
chunk — the exact cost ADR-017 exists to avoid.

**Decision.** Emit level-major: all chunks' level 0, then all level 1, then all
level 2. Consecutive chunks are then adjacent at whichever level they share.

Close the cracks with skirts — a curtain hanging straight down from each
chunk's rim, at each level's own resolution, inside that level's index range so
a chunk stays one draw.

Two details are load-bearing. **Both sides need a skirt**: where a chunk's rim
is above its neighbour's chord its own skirt covers the gap, and where it is
below, the neighbour's does. Skirting only the coarse levels is the tempting
half-measure and leaves half the cracks open. And **the depth is derived**, from
the worst gap a coarsest-level chord can leave along that chunk's *rim* — the
only place a crack can happen, since error across a chunk's interior is
level-of-detail error that no skirt addresses.

**Alternatives considered.** Transition strips, which stitch exactly and need
the neighbour's level known at build time — it is chosen per frame from the
camera. A constant skirt depth, which must be sized for the worst cliff on the
map and then hangs into open air wherever the ground beside a chunk falls away.

**Consequences.** +11.2% vertices on a 1024-square map and +0.047 ms GPU. The
dropped vertices are shared between the segments either side of them; a pair per
segment is the obvious version and doubles that to 22%, five megabytes on a real
map to say the same thing. `TerrainChunk::Lod` now reports `surfaceIndexCount`
separately from `indexCount`, because the surface ranges are what tile the mesh
exactly once and the skirt is deliberately extra over the top.

The wider lesson is the one that cost a milestone: **an optimisation whose
failure mode is invisible needs its invariant asserted, not observed.** Culling
that stops merging renders an identical image.

## ADR-020 — The stratum normal convention is measured, not inferred

**Context.** A `.scmap` names a normal map beside each stratum's albedo. Porting
the blend was straightforward — `terrain.fx`'s `TerrainNormalsXP` is the same
chain of lerps as the albedo path, and ADR-009 already established that the
engine's shaders are the authority.

One thing that file does not state is which channel points up. It samples
`tex2D(...)*2-1` and hands the result straight to `CalculateLighting`, whose own
space is muddled by `.xzy` swizzles elsewhere in the same file. Inferring an
axis from a shader that never says one is precisely the mistake ADR-009 exists
to prevent, and the failure mode is nasty: a normal map read on the wrong axis
lights bumps as dents, which survives a look at the screen.

**Decision.** Measure it. A corpus test decodes the block endpoint colours of
thirty real stratum normal maps and asserts blue is the channel sitting near
+1 while red and green sit near the middle — which is what a tangent-space
normal map looks like, because it is "flat" almost everywhere.

Not a full BC decoder: a block interpolates between two RGB565 endpoints, so
averaging the endpoints estimates the mean colour closely enough to tell 255
from 128, and blocks otherwise go to the GPU verbatim with nothing in this
project ever needing to decode one.

**Also decided:** the detail normal *perturbs* the geometric normal rather than
replacing it. Supreme Commander replaces, because its terrain takes its slope
from a map-wide normal map; this mesh already carries the real slope in its
vertices, and trusting a tiled texture over it would flatten every hillside.
That makes the composition weight (1.5) ours rather than the engine's, since the
original has no such constant to copy — chosen by measurement, at 0.6 the mean
pixel difference against no normal maps is 1.13/255, present in the numbers and
invisible on screen.

**Consequences.** One faithfully reproduced asymmetry that reads as a typo:
`TerrainAlbedoXP` expands its masks (`saturate(m * 2 - 1)`, the correction
ADR-009 made) while `TerrainNormalsXP` twenty lines later reads them **raw**.
Tidying it would cut every stratum's relief off at half weight.

Nine more texture fetches per terrain fragment — naively +41% GPU, more than the
planar reflection costs. Skipping absent slots (uniform across the draw) and
zero-weight strata (per fragment but spatially coherent) takes that to +6.2%,
both leaving the image bit-identical in intent. It is a quality setting for the
same reason the reflection pass is.

---

## ADR-021 — A selection is marked on the ground, never on the unit

**Context.** Milestone 13 shipped two selection cues at once: a ring on the
ground, and the selected unit's team colour overwritten with white. The tint was
almost free — the team-colour field is already per instance, already uploaded
every frame, and already read by both families' shaders — and the argument for
keeping both was that they fail in different places (a ring hides under a unit at
a low camera angle; a tint is invisible on an already-white model).

**Decision.** Rings only. The tint is removed.

The cost argument was never the problem: the *meaning* was. In an RTS a unit's
colours are its allegiance, and that is a fact the player reads constantly and
involuntarily — which shape belongs to whom is the primary question of the genre.
Overwriting that channel to mean "selected" makes a unit appear to change sides
for exactly as long as it is in the selection, and it does so in the one register
the player trusts least consciously and therefore checks least. Two cues for one
piece of state is a redundancy worth paying for; two *meanings* on one channel is
not.

**Alternatives considered.** Tinting with a colour no team owns (the palette's
white team is 0.92, so pure 1.0 is unclaimed) — this is what shipped, and it
answers "could a player tell these apart side by side" rather than "does a
glancing look still report the right army". Brightening the unit's *own* hue
instead of replacing it keeps allegiance readable, but selection would then mean
a different colour per team, which is the opposite of what a selection cue wants:
one appearance for "mine, and this order will reach it". Outlines around the
selected model are what a modern RTS actually does, and remain open — they need a
stencil or an id buffer, so they are a milestone of their own rather than a
constant.

**Consequences.** The click handler lost 40 lines. With no per-unit paint to undo
there is nothing to remember, so the caller's `Selection` struct — batch,
instance, and the colour to restore — collapsed into `rm::SelectionEntry`, which
already held the first two, and the enter/leave set arithmetic collapsed into one
assignment. The frame callback rebuilds the rings from that list as it already
did.

The tint's genuine advantage is now genuinely lost: at a low camera angle a
crowd's rings are hidden behind the units standing on them. That is the argument
for outlines rather than the argument for tinting a team colour.

---

## ADR-022 — The LOD thresholds are set by a screenshot diff, not by an error budget

**Context.** Milestone 12 introduced per-chunk detail levels with the transitions
at 8 and 20 chunk widths, and milestone 13 discovered that the cull-and-merge
those numbers were chosen alongside had never fired: each chunk's three levels
were emitted back to back, so no two chunks were adjacent at the level they were
drawn at. The thresholds were therefore tuned against a cost model where a change
of level was free, and needed revisiting now that one costs a broken run.

**Decision.** 6 and 14, chosen by measurement. The merge itself moved out of
`Renderer.mm` into `core/mesh/ChunkDraws.hpp` so that a draw count is a test
rather than something nothing can see.

Two measurements, both on aw04 with the real camera: `--bench-offscreen` GPU time
at 1920x1080 with 200 units, and the mean per-channel difference of a screenshot
against the same view with detail levels off entirely.

| near/far | whole-map GPU | draws | triangles | whole-map diff | focus-60 diff |
|---|---|---|---|---|---|
| off | 2.598 ms | 1 | 2 097 152 | — | — |
| 16 / 40 | 1.693 ms | 1 | 524 288 | 0.247/255 | 0.000/255 |
| 8 / 20 (shipped) | 1.510 ms | 6 | 235 520 | 0.385/255 | 0.000/255 |
| **6 / 14** | **1.380 ms** | **1** | **131 072** | 0.452/255 | 0.000/255 |
| 4 / 10 | 1.384 ms | 1 | 131 072 | 0.452/255 | 0.258/255 |

**The premise turned out to be wrong, which is the useful part.** The worry was
that band boundaries were breaking runs and costing draw calls. They were — and
it does not matter. 8/20 issues six draws against 6/14's one and is still 0.13 ms
*slower*, because it draws 80% more triangles. A whole-map framing is vertex-bound
and half a dozen extra draw calls do not register against 100k triangles. The
merge's value is that it makes the threshold cheap to get wrong, not that it is
itself fast.

6 is where the near threshold stops being free. At `--focus 60` — a mid-range
working camera — 6 renders an image byte-identical to full detail, no pixel off by
more than 3, while 4 moves 1.23% of them by up to 207. Below 6 there is nothing
to win: 4/10 measured no faster, because both already put the whole overview at
level 2.

**Alternatives considered.** Deriving the thresholds from a projected-error
budget, which was the first attempt and is the textbook answer. A level-1 chord
on this map is 13 elmos out at the median chunk and 92 at the worst; at 800 pixels
of viewport and a 60-degree field of view, holding that under one pixel demands a
threshold near 60 chunk widths — four times the width of the entire map. The model
says "never use LOD" and the screenshots say the error is invisible, because it
hides under ground texture and shadow. Where a model and the corpus disagree this
project has a rule about which wins.

Ordering the chunks along a space-filling curve so that distance bands are
contiguous in the buffer, and boundaries cost one run break instead of one per
row, was the other option. It is the right fix for the problem the measurement
says we do not have.

**Consequences.** 47% off the whole-map frame against no detail levels, 8.6%
against the shipped thresholds, for a mean difference of 0.45/255 — under a fifth
of one step of an 8-bit channel. Close and mid cameras are untouched: every chunk
within 3072 elmos of the eye is at full detail, which is more than fills such a
view, so LOD does not engage at all where the player is working.

Also settled while reading it: the shadow pass draws terrain at level 0
deliberately, not at the cheapest level as its comment had claimed. A shadow map
records the surface the main pass will shade, and a coarser one disagrees with it
by the whole chord error of that level — 13 elmos at the median here, far more
than any depth bias absorbs. The ground would shadow itself in bands.

---

## ADR-023 — The water's refraction offset ships switched off, and the reason is the wave field

**Context.** Milestone 12's water absorbs what is behind it but cannot bend it: it
reads the scene through a framebuffer fetch, which returns the current pixel and
no other. `water2.fx` offsets that read by the surface normal, which needs a
sampleable copy of the colour target — recorded as an open item ever since.

**Decision.** Build the copy, ship the offset, default it OFF.

The copy is a grab pass: end the render encoder after the terrain, units, props
and rings, blit the colour attachment into a texture, resume with both
attachments loaded back, then draw the water sampling that texture. `encodeScene`
now owns its encoder rather than receiving one, because it may need two — which
also unified the three call sites that each used to build a pass and end an
encoder around it. Measured at +0.17 ms on aw04 and +0.28 ms on a Supreme
Commander sea map: the colour and depth attachments are written out to memory and
read back, which a single pass on a tile-based GPU never does.

That part works. What does not is the field being bent by.

**This water's normal is two analytic wave trains** standing in for the engine's
four scrolling normal maps (ADR-018). Displacing a screen-space sample by a field
that regular draws the field's own lattice over the water — dark rings tens of
pixels across when driven by the swells, and a diagonal hatch when driven by a
ripple field eight times finer. Measured at four strengths, down to a quarter of
what the map's own `refractionScale` asks for; the pattern survives all of them,
because it is not a matter of degree. A sum of two sinusoids has a lattice, and
moving a sample by it shows the lattice.

Two things do help and are kept, because both are also just correct. The offset is
weighted by how much of the bottom still shows — the same Beer-Lambert attenuation
the colour gets — so there is no bend where the water has already swallowed the
view, which is where the rings were worst. And it fades in over the first eight
elmos of depth, because at the waterline any offset samples dry ground or sky and
paints it inside the water as a bright fringe along the coast.

**Alternatives considered.** Shipping it on at a strength where the lattice is
"probably not noticeable" — rejected on the measurements above; there is no such
strength, only strengths where the artefact is smaller than the effect it is
supposed to produce. Adding more octaves to the analytic field pushes the lattice
finer rather than removing it, and at overview zoom a fine lattice aliases, which
is worse than a coarse one. The real fix is the engine's own scrolling normal-map
textures, which were always the plan and are now the blocking dependency rather
than the colour-target copy everyone assumed.

**Consequences.** Four quality switches, one of them off by default — which is the
looks-best rule applied rather than abandoned, since with this wave field
refraction does not look best. `f` toggles it live and `--refraction` turns it on
for a run, the only switch here with a positive flag, because "not this time" is
not the useful thing to say about something already off.

One incidental find, worth recording because it cost a segfault: metal-cpp's
`MTL::TextureDescriptor::texture2DDescriptor` is a class factory method, so what
it returns is autoreleased. Releasing it is an over-release that crashes a frame
or two later, nowhere near the mistake.

---

## ADR-024 — Props are drawn by the unit pipeline, and kept out of the unit list

**Context.** Milestone 14 found that the map corpus is full of scenery after all:
59 of the 60 stock `.scmap` maps place props, 418 942 of them, mostly trees and
boulders. A prop is a static model with one texture, which is very nearly what a
unit is.

**Decision.** Share the unit pipeline and shaders; do not share the unit list.

The GPU side is the same problem, so it uses the same solution — same vertex
function, same fragment function, same instance layout (`UnitInstance`, whose
team-colour field a prop simply does not use). Two differences, both flagged by a
uniform: a prop's albedo alpha is a CUTOUT rather than a team-colour mask, and it
has no shading texture at all.

The alpha distinction is not cosmetic. Both content families keep a team-colour
mask in an alpha channel somewhere — Recoil in tex1's, Supreme Commander in
`_SpecTeam`'s — and a prop's albedo alpha is the shape of a leaf cut out of the
quad it is painted on. Read as a mask, a palm frond renders as a solid green card
in the player's colour. It is a `discard` rather than a blend because scenery is
drawn in arbitrary order and alpha blending without sorting puts a near frond
behind a far one; a cutout is order-independent, which is what keeps 14 000 trees
one instanced draw.

**The list is where they must NOT be shared**, and the reason is the sim rather
than the renderer. Everything in the unit list is ticked, collided against and
pickable, so a tree in it would be shoved aside by passing infantry, would be
selectable, and would accept a move order. Hence `PropBatch` and `setProps`, which
duplicate `UnitBatch` and `setUnits` in shape and differ in exactly that.

**Two scale conversions, and both are needed.** A blueprint's `UniformScale` takes
the mesh to OGRIDS — cross-checked against the blueprints' own `SizeY`, the
collision height in ogrids, which states 1 for both a palm and a pine whose meshes
measure 0.96 and 1.18 once scaled — and an ogrid is 8 elmos, the same factor the
prop positions take when the map is read. Applying only the first leaves a pine 1.2
elmos tall, which renders as a scatter of dark specks and reads as a texture
problem rather than a units one.

**Consequences.** Props do not cast shadows. The shadow pipeline has no fragment
shader — the depth attachment is its whole output — so it cannot honour a cutout,
and a tree pushed through it would cast the shadow of the untrimmed quads its
leaves are painted on: hard rectangles scattered over the ground, worse than
nothing. Giving that pass a discarding fragment shader would cost every
shadow-casting surface its early-depth rejection, for scenery.

**And they are culled by distance, per prop, per frame**, using the furthest LOD
cutoff their own blueprint states — 10 to 1000 ogrids across the 335 shipped ones,
so a shrub goes at 800 elmos and a landmark tree survives to 8000.

This is why they stopped being expensive. Before it, 5182 props cost 2.8 ms of a
7.6 ms frame and the busiest map's 46 971 cost 6.2 of 11.2 — paid at exactly the
framing where a tree is a pixel. After it, a whole-map view costs nothing
measurable (4.156 ms against 4.263 with the feature off) and a working zoom costs
0.17 ms for scenery that is actually visible.

The cull is a per-frame CPU pass over every prop, which sounds like the wrong trade
and is not: the test is a squared distance, the OUTPUT is small in both regimes
(almost nothing survives when zoomed out, and only what is near survives when zoomed
in), and it buys back six milliseconds of vertex shading. It needs the instances kept
CPU-side to filter from — 2.2 MB on the busiest map — and it turns the prop instance
buffer into a per-frame ring like the units', since what is in it now changes even
though the props do not move.

Nothing pops, and not by luck: graded cutoffs spread the disappearance across the
zoom range. Measured as the share of pixels the scenery accounts for while pulling
back — 12.1%, 8.6%, 2.6%, 1.11%, 0.33%, 0.05%, none — so the last props to go are
contributing a twentieth of one percent of the frame.

What remains is the mesh LODs: a prop that IS drawn is drawn at its finest level,
and using the other two cutoffs would need a batch per (blueprint, level) with the
level chosen per prop per frame. That buys the mid-range, which is the 0.17 ms still
on the table.

---

## ADR-025 — A particle uploads its history, not its state

**Context.** Milestone 14's dust needed a particle system, and the obvious design
walks every particle on the CPU each frame to move it, then uploads the result.

**Decision.** Upload where a particle was born, the velocity it was born with and
how long ago that was. The vertex shader works out where it is: origin + v·t +
½g·t². The CPU never integrates a position.

This is less code, not just less work. The alternative does the same arithmetic in
the slower place and then has to upload it anyway; here the per-frame CPU cost is
advancing one float per particle and dropping the expired, and the buffer contents
for a given particle never change after birth.

Geometry follows from the same idea: each particle is one instance of a four-vertex
draw whose quad is expanded from the vertex id and turned to face the camera using
the view-projection's own rows. No geometry is uploaded and there is no index
buffer. 1983 particles measured at 1.428 ms against 1.423 without — free, within
noise.

Blending is PREMULTIPLIED, source One rather than SourceAlpha, so one pipeline
serves both kinds of particle this will ever want: translucent dust is (rgb·a, a),
an additive spark is (rgb, 0). With straight alpha those are two blend states, two
pipelines, and no shared draw.

**Consequences, and two mistakes worth keeping written down.**

A puff born exactly on the ground is coplanar with the terrain drawn there, and a
depth-tested particle pass loses that fight. It fails totally and silently — the
draw is issued, the count is right, and nothing appears, which reads as the feature
being broken rather than as a depth result. Fixed the way the selection rings fixed
it: `LessEqual` plus an elmo of lift. Establishing that the pipeline worked at all
took temporarily colouring the dust bright red at alpha 0.95.

And the fragment's radial falloff was squared, which looked right in the abstract
and shrank a puff's visible core to a fraction of its quad — a 17-elmo puff read as
a speck, with most of the sprite spent on a gradient too faint to see. Linear.

The emission rate carries a fractional debt between frames so it does not depend on
the frame rate, and clears rather than banks it while nothing moves — otherwise a
squad that stands still for a minute exhales a minute of dust in the frame it moves
again.

A third mistake, and the most instructive: dust was first gated on
`MoveState::speedElmosPerSecond`, which is a unit's TOP speed — a capability it
carries whether or not it has anywhere to be — rather than on `moving`, the flag
the sim maintains for "has an order and has not arrived". Every parked unit smoked.
It was caught by a line of output disagreeing with itself: `0 of 60 units routed`
alongside `840 dust particles still in the air`. Reading a capability as a state is
a whole class of bug, and the reason `DustEmitter` now names both fields for what
they are.

---

## ADR-026 — A prop normal map carries two axes, and the third is reconstructed

**Context.** Prop blueprints name a normal map (`NormalsName`, on 244 of the shipped
levels) and the loader was dropping it. ADR-020 established for the STRATUM maps
that the convention has to be measured rather than inferred, since reading one on
the wrong axis lights bumps as dents. The prop maps are a different convention
again — extracting them broke the stratum corpus test, which was the first hint.

**Decision.** Two channels, third reconstructed:
`x = a·2−1`, `y = grey·2−1`, `z = √(1 − x² − y²)`.

**Measured, over all 221 prop normal maps in the extracted content.** Every one is
BC3, and in every one *red, green and blue are equal* — one value replicated across
the colour channels, with a second in alpha. Means across the corpus: 131 for the
grey, 127 for alpha, both within a few counts of neutral, which is what a normal map
averages to and confirms the reading. A stratum map by contrast puts z in blue near
255. Two channels rather than three because BC3's alpha block is a better encoder
than its RGB565 colour block, so an axis kept there survives compression where a
third of a packed triple does not.

**WHAT IS NOT MEASURED, stated plainly.** Which of the two is x and which is y, and
their signs. The layout implies the DXT5nm convention — x in alpha, y in the
replicated colour — and that is what this uses, but the corpus cannot settle it: both
axes average zero by construction, so no per-channel statistic distinguishes a swap
or a flip. Nor could it be settled by eye here, because the props with real relief
(rocks, wrecks) are placed sparsely and far from anywhere a camera can be put by the
flags this app has. It matters: with the map on, a close view differs from the same
view without by a mean of 3.381/255 across 9.22% of its pixels, which is more than
the stratum maps move. A prop whose relief is unmistakable, viewed close, would
settle it in one screenshot.

**Alternatives considered.** Per-vertex tangents, which is the textbook way and which
the data supports — `.scm` carries a tangent and a binormal per vertex and this
loader reads past them. Rejected on cost: plumbing them through would grow
`ModelVertex` from 36 bytes to 60 for EVERY model in the project, 2000 BAR unit
meshes included, to normal-map scenery. The basis is instead derived per pixel from
screen-space derivatives of world position and uv, Gram-Schmidt'd against the
interpolated normal — a few instructions on the prop path alone and no memory
anywhere.

**Consequences.** The prop path now reads the shading texture slot as a normal map,
which is why `hasTexture2` means something different there than on the unit path and
why `alphaIsOpacity` is what distinguishes them. The reconstruction clamps its
radicand: a compressed pair can leave the unit disc, and a negative one would come
back NaN and paint the fragment black.
