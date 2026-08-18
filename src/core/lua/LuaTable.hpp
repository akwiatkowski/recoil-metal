#pragma once

#include "core/lua/LuaValue.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace rm::lua {

struct ParseError {
    std::string message;  ///< includes a line number
    std::size_t line = 0;
};

// Parses the first table literal in a Lua source file — the shape every
// mapinfo.lua uses:
//
//     local mapinfo = { name = "...", smf = { minheight = -150 }, ... }
//     return mapinfo
//
// SCOPE, stated plainly. This is a parser for Lua *data*, not an interpreter
// for Lua *programs*. It understands comments, nested tables, strings (quoted
// and [[long]]), numbers (decimal, hex, exponent, unary sign), booleans, nil,
// positional entries, `[key] =` entries, and both `,` and `;` separators.
//
// It does NOT evaluate anything: no arithmetic, no concatenation, no variables,
// no `VFS.DirList` merge hooks. A mapinfo.lua that *computes* a value gets a
// ParseError rather than a guess, and callers fall back to whatever the binary
// header said. That is the honest failure mode; embedding a real Lua interpreter
// would be a dependency this repo does not want, and guessing at computed values
// would reintroduce exactly the silent-wrongness bug that motivated reading
// mapinfo.lua in the first place.
//
// THE ONE EXCEPTION, and why it is not a hole in the rule. Supreme Commander's
// map files wrap every leaf in a *data constructor* — `VECTOR3( 1, 2, 3 )`,
// `STRING( 'x' )`, `GROUP { ... }` — which take literal arguments and hand them
// straight back. Those are data written in call syntax, not computation, so
// this reader evaluates exactly six of them by name and refuses every other
// call as before. The set is closed by evidence rather than by taste: they are
// the only calls appearing anywhere in the 61 stock maps' _save.lua, which the
// corpus tests re-check. Wrong argument counts and wrong argument types are
// errors too, so a file that disagrees with this reader says so.
//
// Bracketed numeric keys are stored under their decimal spelling, so `[0]` and
// `["0"]` collide. Harmless for map metadata; noted rather than hidden.
[[nodiscard]] std::expected<Value, ParseError> parseTable(std::string_view source);

/// Parses the first table literal in a file. A missing file is a ParseError.
[[nodiscard]] std::expected<Value, ParseError> parseTableFile(std::string_view path);

} // namespace rm::lua
