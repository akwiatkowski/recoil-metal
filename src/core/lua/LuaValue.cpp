#include "core/lua/LuaValue.hpp"

#include <algorithm>

namespace rm::lua {

const Value* Value::find(std::string_view key) const noexcept {
    if (type != Type::Table) {
        return nullptr;
    }

    // Linear scan: map metadata tables have a handful of keys each, and keeping
    // source order matters more here than lookup speed.
    const auto match = std::find_if(fields.begin(), fields.end(),
                                    [key](const Field& field) { return field.key == key; });
    return match == fields.end() ? nullptr : &match->value;
}

std::optional<double> Value::asNumber() const noexcept {
    if (type != Type::Number) {
        return std::nullopt;
    }
    return number;
}

std::optional<std::string_view> Value::asString() const noexcept {
    if (type != Type::Text) {
        return std::nullopt;
    }
    return std::string_view{text};
}

std::optional<bool> Value::asBoolean() const noexcept {
    if (type != Type::Bool) {
        return std::nullopt;
    }
    return boolean;
}

std::optional<double> Value::numberAt(std::string_view key) const noexcept {
    const Value* child = find(key);
    return child == nullptr ? std::nullopt : child->asNumber();
}

std::optional<std::string_view> Value::stringAt(std::string_view key) const noexcept {
    const Value* child = find(key);
    return child == nullptr ? std::nullopt : child->asString();
}

} // namespace rm::lua
