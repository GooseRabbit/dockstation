#pragma once

#include <string>

#include "platform/linux/runtime_config.hpp"

namespace dockstation::platform::linux_runtime {

struct RuntimeConfigOverrideMask {
    bool api_port{false};
    bool log_level{false};
    bool log_file{false};
    bool log_stderr{false};
    bool max_file_size_bytes{false};
    bool max_rotated_files{false};
};

enum class CommandLineParseStatus : unsigned char {
    Success,
    HelpRequested,
    InvalidArguments
};

struct CommandLineParseResult {
    CommandLineParseStatus status{CommandLineParseStatus::InvalidArguments};
    RuntimeConfig config{};
    RuntimeConfigOverrideMask overrides{};
    std::string config_file_path{};
    std::string error{};
};

// Parses process arguments only. It does not initialize services or access the filesystem.
[[nodiscard]] CommandLineParseResult parse_command_line(
    int argc,
    const char* const argv[]);

// Applies only options explicitly present on the command line to the supplied base config.
void apply_command_line_overrides(
    const CommandLineParseResult& parsed,
    RuntimeConfig& config);

// Returns help for the options supported by the current RuntimeConfig contract.
[[nodiscard]] std::string command_line_help(const std::string& executable_name);

}  // namespace dockstation::platform::linux_runtime
