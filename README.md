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

   ![units rallying, 5 seconds in](docs/images/m8-rally-05s.png)
   ![the same order 50 seconds in](docs/images/m8-rally-50s.png)

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

    ![sky and water on a Recoil map](docs/images/m11-sky-water.png)
    ![the same on a Supreme Commander map](docs/images/m11-sky-water-fa.png)

    Still absent: per-map sky and water colours (a `.scmap` carries them in its
    skybox block, which the loader parses but does not expose), refraction and
    planar reflection, and dynamic per-patch terrain LOD.

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

Left-click selects the unit under the cursor (it turns white); right-click
orders it to the ground under the cursor. Drags still belong to the camera —
only a press and release that stayed put counts as a click.

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

Recoil is GPL-2.0; loader code adapted from it makes this project GPL-2.0 as
well. Reading Recoil source as a spec is unrestricted. No game assets are or
will ever be committed — same rule as FAR.
