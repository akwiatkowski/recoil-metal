# Native input acceptance

`--input-acceptance OUTPUT.png` runs a windowed acceptance scenario using real
`NSEvent` press/release pairs sent to the application's `NSWindow`. Events pass
through the ordinary view handlers, click-slop logic, coordinate conversion,
world picking and `Run.mm` widget handling. It requires no Accessibility access.

The initial fixture is a real T1 land factory supplied with `--units` alongside
a normal skirmish. The driver chooses a factory separated from other units, since
map starts may also contain an ACU. Opponent scripts are disabled for this run. Resources, unit
definitions, build times, terrain and every `MatchRunner` simulation stage retain
their ordinary behavior. Waiting stages process twenty ordinary beats per frame;
this accelerates wall-clock pacing without modifying construction or movement rates.

For every product of that factory, the driver clicks its build cell, waits for
production to finish, selects it in the world, clicks Move, observes movement,
uses Shift-right to append a waypoint, clicks an unimplemented rack slot to check
input swallowing, then clicks Stop and checks that movement and queued orders
stop. It then clicks Guard, right-clicks its own factory as the target, verifies
the retained Guard command and clicks Stop again. Immediate checks observe command intake; checks after the next frame verify
exactly one accepted recorded command for the intended unit. The faction's
engineer also pages through its build tray when it overflows, places its real T1
generator with a world click, and waits for completion.

Before production, the driver fills two queue pages through build-tray clicks,
pages to the final pending entry and cancels it. It verifies every surviving
command ID, cancels the active entry, verifies the remaining IDs again and uses
Clear Queue. A separate `OUTPUT.png.queue.png` capture shows the paged controls.

Selection waits for a rendered frame after focusing the camera. Build completion
compares full generational handles, including units created in recycled slots.
The final capture runs from a main-loop timer between renderer frames. A failed
assertion or timeout saves a diagnostic capture and returns nonzero. A successful run prints one PASS line for
every product, then a final summary and saves a PNG.

Run the complete 24-case matrix (four factories, three HUD profiles, two scales):

```sh
mise exec -- python3 tools/input_acceptance.py \
  --map '/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/maps/SCMP_009/SCMP_009.scmap' \
  --gamedata '/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance/gamedata' \
  --output /tmp/recoil-native-input
```

Use `--factories UEB0101 --profiles compact --backings 1` for a focused run.
The runner requires the real map and content, retains each process's log, checks
every product result and checks PNG pixel dimensions against logical window size
times backing. Missing content is a failure.

For an extracted VFS tree, use `--data-dir` in place of `--gamedata`. An available
BAR `.smf` map can exercise the same native input and real unit simulation while
the retail install is disconnected. Logs retain missing-texture and missing-data
warnings. Such a run proves the listed interactions on that map; it does not
replace the retail archive gate, retail-map screenshots or the golden replay.

The backing override is a **simulated display scale**, applied consistently to
the Metal layer and `UiViewport`. It exercises 1x/2x input/render conversions on
one physical screen; it does not claim a physical multi-display migration test.
Profiles use an explicit HUD-scale preference to expose Compact, Standard and
Wide layouts at manageable window sizes. This is native responder-dispatch
acceptance, not a test of OS-level hardware event delivery or Accessibility.

Passing this scenario proves production and the listed common controls for all
23 products. It does not prove each unit's attack, radar, special abilities,
transport behavior or retail simulation parity; those require their own tests.

On 2026-09-05, all 24 expanded cases passed using `aw04.smf` and the extracted
FA unit tree, including Guard targeting and Stop for every product. Logs and captures
are under `/tmp/recoil-native-guard-matrix` (the earlier queue-only expansion is
under `/tmp/recoil-native-cancel-matrix`).
The compact UEF queue capture and compact Aeon product capture were inspected.
