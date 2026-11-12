#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "platform/linux/command_line.hpp"
#include "platform/linux/runtime_config_file.hpp"

using namespace dockstation::platform::linux_runtime;

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const char* name) {
    ++checks;
    if (condition) {
        std::cout << "PASS - " << name << '\n';
    } else {
        std::cerr << "FAIL - " << name << '\n';
        ++failures;
    }
}

CommandLineParseResult parse(const std::vector<std::string>& arguments) {
    std::vector<const char*> values;
    values.reserve(arguments.size());
    for (const std::string& argument : arguments) values.push_back(argument.c_str());
    return parse_command_line(static_cast<int>(values.size()), values.data());
}

std::string create_file(const std::string& contents) {
    char path[] = "/tmp/dockstation_runtime_config_XXXXXX";
    const int descriptor = ::mkstemp(path);
    if (descriptor < 0) return {};
    std::size_t offset = 0U;
    while (offset < contents.size()) {
        const ssize_t count = ::write(descriptor, contents.data() + offset,
                                      contents.size() - offset);
        if (count <= 0) {
            (void)::close(descriptor);
            (void)::unlink(path);
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        (void)::unlink(path);
        return {};
    }
    return path;
}
}  // namespace

int main() {
    auto result = parse_runtime_config_text(
        "# DockStation runtime settings\n"
        "api_port = 8090\n"
        "log_level=debug\n"
        "log_file=/tmp/dock station/app.jsonl\n"
        "log_stderr=false\n"
        "log_max_file_size_bytes=8192\n"
        "log_max_rotated_files=5\n");
    expect(result.status == RuntimeConfigFileStatus::Loaded &&
               result.config.api_port == 8090U &&
               result.config.logger.minimum_level == dockstation::platform::logging::LogLevel::Debug &&
               result.config.logger.file_path == "/tmp/dock station/app.jsonl" &&
               !result.config.logger.write_to_stderr &&
               result.config.logger.max_file_size_bytes == 8192U &&
               result.config.logger.max_rotated_files == 5U,
           "parses all supported settings and preserves spaces inside values");

    result = parse_runtime_config_text("");
    expect(result.status == RuntimeConfigFileStatus::Loaded &&
               result.config.api_port == 8080U,
           "empty file leaves documented defaults unchanged");

    result = parse_runtime_config_text("\xEF\xBB\xBF" "api_port=8081\n");
    expect(result.status == RuntimeConfigFileStatus::Loaded &&
               result.config.api_port == 8081U,
           "accepts UTF-8 BOM at the beginning of the file");

    result = parse_runtime_config_text("api_port=8080\napi_port=8081\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid &&
               result.error_line == 2U && result.field == "api_port",
           "rejects duplicate keys and reports the offending line");

    result = parse_runtime_config_text("unknown_setting=true\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid &&
               result.error_line == 1U,
           "rejects unknown keys");

    result = parse_runtime_config_text("api_port 8080\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects lines without equals separator");

    result = parse_runtime_config_text("=8080\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects empty keys");

    result = parse_runtime_config_text("api_port=\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects empty values");

    result = parse_runtime_config_text("api_port=0\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects API port zero");

    result = parse_runtime_config_text("api_port=65536\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects API port above 65535");

    result = parse_runtime_config_text("api_port=not-a-number\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects non-numeric API port");

    result = parse_runtime_config_text("log_stderr=yes\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "accepts only explicit true/false booleans");

    result = parse_runtime_config_text("log_level=TRACE\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects unknown log level");

    result = parse_runtime_config_text("log_max_rotated_files=21\n");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects more than twenty rotated log files");

    result = parse_runtime_config_text(std::string("api_port=8080\n") + '\0');
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects embedded null bytes");

    result = parse_runtime_config_text(std::string(kMaximumRuntimeConfigFileBytes + 1U, ' '));
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects configuration text above the size limit");

    const std::string path = create_file(
        "api_port=8091\nlog_stderr=false\nlog_file=/tmp/test.jsonl\n");
    expect(!path.empty(), "creates temporary configuration file for file loader");
    if (!path.empty()) {
        result = load_runtime_config_file(path);
        expect(result.status == RuntimeConfigFileStatus::Loaded &&
                   result.config.api_port == 8091U,
               "loads configuration from a regular file");
        (void)::unlink(path.c_str());
    }

    result = load_runtime_config_file(
        "/tmp/dockstation-file-that-does-not-exist-987654.conf");
    expect(result.status == RuntimeConfigFileStatus::NotFound,
           "distinguishes a missing configuration file");

    result = load_runtime_config_file("/tmp");
    expect(result.status == RuntimeConfigFileStatus::ReadError,
           "rejects a directory instead of treating it as a configuration file");

    const std::string oversized_path =
        create_file(std::string(kMaximumRuntimeConfigFileBytes + 1U, 'x'));
    expect(!oversized_path.empty(), "creates an oversized temporary configuration file");
    if (!oversized_path.empty()) {
        result = load_runtime_config_file(oversized_path);
        expect(result.status == RuntimeConfigFileStatus::Invalid,
               "rejects an on-disk configuration file above the size limit");
        (void)::unlink(oversized_path.c_str());
    }

    result = load_runtime_config_file("");
    expect(result.status == RuntimeConfigFileStatus::Invalid,
           "rejects an empty configuration file path");

    auto parsed = parse({"dockstation", "--config-file", "/tmp/runtime.conf",
                         "--api-port", "8100", "--log-level", "warning"});
    expect(parsed.status == CommandLineParseStatus::Success &&
               parsed.config_file_path == "/tmp/runtime.conf" &&
               parsed.overrides.api_port && parsed.overrides.log_level,
           "parses config-file option and records explicit CLI overrides");

    RuntimeConfig merged{};
    merged.api_port = 8091U;
    merged.logger.write_to_stderr = false;
    merged.logger.file_path = "/tmp/from-file.jsonl";
    apply_command_line_overrides(parsed, merged);
    expect(merged.api_port == 8100U &&
               merged.logger.minimum_level == dockstation::platform::logging::LogLevel::Warning &&
               !merged.logger.write_to_stderr &&
               merged.logger.file_path == "/tmp/from-file.jsonl",
           "command-line values override only explicitly specified file settings");

    parsed = parse({"dockstation", "--config-file", "/tmp/runtime.conf",
                    "--no-log-stderr"});
    expect(parsed.status == CommandLineParseStatus::Success,
           "defers cross-field validation when a config file will be merged");

    parsed = parse({"dockstation", "--config-file", "--api-port", "8080"});
    expect(parsed.status == CommandLineParseStatus::InvalidArguments,
           "does not consume the next option as a config file path");

    parsed = parse({"dockstation", "--config-file", "/tmp/runtime.conf",
                    "--config-file", "/tmp/other.conf"});
    expect(parsed.status == CommandLineParseStatus::InvalidArguments,
           "rejects repeated config-file options");

    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
