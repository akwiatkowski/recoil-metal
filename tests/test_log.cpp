#include "core/log/Log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("logging filters by level and writes structured records to a file") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "recoil-metal-test.log";
    std::filesystem::remove(path);

    const auto configured = rm::log::configure({
        .level = rm::log::Level::Info,
        .filePath = path.string(),
        .stderrEnabled = false,
    });
    REQUIRE(configured);

    rm::log::write(rm::log::Level::Debug, "render", "hidden detail");
    rm::log::writef(rm::log::Level::Warn, "sim", "stalled at tick %d", 7);
    const std::string longMessage(700, 'x');
    rm::log::writef(rm::log::Level::Error, "content", "%s", longMessage.c_str());

    std::ifstream input{path};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    CHECK(contents.find("hidden detail") == std::string::npos);
    CHECK(contents.find("Z WARN [sim] stalled at tick 7\n") != std::string::npos);
    CHECK(contents.find("Z ERROR [content] " + longMessage + "\n") != std::string::npos);
    CHECK(contents.size() >= 24);
    CHECK(contents[4] == '-');
    CHECK(contents[7] == '-');
    CHECK(contents[10] == 'T');

    CHECK(rm::log::parseLevel("trace") == rm::log::Level::Trace);
    CHECK(rm::log::parseLevel("ERROR") == rm::log::Level::Error);
    CHECK_FALSE(rm::log::parseLevel("verbose"));

    REQUIRE(rm::log::configure({.level = rm::log::Level::Off,
                                .stderrEnabled = false}));
    std::filesystem::remove(path);
}
