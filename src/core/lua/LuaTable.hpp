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
// no function calls, no `VFS.DirList` merge hooks. A mapinfo.lua that *computes*
// a value gets a ParseError rather than a guess, and callers fall back to
// whatever the binary header said. That is the honest failure mode; embedding a
// real Lua interpreter would be a dependency this repo does not want, and
// guessing at computed values would reintroduce exactly the silent-wrongness
// bug that motivated reading mapinfo.lua in the first place.
//
// Bracketed numeric keys are stored under their decimal spelling, so `[0]` and
// `["0"]` collide. Harmless for map metadata; noted rather than hidden.
[[nodiscard]] std::expected<Value, ParseError> parseTable(std::string_view source);

/// Parses the first table literal in a file. A missing file is a ParseError.
[[nodiscard]] std::expected<Value, ParseError> parseTableFile(std::string_view path);

} // namespace rm::lua
