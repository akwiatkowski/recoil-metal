#include "app/Cli.hpp"

#include "core/lua/LuaTable.hpp"
#include "core/unit/UnitBlueprint.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace rm::app {


[[nodiscard]] ShotOptions parseShot(int argc, const char* argv[]) {
    ShotOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} != "--screenshot") {
            continue;
        }
        options.enabled = true;
        if (i + 1 < argc) {
            options.path = argv[i + 1];
        }
        if (i + 3 < argc) {
            const int w = std::atoi(argv[i + 2]);
            const int h = std::atoi(argv[i + 3]);
            if (w > 0 && h > 0) {
                options.width = static_cast<unsigned int>(w);
                options.height = static_cast<unsigned int>(h);
            }
        }
        break;
    }
    return options;
}

/// Every `--units <model> [count] [scale]` on the command line, in order.
///
/// Repeatable: one flag per model, which is how a scene with several models is
/// described. `--focus` is a scene-wide flag and is read separately, since it
/// may appear before, between or after the positional arguments.
[[nodiscard]] std::vector<UnitOptions> parseUnits(int argc, const char* argv[]) {
    std::vector<UnitOptions> requests;

    // ONE pass, because `--animate` attaches to the `--units` it follows and two
    // passes can disagree about which that is: a bare `--units` with no path is
    // dropped from the list but would still be counted while attaching, landing
    // the animation on the previous model.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--animate") {
            if (i + 1 < argc && !requests.empty()) {
                requests.back().animationPath = argv[i + 1];
            } else if (requests.empty()) {
                std::fprintf(stderr, "--animate before any --units; ignored\n");
            }
            continue;
        }

        if (arg != "--units") {
            continue;
        }

        UnitOptions options;
        if (i + 1 < argc) {
            options.modelPath = argv[i + 1];
        }
        // Positional arguments stop at the next flag, so `--units a.s3o --units
        // b.s3o` does not read "--units" as a count.
        if (i + 2 < argc && argv[i + 2][0] != '-') {
            const int parsed = std::atoi(argv[i + 2]);
            if (parsed > 0) {
                options.count = static_cast<std::size_t>(parsed);
            }
        }
        if (i + 3 < argc && argv[i + 3][0] != '-') {
            const double parsed = std::atof(argv[i + 3]);
            if (parsed > 0.0) {
                options.scale = static_cast<float>(parsed);
            }
        }

        if (options.modelPath.empty()) {
            std::fprintf(stderr, "--units with no model path; ignored\n");
            continue;
        }
        requests.push_back(std::move(options));
    }

    return requests;
}

/// Builds the asset search path from `--data-dir <dir>`.
///
/// The BAR path, which resolves by filename under real directories because that is
/// how a `.s3o` names its textures. The Supreme Commander path goes through the VFS
/// instead — see parseContent.
[[nodiscard]] rm::vfs::AssetSearch parseAssetSearch(int argc, const char* argv[]) {
    rm::vfs::AssetSearch search;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string{argv[i]} == "--data-dir") {
            search.addRoot(argv[i + 1]);
        }
    }

    return search;
}

/// Mounts the game's content: `--gamedata <dir>` for a whole install, `--archive
/// <scd>` for one archive, `--data-dir <dir>` for a loose directory.
///
/// MOUNT ORDER IS PRIORITY, last wins (core/vfs/Vfs.hpp), and the command line's
/// order is honoured verbatim so a mod can be layered over stock content by naming
/// it second. `--gamedata` mounts every `.scd` it finds in name order, which is what
/// the game does absent a mod list.
///
/// This replaced extracting archives to a temporary directory. That worked, and it
/// is not what a game does: it duplicated 2.4 GiB for the two big archives, went
/// stale whenever the install was patched, and could not express a mod at all, since
/// a mod IS a layer rather than a set of files to merge.
[[nodiscard]] rm::vfs::Vfs parseContent(int argc, const char* argv[]) {
    rm::vfs::Vfs content;
    std::size_t archives = 0;

    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--gamedata") {
            std::vector<std::filesystem::path> found;
            std::error_code ec;
            for (const auto& item :
                 std::filesystem::directory_iterator{std::filesystem::path{argv[i + 1]}, ec}) {
                if (item.path().extension() == ".scd" || item.path().extension() == ".sdz") {
                    found.push_back(item.path());
                }
            }
            std::ranges::sort(found);  // the iterator promises no order; a mount is priority
            for (const std::filesystem::path& archive : found) {
                if (content.mountArchive(archive)) {
                    ++archives;
                } else {
                    std::fprintf(stderr, "failed to mount %s\n", archive.string().c_str());
                }
            }
        } else if (arg == "--archive") {
            if (content.mountArchive(argv[i + 1])) {
                ++archives;
            } else {
                std::fprintf(stderr, "failed to mount archive %s\n", argv[i + 1]);
            }
        } else if (arg == "--data-dir") {
            content.mountDirectory(argv[i + 1]);
        }
    }

    if (!content.empty()) {
        std::printf("content: %zu archives, %zu files\n", archives, content.fileCount());
    }
    return content;
}

/// `--time <seconds>`: where in their animations to freeze the units.
///
/// Only meaningful for a screenshot or a benchmark. The windowed app advances
/// its own clock, because an animation that needs a flag to move is not one
/// anybody would notice working.
[[nodiscard]] float parseAnimationTime(int argc, const char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string{argv[i]} == "--time") {
            return static_cast<float>(std::atof(argv[i + 1]));
        }
    }
    return 0.0f;
}

/// `--dump-weapon <ID>`: print one unit's weapon timings, authored beside corrected.
///
/// §7 P3.5's stated manual check. It exists because the FA duration correction is invisible
/// otherwise: the numbers it changes are inside a blueprint, and the only way to see that a
/// stated 0.2 became a 0.3 is to print both. Reading the Lua table again alongside the parsed
/// `UnitDef` is what makes that possible without keeping an authored copy of every field on
/// `Weapon` for the sake of one diagnostic.
///
/// Prints the tick counts too, at this run's rate, because "0.3 seconds" and "3 ticks" are
/// different claims and only the second is what the sim will do.
[[nodiscard]] bool dumpWeapons(const rm::vfs::Vfs& content, const std::string& id) {
    const std::string path = "/units/" + id + "/" + id + "_unit.bp";
    const std::optional<std::vector<std::byte>> bytes = content.read(path);
    if (!bytes) {
        std::fprintf(stderr, "no blueprint at %s\n", path.c_str());
        return false;
    }
    const std::string_view source{reinterpret_cast<const char*>(bytes->data()), bytes->size()};

    const auto def = rm::unitbp::load(source, path);
    if (!def) {
        std::fprintf(stderr, "%s not read: %s\n", path.c_str(), def.error().message.c_str());
        return false;
    }
    // The same source parsed as a plain table, for the AUTHORED figures the corrected `UnitDef`
    // no longer carries. Two readings of one file rather than two files: they cannot disagree.
    const auto raw = rm::lua::parseTable(source);
    const rm::lua::Value* rawWeapons =
        raw ? raw->path("Weapon") : static_cast<const rm::lua::Value*>(nullptr);

    std::printf("%s — %zu weapon(s), at %u ticks a second\n", def->name.c_str(),
                def->weapons.size(), gAppTickRate.ticksPerSecond());

    for (std::size_t w = 0; w < def->weapons.size(); ++w) {
        const rm::unitdef::Weapon& weapon = def->weapons[w];
        const rm::lua::Value* entry =
            (rawWeapons != nullptr && w < rawWeapons->items.size()) ? &rawWeapons->items[w]
                                                                   : nullptr;
        const double authoredDelay =
            entry != nullptr ? entry->numberAt("MuzzleSalvoDelay").value_or(0.0) : 0.0;

        std::printf("  [%zu] %-24s %s%s\n", w,
                    weapon.label.empty() ? "(no label)" : weapon.label.c_str(),
                    weapon.fires() ? "fires" : "not fired",
                    weapon.role == rm::unitdef::WeaponRole::Death ? " (death explosion)" : "");
        // `RateOfFire` is deliberately shown WITHOUT a correction — it is engine-timed
        // (`11 §3.6`), and this line is where someone would otherwise assume otherwise.
        std::printf("        RateOfFire %.4g/s -> reload %d ticks (engine-timed, uncorrected)\n",
                    static_cast<double>(weapon.rateOfFire), weapon.reloadTicks(gAppTickRate));
        if (weapon.burstSize > 1 || authoredDelay > 0.0) {
            std::printf("        MuzzleSalvoSize %d, MuzzleSalvoDelay %.4g s authored"
                        " -> %.4g s corrected -> %llu ticks%s\n",
                        weapon.burstSize, authoredDelay,
                        static_cast<double>(weapon.burstDelay.value),
                        static_cast<unsigned long long>(
                            gAppTickRate.ticks(weapon.burstDelay)),
                        weapon.bursts() ? "" : " (not a burst)");
        }
    }
    return true;
}


[[nodiscard]] MarchOptions parseMarch(int argc, const char* argv[]) {
    MarchOptions options;
    for (int i = 2; i + 3 < argc; ++i) {
        if (std::string{argv[i]} != "--march") {
            continue;
        }
        options.enabled = true;
        options.x = static_cast<float>(std::atof(argv[i + 1]));
        options.z = static_cast<float>(std::atof(argv[i + 2]));
        options.seconds = static_cast<float>(std::atof(argv[i + 3]));
        return options;
    }
    // `--play SECONDS`: the same pre-run sim, minus the blanket move order.
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string{argv[i]} != "--play") {
            continue;
        }
        options.enabled = true;
        options.orderAll = false;
        options.seconds = static_cast<float>(std::atof(argv[i + 1]));
        break;
    }

    // Both take a path and are independent of which pre-run mode is in use, so they are
    // parsed after it rather than inside either branch.
    for (int i = 2; i + 1 < argc; ++i) {
        const std::string flag{argv[i]};
        if (flag == "--hash-log") {
            options.hashLogPath = argv[i + 1];
        } else if (flag == "--check-hash-log") {
            options.checkHashLogPath = argv[i + 1];
        } else if (flag == "--command-log") {
            options.commandLogPath = argv[i + 1];
        }
    }
    return options;
}

/// The unsigned count following a flag, or 0 when the flag is absent or its
/// argument is not a number.
[[nodiscard]] std::size_t parseCount(int argc, const char* argv[], std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != flag) {
            continue;
        }
        char* end = nullptr;
        const unsigned long value = std::strtoul(argv[i + 1], &end, 10);
        if (end != argv[i + 1] && *end == '\0') {
            return static_cast<std::size_t>(value);
        }
    }
    return 0;
}

/// Whether a bare flag appears anywhere in the arguments.
[[nodiscard]] bool hasFlag(int argc, const char* argv[], std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) {
            return true;
        }
    }
    return false;
}

} // namespace rm::app
