#include "core/log/Log.hpp"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>
#include <unistd.h>

namespace rm::log {
namespace {

struct State {
    std::mutex mutex;
    Level level = Level::Info;
    bool stderrEnabled = true;
    std::unique_ptr<std::ofstream> file;
};

[[nodiscard]] State& state() {
    static State instance;
    return instance;
}

[[nodiscard]] bool admitted(Level message, Level threshold) noexcept {
    return threshold != Level::Off && message != Level::Off && message >= threshold;
}

[[nodiscard]] bool equalIgnoringCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z'
                     ? static_cast<char>(character - 'A' + 'a')
                     : character;
        };
        if (lower(left[i]) != lower(right[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                        % 1000;
    const std::time_t wall = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&wall, &utc);

    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec, static_cast<long long>(millis.count()));
    return buffer.data();
}

}  // namespace

std::optional<Level> parseLevel(std::string_view value) noexcept {
    if (equalIgnoringCase(value, "trace")) return Level::Trace;
    if (equalIgnoringCase(value, "debug")) return Level::Debug;
    if (equalIgnoringCase(value, "info")) return Level::Info;
    if (equalIgnoringCase(value, "warn") || equalIgnoringCase(value, "warning")) {
        return Level::Warn;
    }
    if (equalIgnoringCase(value, "error")) return Level::Error;
    if (equalIgnoringCase(value, "off")) return Level::Off;
    return std::nullopt;
}

std::string_view levelName(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Off: return "OFF";
    }
    return "?";
}

std::expected<void, std::string> configure(const Options& options) {
    std::unique_ptr<std::ofstream> file;
    std::string problem;
    if (!options.filePath.empty()) {
        file = std::make_unique<std::ofstream>(options.filePath, std::ios::app);
        if (!*file) {
            problem = "could not open log file '" + options.filePath + "'";
            file.reset();
        }
    }

    State& log = state();
    {
        std::scoped_lock lock{log.mutex};
        log.level = options.level;
        log.stderrEnabled = options.stderrEnabled;
        log.file = std::move(file);
    }
    if (!problem.empty()) {
        return std::unexpected{std::move(problem)};
    }
    return {};
}

bool enabled(Level level) noexcept {
    State& log = state();
    std::scoped_lock lock{log.mutex};
    return admitted(level, log.level);
}

void write(Level level, std::string_view category, std::string_view message) noexcept {
    try {
        State& log = state();
        std::scoped_lock lock{log.mutex};
        if (!admitted(level, log.level)) {
            return;
        }

        std::string record = timestamp();
        record += ' ';
        record += levelName(level);
        record += " [";
        record += category;
        record += "] ";
        record += message;
        record += '\n';

        if (log.stderrEnabled) {
            const bool red = level == Level::Error && ::isatty(::fileno(stderr));
            if (red) std::fputs("\033[31m", stderr);
            std::fwrite(record.data(), 1, record.size(), stderr);
            if (red) std::fputs("\033[0m", stderr);
            std::fflush(stderr);
        }
        if (log.file != nullptr) {
            *log.file << record;
            log.file->flush();
        }
    } catch (...) {
        // Logging must not take down the process it is meant to diagnose.
    }
}

void writef(Level level, std::string_view category, const char* format, ...) noexcept {
    if (format == nullptr || !enabled(level)) {
        return;
    }
    try {
        std::array<char, 512> local{};
        va_list arguments;
        va_start(arguments, format);
        const int required = std::vsnprintf(local.data(), local.size(), format, arguments);
        va_end(arguments);
        if (required < 0) {
            return;
        }
        if (static_cast<std::size_t>(required) < local.size()) {
            write(level, category,
                  std::string_view{local.data(), static_cast<std::size_t>(required)});
            return;
        }

        std::vector<char> expanded(static_cast<std::size_t>(required) + 1);
        va_start(arguments, format);
        std::vsnprintf(expanded.data(), expanded.size(), format, arguments);
        va_end(arguments);
        write(level, category,
              std::string_view{expanded.data(), static_cast<std::size_t>(required)});
    } catch (...) {
        // Keep the same non-throwing contract as `write` even when formatting allocates.
    }
}

}  // namespace rm::log
