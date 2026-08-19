#pragma once

#include "core/lua/LuaTable.hpp"
#include "core/unit/UnitDef.hpp"

#include <expected>
#include <filesystem>

namespace rm::unitbp {

// A Supreme Commander unit blueprint, read into the SAME UnitDef the BAR loader
// fills.
//
// The whole point, and the same seam ADR-004 drew for models: a second content
// family arrives without anything downstream noticing. The sim asks a UnitDef how
// fast a unit moves and how much room it takes up; it does not ask which game
// authored it.
//
// WHAT THE TWO FAMILIES GENUINELY DISAGREE ABOUT, since that is all this file
// really is:
//
//   speed      ogrids/second, so x8 (BAR's `speed` is already elmos/second)
//   turn rate  DEGREES per second, plainly — where BAR authors circle divisions
//              per frame and needs a 65536 and a tick rate to make sense of
//   size       fractional ogrids in `SizeX`/`SizeZ` at the file's ROOT, against
//              BAR's whole squares. 418 of 568 are fractional
//   slope      NOT STATED. BAR gives every unit a `maxslope` and a
//              `maxwaterdepth`; a `.bp` gives a MotionType and nothing else, so
//              where a unit may go is derived from its class
//   name       NOT STATED either. There is no `BlueprintId` in any of the 568,
//              so the unit's id is its FILE NAME — `UEL0201_unit.bp` is UEL0201
//   mesh       NOT STATED. Found beside the blueprint by convention; see
//              core/blueprint/BlueprintMesh.hpp
//
// Everything else a blueprint carries is for a game this engine does not yet
// play: weapons, build options, intel radii, veterancy, audio cues, the Lua
// script class. Read and ignored, on the same rule the BAR loader states — adding
// a field here should mean something reads it.
[[nodiscard]] std::expected<unitdef::UnitDef, lua::ParseError> loadFile(
    const std::filesystem::path& path);

/// Finds a unit's geometry on disk, or returns empty when it has none.
///
/// Split from `loadFile` for the same reason BAR's `unitdef::resolveModel` is:
/// reading a definition is arithmetic and resolving its mesh touches the
/// filesystem, and the corpus tests want to do the first 568 times without
/// paying for the second.
///
/// TWO WAYS A UNIT NAMES ITS MESH, across the 568 shipped blueprints:
///
///   543  by CONVENTION — no `MeshName`, so the mesh is `<id>_lod0.scm` beside the
///        blueprint (core/blueprint/BlueprintMesh.hpp)
///    25  a VFS PATH in `MeshName` — `/Env/Structures/Props/UEF_Warehouse01_lod0.scm`
///        — which is why `root` exists; without one these come back empty
///
/// All 25 end in `.scm`. Worth stating because a survey by regex suggests a third
/// form, `MeshName = 'UEC1101'`, a bare reference to another unit — and that is an
/// artefact: the field is `PlaceholderMeshName`, an editor leftover whose name ends
/// in the one being searched for. Reading it as geometry would put a civilian
/// warehouse where sixteen campaign units should be. Parse, then look; a substring
/// match on a field name is not a field.
///
/// A unit whose mesh is absent comes back empty, which is the honest answer —
/// invisible marker units are a thing the game has, exactly as emitter props are.
///
/// `root` is the extracted game root a VFS path is relative to. Optional because
/// the conventional form does not need it, and most callers have a blueprint long
/// before they have a root.
[[nodiscard]] std::filesystem::path resolveMesh(const unitdef::UnitDef& def,
                                                const std::filesystem::path& blueprintPath,
                                                const std::filesystem::path& root = {});

} // namespace rm::unitbp
