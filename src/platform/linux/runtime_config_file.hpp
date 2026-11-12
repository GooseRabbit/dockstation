#pragma once

#include <cstddef>
#include <string>

#include "platform/linux/runtime_config.hpp"

namespace dockstation::platform::linux_runtime {

inline constexpr std::size_t kMaximumRuntimeConfigFileBytes = 65536U;

enum class RuntimeConfigFileStatus : unsigned char {
    Loaded,
    NotFound,
    ReadError,
    Invalid
};

struct RuntimeConfigFileResult {
    RuntimeConfigFileStatus status{RuntimeConfigFileStatus::ReadError};
    RuntimeConfig config{};
    std::size_t error_line{0U};
    std::string field{};
    std::string reason{};
};

// Parses the strict key=value format. Values overlay RuntimeConfig defaults.
// Cross-field validation is performed after command-line overrides are applied.
[[nodiscard]] RuntimeConfigFileResult parse_runtime_config_text(
    const std::string& contents);

// Reads at most kMaximumRuntimeConfigFileBytes from a regular file.
[[nodiscard]] RuntimeConfigFileResult load_runtime_config_file(
    const std::string& path);

}  // namespace dockstation::platform::linux_runtime
