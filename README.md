<!-- Generated and maintained by Claude -->
# recoil-metal

A Mac-native, Metal-only renderer that reads **Recoil** content formats —
research spun out of [FAR](../forged-alliance-reborn/README.md).

The original premise was that FAR was *blocked* on macOS: Recoil's model path
needs OpenGL 4.3+ SSBOs and Apple caps OpenGL at 4.1. That turned out to be
wrong, and usefully so — Apple's OpenGL is not the only OpenGL on the platform.
FAR now renders on this Mac through **SDL3 → Mesa EGL → Zink → kosmickrisp →
Metal** (see FAR's `docs/recoil-macos-rendering.md`). The 4.1 cap is irrelevant.

That makes the question sharper rather than moot:

> Recoil reaches the GPU on Apple Silicon through four translation layers. What
> does that cost, and what does a purpose-built Metal renderer buy back — and
> what does a modern C++ engine core look like when built test-first, without
> 20 years of legacy coupling?

It also makes the answer *measurable*: milestone 4 now has a working OpenGL
baseline to benchmark against on the same machine, which it previously did not.

This is a personal research project. It is also a deliberate modern-C++
showcase: C++23, RAII everywhere, rule of five, pImpl at platform boundaries,
TDD with Catch2, warnings-as-errors.

## What it looks like

Every image below is this renderer's own output, written by `--screenshot`.
Both content families throughout: Recoil/BAR maps and models, and Supreme
Commander `.scmap` maps with `.scm` models.

|  |  |
|---|---|
| ![a BAR map with sky and water](docs/images/m11-sky-water.jpg) | ![the same on a Supreme Commander map](docs/images/m11-sky-water-fa.jpg) |
| **Sky and water**, ported from the games' own `sky.fx` and `water2.fx` | The same code on a `.scmap` — the horizon colour is the map's, not ours |
| ![the ground splat up close](docs/images/m6-splat-close.jpg) | ![Seton's Clutch from above](docs/images/m6-splat-setons.jpg) |
| **The Supreme Commander ground splat** — nine tiled strata assembled per frame from a recipe, since a `.scmap` bakes no ground texture at all | Seton's Clutch, whole-map |
| ![shadows across the terrain](docs/images/m10-shadows.jpg) | ![800 units](docs/images/m10-crowd.jpg) |
| **Shadow mapping** from a camera-following light box | **800 instanced units**, each on its own animation phase |
| ![units rallying, 5 seconds in](docs/images/m8-rally-05s.jpg) | ![the same order, pathed round terrain](docs/images/m9-rally-pathed.jpg) |
| **Click-to-move**, five seconds after the order | The same order once **A\* pathfinding** landed — units route round what they cannot climb |
| ![refraction and per-map sky](docs/images/m12-water-refraction.jpg) | ![a 4096-square map](docs/images/m10-4096-map.jpg) |
| **Refraction and planar reflection** — the sea absorbs by Beer-Lambert, so a shallow sandy bottom stays sandy | A **4096-square** map, decimated to fit rather than refused |
| ![selection rings](docs/images/m13-selection-rings.jpg) | ![stratum normal maps](docs/images/m13-stratum-normals.jpg) |
| **Selection rings** that follow the ground rather than hovering flat over it | **Per-stratum normal maps**, at the scale an 8-elmo height sample cannot reach |
| ![a forested Supreme Commander map](docs/images/m14-props-forest.jpg) | ![dust behind a moving tank](docs/images/m14-dust.jpg) |
| **The scenery a map places** — 5182 props here, and 46 971 on the busiest stock map | **Dust**, aged entirely in the vertex shader |

More in the milestone notes below: [`m5-units-close`](docs/images/m5-units-close.jpg),
[`m5-units-wide`](docs/images/m5-units-wide.jpg),
[`m8-rally-50s`](docs/images/m8-rally-50s.jpg),
[`m10-water`](docs/images/m10-water.jpg),
[`m10-water-fa`](docs/images/m10-water-fa.jpg),
[`m12-map-sky-coast`](docs/images/m12-map-sky-coast.jpg),
[`m12-map-sky-desert`](docs/images/m12-map-sky-desert.jpg).

## Why not port Recoil class-by-class

Measured on the real tree (`reference/RecoilEngine` in FAR, Spring/Recoil
`rts/`): of ~1.95M lines, **1.57M is `rts/lib/`** — vendored third-party code
nobody rewrites. The engine proper is ~385k lines:

| Subsystem | kLOC | Plan |
|---|---|---|
| Sim (units, weapons, pathing) | 83k | later, *semantics* reimplemented — not classes |
| Lua bindings | 74k | never port; reimplement minimal when needed |
| Rendering (OpenGL) | 67k | **replaced — this project** |
| System (SDL, threads, FS) | 65k | take only what's needed |
| Game / ExternalAI / RmlUI / Net / Menu | ~81k | never port |

Two facts make the strategy possible: `rts/Sim` includes **zero** GL headers,
and only 18 sim files touch rendering globals — the sim↔rendering boundary is
real. But a horizontal port (Sim → Lua → Game → System) still means ~250k
lines before one pixel renders. So instead: **vertical slices through data
formats**. Loaders are small and data-shaped; the renderer is new.

The rule, inherited from FAR: *assets/formats convert, behaviour gets
reimplemented.*

## Milestones

1. **Window + runtime-compiled shader.** NSWindow, `CAMetalLayer`, vsync via
   `CAMetalDisplayLink`. The shader pipeline is built from *source at runtime* —
   there is no Xcode on this machine, hence no offline `metal` compiler. This
   milestone existed to prove that toolchain story holds. **✔ done**
2. **SMF terrain.** SMF header + heightmap loader adapted from Recoil
   (`rts/Map/SMF/`), full-resolution heightmap mesh with central-difference
   normals, depth buffer, Lambert shading coloured by elevation, orbit camera.
   Verified against a real BAR map (Angel Crossing 1.4: 1024×1024 squares,
   1.05M vertices, 2.1M triangles). **✔ done**
3. **Textured terrain.** SMT tile decoding, an 8192x8192 BC1 ground atlas
   uploaded without transcoding, a real `mapinfo.lua` reader, and Recoil's
   fixed y=0 water plane. **✔ done**
4. **Benchmark harness.** Same map, both renderers, vsync off on both.
   **✔ done** — Recoil through zink: **7.416 ms/frame mean**. recoil-metal
   native: **0.554 ms**. A 13.4× gap, with the scope limits spelled out in
   [`docs/benchmark-m4.md`](docs/benchmark-m4.md) — it is a full engine frame
   against a terrain draw, not the same work through two APIs. Enough headroom
   to justify continuing, which is what this milestone existed to decide.
5. **Units — Recoil content first.** `.s3o` loading, piece hierarchy, instanced
   rendering, DDS textures. **✔ done** — models render on the terrain:
   validated against all **2034** BAR `.s3o` models and **2552** `.dds`
   textures, placed at map start positions plus a deterministic scatter.
   800 instances of a 3980-triangle model costs 2.29 ms/frame (436 fps)
   against 0.69 ms for terrain alone. Both texture channels are honoured —
   tex1's alpha is the team-colour mask, tex2 carries self-illumination and
   reflectivity — shaded by a port of the engine's own model shader
   (`ModelFragProgGL4.glsl`). Several models per scene, ordered so each texture
   *pair* binds once a frame — the unit Recoil batches on too.

   Deliberately *not* glTF. The engine must eventually handle both Recoil and
   Forged Alliance content, but glTF is nobody's native format — it is FAR's
   Blender-produced intermediate, it costs a JSON dependency, and Recoil discards
   glTF animation tracks entirely. Supreme Commander's own `.scm`/`.sca` are plain
   fixed-stride binary (608 and 139 files on disk, 228-line reference reader) and
   carry the animation glTF loses. Both native formats decode into one shared
   model struct, so the FA path is additive rather than a rewrite. See
   [ADR-004](ADR_DECISIONS.md).

6. **Supreme Commander maps.** `.scmap` v60, decoded byte-exactly and validated
   to EOF on all 60 retail stock maps. The heightmap lands in the *same*
   `HeightField` the SMF loader fills — that seam was designed for this at
   milestone 2 and cost nothing to collect on. Which loader runs is decided by
   the file's magic, not its extension, so nothing downstream knows which family
   a map came from. **✔ done** — geometry, terrain-type ground colour, and the
   per-map water plane (17 of the 60 stock maps are dry; Recoil's is a fixed
   plane at y=0), and start positions.

   Start positions were the surprise: `.scmap` carries none, and neither does
   the `_scenario.lua` that looks like it should. They live in `<map>_save.lua`
   as `ARMY_<n>` markers, alongside dozens of transport and mass markers. That
   file wraps every leaf in a data constructor — `VECTOR3( 672.5, 18.7, 346.5 )`,
   `GROUP { ... }` — so the Lua reader learned exactly six of them by name, which
   is data in call syntax rather than an interpreter. All 61 stock `_save.lua`
   files parse; the 54 skirmish maps yield starts and the 7 campaign maps
   correctly yield none, because their armies are spawned by mission script.

   **The ground splat, too.** SupCom bakes no ground texture: a `.scmap` names
   nine tiled layers that live in `env.scd` and embeds only the two masks that
   weight them, so the ground is assembled every frame from a recipe rather than
   loaded as a picture. Layer 0 covers the map; layers 1-8 are laid over it in
   order, weighted per texel by mask A's `rgba` then mask B's — a chain of mixes
   rather than a weighted sum, since a sum washes out wherever two strata meet.
   18 of the 57 loadable stock maps use the upper four strata, so mask B is not
   decoration.

   Three things the corpus decided rather than the plan. The stratum `scale` is
   **ogrids per texture repeat**, not repeats per map — the stock maps set the
   macrotexture to 128 on 256- and 2048-square maps alike, which is only sensible
   as a physical size. The layers are **not** a `texture2d_array` as planned:
   they come in eight distinct size/format combinations (256²-1024², BC1/BC2/BC3
   and uncompressed BGRA8), and an array demands one of each, so they bind
   individually — no transcode, no lost mips. And they need their **own repeating
   sampler**: the existing one clamps, correctly, because the atlas and both
   masks cover the map exactly, but a layer's uv reaches into the hundreds and
   under clamping every repeat past the first samples the edge texel, which
   renders as a smooth smear that reads as a missing texture rather than a wrong
   sampler.

   Two details come from the engine's own shader rather than from inference.
   Supreme Commander ships its HLSL in `gamedata/effects.scd`, and
   `effects/terrain.fx` contains `TerrainAlbedoXP` — exactly this path, eight
   strata through two masks. It confirms the blend chain, and settles two things
   guessing had got wrong or left out. The masks are **expanded**, not raw:
   `saturate(mask * 2 - 1)`, so the bottom half of the range means *absent*
   rather than *a little*, and using the raw value bleeds every stratum across
   the map at up to half strength. And the macrotexture (slot 9) is lerped over
   the result **keyed on its own alpha**, not on a mask channel and not
   multiplied — `lerp(albedo, upper.rgb, upper.w)`.

   Costs 2.063 ms GPU against 0.864 for the single-fetch path — twelve texture
   reads instead of one — and leaves the Recoil path untouched at 0.546 ms.

   Deliberately not done: the shipped per-map DXT5 normal map is parsed and
   validated but unused. Its convention is now known — `terrain.fx` reads it
   `.xyz * 2 - 1`, straight, with no swizzle — so what remains is wiring it into
   the terrain lighting rather than a question about the data.

   Maps above 2048 squares are still refused rather than half-loaded: their mesh
   alone would want ~800 MB and there is no LOD yet.

7. **Supreme Commander models and animation.** `.scm` loads into the *same*
   `Model` struct `.s3o` does — validated against all **1148** retail models
   (8379 bones, 1.23M vertices, 838471 triangles), cross-checked against an
   independent Python read. `.sca` gives what glTF would have lost: real
   keyframed bone animation, all **474** retail animations decoding to the byte.
   **✔ done** — a walk cycle plays on a Supreme Commander bot standing on a
   Supreme Commander map, next to Beyond All Reason units, in one scene and one
   binary.

   Two conventions genuinely differ between the families and `Model` records
   which one a model follows — vertices are bone-local in `.s3o` and model-space
   in `.scm`, and the team-colour mask lives in tex1's alpha versus
   `_SpecTeam`'s. Everything else is family-blind. See
   [ADR-005](ADR_DECISIONS.md) and [ADR-006](ADR_DECISIONS.md).

8. **Movable units.** Click to select, right-click to order. **✔ done** — the
   first thing in the engine whose state changes between frames.

   Everything before this was static once uploaded. Instances were baked into a
   GPU buffer at load and never touched again, and animation playback is
   deliberately a *buffer offset* into pre-baked poses so that a moving model
   costs no per-frame CPU work at all. Movement is what finally needs the other
   path, and it needs it safely: instance storage is `StorageModeShared`, so
   rewriting it while the GPU may still be reading last frame's copy is a plain
   data race. It became a three-deep ring with a completion-handler semaphore —
   the same `kMaxFramesInFlight` the offscreen benchmark already ran on — and
   drawing picks a slot by offset, exactly as animation picks a pose.

   The sim is movement and nothing else: units walk straight lines and pass
   through each other and through cliffs. Pathfinding and collision are the
   Stage B cliff below. It ticks at **30 Hz**, matching Recoil's own `GAME_SPEED`
   (`GlobalConstants.h:52`) — worth matching exactly rather than picking
   something convenient, because every speed and turn rate in the content is
   authored against that rate. Speed and turn rate default to BAR's Pawn (87
   elmos/second, and a `turnrate` of 1214.4 converted out of Recoil's circle
   divisions per frame to 3.49 rad/s), so the numbers are representative rather
   than invented.

   Three things fell out of building it that a plan would not have predicted.
   Facing is `atan2(dx, dz)`, not the usual `atan2(z, x)` — the vertex shader
   maps a model's local +Z to `(sin yaw, cos yaw)`, and swapping the arguments
   compiles, runs, and renders every unit walking sideways. Forward speed scales
   with `cos` of the remaining heading error, which makes a unit pivot in place
   before setting off instead of driving away and arcing back, and needs no
   arbitrary "turn until aligned" threshold because the cosine already is one.
   And `setUnits` **reorders** batches by texture pair, so the caller's batch
   index is not the renderer's slot index — pushing instances by the wrong one
   silently moves the wrong model.

   The nearest-corner ground sampler from milestone 5 had to go: it is exact at
   every corner and wrong everywhere between, and a unit crossing a square held
   its height for four elmos and then jumped the whole difference. Placement and
   the sim now share one interpolating sampler, because if they disagree about
   where the ground is a unit jumps the instant it is first stepped.

   Picking is arithmetic on the heightfield rather than a read-back of a depth
   or ID buffer: marched at half-square steps and bisected, which costs a GPU
   round-trip nothing and makes "does clicking there select that unit" a test
   instead of something to squint at.

   ![units rallying, 5 seconds in](docs/images/m8-rally-05s.jpg)
   ![the same order 50 seconds in](docs/images/m8-rally-50s.jpg)

   Deliberately not done at the time: **slope alignment** (`UnitInstance`
   carries a single yaw by design, so tilting to the terrain means growing a
   struct the shader reads verbatim). The other two deferrals — per-instance
   animation phase and foot sliding — are milestone 9.

9. **Squads, pathfinding, and the unit shader's last guess.** **✔ done.**

   **Each instance gets its own animation phase.** Playback used to bind the
   bone buffer at one pose's offset for the whole batch, so every unit in it
   shared a clock and a squad walked in perfect lockstep — which reads as one
   unit drawn several times. The whole keyframe buffer is now bound and the
   vertex shader indexes it per instance. ADR-006's bargain survives: poses are
   still baked once at upload, and nothing is written per frame.

   **The walk cycle is paced by ground covered, not by wall time.** A clock and
   a walk cycle disagree constantly — a unit standing still keeps striding, one
   pivoting on the spot keeps striding, one that has arrived strides forever.
   Distance has none of those cases. The stride is `speed * duration`, the
   distance one cycle covers at full speed, which is the assumption the
   animation was authored under; at top speed the cadence is exactly what the
   clock gave, and every slower case falls out for free. A batch declares which
   of the two drives it, so headless captures stay on the clock and `--time`
   keeps meaning something.

   **Units route instead of walking through things.** Passability is a coarse
   grid — 8×8 heightmap squares per cell, so a 1024-square map searches 16k
   nodes rather than a million — from two of the engine's own rules. A face's
   slope is `1 - normal.y`, the quantity Recoil's slope map holds
   (`ReadMap.cpp:778`), compared against `1 - cos(clamp(deg, 0, 60) * 1.5)`
   exactly as `MoveDefHandler.cpp:84` computes it — the 1.5 and the clamp mean a
   unit def's nominal 0..60 really spans 0..90 degrees of ground. Water is a
   *depth* limit rather than a line, because a unit fords shallows. Both
   defaults are BAR's Pawn again: `maxslope 17`, `maxwaterdepth 12`.

   The search is A* with an octile heuristic; diagonal steps require both
   orthogonal neighbours open, or units clip the corners of cliffs. An
   unreachable destination means the unit stays put rather than setting off into
   the sea — on an island map like Angel Crossing that is 27 of 120 units
   standing still, which the app says out loud so it does not read as a bug.

   **And `_SpecTeam`'s channels are no longer guessed.** ADR-005 left green and
   blue unused because nothing verified said what they held. `effects/mesh.fx`
   settles all four: red multiplies the environment reflection, green scales an
   additive Phong highlight, blue is emissive at `glowMultiplier` 2.0, alpha is
   the team mask. The old reading conflated two independent channels — it drove
   both the highlight and the reflection from red — and rendered every emissive
   surface unlit. It also exposed a fallback bug: a model with no `_SpecTeam`
   defaulted to alpha 1, which under that layout means *paint the whole thing in
   the team's colour*. Both normal-map conventions came out of the same read and
   they differ, so neither could have been assumed from the other: model normal
   maps are DXT5nm-swizzled (`.gaa`, Z reconstructed), the per-map one is
   straight `.xyz`. See [ADR-012](ADR_DECISIONS.md).

   Still not done: slope alignment, and unit-unit collision — units mass at a
   rally point by standing inside each other.

10. **Content, volume, scale and light.** **✔ done** — five things that each
    removed a lie the renderer was telling.

    **Units read their own definitions.** BAR ships 968 Lua files; 943 units
    parse out of them. Speed is already elmos/second in the modern field, turn
    rate is circle divisions per *frame* over 65536 to the circle, and
    footprints carry the engine's scale of 2 plus its lower clamp — which earns
    its keep, since several units declare `footprintx = 0` and would otherwise
    occupy no space at all. 36 files are refused, every one for the Lua
    reader's stated contract: a value needing evaluation, an expression calling
    into the engine, or units built in a loop. Aircraft turned out to carry a
    `turnradius` and no turn rate at all, so `canFly` is read too.

    **Units have volume.** A rally point used to be 93 models standing inside
    each other. Overlapping pairs are pushed apart by half the overlap each,
    over a uniform grid two radii across. Exactly coincident units — which is
    precisely what a rally order produces — take a direction from their pair of
    indices, so a stack fans out instead of collapsing onto one axis.

    **Large maps load.** Above 2048 squares a map was refused outright: the
    mesh wanted 806 MB. The builder now decimates at load, so a 4096-square map
    costs what a 1024-square one does. The cap moved to 8192 and now bounds the
    *heightfield*, which is not decimated. Lifting it also made an old corpus
    test come true — it asserted the embedded normal map is always one DXT5
    tile, noting that only the three 4096-square maps store four and that they
    were refused for size "so that the day it is not, this says so". It did.

    **Shadows.** A depth-only pass from the sun, four-tap filtered through a
    comparison sampler. Two things had to be right and the first attempt got
    both wrong. The light box follows the *camera*, not the map: covering the
    whole map is 4 elmos per shadow texel and a depth range spanning the map's
    diagonal, so the bias needed to stop the ground shadowing itself is most of
    a tank's height — the shadow lifts off its caster and nothing renders. And
    the bias is applied in the shader, scaled by how obliquely the sun strikes
    the surface, because a single constant either leaves oblique faces striped
    with acne or lifts the shadows off everywhere else.

    **Water.** Was a flat translucent quad. It is now a grid whose vertices
    carry their own depth, so shorelines fade rather than ending in a line;
    shallow water is green-grey and deep water blue, because water absorbs the
    red end first. Reflectance follows Schlick — 0.02 head-on, a mirror at
    grazing angles — which is most of what makes water look wet. The two wave
    trains run at oblique, incommensurable angles: aligned to X and Z their
    crests intersect on a regular lattice and open water reads as graph paper.

    Shadows cost 1.060 → 1.668 ms GPU on 200 units at 1920×1080 (1492 → 783
    fps); water is free within noise at 1.681 ms. The shadow pass re-submits
    all 2.1M terrain triangles even though the light box covers a fraction of
    the map, so culling to the box is the obvious next saving.

    Also in: an asset search path with `.sdz` extraction, slope alignment, and
    multi-unit selection. Still absent: per-unit passability (one grid serves
    the whole scene, so a unit's own `maxslope` is not honoured), and units of
    different models still pass through each other.

11. **Closing the loose ends.** **✔ done** — five items that were each a known
    lie or a measured cost.

    **Collision sees across models.** Instances are held one array per model,
    and the separation pass ran once per array — so two units of *different*
    models could stand in exactly the same spot with neither pass able to see
    the other. It now takes a list of groups and flattens them into one index
    space.

    **Each unit routes on the map its own limits see.** Definitions had
    supplied `maxslope` and `maxwaterdepth` since they were read, but one grid
    served the whole scene. Grids are now built per distinct pair of limits and
    cached — keyed on the limits, not the unit type, since the grid depends on
    nothing else. On Angel Crossing that is 59% of the map walkable to BAR's
    Pawn against 55% to its Stumpy tank.

    **The shadow pass is culled to the light box** — 1.082 → 0.705 ms GPU on a
    close camera, a third off. The terrain is emitted in 64-square chunks, each
    a contiguous range of the index buffer with its own bounds. Two details
    matter: chunks are emitted chunk-by-chunk rather than row-by-row, or their
    triangles are not contiguous and cannot be drawn alone; and survivors are
    merged into runs, without which culling trades one draw of the terrain for
    one per chunk and is *slower* whenever the camera is far enough back to
    keep them all — which is exactly what the first version measured.

    **A sky, and water ported from `water2.fx`.** Every other shader here is a
    port with a `file:line` citation; the water was mine. Three of the engine's
    constants are not what one would guess: `waterLerp` is
    `clamp(depth, 0.3, 0.3)` — a *constant* 0.3 of `waterColor`, not a depth
    ramp — the depth dependence living in `skyreflectionAmount * saturate(depth
    * 10)` instead; and Fresnel is bias 0.1 with power 1.5, far softer than a
    physical Schlick 5, which is why the engine's water reflects noticeably
    even looked at straight down. The sky is `AtmospherePS`'s horizon-to-zenith
    lerp, drawn as one full-screen triangle at the far plane, and the water
    reflects the same function — so the sea and the sky above it agree.

    ![sky and water on a Recoil map](docs/images/m11-sky-water.jpg)
    ![the same on a Supreme Commander map](docs/images/m11-sky-water-fa.jpg)

    Still absent: per-map sky and water colours (a `.scmap` carries them in its
    skybox block, which the loader parses but does not expose), refraction and
    planar reflection, and dynamic per-patch terrain LOD.

12. **Culling, per-map environment, and real water.** **✔ done.**

    **Both terrain passes cull now**, sharing one cull-and-merge helper and
    differing only in the predicate — the light's box for the shadow pass, the
    view frustum for the visible one. Frustum culling took the close camera
    from 0.702 to **0.344 ms**. Plane extraction lives in `core/` with tests,
    because two details are easy to get wrong and invisible until they bite:
    Metal's clip depth runs 0..1, so the near plane is row 2 alone rather than
    OpenGL's row3 + row2 — use the OpenGL form and everything near the camera
    vanishes — and the box test must keep anything that merely *straddles* a
    plane, or terrain clips at the screen edge as the camera turns.

    **Detail varies with distance.** Each chunk carries three index sets and
    the draw picks by distance: whole-map view 2.447 → **1.472 ms**. Not
    stitched — neighbouring chunks at different levels can crack in principle,
    though the transition is eight chunks out where a one-level difference is
    subpixel, and none was visible. The honest fix is skirts.

    **Maps state their own sky and sea.** A `.scmap` carries a lighting block
    and a water block; both were parsed only to prove the layout, so every map
    got `water2.fx`'s stock values and every horizon looked the same. Reading
    them means a desert map gets a warm sandy sky and a coastal one green-grey.
    Validated across all 60 retail maps — and the assertion that matters is
    that more than five *distinct* fog colours come back, since a parse landing
    on a constant passes every range check.

    **Water refracts.** On a tile-based GPU the fragment shader can read the
    colour already in the render target, so the scene behind the water needs no
    second pass at all — that is the engine's refraction input arriving free.
    It is absorbed by Beer-Lambert rather than lerped toward a painted colour,
    so a shallow sandy bottom stays sandy and a deep one goes blue without
    either being painted that way. Planar reflection is a real second pass and
    the expensive one, and its reflected camera cannot be an `OrbitCamera`:
    that type clamps pitch positive and a mirrored camera looks *up* from below
    the surface.

    ![refraction and per-map sky](docs/images/m12-water-refraction.jpg)

    Selection rules moved to `core/scene/Selection.hpp` with nine tests. They
    had lived inside an AppKit callback, where the only way to check whether
    shift-click adds was to click.

13. **Interface, detail, and two quality switches.** **✔ done.**

    **Selection rings** — the one piece of interface this renderer draws.
    Selection had been a white tint on the model; a ring on the ground reads
    better, and it *conforms* to the terrain rather than being a flat disc
    tilted by the surface normal. That matters more than it sounds: a ring is 8
    to 40 elmos across and a heightfield square is 8, so a flat ring spans
    several squares of real relief and buries a third of itself in any slope
    that is not planar. Each vertex takes its height from the ground under
    *it*, which is also why a ring on a hillside stays level with the hill
    instead of with the unit standing on it. Pure geometry, so ten tests pin
    the shape — including that a ring wider than twice its radius clamps
    rather than folding through the centre into a bow tie, which happens for
    real, since a two-square footprint is a radius of eight elmos.

    ![selection rings on the ground](docs/images/m13-selection-rings.jpg)

    **Per-stratum normal maps**, ported from `terrain.fx`'s `TerrainNormalsXP`.
    A `.scmap` names a normal map beside each stratum's albedo — nine against
    ten, the macrotexture having none — and the loader had been parsing them
    and dropping them. They put grain in grass and relief on gravel at the
    scale a heightfield sample (8 elmos) cannot express at all.

    | with | without |
    |---|---|
    | ![stratum normals on](docs/images/m13-stratum-normals.jpg) | ![and off](docs/images/m13-no-stratum-normals.jpg) |

    Two things the engine's own shader settled and one it could not. It
    confirmed the blend is the same chain of lerps as the albedo. It revealed
    an asymmetry that looks like a typo and is not: `TerrainAlbedoXP` expands
    its masks — `saturate(m * 2 - 1)` — while `TerrainNormalsXP` twenty lines
    later reads them **raw**. What it could *not* settle is which channel points
    up, because it hands the sample straight to a lighting function whose own
    space is muddled by `.xzy` swizzles. So that was **measured** instead: a
    corpus test reads the block endpoints of thirty real stratum normal maps
    and asserts blue is the channel sitting near +1. A normal map read on the
    wrong axis lights bumps as dents, which survives a look at the screen.

    **Skirts** close the per-chunk LOD cracks milestone 12 shipped with. Both
    sides of a boundary need one — where a chunk's rim is above its neighbour's
    chord its own skirt covers the gap, where below, the neighbour's does — and
    the depth is derived from the worst gap a coarse chord can leave along that
    chunk's rim, not a constant. A constant deep enough for a cliff would hang
    into open air wherever the ground beside a chunk falls away. Flat ground
    gets a skirt of depth zero. Cost: +11.2% vertices and +0.05 ms.

    **A fix that no screenshot could have found.** Milestone 12's per-chunk LOD
    emitted each chunk's three levels back to back, which put chunk N's coarse
    ranges between chunk N's fine range and chunk N+1's — so no two chunks were
    ever adjacent at the level they were drawn at, and the cull-and-merge from
    milestone 11 had not fired since. Terrain was one draw per chunk for a
    whole milestone. A renderer issuing 256 draws where one would do renders an
    identical image; it took asserting the adjacency the merge depends on,
    which is now a test.

    **Two quality switches**, `r` and `n` live, or `--no-reflections` and
    `--no-stratum-normals`. Both features are worth their cost only if you can
    see what they buy, and a side-by-side of two runs cannot show a difference
    moving:

    | | GPU | vs off |
    |---|---|---|
    | planar reflection | 2.527 ms | +0.50 ms |
    | stratum normals, naive | 4.604 ms | +1.34 ms |
    | stratum normals, skipping absent and zero-weight | 3.529 ms | +0.21 ms |

    That last row is two `continue`s. A map naming five strata was sampling
    four slots bound to a fallback texture whose contents were then multiplied
    away, and a stratum's weight is zero across most of the map — both skips
    leave the image bit-identical in intent, and take 6.5× off the feature's
    cost.

14. **Scenery, and the answers to three open questions.** **✔ done.**

    **Props.** The map corpus turned out to be full of scenery nobody had counted.
    Feature rendering had been recorded as blocked because BAR's aw04 declares zero
    features in its SMF block and places its objects through a runtime Lua gadget —
    but the Supreme Commander side was never looked at, and **59 of the 60 stock
    maps carry props: 418 942 of them**, a median of 4355 per map and 46 971 on the
    busiest, across 207 distinct blueprints. The section was already being walked to
    prove the parse reached EOF, so reading it replaced four skips with four reads.

    | | |
    |---|---|
    | ![a forested map](docs/images/m14-props-forest.jpg) | ![individual conifers](docs/images/m14-props-close.jpg) |
    | **5182 props from 19 meshes and 5 textures** on SCMP_009 | Alpha cutouts up close — the leaf shape, not the quad |

    A prop is a static model with one texture, so it shares the unit pipeline and
    differs in two flagged things: no shading texture, and its albedo's alpha is a
    CUTOUT rather than a team-colour mask. That second one matters more than it
    sounds — both families keep a team mask in an alpha channel somewhere, so read
    as a mask a palm frond renders as a solid green card in the player's colour. It
    is a `discard` rather than a blend because scenery is drawn in arbitrary order,
    and a cutout is order-independent, which is what keeps 14 000 trees one draw.

    What it does *not* share is the unit list, for a reason about the sim rather
    than the GPU: everything in there is ticked, collided and pickable, so a tree in
    it would be shoved aside by passing infantry and would accept a move order.

    Two scale conversions, and both are needed. `UniformScale` in the blueprint
    takes the mesh to ogrids — cross-checked against the blueprints' own `SizeY`,
    the collision height in ogrids, which says 1 for both a palm and a pine whose
    meshes measure 0.96 and 1.18 once scaled — and an ogrid is 8 elmos. Miss the
    second and a pine is 1.2 elmos tall: a scatter of dark specks that reads as a
    texture problem rather than a units one. See [ADR-024](ADR_DECISIONS.md).

    **Props are culled by distance, per prop, using the cutoff their own blueprint
    states** — which is what makes zooming out free. A blueprint's LOD table gives a
    cutoff per level, and across the 335 shipped ones the furthest runs from 10 to
    1000 ogrids: a shrub stops being drawn at 800 elmos where a landmark tree
    survives to 8000. So the scenery thins from the small detail upwards as the
    camera pulls back, and at a whole-map framing every prop is past its own cutoff.

    | view | props on | props off | before the cull |
    |---|---|---|---|
    | SCMP_009 whole-map, 5182 props | 4.036 ms | 4.051 ms | +2.8 ms |
    | SCMP_005 whole-map, 46 971 props | 4.156 ms | 4.263 ms | +6.2 ms |
    | SCMP_009 at a working zoom | 10.700 ms | 10.530 ms | +2.8 ms |

    Nothing pops, and not by luck: because the cutoffs are graded per prop the
    disappearance is spread over the zoom range rather than being a cliff. Measured
    as the share of pixels the scenery accounts for while pulling back — 12.1% at a
    close zoom, 8.6%, 2.6%, then 1.11%, 0.33%, 0.05% and nothing. The last props to
    go are contributing a twentieth of one percent of the frame.

    Reading a blueprint needed two things of the Lua data reader. Its
    call-with-table allow-list grew from `GROUP` alone to the four names that appear
    across 335 blueprints and 61 stock maps. And `#` became a line comment, which is
    **not Lua** — real Lua allows it only on a first line and otherwise reads it as
    the length operator — but 47 blueprints use it as one anyway, so the game's own
    reader must. Accepted only at the start of a line, where all 47 sit; anywhere
    else it would swallow the rest of a line and quietly drop a field.

    **Dust**, on a new particle pass, and the first thing here that is neither
    terrain, model nor interface.

    ![dust behind a moving tank](docs/images/m14-dust.jpg)

    What the CPU uploads is a particle's *history* rather than its state — where it
    was born, the velocity it was born with, how long ago — and the vertex shader
    works out the rest. So nothing on the CPU integrates a position, and the quad is
    expanded from the vertex id and turned to face the camera in the shader, which
    means no geometry uploaded and no index buffer. 1983 particles cost 1.428 ms
    against 1.423 without: free, within noise. Blending is premultiplied, so the one
    pipeline covers translucent dust *and* an additive spark
    ([ADR-025](ADR_DECISIONS.md)).

    Three mistakes worth recording, all now tests. A puff born exactly on the ground
    is coplanar with the terrain drawn there and loses the depth test — totally and
    silently, since the draw is issued and the count is right and nothing appears.
    The radial falloff was squared, which shrank a puff's visible core to a fraction
    of its quad. And the emission was gated on `MoveState::speedElmosPerSecond`,
    which is a unit's *top* speed rather than its current one, so every parked unit
    smoked — caught by a line of output disagreeing with itself: `0 of 60 units
    routed` next to `840 dust particles still in the air`.

    **Order markers.** A right-click leaves a crossed amber ring that shrinks toward
    the point and fades. A cross rather than a second ring: it appears on the same
    ground as a selection ring within a second of it, so hue alone is one
    distinction too few.

    ![an order marker among selection rings](docs/images/m14-order-marker.jpg)

    The buffer these ride was named for rings alone, which stopped being true with a
    second kind of thing in it — hence `GroundDecals`. No pipeline, buffer or draw
    call was needed for the new decal, because the renderer already took arbitrary
    ground-conforming coloured triangles.

    **And the three open questions milestone 13 left.**

    *Should the selection tint stay?* No. It was free — the team-colour field is per
    instance and already uploaded — but in an RTS a unit's colours are its
    allegiance, so repainting that channel to mean "selected" makes a unit appear to
    change sides for as long as it is in the set. Two cues for one piece of state is
    a redundancy worth paying for; two *meanings* on one channel is not. Rings only
    now, and the click handler lost 40 lines ([ADR-021](ADR_DECISIONS.md)).

    *Should the quality switches default to looks-best or costs-least?* Looks-best,
    and there is now a settings file to say otherwise. What that obliges is that a
    benchmark states which switches were on, since two of this renderer's own
    numbers are otherwise not comparable — which it now does.

    *And the LOD thresholds, retuned now that the cull-and-merge actually fires.*
    The premise turned out to be wrong in a useful way: band boundaries do break
    runs, and it does not matter. The shipped 8/20 issues six draws against 6/14's
    one and is still **0.13 ms slower**, because it draws 80% more triangles — a
    whole-map framing is vertex-bound and a handful of draw calls does not register
    against 100k triangles.

    | near/far | whole-map GPU | draws | triangles | vs no LOD | focus-60 diff |
    |---|---|---|---|---|---|
    | off | 2.598 ms | 1 | 2 097 152 | — | — |
    | 8 / 20 (was) | 1.510 ms | 6 | 235 520 | 0.385/255 | 0.000/255 |
    | **6 / 14 (now)** | **1.380 ms** | **1** | **131 072** | 0.452/255 | 0.000/255 |
    | 4 / 10 | 1.384 ms | 1 | 131 072 | 0.452/255 | 0.258/255 |

    6 is where the near threshold stops being free: at a mid-range working camera it
    renders an image byte-identical to full detail while 4 already moves 1.23% of
    the pixels. 47% off the whole-map frame for a mean difference of 0.45/255. The
    thresholds are deliberately *not* derived from a projected-error budget, which
    was the first attempt and demands a threshold four times the width of the map —
    the model says "never use LOD" and the screenshots say the error is invisible
    under ground texture and shadow ([ADR-022](ADR_DECISIONS.md)).

    The merge itself moved out of the renderer into `core/mesh/ChunkDraws.hpp` with
    ten tests, because a draw count is the one part of a frame that has to be
    asserted rather than looked at — which is exactly how milestone 13 lost it for a
    whole milestone.

    **Refraction, built and switched off.** The water can now bend what is under it:
    the offset needs a copy of the colour target, since a framebuffer fetch reads one
    pixel and no other, so the pass splits in two around a blit (+0.17 ms on aw04,
    +0.28 on a Supreme Commander sea map). That part works. What it reveals is that
    the field being bent by is two analytic wave trains standing in for the engine's
    four scrolling normal maps — and moving a screen-space sample by a field that
    regular draws the field's own lattice across the water: rings tens of pixels
    across at swell frequency, a diagonal hatch at ripple frequency, at every
    strength down to a quarter of the engine's. So it ships off, and the blocker was
    never the copy ([ADR-023](ADR_DECISIONS.md)).

Stage B (sim semantics, only if milestones 1–5 prove out) is deliberately not
planned. The cliff is real; plan when we're on it.

## Build

Requires: CMake + Ninja (brew), Apple clang (CLT — no Xcode needed), macOS 14+.

```sh
# One-time dependencies, both vendored into third_party/ and neither committed.
# Apple's official C++ bindings for Metal:
git clone --depth 1 https://github.com/apple/metal-cpp third_party/metal-cpp

# miniz, a single-file MIT ZIP reader — Recoil ships content in .sdz archives,
# which are ZIPs. CMake fails with this exact command if either is missing.
mkdir -p third_party/miniz && curl -L \
  https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip \
  | tar -xf - -C third_party/miniz

mise exec -- cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
mise exec -- cmake --build build
mise exec -- ctest --test-dir build --output-on-failure

./build/recoil-metal                    # procedural terrain, no assets needed
./build/recoil-metal path/to/map.smf    # a real Recoil map

# Benchmark. --bench is windowed and vsync-limited (use it to eyeball GPU ms);
# --bench-offscreen is headless and unthrottled, and is the comparable number.
./build/recoil-metal path/to/map.smf --bench 600 docs/bench.csv
./build/recoil-metal path/to/map.smf --bench-offscreen 2060 docs/bench.csv

# The Recoil OpenGL baseline for the same map (needs FAR's zink build)
tools/bench_recoil_gl.sh docs/bench-recoil-gl.csv
```

### Units on a map

```sh
BAR=~/projects/llm/games/forged-alliance-reborn/reference/BAR/objects3d/Units

# 800 instances: one per map start position, the rest scattered on land
./build/recoil-metal path/to/map.smf --units $BAR/corgantbig.s3o 800

# --focus frames the first instance instead of the whole map, because a unit is
# under 1% of an 8192-elmo map's width and otherwise renders as a few pixels
./build/recoil-metal path/to/map.smf --units $BAR/corgantbig.s3o 40 --focus

# repeat --units for more models; textures shared between them upload once
./build/recoil-metal path/to/map.smf \
    --units $BAR/corgantbig.s3o 30 --units $BAR/armstump.s3o 60 \
    --units $BAR/corraid.s3o 60 --focus
# scene: 3 models, 4 textures uploaded, 2 texture binds per frame
```

### Quality settings

Three switches, all on by default, all one keypress away:

| key | flag | default | costs |
|---|---|---|---|
| `r` | `--no-reflections` | on | +0.50 ms |
| `n` | `--no-stratum-normals` | on | +0.21 ms |
| `p` | `--no-props` | on | +0.17 ms at a working zoom, nothing zoomed out |
| `f` | `--refraction` | **off** | +0.17 to +0.28 ms |

Defaults are **looks-best**, deliberately. The cheap-by-default argument is the
usual one and it is wrong here: what is being demonstrated is how the content
looks when a Metal renderer draws it, and a reader who runs this should see what
the screenshots show. What that does oblige is that the benchmark states which
switches were on, since otherwise two of its own numbers are not comparable —
which it now does.

The water's refraction is the one that defaults off, and by the same rule rather
than as an exception to it: with the wave field this water has, it does not look
best. The offset itself works — it needs a copy of the colour target, since a
framebuffer fetch reads one pixel and no other, so the pass splits in two around a
blit — and what it reveals is that the field being bent by is two analytic wave
trains standing in for the engine's four scrolling normal maps. Move a
screen-space sample by a field that regular and it draws the field's own lattice
across the water: rings tens of pixels wide at swell frequency, a diagonal hatch
at ripple frequency, at every strength down to a quarter of the engine's. It is
waiting on the water's normal coming from real textures, which was never the part
that looked hard.

To state a preference rather than pass a flag, write a Lua table to
`~/Library/Application Support/recoil-metal/settings.lua`. It is read by the same
data reader that reads `mapinfo.lua`; a missing file is the ordinary case, and a
key of the wrong type is reported rather than coerced.

```lua
-- a machine that would rather have the frame rate
{
    reflections = false,
    stratum_normals = true,
    props = false,
    refraction = false,
}
```

Flags beat the file: a flag is somebody asking for this run, the file is somebody
stating a preference. There is no `--reflections` to turn one back on, because the
defaults are already on and the only thing a flag has ever needed to say is "not
this time".

### Finding content

`--data-dir <dir>` adds a directory to the asset search path, and `--archive
<file.sdz>` extracts a Recoil archive to a temporary directory and searches
that. Both are repeatable and searched in the order given. Model and texture
lookups fall back to the historical hard-coded BAR layout, so existing command
lines keep working.

```sh
# A unit definition names its own model, found through the search path
./build/recoil-metal "$MAP" --data-dir ~/games/bar \
    --units ~/games/bar/units/ArmBots/armpw.lua 40
```

### Moving units

Left-click selects the unit under the cursor (a ring appears under it);
right-click orders it to the ground under the cursor and marks the spot — a
crossed amber ring that shrinks toward the point and fades over a second and a
half. A cross rather than another ring, because the two appear on the same ground
within a second of each other and colour alone is one distinction too few. Drags still belong to the
camera — only a press and release that stayed put counts as a click.

```sh
# --march <x> <z> <seconds>: order EVERY unit to a point and pre-run the sim.
# Click-to-move cannot be screenshotted, so this is the reproducible way in:
# the same orders and the same tick count give the same scene every run.
./build/recoil-metal "$MAP" --units $BAR/armstump.s3o 200 --march 1500 1500 0 --focus

# ...and the same scene frozen 14 seconds later, headless
./build/recoil-metal "$MAP" --units $BAR/armstump.s3o 40 \
    --march 2000 2000 14 --focus --screenshot /tmp/marched.png 1200 800
```

`--units <model.s3o> [count] [scale]`, repeatable. Textures are resolved by the
name the `.s3o` carries, under BAR's `unittextures/`; models naming the same
file share one upload, and the draws are ordered so each texture *pair* is bound
once per frame rather than once per model — which is the unit Recoil batches on
too. The first `--units` takes the map's start positions; the rest are scattered.

### Supreme Commander models

`.scm` loads into the same `Model` struct `.s3o` does, and the same magic sniff
picks the loader:

```sh
FA=~/projects/llm/input/faf/units
./build/recoil-metal "$MAP" --units $FA/UEL0201/UEL0201_LOD0.scm 40 --focus

# both content families in one scene, one draw each
./build/recoil-metal "$MAP" --units $FA/UEL0201/UEL0201_LOD0.scm 40 \
    --units $BAR/armstump.s3o 40
```

Models are extracted once from the retail install's `units.scd` (a ZIP) into
`~/projects/llm/input/faf/` — see `tests/test_real_scm.cpp` for the command.
Textures are found beside the model by Supreme Commander's naming convention
(`_Albedo`, `_SpecTeam`), since `.scm` names none.

### Ground layer textures

The splat's layers are paths into `env.scd`, another ZIP. Only the layer
directories are extracted — 402 files and 135 MiB of the archive's 1.15 GiB of
DDS, of which the stock maps name 184 — so a ZIP reader stays deferred:

```sh
python3 - <<'PY'
import zipfile, os
scd = '/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata/env.scd'
dest = os.path.expanduser('~/projects/llm/input/faf')
z = zipfile.ZipFile(scd)
for n in z.namelist():
    if n.lower().startswith('env/') and '/layers/' in n.lower() and n.lower().endswith('.dds'):
        z.extract(n, dest)
PY
```

Without them the map still draws: the terrain-type colour bands from milestone 6
stay loaded as the fallback, and the splat simply does not switch on.

### Prop meshes

The same archive, and the same deal — a `.scmap`'s props name blueprints
(`/env/Tropical/Props/Trees/Palm02_s1_prop.bp`) whose meshes and textures sit
beside them. 1511 files and 160 MiB, against 335 blueprints the stock maps
between them reference:

```sh
python3 - <<'PY'
import zipfile, os
scd = '/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata/env.scd'
dest = os.path.expanduser('~/projects/llm/input/faf')
z = zipfile.ZipFile(scd)
for n in z.namelist():
    if n.lower().startswith('env/') and '/props/' in n.lower() \
            and n.lower().endswith(('.bp', '.scm', '.dds')):
        z.extract(n, dest)
PY
```

Without them the map draws bare: the prop list is still parsed and counted, and
every blueprint that fails to resolve is reported once.

### Animation

`--animate <file.sca>` attaches an animation to the `--units` before it. The
windowed app advances its own clock; `--time <seconds>` freezes it, which is what
makes a screenshot or a benchmark of an animated scene reproducible.

```sh
./build/recoil-metal "$MAP" \
    --units $FA/DEL0204/DEL0204_lod0.scm 1 \
    --animate $FA/DEL0204/DEL0204_awalk.sca --focus
#   animation DEL0204_awalk: 2.33s, 71 keyframes, 19 of 20 bones driven
```

Every keyframe's pose is computed once at upload and playback is a buffer
offset, so 800 animated units cost 0.70 ms/frame — the same as 800 static ones.
`.s3o` has no equivalent: the format carries no rotation at all, and Recoil
drives unit motion from scripts instead.

### Supreme Commander maps

Same binary, same arguments — the format is sniffed from the file's magic:

```sh
FA="/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/maps"
./build/recoil-metal "$FA/SCMP_009/SCMP_009.scmap"

# BAR units on a Forged Alliance map: both content families in one scene
./build/recoil-metal "$FA/SCMP_009/SCMP_009.scmap" --units $BAR/corgantbig.s3o 250
```

Ground colour comes from the map's terrain-type array rather than a texture:
`.scmap` ships no baked ground, so until the splat shader exists those bands are
the stand-in. No game assets are read from anywhere but the retail install.

### Screenshots

`--screenshot` renders one frame offscreen and writes a PNG. No window, so it
works regardless of which Space is active — capturing the app's own window by id
fails outright for a window on an inactive Space, which is why this exists.

```sh
./build/recoil-metal path/to/map.smf --units $BAR/corgantbig.s3o 40 --focus \
    --screenshot docs/images/units.png 2000 1200
```

PNG encoding goes through ImageIO, which is part of the OS — not a dependency.

The renderer always writes PNG. What the README shows is a **downscaled JPEG**
of it (1600 px wide, quality 90), which is a fifth of the bytes for detail that
is invisible at the width GitHub renders an image anyway. Regenerate the whole
set after adding a shot:

```sh
cd docs/images && for f in *.png; do
    sips -Z 1600 -s format jpeg -s formatOptions 90 "$f" --out "${f%.png}.jpg"
done
```

The PNGs stay out of git (`.gitignore`); the JPEGs are what is committed.

Drag to orbit, scroll to zoom, right-drag (or shift-drag, for a trackpad) to
pan. Panning drags the ground under the cursor rather than moving by a tuned
constant — the step comes from the frustum's width at the camera's target, so it
keeps pace with the pointer at any zoom. The target is held inside the map, since
at a whole-map framing one point of mouse travel is ~18 elmos and a single
ordinary drag would otherwise leave the map with nothing to navigate back by.

Catch2 is fetched by CMake at configure time.

### Getting a real map

No game assets are committed, so the real-map tests skip unless one is present.
To provide it:

```sh
mkdir -p ~/projects/llm/input/recoil/maps && cd $_
curl -sLO "https://files-cdn.beyondallreason.dev/file/9fc29b4e9dd666d9f9866280fb3c0861/angel_crossing_1.4.sd7"
7zz e -y angel_crossing_1.4.sd7 maps/aw04.smf maps/aw04.smt mapinfo.lua -o.
```

Extract all three, not just the `.smf`: `mapinfo.lua` carries the authoritative
height range (the binary header's is wrong on this map), and `aw04.smt` is the
ground texture. Without the `.smt` the terrain still renders, shaded by
elevation — which is also what the engine does for a missing tile file.

## Layout

```
recoil-metal/
├── CMakeLists.txt      single build file, sections commented
├── src/
│   ├── core/           pure C++ — no Metal, no AppKit, fully unit-tested
│   │   ├── map/        SMF/SMT loaders, mapinfo.lua, tile atlas
│   │   ├── lua/        Lua table-literal reader (data, not programs)
│   │   ├── mesh/       heightfield triangulation
│   │   └── camera/     orbit camera + projection
│   ├── render/         Metal renderer (Objective-C++ where bridging)
│   ├── platform/       AppKit window + display link (pImpl hides ObjC)
│   └── main.mm         thin entry point
├── tests/              Catch2 unit tests, mirrors src/core
├── third_party/        metal-cpp (git-cloned, gitignored)
└── docs/               research notes, benchmark results
```

## Legal

**Licence: GPL-2.0-or-later** — the full text is in [LICENSE](LICENSE).

Recoil states its own terms as "version 2 of the License, or (at your option)
any later version" (`LICENSE` in the engine tree). Loader code here is adapted
from it, so this project takes the same terms rather than a subset: `GPL-2.0`
alone would be *narrower* than upstream and would strip the downstream choice
Recoil deliberately grants.

What is adapted, and therefore what carries the obligation:

| Here | From |
|---|---|
| `.smf` / `.smt` map loaders | `rts/Map/SMF/` |
| `.s3o` model loader and its team-colour convention | `rts/Rendering/Models/`, `ModelFragProgGL4.glsl` |
| The 30 Hz tick, and `GAME_SPEED` with it | `rts/Sim/Misc/GlobalConstants.h` |
| Water plane at `y = 0` | `rts/Map/Ground.h` |
| Model lighting defaults | `rts/Map/MapInfo.cpp` |

Supreme Commander shaders (`terrain.fx`, `mesh.fx`, `water2.fx`, `sky.fx`) are
read as a **specification** and reimplemented in MSL — no HLSL is copied, and
none of it is redistributed here. Reading either engine's source as a spec is
unrestricted; it is the *adapted* Recoil code above that sets the licence.

No game assets are or will ever be committed — same rule as FAR. The
screenshots under `docs/images/` are output of this renderer, not game content,
though they necessarily depict textures the two games ship.
