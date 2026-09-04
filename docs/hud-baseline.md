<!-- Generated and maintained by Claude -->
# HUD baseline — slice 0

The measured starting point for every interface change after it: what the native HUD looks like
and what it costs, in seven states at four size points, captured headless and deterministic so a
later run diffs against this one image for image. Item `recoil-metal-3628`; feeds `WP-39`/`WP-40`
and the `FA-UI` row of [`fa-gameplay-progress.md`](fa-gameplay-progress.md).

**Evidence lives outside the repo**, as the item asked: PNGs, per-frame timing CSVs, labelled
contact sheets, the CSV behind the table below and every command that produced it are in
`~/projects/llm/input/recoil-metal/hud-baseline/<date>/` (`REPORT.md` there is this table with
the machine and revision stamped). Regenerate with:

```sh
tools/hud_baseline.sh            # -> ~/projects/llm/input/recoil-metal/hud-baseline/<today>
FA_ROOT=/path/to/FA FRAMES=600 tools/hud_baseline.sh /elsewhere
```

## What was captured

**Scene.** The README's deterministic skirmish on `SCMP_009` at five minutes (`--skirmish
--armies 2 --play 300 --focus --no-interpolate`): a base under construction, a stall on the
economy panel, the player's commander framed. The engineer state is a units-only scene with one
`URL0105` — see the caveat below.

| state | flags beyond the scene | shows |
|---|---|---|
| default | — | economy panel, match readout, minimap; nothing selected |
| commander | `--select 1` | selection and range rings, roster with inspector, build palette (two pages) |
| selection | `--select 6` | a mixed roster: several types, the total in the header |
| engineer | `--units URL0105_unit.bp 1 --select 1` | an engineer selected outside any army |
| placement | `--select 1 --hover 1 --ghost <beside the commander>` | the hovered option's info card and the placement ghost |
| observer | `--observer` | no seat: no fog, no selection, the observer's economy panel |
| faf | `--select 1 --ui faf` | the commander state in the classic FAF chrome |

**Size points.** Logical size with the display scale it stands in for. `--backing` (added for
this baseline) lays the interface out in points, rasterises fonts at the backing scale and
renders `W·S × H·S` pixels — the contract a Retina window gives the renderer.

| label | logical | backing | pixels | HUD scale | stands in for |
|---|---|---|---|---|---|
| hd-1x | 1280x720 | 1 | 1280x720 | 1.00 | the design size |
| hd+-1x | 1600x900 | 1 | 1600x900 | 1.25 | a larger 1x window |
| mbp14-2x | 1512x982 | 2 | 3024x1964 | 1.18 | the 14-inch MacBook Pro, fullscreen |
| 5k-2x | 2560x1440 | 2 | 5120x2880 | 2.00 | a 5K display, fullscreen |

**Measurements.** Per state and size: the six HUD layers' uploaded vertex counts (uploads equalled
submissions in every layer, so nothing was dropped), and the offscreen benchmark's GPU mean and
p95 over 240 measured frames, once with `--bench-hud` (the interface exactly as the capture
composes it) and once world-only; `HUD ms` is the difference of the two means.

## Results

Captured 2026-09-04 at `9c98c69`+ on an Apple M4 Pro, 300 benchmark frames per point (60 warmup).

| state | size | pixels | HUD scale | HUD vertices surface/chrome/icon/label/readout | gpu HUD ms (p95) | gpu world ms (p95) | HUD ms |
|---|---|---|---|---|---|---|---|
| default | hd-1x | 1280x720 | 1.000 | 36/354/0/132/240 | 1.523 (2.216) | 1.546 (2.283) | -0.023 |
| commander | hd-1x | 1280x720 | 1.000 | 60/1056/78/1332/342 | 1.577 (2.194) | 1.519 (2.103) | 0.058 |
| selection | hd-1x | 1280x720 | 1.000 | 60/1140/90/1338/342 | 1.577 (2.259) | 1.510 (2.031) | 0.067 |
| engineer | hd-1x | 1280x720 | 1.000 | 48/456/6/204/210 | 1.051 (1.114) | 1.041 (1.169) | 0.010 |
| placement | hd-1x | 1280x720 | 1.000 | 60/1062/78/1350/324 | 1.602 (2.190) | 1.521 (2.069) | 0.081 |
| observer | hd-1x | 1280x720 | 1.000 | 36/462/0/60/168 | 1.503 (2.245) | 1.656 (2.341) | -0.153 |
| faf | hd-1x | 1280x720 | 1.000 | 306/696/78/1332/342 | 1.546 (2.296) | 1.522 (2.252) | 0.024 |
| default | hd+-1x | 1600x900 | 1.250 | 36/354/0/132/240 | 1.980 (2.667) | 1.985 (2.685) | -0.005 |
| commander | hd+-1x | 1600x900 | 1.250 | 60/1056/78/1332/342 | 2.068 (2.739) | 1.970 (2.656) | 0.098 |
| selection | hd+-1x | 1600x900 | 1.250 | 60/1140/90/1338/342 | 2.070 (2.754) | 1.970 (2.675) | 0.100 |
| engineer | hd+-1x | 1600x900 | 1.250 | 48/456/6/204/210 | 1.323 (2.101) | 1.322 (2.088) | 0.001 |
| placement | hd+-1x | 1600x900 | 1.250 | 60/1062/78/1350/324 | 2.132 (2.748) | 1.717 (1.876) | 0.415 |
| observer | hd+-1x | 1600x900 | 1.250 | 36/462/0/60/168 | 1.952 (2.536) | 2.075 (2.760) | -0.123 |
| faf | hd+-1x | 1600x900 | 1.250 | 306/696/78/1332/342 | 2.141 (2.736) | 2.067 (2.617) | 0.074 |
| default | mbp14-2x | 3024x1964 | 1.181 | 36/354/0/132/240 | 6.151 (6.846) | 6.281 (6.960) | -0.130 |
| commander | mbp14-2x | 3024x1964 | 1.181 | 60/1056/78/1332/342 | 6.697 (7.410) | 6.301 (7.019) | 0.396 |
| selection | mbp14-2x | 3024x1964 | 1.181 | 60/1140/90/1338/342 | 6.677 (7.383) | 6.302 (7.037) | 0.375 |
| engineer | mbp14-2x | 3024x1964 | 1.181 | 48/456/6/204/210 | 5.341 (6.058) | 5.150 (5.863) | 0.191 |
| placement | mbp14-2x | 3024x1964 | 1.181 | 60/1062/78/1350/324 | 6.649 (7.354) | 6.253 (6.962) | 0.396 |
| observer | mbp14-2x | 3024x1964 | 1.181 | 36/462/0/60/168 | 6.055 (8.115) | 6.986 (7.653) | -0.931 |
| faf | mbp14-2x | 3024x1964 | 1.181 | 306/696/78/1332/342 | 6.703 (7.395) | 6.313 (7.054) | 0.390 |
| default | 5k-2x | 5120x2880 | 2.000 | 36/354/0/132/240 | 12.419 (12.790) | 12.760 (13.087) | -0.341 |
| commander | 5k-2x | 5120x2880 | 2.000 | 60/1056/78/1332/342 | 13.380 (13.674) | 12.777 (13.109) | 0.603 |
| selection | 5k-2x | 5120x2880 | 2.000 | 60/1140/90/1338/342 | 13.394 (13.678) | 12.115 (12.283) | 1.279 |
| engineer | 5k-2x | 5120x2880 | 2.000 | 48/456/6/204/210 | 10.584 (11.153) | 10.130 (10.836) | 0.454 |
| placement | 5k-2x | 5120x2880 | 2.000 | 60/1062/78/1350/324 | 12.828 (12.961) | 12.599 (13.184) | 0.229 |
| observer | 5k-2x | 5120x2880 | 2.000 | 36/462/0/60/168 | 12.480 (12.931) | 14.550 (14.869) | -2.070 |
| faf | 5k-2x | 5120x2880 | 2.000 | 306/696/78/1332/342 | 12.450 (13.345) | 12.282 (13.536) | 0.168 |

## Findings

- **Layout holds at every size point.** No clipping, overlap or missing module in 28 captures; the
  deck's three panels and the minimap keep their places from 1280x720 at 1x to 5K at 2x, and the
  2x captures rasterise their type at 2x rather than upscaling it.
- **The interface is cheap.** Its GPU cost is below run-to-run variation at 1x (tens of
  microseconds against a 1.5 ms frame) and at most 1.3 ms at 5K with the full deck, where a
  frame costs 12.8 ms. Vertex counts are flat across sizes — the HUD magnifies, it does not
  subdivide — and peak at about 2.9k vertices for the commander states out of 36k of capacity.
- **FAF chrome costs nothing measurable** over the native skin at the same geometry: the same
  vertex counts except the panel-surface layer (nine-slice quads, 306 against 60), and GPU
  means within noise of each other.

## Caveats, each with the follow-up it names

- **The engineer state is not an engineer in a skirmish.** `--select N` picks by draw order, so
  the only way to ring an engineer is a units-only scene, and a unit outside an army has no
  faction, no economy and no buildable list: the capture shows the roster and rings but no build
  palette, and the mesh draws untextured because that loader path resolves textures beside the
  blueprint. A `--select-type <blueprint id>` for the headless paths closes this; it is the
  dashboard's next FA-UI task.
- **`HUD ms` is a noisy subtraction.** The world-only benchmark builds its particles and icons on
  its own path (`appendSceneIcons` without strategic refs) while the HUD benchmark composes the
  scene exactly as a capture does, so the two differ by more than the interface; the observer
  state, which draws the most units, comes out negative at every size. The honest reading is
  "below noise at 1x, about a millisecond at 5K". Routing the world-only benchmark through the
  same composition minus the HUD would make the subtraction clean.
- **Real fullscreen windows were not measured.** The 2x points are headless captures at the
  displays' logical sizes; the windowed CLI still has a fixed 1280x720 window and no fullscreen
  option, and a backgrounded display link throttles. The offscreen numbers are the comparable
  ones; a windowed fullscreen figure would need a `--window <w> <h>` / `--fullscreen` flag.
