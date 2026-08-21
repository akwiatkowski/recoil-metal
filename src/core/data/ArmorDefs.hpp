#pragma once

#include "core/lua/LuaValue.hpp"
#include "core/unit/Armor.hpp"

#include <string_view>
#include <vector>

namespace rm::data {

// The game's own armour table, read from the content it ships (PLAN2.md §7 P10.1, `ADR-033`).
//
// WHY THIS IS PARSED AND NOT HARDCODED, which is the whole point of the file and was proved by
// measurement rather than assumed:
//
//   **Retail Forged Alliance and FAF ship DIFFERENT tables**, and not by a little.
//
//     | | classes | non-1.0 entries | `Structure`/`Overcharge` |
//     |---|---:|---:|---|
//     | Retail `lua.scd:lua/armordefinition.lua` | 6 | 5 | **0.066666** |
//     | FAF `reference/FAF-fa/lua/armordefinition.lua` | 9 | 10 | **0.25** |
//
//   FAF adds `ExperimentalStructure`, `ASF` and `TMD`, and rebalances the multipliers by nearly
//   a factor of four. A constant compiled into the engine would be wrong for one of them
//   whichever we picked, and silently — a building taking a quarter of an Overcharge instead of
//   a fifteenth is a balance change nobody would trace back to a hardcoded table.
//
//   `ADR-033` cites the FAF figures because that is the copy that was read when the decision was
//   taken. Both are recorded here so the ADR's numbers are reproducible rather than merely
//   quoted, and neither is "the" answer: **the mounted content is.**
//
// WHAT THE MOUNTED RETAIL CORPUS ACTUALLY USES, measured over all 568 `*_unit.bp` in `units.scd`:
// `Normal` 296, `Structure` 206, `Light` 56, `Experimental` 6, `Commander` 4 — five classes, and
// every one of them is declared in the table above. No blueprint names a class the table omits,
// and no `ASF`/`TMD` appears at all, those being FAF's.
//
// THE FILE FORMAT, which is a Lua array of arrays of strings:
//
//     armordefinition = {
//         {   # Armor Type Name
//             'Structure',
//             # Armor Definition
//             'Normal 1.0',
//             'Overcharge 0.066666',
//         },
//         ...
//     }
//
// The first string in each block is the CLASS NAME; every later string is a
// `"<DamageType> <multiplier>"` pair separated by whitespace. Retail comments with `#` rather
// than `--`, which `core/lua` already accepts (`LuaTable.cpp:229-249`) because 209 mid-line `#`
// comments appear across the shipped blueprints.

/// A parsed armour table: the classes, and the exceptions to "full damage".
///
/// TOGETHER, because they are useless apart — a multiplier names a class, so a row can only be
/// resolved against the registry that numbered it. Returning them as one value is what stops a
/// caller building the registry from one file and the matrix from another.
struct ArmorTable {
    unitdef::ArmorRegistry registry;

    /// Only the rows that are NOT 1.0. A row of 1.0 is what an absent row already means, so
    /// keeping it would spend an override slot on saying nothing — see `DamageProfile`.
    std::vector<unitdef::ArmorMultiplier> multipliers;
};

/// Reads a parsed `armordefinition.lua`.
///
/// LENIENT, deliberately, and every skip below is a real shape in one of the two shipped copies
/// rather than defensive programming:
///
///   - A block with no strings at all is skipped. The table is written with comments between
///     the braces and a trailing comma after the last entry, which some readers surface as an
///     empty trailing element.
///   - A row that is not `"<name> <number>"` is skipped rather than failing the load. A malformed
///     row costs one multiplier; refusing the file costs every armour class in the game, and the
///     playable failure is the first one.
///   - A multiplier of exactly 1.0 is dropped, since that is the default.
///
/// Returns a table with just `default` when the value is not an armour definition at all, which
/// is what a caller mounting content without one should get: the pre-armour engine, not a crash.
[[nodiscard]] ArmorTable armorTableFrom(const lua::Value& parsed);

/// The same, from the file's text. Convenience for a caller holding bytes out of the VFS.
///
/// An unparseable file yields the `default`-only table, for the reason above: content that does
/// not load should cost the feature, not the match.
[[nodiscard]] ArmorTable armorTableFromSource(std::string_view source);

/// Where the file lives inside both games' content. Named rather than spelled at the call site
/// so the one string is not duplicated between the app and its tests.
inline constexpr std::string_view kArmorDefinitionPath = "lua/armordefinition.lua";

} // namespace rm::data
