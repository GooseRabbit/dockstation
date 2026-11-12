#include "platform/logging/logger.hpp"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace dockstation::platform::logging {
namespace {

struct LoggerState {
    LoggerConfig config{};
    std::ofstream file{};
    bool initialized{false};
    std::mutex mutex{};
};

LoggerState& state() {
    static LoggerState instance{};
    return instance;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20U) {
                    constexpr char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[(ch >> 4U) & 0x0FU]
                           << digits[ch & 0x0FU];
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

std::string timestamp_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    if (gmtime_r(&now, &value) == nullptr) {
        return "TIME_UNAVAILABLE";
    }
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value) == 0U) {
        return "TIME_UNAVAILABLE";
    }
    return buffer;
}

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool valid_log_level(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
        case LogLevel::Info:
        case LogLevel::Warning:
        case LogLevel::Error:
            return true;
    }
    return false;
}

bool is_sensitive_key(const std::string& key) {
    const std::string normalized = lower_ascii(key);
    const char* const sensitive_parts[] = {
        "password", "passwd", "token", "authorization", "secret",
        "private_key", "private-key", "privatekey", "credential",
        "api_key", "api-key", "apikey"
    };
    for (const char* part : sensitive_parts) {
        if (normalized.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool file_exists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

LoggerResult rotate_locked(LoggerState& logger, bool force) {
    if (logger.config.file_path.empty()) {
        return {LoggerStatus::Available, {}};
    }

    logger.file.flush();
    if (!logger.file.good()) {
        return {LoggerStatus::Unavailable, "cannot flush log file before rotation"};
    }

    struct stat info {};
    if (::stat(logger.config.file_path.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return {LoggerStatus::Unavailable, "configured log file disappeared before rotation"};
        }
        return {LoggerStatus::Unavailable, "cannot inspect log file before rotation"};
    }
    if (!force && static_cast<std::uint64_t>(info.st_size) <
                      logger.config.max_file_size_bytes) {
        return {LoggerStatus::Available, {}};
    }

    logger.file.close();
    if (logger.config.max_rotated_files == 0U) {
        if (::unlink(logger.config.file_path.c_str()) != 0 && errno != ENOENT) {
            logger.file.open(logger.config.file_path, std::ios::out | std::ios::app);
            return {LoggerStatus::Unavailable, "cannot truncate rotated log file"};
        }
    } else {
        const std::string oldest = logger.config.file_path + "." +
            std::to_string(logger.config.max_rotated_files);
        if (::unlink(oldest.c_str()) != 0 && errno != ENOENT) {
            logger.file.open(logger.config.file_path, std::ios::out | std::ios::app);
            return {LoggerStatus::Unavailable, "cannot remove oldest rotated log file"};
        }
        for (std::size_t index = logger.config.max_rotated_files; index > 1U; --index) {
            const std::string from = logger.config.file_path + "." + std::to_string(index - 1U);
            const std::string to = logger.config.file_path + "." + std::to_string(index);
            if (file_exists(from) && ::rename(from.c_str(), to.c_str()) != 0) {
                logger.file.open(logger.config.file_path, std::ios::out | std::ios::app);
                return {LoggerStatus::Unavailable, "cannot shift rotated log files"};
            }
        }
        const std::string first = logger.config.file_path + ".1";
        if (::rename(logger.config.file_path.c_str(), first.c_str()) != 0) {
            logger.file.open(logger.config.file_path, std::ios::out | std::ios::app);
            return {LoggerStatus::Unavailable, "cannot rotate current log file"};
        }
    }

    logger.file.open(logger.config.file_path, std::ios::out | std::ios::app);
    if (!logger.file.is_open()) {
        return {LoggerStatus::Unavailable, "cannot reopen log file after rotation"};
    }
    return {LoggerStatus::Available, {}};
}

std::string serialize_record(LogLevel level,
                             const std::string& component,
                             const std::string& message,
                             const std::vector<LogField>& fields) {
    std::ostringstream output;
    output << "{\"timestamp_utc\":\"" << json_escape(timestamp_utc())
           << "\",\"level\":\"" << level_name(level)
           << "\",\"component\":\"" << json_escape(component)
           << "\",\"message\":\"" << json_escape(message)
           << "\",\"fields\":{";
    const std::vector<LogField> safe_fields = redact_sensitive_fields(fields);
    for (std::size_t index = 0; index < safe_fields.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << "\"" << json_escape(safe_fields[index].key)
               << "\":\"" << json_escape(safe_fields[index].value) << "\"";
    }
    output << "}}\n";
    return output.str();
}

}  // namespace

std::vector<LogField> redact_sensitive_fields(const std::vector<LogField>& fields) {
    std::vector<LogField> result;
    result.reserve(fields.size());
    for (const LogField& field : fields) {
        result.push_back({field.key,
                          is_sensitive_key(field.key) ? "[REDACTED]" : field.value});
    }
    return result;
}

LoggerResult initialize_logger(const LoggerConfig& config) {
    LoggerState& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);

    if (!valid_log_level(config.minimum_level)) {
        return {LoggerStatus::Unavailable, "unknown minimum log level"};
    }
    if (config.file_path.find('\0') != std::string::npos) {
        return {LoggerStatus::Unavailable, "log file path contains a null byte"};
    }
    if (!config.write_to_stderr && config.file_path.empty()) {
        return {LoggerStatus::Unavailable, "at least one log sink must be enabled"};
    }
    if (!config.file_path.empty() && config.max_file_size_bytes == 0U) {
        return {LoggerStatus::Unavailable, "maximum log file size must be greater than zero"};
    }
    if (config.max_rotated_files > 20U) {
        return {LoggerStatus::Unavailable, "maximum rotated file count cannot exceed 20"};
    }

    if (logger.file.is_open()) {
        logger.file.flush();
        if (!logger.file.good()) {
            return {LoggerStatus::Unavailable, "cannot flush previous log file"};
        }
        logger.file.close();
        if (logger.file.fail()) {
            return {LoggerStatus::Unavailable, "cannot close previous log file"};
        }
    }
    logger.config = config;
    logger.initialized = true;

    if (!config.file_path.empty()) {
        logger.file.open(config.file_path, std::ios::out | std::ios::app);
        if (!logger.file.is_open()) {
            logger.initialized = false;
            return {LoggerStatus::Unavailable, "cannot open configured log file"};
        }
    }
    return {LoggerStatus::Available, {}};
}

LoggerResult rotate_log_files() {
    LoggerState& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    if (!logger.initialized) {
        return {LoggerStatus::Unavailable, "logger is not initialized"};
    }
    return rotate_locked(logger, false);
}

LoggerResult write_log_record(LogLevel level,
                              const std::string& component,
                              const std::string& message,
                              const std::vector<LogField>& fields) {
    LoggerState& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    if (!logger.initialized) {
        return {LoggerStatus::Unavailable, "logger is not initialized"};
    }
    if (!valid_log_level(level)) {
        return {LoggerStatus::Unavailable, "unknown log record level"};
    }
    if (static_cast<unsigned int>(level) <
        static_cast<unsigned int>(logger.config.minimum_level)) {
        return {LoggerStatus::Available, {}};
    }

    const std::string record = serialize_record(level, component, message, fields);
    if (!logger.config.file_path.empty()) {
        struct stat info {};
        if (::stat(logger.config.file_path.c_str(), &info) != 0) {
            return {LoggerStatus::Unavailable,
                    "cannot inspect configured log file before writing"};
        }
        if (info.st_size < 0) {
            return {LoggerStatus::Unavailable, "configured log file reports a negative size"};
        }
        const std::uint64_t current_size = static_cast<std::uint64_t>(info.st_size);
        const std::uint64_t record_size = static_cast<std::uint64_t>(record.size());
        const bool exceeds_limit =
            current_size > logger.config.max_file_size_bytes ||
            record_size > logger.config.max_file_size_bytes - current_size;
        if (exceeds_limit) {
            const LoggerResult rotation = rotate_locked(logger, true);
            if (rotation.status != LoggerStatus::Available) {
                return rotation;
            }
        }
        logger.file << record;
        if (!logger.file.good()) {
            return {LoggerStatus::Unavailable, "cannot write log file"};
        }
    }
    if (logger.config.write_to_stderr) {
        const std::size_t written = std::fwrite(record.data(), 1U, record.size(), stderr);
        if (written != record.size()) {
            return {LoggerStatus::Unavailable, "cannot write log record to stderr"};
        }
    }
    return {LoggerStatus::Available, {}};
}

LoggerResult flush_log() {
    LoggerState& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    if (!logger.initialized) {
        return {LoggerStatus::Unavailable, "logger is not initialized"};
    }
    if (logger.file.is_open()) {
        logger.file.flush();
        if (!logger.file.good()) {
            return {LoggerStatus::Unavailable, "cannot flush log file"};
        }
    }
    if (logger.config.write_to_stderr && std::fflush(stderr) != 0) {
        return {LoggerStatus::Unavailable, "cannot flush stderr"};
    }
    return {LoggerStatus::Available, {}};
}

LoggerResult shutdown_logger() {
    LoggerState& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    if (!logger.initialized) {
        return {LoggerStatus::Available, {}};
    }
    if (logger.file.is_open()) {
        logger.file.flush();
        if (!logger.file.good()) {
            return {LoggerStatus::Unavailable, "cannot flush log file during shutdown"};
        }
        logger.file.close();
        if (logger.file.fail()) {
            return {LoggerStatus::Unavailable, "cannot close log file during shutdown"};
        }
    }
    logger.initialized = false;
    return {LoggerStatus::Available, {}};
}

}  // namespace dockstation::platform::logging
