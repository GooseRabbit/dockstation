#pragma once

#include <string>

#include "platform/linux/runtime_config.hpp"

namespace dockstation::platform::linux_runtime {

enum class CommandLineParseStatus : unsigned char {
    Success,
    HelpRequested,
    InvalidArguments
};

struct CommandLineParseResult {
    CommandLineParseStatus status{CommandLineParseStatus::InvalidArguments};
    RuntimeConfig config{};
    std::string error{};
};

// Parses process arguments only. It does not initialize services or access the filesystem.
[[nodiscard]] CommandLineParseResult parse_command_line(
    int argc,
    const char* const argv[]);

// Returns help for the options supported by the current RuntimeConfig contract.
[[nodiscard]] std::string command_line_help(const std::string& executable_name);

}  // namespace dockstation::platform::linux_runtime
