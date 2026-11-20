#pragma once

#include <string>

#include "platform/linux/controller_link_service.hpp"
#include "platform/linux/platform_capabilities.hpp"
#include "hardware/interfaces/controller_telemetry_snapshot.hpp"
#include "platform/linux/prototype_uart_telemetry_service.hpp"

namespace dockstation::communications {

[[nodiscard]] std::string json_escape(const std::string& value);
[[nodiscard]] std::string serialize_health_response();
[[nodiscard]] std::string serialize_system_diagnostics(
    const platform::linux_platform::ControllerLinkServiceStatus* controller_link_status = nullptr,
    const platform::linux_runtime::LinuxPlatformCapabilities* platform_capabilities = nullptr,
    const hardware::interfaces::ControllerTelemetrySnapshot* telemetry_snapshot = nullptr,
    const platform::linux_platform::PrototypeUartTelemetryService* prototype_uart = nullptr);
[[nodiscard]] std::string serialize_controller_link_status(
    const platform::linux_platform::ControllerLinkServiceStatus& status);
[[nodiscard]] std::string serialize_error_response(
    const std::string& code,
    const std::string& message);

}  // namespace dockstation::communications
