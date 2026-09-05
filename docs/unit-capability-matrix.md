# Unit capability evidence

This is a coverage inventory, not a declaration of Forged Alliance parity.
Scope: all 23 mobile TECH1 products of the four retail T1 land factories and
all 21 product/factory pairs of the four T1 air factories (including their land engineers).
The factory build expressions determine membership; the independent roster
table detects missing or extra entries. A unit passing one role scenario does
not imply that its other abilities work.

## Evidence levels

- **BP**: `tests/test_unit_capabilities.cpp`, `[corpus][capability]`, checks
  roster membership, role, movement speed, vision, build time and role-specific
  blueprint fields. It does not execute those abilities.
- **Behavior**: `tests/test_unit_scenarios.cpp`, the case named
  `every retail T1 land factory product performs its role through the match runner`,
  executes movement and one primary-role job using real definitions.
- **UI model**: that same per-unit scenario checks the named command exists in
  the rack descriptors and is enabled for this unit. It does not click a widget.
- **UI reachable**: end-to-end native input/selection/command execution for each unit.
  **Native common** below means real factory-tray production, world selection,
  Move, queued Move, Stop and Guard, plus generator construction for engineers.
  All 23 land products passed on `aw04.smf` with the extracted FA tree, across
  compact/standard/wide layouts and simulated 1x/2x backing; logs are in
  `/tmp/recoil-native-guard-matrix`. See [native acceptance](native-input-acceptance.md)
  for the subsequent Attack extension and its verification. Air-factory input
  remains unverified. Direct `issueBuild` calls are not UI evidence.

Behavior abbreviations: **M** = reaches a movement goal; **S** = reveals a new
location through vision and radar; **B** = finishes its faction's real T1 power
generator; **G** = damages an enemy ground structure; **A** = damages a patrolling
enemy scout aircraft. Damage proves a usable weapon, not exact damage, cadence,
range, projectile scripting, or performance against all eligible target layers.

The Behavior column inventories the role scenarios and their intended coverage.
The latest full CTest run cannot execute the required 23-product role case
because the retail `projectiles.scd` mount is unavailable. Native input
success does not replace that content gate or prove the weapon-role results.

## T1 land matrix

| Factory | Product | BP | Behavior | UI model | UI reachable |
|---|---|---|---|---|---|
| UEB0101 | UEL0101 | Checked | M, S | Move | Native common |
| UEB0101 | UEL0103 | Checked | M, G | Move, Attack | Native common |
| UEB0101 | UEL0104 | Checked | M, A | Move, Attack | Native common |
| UEB0101 | UEL0105 | Checked | M, B | Move | Native common |
| UEB0101 | UEL0106 | Checked | M, G | Move, Attack | Native common |
| UEB0101 | UEL0201 | Checked | M, G | Move, Attack | Native common |
| UAB0101 | UAL0101 | Checked | M, S | Move | Native common |
| UAB0101 | UAL0103 | Checked | M, G | Move, Attack | Native common |
| UAB0101 | UAL0104 | Checked | M, A | Move, Attack | Native common |
| UAB0101 | UAL0105 | Checked | M, B | Move | Native common |
| UAB0101 | UAL0106 | Checked | M, G | Move, Attack | Native common |
| UAB0101 | UAL0201 | Checked | M, G | Move, Attack | Native common |
| URB0101 | URL0101 | Checked | M, S | Move | Native common |
| URB0101 | URL0103 | Checked | M, G | Move, Attack | Native common |
| URB0101 | URL0104 | Checked | M, A | Move, Attack | Native common |
| URB0101 | URL0105 | Checked | M, B | Move | Native common |
| URB0101 | URL0106 | Checked | M, G | Move, Attack | Native common |
| URB0101 | URL0107 | Checked | M, G | Move, Attack | Native common |
| XSB0101 | XSL0101 | Checked | M, S | Move | Native common |
| XSB0101 | XSL0103 | Checked | M, G | Move, Attack | Native common |
| XSB0101 | XSL0104 | Checked | M, A | Move, Attack | Native common |
| XSB0101 | XSL0105 | Checked | M, B | Move | Native common |
| XSB0101 | XSL0201 | Checked | M, G | Move, Attack | Native common |

## T1 air-factory matrix

The independent roster contract checks all 21 pairs, including the Cybran
XRA0105 gunship. Air scouts need vision; radar is not assumed. Interceptors
need an air-targeting weapon, bombers/gunships a ground-targeting weapon, and
transports the authored Transport cap. Those are blueprint checks, not flight,
bombing or cargo execution. The four engineers share the land behavior evidence
above; producing them through an air-factory tray remains unverified.

UEA0102 additionally has a bounded flight contract: real authored tuning, planar
combat turns/breakoff/recovery, and fresh-scene save continuation with matching
per-tick hashes. The 900-tick app pursuit capture at `/tmp/recoil-air-turn.png`
was inspected, and its replay compared against the saved hashes. It uses an
allied moving target to isolate pursuit; it does not prove interceptor weapon
parity, three-axis banking, or air-factory input. See ADR-091.

| Factory | Product | Role | BP | Behavior | UI reachable |
|---|---|---|---|---|---|
| UEB0102 | UEL0105 | Engineer | Checked | M, B (land scenario) | Unverified |
| UEB0102 | UEA0101 | Scout | Checked | Unverified | Unverified |
| UEB0102 | UEA0102 | Interceptor | Checked | Planar pursuit and save continuation | Air-factory production unverified |
| UEB0102 | UEA0103 | Bomber | Checked | Unverified | Unverified |
| UEB0102 | UEA0107 | Transport | Checked | Unverified | Unverified |
| UAB0102 | UAL0105 | Engineer | Checked | M, B (land scenario) | Unverified |
| UAB0102 | UAA0101 | Scout | Checked | Unverified | Unverified |
| UAB0102 | UAA0102 | Interceptor | Checked | Unverified | Unverified |
| UAB0102 | UAA0103 | Bomber | Checked | Unverified | Unverified |
| UAB0102 | UAA0107 | Transport | Checked | Unverified | Unverified |
| URB0102 | URL0105 | Engineer | Checked | M, B (land scenario) | Unverified |
| URB0102 | URA0101 | Scout | Checked | Unverified | Unverified |
| URB0102 | URA0102 | Interceptor | Checked | Unverified | Unverified |
| URB0102 | URA0103 | Bomber | Checked | Unverified | Unverified |
| URB0102 | URA0107 | Transport | Checked | Unverified | Unverified |
| URB0102 | XRA0105 | Gunship | Checked | Unverified | Unverified |
| XSB0102 | XSL0105 | Engineer | Checked | M, B (land scenario) | Unverified |
| XSB0102 | XSA0101 | Scout | Checked | Unverified | Unverified |
| XSB0102 | XSA0102 | Interceptor | Checked | Unverified | Unverified |
| XSB0102 | XSA0103 | Bomber | Checked | Unverified | Unverified |
| XSB0102 | XSA0107 | Transport | Checked | Unverified | Unverified |

## Exceptional abilities: evidence still needed

The downloaded wiki is WEB-tier evidence for selecting scenarios, never the
authority for numeric constants or proof that this engine implements a claim.
Paths below refer to the local gitignored mirror; a redirect or missing page is
not positive evidence. Confirm its claims against shipped blueprints/scripts
before implementing behavior.

| Product | Ability not covered by its primary-role scenario | Reference / limit |
|---|---|---|
| UAL0201 | Water-layer targeting remains unverified | `[exceptional]` now executes a land-water-land crossing with the retail Aurora, checking surface height throughout; it exposed and corrected seabed-height movement. This does not verify hover dynamics or water-layer weapon filters. |
| XSL0103 | Zthuee crossing water and complete artillery salvo behavior | `reference/supcom-wiki/units/Seraphim/Seraphim_T1_Mobile_Light_Artillery.md`, amphibious indirect-fire role; damaging one nearby structure does not prove either |
| XSL0101 | Selen stationary cloak/stealth, losing concealment on movement or attack, and secondary combat role | `reference/supcom-wiki/units/Seraphim/Seraphim_T1_Combat_Scout.md`; the scout scenario tests only movement/vision/radar |
| URL0107 | Mantis repair now exercised | `[exceptional]` loads retail `URL0107/URL0107_unit.bp`, issues Repair through MatchRunner, restores an ally to full health, checks resource use and command completion, and retains the authored Reclaim prohibition. |
| URL0104 | Complete nanodart guidance behavior | Retail `projectiles/CAANanoDart01/CAANanoDart01_proj.bp`; moving-target damage is exercised, but target leading, zigzag, and authored lifetime remain outside the implemented controller |
| All engineers | Repair, reclaim, assist, and every buildable structure | The primary-role job builds one generator, not the complete engineering repertoire |
| All combat products | Target filters, terrain obstruction, range boundaries, exact weapon cadence and special damage | One successful attack is intentionally only a minimum role contract |

See [T1 behavioral contract limits](t1-headless-contracts.md) for guidance details
and [headless UI acceptance](headless-ui-acceptance.md) for construction and
rendered scenario evidence. Those captures do not establish per-unit UI access.

## Extending and verifying the inventory

Run `mise exec -- ./build/rm_tests '[capability]'`. Missing corpus currently can
skip content cases; a green summary with skips is not evidence for these rows.
Confirm both the independent roster case and the 23-product scenario execute.
The source corpus and projectile archive must be present.
Use `mise exec -- make test-content` for acceptance that rejects any skipped case.

For another factory/tier, add an independent expected roster, extend the
scenario enumeration, and provide an executable role branch before marking
Behavior as checked. Add exceptional-ability cases by blueprint ID rather than
changing the generic role contract to pretend every unit has the same abilities.
Update this inventory in the same commit as its evidence. Products outside the
table are **not assessed here**, even if other parser or subsystem tests mention
them. Keep BP, Behavior, UI model and UI reachable separate when expanding it.
