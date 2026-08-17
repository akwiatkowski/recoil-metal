<!-- Generated and maintained by Claude -->
# Milestone 4 — Recoil GL vs recoil-metal on the same map

**Date:** 2026-08-17 · **Machine:** Apple M4 Pro · **Map:** BAR "Angel Crossing 1.4"
(`aw04`, 1024×1024 squares = 8192×8192 elmos) · **Resolution:** 1920×1080 ·
**Samples:** 2000 frames each, 60 warmup frames discarded on both sides.

## Result

| | mean | p50 | p95 | p99 | max | implied fps |
|---|---:|---:|---:|---:|---:|---:|
| **Recoil, OpenGL via zink** | 7.416 ms | 7.058 | 10.604 | 13.249 | 23.169 | 135 |
| **recoil-metal, native Metal** | **0.554 ms** | 0.529 | 0.721 | 0.873 | 2.506 | **1805** |

**Ratio: 13.4× on the mean, 13.3× on p50.**

recoil-metal's GPU time, measured separately from the command buffer's own
`GPUStartTime`/`GPUEndTime`: mean 0.889 ms, p50 0.856, p95 1.089, p99 1.172.

Raw per-frame data: [`bench-recoil-gl-aw04.csv`](bench-recoil-gl-aw04.csv),
[`bench-metal-offscreen-aw04.csv`](bench-metal-offscreen-aw04.csv).

## Read this before quoting the number

**This is not "the same work through two APIs."** It is *a full engine frame
through four translation layers* against *a terrain draw natively*. The 13.4×
is real and it is measured, but attributing it to the translation stack would be
wrong. What differs:

| | Recoil GL | recoil-metal |
|---|---|---|
| Terrain geometry | **ROAM LOD** — adaptively fewer triangles | brute force, all 2.1M triangles every frame |
| Terrain shading | splat + detail + normal maps, multiple texture layers | one BC1 diffuse, Lambert |
| Water | `BumpWater`/`DynWater` shader | one translucent quad |
| Sky, GUI, fonts | drawn | not drawn |
| Simulation | full sim step inside the measured frame | none |

Note the first row cuts *against* recoil-metal: Recoil draws **fewer** triangles
than we do, because it has LOD and we do not. So the gap is not simply "we do
less geometry" — it is that Recoil does more per-pixel work, more subsystems, and
carries a simulation step, all through zink → kosmickrisp → Metal.

**What the number does support:** there is roughly an order of magnitude of
headroom on this hardware, and a native Metal path is nowhere near being the
bottleneck at this map size. That is enough to justify continuing the project,
which is what milestone 4 existed to decide.

**What it does not support:** any claim about the specific cost of the
translation stack, or about Recoil's renderer being poorly written. Isolating the
translation cost would need the same Recoil build measured against a native GL
driver, which this machine cannot provide.

## Method

Both sides report **wall-clock frame period with vsync disabled**. That is the
only axis available on both: Recoil exposes no GPU timer to Lua. Comparing
against recoil-metal's *windowed* mode would have compared two display refresh
rates rather than two renderers — the windowed path measures 16.68 ms mean with a
16.77 p99, which is 60 Hz to four significant figures and says nothing about the
renderer.

**Recoil side** — `tools/bench_recoil_gl.sh`:

* the engine is FAR's zink build, `GL_RENDERER = "zink Vulkan 1.3(Apple M4 Pro
  (MESA_KOSMICKRISP))"`, `GL_VERSION = "4.6 (Compatibility Profile) Mesa
  26.1.0-devel"`, chain documented in FAR's `docs/recoil-macos-rendering.md`
* `tools/rmbench.sdd`, a minimal game with **no units**, so Recoil is not also
  benchmarking FAR's converted content
* `VSync = 0`, `Shadows = 0`, 1920×1080 windowed
* timings from `Spring.GetTimerMicros` in `LuaUI/main.lua`, buffered and emitted
  only after the run

**recoil-metal side** — `--bench-offscreen 2060 out.csv`:

* no window, no `CAMetalDisplayLink`, hence no vsync
* renders into a private offscreen target at 1920×1080
* up to 3 frames in flight, so the figure is pipelined throughput rather than
  per-frame latency

## Three measurement mistakes worth remembering

**1. Instrumentation inside the measured loop.** The first version echoed one log
line per frame from Lua. That put a synchronous write in every frame being timed
and reported **8.645 ms** against the corrected **7.416 ms** — my own harness was
14% of the result. Samples are now buffered and emitted after timing ends.

**2. Timer resolution.** `Spring.GetTimer` has millisecond resolution, so at
~7 ms/frame every sample quantised to a whole number (7.000, 15.000, 21.000) and
the percentiles were meaningless. `Spring.GetTimerMicros` with
`DiffTimers(..., fromMicroSecs = true)` fixes it. Also: those timers are
**lightuserdata**, not numbers — subtracting them directly raises a Lua error
that silently kills the callin and yields *no samples at all*, which at least
fails loudly in aggregate.

**3. GPU time is not 1/throughput.** recoil-metal's per-command-buffer GPU time
(0.889 ms) is *longer* than its wall-clock frame period (0.554 ms). That is not a
contradiction: with 3 frames in flight, command buffers overlap on the GPU, so
per-buffer duration measures latency and cannot be inverted into a frame rate.

## What would sharpen this

* Reduce Recoil's frame to terrain only (disable water, sky, GUI) — moves the
  comparison closer to like-for-like without touching recoil-metal.
* Add LOD to recoil-metal. Now justified: there is finally a measurement to
  prove whether it helps, which is why it was deliberately deferred until here.
* recoil-metal's own tail is worth a look — p99 0.873 against a 0.529 median.
