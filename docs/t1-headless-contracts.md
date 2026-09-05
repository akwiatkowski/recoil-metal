# T1 land behavioral contracts

The [capability matrix](unit-capability-matrix.md) separates blueprint, behavior,
UI-model and end-to-end UI evidence, including untested exceptional abilities.

`tests/test_unit_scenarios.cpp` enumerates the real products of all four retail T1
land factories using their build expressions. It requires 23 products, then runs
each through `MatchRunner` in isolation: movement plus its primary role. Scouts
must reveal a new location through vision and radar; engineers finish their
faction's real power generator; combat units damage a ground structure or a
patrolling scout aircraft. These are minimum behavioral contracts, not full unit
parity. The existing capability table remains the independent roster/role check.

The test exposed two missing behaviors: collision resolution flattened airborne
units after the flight integrator, and Cybran mobile AA launched its tracking
nanodarts as straight projectiles. The collision pass now preserves flight
altitude; guided projectiles retain a generation-safe target and obey authored
turn-rate, acceleration and maximum-speed inputs. Guidance state is hashed.

## Guidance decision and limits

Source: retail `projectiles.scd`,
`projectiles/CAANanoDart01/CAANanoDart01_proj.bp`, `Physics`: TrackTarget=true,
TurnRate=80 degrees/second, Acceleration=2 ogrids/second squared, MaxSpeed=25
ogrids/second. An ogrid is eight elmos. General projectile blueprints do not
require the silo-specific Economy block.

Use a fixed-point bounded yaw/pitch pursuit controller with a shared angular
budget. This gives the authored fields executable meaning without an unverified
claim to retail's native steering equation. Do not replace the moving-target
test with a stationary target merely to accommodate straight missiles.

Still not modeled here: target leading, zigzag, authored projectile lifetime,
lost-target lifetime, scripted guidance changes, and guidance onto other
projectiles. A dead target is not replaced by a recycled unit handle. Existing
flare/redirect paths replace the guidance target so subsequent pursuit does not
undo their redirection. Match saves currently do not serialize projectiles;
this change does not imply full in-flight save/resume support.

Run `mise exec -- ./build/rm_tests '[scenario],[guidance]'` for focused checks.
The full acceptance gate remains CTest plus `make verify`; the golden compares
our simulation against itself, never against retail.
