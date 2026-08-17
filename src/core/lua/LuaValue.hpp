#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rm::lua {

struct Field;

// A parsed Lua value: nil, boolean, number, string, or table.
//
// This exists to read mapinfo.lua, which the engine treats as authoritative for
// map metadata the binary .smf either lacks or gets wrong. See LuaTable.hpp for
// what this deliberately does NOT do.
class Value {
public:
    // The obvious spellings for the first two — Nil and Boolean — are both
    // taken by system macros/typedefs (`Nil` in <objc/objc.h>, `Boolean` in
    // <MacTypes.h>), and this header is included from .mm translation units.
    // Even scoped enumerators lose to the preprocessor, so they are renamed
    // rather than fought. None == Lua's nil.
    enum class Type { None, Bool, Number, Text, Table };

    Type type = Type::None;
    bool boolean = false;
    double number = 0.0;
    std::string text;

    // std::vector may hold an incomplete element type (C++17 onwards), which is
    // what lets these two members close the recursion. Every member function
    // touching them is therefore defined out of line, after Field is complete.
    std::vector<Field> fields;  ///< string-keyed entries, in source order
    std::vector<Value> items;   ///< positional entries (Lua's array part)

    [[nodiscard]] bool isNil() const noexcept { return type == Type::None; }
    [[nodiscard]] bool isTable() const noexcept { return type == Type::Table; }

    /// Direct child by key. Returns nullptr when absent or when this is not a
    /// table. Never throws — absence is ordinary.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;

    /// Dotted lookup: path("smf", "minheight"). Any missing link yields nullptr.
    template <typename... Keys>
    [[nodiscard]] const Value* path(std::string_view first, Keys... rest) const noexcept {
        const Value* child = find(first);
        if constexpr (sizeof...(rest) == 0) {
            return child;
        } else {
            return child == nullptr ? nullptr : child->path(rest...);
        }
    }

    // Typed reads. Each returns nullopt when the value is absent or is of a
    // different type — never a coerced default, because silently substituting
    // 0.0 for a missing height is exactly the class of bug this file exists to
    // avoid.
    [[nodiscard]] std::optional<double> asNumber() const noexcept;
    [[nodiscard]] std::optional<std::string_view> asString() const noexcept;
    [[nodiscard]] std::optional<bool> asBoolean() const noexcept;

    /// Convenience: numeric child at a key path.
    [[nodiscard]] std::optional<double> numberAt(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<std::string_view> stringAt(std::string_view key) const noexcept;
};

struct Field {
    std::string key;
    Value value;
};

} // namespace rm::lua
