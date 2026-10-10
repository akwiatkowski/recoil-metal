#include "core/log/Log.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
namespace fs = std::filesystem;

void environment(const char* name, const std::string& value) {
    if (setenv(name, value.c_str(), 1) != 0)
        throw std::runtime_error(std::string("Cannot set ") + name + ": " + std::strerror(errno));
}

int prepare(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child = 0;
    const int error = posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error != 0) throw std::runtime_error("Cannot run preparation tool: " + std::string(std::strerror(error)));
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot wait for preparation tool");
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

void help() {
    std::cout << R"(recoil-bar: real BAR skirmish through Recoil / SDL3 / Mesa Zink / Kosmickrisp.
Usage: mise exec -- build/recoil-bar [options]
  --engine PATH             Existing pinned spring executable
  --mesa DIR                Pinned macOS Mesa install directory
  --game DIR                External pinned BAR checkout (never modified)
  --map PATH                External .sd7/.sdz map archive
  --map-name NAME           Engine map name (default: Angel Crossing 1.4)
  --map-sha256 SHA256        Required checksum for a different map archive
  --font PATH               Pinned FreeSansBold.otf
  --vulkan-loader PATH      Pinned libvulkan.1.dylib
  --dependencies-dir DIR    Imported pinned non-system dylibs
  --bundle DIR              Bundle exported by tools/bar_runtime.py
  --data-dir DIR            Isolated writable directory (default: build/bar)
  --source-root DIR         Project directory containing tools/ and src/
  --prepare-only            Verify pins and write mounts/script; do not play
  --smoke                   BAR's real test runner: move/build/AI evidence, then quit
  --help                    Show this help

Defaults use the project-owned engine/Mesa builds under build/bar and external game/content paths.
Portable setup, source fetch recipes, verified bundle export/import and licensing:
  mise exec -- python3 tools/bar_runtime.py --help
No game assets are included. SimpleAI is BAR's actual built-in Lua opponent,
not native BARb (native skirmish AI libraries are not part of the selected engine build).
Normal launch keeps BAR Lua UI, simulation and OpenGL rendering intact. Engine
stdout/stderr and exit status are preserved. The game starts after loading;
left-click selects, right-click orders, and the build menu constructs units.
)";
}
}  // namespace

int main(int argc, char** argv) {
    try {
#ifdef RM_SOURCE_DIR
        fs::path root = RM_SOURCE_DIR;
#else
        fs::path root = fs::absolute(__FILE__).parent_path().parent_path().parent_path().parent_path();
#endif
        const char* home = std::getenv("HOME");
        const fs::path faf = fs::path(home ? home : "") / "projects/llm/games/faf";
        fs::path engine = root / "build/bar/engine-build/spring";
        fs::path mesa = root / "build/bar/mesa-install";
        fs::path game = faf / "forged-alliance-reborn/reference/BAR";
        fs::path map = fs::path(home ? home : "") / "projects/llm/input/recoil/maps/angel_crossing_1.4.sd7";
        fs::path font = faf / "recoil-macos/cont/fonts/FreeSansBold.otf";
        fs::path vulkan = "/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib";
        fs::path dependencies;
        fs::path data;
        std::string mapName = "Angel Crossing 1.4";
        std::string mapHash;
        bool prepareOnly = false;
        bool smoke = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--help" || flag == "-h") { help(); return 0; }
            if (flag == "--prepare-only") { prepareOnly = true; continue; }
            if (flag == "--smoke") { smoke = true; continue; }
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + std::string(flag));
            const std::string value = argv[++i];
            if (flag == "--engine") engine = value;
            else if (flag == "--mesa") mesa = value;
            else if (flag == "--game") game = value;
            else if (flag == "--map") map = value;
            else if (flag == "--font") font = value;
            else if (flag == "--vulkan-loader") vulkan = value;
            else if (flag == "--dependencies-dir") dependencies = value;
            else if (flag == "--data-dir") data = value;
            else if (flag == "--source-root") root = value;
            else if (flag == "--map-name") mapName = value;
            else if (flag == "--map-sha256") mapHash = value;
            else if (flag == "--bundle") {
                const fs::path bundle(value);
                engine = bundle / "engine/spring";
                mesa = bundle / "mesa";
                game = bundle / "game.sdd";
                map = bundle / "map/angel_crossing_1.4.sd7";
                font = bundle / "font/FreeSansBold.otf";
                dependencies = bundle / "dependencies";
                vulkan = dependencies / "libvulkan.1.dylib";
            } else throw std::runtime_error("Unknown option: " + std::string(flag));
        }
        root = fs::absolute(root);
        engine = fs::absolute(engine);
        mesa = fs::absolute(mesa);
        if (data.empty()) data = root / "build/bar";
        data = fs::absolute(data);
        const auto configured = rm::log::configure({});
        if (!configured) throw std::runtime_error(configured.error());
        std::vector<std::string> prep = {"mise", "exec", "--", "python3",
            (root / "tools/bar_runtime.py").string(), "prepare", "--engine", engine.string(),
            "--mesa", mesa.string(), "--game", fs::absolute(game).string(), "--map", fs::absolute(map).string(),
            "--font", fs::absolute(font).string(), "--vulkan-loader", fs::absolute(vulkan).string(),
            "--data-dir", data.string(), "--map-name", mapName};
        if (!dependencies.empty()) { prep.emplace_back("--dependencies-dir"); prep.push_back(fs::absolute(dependencies).string()); }
        if (!mapHash.empty()) { prep.emplace_back("--map-sha256"); prep.push_back(mapHash); }
        if (smoke) prep.emplace_back("--smoke");
        const int result = prepare(prep);
        if (result != 0) return result;
        if (prepareOnly) return 0;

        environment("VK_DRIVER_FILES", (data / "kosmickrisp.json").string());
        environment("MESA_VULKAN_LIBRARY", fs::absolute(vulkan).string());
        environment("EGL_PLATFORM", "surfaceless");
        environment("MESA_LOADER_DRIVER_OVERRIDE", "zink");
        environment("MESA_GL_VERSION_OVERRIDE", "4.6COMPAT");
        environment("MESA_GLSL_VERSION_OVERRIDE", "460");
        environment("SPRING_DATADIR", data.string());
        environment("DYLD_LIBRARY_PATH", (data / "runtime-libs").string());
        // Keep this patched runtime's shader cache out of the user's global Mesa cache.
        environment("MESA_SHADER_CACHE_DIR", (data / "shader-cache").string());
        // Run the engine directly: invoking a system shell here would trigger SIP
        // stripping DYLD_* and would make imported bundles resolve donor paths.
        std::vector<std::string> arguments = {engine.string(), "--isolation", "--isolation-dir=" + data.string(),
            "--write-dir=" + data.string(), "--config=" + (data / "springsettings.cfg").string(),
            (data / "skirmish.txt").string()};
        std::vector<char*> engineArgv;
        engineArgv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) engineArgv.push_back(argument.data());
        engineArgv.push_back(nullptr);
        rm::log::writef(rm::log::Level::Info, "bar", "Launching pinned BAR skirmish: %s", engine.c_str());
        if (chdir(data.c_str()) != 0) throw std::runtime_error("Cannot enter data directory: " + std::string(std::strerror(errno)));
        execv(engine.c_str(), engineArgv.data());
        throw std::runtime_error("Cannot execute engine: " + std::string(std::strerror(errno)));
    } catch (const std::exception& error) {
        rm::log::write(rm::log::Level::Error, "bar", error.what());
        return 1;
    }
}
