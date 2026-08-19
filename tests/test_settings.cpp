#include "core/settings/Settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// A settings file in a temporary place, removed with the test.
class TempSettings {
public:
    explicit TempSettings(std::string_view contents)
        : path_{std::filesystem::temp_directory_path() / "rm_test_settings.lua"} {
        std::ofstream out{path_};
        out << contents;
    }
    ~TempSettings() { std::filesystem::remove(path_); }

    TempSettings(const TempSettings&) = delete;
    TempSettings& operator=(const TempSettings&) = delete;
    TempSettings(TempSettings&&) = delete;
    TempSettings& operator=(TempSettings&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("with no settings file, everything is on") {
    // The default matters more than it looks: until this file existed, the value
    // compiled in was the only one any run would ever see. Looks-best rather than
    // costs-least, deliberately — see Settings.hpp.
    std::vector<std::string> problems;
    const rm::Settings settings =
        rm::loadSettings(std::filesystem::temp_directory_path() / "rm_no_such_settings.lua",
                         problems);

    CHECK(settings.reflections);
    CHECK(settings.stratumNormals);
    CHECK(settings.props);
    // A missing file is the ordinary case and must not be reported as a fault.
    CHECK(problems.empty());
}

TEST_CASE("a settings file turns switches off") {
    const TempSettings file{R"LUA(
-- a machine that would rather have the frame rate
{
    reflections = false,
    stratum_normals = true,
    props = false,
}
)LUA"};

    std::vector<std::string> problems;
    const rm::Settings settings = rm::loadSettings(file.path(), problems);

    CHECK_FALSE(settings.reflections);
    CHECK(settings.stratumNormals);
    CHECK_FALSE(settings.props);
    CHECK(problems.empty());
}

TEST_CASE("a key the file does not mention keeps its default") {
    const TempSettings file{"{ props = false }"};

    std::vector<std::string> problems;
    const rm::Settings settings = rm::loadSettings(file.path(), problems);

    CHECK_FALSE(settings.props);
    CHECK(settings.reflections);
    CHECK(settings.stratumNormals);
    CHECK(problems.empty());
}

TEST_CASE("a setting of the wrong type is reported, not coerced") {
    // `props = "yes"` is somebody expecting something. Reading it as false would
    // be a setting that silently stopped working.
    const TempSettings file{"{ props = 'yes', reflections = 0 }"};

    std::vector<std::string> problems;
    const rm::Settings settings = rm::loadSettings(file.path(), problems);

    CHECK(problems.size() == 2);
    // ...and the defaults stand, so a typo costs a warning rather than the frame.
    CHECK(settings.props);
    CHECK(settings.reflections);
}

TEST_CASE("a settings file that is not Lua data is reported") {
    const TempSettings file{"{ props = someVariable }"};

    std::vector<std::string> problems;
    const rm::Settings settings = rm::loadSettings(file.path(), problems);

    REQUIRE(problems.size() == 1);
    CHECK(problems.front().find("not read") != std::string::npos);
    CHECK(settings.props);
}

TEST_CASE("the settings file sits where macOS keeps such things") {
    const std::filesystem::path path = rm::settingsPath();
    CHECK(path.filename() == "settings.lua");
    CHECK(path.parent_path().filename() == "recoil-metal");
    // Not ~/.config, which is a Linux convention this project has no reason to
    // import — the same settled decision that rules out SDL.
    CHECK(path.string().find("Library/Application Support") != std::string::npos);
}
