#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dockstation::platform::logging {

enum class LogLevel : std::uint8_t { Debug, Info, Warning, Error };
enum class LoggerStatus : std::uint8_t { Available, Unavailable };

struct LogField {
    std::string key{};
    std::string value{};
};

struct LoggerConfig {
    LogLevel minimum_level{LogLevel::Info};
    std::string file_path{};
    std::uint64_t max_file_size_bytes{1024U * 1024U};
    std::size_t max_rotated_files{3};
    bool write_to_stderr{true};
};

struct LoggerResult {
    LoggerStatus status{LoggerStatus::Unavailable};
    std::string reason{};
};

[[nodiscard]] LoggerResult initialize_logger(const LoggerConfig& config);

// Sensitive fields are replaced before serialization. Do not place secrets in message text.
[[nodiscard]] std::vector<LogField> redact_sensitive_fields(
    const std::vector<LogField>& fields);

[[nodiscard]] LoggerResult write_log_record(
    LogLevel level,
    const std::string& component,
    const std::string& message,
    const std::vector<LogField>& fields = {});

[[nodiscard]] LoggerResult rotate_log_files();
[[nodiscard]] LoggerResult flush_log();
[[nodiscard]] LoggerResult shutdown_logger();

}  // namespace dockstation::platform::logging
