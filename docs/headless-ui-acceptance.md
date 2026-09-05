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

This proves state-to-view behavior and geometry, not an active-work screenshot.
Deterministic rendered scenarios are the next acceptance layer.
