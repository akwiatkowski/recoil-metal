#pragma once

// BAR's display names, out of language/<code>/units.json.
//
// The BAR family is the opposite of Supreme Commander's about naming: a unit `.lua` carries
// no player-facing text at all, and the names live per language in a Transifex-managed JSON
// file — `units.names.armstump` is the flavour name ("Stout"), `units.descriptions.armstump`
// the type name ("Medium Assault Tank"). This reads the TYPE name, because that is the one
// vocabulary the interface speaks (the same decision that picked FA's `Description` over
// `UnitName` — see UnitBlueprint.cpp).
//
// A SCANNER, NOT A JSON LIBRARY, deliberately. The file is two flat string maps and the
// question is "one key's value"; a parser dependency for that would be the exact thing
// AGENT.md's YAGNI rule names. The scanner honours quoting and escapes, and answers empty
// for anything malformed — the caller falls back to the unit's id, which is what every BAR
// unit showed before names existed.

#include <string>
#include <string_view>

namespace rm::unitdef {

/// The type name for `key` out of a `units.json`'s text — `descriptions.<key>` — or empty
/// when the file, the section, or the key is absent or malformed.
[[nodiscard]] std::string barUnitName(std::string_view unitsJson, std::string_view key);

} // namespace rm::unitdef
