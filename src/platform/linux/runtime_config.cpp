#include "platform/linux/runtime_config.hpp"

#include <cstddef>
#include <cstdint>

namespace dockstation::platform::linux_runtime {
namespace {

RuntimeConfigValidationResult invalid(const char* field, const char* reason) {
    return {RuntimeConfigValidationStatus::Invalid, field, reason};
}

bool valid_log_level(logging::LogLevel level) {
    switch (level) {
        case logging::LogLevel::Debug:
        case logging::LogLevel::Info:
        case logging::LogLevel::Warning:
        case logging::LogLevel::Error:
            return true;
    }
    return false;
}

bool contains_null_byte(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

}  // namespace

RuntimeConfigValidationResult validate_runtime_config(const RuntimeConfig& config) {
    if (config.api_port == 0U || config.api_port > 65535U) {
        return invalid("api_port", "API port must be between 1 and 65535");
    }

    const logging::LoggerConfig& logger = config.logger;
    if (!valid_log_level(logger.minimum_level)) {
        return invalid("logger.minimum_level", "unknown log level");
    }
    if (!logger.write_to_stderr && logger.file_path.empty()) {
        return invalid("logger", "at least one log sink must be enabled");
    }
    if (contains_null_byte(logger.file_path)) {
        return invalid("logger.file_path", "log file path contains a null byte");
    }
    if (!logger.file_path.empty() && logger.max_file_size_bytes == 0U) {
        return invalid("logger.max_file_size_bytes",
                       "maximum log file size must be greater than zero");
    }
    if (logger.max_rotated_files > 20U) {
        return invalid("logger.max_rotated_files",
                       "maximum rotated file count cannot exceed 20");
    }

    return {RuntimeConfigValidationStatus::Valid, {}, {}};
}

}  // namespace dockstation::platform::linux_runtime
