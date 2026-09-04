#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace rm::log {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

struct Options {
    Level level = Level::Info;
    std::string filePath;
    bool stderrEnabled = true;
};

/// Case-insensitive level parsing for command-line and settings boundaries.
[[nodiscard]] std::optional<Level> parseLevel(std::string_view value) noexcept;
[[nodiscard]] std::string_view levelName(Level level) noexcept;

/// Replaces the process-wide logging configuration. File output is appended and flushed after
/// every record so the tail survives a crash. A file-open failure leaves stderr logging usable.
[[nodiscard]] std::expected<void, std::string> configure(const Options& options);

[[nodiscard]] bool enabled(Level level) noexcept;
void write(Level level, std::string_view category, std::string_view message) noexcept;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void writef(Level level, std::string_view category, const char* format, ...) noexcept;

}  // namespace rm::log
