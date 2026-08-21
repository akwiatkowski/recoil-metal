#include "app/View.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace rm::app {

/// How far back `--focus` should sit, in model radii, or 0 when it was not
/// given at all.
///
/// 2.5 radii fills the frame with one unit, which is what the flag was for —
/// checking a model. A squad, or a ring around each of several units, needs to
/// see further out, so the distance takes an optional argument rather than
/// forcing a choice between one unit and the whole map.
[[nodiscard]] float parseFocus(int argc, const char* argv[]) {
    constexpr float kDefaultRadiiBack = 2.5f;

    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} != "--focus") {
            continue;
        }
        if (i + 1 >= argc) {
            return kDefaultRadiiBack;
        }
        char* end = nullptr;
        const double radii = std::strtod(argv[i + 1], &end);
        // Only a bare number counts as the argument; the next flag does not.
        if (end != argv[i + 1] && *end == '\0' && radii > 0.0) {
            return static_cast<float>(radii);
        }
        return kDefaultRadiiBack;
    }
    return 0.0f;
}





[[nodiscard]] LookOptions parseLook(int argc, const char* argv[]) {
    LookOptions options;
    for (int i = 2; i + 3 < argc; ++i) {
        if (std::string{argv[i]} != "--look") {
            continue;
        }
        options.enabled = true;
        options.x = static_cast<float>(std::atof(argv[i + 1]));
        options.z = static_cast<float>(std::atof(argv[i + 2]));
        options.radiusElmos = static_cast<float>(std::atof(argv[i + 3]));
        break;
    }
    return options;
}


/// Recognises `--bench <frames> <out.csv>` anywhere after the map path.
[[nodiscard]] BenchOptions parseBench(int argc, const char* argv[]) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg != "--bench" && arg != "--bench-offscreen") {
            continue;
        }
        options.enabled = true;
        options.offscreen = (arg == "--bench-offscreen");
        if (i + 1 < argc) {
            options.frames = static_cast<std::size_t>(std::max(1, std::atoi(argv[i + 1])));
        }
        if (i + 2 < argc) {
            options.csvPath = argv[i + 2];
        }
        break;
    }
    return options;
}

/// Writes a recorder's per-frame CSV, reporting rather than throwing on failure.
void writeCsv(const std::string& path, const rm::bench::FrameRecorder& recorder) {
    if (path.empty()) {
        return;
    }
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::fprintf(stderr, "  could not write %s\n", path.c_str());
        return;
    }
    out << recorder.toCsv();
    std::printf("  wrote %s (%zu frames)\n", path.c_str(), recorder.recorded());
}

} // namespace rm::app
