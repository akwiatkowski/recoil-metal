# Headless UI acceptance

## Construction lifecycle

Run `mise exec -- ./build/rm_tests '[lifecycle]'`.

The fixture loads real UEL0105 and UEB1101 definitions, issues the build through
the normal app command path, and advances `MatchRunner`. Assertions cover:

- No construction inspector before the order.
- A partial, active build with progress strictly between zero and one.
- Empty resource storage: after the cached funding/residue drains, STALLED with
  unchanged progress across further simulation ticks.
- Restored funding: ACTIVE with increasing progress, then a completed generator
  and no stale construction inspector.
- Stop during work: no inspector and no subsequently spawned generator.
- Builder death: no inspector and no further work advancement.

`constructionCard` reads current simulation state; it owns no clock or progress
counter. Both live and offscreen HUD paths use it for the selected builder's
default inspector. Hover/armed information still takes precedence. Geometry
tests in `test_hud.cpp` use the real 68-point inspector height and check the
clamped bar stays inside it.

## Deterministic rendered scenarios

Run `mise exec -- bash tools/capture_hud_scenarios.sh /tmp/recoil-hud-captures`.
This requires Metal access and the retail install; `FA_INSTALL` overrides its
location. `CAPTURE_WIDTH`, `CAPTURE_HEIGHT`, and `CAPTURE_BACKING` select a viewport.

Four scenarios replay versioned semantic commands on SCMP_009 with two armies:

- Construction at 12 seconds: the real commander builds a generator, 39% complete.
- Mixed selection at 22 seconds: own commander, extractor and completed generator.
- Factory production at 405 seconds: a real factory with 30 tanks ordered.
- Energy starvation at 410 seconds: the factory has zero stored energy and about
  1% funding. This still advances slowly; it is not a zero-progress claim.

The setup's extractor consumes source-local command ID zero. The fixtures submit
ID one at tick 70, after that work completes. Factory production waits until tick
4000: with no generator the factory itself needs roughly 161 seconds to finish.
The capture runner checks exact replay hashes and byte-identical PNGs across two
runs, plus semantic `hud-state` diagnostics from the actual selected builder.
Artifacts stay outside the repository. Failure prints the relevant log tail.

Queue count is verified in simulation diagnostics and rendered by the connected
production panel above the command rack. Both factory captures require its
diagnostic, and the image shows the current product and x30 count. Headless
player selection excludes enemy units; observer captures remain unrestricted.

## Factory queue controls

The live and offscreen paths both use `gatherProduction`, `productionPanelRect`
and `appendProductionPanel`. The active builder owns the panel, matching the
build tray; it does not combine queues from multiple factories. The existing
command rack stays accessible. The first row is the current/next product, with
remaining count and a progress bar. Long queues retain the existing overflow
summary rather than drawing outside the panel.

Click REPEAT OFF/ON to toggle queue repetition; click CLEAR QUEUE to submit Stop
to this factory, cancelling its current work and queued orders. Clear is disabled
when idle. Add products through the existing build tray. Per-row deletion and
drag reordering are not controls in this slice. Panel clicks and drags are
intercepted, so they cannot select/order units behind the panel.

`mise exec -- ./build/rm_tests '[production]'` checks shared rendered/hit-test
bounds, overflow and text bounds, and real UEB0101 production through MatchRunner.
The scenario submits two tank builds, observes the product/count and partial
progress, clicks the same control handler as the window, checks Repeat on/off,
rejects an enemy player's toggle at dispatch, and clears work with no delayed
unit appearing. Dead factory handles hide the panel and reject controls.
