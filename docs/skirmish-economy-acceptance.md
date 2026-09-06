# Skirmish after the economy correction

Observed on 2026-09-06 with retail SCMP_009, two observer-controlled FAF armies,
default faction seats (UEF/Aeon), and 1,800 simulated seconds. The simulation
includes b63fcb1's enhancement-only ammo exclusion and deposit validation.

Team 0 won at 1,346.8 seconds. The run fired 3,674 shots, destroyed 308 units,
and completed all 496 construction records across 30 unit types. At the end,
the surviving commander had 13,200/13,200 HP; the defeated commander had 0/11,000.
Army 0 held 1,800 mass and 10,000 energy, earned 124.5 mass and 585 energy per
second, and was 100% funded against 304 energy per second of maintenance.

A second run matched all 18,000 tick hashes and captured the victory screen.
This demonstrates a reproducible completed battle after the economy correction;
it does not establish general AI readiness or retail parity. The AI reported
13 decision failures due to its instruction budget, additional fail-closed
condition errors, and the already unsupported GridReclaim field. It executed
107 modules, with nine absent, zero failed modules and zero coroutine errors.
These budget failures also appear in the earlier, pre-fix battle log.

The first run completed simulation but aborted during sandboxed Metal setup.
The repeat completed with Metal access and a successful screenshot. No native
window test was needed. Reproduce from the repository root:

```sh
fa_install='/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance'
mise exec -- ./build/recoil-metal "$fa_install/maps/SCMP_009/SCMP_009.scmap" \
  --gamedata "$fa_install/gamedata" --skirmish --observer --armies 2 \
  --ai-faf --ai-sanity --mute --play 1800 \
  --hash-log build/ai-economy-fixed.hash \
  --screenshot build/ai-economy-fixed.png 1400 900 \
  > build/ai-economy-fixed.log 2>&1
```

For the second run, replace `--hash-log` with `--check-hash-log` using the same
hash path and write stdout/stderr to `build/ai-economy-fixed-repeat.log`.
Generated logs, hashes and images are local, reproducible build artifacts; the
observations above are retained here so they survive a build-directory cleanup.

## After the instruction-budget fix

Rerun on 2026-09-06 with the same command after `860056c` (ADR-100: bounded
watchdog refills, staggered condition-cache expiry, per-pass unit-count memo),
writing `build/ai-budget-fixed.{log,hash,png}`. The AI reported zero decision
failures and zero condition errors; the only missing brain field is `GridReclaim`
(783 reads). It executed 107 modules with nine absent, zero failed modules and
zero thread errors. Team 0 won at 1,373.8 seconds after 3,395 shots and 345 units
destroyed, completing all 500 construction records; army 0 held 1,750 mass and
5,000 energy, earning 110.5 mass and 665 energy per second, 100% funded against
284 energy per second of upkeep. A repeat with `--check-hash-log` matched all
18,000 tick hashes. The course differs from the earlier log because staggering
changes when conditions are re-checked; the outcome does not.

The earlier 13 decision failures were one genuine overrun per affected pass plus
a watchdog cascade, not thirteen slow conditions.

Engineer menu checks use `[engineer-tiers]` against real T1/T2/T3 blueprints.
The T3 offscreen menu exposed 45 options with higher tiers first, including
authored experimentals, and pagination for lower tiers. Unenhanced ACUs remain
restricted to their T1 menu. The extractor queue capture uses
`tests/fixtures/hud-extractor-queue.commands`; `make test-upgrade-ui` compares
repeated pixels and hashes and checks that both upgrade rows are present.
