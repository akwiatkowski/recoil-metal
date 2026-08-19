#include "core/settings/Settings.hpp"

#include "core/lua/LuaTable.hpp"

#include <cstdlib>

namespace rm {
namespace {

/// Reads one boolean, leaving the default in place when the key is absent.
///
/// A key present with the WRONG type is reported rather than coerced, for the
/// same reason the Lua reader refuses `STRING(3)`: `props = "yes"` is somebody
/// expecting something, and quietly reading it as false would be worse than
/// saying so.
void readFlag(const lua::Value& table, const char* key, bool& into,
              std::vector<std::string>& problems) {
    const lua::Value* value = table.find(key);
    if (value == nullptr || value->isNil()) {
        return;
    }
    if (const std::optional<bool> flag = value->asBoolean()) {
        into = *flag;
        return;
    }
    problems.emplace_back(std::string{"settings: '"} + key + "' is not true or false");
}

} // namespace

std::filesystem::path settingsPath() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home == nullptr ? "" : home} / "Library" / "Application Support"
           / "recoil-metal" / "settings.lua";
}

Settings loadSettings(const std::filesystem::path& path, std::vector<std::string>& problems) {
    Settings settings;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return settings;  // the ordinary case
    }

    const auto table = lua::parseTableFile(path.string());
    if (!table) {
        problems.push_back("settings: " + path.string() + " not read: " + table.error().message);
        return settings;
    }

    readFlag(*table, "reflections", settings.reflections, problems);
    readFlag(*table, "stratum_normals", settings.stratumNormals, problems);
    readFlag(*table, "props", settings.props, problems);
    return settings;
}

} // namespace rm
