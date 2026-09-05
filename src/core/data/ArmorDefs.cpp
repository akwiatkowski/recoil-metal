#include "core/data/ArmorDefs.hpp"

#include "core/lua/LuaTable.hpp"

#include <cstdlib>
#include <string>

namespace rm::data {
namespace {

/// Splits `"Overcharge 0.066666"` into its damage type and its multiplier.
///
/// `strtod`, which is what `core/lua` already puts every content number through
/// (`LuaTable.cpp:367`). Two alternatives were tried and rejected:
///
///   `std::from_chars` would be locale-independent by specification, which is the property worth
///   having — but Apple's libc++ ships no floating-point overload, so the call resolves to the
///   DELETED `bool` one. It compiles as a `-Wfloat-conversion` error rather than silently, which
///   is the good failure, but it is still not available.
///
///   A hand-rolled decimal parser would avoid the C locale entirely, and was rejected for being
///   a *second* way to read a number in a codebase that already has one. `strtod`'s decimal point
///   is locale-dependent, so a European locale would read `0.066666` as `0` — but that hazard is
///   already uniform across the whole content layer, since every blueprint number takes the same
///   path. Nothing here calls `setlocale`, so the C locale is in force. If that ever changes, it
///   breaks the blueprint reader first and this with it, and the fix belongs there.
[[nodiscard]] bool splitRow(std::string_view row, std::string_view& type, float& value) {
    const std::size_t gap = row.find_first_of(" \t");
    if (gap == std::string_view::npos) {
        return false;
    }
    type = row.substr(0, gap);
    if (type.empty()) {
        return false;
    }

    const std::size_t numberStart = row.find_first_not_of(" \t", gap);
    if (numberStart == std::string_view::npos) {
        return false;
    }

    // NUL-terminated for `strtod`, which a `string_view` is not.
    const std::string number{row.substr(numberStart)};
    char* parseEnd = nullptr;
    const double parsed = std::strtod(number.c_str(), &parseEnd);
    if (parseEnd == number.c_str()) {
        return false;  // nothing numeric at all
    }
    value = static_cast<float>(parsed);
    return true;
}

/// The class name and its rows, for one block of the table.
struct Block {
    std::string_view name;
    std::vector<std::string_view> rows;
};

[[nodiscard]] std::vector<Block> blocksOf(const lua::Value& definition) {
    std::vector<Block> blocks;
    blocks.reserve(definition.items.size());

    for (const lua::Value& entry : definition.items) {
        Block block;
        for (const lua::Value& row : entry.items) {
            if (row.text.empty()) {
                continue;
            }
            if (block.name.empty()) {
                block.name = row.text;
            } else {
                block.rows.push_back(row.text);
            }
        }
        // A block with no strings is the trailing-comma artefact, not a class.
        if (!block.name.empty()) {
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

} // namespace

ArmorTable armorTableFrom(const lua::Value& parsed) {
    ArmorTable table;

    // The value may be the file (a table with an `armordefinition` field) or the definition
    // itself, because a test writes the second and the VFS yields the first. Accepting both
    // costs one branch and removes a wrapper nobody would remember to apply.
    const lua::Value* definition = parsed.find("armordefinition");
    if (definition == nullptr) {
        definition = &parsed;
    }

    const std::vector<Block> blocks = blocksOf(*definition);
    if (blocks.empty()) {
        return table;  // `default` only — see the header
    }

    // TWO PASSES, because a multiplier names a class and a class needs a number first. The
    // registry sorts, so no row can be resolved until every name has been seen.
    std::vector<std::string_view> names;
    names.reserve(blocks.size());
    for (const Block& block : blocks) {
        names.push_back(block.name);
    }
    table.registry = unitdef::ArmorRegistry::fromNames(names);

    for (const Block& block : blocks) {
        const ArmorClass armor = table.registry.classFor(block.name);
        for (const std::string_view row : block.rows) {
            std::string_view type;
            float value = 1.0f;
            if (!splitRow(row, type, value)) {
                continue;  // a malformed row costs one multiplier, not the file
            }
            // 1.0 is what an ABSENT row already means. Dropping it here is what keeps the
            // shipped tables down to 5 (retail) and 10 (FAF) entries rather than 6 and 21.
            if (value == 1.0f) {
                continue;
            }
            table.multipliers.push_back(unitdef::ArmorMultiplier{
                .armor = armor,
                .damageType = std::string{type},
                .multiplier = value,
            });
        }
    }

    return table;
}

ArmorTable armorTableFromSource(std::string_view source) {
    const std::expected<lua::Value, lua::ParseError> parsed = lua::parseTable(source);
    if (!parsed) {
        return ArmorTable{};
    }
    return armorTableFrom(*parsed);
}

} // namespace rm::data
