#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <string>

namespace rm::blueprint {

// Where a Supreme Commander blueprint's geometry lives.
//
// THE MESH IS NOT NAMED IN THE FILE — not for props and not for units. A `.bp`'s
// LOD table gives textures and cutoff distances and never the geometry; the
// engine finds it by file name, and both content families spell the convention
// the same way:
//
//     env/Evergreen/props/Tree01_prop.bp   ->  Tree01_lod0.scm, Tree01_lod1.scm, …
//     units/UEL0201/UEL0201_unit.bp        ->  UEL0201_lod0.scm, UEL0201_lod1.scm, …
//
// One function rather than one per blueprint kind, because the only difference
// is which suffix the stem must end in, and having two copies of a filename
// convention is how they drift.
//
// The suffix is matched CASE-INSENSITIVELY, and so is the `_lodN` that replaces
// it, because the shipped content disagrees with itself: a map names
// `/env/tropical/props/trees/palm02_s4_prop.bp` in lower case while the archive
// holds `Palm02_s4_lod0.scm`, and `UEL0201_unit.bp` sits beside `UEL0201_LOD0.scm`
// in upper and `UEL0201_lod1.scm` in lower — in one directory. macOS's default
// filesystem is case-insensitive so the JOIN survives all of that; a string
// comparison would not, which is why the match is spelled out rather than left
// to `==`.
[[nodiscard]] std::filesystem::path meshBeside(const std::filesystem::path& blueprintPath,
                                              std::string_view expectedSuffix,
                                              std::size_t level);

/// The suffix a prop blueprint's stem ends in, and a unit's.
///
/// Named rather than spelled at each call site: these are the two the shipped
/// content uses, and a typo in a literal would read as "this blueprint has no
/// mesh" — a legitimate answer for an emitter, so it would not look like a bug.
inline constexpr std::string_view kPropSuffix = "_prop";
inline constexpr std::string_view kUnitSuffix = "_unit";
/// And a projectile's: `/projectiles/TDFGauss01/TDFGauss01_proj.bp` sits beside
/// `TDFGauss01_lod0.scm`, the same rule again.
inline constexpr std::string_view kProjectileSuffix = "_proj";

/// `C-271`'s bp→script binding: the (module path, class name) a blueprint's
/// script resolves to, retail's fallback chain verbatim.
///
/// `ScriptModule` (`bp+0x70`) wins when authored; else the blueprint's own
/// `Source` suffix convention — `X_unit.bp` → `X_script.lua`. `ScriptClass`
/// (`bp+0x8c`) wins when authored; else `"TypeClass"`. A module that imports
/// but lacks the class falls back to the class NAME inside the module; an
/// import failure falls back to `/lua/sim/{unit,projectile,prop}.lua`'s
/// `Unit`/`Projectile`/`Prop` — the caller supplies that default pair.
struct ScriptBinding {
    std::string module;   ///< VFS path of the Lua module to import
    std::string className;  ///< the class to read out of it
};

/// Resolves the binding for one blueprint. `authoredModule`/`authoredClass`
/// are the blueprint's `ScriptModule`/`ScriptClass` keys, empty when absent.
/// `fallbackModule`/`fallbackClass` are the import-failure default —
/// `/lua/sim/unit.lua`/`"Unit"` for a unit blueprint, and so on.
[[nodiscard]] ScriptBinding scriptBindingFor(std::string_view blueprintPath,
                                             std::string_view authoredModule,
                                             std::string_view authoredClass,
                                             std::string_view fallbackModule,
                                             std::string_view fallbackClass);

} // namespace rm::blueprint
