# Unit capability evidence

This is a coverage inventory, not a declaration of Forged Alliance parity.
Scope: all 23 mobile TECH1 products of the four retail T1 land factories.
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
- **UI reachable**: end-to-end input/selection/command execution for each unit.
  This remains **unverified for every row**. In particular, calling `issueBuild`
  directly does not prove a product is reachable from the factory build tray.

Behavior abbreviations: **M** = reaches a movement goal; **S** = reveals a new
location through vision and radar; **B** = finishes its faction's real T1 power
generator; **G** = damages an enemy ground structure; **A** = damages a patrolling
enemy scout aircraft. Damage proves a usable weapon, not exact damage, cadence,
range, projectile scripting, or performance against all eligible target layers.

## T1 land matrix

| Factory | Product | BP | Behavior | UI model | UI reachable |
|---|---|---|---|---|---|
| UEB0101 | UEL0101 | Checked | M, S | Move | Unverified |
| UEB0101 | UEL0103 | Checked | M, G | Move, Attack | Unverified |
| UEB0101 | UEL0104 | Checked | M, A | Move, Attack | Unverified |
| UEB0101 | UEL0105 | Checked | M, B | Move | Unverified |
| UEB0101 | UEL0106 | Checked | M, G | Move, Attack | Unverified |
| UEB0101 | UEL0201 | Checked | M, G | Move, Attack | Unverified |
| UAB0101 | UAL0101 | Checked | M, S | Move | Unverified |
| UAB0101 | UAL0103 | Checked | M, G | Move, Attack | Unverified |
| UAB0101 | UAL0104 | Checked | M, A | Move, Attack | Unverified |
| UAB0101 | UAL0105 | Checked | M, B | Move | Unverified |
| UAB0101 | UAL0106 | Checked | M, G | Move, Attack | Unverified |
| UAB0101 | UAL0201 | Checked | M, G | Move, Attack | Unverified |
| URB0101 | URL0101 | Checked | M, S | Move | Unverified |
| URB0101 | URL0103 | Checked | M, G | Move, Attack | Unverified |
| URB0101 | URL0104 | Checked | M, A | Move, Attack | Unverified |
| URB0101 | URL0105 | Checked | M, B | Move | Unverified |
| URB0101 | URL0106 | Checked | M, G | Move, Attack | Unverified |
| URB0101 | URL0107 | Checked | M, G | Move, Attack | Unverified |
| XSB0101 | XSL0101 | Checked | M, S | Move | Unverified |
| XSB0101 | XSL0103 | Checked | M, G | Move, Attack | Unverified |
| XSB0101 | XSL0104 | Checked | M, A | Move, Attack | Unverified |
| XSB0101 | XSL0105 | Checked | M, B | Move | Unverified |
| XSB0101 | XSL0201 | Checked | M, G | Move, Attack | Unverified |

## Exceptional abilities: evidence still needed

The downloaded wiki is WEB-tier evidence for selecting scenarios, never the
authority for numeric constants or proof that this engine implements a claim.
Paths below refer to the local gitignored mirror; a redirect or missing page is
not positive evidence. Confirm its claims against shipped blueprints/scripts
before implementing behavior.

| Product | Ability not covered by its primary-role scenario | Reference / limit |
|---|---|---|
| UAL0201 | Aurora crossing water and water-layer targeting | `reference/supcom-wiki/units/Aeon/Aeon_T1_Light_Tank.md`, description of hover movement; the current field is dry |
| XSL0103 | Zthuee crossing water and complete artillery salvo behavior | `reference/supcom-wiki/units/Seraphim/Seraphim_T1_Mobile_Light_Artillery.md`, amphibious indirect-fire role; damaging one nearby structure does not prove either |
| XSL0101 | Selen stationary cloak/stealth, losing concealment on movement or attack, and secondary combat role | `reference/supcom-wiki/units/Seraphim/Seraphim_T1_Combat_Scout.md`; the scout scenario tests only movement/vision/radar |
| URL0107 | Mantis repair | Retail `URL0107/URL0107_unit.bp` builder and Repair command-cap fields; the BP table checks these and Repair availability, but does not repair a damaged unit; no verified Mantis wiki page is claimed |
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

For another factory/tier, add an independent expected roster, extend the
scenario enumeration, and provide an executable role branch before marking
Behavior as checked. Add exceptional-ability cases by blueprint ID rather than
changing the generic role contract to pretend every unit has the same abilities.
Update this inventory in the same commit as its evidence. Products outside the
table are **not assessed here**, even if other parser or subsystem tests mention
them. Keep BP, Behavior, UI model and UI reachable separate when expanding it.
