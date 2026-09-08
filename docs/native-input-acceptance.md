# Native input acceptance

`--input-acceptance OUTPUT.png` runs a windowed acceptance scenario using real
`NSEvent` press/release pairs sent to the application's `NSWindow`. Events pass
through the ordinary view handlers, click-slop logic, coordinate conversion,
world picking and `Run.mm` widget handling. It requires no Accessibility access.
The scenario starts with the app inactive. Only the helper's synchronous injected
pair opts into `acceptsFirstMouse`; normal physical clicks retain AppKit's default
activation behavior. This prevents another app taking focus from silently
discarding left clicks while right clicks continue to arrive. Acceptance mode
does not request foreground activation at startup. Its window stays above ordinary
windows during the run so macOS does not suspend the display callbacks that drive
the scenario when another application covers it. Normal gameplay windows do not
use this acceptance-only window level.
The acceptance window also ignores WindowServer mouse input so physical clicks
cannot change selection or camera state between scripted events. Explicit test
events still use `NSWindow::sendEvent` and the ordinary responder handlers.

The initial fixture is a real T1 land factory supplied with `--units` alongside
a normal skirmish. The driver chooses a factory separated from other units, since
map starts may also contain an ACU. Opponent scripts are disabled for this run. Resources, unit
definitions, build times, terrain and every `MatchRunner` simulation stage retain
their ordinary behavior. Waiting stages process twenty ordinary beats per frame;
this accelerates wall-clock pacing without modifying construction or movement rates.

The run opens with the spawned commander: the driver finds the player ACU by its
faction blueprint (`UEB0101` → `UEL0001`, and so for Aeon/Cybran/Seraphim),
selects it with a native world click, orders it with an ordinary right-click 80
elmos away, and waits until it has visibly moved 12 elmos. This is the exact
player-ready flow the 2026-08-24 rejection named.

For every product of that factory, the driver clicks its build cell, waits for
production to finish, selects it in the world, clicks Move, observes movement,
uses Shift-right to append a waypoint, clicks an unimplemented rack slot to check
input swallowing, then clicks Stop and checks that movement and queued orders
stop. It then clicks Guard, right-clicks its own factory as the target, verifies
the retained Guard command and clicks Stop again. Armed products then click Attack
and right-click an unarmed enemy fixture on an eligible target layer: a generator
for ground weapons or a scout aircraft for anti-air. After a rendered frame, the
driver checks that the order retains the exact enemy handle, captures it in
`OUTPUT.png.PRODUCT.attack.png`, clicks Stop and checks cancellation. It removes
the fixture before proceeding. Engineers and the Cybran scout instead verify
that their unavailable Attack cell accepts no command and does not arm targeting.

Immediate checks observe command intake; checks after the next frame verify
exactly one accepted recorded command for the intended unit. The faction's
engineer also pages through its build tray when it overflows, places its real T1
generator with a world click, and waits for completion.

Before production, the driver fills two queue pages through build-tray clicks,
pages to the final pending entry and cancels it. It verifies every surviving
command ID, cancels the active entry, verifies the remaining IDs again and uses
Clear Queue. A separate `OUTPUT.png.queue.png` capture shows the paged controls.

After all T1 products, the driver upgrades the selected land factory through T2
and T3 using its build tray, checking that selection follows each replacement
handle. It then selects the produced engineer, arms its T1 shipyard, finds the
nearest valid snapped water site, focuses the camera and clicks that site. The
engineer must approach and complete the shipyard; `OUTPUT.png.shipyard.png` records
the result. Finally, it clicks AUTO MEX, waits for three newly completed deposit
structures backed by the engineer's logged Build commands, and clicks AUTO MEX
off again. These stages require a map with navigable water and at least three
available resource deposits. They retain ordinary income, costs and build rates.

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
  --output "$HOME/projects/llm/input/recoil-metal/acceptance/2026-09-05-native-input"
```

Use `--factories UEB0101 --profiles compact --backings 1` for a focused run.
Choose a new dated output directory for each run to retain earlier evidence.
The default per-case timeout is 900 seconds; `--timeout` changes it. Upgrades and
long approaches account for the larger budget than the original product-only run.
The runner requires the real map and content, retains each process's log, checks
every product result and checks PNG pixel dimensions against logical window size
times backing. Independent expected armed-product counts are 5 UEF, 5 Aeon,
4 Cybran and 4 Seraphim. Missing content is a failure.

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
23 products, including Attack dispatch and cancellation for the 18 armed products.
It does not prove weapon damage, radar, special abilities,
transport behavior or retail simulation parity; those require their own tests.

On 2026-09-05, all 24 expanded cases passed using `aw04.smf` and the extracted
FA unit tree, including Guard targeting and Stop for every product.
The compact UEF queue capture and compact Aeon product capture were inspected.
These are recorded session observations; their original temporary logs and
captures were not durably archived. The runner above reproduces the checks.

The subsequent Attack extension in `76e781b` exposed inactive-window left-click
loss at unrelated Stop/selection checks. The current helper fixes that dispatch
boundary and starts the fixture inactive. The focused standard 1x UEF case
passed; all four compact 1x faction cases also passed. A later startup deactivation
race is handled by waiting for AppKit's inactive state before starting input.
The final isolated-input run recorded
22 complete passing cases before session wrap-up. Wide 2x Cybran was incomplete
and wide 2x Seraphim had no completed result.

On 2026-09-06 the two remaining cases, wide 2x Cybran and wide 2x Seraphim,
passed (`acceptance/2026-09-06-native-input-wide2x`, 5 and 6 products, zero page
clicks), completing the 24-case matrix for the Attack extension. Two earlier
attempts that day timed out at input stage 0 for a reason the report now names:
the fixture waits for AppKit to grant deactivation, which needs another app
willing to take focus, and an unattended desktop launched from a background
shell never grants it. The passing run kept a helper activating Finder every
twelve seconds during startup:

```sh
( for i in $(seq 1 40); do sleep 12; osascript -e 'tell application "Finder" to activate'; done ) &
```

This is a property of the deactivation handshake, not of Cybran or the wide
profile; the earlier "incomplete" result most likely had the same cause.

On 2026-09-07, the full 24-case matrix including the ACU opening passed (recorded
in the KB handoff). The ACU extension was committed as `fc38762`; four Compact 1x
retail-map faction cases passed again on September 8 before that commit.

The subsequent factory-upgrade, shipyard and AUTO MEX stages have **not been run
through AppKit**. At the user's request, their current verification is headless:
`mise exec -- ./build/rm_tests '[upgrade-chain],[naval-placement],[auto-expand],[station]'`.
It exercises both factory upgrades with retail definitions for all four factions,
shore-to-water shipyard construction (immediate and queued), the shared AUTO MEX
rack handler and three completed deposits, and the Kennel's authored assistance
rate. The initial checks use fixture terrain/resources.

On September 8, `[retail-workflows]` additionally loaded SCMP_009's real heightfield,
waterline, start position and resource markers. All four retail T1 engineers reached
and completed a shipyard, then the shared AUTO MEX control built three real deposits
and toggled off (68 assertions). The site search checks both the yard footprint and
the engineer's path: Aeon/Seraphim's nearest valid water footprint was unreachable on
their stricter Hover grid. The native scenario's search now makes that same check.
Storage is replenished each tick to isolate geography and command flow; this does not
prove a sustainable opening economy. Logs: `build/retail-workflows.log`.

`[submarine]` covers the real Tigershark's logged Dive/Surface control, committed-layer
weapon eligibility, depth clamp, authority, Move/Stop independence, replay and vertical
save continuation. `tests/fixtures/submarine-surface.commands` drives an offscreen
SCMP_009 surfaced screenshot at `build/submarine-surfaced.png`; the visible rack exposes
Dive/Surface. No AppKit input delivery, camera picking or headed acceptance is claimed
for these new workflows.
The earlier 24-case results apply to the earlier driver, not these new stages.
