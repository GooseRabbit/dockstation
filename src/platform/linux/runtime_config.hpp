#pragma once

#include <cstdint>
#include <string>

#include "platform/logging/logger.hpp"

namespace dockstation::platform::linux_runtime {

// The diagnostic Gateway is deliberately restricted to loopback in this stage.
inline constexpr char kGatewayBindAddress[] = "127.0.0.1";
inline constexpr std::uint32_t kDefaultGatewayPort = 8080U;

struct RuntimeConfig {
    std::uint32_t api_port{kDefaultGatewayPort};
    logging::LoggerConfig logger{};
};

enum class RuntimeConfigValidationStatus : std::uint8_t {
    Valid,
    Invalid
};

struct RuntimeConfigValidationResult {
    RuntimeConfigValidationStatus status{RuntimeConfigValidationStatus::Invalid};
    std::string field{};
    std::string reason{};
};

// Validates process settings only. It does not read files, open sockets or start services.
[[nodiscard]] RuntimeConfigValidationResult validate_runtime_config(
    const RuntimeConfig& config);

}  // namespace dockstation::platform::linux_runtime
