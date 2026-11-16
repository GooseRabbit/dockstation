#include "platform/linux/runtime_config.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "platform/logging/logger.hpp"

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
    return value.find(static_cast<char>(0)) != std::string::npos;
}

bool valid_controller_uart_path(const std::string& value) {
    if (value.size() < 6U || value.size() > 255U ||
        value.compare(0U, 5U, "/dev/") != 0 || value.back() == '/' ||
        contains_null_byte(value)) {
        return false;
    }

    std::size_t offset = 5U;
    while (offset < value.size()) {
        const std::size_t separator = value.find('/', offset);
        const std::size_t end = separator == std::string::npos
            ? value.size() : separator;
        const std::string_view component{value.data() + offset, end - offset};
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const unsigned char ch : component) {
            const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                               (ch >= 'a' && ch <= 'z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!alpha && !digit && ch != '_' && ch != '-' && ch != '.') {
                return false;
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return true;
}

bool valid_controller_id(const std::string& value) {
    if (value.empty() || value.size() > 128U || contains_null_byte(value)) {
        return false;
    }
    for (const unsigned char ch : value) {
        const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                           (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '_' && ch != '-' && ch != '.' && ch != ':') {
            return false;
        }
    }
    return true;
}

bool valid_controller_source(const std::string& value) {
    if (value.empty() || value.size() > 128U || contains_null_byte(value)) {
        return false;
    }

    // Source names may use slash-separated namespaces, but empty, "." and ".."
    // components are forbidden so the identifier is never treated as a path.
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const std::size_t separator = value.find('/', offset);
        const std::size_t end = separator == std::string::npos
            ? value.size() : separator;
        const std::string_view component{value.data() + offset, end - offset};
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const unsigned char ch : component) {
            const bool alpha = (ch >= 'A' && ch <= 'Z') ||
                               (ch >= 'a' && ch <= 'z');
            const bool digit = ch >= '0' && ch <= '9';
            if (!alpha && !digit && ch != '_' && ch != '-' &&
                ch != '.' && ch != ':') {
                return false;
            }
        }
        if (separator == std::string::npos) {
            return true;
        }
        offset = separator + 1U;
    }
    return false;
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

    const bool controller_link_configured =
        !config.controller_uart_device.empty() ||
        !config.controller_local_id.empty() ||
        !config.controller_peer_id.empty();
    if (controller_link_configured) {
        if (!valid_controller_uart_path(config.controller_uart_device)) {
            return invalid("controller_uart_device",
                           "controller UART path must be a normalized /dev/... path");
        }
        if (!valid_controller_id(config.controller_local_id) ||
            !valid_controller_id(config.controller_peer_id) ||
            config.controller_local_id == config.controller_peer_id) {
            return invalid("controller_link",
                           "controller IDs must be distinct non-empty ASCII identifiers of at most 128 bytes");
        }
    }

    const bool event_ingress_configured =
        !config.station_id.empty() || !config.controller_source.empty();
    if (event_ingress_configured) {
        if (!controller_link_configured) {
            return invalid("controller_event_ingress",
                           "station_id and controller_source require a configured ControllerLink");
        }
        if (!valid_controller_id(config.station_id)) {
            return invalid("station_id",
                           "station ID must be a non-empty ASCII identifier of at most 128 bytes");
        }
        if (!valid_controller_source(config.controller_source)) {
            return invalid("controller_source",
                           "controller source must be a non-empty wire-safe identifier of at most 128 bytes");
        }
    }

    return {RuntimeConfigValidationStatus::Valid, {}, {}};
}

RuntimeLoggerInitializationResult initialize_runtime_logger(const RuntimeConfig& config) {
    const RuntimeConfigValidationResult validation = validate_runtime_config(config);
    if (validation.status != RuntimeConfigValidationStatus::Valid) {
        return {RuntimeLoggerInitializationStatus::Rejected, validation.field, validation.reason};
    }
    const logging::LoggerResult result = logging::initialize_logger(config.logger);
    if (result.status != logging::LoggerStatus::Available) {
        return {RuntimeLoggerInitializationStatus::Unavailable, "logger", result.reason};
    }
    return {RuntimeLoggerInitializationStatus::Ready, {}, {}};
}

}  // namespace dockstation::platform::linux_runtime
