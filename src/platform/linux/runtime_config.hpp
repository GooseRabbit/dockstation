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
    // Leave all three values empty until the paired ESP32 and UART device are configured.
    std::string controller_uart_device{};
    // Temporary plaintext DHT22 prototype channel; independent of secure ControllerLink.
    std::string prototype_uart_device{};
    std::string controller_local_id{};
    std::string controller_peer_id{};
    // Optional authenticated subsystem-event binding; Station ID is not
    // controller_local_id. The source must have a matching local DB registration.
    std::string station_id{};
    std::string controller_source{};
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

enum class RuntimeLoggerInitializationStatus : std::uint8_t {
    Ready,
    Rejected,
    Unavailable
};

struct RuntimeLoggerInitializationResult {
    RuntimeLoggerInitializationStatus status{RuntimeLoggerInitializationStatus::Unavailable};
    std::string field{};
    std::string reason{};
};

// Validates the complete runtime configuration before initializing PlatformLogger.
[[nodiscard]] RuntimeLoggerInitializationResult initialize_runtime_logger(
    const RuntimeConfig& config);

}  // namespace dockstation::platform::linux_runtime
