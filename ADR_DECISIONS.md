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

**Consequences.** Props cast shadows from a pipeline of their own — the only one in
the shadow pass with a fragment shader. It exists to run one `discard`: a prop's
shape is cut out of its quads by the albedo's alpha, and a depth-only pass records
the untrimmed quad, so a tree would cast the shadow of the card its leaves are
painted on.

This was first written up as a decision NOT to, on the grounds that a discarding
fragment shader would cost every shadow caster its early-depth rejection. That was
wrong, and worth correcting rather than quietly fixing: early-Z behaviour is a
property of the PIPELINE, not of the pass, so the terrain and the units keep theirs
and only the props pay. Measured at +0.50 ms over three alternating rounds — the
same order as the planar reflection, for tree shadows. Shipped on, without a switch
of its own, since turning the props off already turns their shadows off.

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
loader reads past them. Rejected on cost: after ADR-078's UV1 addition, plumbing them through
would grow `ModelVertex` from 44 bytes to 68 for EVERY model in the project, 2000 BAR unit
meshes included, to normal-map scenery. The basis is instead derived per pixel from
screen-space derivatives of world position and uv, Gram-Schmidt'd against the
interpolated normal — a few instructions on the prop path alone and no memory
anywhere.

**Consequences.** The prop path now reads the shading texture slot as a normal map,
which is why `hasTexture2` means something different there than on the unit path and
why `alphaIsOpacity` is what distinguishes them. The reconstruction clamps its
radicand: a compressed pair can leave the unit disc, and a negative one would come
back NaN and paint the fragment black.

## ADR-027 — A Supreme Commander unit's passability comes from its motion class

**Context.** The passability grid (`core/sim/Pathfinding.hpp`) needs a slope limit and
a wading depth per unit. BAR states both per unit — `maxslope` and `maxwaterdepth` on
every mobile definition — so the SMF path reads two fields and is done. A `.scmap`
unit blueprint states NEITHER, for any of the 568. What it states instead is
`Physics.MotionType`, one of eight `RULEUMT_*` values, and that is the whole of what
the file says about where the unit may go.

So this is not a format conversion. It is reimplemented semantics, which is the line
ADR-004 and ADR-005 draw, and it gets recorded because the numbers are ours.

**The evidence, such as it is.** The only slope figure the format carries anywhere is
`Footprint.MaxSlope`, in 58 of the 568 — and it is a GRADIENT rather than an angle
(0.25 in 57 of them, 0.5 in one), on aircraft, where it bounds the ground an aeroplane
may LAND on rather than the ground a tank may climb. Useful as a sanity bracket and
useless as the answer.

**Decision.** The class decides, and the slope number was already in the engine.

| motion class | count | slope | water depth |
|---|---|---|---|
| `Land` | 50 | engine default | **0 — does not enter water at all** |
| `Hover`, `AmphibiousFloating` | 27 | engine default | unlimited (travels on the surface) |
| `Amphibious` | 17 | engine default | unlimited (walks the seabed) |
| `Air` | 60 | — | — (not a ground mover) |
| `Water`, `SurfacingSub` | 40 | refused | refused |
| `None` | 374 | — | — (immobile) |

`sim::kDefaultMaxSlopeDegrees` of 17 means 25.5 real degrees once the engine's 1.5
factor is applied (Pathfinding.hpp), a gradient of 0.48 — steeper than the 0.25 an
aircraft needs to land on, shallower than a wall, and within a degree or two of what
every BAR ground unit in the corpus authorises. So the two content families agree
without either being bent to fit, and **nothing is invented that the engine did not
already assume**. One number for all ground classes, because the file gives no basis
for distinguishing a tank from a bot and inventing one would be inventing balance.

**Land's zero is the sharp end, and it is not a missing value.** Supreme Commander's
land units do not ford; BAR's wade 12 elmos and treat a shoreline as passable. The
passability fallback in `main.mm` used to read a 0 as "unstated" and substitute BAR's
default, which walked SupCom's tanks into the sea — so the test asks
`travelsOnGround(motion)` rather than whether the number is positive.

**Water and SurfacingSub are REFUSED rather than approximated**, via
`unitdef::travelsOnGround`. They need the inverse of the grid — water deep enough
rather than shallow enough — and the grid cannot express that: one grid serves the
whole scene and its predicate is "every square in this cell is walkable". Routing a
ship over it would path the ship across dry land, confidently. An empty answer is
worth more than a wrong one, and it is the honest report of a feature that does not
exist yet.

**Alternatives considered.** *Read `Footprint.MaxSlope` where it exists* — 58 units,
all aircraft, none of which uses the ground grid; it would set a limit for exactly
the units the limit does not apply to. *Derive a slope from `MaxSteerForce` or
`MaxAcceleration`*, which do vary per unit and do correlate with how nimble something
is — rejected as numerology: neither field is about terrain, and a plausible curve
fitted to them would be balance invented and then hidden behind arithmetic.
*Per-class slope numbers* — no basis in the data, as above.

**Consequences.** A per-unit slope limit still does not reach the grid: passability is
one grid for the whole scene (README, milestone 9), so a mixed force paths as
whichever unit built the grid. That was already true and this does not worsen it, but
it is now the reason two motion classes are refused rather than merely a known gap.
When per-class grids arrive, `travelsOnGround` is the seam they hang from, and
`Water`/`SurfacingSub` become an inverted grid rather than a special case.

## ADR-028 — The Lua story is two tiers, and the shim owns moho's names

**Context.** The project's goals now state both halves out loud: run the existing
Supreme Commander mods, *and* give people a modern way to write new ones. PLAN.md had
left the fork open ("our own API" vs "moho's API") and its decision 2 required native
functions to carry moho's names. The 2026-08-20 audit
(`docs/recoil-metal/supcom-lua-gameplay-survey.md`) settled both empirically: not one
of the 319 contract names appears in `src/` — the engine grew its own, better
vocabulary — while the audit itself hand-wrote the moho→C++ mapping table (~120 lines)
that the naming rule existed to avoid. The predicted cost has been paid once, as data.

**Decision.** Both, layered. **Tier 1** is the engine's own clean Lua API: what the
C++ actually exposes, what new mods target, what gets documentation, versioning and
tooling. **Tier 2** is moho compatibility — `moho.*`, the LuaPlus dialect loader
(ADR-029), the `On*` callbacks — written **in Lua on top of Tier 1** wherever
possible, seeded from the audit's mapping table. PLAN.md decision 2 is amended
accordingly: the shim owns the name mapping; the engine keeps its vocabulary and owns
the unit conversions (ogrids, degrees) at its boundary.

**Alternatives considered.** *moho names in C++* (the original decision) — paid
nothing in five milestones, makes the C++ worse to read, and the translation table it
was meant to avoid now exists anyway. *moho-only API* — every modder inherits the
fidelity debt and a dead dialect. *Own API only* — loses the ecosystem; no existing
mod runs at any price, and the ecosystem is the reason an open Moho matters.

**Consequences.** Fidelity work lands in Lua, where the community can patch it without
touching the engine — the same reason GZDoom's DECORATE-under-ZScript split outlived
the engine it reimplemented. The coverage score becomes a property of the shim, tracked
per milestone. The engine's API becomes a public contract and needs the discipline that
implies: versioning, and no leaking of sim internals through it.

## ADR-029 — LuaPlus is a dialect to accept, not a VM to adopt

**Context.** The corpus is written for GPG's LuaPlus-flavoured Lua 5.0: `#` line
comments in 1,240 of 1,414 files, `table.getn` in 477 places, `math.mod`, `arg[]`
varargs. LuaPlus itself is long unmaintained, and fusing dialect to binary is exactly
the coupling that has FAF stuck on a 2007 VM today.

**Decision.** A current VM — Lua 5.4, or the 5.1 family if ADR-030's determinism work
prefers it; determinism picks the VM, not nostalgia — plus a small lexer patch
accepting `#` as a line comment and a compat library for the 5.0 idioms. The dialect
is a **loader mode applied only to legacy archives**; Tier-1 content never sees it.
Acceptance gate, cheap and mechanical: all 1,414 shipped files parse.

**Alternatives considered.** *Embed Lua 5.0.2* — frozen, stop-the-world GC over
thousands of unit tables, no modern tooling. *Embed LuaPlus itself* — dead code, and
it reproduces FAF's coupling problem verbatim.

**Consequences.** New mods get a living language with debuggers and an LSP. The compat
surface is finite and enumerable (the audit lists every 5.0-ism by count), so "does the
corpus load" is testable in CI without any engine function existing yet.

## ADR-030 — The network protocol is ours: lockstep, deterministic, FAF at the lobby

**Context.** The goal is multiplayer integrated with FAF — whose players today suffer
desyncs from a closed 2007 engine nobody can fix. Compatibility with the retail sim
protocol would require matching that binary bit for bit, forever, and is explicitly
not wanted.

**Decision.** No retail sim or network compatibility, ever. The engine's own lockstep
on the 10 Hz tick, with three named obligations: **cross-platform float determinism**
(no fast-math, no FMA contraction in sim code, deterministic transcendentals — IEEE
basic arithmetic is already bit-exact everywhere, `sin`/`cos`/`exp` are not, which is
the lesson streflop encoded for Spring in 2006); **a deterministic VM configuration**
for sim-side Lua (iteration order, no GC-visible semantics); and **a per-tick state
hash with an immediate desync report**, Recoil's model, so a divergence names its tick
instead of ruining a match silently. "Integrated with FAF" means the lobby ecosystem —
server, client, ICE adapter, all open source — not the game protocol.

**Alternatives considered.** *Retail protocol compatibility* — bit-fidelity with a
closed binary is the one goal that would make every other goal impossible.
*Client-server with authoritative state* — thousands of units make state replication
the wrong economy; lockstep is what every shipped engine in this genre chose.

**Consequences.** Determinism becomes a standing constraint on all sim code from the
host arc onward, latent while the only platform is one compiler on one architecture,
binding the moment Linux (ADR-032) lands. Replays stay what they already are — an
input log against a fixed tick.

## ADR-031 — Beyond All Reason compatibility means content, not the Spring API

**Context.** The engine already loads both families — Recoil maps and models, BAR
blueprints, SupCom archives — with per-family semantics where they differ (the
turnrate-per-frame discovery of milestone 16). "Usable for BAR" could mean two
products: running BAR *content*, or replacing Recoil under the BAR *game*, which
would mean reimplementing `Spring.*`, LuaRules and LuaUI — a second Moho-sized
fidelity project against a moving target that has its own active engine team.

**Decision.** Content, not API. Maps, models, blueprints and tick semantics stay
green as a passive track; the Spring Lua API is refused. One substrate — VFS, render,
platform, pathfinding — with a per-game sim personality on top.

**Alternatives considered.** *Drop-in Recoil replacement* — starves the Supreme
Commander goal, which is the project nobody else is building, to duplicate one that
is actively maintained.

**Consequences.** If the Recoil community ever wants this renderer, the door is the
substrate, not a promise of API parity. BAR content stays a corpus for tests and a
second proof that the engine's abstractions are not shaped around one game.

## ADR-032 — Linux arrives through an RHI seam; Metal stays the reference

**Context.** The goal is macOS **and Linux**; the renderer is hand-built Metal with
the game's own shaders ported to MSL (`terrain.fx`, `water2.fx`). Metal does not run
on Linux, and nothing translates it there.

**Decision.** No rewrite now. When Linux becomes a milestone: a thin render-hardware
interface with WebGPU (Dawn / wgpu-native) and SDL3's GPU API as the candidates —
either gives Metal and Vulkan from one codepath — and shaders moved to a single
source language (Slang or WGSL) and transpiled. Until then the obligation is
**confinement only**: platform and GPU code stays inside `src/platform` and
`src/render`, which is already the layout, and new shader logic stays self-contained
and commented so the port is mechanical rather than archaeological.

**Alternatives considered.** *Rewrite on Vulkan today* — delays a playable game for
portability no one can use yet, and trades a working renderer for a regression hunt.
*Stay Metal-only* — contradicts a stated goal; macOS alone is not "for society".

**Consequences.** The real porting cost is the shaders, not the API layer, and it
grows with every MSL line written — which is accepted and bounded by the confinement
rule. The RHI decision date is a milestone boundary, not a surprise.

## ADR-033 — Damage is a sparse profile keyed by an armour class resolved at load

**Context.** A weapon deals one scalar (`unitdef::Weapon::damage`, a `Mag`), so every unit
is a damage-per-second number and unit composition cannot matter. Recoil's `DamageArray`
(`Sim/Misc/DamageArray.h`) holds one figure **per armour class**; the class names are
interned by `CDamageArrayHandler` from `gamedata/armordefs.lua`. Supreme Commander models
the same idea transposed: a unit declares `Defense.ArmorType`, a weapon declares
`DamageType`, and `lua/armordefinition.lua` holds `[ArmorType][DamageType] -> multiplier`.
Both corpora must import.

**Decision.** One runtime shape — **absolute damage per armour class, stored sparsely** —
with the two content families differing only in the importer. A `DamageProfile` is a base
`Mag` plus a short inline list of `{ArmorClass, Mag}` overrides; `against(armor)` is a
linear scan of that list falling back to the base. Armour classes are **names in the data
and a dense `std::uint8_t` index in the tick**, resolved once at load by an `ArmorRegistry`
built from a sorted, case-folded name list, with index 0 reserved for `default`.

Measured from `reference/FAF-fa/lua/armordefinition.lua:48-118`, which is what makes the
sparse form the right one rather than a guess: the whole game's matrix is **8 armour
classes x 9 damage types with exactly 10 non-1.0 entries and 4 distinct values**
(0.0, 0.032, 0.25, 0.55). An unlisted pair means 1.0 — `Experimental` lists only
`ExperimentalFootfall 0.0`, and Experimental units are plainly not immune to everything
else. Flattening the matrix into weapons at import therefore leaves **about 16 of the 494
shipped weapons carrying any override at all**; the other 478 are a bare scalar, exactly
what they are today. (Report `02 §9.6` estimates "~6 non-1.0 multipliers"; counted directly
it is 10 entries. The conclusion it draws is unaffected and correct.)

`paralyzeSeconds` rides on the profile from the start rather than being retrofitted: FA has
`EMP` and `Stun` damage types and Recoil has `paralyzeDamageTime` in the same struct, and
paralysis is an accumulator rather than a subtraction, so it cannot be bolted onto a `Mag`.

**Alternatives considered.** *Keep FA's shape — weapon carries `damage` + `damageType`, a
global matrix is consulted at impact* — preserves the authored structure, but costs a
matrix lookup per damage event, and BAR/Recoil content has no damage types at all, so BAR
import would have to invent a synthetic one per weapon. *Copy `DamageArray` literally* —
`std::vector<float> damages` is a heap allocation per weapon def and a pointer chase per
lookup, for a table that is empty in 97% of cases. *A hash map keyed by class name* —
rejected on the same grounds `IdPool` rejects Recoil's `SimObjectIDPool`: hash-container
iteration order is a determinism hazard, and a string hash in the damage inner loop at
5,000 units is real cost for no expressiveness.

*First-seen interning order for class indices* — rejected in favour of sorting. Recoil
sorts its key list (`DamageArrayHandler.cpp:43-45`) so the numbering is a function of the
*set* of names rather than of load order; that property is worth more here than there,
because the replay hash is the project's success criterion.

**Consequences.** Every damage call site grows an armour-class argument, which is why this
lands before more weapons exist rather than after. Shields become nearly free once it
exists: Recoil implements a shield as *just another armour class*
(`PlasmaRepulser.cpp:201-202` reads `damageArray.Get(weaponDef->shieldArmorType)`), and
that unification is worth inheriting.

We diverge from Recoil in one place deliberately: **armour class names are folded to lower
case.** `14-blueprint-census.md §8.7` measured `Structure` (222 units) against `STRUCTURE`
(1 unit) — the same class spelled two ways. Recoil's case-sensitive `armordefs.lua` would
hand that one unit a private armour class with no multipliers defined. Folding costs a
`tolower` at load and removes a silent content bug.

## ADR-034 — Projectiles vary by a motion tag, not by a class hierarchy

**Context.** There is one `Projectile` struct with one straight-or-arced advance
(`core/sim/Combat.hpp`), which cannot express AA, torpedoes, tracking missiles, nukes or
interception. Recoil's answer is a `CWeapon` hierarchy of fifteen classes plus a parallel
projectile hierarchy — 4,757 and 6,655 lines. Supreme Commander's is a four-layer Lua class
tree over 306 projectile blueprints.

**Decision.** Neither hierarchy. A `ProjectileKind` **tag plus a flags bitmask** on the same
flat, trivially copyable struct, dispatched by a `switch` in `advanceProjectiles`.

The evidence that the hierarchies are mostly not about simulation is in the corpus.
`13-projectiles-effects.md §2.1` classifies all 306 SupCom projectile leaves by root class:
**91 `MultiPolyTrail`, 73 `SinglePolyTrail`, 57 `Emitter`** — differences in what is
*drawn*, not in what is *simulated*, and that report's own conclusion is that such a leaf
"becomes one line in a weapondef: `cegTag = ...`". The axes that genuinely change the sim
are few: motion (straight / ballistic / tracking / semi-ballistic / torpedo), target-layer
mask, interception, splitting, and a proximity fuse.

**Alternatives considered.** *Virtual dispatch, as both reference engines use* — rejected
on two grounds, and the second is the load-bearing one. It is an indirect call per
projectile per tick at a target of thousands in flight; and it destroys the property
`Combat.hpp` already has and the whole thesis depends on, that a projectile is a fixed-size
trivially copyable value which the state hash can walk and a log can record. A polymorphic
projectile is not hashable without a visitor per subclass. **This is a case where the
existing design's instinct is right and both reference engines are wrong for our goals**,
and it is recorded so a later reader does not "fix" it toward Recoil.

*Model beams as short-lived projectiles* — rejected. A beam is instantaneous; it is a ray
query in the firing pass that applies damage at once and emits an event carrying its
endpoints, with nothing entering the projectile list. Keeping that list homogeneous is what
keeps the `switch` cheap and the hash simple.

**Consequences.** Interception is copied from Recoil rather than from FA: a `targetable`
bitmask on the projectile and an `interceptor` bitmask on the weapon
(`WeaponProjectile.cpp:428-431`). FA expresses the same rule as Lua predicate code
(`lua/sim/Projectile.lua:214` — torpedoes die only to `ANTITORPEDO`, tacticals only to
`ANTIMISSILE`, and only if `other.OriginalTarget == self`); the bitmask is that rule made
data, which is D6.

One capability goes beyond both engines because it is nearly free here: a **proximity
fuse**. `13 §1.5` records that nine FA blueprints use `DetonateAboveHeight` and that "there
is no Recoil proximity fuse" — a BAR import has to approximate it with a gadget. For us it
is one field and one comparison per tick.

## ADR-035 — Pathfinding: a cost field and shared flow fields, hierarchical A* deferred

**Context.** Routing is one uniform 64-elmo grid with 8-connected A* per unit
(`core/sim/Pathfinding.hpp`), with no cost model, no cache, no dynamic blocking and no
sharing — fifty units ordered to one point run fifty full searches. Recoil ships two
pathfinders totalling 16,325 lines, and `16-new-engine-feasibility.md §3a` explains why:
"Pathing is where RTS engines go to die... Recoil has two pathfinders because the first one
was not good enough after a decade."

**Decision.** Build in layers, cheapest first, and **treat this ADR as a staging post
rather than the answer** — the layers below are chosen to be individually useful and
individually replaceable, and the choice of a final architecture is deliberately deferred
until the engine can measure its own pathing.

1. **A cost field, not binary passability.** One byte per cell per motion class where 0 is
   impassable and otherwise the value is a speed divisor. This is a prerequisite for
   everything else and it fixes the current design's worst artefact on its own:
   `buildPassability` marks a 64-elmo cell impassable if *any* of its 64 squares is, which
   will refuse legitimate routes through tight gaps as soon as maps get crowded. A cost
   lets a partly-blocked cell be expensive instead.
2. **Flow fields, shared per destination.** Integer Dijkstra from the goal over the cost
   field, yielding a per-cell direction; goals snapped to a coarse cell so nearby clicks
   share one field; LRU cached. This is the direct answer to fifty-searches-for-one-order,
   it removes the per-unit stored path (and with it a variable-length run in the state
   hash), and it parallelises as one worker per field.
3. **A dynamic blocking overlay**, kept separate from static terrain cost, so a placed
   building dirties only the fields whose region it touches.
4. **Local avoidance by unit density folded into the field cost** before anything more
   elaborate — integer, deterministic, and no new subsystem.

Deterministic by the same rule the current A* already follows: integer costs with ties
broken on cell index (`Pathfinding.hpp:102-105`).

**Alternatives considered.** *JPS / JPS+* — a large constant-factor win, but valid only on
**uniform-cost** grids; layer 1 above makes costs non-uniform, so it is ruled out on
purpose and recorded here so the idea is not re-derived. *Navmesh (Recast/Detour)* —
float-heavy, hard to make bit-deterministic, and poorly matched to grid-aligned building
footprints; note that FAF's own 1,918-line Lua navmesh (`12 §1.4`) exists because Moho's
pathfinder is *opaque*, which is a workaround rather than an endorsement. *QTPFS-style
quadtree first* — Recoil built it because HAPFS was not good enough after a decade; two
pathfinders is not a starting position. *Hierarchical A* (HPA*, Botea/Müller/Schaeffer
2004) first* — the right layer for sparse single-unit queries and the intended layer 5, but
it is the wrong thing to build before the cost field exists.

**Consequences.** Flow fields are wasteful when one unit wants to go somewhere, which is
exactly what the deferred hierarchical layer is for; until it exists, a threshold on
shared-goal count decides which path is taken and the sparse case pays a full field.

Worth knowing that this is not exotic and that the content is already calibrated for it:
`12-moho-api-surface.md:827` documents Supreme Commander's own pathfinder LOD switch at 50
ogrids — "personal per-unit waypoints" close in, "waypoints shared by many units" far out.
FA already routes crowds on shared paths.

## ADR-036 — Parallelism may reorder work, never results

**Context.** The engine is single-threaded — no `std::thread`, `std::async` or
`dispatch_*` anywhere in `src/` — on a machine with 10 to 14 cores, against a 5,000-unit
target. Recoil multithreads path requests (`UnitHandler.h:88-90`) and parts of rendering.

**Decision.** Fork-join only, under one rule stated so it can be checked:

> Parallelism may change **when** work happens. It may never change **what order results
> are applied in.**

Concretely: fan out read-only work; each worker writes only to its own pre-sized slot,
indexed by unit slot; join; apply in slot order. No shared mutable state, no atomic that
decides a value, no work-stealing whose schedule can reach a result.

Ranked by value: path and flow-field computation first (it can leave the tick entirely,
with results applied at one named point); then targeting, which is a read-only query over
the spatial grid writing one answer per unit; then the snapshot-to-instance gather, which
is unsynced and needs none of this discipline; then the spatial grid rebuild; and last
damage and collision, which must compute in parallel and apply in slot order.

**The tick's pass order is explicitly not parallelised.** `Skirmish.hpp` documents why each
pass must follow the last; overlapping ticks is a determinism trap, not an optimisation.

**Alternatives considered.** *A general task graph over the whole tick* — the dependency
chain in `tickSkirmish` is genuinely serial, so the achievable win is intra-pass and the
graph would be scaffolding around one shape. *Threading now* — rejected on measurement
grounds, not principle: a fork-join barrier costs on the order of 5-20 us and the passes
at present unit counts do not. The trigger is a measured pass exceeding ~100 us in
`--bench`, not a unit count.

**Consequences.** This is the second dividend from D1 and the reason it is safe to attempt
at all: **parallel float reductions are order-dependent and parallel integer reductions are
not.** Recoil cannot parallelise as freely as we can precisely because its sim is float —
so a decision taken for cross-platform determinism turns out to buy multi-core headroom
too.

One macOS-specific constraint, since ADR-032 makes Metal the reference platform: an Apple
silicon part is 10-14 cores but only 8-10 of them are performance cores. Sizing a pool from
`hardware_concurrency()` schedules sim work onto efficiency cores and can be *slower* than
running serially. The pool is sized to the performance-core count, or the work is submitted
at `QOS_CLASS_USER_INTERACTIVE` and left to the scheduler.

## ADR-037 — Intel is a refcount grid per alliance; occlusion is a per-type algorithm

**Context.** The engine has no vision of any kind: `Minimap.hpp` states "no fog of war",
and `nearestTarget` (`Combat.hpp:142`) picks from the whole unit store filtered only by
hostility and range, so every unit shoots things it cannot see. The two engines we read
disagree about what vision *is*. Recoil raycasts terrain for LOS and radar and takes
circles for everything else (`LosHandler.cpp:92`). Supreme Commander does not consider
terrain at all: `effects/vision.fx:35` stamps `radius * vertex.xz + position.xy` — a flat
2D disc with no height input and no heightmap sample anywhere in the file.

**Decision.** Copy Recoil's *structure* and make the algorithm a setting.

The structure is a **reference count per square per alliance**, not a boolean
(`ILosType`, LosHandler.h:84-88). That is the load-bearing choice: with booleans one
unit's sight cannot be withdrawn without re-deriving every other unit's, and removal is
most of what an intel system does. Alliance rather than army because `Army.hpp` already
names `AllianceIndex` as "who wins together, and later who shares vision".

The algorithm is per intel type, as Recoil's own `algoType` is, and the two named
configurations are selectable:

  - **FA style** — every type a flat circle. What Forged Alliance does.
  - **Recoil style** — raycast with terrain occlusion for vision and radar, circles for
    sonar. Hills block sight and high ground is worth holding.

**Alternatives considered.** *Circles only*, as FA-faithfulness would argue: rejected
because it fixes the answer to a gameplay question a player should be able to ask, and
because occlusion is the more interesting game. *Raycast only*: rejected because it
renders FA maps in a way FA never did, on the content family we primarily load.

**Consequences.** We carry both algorithms and a mipped heightmap the circle path does not
need. The grid is integer and enters the state hash — `StateHash.cpp:35` makes a float in
hashed state a compile error — so both styles are covered by `--hash-log` and `make verify`
without new determinism machinery, and a style change invalidates a recorded log, correctly.
First pass carries Vision, Radar and Sonar, being what 391, 57 and 67 of the shipped
blueprints declare. The remaining nine FA intel types — Omni, Cloak, CloakField,
RadarStealth(Field), SonarStealth(Field), Jammer, Spoof — are additions to the same type,
not a redesign.

**Second pass, and the prediction held for five of the nine.** Omni is a fourth grid;
RadarStealth (15 units), SonarStealth (9), Cloak (4) and FreeIntel (8) are per-unit FLAGS read
at contact time, because a stealthed unit is ABSENT from a sense rather than harder to find in
it — which is why they short-circuit the query instead of shrinking anybody's radius. Omni
returns `Seen` rather than a blip: what makes it omni is that nothing hides from it, so it
carries an identity as well as a position, and it is the one query the flags cannot answer
their way out of.

**Three of the remaining four are a different shape, and one does not exist.** The two stealth
FIELDS and the jammer act on OTHER units, so they want a second KIND of grid — "who is hidden
here" rather than "who can see here", and every grid in `Intel.hpp` answers the second
question; `JamRadius` is also a table in the blueprints rather than a scalar. `WaterVision` is
read and unused until something is submerged. And **`CloakFieldRadius` appears zero times in
retail** — the list above named it from Recoil's vocabulary rather than from a count, and only
`Cloak` on four units is real.

The grid array cost one bug worth recording: `configure` emplaced three grids per alliance
while every index into it is `alliance * kIntelKindCount + kind` with no bounds check, so
adding a kind without adding a grid read into the next alliance's block. It surfaced as an
alliance losing its VISION, which points nowhere near omni. There is an assertion now.


## ADR-038 — An opponent is a command source, and the port says so

**Context.** The opponent is `core/sim/BuildOrder.hpp` — a fixed build order plus one attack
wave — and `tickSkirmish` calls it by name. Two things already point past that. P4.2 routed
every input path through commands, so the script's orders and a player's right-click are the
same kind of thing. And `Events.hpp` was built as "the sim's outward voice", naming among its
intended consumers "eventually a Lua host". What is missing between them is an *interface an
opponent implements*: a second opponent today is a second call site in the assembly, the
scripted one cannot be switched off, swapped, or benchmarked against a replacement, and
nothing can consume the sim the way `Events.hpp` anticipated.

**Decision.** Name the port. An opponent observes, thinks, and emits commands:

    observe(const World&, std::span<const Event>)   // a read view, plus what happened
    advance(Tick)                                   // do this tick's thinking
    drain() -> std::span<const Command>             // what it decided

Three properties are load-bearing.

  - **It emits decisions, not mutations.** No pointers into the sim — the rule
    `BuildOrder.hpp` already states for the script ("the script holds no pointers into the
    sim"), promoted from a comment to the interface.

    *Not* "commands and nothing else", which an earlier draft of this ADR claimed and which
    the code disproves. Construction does **not** go through `applyCommand`: `runOpponents`
    pushes a `Construction` into `scene.building` directly and emits the
    `ConstructionStarted` event itself, with `Match.cpp` calling it "a known hole rather than
    a preference" — `Construction::blueprintIndex` indexes `scene.buildable` while
    `applyCommand` reads `catalog.def()`, two index spaces wearing one type name. So the port
    carries a decision type wide enough for what the opponent actually decides, of which
    `Command` is one arm and "start this construction" is another. Narrowing that to `Command`
    is a separate change that has to close the index-space hole first, and is not a
    prerequisite for the port.
  - **`advance` returns.** Whatever happens inside — a tick function, a resumed coroutine, a
    worker pool — the port sees only "this tick's work is done". Control flow is the
    implementation's business, and that is what keeps the port small enough to be worth having.
  - **The implementation is chosen once, at match setup, from the content family.** Not per
    unit and not per tick, because a match is one family throughout.

`ScriptedOpponent` is implementation one: today's `BuildOrder`, moved behind the port, unchanged.

**Alternatives considered.** *Leave the script called by name* — rejected: it is precisely what
makes a second opponent a second call site, and what leaves the scripted opponent with no A/B
partner, so no AI work after it could be measured. *An observer interface where the opponent
mutates the sim directly* — rejected: it puts an unaudited writer inside the tick and breaks the
P4.2 invariant that an order reaches the sim through `applyCommand` and nowhere else. *A neutral AI object model (`AIUnit`, `AIOrder`)
for opponents to consume* — rejected: it is a third dialect nobody speaks, and every consumer
pays to translate into it. `World` is a read view over `UnitStore`, `SpatialGrid`, `Terrain` and
`Roster` — this engine's own vocabulary — and anything foreign is translated at its own adapter,
once. *Defer the port until an AI needs it* — rejected on cost: it is one interface plus one
existing implementation relocated, and it is roughly 80% implied by what `BuildOrder`, `Events`
and `Command` already are.

**Determinism: not required by the network, still required by the gate.** The opponent runs on
the machine serving the game and nowhere else, so ADR-030's lockstep contract never depends on
its reasoning — an opponent may draw random numbers or iterate a hash table without that being
a network concern. FAF's AI draws 101 random numbers and relies on it (`moveFirst = 'Random'`).

But an earlier draft went on to say the command log replays an opponent anyway, and **there is
no command log.** `Replay.hpp` is explicit: *"WHAT IS NOT HERE YET, deliberately: player
commands … there are no commands to log … what a `--play` match does is already a pure function
of its setup, so a hash log is a complete record of it today."* `make verify` re-simulates and
compares per-tick hashes. It passes because the scripted opponent is deterministic C++, and it
only passes for that reason.

So the real consequence, which is the opposite of what that draft implied: **a nondeterministic
opponent cannot be covered by the golden match.** That does not force an adapter to be
deterministic — it forces a choice, per opponent, between being deterministic and being outside
the strongest regression gate this project has. Seeding an adapter's RNG from the sim's stream
is therefore not the nice-to-have it was called: it is what buys a foreign AI a golden match at
all, and it costs the difference between calling `rand()` and calling the sim's stream, since
`Random` is one of the 84 globals an adapter must bind regardless.

**Consequences.** `World`'s shape becomes a real design surface: it must answer role/faction/tech
questions (`Roster.hpp`) and neighbourhood questions (`SpatialGrid`) without handing out mutable
handles, and every method added to it is a claim about what an opponent is allowed to know. The
opponent is *not* in the state hash, correctly — it is an input source, not simulated state.
The port costs nothing if no adapter is ever built.


## ADR-039 — Foreign AIs are adapted, never modified; FAF's is first

**Context.** Two mature AI codebases exist for the two content families this engine loads, and
both were measured on the checkouts here rather than estimated.

*FAF's AI* (`reference/FAF-fa`, `lua/AI` + `lua/aibrains` + `aibrain.lua` + `platoon.lua`):
83,006 lines across 174 files, calling 222 distinct engine entry points — 84 globals, 138
methods. Of 548 distinct methods called, 377 are defined inside the corpus itself and **33**
leak into external game Lua. Its dialect is the surprise: **zero** files use `#` line comments,
zero use `arg[]`, 49 sites use the 5.1 `#x` length operator, against 115 `table.getn` and one
`math.mod`. It runs on stock Lua 5.1 with three shims. `PLAN.md`'s Lua-5.0-fork argument was
measured on the *sim* corpus (1240 of 1414 files using `#` comments) and is correct there; it
does not transfer to this subset.

*Circuit/BARb* (`recoil-macos/AI/Skirmish/BARb`): 53,648 lines of C++ behind a C ABI of 596
callbacks, of which roughly 180 are reached — an estimate by name-matching, so approximate. It
builds its own threat map, influence map, pathfinder and metal manager, and wants none of ours.

**Decision.** Three parts.

*The hard rule: vendored AI source is never modified.* Upstream compatibility is the whole
point — if the FAF or BAR communities ship something better, it drops in — so **a change that
can only be made by patching the AI is a change to the adapter or to the engine, or it is not
made.**

A rule nothing enforces is a preference, so the storage mechanism is chosen to enforce it. The
trees are **fetched at pinned commits and gitignored**, never committed: `tools/fetch_ai.sh`
holds the pins and the path lists, `make ai` runs it, `vendor/ai/` is ignored, and
`vendor/README.md` and `vendor/NOTICE.md` are the only committed artefacts. There is then no
local copy in history for anyone to quietly edit — `--force` discards local state, and the
repository never disagreed with upstream to begin with. That is a stronger guarantee than a CI
diff and it costs a `.gitignore` line rather than 16 MB and 1,016 files of someone else's code
in the history, which is the same reason `.gitignore` already gives for `third_party/`. Bumping
a version becomes a one-line commit that is its own diagnosis when the adapter breaks.

*Adapters, not a shared AI model.* Each foreign AI gets one adapter implementing ADR-038's port,
translating this engine's `World`/`Event`/`Command` into that AI's dialect and back. Adapters
hold zero game logic — units, coordinate space, id mapping, enum mapping, nothing else. The
control-flow mismatch stays inside them: `FafLuaAdapter::advance` resumes the coroutines due
this tick and returns; a later `CircuitAdapter::advance` fires `EVENT_UPDATE` and returns. The
port never learns that Lua exists.

*FAF first, at accepted partial fidelity.* The premise, stated plainly because every estimate
here depends on it: **we do not fully know how Moho works, and we are not going to find out
before writing the adapter.** The engine is closed, the API has signatures without
implementations, and a binding written against a signature is a guess dressed as a function.
Waiting for certainty means never starting; the alternative is to bind what we can, run it, and
let the game tell us what is wrong. That is the plan, and it is a choice rather than a shortfall.

So the hypothesis under test is that **a game-agnostic port can serve a foreign AI without the AI
or the sim being modified** — not that the AI will play well. It will not, at first. The
adapter's first goal is an AI that *runs*, and its second is a report of what it is running badly.

**The first milestone is deliberately smaller than a match**, because "run it and see" against a
partly-implemented engine does not produce an AI that plays poorly — it produces one that dies in
the first seconds, and a single undifferentiated failure carries no information. The first
observable success is therefore: the brain initialises, forms one platoon, and issues one order
that the sim executes. Everything after that is the instrumentation report doing its job.

That makes instrumentation part of the adapter rather than a later debugging aid. Every one of
the 222 names is bound to *something* and carries a confidence tag — `known`, `guessed`, or
`stub` — and the adapter records call counts per name across a match. "Which parts of the AI are
not connected" is then a ranked report after one game, not an investigation: a `stub` with 4,000
calls is the next thing to implement, a `known` with zero calls is dead surface. Without this the
partial-fidelity strategy has no feedback loop and degrades into guessing about guesses.

What the FAF adapter needs: a Lua 5.1 interpreter; 84 globals and 138 methods bound over `World`;
33 shims for the external game-Lua methods (`SetTargetPriorities`, `HasEnhancement`,
`CreateEnhancement`, `EnableUnitIntel`, the `On*` economy callbacks); a deterministic coroutine
scheduler for `ForkThread`/`WaitSeconds`/`WaitTicks` (~225 sites each); and a threat map, which
FAF's AI expects the *engine* to own (`GetThreatAtPosition`, 122 sites) — unlike Circuit, which
builds its own.

Two existing assets are why FAF is first rather than merely possible. `ScenarioSave.hpp` already
reads the full marker table those behaviours are built on — 3,508 mass deposits, 1,114 defensive
points, 677 transport markers, 524 rally points, plus the retail navigation graph — and
`Roster.hpp` already answers `role + faction + tech -> blueprint`, which is the question both
dialects need and neither can express natively.

**Alternatives considered.** *Reimplement one AI natively against the port* — not rejected,
deferred: it stays possible by construction, since ADR-038 admits any number of implementations,
but it cannot be *evaluated* until a strong reference opponent exists to score against, and the
adapter is what provides one. *A "unified AI" playing both families* — **explicitly not a goal.**
The port must not foreclose it and nothing is designed toward it. An AI playing both adequately
would still be worse at each game than the specialist its community has tuned for a decade, and
how well it plays is an AI's entire value. *Patch the AI where patching is easier than adapting*
— rejected by the hard rule: it converts a tracked dependency into a fork, and forks rot.
*Circuit first* — the strongest alternative, and it rests on an asymmetry worth recording because
it does not go away. **Recoil is open, so its AI callbacks have a reference implementation we can
read**: `rts/ExternalAI/SSkirmishAICallbackImpl.cpp` is 5,554 lines implementing all 596, with the
complete name-to-implementation map at line 5263. `Unit_getPos` is not a guess — it is
`GetCallBack(id)->GetUnitPos(unitId)`, cheats branch included. A Circuit adapter is therefore
*translation against a readable spec*, and every discrepancy is attributable: Recoil returns this,
we return that. A FAF adapter is archaeology, and a failure there is ambiguous — bad port, or bad
guess at Moho. Circuit is the cleaner experiment.

It is nonetheless second, on relevance and content cost: the near target is a Supreme Commander
skirmish on `SCMP_009`, so FAF's is the AI for the game being built, while Circuit needs BAR
content loaded and a metal map parsed — `metalmapPtr` appears in this engine only as a comment in
`Smf.cpp:26`. The asymmetry is accepted with eyes open rather than argued away: we take the harder
experiment first because it is the useful one, and we lose the ability to attribute its failures
cleanly. The instrumentation above is what partially buys that back.

**The dialect measurement above was wrong, and running the corpus is what said so.** It tested
for the 5.0-isms `PLAN.md` had catalogued on the *sim* corpus — `#` line comments, `arg[]`,
`table.getn`, `math.mod` — found almost none, and concluded stock Lua 5.1. Loading the files
found five Moho extensions it never looked for:

| Extension | Reach | Status |
|---|---|---|
| `!=` for `~=` | 108 uses, 39 files | rewritten in memory by the adapter |
| `{&1 &0}` table preallocation hints | 14 constructors | blanked; an optimisation with no observable semantics |
| bitwise `&` `\|` `<<` `>>` | 48 files | **native in 5.4**, absent from 5.1 |
| `continue` as a statement | 25 of 208 files | **unsolved** — needs `goto`, which 5.1 lacks |
| implicit `arg` in vararg functions | 15 sites, incl. `class.lua` | **unsolved** — removed in 5.2, no compat switch in 5.4 |

So **the VM is Lua 5.4, not 5.1**, and the reason it is cheap to say so is the other half of the
measurement: `setfenv`/`getfenv` — the change that breaks most 5.1 code on 5.2+ — appear **zero**
times in this corpus. Bare `unpack` (43 sites) is one shim line. 5.4 brings bitwise operators
natively and `goto` to lower `continue` onto, so it fixes one blocker outright and makes the
other solvable.

None of this touches `vendor/ai/`. The rewrites happen on the buffer between reading a file and
compiling it, so the tree stays byte-identical to upstream and the hard rule holds — a transform
in memory is the adapter's business, a patched file on disk would be a fork.

**Where it stands.** 254 names bound before any AI runs; `import` real and caching (592 call
sites, the one binding that could not be a stub); `lua/system/class.lua` loading, which is what
the corpus's whole object model is built on. The two unsolved extensions are what stand between
here and the rest, and they are named rather than worked around, because a silent workaround
would be worse than a blocker with a number attached.

**The largest single risk is the threat map**, and it is named here so it is not rediscovered as
a schedule slip. `GetThreatAtPosition` has 122 call sites; FAF's AI assumes the *engine* owns a
threat model, and no annotation stub says what it returns. Circuit's own implementation is not a
reference, because it answers a different engine's question. Every other item on the FAF adapter's
bill is bounded work against a known signature; this one is a design problem with no ground truth,
and if any single thing overruns its estimate it is this.

**Consequences.** A Lua VM enters the build. `LuaTable.hpp` declines an interpreter for *data
parsing* and that stands — the data parser is untouched and this overturns nothing except for
the AI sandbox, which is a different justification for a different job. If Circuit is adopted
later, two threat maps will exist, because it declines to use the engine's; accepted, they are
not the same object. FAF-fa carries no licence and is vendored as-is by explicit decision, with
provenance recorded rather than asserted; this ADR is the record that the question was asked.
The adapter is roughly **255 translation points** (84 + 138 + 33), not a shim — planning that
calls it a shim will underestimate it by an order of magnitude. Because a missing Lua binding
fails mid-match rather than at build time, the adapter asserts at load that every one of the 222
names in `FAF-fa/engine/`'s annotation stubs is bound to *something*, stub included — turning a
forty-minutes-in nil-call into a startup error, without pretending a bound stub is a working one.
Every pin is verified, and verifying them is what caught the one thing worth knowing. Each tree
was measured on a local checkout, then re-fetched from upstream and compared: FAF returns the
same 257 files; CircuitAI is byte-identical, and `git fetch --depth 1 origin 0ef3626` would fail
outright if upstream lacked that commit, which is what turned a plausible hash into a proven one;
Recoil's `rts/ExternalAI/Interface/` matches, so the pin names the same 596 callbacks this ADR
counts. **`AI/Wrappers/` does not match** — three files differ and upstream no longer ships
`JavaOO` — and the difference is in the *local* Recoil checkout, which carries four macOS build
commits over an unversioned import. Upstream is the clean tree and upstream is what is fetched,
so a Circuit adapter behaving differently here than the local Recoil build does would be the
local build's doing. `vendor/NOTICE.md` keeps the details.


## ADR-040 — Screen input arrives in the space the interface is laid out in

**Context.** The interface is laid out in backing PIXELS: `Window::width()` returns the
drawable's size and says so — "the space the interface is laid out in" — and the text shader
divides a vertex by exactly that. Mouse input arrived in POINTS, which on a Retina display are
half of it. `MouseModifiers::pointX` carried that value under a comment claiming it was "the
same space the HUD lays out in", so the two doc comments in the same subsystem asserted
different things and the code followed the wrong one. Every consumer compares this against a
layout built from `width()`/`height()`, so on this machine a minimap click had to land in the
lower-left quarter of the minimap's own rectangle to register (P7.4 shipped that way), and the
build panel's cells inherited it.

**Decision.** Screen positions handed to app code are in **backing pixels, top-left origin** —
one space, the interface's own. `Window` converts once, at the boundary, and states the space
at every field that carries one. `Window::cursor()` returns the same space for the same reason.

The **world ray keeps points**, because `screenRay` divides by the view's bounds and the two
must agree. That the two coexist is why each is now named at its definition rather than left to
be inferred from a call site.

**Alternatives considered.** *Lay the HUD out in points* — rejected: the shader divides by the
drawable, so the HUD would have to be scaled back up somewhere, and "somewhere" is how this
happened. *Convert at each consumer* — rejected: it is the same conversion at every hit test,
and the one that already existed was the one that was missing.

**Consequences.** A hit test may be compared directly against a layout with no conversion, which
is what makes "drawn in one place, clicked in another" a bug that cannot be written by accident
rather than one caught by looking. The conversion itself lives behind AppKit and `rm_tests` does
not link it, so it is not directly testable; `tests/test_build_panel.cpp` pins the relationship
that makes a mismatch fatal — at twice the drawable size every cell is somewhere else, so a
point good in one space is a miss in the other rather than merely imprecise.

## ADR-041 — The drawable's size is the one source of the viewport, and the view keeps it true

**Context.** Resizing the window slid every bottom- and right-anchored panel — minimap, build
tray, roster, clock — off the visible area. The HUD was laid out against
`Window::width()`/`height()` (then: layer frame × `backingScaleFactor`) while the shader mapped
those vertices through the drawable texture's own size, and the two disagreed after any resize.
Measured cause: **`CAMetalLayer` computes `drawableSize` once, at the first `-nextDrawable`,
and never follows a later frame change** — shrink the layer from 1600×900 to 900×500 and the
next drawable is still 3200×1800. MTKView resyncs it on every resize precisely because the
layer will not; a raw layer-hosting NSView has to do the same by hand, and this one did not.

**Decision.** Two halves, both needed. `RMTerrainView` overrides `setFrameSize:` and
`viewDidChangeBackingProperties` to re-derive `drawableSize` from bounds × `contentsScale`
(and to re-match `contentsScale` to the screen, for a window dragged between displays), with
one explicit sync at construction because the value is 0×0 until somebody computes it. And
`Window::width()`/`height()` now read `drawableSize` itself rather than recomputing it from
the frame — the layout space and the shader's viewport are one value read from one place, so
they cannot disagree by construction, whatever AppKit does between frames.

**Alternatives considered.** Keeping frame × scale as the report and only adding the sync —
rejected: it leaves two computations of the same quantity agreeing by luck, which is exactly
what just failed. An `MTKView` — rejected long ago for the display-link and ownership reasons
in `Window.mm`; adopting it for this alone would trade a two-method fix for a re-plumbing.

**Consequences.** A live-resize frame may still lay out against a size one frame stale — the
next frame corrects it, invisible in practice. Verified end to end by resizing the running app
through the same `setFrameSize:` path a manual drag takes; not unit-testable, as `rm_tests`
does not link AppKit (ADR-040's caveat).

## ADR-042 — The build ghost is a batch with no units, drawn in the interface's light

**Context.** Placing an armed build showed only a ring — the sim's own test is a disc
(`sitePlaceable`), but a circle says nothing about WHAT would stand there. The model is the
answer, and the blocker was structural: the only way to get a blueprint's model on screen was
to spawn a unit, and `setUnits` skipped any batch with no instances, so "registered but not
built" could not reach the GPU at all.

**Decision.** Three parts. `ensureDrawableType` factors the drawable half out of `spawnUnit`
(model, textures, batch, traits — no unit), called when a build cell is armed. Empty batches
now UPLOAD with capacity for one instance, and the draw loop skips them by instance count —
so a model can exist on the GPU with nothing standing. The ghost itself is renderer state
(`setGhost`/`clearGhost`, sticky unlike the per-frame lists): one instance drawn through the
unit vertex stage with a flat translucent fragment (`unitGhostFragment`) in the ring's own
two colours, depth-tested against the world but writing nothing, after the decals and
skipped in the reflection pass.

**Alternatives considered.** Tinting via the team-colour mask — rejected: the mask covers
only livery regions, so most of the hull would stay painted metal and read as built. A
footprint rectangle decal — deferred, deliberately: the sim tests a disc today, and a
rectangle would promise a precision the placement does not check. Spawning a real unit and
deleting it — the structural hack this replaces.

**Consequences.** Arming a new blueprint grows the batch list; the ghost draws one frame
after the re-upload notices (the renderer draws nothing for an unmapped batch, so the gap is
invisible). Every armed-once blueprint keeps an uploaded model for the session — bounded by
the build menu's size. `--ghost X Z` captures it headlessly, per the screenshot-or-it-
didn't-happen rule.

## ADR-043 — The FAF opponent: the corpus decides, the adapter places

**Context.** Stage D of the AI integration (item #28904): make FAF's AI actually play, given
a sandbox that loads the corpus but hosts no managers, and a sim whose brain/unit binding
surface is mostly stubs.

**Decision.** `FafOpponent` behind the ADR-038 port, with the split stated in its header:
FAF's data and code decide WHAT (builder specs walked by priority, their conditions evaluated
by the corpus's own `/lua/editor` functions over a per-tick snapshot), the adapter decides
WHERE (site helpers shared with the scripted opponent) and converts to `Decision`s through
`applyCommand`. The manager stack is stood in for by a serialized build queue — one structure
underway per builder unit — and every brain method a condition wants and lacks fails closed
and is counted (`BRAIN METHODS MISSING` in the sanity report). Lua's string-hash seed is
pinned so `pairs` order cannot desync a match. Behind `--ai-faf`; the scripted default and
its golden log are untouched.

**Alternatives considered.** Booting base-ai.lua's real manager stack — rejected for now:
it needs unit objects, per-unit callbacks and an economy-event surface that would all be
Guessed bindings; the ledger-driven driver gets the corpus's judgement running first and
tells us which of those bindings to build next. A C++ reimplementation of the builder logic —
rejected by ADR-039's premise.

**Consequences.** The AI plays a real opening (mex expansion, power, factories, tech,
raids) but with adapter-shaped tactics: one base location, headcount threat, attack target
picked by the adapter. Each is a named stand-in to be replaced by the real corpus mechanism
as its bindings land.

## ADR-044 — miniz is the archive dependency, ratified

**Context.** miniz (third_party/, public-domain, single-file) was added mid-session for
`.sdz`/`.scd` zip extraction and never justified in an ADR — item #1588's standing complaint.

**Decision.** Keep it, now on the record. The VFS mounts zip archives on every content path
this engine reads (17 archives, 23k files for retail FA alone); miniz is ~5k lines, no
transitive dependencies, fetched pinned like Lua and Catch2 rather than committed.

**Alternatives considered.** libzip/zlib — real dependency trees for the same job.
Reimplementing inflate — a solved problem this project has no business re-solving.
Requiring pre-extracted content — pushes a chore onto every user of every archive forever.

**Consequences.** A third-party C file in the trust base, mitigated by pinning to a release
checksum'd by its own project. The build stays curl-and-go.

## ADR-045 — Aggressive movement keeps its waypoint and borrows a target

**Context.** Attack-move and patrol must interrupt movement for combat without forgetting the
route, seeing through fog, or adding a second hidden order queue.

**Decision.** `AttackMove` and `Patrol` remain ordinary serialized commands. Their position is
the permanent waypoint; their generational `target` temporarily names a visible hostile chosen
after movement and intel update by the existing deterministic combat query. Losing, killing, or
failing to route to that target clears only the handle and resumes the waypoint. Patrol is a
deque of those commands: arrival rotates the head to the back, and the first click adds the
unit's starting point as the return leg.

**Alternatives considered.** Hidden attack suborders — rejected because replay and state hashing
would need another state machine. Reusing targeted `Attack` — rejected because its chase rewrites
the destination, destroying the route that must resume. Acquiring before intel — rejected as a
fog oracle.

**Consequences.** Existing command logs and queue hashing cover the mechanic without a new data
type; old command-kind ordinals remain fixed. Unarmed patrols still cycle, minimum-range weapons
do not stop in their dead zone, and a patrol with fewer than two points dissolves instead of
spinning forever. A patrolling builder performs at most one service action already inside build
reach: repair first, then reclaim, without leaving the route or creating an internal order.
Repair uses native `maxHealth × work / BuildTime`; explicit reclaim keeps priority over autonomous
cleanup, and full storage suppresses the latter rather than destroying value.

## ADR-046 — Air is a direct movement layer at fixed terrain clearance

**Context.** The corpus already identifies `RULEUMT_Air`, but the app discarded its no-ground-
grid meaning: aircraft used landing speed, ground A*, terrain height, slope alignment, and ground
collision. The T1 air slice needs flight without importing a full takeoff/landing model.

**Decision.** Air commands bypass A* and route directly in X/Z. `MoveState::airborne`, derived
from the unit definition at spawn, lets the hot movement and collision passes keep aircraft 80
elmos above local terrain and level; 80 is Recoil `AAirMoveType`'s sourced default clearance.
Air and ground do not separate, while aircraft sharing that altitude still do. FA aircraft speed
comes from `Air.MaxAirspeed`, falling back to `Physics.MaxSpeed` only when absent.

**Alternatives considered.** An all-passable air grid — rejected because it still bends flight
through cell centres and pays for A*. A full fixed-wing mover — deferred: banking, takeoff,
landing, fuel, and strafe runs are distinct mechanics. Radius zero — rejected because radius also
controls hits, picking, wrecks, and death retirement.

**Consequences.** Flight is deterministic and intentionally simplified: fixed clearance follows
terrain vertically and aircraft may stop or pivot in place. The airborne bit is derived state and
is hashed only when true: historical ground hashes stay stable while a broken aircraft spawn
invariant still changes the lockstep checksum.

## ADR-047 — Weapons carry a two-layer target mask

**Context.** FA's T1 interceptor and bomber weapons already describe what they may hit, but the
engine discarded `FireTargetLayerCapsTable` and the AIR restrictions. Acquisition, beams,
projectile collision, and splash therefore treated aircraft and surface units as interchangeable.

**Decision.** Select the `Air` source row for aircraft, collapse all other source rows and FA's
Land, Water, and Seabed destinations into the current sim's Surface bit, and retain Air as the
second destination bit. A weapon defaults to both only when content has no FA cap table. The
resolved mask gates every target choice and damage path, travels with projectiles, and is hashed
when it differs from the historical surface-only projectile state.

**Alternatives considered.** Weapon role alone was rejected because retail target caps are more
specific and dual-purpose weapons exist. Full category-expression targeting was deferred because
the approved air slice needs only two physical layers, not FA's complete target-priority language.

**Consequences.** Interceptors attack aircraft, bombers and ordinary guns attack the surface, and
shots cannot leak into the wrong layer through collision or splash. Water and seabed remain one
surface domain until naval depth becomes a simulated movement layer.

## ADR-048 — Ordinary shields intercept at the shared damage boundary

**Context.** Retail FA authors ordinary bubble capacity, diameter, vertical offset, regeneration,
collapse recovery, and energy upkeep, but every damage path previously went straight to hulls.
Beams, projectiles, and death blasts already converge on `damageArea`.

**Decision.** Parse non-personal `Defense.Shield` bubbles and derive their rates in `UnitCatalog`.
Keep current power and recovery timers with `Health`. Before hull falloff, `damageArea` lets the
lowest-slot hostile active sphere containing the impact absorb one damage profile; overkill leaks
proportionally to hulls. Partial shields regenerate after the corrected FA delay, collapsed ones
return full after the corrected authored recharge, and existing unit upkeep supplies energy drain.

**Alternatives considered.** Projectile-only dome collision was rejected because beams and death
blasts would bypass it. A separate shield component array was unnecessary while shield state is
part of unit survivability. A dedicated Metal shield pipeline was unnecessary: the existing
blended, depth-tested world-triangle pass can draw a low-poly translucent sphere.

**Consequences.** Focus fire collapses bubbles, protected hulls survive until then, and shield
state is deterministic, hashed, and visible as a cyan bar, impact flash, and external sphere.
Personal, transport, enhancement, energy-stall, overlap-overspill, toggle, and special-nuke rules
remain explicit future work rather than approximations hidden in this slice.

## ADR-049 — Assist is a standing unit order and a derived construction rate

**Context.** FA lets engineers guard a builder or factory and lend BuildRate to its work. Here a
`Build` command is instantaneous and constructions outlive the founder's command queue, so an
assister cannot discover the target's project from its current order.

**Decision.** Each construction records the `UnitId` that founded it. A targeted, queueable
`Assist` order follows a distinct same-army builder, holds at the helper's build reach, and each
tick contributes to that target's oldest unfinished construction. Contribution is recomputed from
live orders and positions before economy funding; founder plus helpers drives both resource demand
and progress.

**Alternatives considered.** Keeping `Build` resident until completion would rewrite existing
construction and factory semantics. Storing helper lists on constructions would require death and
retask bookkeeping. Free acceleration was rejected because it removes assist's economy decision,
and position-matching a helper to a site cannot reliably identify whose project it is helping.

**Consequences.** Assist is replayable, cancellable, survives an idle target, rolls to the next
factory project, and stops immediately when its order, range, helper, or target ceases to qualify.
Stalls slow the combined rate through the existing shared funding fraction. Founder identity and
the derived contribution are hashed; recording founders on historical builds intentionally moves
the golden state stream even in matches that issue no Assist order.

## ADR-050 — Surface navy uses an inverse water domain

**Context.** Treating every non-air unit as ground-bound placed ships on the seabed and let an
amphibious commander found naval yards on land. Submarine depth and sonar remain unsimulated.

**Decision.** Surface ships and naval factories use a deterministic inverse passability grid whose
cells lie wholly below the map waterline. Build validation follows the product domain, including
queued and replayed orders; ordinary immobile structures retain the builder-grid fallback.

**Alternatives considered.** Amphibious routing and direct movement toward a shoreline were
rejected because both permit land occupancy. A full depth-cost field was deferred with submarines.

**Consequences.** Surface ships spawn and collide at the waterline, route only through connected
water, and approach land objectives at the nearest reachable water cell. Submersibles remain
refused rather than approximated as surface craft.

## ADR-051 — The HUD uses logical points; the world uses drawable pixels

**Status.** Superseded by ADR-057 for HUD coordinates and ownership; its world-drawable rule remains.

**Context.** The pixel-space HUD fixed mismatched hit tests but made every module physically half
as large on a Retina display and made responsive breakpoints depend on backing scale.

**Decision.** AppKit input and all HUD geometry use top-left-origin logical points. Metal keeps the
world viewport in drawable pixels; the UI shader receives its own point viewport, while fonts are
rasterized at backing scale and report point-sized metrics. One tested frame chooses Compact,
Standard, or Wide and owns every bounded module rectangle.

**Alternatives considered.** Converting every input to backing pixels preserves one coordinate
space at the cost of display-dependent UI size. Uniformly scaling one fixed frame cannot preserve
both readable controls and useful battlefield area across supported windows.

**Consequences.** Drawing and hit testing share stable geometry across 1x/2x displays; moving
between displays rebuilds only the font atlases. Headless captures remain 1x. This supersedes
ADR-040's pixel-layout decision and ADR-041's use of drawable size for HUD layout; drawable sync
remains authoritative for world rendering.

## ADR-052 — Per-frame GPU lists grow; per-frame growth checks are held across frames

**Context.** Two silent losses, both invisible to a green suite because nothing links the renderer
into the test binary. `setInstances` clipped at each batch's upload-time instance count, so the
second unit of a type was never drawn until an unrelated batch-list growth forced a full
re-upload. And the frame loop captured `batches.size()` at the top of the frame and compared at
the bottom, so a batch created LATER in the frame — which is exactly when the build ghost resolves
its model — was never uploaded at all: the next frame's "before" already counted it.

**Decision.** Instance buffers reallocate when a batch outgrows them, doubling, with the old buffer
retired against a frame counter and freed once `kMaxFramesInFlight` frames have opened. The
upload-on-growth check reads a counter held OUTSIDE the frame callback and runs twice: after the
gather, and again after the ghost and construction passes have had their chance to load a model.

**Alternatives considered.** Re-uploading every batch whenever any one grows is correct and walks
every model and texture to do it. Pre-sizing every batch to a worst case wastes the memory of the
largest army on the smallest. Neither addresses the growth-check bug, which is about ordering.

**Consequences.** A capture now reports `units: N instance(s) drawn, M alive in the sim`; the two
disagreeing is the signature of a dropped instance, which is the only instrument available while
the renderer has no tests.

## ADR-053 — The interface is laid out small and drawn big

**Status.** Amended by ADR-057: HUD geometry now has its own authored-point space, conversion
lives in `UiViewport`, and requested magnification is capped by available fit.

**Context.** Every HUD metric is a constant in points, and the responsive frame spent a larger
viewport on MORE COLUMNS — nine cells across 528 points at 1280 wide, sixteen across 893 at 2240,
the cell itself shrinking from 54 points to 52. The interface therefore got physically smaller
relative to the screen the larger the screen was, and a HiDPI panel measuring under 1600x900
logical points received the smallest profile of the three.

**Decision.** `ui::hudScale` returns how far the viewport is from a 1280x720 design resolution,
clamped to 1..2.5 and multiplied by the player's `--ui-scale`. Layout happens in that design space;
the renderer's HUD projection maps the design viewport across the whole drawable, which magnifies
the result. Input divides by the same scale, fonts rasterise at `backing x hudScale` and report
design-point metrics, and the column counts came down (6/7/9) so a cell can hold a NAME.

**Alternatives considered.** Multiplying every metric at every layout site touches four files of
arithmetic and leaves each new constant free to forget. A pure font-size setting fixes the text and
leaves the panels the size they were.

**Consequences.** The whole HUD vertex stream lives in one space, so world-projected overlays —
health bars, strategic icons, contact blips, the band-select box — are fed the design viewport too
and scale with the chrome. `Window::cursor` stays in logical points for picking; `hudCursor` is the
divided one, and using either for both puts the pointer off by exactly the scale factor.

## ADR-054 — A construction is drawn from the economy's ledger, per faction

**Context.** Nothing existed in the world between ordering a building and its completion: a
`sim::Construction` is a row in the economy and `spawnUnit` runs only on completion, so placing a
silhouette produced a mass drain, a console line, and bare ground. The reported symptom was
"when clicking silhouette on ground I don't see command was building it".

**Decision.** The app translates `scene.building` into draw records each frame — the product's
model, its progress, the builder's faction — and a dedicated pass reveals the model in step with
the work. The four faction effects share one pipeline and one uniform block and differ in the
SHAPE of the reveal: UEF a rising plane, Cybran a hashed dissolve, Aeon a rise from a pad with the
unbuilt part drawn as translucent light, Seraphim a sweep about the site's vertical axis. Pads and
build streams reuse the existing decal and particle passes.

**Alternatives considered.** Spawning the unit at build start and scaling it up puts a real object
in the sim for presentation's sake and changes collision, targeting and the state hash. A generic
scaffold for all four is less work and discards the one thing the request was specifically about.

**Consequences.** `scene.building` is a LEDGER, not a queue — finished rows stay so the census can
count standing structures — so the gather must filter on `constructionInProgress`; drawing every
row put a permanent half-built shell on every completed building. The reveal uses
`UnitDef::meshHeightElmos` (`Physics.MeshExtentsY`), not the collision height: a UEF land factory
is `SizeY = 0.6` against `MeshExtentsY = 4.5`, and the collision box is the apron rather than the
gantry.

## ADR-055 — Sight is Forged Alliance's flat disc by default, and the raycast reads sensor height

**Context.** `VisionStyle` defaulted to Recoil's terrain raycast on the argument that blocked sight
is the more interesting game. This engine reads Forged Alliance's content, and that game's sight is
`radius * vertex.xz + position.xy` — a flat disc, with no heightmap sample anywhere in
`effects/vision.fx`. Separately, the raycast was handed the emitter's transform as its eye, which
is the ground under its feet: every unit in the game looked out from ankle level.

**Decision.** The default is `VisionStyle::ForgedAlliance`. The raycast stays as `--vision-style
recoil` and is now genuinely the advanced model: `UnitCatalog::IntelRadii::eyeHeight` carries each
type's `SizeY`, so a commander sees from its own head.

**Alternatives considered.** Keeping the raycast default and only adding eye height leaves a
transcription playing a different game from the content it transcribes.

**Consequences.** Both halves change what armies can see and therefore the match; the golden log
was re-recorded deliberately. `SizeY` is the collision box and stands in for a sensor mount, which
the blueprints do not state — the mesh height is a different field and belongs to ADR-054.

## ADR-056 — Every blended pipeline declares straight or premultiplied alpha

**Context.** HUD text and images premultiplied RGB in their fragment shaders, while the shared
Metal pipeline multiplied RGB by source alpha again. Translucent UI was therefore dimmed twice,
and every blended pipeline also multiplied the framebuffer's alpha contribution twice.

**Decision.** Pipeline creation takes `Opaque`, `StraightAlpha`, or `PremultipliedAlpha`. Straight
RGB uses `SourceAlpha`; premultiplied RGB uses `One`; both use `One` for source alpha and
`OneMinusSourceAlpha` for the destination. CPU HUD colours and image texels remain straight;
their shaders premultiply, while decals, ghosts, and construction effects remain straight.

**Alternatives considered.** Premultiplying every CPU vertex and uploaded texture would spread the
contract across content loaders and UI builders. Returning straight output from every shader would
discard the particle pipeline's useful translucent/additive representation.

**Consequences.** Minimap fog now premultiplies its tint, particles share the common pipeline
builder, and compile-time assertions pin both blend equations. Existing translucent HUD colours
render brighter because they now contribute exactly once rather than by alpha squared.

## ADR-057 — One viewport value owns every UI coordinate conversion

**Context.** Logical extent, HUD magnification, backing scale, shader viewport, safe content, and
capture scale were derived through separate APIs. Their formulas agreed, but resize, display-move,
input, and capture paths could update only part of that contract.

**Decision.** `UiViewport` carries AppKit extent, backing scale, safe content, and the resulting
HUD magnification. It derives HUD extent, input conversion, drawable extent, and font raster scale.
Layout anchors fixed modules inside safe content; constrained width contracts the build palette
and selection bay rather than crossing modules, and the palette reduces columns before cells become
invalid. World overlays project through the full HUD extent. Window and capture paths pass the same
value to layout and Renderer before obtaining font views.

**Alternatives considered.** Keeping `hudWidth`, `hudCursor`, `setUiScale`, and `setHudViewport`
preserves smaller local edits but leaves correctness dependent on callers invoking matching pairs.

**Consequences.** A 1280x720-point 2x window and a 2560x1440-pixel 1x capture derive identical HUD
geometry and font raster scale; larger same-pixel pairs may differ after the automatic 2.5x cap.
Mouse events remain in top-left AppKit logical-point space until one explicit conversion, and drag
thresholds remain in that same logical space. Display scale changes rebuild font atlases without
changing their authored metrics. Requested user magnification cannot exceed available fit, and an
undersized capture scales Compact down instead of collapsing its geometry. This consolidates
ADR-051 and ADR-053's coordinate rules and replaces ADR-053's separate HUD accessors.

## ADR-058 — UI geometry is partitioned by semantic compositing layer

**Context.** HUD geometry was grouped by whichever texture could draw it: label-font solids and
glyphs, readout-font solids and glyphs, or interface-atlas images. Draw order was therefore an
accident of material, and one shared 6,000-vertex upload ceiling could silently remove unrelated
visuals when any producer became busy.

**Decision.** Producers submit six back-to-front semantic layers: world overlay, panel surface,
chrome, icon, label, and foreground readout. Each owns a fixed 1,000-quad partition in the
triple-buffered upload. Mixed solid/image layers share their partition deterministically, only
whole quads upload, and the renderer reports submitted, uploaded, and dropped vertices per layer.
Solids use a texture-independent fragment function rather than borrowing a font-atlas texel.

**Alternatives considered.** Sorting individual quads by z preserves one stream but adds keys and
per-frame work to an interface whose order is static. Dynamically growing one shared buffer avoids
fixed limits but can still let labels displace icons and complicates buffers already in GPU flight.

**Consequences.** Material changes can add draws inside a layer but cannot reorder semantic roles.
Overflow in one role cannot evict another; debug builds assert, release builds truncate safely and
warn once, and screenshot telemetry exposes the actual frame counts. The fixed budget grows
from 6,000 to 36,000 vertices per frame, about 3.3 MiB for all three ring slots.

## ADR-059 — Game-interface profile is explicit run state

**Context.** `--ui faf` mutated one process global before startup, while atlas packing mutated a
second global that theme resolution read later. A fresh capture could therefore build panel
geometry before the packed nine-slice skin existed, and neither path could represent BAR or neutral
presentation without adding more hidden switches.

**Decision.** A closed `GameProfile` value (`Fa`, `Bar`, `Neutral`, `ClassicFaf`) is parsed once and
carried by `Session`. Atlas packing returns its texture and optional panel skin together; callers
pass that skin explicitly to profile-aware theme resolution. `fa` remains the default and `faf`
remains the classic-chrome alias.

**Alternatives considered.** A profile resolver or plugin interface invents extension machinery
before a fifth profile exists. Inferring BAR from loaded model formats is incorrect for mixed-content
scenes and makes presentation depend on whichever asset happened to load first.

**Consequences.** Captures and the windowed loop have the same data flow and no UI skin globals.
FA and classic FAF keep faction accents; BAR and Neutral deliberately use neutral materials, while
their distinct resource names live in the fixed profile-owned views of ADR-060. Selecting a profile
is deterministic and independently testable without retail assets.

## ADR-060 — Economy presentation is a fixed pair of resource views

**Context.** The HUD renderer reached into `MatchState::mass` and `MatchState::energy` and wrote
their labels itself. BAR calls the construction material Metal, while a game-neutral presentation
cannot honestly call it either game's resource.

**Decision.** `MatchState` carries exactly two ordered `ResourceView` values: the profile names the
first Mass, Metal, or Material, and the second remains Energy. Each view owns its gauge and tint,
so the renderer iterates presentation data without knowing simulation field names.

**Alternatives considered.** An arbitrary resource registry or plugin API solves games this engine
does not support. Letting panels translate raw simulation resources repeats profile policy in every
future panel.

**Consequences.** The two-resource simulation is unchanged. FA and classic FAF present Mass then
Energy, BAR presents Metal then Energy, and Neutral presents Material then Energy; order and count
are compile-time fixed and independently tested.

## ADR-061 — AppKit emits one complete logical key event

**Context.** Keyboard input crossed the platform boundary through a repeating key-down callback, a
second press/release callback, a character-keyed held set, and live modifier polls. A consumer had
to combine facts sampled through different APIs, and every binding compared raw characters.

**Decision.** AppKit translates each bound layout-aware character once into `KeyEvent`: a small
logical `Key` enum plus press/release phase, AppKit repeat state, and Shift/Command/Control. One
callback receives both phases; held movement polls the same enum-keyed set. Mouse band selection
continues polling live Shift because it completes from per-frame mouse state, not a key event.

**Alternatives considered.** Hardware key codes would silently switch to physical-key bindings and
change behavior across keyboard layouts. Keeping separate tap and held callbacks preserves two
partial event models and still leaves modifiers out-of-band.

**Consequences.** Existing bindings and repeat behavior are unchanged, while application code has
no raw key-character comparisons. Every future keyboard consumer must handle phase, repeat, and
modifiers from one event rather than reconstructing them from AppKit state.

## ADR-062 — The command rack keeps Forged Alliance's stable order positions

**Context.** The HUD reserves a command rectangle but has no view data for it. Compacting whichever
commands a selection supports would make Move, Stop, and Assist jump between slots as selection
capabilities change, while inventing buttons for absent mechanics would promise gameplay that does
not exist.

**Decision.** Use a fixed four-column by three-row descriptor table based on FAF's `preferredSlot`
order: AttackMove, Move, Attack, Patrol, Stop, Assist, reserved fire-state, Overcharge, three
unit-specific reserves, and Reclaim. Build remains in the construction panel. A separate parallel
array reports enabled state; mixed selections enable a semantic when any selected unit can execute
it, matching existing per-handle order dispatch.

**Alternatives considered.** A compact list causes positional reflow. A profile-specific command
registry invents extension machinery before profiles differ here. Placing Build in a spare slot
duplicates the blueprint palette and has no Forged Alliance order-rack counterpart.

**Consequences.** Selection changes can only alter disabled state, never common-command positions.
Unimplemented canonical positions remain visible to future rendering as disabled reserves, and this
slice adds no command execution, click handling, or gameplay capability.

## ADR-063 — One explicit selected builder owns the build panel

**Context.** `gatherBuildOptions` silently chose the first build-capable unit in a selection on
every frame. The panel header and build dispatch shared its returned handle, but there was no
active-builder state to preserve that choice when the same selection arrived in a different order,
and no testable boundary between discovering candidates and choosing one.

**Decision.** Gather live build-capable handles in selection order, retain the exact current handle
while it remains among them, and otherwise fall back to the first candidate. Pass that handle
explicitly into option gathering; the returned `BuildSelection::builder` remains the sole handle
used by factory, upgrade, and placed-build dispatch.

**Alternatives considered.** Intersecting every builder's options empties useful menus in mixed
selections. Sorting candidates discards the player's deterministic selection order. Re-choosing the
first candidate every frame preserves old behavior but leaves panel ownership implicit and unstable.

**Consequences.** Reordering an unchanged mixed selection no longer changes whose menu is shown.
Death, deselection, or filtering of the active builder chooses the next candidate deterministically;
no candidates clear the panel and invalidate the active handle. This adds no builder-cycle control.

## ADR-064 — Panel pages belong to the context that gives them meaning

**Context.** The window kept one build-page counter and one roster-page counter. Switching builders
could either erase useful position or apply a page from an unrelated menu, while a changed selection
could inherit a roster page whose tiles no longer represented the same units. The future command
rack has the same ownership question across interface profiles.

**Decision.** Keep build pages by `UnitTypeIndex`, command pages by the closed `GameProfile`, and the
roster page with its exact ordered generational selection identity. An identity change resets only
the roster page. `buildPanelLayout` and `rosterLayout` still clamp requested pages against current
content and viewport capacity, and the window writes those clamped values back to the owner.

**Alternatives considered.** One global counter loses context. Keying builds by unit handle makes two
units of one type remember inexplicably different menus. Hashing the roster risks collisions where
exact comparison is cheap. A generic panel registry adds indirection for three fixed instruments.

**Consequences.** Returning to a builder type or interface profile restores its previous page;
changing selection order, membership, or a handle generation returns the roster to page zero.
Content shrinkage and viewport changes remain layout concerns rather than being duplicated in state.

## ADR-065 — Veterancy is recomputed from the blueprint, never accumulated

**Context.** Retail Forged Alliance recomputes every buff from the unit's blueprint value and
declares its veterancy buffs `Stacks = 'REPLACE'`, so only the current level's buff is ever
applied (`lua/sim/Buff.lua`, `BuffCalculate`). Level 3 health is `base x 1.3`. An implementation
that applied each level's multiplier as the unit was promoted would reach 1.1 x 1.2 x 1.3 = 1.716,
32% too much health, and would read as a balance complaint rather than as a bug. Promotion also
heals: retail raises the maximum and then adds the increase to current health.

**Decision.** Store `kills` and `level` per unit and derive the maximum from the blueprint each
promotion, `veterancyMaxHealth(blueprintMax, level)`. Scale as an exact integer ratio on the raw
fixed-point value rather than by an `Fx` multiplier, because `Fx::fromRatio(11, 10)` is 18022/16384
and turns 1,000 health into 1,099.976. Kill credit is awarded from `retireDead`, which is where
retail calls `OnKilledUnit` — before the death weapon and before the wreck. Thresholds and the
regeneration ladder come from the blueprint's `Veteran` and `Buffs.Regen` tables, defaulting to
`Game.VeteranDefault`.

**Alternatives considered.** Accumulating multipliers per promotion is simpler and wrong, as above.
A global threshold constant was implemented first and rejected on measurement: 193 of 568 shipped
blueprints override the thresholds and 192 override the regeneration, most by large factors — an
interceptor promotes at 2 kills, not 25. Converting the veteran regeneration bonus at content load
was impossible because it depends on the unit's level rather than its type, so it is derived inside
the tick in whole numbers, which keeps floating point out of the sim.

**Consequences.** Veteran health and regeneration match retail exactly, including that a veteran
hits no harder — retail's per-weapon damage buff is commented out. `Health` grows a `Veterancy`
member and the state hash feeds it only when non-zero, so every historical replay hash of a match
without promotions is unchanged. A killer at zero health that has not yet been retired earns
nothing, otherwise the promotion heal would resurrect a unit the same loop was about to bury.

## ADR-066 — Area damage is uniform inside the radius, with no distance falloff

**Context.** `damageArea` scaled damage by `1 - distance/radius`, full at the centre and nothing
at the rim. That is what Total Annihilation and Spring do and what almost everyone assumes every
RTS does. Supreme Commander does not. Traced in the retail executable: the sphere worker
(`0x0073e100`) and ring worker (`0x0073e5b0`) pass the amount to `DealDamage` (`0x0073dbc0`)
untouched, and the only thing between the spatial query and that call is shield absorption — a
flat subtraction with no distance term. The target-to-centre delta is computed and written into
the damage record as a *direction*; it is never multiplied into the amount.

**Decision.** Inside the radius every target takes the full amount; outside it takes none. The
point-hit path (zero radius, tolerance equal to the target's own radius) is unchanged. Recorded
as claim `C-061` in `docs/fa-exe-analysis-plan.md`.

**Alternatives considered.** Keeping the curve as a deliberate deviation was rejected: the
project's purpose is parity, and this changes blast totals by roughly a factor of two, which
propagates into deaths, wrecks and the state hash. Making it configurable was rejected as
speculative — nothing asks for both behaviours, and a switch here would be a second source of
truth about what the game is.

**Consequences.** Blast weapons hit substantially harder and blueprint damage numbers now mean
what the blueprint says. Two tests changed sides: `test_combat.cpp`'s falloff case now asserts
uniformity and a doubled total, and the death-explosion case expects full damage at half radius.
One test lost its purpose and says so — `test_armor.cpp`'s "armour before falloff" could only
discriminate ordering because the share was fractional; with a share of 1 or 0 the two orders are
arithmetically identical, so that ordering is no longer observable through it and will need
catching elsewhere if it ever matters.

## ADR-067 — Construction is settled in the command-dispatch stage, not the economy pass

**Context.** A build advanced, completed and retired its own order inside `tickEconomy`'s
write-back at the foot of the tick. Retail does all three inside the builder's own task in the
**command-dispatch stage**, the first stage of a beat (`C-142`, `C-188`), and writes the economy
ratio that work is multiplied by in the motion stage, the last one (`C-162`). Doing it our way
made construction a second site that mutates a queue head, which is the residue `C-112` had left
open against `C-211`'s "one site advances a unit's queue as a consequence of that unit's own
sub-task finishing". The fix was blocked on one unread fact: whether a build ordered on a beat
also advances on that beat. `C-232` answers it — every state transition in
`CUnitMobileBuildTask::TaskTick` returns task status `0`, meaning re-run immediately, same beat.

**Decision.** `advanceOrders` advances each builder's construction, reports what completed, and
retires the finished order — all in one per-slot pass, cascading into whatever order follows.
`tickEconomy` keeps the bill and the ratio, and charges work that finished this beat on a
`workedThisTick` stamp, which is retail's `Entity+0x520` worked-this-beat flag (`C-187`). The
assist scan moves to the head of the tick, alongside the stage retail's assisting builders run
their own tasks in.

**Alternatives considered.** Leaving construction where it was and moving only the queue pop to
the next beat's dispatch was rejected: it buys the single-site invariant by making the pop a beat
late, trading one mismatch for another. Leaving it entirely and recording the stage as a
permanent divergence was rejected once `C-232` made the faithful version cheap. Keeping the
economy's own completion reporting alongside the new one was rejected as a second source of
truth about when a thing is built.

**Consequences.** `docs/golden-p1.log` matches unchanged across the change, 7,000 ticks — the
work is re-staged inside the beat without any outcome moving, which is the evidence that it is a
fidelity fix. Two behaviours change and are now retail's: a completed build cascades, so the
order behind it starts and, if it is a build, materialises in the same beat; and a construction
whose founder is dead stops rather than continuing at the founder's rate, because a record with
no live builder over it is advanced by nothing. Tests that drove `tickEconomy` as a whole beat
now go through `tests/support/EconomyTick.hpp`, which pairs the two stages in tick order — a
`tickEconomy` call on its own now bills for work that never happened, and that is a different
beat rather than a smaller one.

## ADR-068 — The order queue is capped at retail's boundary, checked where retail checks it

**Context.** `C-214` recorded that retail rejects a new command when a unit's queue exceeds 500,
and did not record the comparison. Read at `0x006f7e30`–`0x006f7e48`: the element count is
compared with `cmpl $0x1f4` and `jbe`, so the test is strictly greater — a queue already holding
500 accepts one more and 501 is the ceiling a player can reach. The whole test is skipped when
the incoming command carries the clear-queue flag. We had no cap at all.

**Decision.** `kCommandQueueCap = 500` with `CommandQueue::atCapacity()` as strictly-greater, and
the check in `applyCommand` — retail's own site, on the way in from a command source — so it
bounds what a player can pile up and leaves engine-generated inserts alone. Orders that clear the
queue, which is every unqueued click and every `Stop`, are exempt, so a unit at the cap stays
commandable. Recorded as claim `C-231`.

**Alternatives considered.** Putting the check inside `CommandQueue::give` was rejected: it would
also refuse the synthetic origin waypoint a patrol pairs with its first destination, which retail
never sees because that insert does not come through `Sim::IssueCommand`. Rounding the boundary
to "no more than 500 entries" was rejected because the off-by-one is the only part of this a test
can fail on, and adopting it would quietly make the cap a different number from retail's.

**Consequences.** A player shift-clicking past the cap gets a refused order that changes nothing,
rather than an unbounded queue. The refusal is silent, as retail's is — the unit is simply
skipped — so nothing in the UI reports it yet.

**Not yet exact, and the reason is recorded rather than deferred silently.** Retail counts raw
queue entries, and one player patrol issue currently costs us **two** of them — the destination
plus a synthetic origin waypoint — where retail inserts one shared command. Exempting the
engine-generated origin keeps the player-facing boundary right, which is what this ADR claims;
it does not make the raw-entry count match. That reconciliation belongs to the shared-command
work (`WP-12` slice 1 in the project plan), and the cap should be re-checked against retail once
a patrol issue is one entry.

---

## ADR-069 — Parallel agents get worktrees; the sim core gets one writer per wave

**Context.** The parity campaign is wide — 20 subsystems, 45 work packages — and
most of its cost is reverse engineering, which parallelises perfectly because it
writes nothing. Implementation does not: `src/core/sim/` is a single fixed-point
machine with one tick order, and `docs/golden-p1.log` is a whole-sim baseline
that any behavioural change invalidates. Running several implementer agents
against one checkout produces interleaved edits to `UnitStore`, `Command` and
`StateHash`, and a golden log that nobody can attribute.

**Decision.** Two agent classes with different concurrency rules, recorded in
AGENT.md under *Working as a fleet*. Analysis agents are read-only and unbounded
in number. Implementer agents run in `git worktree` checkouts, own a work
package rather than a folder, and at most one per wave may touch any shared sim
file; a single integrator owns tick order, `SaveState` version and `StateHash`
coverage, and re-runs `make verify` against `main`'s golden log after each merge.
Worktrees symlink `third_party/` and `vendor/ai/` and keep their own `build/`.

**Alternatives considered.** One shared checkout with file-level locking —
rejected: the conflicts that matter are semantic (two agents each adding a pass
to the same tick) and no lock catches those. Branch-per-agent in one working
directory — rejected: agents run builds concurrently and would fight over
`build/`. Copying the repo instead of worktrees — rejected: it detaches history,
and worktrees already exclude game content because nothing tracked contains any.
Letting builders re-bless the golden log — rejected: a worktree's `make verify`
compares against that worktree's own log, so a branch can bless away its own
regression; only the integrator's re-run against `main` is evidence.

**Consequences.** Fleet width is set by evidence, not by cores: analysis scales
to as many agents as there are open envelopes, implementation stays near one
writer per shared file. Disk cost is ~560 MB of build output per worktree, which
is why a shared `ccache` is assumed. Attribution survives — every behavioural
change arrives as one branch with one explained golden diff.

**What this does not decide.** How the waves are ordered, which is a scheduling
question owned by `docs/fa-gameplay-progress.md`'s critical path, not by this
record.

---

## ADR-070 — Factory guard reserves guarded-unit work for a commandless child task

**Context.** Retail's immobile-factory branch in `CUnitGuardTask` does not lend BuildRate like
an engineer. It scans the guarded unit's queue, mutates the selected shared command before work
starts, and creates a `CFactoryBuildTask` with a null command pointer. The evidence ledger used
to point at `0x0061952c`, an epilogue; the actual reservation ladder is
`0x00619532`–`0x0061958f`, followed by child creation at `0x0061958f`–`0x006195ab`. The same
audit exposed a local bug: ordinary count-one factory completion called the global
`DecreaseCommandCount` path and could remove a grouped build from sibling factories.

**Decision.** An immobile factory may hold the existing `Assist` order. While that order is
active and the factory has no unfinished construction, command dispatch scans the guardee's
factory-build commands in queue order. A head is eligible above count one, or at final count only
when it is the lone repeat-enabled entry; qualifying non-head entries are eligible. Consumption
then decrements above one, otherwise restores and exact-rotates for repeat, or exact-removes only
from the guardee.
It then starts an ordinary `Construction` owned by the guarding factory while leaving Assist at
the head. The child gets no semantic command, id, source counter, creation serial, accepted-unit
membership, or log record, and its completion performs no second queue mutation. Ordinary build
completion now always retires only the completing unit's local entry; global zero-count removal
remains an explicit out-of-band operation.

**Alternatives considered.** Reusing engineer assistance was rejected because it accelerates the
guardee's one project rather than producing a second unit. Enqueuing a copied Build on the guard
factory was rejected because retail passes a null command pointer and such a copy would invent
authoritative input and allocator state. Calling global count exhaustion on final completion was
rejected because retail's dispatcher locally removes while the global helper is a separate site.
Adding a new task/state type was unnecessary: active Assist, existing construction ownership,
shared counts, and queue order already carry the visible state.

**Consequences.** Compatible factories now mirror guarded production with retail's shared-count,
local-removal, repeat, and no-double-consumption rules. The command panel
exposes Assist for immobile factories, while `applyAssistance` keeps them out of the engineer
BuildRate path. Replay encoding is unchanged: existing Build, Assist, and repeat records derive
the mirror during playback, so old replay streams remain importable. The guarding factory's own
queued BuildFactory/Upgrade priority branch and the rest of the nine-step guard ladder remain
explicit follow-up work. SaveState still serializes the unit store but not `Match::building`;
that pre-existing construction-resume gap now also means a save made during a mirrored build
loses already-reserved work and belongs to `WP-44`. Savegames may version that state independently
without changing the replay command format.

One architectural residue remains explicit: Recoil checks the product grid and pad before
reservation, whereas retail reserves after `Unit::CanBuild` and lets `CFactoryBuildTask` retry
placement. A blocked pad therefore leaves Recoil's guardee command untouched for a later scan;
retail has already consumed it into a retrying child task. Matching that timing requires child
task state rather than a guessed destructive fallback.

---

## ADR-071 — Operational diagnostics use one dependency-free process logger

**Context.** Runtime failures were printed directly from 51 call sites, without timestamps,
severity, consistent categories, or a way to retain them. The status text consumed by benchmark
and verification tools is separate stdout output and must remain stable.

**Decision.** Operational diagnostics route through one thread-safe C++ logger with UTC
millisecond timestamps, `trace` through `error` filtering, a short category, stderr output, and an
optional append-mode file sink. `--log-level` and `--log-file` configure it before content is
mounted. Each record is flushed immediately so the useful tail survives a crash. The logger uses
only the standard library and keeps the existing printf-style formatter so migrating diagnostics
does not add a formatting dependency or rebuild messages by hand.

**Alternatives considered.** A third-party logging package was rejected because the required
surface is small and does not justify another fetched dependency. Apple unified logging alone was
rejected because the core and planned Linux build are portable C++, and file logs must behave the
same in headless runs. Redirecting stdout was rejected because it would mix machine-readable
benchmark and hash reports with operational records.

**Consequences.** All runtime stderr diagnostics now share a searchable record shape and can be
retained with `--log-file`; normal status and benchmark stdout remain unchanged. The sink uses one
mutex and flushes each record, intentionally favoring reliable debug evidence over throughput.
Per-thread queues or rotation belong only after profiling or long-running deployment demonstrates
a need.

---

## ADR-072 — Builder arms use per-instance shader offsets over shared authored poses

**Context.** A unit batch shares one baked skeletal pose, but builders in that batch can work on
different targets. Rotating the whole unit toward its construction target made the simulation
heading visually wrong and still did not reproduce retail's `BuilderArmManipulator`, which aims
the authored yaw, pitch, and tool bones independently for each builder.

**Decision.** Parse the primary `BuildBones` arm rig and its authored arcs and slew rates from the
blueprint, then resolve bone indices, descendant flags, pivots, and axes once per model. Keep the
shared static pose buffer and add only yaw and pitch offsets to each unit instance. The unit
shader applies pitch and then yaw to marked bone subtrees for the normal, outline, and shadow
passes. Presentation derives the current target from construction state without changing the
simulation heading, and construction beams apply the same transform to their tool origin.

**Alternatives considered.** Uploading a complete pose per instance was rejected because two
angles describe the varying state and a dynamic bone buffer would add avoidable memory and upload
work. Splitting active builders into separate draw batches was rejected because it would trade a
small instance payload for draw-call churn. Mutating simulation heading was rejected because it
does not match retail behavior and would contaminate deterministic state for a visual concern.

**Consequences.** Independent builder arms cost eight bytes per unit instance and no per-frame
bone upload. Blueprint limits and slew rates are honored, numeric tool references fall back to an
authored build-effect bone when appropriate, and simulation hashes remain unchanged. The Metal
uniform uses explicitly packed padding, guarded by matching CPU layout assertions. Build-open
animation sequencing and alternate `BuildBonesAlt1` rigs remain explicit follow-up work.

---

## ADR-073 — Script commands have native scheduling and an injected Lua host

**Context.** Retail's `CUnitScriptTask` is neither an ordinary native command nor a free-running
Lua coroutine. `IssueScript` creates a serializable native command task, loads a named task module,
calls `OnCreate`, drives `TaskTick` through the native task-status scheduler, and calls `OnDestroy`.
The task's Lua object and command link survive serialization. Recoil Metal already has an app-side
Lua VM for opponent decisions, but making the deterministic sim depend on that AI adapter would
combine two unrelated sandboxes and leave cancellation cleanup outside queue ownership.

**Decision.** Add a semantic `Script` command whose shared payload carries a bounded task name and
opaque command bytes. Each queue entry owns bounded opaque execution bytes plus creation,
suspension, sleep, and AI-result state. Command dispatch implements the recovered status contract:
zero repeats in the same beat, positive values wait that many beats, `-4` resumes at the end of the
beat, `-2` suspends, `-1` completes, and `-3` aborts the task thread. An injected
`ScriptTaskHost` implements `OnCreate`, `TaskTick`, and `OnDestroy`; it owns interpreter behavior
but cannot retain pointers into movable queue storage. The queue retains the transient host pointer
so every removal path—including replacement, cancellation, and unit retirement—destroys a created
task exactly once. SaveState v12, semantic command-log v2, and the state hash preserve the new
identity and execution state. Restored queues bind a host before resuming.

**Alternatives considered.** Reusing the FAF opponent VM was rejected because it is an app-side AI
sandbox, not unit-script state. Encoding arbitrary Lua tables in the sim was rejected because table
semantics belong to the adapter and would turn a narrow seam into a second interpreter. A registry
of numeric task IDs was rejected because load-order IDs are not portable across replay or mods.
Skipping `OnDestroy` on out-of-band queue edits was rejected because retail cleanup is part of the
task contract and replacement is the common failure path.

**Consequences.** Native scheduling, replay, save/load, hashing, and cleanup can be tested with a
small host double before gameplay Lua exists. A later adapter can load
`/lua/sim/tasks/<TaskName>.lua` with the retail fallback and serialize its own object into the opaque
bytes without changing core command machinery. There is deliberately no production Lua adapter or
EnhanceTask implementation in this slice; those remain `WP-07` and `WP-34` work.

---

## ADR-074 — Lower-deck controls keep stable slots and pack only visible pages

**Context.** The HUD already had fixed frame rectangles, deterministic page ownership, and command
descriptors, but the bottom-right rectangle still showed factory production instead of actionable
commands. Build and roster art for every page was packed into one 144-slot atlas, so a synthetic
large catalog could evict strategic glyphs even though almost all of those icons were invisible.

**Decision.** The selection bay owns one fixed inspector and paged stable roster. Construction uses
the frame's fixed two-row page. The command rectangle always becomes a 4x3 rack when a selection
exists; canonical positions never compact, unsupported or unavailable commands remain disabled,
and the complete rectangle swallows input. Enabled cells dispatch through the existing semantic
command helpers, with Stop immediate and other commands arming the next target. Atlas packing clears
all item slots and assigns slots only to the currently visible build and roster ranges, followed by
strategic glyphs and optional classic chrome. Page changes are explicit repack keys and the combined
slot count is asserted against atlas capacity.

**Alternatives considered.** Keeping the factory production panel in the command rectangle was
rejected because two instruments cannot own the same hit geometry. Compacting enabled commands was
rejected because capabilities would move learned orders between selections. Growing the atlas or
adding one texture per page was rejected because invisible content should consume neither capacity
nor binds.

**Consequences.** The lower deck is bounded and actionable at the 1280x720 floor, command hover and
targeting participate in inspector priority, and hundreds of catalog entries no longer threaten the
atlas. The production view remains as tested view data but has no deck surface until a later design
assigns it non-conflicting geometry. Command artwork and secondary content-specific pages remain
future work; the first rack deliberately uses readable labels and only simulation semantics that
already exist.

---

## ADR-075 — One minimap projection owns every map-space surface and input

**Context.** Pips and world clicks already used an aspect-preserving fit, but the preview and fog
still filled the square panel, clicks in letterbox bars clamped to a map edge, and diagonal camera
footprint edges were axis-aligned bounding rectangles. A rectangular map therefore had several
plausible but mutually inconsistent projections.

**Decision.** `minimapProjection` returns the sole content rectangle and points-per-elmo scale for
a fixed square panel. World projection and inverse input use it; inverse input returns no point in
letterbox space. Headless and windowed render paths give the same content rectangle to the shared
preview/fog pass. Camera footprint edges are emitted as rotated solid quads with constant point
thickness. The closed game-profile descriptor supplies resource vocabulary and ownership policy;
BAR gets a deterministic warm field-acrylic fallback while neutral remains explicitly ownerless.

**Alternatives considered.** Clamping letterbox clicks was rejected because an empty bar is not a
map edge. Stretching the short map axis was rejected because it makes positions, motion, and camera
coverage disagree with world aspect. A separate preview fit was rejected because duplicated
projection arithmetic caused the divergence. A general plugin/profile resolver was rejected in
favor of the four concrete profiles actually supported.

**Consequences.** Preview, fog, pips, click/drag navigation, and camera footprint align on square
and rectangular maps. X1CA_003's 2048x1024 geometry visibly occupies a centered 2:1 content region,
with inert bars above and below. Content-specific BAR faction emblems and the later material tuning
remain separate from this profile seam.

---

## ADR-076 — One shared quarter-resolution backdrop, with a genuinely direct Off path

**Context.** Every HUD panel needs the same post-water battlefield behind it, but blurring each
panel independently would repeat filtering work and sampling a drawable after rendering is not a
portable Metal contract. Water refraction already owns a pre-water colour copy; reusing it would
give the interface an incomplete scene. Reduced Transparency also needs to remove the composition
cost, not merely hide a blurred result below an opaque colour.

**Decision.** Full and Reduced render the world without UI into one full-resolution
shader-readable texture, raster-downsample it into a quarter-resolution pair, run exactly one
`MPSImageGaussianBlur`, and compose the sharp world before semantic UI layers. Only solid panel
surfaces sample the shared result; minimap art, chrome, icons, labels, and readouts remain crisp.
Reduced selects a smaller immutable kernel and stronger veil. Off calls the original direct scene
encoder, never ensuring the backdrop textures or encoding downsample, blur, or composition. The
window resolves macOS Reduced Transparency to Off unless `--ui-effects` explicitly overrides it.

**Alternatives considered.** A blur per panel was rejected as duplicate GPU work. Reusing the
water refraction copy was rejected because it is deliberately captured before water. A custom
compute blur was rejected because Metal Performance Shaders already supplies the platform kernel.
Keeping the intermediate graph alive under an opaque Off material was rejected because it would
make the accessibility path cosmetically different but no cheaper.

**Consequences.** The material has one shared scene source and one filter dispatch per frame at
every HUD size. The direct Off graph is preserved outside the extracted UI helper. The Objective-C
MPS call is isolated in a tiny `.mm` bridge because importing Objective-C Metal and metal-cpp
declarations in one translation unit produces symbol redeclarations. On the same headless scene,
Full versus Off measured +0.012 ms GPU mean at 2560x1440 pixels and +0.390 ms at 5120x2880; the
later window-size task remains responsible for equivalent real-window 5K evidence.

---

## ADR-077 — Material identity changes response, never interface meaning or geometry

**Context.** Accent colour alone made the six native presentations look like recolours, while
decorative texture, distortion, or animation would compete with information and violate the first
glass review. Classic FAF also has a different implementation: nine retail image slices replace
procedural fill and bevel, and a partial set must not leak unused pieces or faction glass into an
otherwise neutral fallback.

**Decision.** Keep frame rectangles, vertex positions, typography, semantic resource/state colours,
and hit tests profile-independent. Each native material supplies only three backdrop coefficients:
tint strength, source saturation, and absorption. UEF is laminated blue tactical glass; Aeon is
light, desaturated opalescent ceramic; Cybran is a dark smoked magenta composite; Seraphim is a
bright warm crystalline plate; BAR is warm field acrylic; Neutral is restrained cyan observer
chrome. Existing bevel colours provide the edge response. Complete classic art replaces native
fill atomically; missing or incomplete art selects the entire Neutral material.

**Alternatives considered.** Grain, refraction, fracture textures, glints, and decorative motion
were rejected because the screenshot review showed no need for them and the binding plan reserves
them until evidence does. Faction-specific frame shapes were rejected because controls must not
move when presentation changes. Mixing successfully loaded classic pieces with native glass was
rejected because it creates an accidental seventh material and unstable atlas behavior.

**Consequences.** Every native presentation is recognizable in the seven-case 1280x720 contact
set without moving a pixel of module geometry. Full and Reduced reuse the same one-blur graph with
different coefficients; Off keeps semantic chrome but bypasses material sampling. Solid black
shadow quads remain ordinary premultiplied alpha and, by themselves, no longer activate the blur,
so classic FAF pays no native-glass cost.

---

## ADR-078 — Unit normal maps keep UV1 and derive their tangent frame per pixel

**Context.** Forged Alliance units conventionally ship a third `_NormalsTS.dds` texture. Retail
`mesh.fx` samples it with the SCM vertex's second UV pair and decodes `.gaa`: green is X, alpha is
Y, and Z is reconstructed. Recoil Metal retained only UV0 and therefore could not bind this map.
The SCM record also carries tangent and binormal vectors, but retaining both would grow every
shared model vertex—including BAR models—from 36 to 68 bytes.

**Decision.** Retain UV1, growing the shared vertex to 44 bytes, and load `_NormalsTS.dds` as an
optional third Supreme Commander texture. Decode green then alpha exactly as `mesh.fx:594` states.
Reuse the prop shader's derivative-built tangent frame and guarded Z reconstruction; S3O duplicates
UV0 into UV1 but never binds a unit normal map. Fine and coarse SCM levels resolve their maps by
their own mesh names.

**Alternatives considered.** Alpha-then-green was rejected because it contradicts the retail
shader. Retaining authored tangent and binormal vectors was rejected for a 24-byte per-vertex cost
when the existing derivative frame supplies the needed basis. Repacking three retail textures into
Recoil's two-texture convention was rejected because it is lossy and makes mounted content require
an offline conversion step.

**Consequences.** Normal-mapped units gain authored surface relief through the existing VFS and
Metal path, while units lacking the optional map keep their geometric normal. The extra 8 bytes are
paid by every model vertex; no extra texture is loaded for BAR content. The prop channel-order
question remains separate because its content contract is not established by the unit shader.

---

## ADR-079 — Shield geometry is an explicit sphere-or-box contract

**Context.** Ordinary Forged Alliance bubbles create a spherical shield entity, while retail
`UnitShield` creates an attached box from `CollisionCenter*` and `CollisionSize*`. Treating the
personal shield as either absent or as `ShieldSize / 2` loses its health pool or lets it protect
nearby units outside the owner's collision shell.

**Decision.** Carry an explicit sphere-or-box shape from `Defense.Shield` through `UnitCatalog`.
PersonalShield imports the retail box fields, with the Lua defaults of centre zero and size one,
and converts full ogrid sizes into elmo half-extents. Area containment, blast admission, and direct
projectile sweeps dispatch on that shape. A conservative enclosing radius is retained only for the
spatial broadphase and retail's inside-origin admission rule. PersonalBubble and TransportShield
remain excluded because their owner and cargo semantics are separate work.

**Alternatives considered.** Using `ShieldSize` as a personal sphere was rejected because retail
does not use it for UnitShield collision. Treating every personal shield as owner-only extra health
was rejected because it erases the collision shape and direct-projectile ordering already supported
by the shield entity path. A polymorphic collision hierarchy was rejected in favor of one enum and
two compact geometry records.

**Consequences.** Personal shields absorb only for points inside their attached box, and shots
outside a face do not hit a spherical approximation. Ordinary bubble behavior is unchanged. Box
shields currently have no separate presentation shell; the owner model remains the visible retail
surface, while PersonalBubble and transport coverage stay explicitly unsupported.

---

## ADR-080 — Retail command caps constrain the native command rack

**Context.** The native rack inferred every command from broad engine capabilities. That mostly
works, but it cannot express authored exceptions: URL0107 has BuildRate 1 for its Mantis repair arm,
so inference enabled both Repair and Reclaim even though `General.CommandCaps` explicitly enables
the first and disables the second. Retail `lua/ui/game/orders.lua:SetAvailableOrders` instead
receives `GetUnitCommandData(selection)` and builds the page from that result.

**Decision.** Import the true entries in `General.CommandCaps` and separately retain whether the
table was authored. For FA content, a supported command needs both the simulation capability and
the corresponding retail cap. Mixed selections keep any-capable-unit semantics. Definitions with
no command-cap table, including BAR and focused test fixtures, retain the existing capability
fallback. The rack geometry, labels, and semantic dispatch remain unchanged.

**Alternatives considered.** Inferring everything from movement, weapons, and BuildRate was
rejected because it cannot represent explicit false entries. Treating an empty imported list as
"missing" was rejected because an authored all-false table is meaningful. Importing ToggleCaps and
OrderOverrides in the same slice was deferred because their actions and conflict rules need state
the simulation does not yet expose.

**Consequences.** The first production command page is selected-data-driven without moving learned
controls. The Mantis can Assist and Repair but no longer advertises Reclaim. Unsupported retail
toggles and presentation overrides remain a named next slice rather than inert buttons pretending
to work.

---

## ADR-081 — Retail unit playability is checked by data-driven capability contracts

**Context.** The downloaded unit corpus can prove that blueprints parse, but successful parsing
does not prove a factory product is playable. A scout with no vision, a bomber with no ground
weapon, or a transport without its command cap would all pass a structural corpus test. Writing a
separate test per unit would duplicate the same loading, factory-membership, and role assertions
hundreds of times.

**Decision.** Keep one table of factory id, product id, expected role, domain, and only genuinely
exceptional capabilities. A shared harness loads the corpus once, verifies the table is the exact
`TECH1 + MOBILE` result of each factory's authored build expression, then applies the capability
contract for the declared role. Land and air T1 products are the first complete slices.

**Alternatives considered.** One bespoke test per unit was rejected as repetitive and difficult to
audit for omissions. Inferring the expected role from the same blueprint fields under test was
rejected because it would make a broken role classifier its own oracle. Treating the wiki mirror
as executable authority was rejected; it supplies product intent, while shipped blueprints remain
authoritative for membership and capabilities.

**Consequences.** Adding another factory or tier is predominantly table data, and exact roster
comparison exposes both missing and unexpected products. The table is intentionally explicit and
must be reviewed when retail intent is ambiguous; it is not a generated restatement of production
logic.

---

## ADR-082 — Wreck reclaim advances one shared work fraction

**Context.** Retail returns mass, energy, and time separately but advances the target once through
`Materialize`. The actual applied fraction then scales both resource credits. Independently
draining mass and energy produces the wrong completion time for mixed-resource wrecks and can lose
fixed-point residue on the final tick.

**Decision.** Store immutable maximum values, damage ratio, and one current work fraction on each
wreck. Each reclaimer applies a bounded work slice and credits mass and energy by that same slice;
the final slice receives the exact remaining fixed-point values. Wreck damage recomputes value,
work, and reclaim rate from health without discarding the already-materialised fraction.

**Alternatives considered.** Independent resource drains were rejected because they cannot model
one `Materialize` return value. Using health alone as progress was rejected because a finished wreck
may start below maximum health while materialised fraction remains one. Floating-point ratios were
rejected because simulation state is fixed point and must hash deterministically.

**Consequences.** Mixed-resource reclaim has one completion boundary, concurrent reclaimers share
it safely in deterministic slot order, and exact totals survive rounding. Features carry more
persistent fields, so the state hash and golden determinism log intentionally change when the first
wreck appears: tick 5734 is the first divergence, when `hashMatch` begins feeding the new wreck
health, maximum-value, and shared-work fields. The first 5734 hashes remain unchanged.

---

## ADR-083 — Stored resources inform build cells but never authorize them

**Context.** The build tray treated `stored mass >= total mass cost` as an enable gate. That is a
bank-purchase model, but Forged Alliance construction is flow-funded: a player may start a build or
upgrade with an empty store, after which the economy allocator slows progress under shortage.

**Decision.** Keep the bank comparison only as presentation state for the mass-cost colour. Every
authored build option remains fully visible and clickable. Factory products and upgrades submit at
the builder immediately; placed structures arm their ordinary placement ghost regardless of the
current store.

**Alternatives considered.** Keeping the dimmed disabled cell was rejected because it contradicts
the economy model and makes a valid order look inert. Hiding unaffordable options was rejected
because it would also reflow learned tray positions. Removing the indicator entirely was rejected
because a short bank is still useful planning information.

**Consequences.** UI authorization now agrees with simulation authorization, while the existing
economy request path remains the sole authority over construction speed. A focused pure-UI test
pins both low-resource activation modes without requiring a window or synthetic pointer events.

## ADR-084 — Behavioral T1 contracts and bounded projectile pursuit

**Context.** Running every real T1 land factory product through MatchRunner exposed
aircraft flattened by collision alignment and Cybran AA unable to hit a patrolling
aircraft because authored tracking projectiles flew straight.

**Decision.** Preserve integrated airborne altitude during collision resolution.
Resolve ordinary projectile blueprints as well as counted ammunition, and use
generation-safe fixed-point pursuit with the authored turn, acceleration and speed
limits. Keep reusable primary-role scenarios beside independent roster contracts.

**Alternatives.** Stationary-only AA tests would conceal the failure. Reproducing
the complete undocumented retail guidance controller is a separate parity task.

**Consequences.** Guidance is executable and hashed, but not claimed retail-exact.
See `docs/t1-headless-contracts.md` for sources, checks and named limitations.

## ADR-085 — Factory queue controls alongside the command rack

**Context.** ProductionPanel had queue/progress drawing but no active HUD caller.
Replacing the command rack would make existing factory commands inaccessible.

**Decision.** Anchor the existing production panel immediately above the rack.
Share its rectangle and control hit tests across drawing and input. Repeat and
Clear Queue submit existing ToggleFactoryRepeat and Stop commands through the
normal recorded dispatch path; the UI never mutates a queue or repeat flag.

**Alternatives.** Replacing the rack loses controls. A separate queue model or
new cancellation protocol is unnecessary for repeat and whole-queue clearing.

**Consequences.** The panel occupies some battlefield space only when an active
factory is selected. Click and drag interception use its rendered bounds. The
build tray still adds products; per-row deletion and reordering remain outside
this slice. Headless real-factory controls and deterministic captures verify it.

## ADR-086 — Hovercraft have a surface layer independent of ship routing

**Context.** The real Aurora land-water-land test routed successfully but submerged the unit:
hover passability allowed water while movement and collision alignment still used seabed height.

**Decision.** Cache the Hover motion class and authored Physics.Elevation in MoveState. Both
movement and collision placement use max(ground, water) plus that clearance, with a level hull.
Keep ship-only passability separate. Save v14 carries the cached hover state and StateHash covers
it; the test checks movement into water and back onto dry land through MatchRunner.

**Alternatives.** Marking hovercraft as ships would constrain them to water and put dry-land units
at water height. Correcting only render positions would leave targeting and collision submerged.

**Consequences.** The surface invariant is executable, but this does not claim retail suspension,
banking or hover acceleration parity. Historical save versions remain readable; old snapshots did
not preserve a hover layer. Golden changes must be attributed before updating the baseline.

## ADR-087 — Explicit Guard shares the assistance ladder without granting build power

**Context.** The assistance ladder could escort, fight and help construction, but its Assist
entry point required a builder. Combat units could not receive a dedicated Guard order.

**Decision.** Append a semantic Guard kind and expose it in the command rack. Reuse the existing
ladder and standing-order lifecycle, checking the guard capability, allied ownership and living
non-self target. Reclaim/repair/build assistance still require engineering rates. Save v14 accepts
Guard queues; command logs preserve the kind. Legacy Assist retains its builder-only contract.

**Alternatives.** Relaxing Assist would silently change its public capability contract. Giving
combat guards synthetic build rates would permit them to repair or construct.

**Consequences.** Real-unit tests cover escort, automatic fire, refusal, target alliance/death,
queued followers and live/replay equality. Those tests also exposed ordinary targeting treating
ALLUNITS as an authored tag; category expressions now recognize the universal set while retaining
conjunctive restrictions. That changes automatic targeting beyond Guard and needs a newly
attributed golden comparison once retail assets are accessible. Refuel/staging, ferry, exact
multi-weapon arbitration and physical display migration remain outside the verified behavior.
Native NSEvent Guard targeting and Stop now pass for all 23 T1 land products across all
24 faction/profile/simulated-backing cases on aw04.smf.

The construction prepass now shares the dispatcher's return/attack eligibility checks, so help
is not credited before a higher-priority attack or a changed alliance cancels it. Copied reclaim
and scanned repair also honor declared capabilities; the real Mantis cannot gain reclaim by
guarding an engineer. These cases are exercised through the complete skirmish tick.

## ADR-088 — Factory cancellation names one queued command

**Context.** Repeat and Clear Queue do not let a player remove one production entry. A row
index cannot serve as command identity because earlier production can finish before dispatch.

**Decision.** Add an immediate CancelFactoryBuild issue carrying a stable cancelCommandId.
Dispatch checks factory ownership and removes only that factory's matching mobile-build entry,
including its remaining count. Cancelling active work discards its unfinished construction;
pending deletion leaves current progress intact. Other factories sharing the issue retain it.
Own production behind a standing Guard records its retained command ID on the child construction,
so cancelling that row also aborts the child without disturbing mirrored guardee work. The ID
participates in the state hash; construction persistence is part of the separate save/resume work.
The UI shows a Cancel control per row and pages through all entries. Semantic log v3 preserves
the selected command ID, while reading v2 remains supported.

**Alternatives.** Direct UI mutation bypasses replay and authorization. Deleting by product type
or queue index can delete a different request. Reusing unit target coordinates for a command ID
would hide its distinct meaning from validation and serialization.

**Consequences.** This is an instantaneous issue, not a persistent queued command; save snapshots
already preserve the resulting queue and allocator state. Focused tests cover active/pending
work, forbidden owners, stale clicks, shared issues, UI routing and continued file replay.
Native responder acceptance covers pagination, pending and active row cancellation, surviving
command identities and Clear Queue in all 24 faction/layout/backing cases using extracted units
on the available BAR map. Reordering and decrementing a stack by one unit are separate operations.

## ADR-089 — Save economy carry and army lifecycle at completed tick boundaries

**Context.** Unit snapshots could round-trip while losing construction funding, allied gifts,
and the pending victory/defeat timers. Recreating armies from initial conditions silently changes
the next tick, even if unit positions match.

**Decision.** Save v15 adds an optional economy/army section containing every Economy field,
construction history and active work (including retained factory command IDs), army identity and
alliances, commander history, storage configuration and lifecycle timers. Capture after the
economy pass settles its work stamps; encoding unfinished tick work is rejected. Restore replaces
owning vectors and rebinds Match spans after runner construction. Historic v1–v14 decoding remains.

**Alternatives.** Recomputing carry or resetting lifecycle loses next-tick state. Serializing only
resource balances would miss allocated-but-unspent resources and gifts due on the following tick.

**Consequences.** Two fresh-scene continuations with real blueprints compare every tick through
completed construction, resource sharing, delayed defeated-army cleanup and winner confirmation.
The test exposed a separate C-163 wiring bug: tickEconomy clamped excess before shareOverflow.
Whole-match allocation now defers capacity until sharing finishes; non-sharing armies still lose
their excess. This behavioral change requires golden attribution once retail assets return.

This completes the economy/army subsystem snapshot, not general in-game save/load: pending path
searches, feature pools, projectiles, intel history, queued external input and opponent VM state
still need their own persistence. The continuation fixture has no such outstanding state and
keeps the immutable blueprint registration order. No save/load UI is exposed by this change.

## ADR-090 — Wrecks receive area damage through their own object pool

**Context.** Wreck damage/reclaim-value recalculation existed, but ordinary combat never called
it. Prop collision callbacks alone do not establish projectile interception: retail's ordinary
sweep uses mask 0xD00, excluding props, while DealDamage's area query uses 0xF00 (C-065, C-086,
C-137). Wreckage.lua:17–39 reduces health and recalculates reclaim values after damage.

**Decision.** Positive-radius projectile impacts, beams and death blasts enumerate the live
feature pool and call damageFeature. Deferred projectile impacts query current geometry at their
recorded impact position on the following beat. Radius-zero hits retain their named unit target;
ordinary sweeps still exclude wrecks. Feature handles never resolve through UnitStore, even when
the two pools issue numerically identical handles.

**Alternatives.** Adding wreck interception to every projectile would contradict the recovered
query mask. Converting a FeatureId to UnitId would damage an unrelated live unit. Recalculating
reclaim independently would duplicate Wreckage.lua's existing damage path.

**Consequences.** Tests cover artillery through MatchRunner, continued command replay hashes,
the delayed impact beat, point-hit pool separation, slot reuse, box/sphere boundaries and lethal
damage. This uses the current upright unit-box approximation, not exact authored wreck offsets
and orientation from Unit.lua:1111–1113. Named wreck attacks remain unavailable. The existing
instantaneous death-blast/wreck creation order remains a separate approximation to Unit.lua's
independent delayed threads (1195–1219). No general save/load of features or projectiles is claimed.

## ADR-091 — Persist winged combat tactics and use a planar turning controller

**Context.** Winged attacks implemented only states 1 and 2. They aimed horizontal velocity
directly at the target, clamped aircraft to map edges, and saved neither authored controller
tuning nor any random state actually consumed by the match. These shortcuts prevented sustained
turns, timed breakoff and off-map recovery from functioning or continuing after a save.

**Decision.** C-224's state/deadline controller now runs for winged entity attacks: states 3/4
request minimum airspeed, 5 maximum, 6 flies forward until its deadline, and 7 steers to map centre
until inside the five-ogrid inset. Turn choice and duration consume separate multiply-high MT19937
draws, with exclusive upper bounds and inclusive deadlines. The sustained-turn counter uses a
strict threshold comparison before incrementing. New attacks reset tactical state. The match owns
the random stream; its future sequence and dynamic aircraft state enter the match hash.

Horizontal combat desire follows aircraft heading. A planar PD controller uses Air.TurnSpeed,
CombatTurnSpeed, KTurn, KTurnDamping and TightTurnMultiplier; the hard-turn multiplier augments
the gain, not the angular rate. This is a deliberate reduction of the three-axis solver, with
mass ratio fixed at one, no quaternion bank/pitch torque and no bomb-drop prediction. It does not
establish complete retail flight parity. The two spawn paths now share motionFor.

**Sources.** ART-E001 ComputeAirCombatTactics 0x006c3830–0x006c420d; random turn choice
0x006c3e9f–0x006c3eee, breakoff deadline 0x006c3cb5–0x006c3d16, recovery 0x006c3a6c–0x006c3bbd.
CalcWingedOrientation 0x006c4908–0x006c4951 adds the hard-turn gain correction. Air schema
0x00527a40 maps KTurn/KTurnDamping to offsets 0x50/0x54; constructor 0x00525a00 initializes
both to 3, while UEA0102 authors 1 and 1.5. Lengths convert from ogrids to elmos once at load.

**Alternatives.** Resetting a local RNG per call repeats tactics; a process-global RNG couples
matches. Keeping the edge clamp makes recovery unreachable. A full rigid-body solver is outside
this bounded state-machine change and must be validated independently.

**Consequences.** Save v16 appends dynamic and spawn-cached aircraft controller state, retains
v1–v15 readers, and restores the match RNG. Real-interceptor tests compare each continued tick
from both a turn and a recovery checkpoint, with weapons held in reload to isolate motion from
the still-unsaved projectile pool. A 900-tick app pursuit capture on aw04.smf exercises the live
renderer with extracted assets; missing interceptor textures remain visible. Existing 24-case
native land controls include per-product Guard targeting and Stop. The full suite still requires
the disconnected retail projectile archive. The added RNG hash coverage changes hashes from
tick zero; the existing retail-map golden has not been regenerated or blessed.


## ADR-092 — Attribute resource spending and enforce authored deposit restrictions

**Context.** The HUD omitted construction spending, and both spawn paths created
ammo production for enhancement-only ACU weapons. A real SCMP_009 replay with one
extractor and one generator reproduced an idle ACU's 120 energy/s tactical-ammo
bill, then nuclear-ammo demand. The army ledgers were separate. Blueprints also
stated deposit restrictions that construction ignored.

**Decision.** Exclude enhancement-only weapons from automatic ammo setup, matching
their existing disabled firing rules. Expose derived per-unit income and actual
allocator grants for selection and five-second debug reports; retain the existing
allocation algorithm and exclude this telemetry from hashes and saves.

Parse `Physics.BuildRestriction` and carry static map deposits through the existing
terrain view. The shared build command requires an exact matching marker centre;
upgrades retain their established foundation. Snap the placement cursor within
32 elmos (four ogrids), a UI tolerance rather than a retail simulation claim.
Render green mass and amber hydrocarbon rings and minimap markers. Existing
footprint collision checks prevent overlapping extraction sites.

**Alternatives.** UI-only restrictions leave replay and AI bypasses. Inferring
restrictions from unit categories ignores authored blueprint rules. Recomputing
spending in the HUD disagrees with funding limits and retained allocations.

**Consequences.** New games no longer charge uninstalled missile enhancements.
Construction away from required deposits is rejected, including old invalid replay
orders. Map deposits remain static map input, like terrain, rather than saved unit
state. This intentionally changes simulation hashes at tick zero by removing ACU
silo records; no golden baseline is silently regenerated. Tests cover both spawn
paths, enemy-bank isolation, real extractor restrictions, snapping, inspector rates
and diagnostic cadence. An offscreen opening capture verifies +23 energy/s with
one generator and extractor, full storage, and visible deposits.


## ADR-093 — Expose authored upgrades and align building presentation with placement

**Context.** A hard-coded T1 factory-menu filter hid unlocked T2/T3 products.
Extractor roles could not own the build tray despite authored upgrade paths.
Structures faced arbitrary bearings toward map centre, rotating their rendered
models away from axis-aligned foundations. Building placement discarded Shift.

**Decision.** Let factory BuildableCategory determine available products, list
higher tiers first, and keep the successor exclusively in the upgrade button.
Allow upgradeable structures to expose their upgrade without a general build menu
or a factory production panel. Forward Shift to the existing build-command queue
and retain the armed placement while queuing.

Quantize structure facing to the nearest cardinal using fixed-point rounding in
the shared preview/construction/completion helper. FA selection outlines use square
perimeters with terrain samples; range and resource rings retain their meaning.

**Alternatives and consequences.** Additional tier tabs would duplicate existing
pagination. UI-only rotation would disagree with saved headings; the cardinal rule
therefore intentionally changes headings of newly built units, and no golden is
silently blessed. Existing placed units retain their headings. Tests cover real
factory tiers and extractor upgrade orders, sequential queued construction and
outline geometry. The retained `hud-factory-t2.commands` replay produces a completed
T2 factory with the new tray and selection outline for offscreen verification.


## ADR-094 — Carry pending commands through building upgrades

**Context.** The tray offered the active upgrade again instead of its successor.
The dispatcher also tried subsequent commands against the old blueprint, and the
completion spawn discarded the old unit's remaining orders.

**Decision.** Project the upgrade button through active and already queued upgrade
steps. A successor button queues automatically and explains that in its inspector.
Completing an upgrade retires its current command but leaves the rest pending until
the replacement spawns. Transfer each pending execution and shared command identity
to the replacement, update its canonical member handle, and retain factory repeat.
The original recorded issue remains unchanged. UnitFinished names the replaced
handle as its instigator so selection can follow it.

**Alternatives and consequences.** Reissuing new commands loses IDs and changes replay
history. In-place unit mutation would require reworking the existing spawn and draw
registration path. Transfer uses the existing queue/save representations without a
schema version change. Commands validate against the successor on its next dispatch
beat; Stop still clears the whole chain. Tests complete real T1→T2→T3 upgrades,
restore the pending queue from a save, preserve selection, and cancel with Stop.

## ADR-095 — Reuse authored engineer menus and upgrade queue controls

**Context.** Engineer menus still hid all structures above T1 and excluded roles
such as shields. Pending extractor upgrades survived replacement but were invisible
after being queued.

**Decision.** Engineers use the same blueprint BuildableCategory menu as factories,
sorted by tier, including earlier tiers and authored experimentals. Unenhanced ACU
menus retain their T1 restriction because their base blueprints also list categories
unlocked by engineering enhancements, whose installation is not implemented.

Reuse the production panel for an immobile builder's upgrades, with tier labels and
no repeat control. Extend the existing stable-ID cancellation command to upgrades;
cancel a pending T3 without disturbing active T2 work, or cancel T2 and its dependent
T3 together. Ownership checks remain in shared dispatch. Preserve the historical
CancelFactoryBuild command name and numeric value for recorded logs.

**Alternatives and consequences.** A second queue widget or command format would
duplicate existing paths. Keeping later tiers after cancelling their prerequisite
would leave invalid work. No save-format change is needed. Headless tests cover
authored T1/T2/T3 engineer menus, active/pending cancellation, enemy rejection and
completion after cancellation; offscreen captures repeat pixels and hashes.

## ADR-096 — Resolve weapon visuals from original Lua declarations

**Context.** Generic coloured dots and bead chains hide weapon identity. Projectile
blueprints alone do not name the effects: script classes inherit PolyTrails,
FxTrails and BeamType/FxBeam through faction modules and EffectTemplates.

**Decision.** Execute those original declarations in a separate Lua state using
the existing legacy-dialect rewriter and read-only VFS imports. Supply class
inheritance and native base-class shells, without running simulation callbacks.
Bound instruction execution and report missing/unsupported content. Load the
referenced emitter blueprints and DDS textures into presentation-only materials.
Shots retain their projectile path; beam events retain the unit/weapon key and
authored lifetime. Continuous beams remain visible until the next firing beat.
These fields do not participate in simulation hashes.

Render attached sprites and straight textured strips in the existing particle
pass. Read dimensions, colour ramps, beam tints and scrolling from emitter data.
Use additive rendering with the original beam-versus-particle alpha distinction.
Unknown definitions keep the procedural fallback. The verification gallery uses
a one-elmo width floor to make small effects inspectable.

**Alternatives and consequences.** A C++ table of weapon names would duplicate
Lua and miss mod overrides. Running the complete native simulation scripting API
is unnecessary for declarations. This is not a complete emitter engine: historical
trails, emission curves, mesh projectiles, non-additive blend modes, live beam
endpoint tracking, and new muzzle/impact effects remain outside this slice. Short
beams display at least one texture repeat for visibility. The gallery verifies our
rendering, not pixel parity against the original game.

## ADR-097 — Execute authored emitter timing and blend semantics

**Context.** Resolving original assets is insufficient: an Aeon Disruptor uses
inverse modulation, and electron-bolter particles begin at zero size. The initial
additive-only renderer and first-key width validation changed those effects.

**Decision.** Read effect curves as linear segments with per-key random ranges.
Integrate emission rate and invert cumulative births within each step, preserving
fractional emission and interpolating positions. Particles retain sampled birth
values and evolve in the shader using their own lifetime, growth, rotation,
animation and ramp row. Reuse the existing particle storage and tick paths.

Follow retail `effects/particle.fx` blend equations and geometry: authored sizes
are half-widths; flat sprites use XZ; inverse modulation requires separate blend
states; refraction samples the copied scene with the authored RG distortion mask.
The existing water scene-copy mechanism supplies the background, with a post-water
copy when necessary. Draw order follows emitter SortOrder before material batching.

**Alternatives and consequences.** Artificial brightness/width boosts hide source
mismatches. One static sprite cannot represent an emitter's rate or lifetime.
This remains presentation-only and does not change simulation state. Refraction
adds a scene copy only when a refracting particle is actually present. Particle
buffers grow from 76 to 112 bytes per entry. Full native-emitter parity remains a
separate claim: drag, some alignment/water flags and callback-driven modifiers
still need coverage.

## ADR-098 — Drive muzzle and impact emitters from combat events

**Context.** Generic flashes and puffs discarded the weapon scripts' FxMuzzleFlash
and impact-type lists. The native Projectile and DefaultWeapons Lua declarations
also carry inherited defaults and scales, so replacing them with empty classes
lost authored data.

**Decision.** Execute those original declarations in the isolated visual Lua host.
Resolve muzzle lists by unit/weapon label and impact lists by projectile and native
impact type. Retain empty lists as intentional silence, and keep source scales.
Combat events carry cosmetic source identity, initial muzzle and trajectory; their
simulation damage and movement remain unchanged.

Persistent presentation emitters retain their clock and sampled particles outlive
them. Emit at creation before scheduling subsequent particles by integrated rate:
otherwise the retail Gauss flash expires before its first whole rate interval.
Muzzle emitters follow the resolved rest bone through the same roll/pitch/yaw
placement used for drawing, guarded by the unit's generation. Detached impacts
remain at contact and retain projectile direction. The runtime does not execute
gameplay callbacks; terrain-material secondary effects and custom callback-only
effect creation remain outside this declaration-driven path.

**Verification.** Corpus checks cover all four tank effect families and the
Seraphim ACU's inherited muzzle list; a regression handles retail `0then` syntax
without changing strings/comments. Tests cover impact identity after delivery,
bone movement/rotation, slot reuse, source scales and emitter scheduling.
The offscreen `--weapon-gallery --impact-gallery` path shows muzzle, terrain-hit
and unit-hit columns using those same event consumers, fifty milliseconds after
creation.

## ADR-099 — Beams follow live endpoints, drawn one tick at a time

**Context.** ADR-096 drew a beam as one strip particle whose endpoints were the
firing event's snapshot for the authored lifetime. A continuous Cybran microwave
laser therefore stayed nailed to where the target stood when the shot resolved.

**Decision.** `BeamFired` registers a `CombatBeam` in the presentation state with the
shooter, target, key and remaining lifetime; a new shot from the same weapon replaces
its live beam. Each tick draws one fresh strip between the current endpoints with a
lifetime of exactly one tick, carrying the beam's accumulated age so texture scrolling
is continuous. The app moves `from` to the resolved muzzle bone and `to` to the
target's transform, removes beams whose shooter died, and leaves a dead target's beam
pointing where it last stood.

**Alternatives and consequences.** Rewriting the existing strip in place would need
stable particle identity the buffer does not have. Overlapping strips would double
additive brightness, so lifetimes are exact and expiry keeps half a tick of slack.
Simulation state and hashes are untouched. Endpoints are ground transforms, not
target bones.

## ADR-100 — Bound the FAF watchdog's refills and pace condition evaluation

**Context.** The reproducible 1,800-second SCMP_009 duel reported 13 decision
failures and 38 condition errors, all "instruction budget exhausted". Tracing showed
one genuine overrun per affected pass — counting ~200 engineers against every
builder's conditions — and a cascade: the watchdog never refilled after raising, so
once the corpus's `pcall` caught the first error every further thousand instructions
raised again, at whatever line happened to be running.

**Decision.** Refill the budget before raising, at most four times per chunk or
decision pass; past that the fuel stays spent so the chunk still ends. Spread each
builder's first condition-cache expiry over three passes by a per-brain serial, so
the whole list is no longer re-evaluated in one pass. Memoise unit counts per pass
by category text, since expression trees are rebuilt on every call and cannot be
keyed by identity. All three are deterministic.

**Alternatives and consequences.** Raising the 20-million budget hides the cost and
depends on machine speed not at all but on corpus growth. An uncatchable error does
not exist in Lua: a chunk that swallows the error in `pcall` forever cannot be stopped
from inside the VM, so the cap bounds what such a chunk is given, not whether it ends.
String categories keep counting zero as before rather than being parsed, to leave AI
behaviour unchanged. Staggering changes when a condition is re-checked, so the duel's
course differs from the earlier hash log; the outcome is recorded in
`docs/skirmish-economy-acceptance.md`.

## ADR-101 — Ribbon trails over a recorded path, identified by a cosmetic serial

**Context.** A PolyTrail is a ribbon of the projectile's recent positions; the straight
one-tick strip of ADR-096 cannot show an arc bending. The sim compacts its projectile
list on impact and offers no identity across ticks.

**Decision.** `ProjectileTrails` (presentation state on the scene) records positions
once per tick and gives each shot a serial in a cosmetic `Projectile` field the state
hash never reads, alongside the launch origin so the ribbon starts at the muzzle. Per
frame it emits one strip per segment within the authored TrailLength, each carrying a
trail-relative coordinate (`Particle::trailRange`, flag bit 1) so texture, ramp and
colour mix read head-to-tail across the whole ribbon. A dead shot's ribbon keeps moving
at its last speed and drains out; visibility is decided at draw time by the head.

**Alternatives and consequences.** Keying by shooter and tick collides on salvos.
Emitting persistent per-tick puffs cannot bend a textured ribbon or apply a ramp along
it. Particle grows from 112 to 120 bytes. Bolt strips skip ribbon materials; the gallery
still previews them straight. TextureRepeatRate is interpreted as repeats per ogrid of
TrailLength without a retail shader read.

## ADR-102 — Answer GridReclaim from a native per-cell reclaim snapshot

**Context.** The hosted FAF AI read `aiBrain.GridReclaim` 783 times per duel and every
read failed closed; retail's GridReclaim.lua is event-driven over prop objects this
adapter does not mirror. Only `ReclaimAvailableInGrid` executes in our slice.

**Decision.** The decision-pass snapshot carries wreck mass, energy and count per
retail grid cell (Grid.lua geometry: sixteen cells a side, eight on a 256 map), summed
from the same feature pool the harvest pass drains. A small Lua view on the brain
answers `ToGridSpace`, `ToCellFromGridSpace` and `MaximumInRadius` from it, and the
engineer-manager stand-in gains the `Location` the condition reads. Missing cells are
empty; a snapshot without a grid means no reclaim.

**Alternatives and consequences.** Running GridReclaim.lua needs prop objects,
reclaim events and surface heights the sim does not publish. The condition's result
changes no decision yet: reclaim builders carry a platoon state machine the driver does
not dispatch. Platoon readers that also need GridBrain remain unserved.

## ADR-103 — Original projectile meshes through the unit pipeline

**Context.** Retail shells, bombs and missiles are meshes, not strips: 43 projectile
blueprints sit beside a `_lod0.scm` and 65 more name a mesh blueprint through
`Display.MeshBlueprint`, many sharing `/meshes/projectiles/missile_default_mesh.bp`.

**Decision.** At content load, every projectile blueprint's mesh is located by the one
file-name rule of BlueprintMesh.hpp — beside the named mesh blueprint when there is one,
beside the projectile blueprint otherwise — loaded once per distinct mesh into its own
`UnitBatch`, textured from the files beside it, and scaled by `Display.UniformScale`
times the ogrid. Per frame the scene appends one `UnitInstance` per visible in-flight
shot with a mesh, yawed to atan2(vx, vz) and pitched to minus the climb angle, team
coloured by the firing army and extrapolated by the frame fraction; the bolt strip skips
those definitions. Mesh batches carry no unit slots, so picking never lands on a shot.

**Alternatives and consequences.** A dedicated projectile pipeline would duplicate the
unit shader for the same geometry. Lazy loading would need the VFS at draw time; eager
loading costs about a hundred small models at startup. A mesh blueprint's LOD 0
`MeshName`/`AlbedoName`/`NormalsName` override the rule when present, which is how
the shared default missile resolves. Not covered: mesh blueprints absent from the
archives (the Seraphim Laanse missile), `MeshScaleVelocity`, LOD cutoffs, the
`TMeshNoLighting` shader, and roll about the flight axis.

## ADR-104 — A script-object seam that names the store's two destruction moments

**Context.** WP-03 was analyzed but "not ready" for want of a script boundary to test:
retail resolves every native call's receiver fresh from `_c_object` (C-044), nulls it in
`~CScriptObject` (C-045), and distinguishes "death queued, object still readable" from
"pointer gone" (C-046). Fifty-four resolvers raise "Game object has been destroyed";
nineteen lifecycle queries accept a destroyed object.

**Decision.** `UnitStore::resolve(UnitId)` returns the slot and one of `Alive`,
`Destroyed` (health gone this tick, handle still held; the tombstone slot is readable)
or `Stale` (released in `retireDead`; the slot may belong to another unit). It is computed
on every call and never cached. `ScriptObject.hpp` maps the two retail resolver families
onto it: `requireAlive` yields the slot or the retail error string verbatim, and
`lifecycleSlot` accepts `Destroyed`; `beenDestroyed` is retail's `Entity:BeenDestroyed`.
Tests cover all three states, slot reuse, and hash neutrality.

**Alternatives and consequences.** Deferring `kill()` to the next tick would mirror
retail's purge window but flips `slotAlive` a tick later in every state hash and needs a
serialized pending list; the generation already gives the safety the delay exists for, so
tick order and hashes are untouched. Adopting the seam in the FAF driver (replacing its
per-pass index handles) is FA-LUA work and is not done here.

## ADR-105 — Unit reclaim as its own command kind, and the C-157 exemption on top

**Context.** Retail's `IsTargetExempt` keeps a unit from shooting what an own-side engineer
is reclaiming or capturing. The 2026-09-02 gate refused the predicate until a typed unit-work
target existed, because `Reclaim` names a feature and `FeatureId`/`UnitId` share values.

**Decision.** `CommandKind::ReclaimUnit` is the tag: one kind names a unit, the other a
feature, and no handle is ever compared across streams. It executes with FAF's unit
`GetReclaimCosts` formula (work = max(build cost mass, energy) at the reclaimer's BuildRate
per second), C-147's proportional credit, and the fraction falling as health (C-099); a fully
reclaimed unit is destroyed without a wreck, as `OnReclaimed` calls `Destroy`. Own and
allied units are refused. Per tick, `collectUnitWorkClaims` derives claims from the queue
heads and `nearestTarget` rejects a claimed candidate ahead of `DoNotTarget`, dropping an
incumbent too. Save v17 admits the kind; command log v4 names it.

**Alternatives and consequences.** A tagged target field or variant would touch every
consumer and the save layout; the kind byte was already saved and logged. The FAF formula's
bit-for-bit match with the native one is unverified, `ReclaimTimeMultiplier` is taken as one,
and Capture is still absent, so the second retail pass has nothing to test.

## ADR-106 — Weapon fire by the blueprint's own XACT cue

**Context.** The wave banks decode (all retail entries are PCM) but carry no names, so
deaths and impacts played by bank index and every shot was one synthesised click. The
blueprints name their sounds through the sound bank: `Audio.Fire = Sound { Bank, Cue }`.

**Decision.** Parse `Audio.Fire` into the weapon. Read the companion `.xsb` (XACT2 format
43) far enough to turn a cue name into wave-bank entries: header, cue names, complex and
simple cues, sounds, clips, and the two wave-carrying event kinds the retail banks use
(one track with ranges; a weighted track list). `WeaponSounds` loads bank pairs as the
catalog names them and resolves a `WeaponFired` event's `UNIT:Label` key to one of the
cue's takes, picked by the shooter's handle so a replay sounds the same. Unresolved weapons
keep the synthesised shot.

**Alternatives and consequences.** A hand table of cue indices would repeat the sound
bank. The layout was measured on retail bytes rather than taken from a specification, and
anything the walk does not recognise ends that cue's list rather than failing the bank. Not
interpreted: pitch and volume ranges, RPC curves, categories, instance limits, `LodCutoff`
distances, loops.

## ADR-107 — Structures snap to the build grid by default; free placement stays a flag

**Context.** Structures were placed at the exact click. Supreme Commander sites them on the
one-ogrid build grid so skirts abut exactly and adjacency bonuses are a matter of placement;
this engine's adjacency carried a half-ogrid slack and counted overlap as contact only
because nothing snapped (C-074). Retail deposits sit at half-ogrid centres.

**Decision.** `PlacementMode { Grid, Free }` on the sim's `Terrain`, Grid by default, set by
`--placement grid|free`. The snap is applied ONCE, at command intake, for every structure
order whatever its source — the click, the panel, the AI, a replayed log — so the queue, the
finished-work match and the construction row all hold the same site; the ghost preview calls
the same rule. An even footprint centres on a grid line, an odd one on a cell centre;
deposit-bound structures keep the deposit centre. In Grid mode adjacency uses a zero
tolerance. Mobile products site at their factory and are untouched.

**Alternatives and consequences.** Snapping in the UI alone would leave the AI and the panel
path unsnapped and make replays depend on the client. Validating alignment instead of snapping
would turn misaligned AI orders into refusals. Retail's exact snap rule was not read from the
executable; the footprint parity rule matches the deposit positions and the skirt geometry the
repo already records. Fixtures that hand-place structures for other mechanics pin Free mode.

## ADR-108 — Engineer support: assist wins over repair on a building target; stations help unasked

**Context.** The sim's Assist order worked (rate lent within reach of the resolved builder,
C-183 chain), but a player could not see it: the production panel showed progress only, the
rack had no Assist button, and a right-click on a damaged factory issued Repair, never Assist.
Engineering stations (FA `ENGINEERSTATION`: XEB0104/0204, XRB0104/0204/0304) did nothing and
could not be ordered — their blueprints forbid Guard and they cannot move.

**Decision.** (1) Right-click ladder: a target that owns an unfinished construction (matched
by `Construction::builder`, the link the panel and the assist scan use) gets Assist even when
damaged; Repair remains one rack click away. (2) Assist has its own rack descriptor in FA's
first unit-specific slot, enabled for any builder. (3) The production panel prints
`ASSIST +N/S` from `assistPerTick` while a helped build runs. (4) An idle station — alive, no
order — lends its rate to the nearest unfinished allied construction within `constructionReach`;
with none in reach it repairs the nearest damaged ally within repair reach (build before
repair, the guard ladder's order; nearest first, lowest index on a tie). A station with an
order of its own is not idle. The player's answer (2026-09-07) was "assist and repair in reach".

**Alternatives and consequences.** Folding Assist into the Guard button as FA does would hide
a distinction this sim keeps. Modelling pods (XEA3204) as units that travel was rejected: the
station's reach stands for the pods' leash, and the cost is that a station out of reach of
everything is inert, as in retail. Auto-help runs from positions the hash covers, so it is
replay-stable; it changes any golden with an idle station in reach of work (none recorded).

## ADR-109 — Auto-expand is an app-level standing order that issues ordinary logged builds

**Context.** The player asked for engineers that keep claiming mass and hydrocarbon deposits
on their own, preferring the part of the map without the enemy. The sim has no notion of a
standing "keep expanding" order, and inventing one would put map knowledge (markers, start
positions, factions' blueprints) into `core/sim`, which owns none of it.

**Decision.** The order lives in the app's `MatchRunner` (`autoExpanders`), toggled from a
rack cell (`RackAction::AutoExpand`, FA's second unit-specific slot, lit while on for the whole
selection). Each tick before anything else thinks, `runAutoExpansion` gives every flagged
engineer with an empty queue — and no order still staged in the intake — one Build through
`issueBuild`, so the command log holds every order and a replay reproduces the match with no
knowledge of the flag (the pass does not run in replay). The site is `pickExpansionDeposit`:
the nearest free deposit not nearer to a hostile army's start than to the army's own, falling
back to the nearest anywhere; free means no unfinished construction, no standing structure, no
queued or staged Build for it, nothing handed out earlier in the pass and nothing refused to
that engineer. Blueprint: the faction's cheapest extractor, or its cheapest `HYDROCARBON`
energy producer, provided the engineer's build tree names it. A manual order takes precedence
and the engineer resumes when idle; a site the sim accepts and then drops at dispatch is
remembered as refused, which is what stops the pass re-ordering it every tick.

**Alternatives and consequences.** A sim `CommandKind` would have been replay-visible for free
but needs the world's markers inside the sim. Scoring with a soft enemy-distance penalty was
dropped for the simple own-side/other-side split: it is explainable and the bisector already
is "away from the enemy". The claim test is omniscient about enemy structures (the app knows
the whole map); a fogged-in enemy extractor still stops the engineer, by the approach check.

## ADR-110 — Build-site validation and builder approach use separate terrain domains

**Context.** The four-faction headless naval-placement scenario exposed an accepted Build
that disappeared before construction: a valid water site was also being used to choose the
engineer's route grid, so an engineer standing on land could not start a water-only path.

**Decision.** Keep the product grid for footprint validation and supply the builder's own
movement grid for its approach. Command dispatch provides both; deferred and continuing
orders derive both from the existing per-type grid table. The single-grid simulation API
retains its existing default for callers whose builder and product share a domain.

**Alternatives and consequences.** Moving the fixture into water would conceal a player
workflow failure. Relaxing water placement would allow shipyards on land. Separate grids
preserve both restrictions without adding serialized state or changing path-service order.
Immediate and queued shore-to-water construction are checked using all four retail engineer
and shipyard definitions. This establishes the supported amphibious/hover engineer path;
it does not add arbitrary shoreline reach searches for land-only builders.
