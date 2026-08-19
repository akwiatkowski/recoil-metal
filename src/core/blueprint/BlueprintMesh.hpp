#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

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

} // namespace rm::blueprint
