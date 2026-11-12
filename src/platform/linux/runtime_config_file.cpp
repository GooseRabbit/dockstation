#include "platform/linux/runtime_config_file.hpp"

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/logging/logger.hpp"

namespace dockstation::platform::linux_runtime {
namespace {

RuntimeConfigFileResult invalid(std::size_t line,
                                const char* field,
                                const char* reason) {
    RuntimeConfigFileResult result{};
    result.status = RuntimeConfigFileStatus::Invalid;
    result.error_line = line;
    result.field = field;
    result.reason = reason;
    return result;
}

RuntimeConfigFileResult read_error(const char* field, const char* reason) {
    RuntimeConfigFileResult result{};
    result.status = RuntimeConfigFileStatus::ReadError;
    result.field = field;
    result.reason = reason;
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1U);
    }
    return value;
}

bool parse_unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_boolean(std::string_view text, bool& value) {
    if (text == "true") {
        value = true;
        return true;
    }
    if (text == "false") {
        value = false;
        return true;
    }
    return false;
}

bool parse_log_level(std::string_view text, logging::LogLevel& level) {
    if (text == "debug") {
        level = logging::LogLevel::Debug;
    } else if (text == "info") {
        level = logging::LogLevel::Info;
    } else if (text == "warning") {
        level = logging::LogLevel::Warning;
    } else if (text == "error") {
        level = logging::LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

}  // namespace

RuntimeConfigFileResult parse_runtime_config_text(const std::string& contents) {
    if (contents.size() > kMaximumRuntimeConfigFileBytes) {
        return invalid(0U, "file", "configuration file exceeds 65536 bytes");
    }
    if (contents.find('\0') != std::string::npos) {
        return invalid(0U, "file", "configuration file contains a null byte");
    }

    RuntimeConfig config{};
    bool api_port_seen{false};
    bool log_level_seen{false};
    bool log_file_seen{false};
    bool log_stderr_seen{false};
    bool max_file_size_seen{false};
    bool max_rotated_files_seen{false};

    std::size_t line_number{1U};
    std::size_t offset{0U};
    while (offset < contents.size()) {
        const std::size_t newline = contents.find('\n', offset);
        const std::size_t end = newline == std::string::npos ? contents.size() : newline;
        std::string_view line{contents.data() + offset, end - offset};
        line = trim(line);

        if (line_number == 1U && line.size() >= 3U &&
            static_cast<unsigned char>(line[0]) == 0xEFU &&
            static_cast<unsigned char>(line[1]) == 0xBBU &&
            static_cast<unsigned char>(line[2]) == 0xBFU) {
            line.remove_prefix(3U);
            line = trim(line);
        }

        if (!line.empty() && line.front() != '#') {
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos) {
                return invalid(line_number, "line", "expected key=value");
            }

            const std::string_view key = trim(line.substr(0U, separator));
            const std::string_view value = trim(line.substr(separator + 1U));
            if (key.empty()) {
                return invalid(line_number, "key", "configuration key is empty");
            }
            if (value.empty()) {
                return invalid(line_number, "value", "configuration value is empty");
            }

            std::uint64_t number{0U};
            if (key == "api_port") {
                if (api_port_seen) return invalid(line_number, "api_port", "duplicate key");
                api_port_seen = true;
                if (!parse_unsigned(value, number) || number == 0U || number > 65535U) {
                    return invalid(line_number, "api_port", "expected an integer from 1 to 65535");
                }
                config.api_port = static_cast<std::uint32_t>(number);
            } else if (key == "log_level") {
                if (log_level_seen) return invalid(line_number, "log_level", "duplicate key");
                log_level_seen = true;
                if (!parse_log_level(value, config.logger.minimum_level)) {
                    return invalid(line_number, "log_level",
                                   "expected debug, info, warning or error");
                }
            } else if (key == "log_file") {
                if (log_file_seen) return invalid(line_number, "log_file", "duplicate key");
                log_file_seen = true;
                config.logger.file_path.assign(value.data(), value.size());
            } else if (key == "log_stderr") {
                if (log_stderr_seen) return invalid(line_number, "log_stderr", "duplicate key");
                log_stderr_seen = true;
                if (!parse_boolean(value, config.logger.write_to_stderr)) {
                    return invalid(line_number, "log_stderr", "expected true or false");
                }
            } else if (key == "log_max_file_size_bytes") {
                if (max_file_size_seen) {
                    return invalid(line_number, "log_max_file_size_bytes", "duplicate key");
                }
                max_file_size_seen = true;
                if (!parse_unsigned(value, number)) {
                    return invalid(line_number, "log_max_file_size_bytes",
                                   "expected an unsigned decimal integer");
                }
                config.logger.max_file_size_bytes = number;
            } else if (key == "log_max_rotated_files") {
                if (max_rotated_files_seen) {
                    return invalid(line_number, "log_max_rotated_files", "duplicate key");
                }
                max_rotated_files_seen = true;
                if (!parse_unsigned(value, number) ||
                    number > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max()) ||
                    number > 20U) {
                    return invalid(line_number, "log_max_rotated_files",
                                   "expected an integer from 0 to 20");
                }
                config.logger.max_rotated_files = static_cast<std::size_t>(number);
            } else {
                return invalid(line_number, "key", "unknown configuration key");
            }
        }

        if (newline == std::string::npos) {
            offset = contents.size();
        } else {
            offset = newline + 1U;
        }
        ++line_number;
    }

    RuntimeConfigFileResult result{};
    result.status = RuntimeConfigFileStatus::Loaded;
    result.config = config;
    return result;
}

RuntimeConfigFileResult load_runtime_config_file(const std::string& path) {
    if (path.empty()) {
        return invalid(0U, "path", "configuration file path is empty");
    }
    if (path.find('\0') != std::string::npos) {
        return invalid(0U, "path", "configuration file path contains a null byte");
    }

    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            RuntimeConfigFileResult result{};
            result.status = RuntimeConfigFileStatus::NotFound;
            result.field = "path";
            result.reason = "configuration file does not exist";
            return result;
        }
        return read_error("path", std::strerror(errno));
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
        const int error = errno;
        (void)::close(descriptor);
        return read_error("file", std::strerror(error));
    }
    if (!S_ISREG(metadata.st_mode)) {
        (void)::close(descriptor);
        return read_error("file", "configuration path is not a regular file");
    }
    if (metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) >
            static_cast<std::uint64_t>(kMaximumRuntimeConfigFileBytes)) {
        (void)::close(descriptor);
        return invalid(0U, "file", "configuration file exceeds 65536 bytes");
    }

    std::string contents;
    char buffer[4096];
    bool read_failed{false};
    int read_error_number{0};
    for (;;) {
        const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_failed = true;
            read_error_number = errno;
            break;
        }
        const std::size_t chunk_size = static_cast<std::size_t>(count);
        if (contents.size() > kMaximumRuntimeConfigFileBytes - chunk_size) {
            (void)::close(descriptor);
            return invalid(0U, "file", "configuration file exceeds 65536 bytes");
        }
        contents.append(buffer, chunk_size);
    }

    const int close_result = ::close(descriptor);
    if (read_failed) {
        return read_error("file", std::strerror(read_error_number));
    }
    if (close_result != 0) {
        return read_error("file", std::strerror(errno));
    }
    return parse_runtime_config_text(contents);
}

}  // namespace dockstation::platform::linux_runtime
