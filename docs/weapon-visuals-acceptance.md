# Lua-driven weapon visuals

Weapon effects resolve from the mounted game's original projectile/unit scripts,
faction classes, `EffectTemplates.lua`, and `defaultcollisionbeams.lua`. No weapon
name table selects effects in production. A separate Lua state runs declarations;
gameplay callbacks and native engine instances are not executed.

Including muzzle and impact lists, the retail corpus resolves 3,817 visual
definitions using 1,328 emitter materials (empty authored lists are retained).
SIFInainoStrategicMissileEffect01 reports unsupported content because it has no
TypeClass. The electron-bolter emitter correctly starts at zero size and grows.
These counts describe
resolution, not complete reproduction of every emitter's behaviour.

The selected examples are UEF Gauss, Aeon Disruptor, Cybran heavy laser, Seraphim
Oh cannon, Cybran particle cannon, and Cybran microwave beam. The production
resolver is generic; these names are only gallery/test examples.

## Verification

Run `mise exec -- ./build/rm_tests '[weapon-visuals],[fx]'` for real-corpus
resolution, inherited declarations, a VFS Lua override, original texture loading,
bolt positioning, beam endpoints/lifetime, fallback lookup, and cosmetic hash
exclusion. The override test changes a Lua PolyTrails declaration and checks that
the resulting material changes without editing a C++ mapping.

For an offscreen visual check, use the usual map and gamedata arguments with
`--skirmish --observer --armies 2 --play 0 --mute --weapon-gallery
--screenshot build/weapon-gallery.png 1600 900`. No window is opened. The gallery
draws six labelled samples above the terrain and uses a one-elmo width floor for
inspection; ordinary bolts use the existing screen-space visibility floor.
Generated images and logs belong in the ignored `build/` directory and can be
recreated; no documentation depends on temporary files or bundled game assets.

Simulation validation compares a full skirmish's per-tick hashes against the
pre-visual-change run. This checks our simulation invariance, not retail parity.

Validated on 2026-09-06: CTest reported 1,463 passed and two skipped tests out of
1,465 entries. The offscreen gallery rendered all six sample groups; some bolt
heads remain faint at this scale. The 1,800-second FAF skirmish matched all 18,000
ticks in the pre-visual-change hash log. No interactive window testing was used.

## Current limits

The authored-emitter follow-up now evaluates linear curves and their random
ranges, integrates emission rates, interpolates particle birth positions, and
preserves particle lifetime, size changes, velocity/acceleration, rotation,
texture-frame animation and colour-ramp selection. Rendering supports alpha,
inverse modulation, double inverse modulation, additive, premultiplied alpha,
and refraction. Flat emitters use the world XZ plane. Dimensions and scrolling
follow the original particle shader and blueprint conventions.

Sources: retail `effects/particle.fx` and the local FAF source annotations in
`engine/Core/Blueprints/{Effect,Emitter,Trail,Beam}Blueprint.lua`. The original
distortion ring is included only in the gallery to exercise the scene-copy path.
It adds one gallery definition/material to the corpus counts above.

Focused verification currently passes 101 assertions across ten cases, including
rate integration, interpolated births, lifetime/size curves and the original flat
distortion ring. Full CTest passes 1,465 tests with two skips (1,467 entries,
46.13 seconds; `build/emitter-ctest.log`). Offscreen refraction changes pixels within (745,540)–(1131,900)
of the 1600×900 gallery when compared with the same scene without that emitter.

Original muzzle and impact lists now execute through persistent event emitters,
with source scales and explicit impact-type selection. Muzzle emitters follow
the resolved bone's world placement and stop emitting when its original unit
dies; emitted particles finish their own lifetimes. Projectile impact events carry
source identity and trajectory after the projectile is removed. Empty Lua lists
produce no generic replacement. The Seraphim ACU import now accepts retail's
`0then` spelling in AIUtilities.lua without rewriting strings or comments.

Add `--impact-gallery` to the weapon gallery command to capture muzzle, terrain
impact and unit impact columns for the four tank families at 50 ms. The inspected
capture is `build/weapon-impact-gallery.png`; its log records the selected weapon
labels. Focused checks pass 156 assertions across fourteen cases. Full CTest
passes 1,468 tests with two skips (1,470 entries;
`build/impact-ctest.log`). Short emitters now produce their initial particle at
creation; finite lifetimes stop further emission while particles finish naturally.
Negative authored lifetimes remain unbounded, with attached emitters removed when
their original owner dies.

Beams now follow live endpoints: each tick draws one strip from the shooter's
resolved muzzle bone to the target's current position for the authored lifetime,
a refire replaces the weapon's live beam, and a dead shooter removes it (ADR-099).
Endpoints are unit transforms, not target bones. The `[weapon-visuals]` case
"beam strips follow their live endpoints" covers registration, moved endpoints,
continuous strip age, expiry and replacement; no separate visual capture was made
for this slice.

Historical ribbon trails now draw the original TrailBlueprints over each shot's
recorded path (ADR-101). A shot whose visuals include a trail material receives a
presentation serial on first sight; its positions are recorded once per tick from
the muzzle onward, and every frame the last TrailLength elmos of that path are
emitted as strip segments carrying a trail-relative coordinate, so the authored
ramp reads head to tail across the whole ribbon and TextureRepeatRate repeats
along it. After impact the ribbon keeps moving at the shot's last speed and slides
out of the window. The `[weapon-visuals]` case "projectile ribbons retain turns"
covers serial assignment, the turn, clipping to the authored length, head
extrapolation between ticks and draining; the state-hash case confirms the
serial and origin are cosmetic. The offscreen gallery adds a bending arc behind
each bolt sample (`weapon gallery ribbon:` lines report the segment count); the
2026-09-06 capture shows the Gauss and heavy-laser ribbons curving, faint at the
gallery scale. TextureRepeatRate is read as repeats per ogrid of TrailLength, the
beam convention; retail's exact trail shader was not consulted.

Original projectile meshes now draw through the unit pipeline (ADR-103): the mesh
beside `Display.MeshBlueprint`'s mesh blueprint, or beside the projectile blueprint,
is loaded once per distinct mesh and scaled by `Display.UniformScale`; visible shots
with a mesh are drawn as instances pointed along their velocity, team coloured, and
their bolt strip is skipped. The corpus test checks the Gauss mesh, a shared
mesh-blueprint batch, a LOD table that names another mesh outright and the
instance's yaw and pitch. Of the 287 retail projectile blueprints, 43 have a mesh
beside them and 65 name a mesh blueprint; a mesh blueprint's LOD 0 `MeshName`,
`AlbedoName` and `NormalsName` win over the file-name rule when present (the shared
`missile_default_mesh.bp` points at the Flayer missile's geometry). The app loads
100 distinct meshes. Blueprints whose mesh blueprint is absent from every retail
archive, the Seraphim Laanse tactical missile family among them, keep their strips.
No offscreen capture of a mesh in flight was made in this slice; orientation
handedness is asserted against the unit shader's rotation order, not a picture.

Full retail pixel parity is not claimed.
Unresolved visuals retain the existing procedural fallback. Full native-emitter
parity, including drag and every alignment/water flag, is not yet claimed.
