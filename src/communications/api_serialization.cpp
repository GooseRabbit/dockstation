#include "communications/api_serialization.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <string>

#include "platform/linux/network_diagnostics.hpp"
#include "platform/linux/platform_capabilities.hpp"
#include "platform/linux/runtime_environment.hpp"
#include "platform/linux/system_diagnostics.hpp"

namespace dockstation::communications {
namespace {

using platform::linux_diagnostics::AddressFamily;
using platform::linux_diagnostics::DiagnosticValueStatus;
using platform::linux_diagnostics::NetworkDiagnosticStatus;

const char* diagnostic_status(DiagnosticValueStatus status) {
    return status == DiagnosticValueStatus::Available ? "PASS" : "UNAVAILABLE";
}

const char* network_status(NetworkDiagnosticStatus status) {
    return status == NetworkDiagnosticStatus::Available ? "PASS" : "UNAVAILABLE";
}

std::string optional_string(bool available, const std::string& value) {
    return available ? "\"" + json_escape(value) + "\"" : "null";
}

std::string optional_number(bool available,
                            double value,
                            bool allow_negative = false) {
    if (!available || !std::isfinite(value) || (!allow_negative && value < 0.0)) {
        return "null";
    }
    std::ostringstream output;
    output << std::setprecision(9) << value;
    return output.str();
}

std::string optional_integer(bool available, std::uint64_t value) {
    return available ? std::to_string(value) : "null";
}

const char* address_family_name(AddressFamily family) {
    switch (family) {
        case AddressFamily::IPv4: return "IPv4";
        case AddressFamily::IPv6: return "IPv6";
        case AddressFamily::Unspecified: return "UNSPECIFIED";
    }
    return "UNSPECIFIED";
}

std::optional<std::string> utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    std::tm value{};
    if (::gmtime_r(&now, &value) == nullptr) {
        return std::nullopt;
    }
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value) == 0U) {
        return std::nullopt;
    }
    return std::string{buffer};
}

void append_status_message(std::ostringstream& output,
                           const char* status,
                           const std::string& message) {
    output << "\"status\":\"" << status
           << "\",\"message\":\"" << json_escape(message) << "\"";
}

std::string serialize_interface_capability(
    const platform::linux_runtime::PlatformInterfaceCapability& capability) {
    const std::string candidate_path = capability.first_device_path[0] == '\0'
        ? std::string{} : std::string{capability.first_device_path.data()};
    std::ostringstream output;
    output << "{\"status\":\""
           << platform::linux_runtime::platform_capability_status_name(capability.status)
           << "\",\"device_nodes\":" << capability.device_node_count
           << ",\"accessible_device_nodes\":" << capability.accessible_device_count
           << ",\"sysfs_controllers\":" << capability.sysfs_controller_count
           << ",\"kernel_console_conflicts\":" << capability.kernel_console_conflict_count
           << ",\"candidate_device_path\":"
           << optional_string(!candidate_path.empty(), candidate_path)
           << ",\"candidate_only\":true}";
    return output.str();
}

std::string serialize_platform_capabilities(
    const platform::linux_runtime::LinuxPlatformCapabilities& capabilities) {
    const std::string model = capabilities.device_tree_model_available
        ? std::string{capabilities.device_tree_model.data()} : std::string{};
    const std::string compatible = capabilities.device_tree_compatible_available
        ? std::string{capabilities.device_tree_compatible.data()} : std::string{};
    std::ostringstream output;
    output << "{\"profile\":\""
           << platform::linux_runtime::platform_kind_name(capabilities.platform_kind)
           << "\",\"device_tree\":{\"model\":"
           << optional_string(capabilities.device_tree_model_available, model)
           << ",\"compatible\":"
           << optional_string(capabilities.device_tree_compatible_available, compatible)
           << ",\"model_available\":"
           << (capabilities.device_tree_model_available ? "true" : "false")
           << ",\"compatible_available\":"
           << (capabilities.device_tree_compatible_available ? "true" : "false")
           << ",\"kernel_cmdline_available\":"
           << (capabilities.kernel_cmdline_available ? "true" : "false")
           << "},\"interfaces\":{"
           << "\"uart\":" << serialize_interface_capability(capabilities.uart)
           << ",\"gpio\":" << serialize_interface_capability(capabilities.gpio)
           << ",\"i2c\":" << serialize_interface_capability(capabilities.i2c)
           << ",\"spi\":" << serialize_interface_capability(capabilities.spi)
           << ",\"pwm\":" << serialize_interface_capability(capabilities.pwm)
           << ",\"camera\":" << serialize_interface_capability(capabilities.camera)
           << "},\"note\":\"Passive discovery only. Device paths are diagnostic candidates, not selected endpoints; no physical I/O, pinmux, driver or permission changes were performed.\"}";
    return output.str();
}

const char* telemetry_quality_name(
    hardware::interfaces::SensorQualityStatus quality) {
    using hardware::interfaces::SensorQualityStatus;
    switch (quality) {
        case SensorQualityStatus::Ok: return "OK";
        case SensorQualityStatus::Stale: return "STALE";
        case SensorQualityStatus::Invalid: return "INVALID";
        case SensorQualityStatus::Unavailable: return "UNAVAILABLE";
        case SensorQualityStatus::NotConfigured: return "NOT_CONFIGURED";
        case SensorQualityStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string serialize_telemetry_value(
    const hardware::interfaces::ControllerSensorValue& value) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* signed_integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*signed_integer);
    }
    if (const auto* unsigned_integer = std::get_if<std::uint64_t>(&value)) {
        return std::to_string(*unsigned_integer);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        std::ostringstream output;
        output << std::setprecision(17) << *number;
        return output.str();
    }
    if (const auto* text = std::get_if<std::string_view>(&value)) {
        return "\"" + json_escape(std::string{*text}) + "\"";
    }
    return "null";
}

const char* telemetry_value_type(
    const hardware::interfaces::ControllerSensorValue& value) {
    if (std::holds_alternative<bool>(value)) return "BOOLEAN";
    if (std::holds_alternative<std::int64_t>(value)) return "INTEGER";
    if (std::holds_alternative<std::uint64_t>(value)) return "UNSIGNED_INTEGER";
    if (std::holds_alternative<double>(value)) return "NUMBER";
    if (std::holds_alternative<std::string_view>(value)) return "TEXT";
    return "UNKNOWN";
}

std::optional<std::string> telemetry_received_at_utc(std::uint64_t epoch_ms) {
    const std::uint64_t seconds = epoch_ms / 1000U;
    if (seconds > static_cast<std::uint64_t>(
            std::numeric_limits<std::time_t>::max())) {
        return std::nullopt;
    }
    const std::time_t time_value = static_cast<std::time_t>(seconds);
    std::tm value{};
    if (::gmtime_r(&time_value, &value) == nullptr) {
        return std::nullopt;
    }
    char buffer[40]{};
    const std::size_t written = std::strftime(
        buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &value);
    if (written == 0U) {
        return std::nullopt;
    }
    char suffix[8]{};
    const int suffix_size = std::snprintf(
        suffix, sizeof(suffix), ".%03uZ",
        static_cast<unsigned int>(epoch_ms % 1000U));
    if (suffix_size <= 0 ||
        written + static_cast<std::size_t>(suffix_size) >= sizeof(buffer)) {
        return std::nullopt;
    }
    std::copy(suffix, suffix + suffix_size, buffer + written);
    return std::string{buffer};
}

std::string serialize_controller_telemetry(
    const platform::linux_platform::ControllerLinkServiceStatus* link_status,
    const hardware::interfaces::ControllerTelemetrySnapshot* snapshot) {
    const bool connected = link_status != nullptr &&
        link_status->tls_authenticated && link_status->protocol_established;
    const bool available = snapshot != nullptr && snapshot->available();
    const std::size_t count = available ? snapshot->measurement_count() : 0U;
    const char* status = !available
        ? connected ? "UNAVAILABLE" : "NOT_CONFIGURED"
        : count == 0U ? "WARN"
        : connected ? "PASS" : "WARN";
    const char* message = !available
        ? connected
            ? "Controller link is established but no telemetry snapshot has been received"
            : "No accepted telemetry snapshot has been received"
        : count == 0U
            ? "The accepted telemetry snapshot contains no measurements"
            : connected
                ? "Latest telemetry snapshot accepted from the authenticated controller"
                : "Last accepted telemetry snapshot retained; controller link is not currently established";

    std::ostringstream output;
    output << "{\"status\":\"" << status
           << "\",\"message\":\"" << message
           << "\",\"link_connected\":" << (connected ? "true" : "false")
           << ",\"source_controller_id\":"
           << optional_string(available, available ? std::string{snapshot->controller_id()} : std::string{})
           << ",\"controller_instance_id\":"
           << optional_string(available, available ? std::string{snapshot->controller_instance_id()} : std::string{})
           << ",\"sequence_number\":"
           << optional_integer(available, available ? snapshot->sequence_number() : 0U)
           << ",\"received_at_utc\":";
    const auto received_at = available
        ? telemetry_received_at_utc(snapshot->received_at_unix_ms())
        : std::nullopt;
    output << (received_at.has_value()
        ? "\"" + *received_at + "\"" : "null")
           << ",\"measurements\":[";
    if (available) {
        for (std::size_t index = 0U; index < count; ++index) {
            hardware::interfaces::ControllerTelemetryMeasurementView item{};
            if (!snapshot->measurement(index, item)) {
                continue;
            }
            if (index != 0U) {
                output << ',';
            }
            output << "{\"sensor_id\":\"" << json_escape(std::string{item.sensor_id})
                   << "\",\"value_type\":\"" << telemetry_value_type(item.value)
                   << "\",\"value\":" << serialize_telemetry_value(item.value)
                   << ",\"unit\":\"" << json_escape(std::string{item.unit})
                   << "\",\"quality_status\":\"" << telemetry_quality_name(item.quality)
                   << "\",\"measured_at\":\"" << json_escape(std::string{item.measured_at})
                   << "\"}";
        }
    }
    output << "]}";
    return output.str();
}

std::string serialize_platform(
    const platform::linux_runtime::RuntimeValue& os,
    const platform::linux_runtime::RuntimeValue& kernel,
    const platform::linux_runtime::RuntimeValue& architecture,
    const platform::linux_diagnostics::SystemDiagnosticsSnapshot& system,
    const platform::linux_runtime::LinuxPlatformCapabilities& capabilities) {
    const bool os_available =
        os.status == platform::linux_runtime::RuntimeValueStatus::Available;
    const bool kernel_available =
        kernel.status == platform::linux_runtime::RuntimeValueStatus::Available;
    const bool architecture_available =
        architecture.status == platform::linux_runtime::RuntimeValueStatus::Available;

    std::ostringstream output;
    output << "{\"os\":{";
    append_status_message(output, os_available ? "PASS" : "UNAVAILABLE",
                          os_available ? "Operating system information read" : os.reason);
    output << ",\"name\":" << optional_string(os_available, os.value)
           << ",\"version\":null,\"identifier\":null}"
           << ",\"kernel\":" << optional_string(kernel_available, kernel.value)
           << ",\"architecture\":"
           << optional_string(architecture_available, architecture.value)
           << ",\"word_size_bits\":" << (sizeof(void*) * 8U)
           << ",\"uptime\":{";
    append_status_message(output, diagnostic_status(system.uptime.status),
                          system.uptime.status == DiagnosticValueStatus::Available
                              ? "Uptime read from /proc/uptime"
                              : system.uptime.reason);
    output << ",\"seconds\":"
           << optional_number(system.uptime.status == DiagnosticValueStatus::Available,
                              system.uptime.seconds)
           << "},\"resources\":{";

    output << "\"memory\":{";
    append_status_message(output, diagnostic_status(system.memory.status),
                          system.memory.status == DiagnosticValueStatus::Available
                              ? "Read from /proc/meminfo"
                              : system.memory.reason);
    output << ",\"total_bytes\":"
           << optional_integer(system.memory.status == DiagnosticValueStatus::Available,
                               system.memory.total_bytes)
           << ",\"available_bytes\":"
           << optional_integer(system.memory.status == DiagnosticValueStatus::Available,
                               system.memory.available_bytes)
           << ",\"used_bytes\":"
           << optional_integer(system.memory.status == DiagnosticValueStatus::Available,
                               system.memory.used_bytes)
           << ",\"used_percent\":"
           << optional_number(system.memory.status == DiagnosticValueStatus::Available,
                              system.memory.used_percent)
           << "},\"storage\":{";
    append_status_message(output, diagnostic_status(system.storage.status),
                          system.storage.status == DiagnosticValueStatus::Available
                              ? "Read root filesystem capacity"
                              : system.storage.reason);
    output << ",\"path\":\"" << json_escape(system.storage.path)
           << "\",\"total_bytes\":"
           << optional_integer(system.storage.status == DiagnosticValueStatus::Available,
                               system.storage.total_bytes)
           << ",\"free_bytes\":"
           << optional_integer(system.storage.status == DiagnosticValueStatus::Available,
                               system.storage.free_bytes)
           << ",\"available_bytes\":"
           << optional_integer(system.storage.status == DiagnosticValueStatus::Available,
                               system.storage.available_bytes)
           << ",\"used_bytes\":"
           << optional_integer(system.storage.status == DiagnosticValueStatus::Available,
                               system.storage.used_bytes)
           << ",\"used_percent\":"
           << optional_number(system.storage.status == DiagnosticValueStatus::Available,
                              system.storage.used_percent)
           << "},\"cpu_temperature\":{";
    append_status_message(output, diagnostic_status(system.cpu_temperature.status),
                          system.cpu_temperature.status == DiagnosticValueStatus::Available
                              ? "Explicit CPU/SoC/package thermal zone: " +
                                    system.cpu_temperature.sensor_type
                              : system.cpu_temperature.reason);
    output << ",\"celsius\":"
           << optional_number(
                  system.cpu_temperature.status == DiagnosticValueStatus::Available,
                  system.cpu_temperature.celsius, true)
           << ",\"sensor_type\":"
           << optional_string(
                  system.cpu_temperature.status == DiagnosticValueStatus::Available,
                  system.cpu_temperature.sensor_type)
           << "},\"cpu_load\":{";
    append_status_message(output, diagnostic_status(system.cpu_load.status),
                          system.cpu_load.status == DiagnosticValueStatus::Available
                              ? "Read Linux load average from /proc/loadavg"
                              : system.cpu_load.reason);
    output << ",\"one_minute\":"
           << optional_number(system.cpu_load.status == DiagnosticValueStatus::Available,
                              system.cpu_load.one_minute)
           << ",\"five_minutes\":"
           << optional_number(system.cpu_load.status == DiagnosticValueStatus::Available,
                              system.cpu_load.five_minutes)
           << ",\"fifteen_minutes\":"
           << optional_number(system.cpu_load.status == DiagnosticValueStatus::Available,
                              system.cpu_load.fifteen_minutes)
           << ",\"runnable_tasks\":"
           << optional_integer(system.cpu_load.status == DiagnosticValueStatus::Available,
                               system.cpu_load.runnable_tasks)
           << ",\"total_tasks\":"
           << optional_integer(system.cpu_load.status == DiagnosticValueStatus::Available,
                               system.cpu_load.total_tasks)
           << "}},\"capabilities\":"
           << serialize_platform_capabilities(capabilities) << "}";
    return output.str();
}

std::string serialize_network(
    const platform::linux_diagnostics::NetworkDiagnosticsSnapshot& network) {
    const bool interfaces_available =
        network.interfaces.status == NetworkDiagnosticStatus::Available;
    const bool addresses_available =
        network.addresses.status == NetworkDiagnosticStatus::Available;

    std::ostringstream output;
    output << "{\"interfaces\":{";
    const bool have_addresses = addresses_available && !network.addresses.values.empty();
    const char* interface_status =
        !interfaces_available || !addresses_available ? "UNAVAILABLE"
        : have_addresses ? "PASS" : "WARN";
    const std::string interface_message =
        !interfaces_available ? network.interfaces.reason
        : !addresses_available ? network.addresses.reason
        : have_addresses ? "Network interfaces and assigned addresses read"
                         : "No valid assigned IPv4/IPv6 addresses were returned";
    append_status_message(output, interface_status, interface_message);
    output << ",\"interfaces\":[";
    for (std::size_t index = 0; index < network.interfaces.values.size(); ++index) {
        const auto& interface = network.interfaces.values[index];
        if (index != 0U) {
            output << ',';
        }
        output << "{\"name\":\"" << json_escape(interface.name)
               << "\",\"kind\":\""
               << (interface.is_loopback ? "LOOPBACK" : "OTHER")
               << "\",\"oper_state\":null,\"carrier\":null,\"status\":\""
               << (interface.flags_available ? "PASS" : "UNKNOWN")
               << "\",\"message\":\""
               << (interface.flags_available
                       ? "Interface flags read from the Linux kernel"
                       : "Interface flags are unavailable")
               << "\",\"addresses\":[";
        bool first_address = true;
        if (addresses_available) {
            for (const auto& address : network.addresses.values) {
                if (address.interface_name != interface.name) {
                    continue;
                }
                if (!first_address) {
                    output << ',';
                }
                first_address = false;
                output << "{\"family\":\"" << address_family_name(address.family)
                       << "\",\"address\":\"" << json_escape(address.address)
                       << "\",\"scope\":null}";
            }
        }
        output << "]}";
    }
    output << "]},\"default_routes\":{";

    const bool routes_available =
        network.default_routes.status == NetworkDiagnosticStatus::Available;
    const bool have_routes = routes_available && !network.default_routes.values.empty();
    const char* route_status = !routes_available
        ? network_status(NetworkDiagnosticStatus::Unavailable)
        : have_routes ? "PASS" : "WARN";
    const std::string route_message =
        !routes_available ? network.default_routes.reason
        : have_routes ? "Read default routes from the Linux kernel"
                      : "No IPv4 or IPv6 default route is configured";
    append_status_message(output, route_status, route_message);
    output << ",\"routes\":[";
    for (std::size_t index = 0; index < network.default_routes.values.size(); ++index) {
        const auto& route = network.default_routes.values[index];
        if (index != 0U) {
            output << ',';
        }
        output << "{\"family\":\"" << address_family_name(route.family)
               << "\",\"destination\":\"default\",\"gateway\":"
               << optional_string(!route.gateway.empty(), route.gateway)
               << ",\"interface\":"
               << optional_string(!route.interface_name.empty(), route.interface_name)
               << ",\"metric\":" << route.metric << ",\"protocol\":null}";
    }
    const char* family_status = routes_available ? "UNKNOWN" : "UNAVAILABLE";
    output << "],\"family_status\":{\"IPv4\":\"" << family_status
           << "\",\"IPv6\":\"" << family_status << "\"}},"
           << "\"dns\":{\"status\":\"NOT_CONFIGURED\","
           << "\"message\":\"No DNS probe hostname is configured\","
           << "\"hostname\":null,\"addresses\":[]},"
           << "\"ssh\":{\"status\":\"UNAVAILABLE\","
           << "\"message\":\"SSH service state is not yet collected by the C++ diagnostics API\","
           << "\"units\":[]}}";
    return output.str();
}

}  // namespace

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20U) {
                    output << "\\u00" << hex[(ch >> 4U) & 0x0FU]
                           << hex[ch & 0x0FU];
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

std::string serialize_health_response() {
    return "{\"api_version\":\"1\",\"service\":\"dockstation-gateway\","
           "\"status\":\"PASS\",\"read_only\":true}";
}

std::string serialize_system_diagnostics(
    const platform::linux_platform::ControllerLinkServiceStatus* controller_link_status,
    const platform::linux_runtime::LinuxPlatformCapabilities* platform_capabilities,
    const hardware::interfaces::ControllerTelemetrySnapshot* telemetry_snapshot) {
    const bool hardware_controller_connected =
        controller_link_status != nullptr &&
        controller_link_status->tls_authenticated &&
        controller_link_status->protocol_established;

    const auto timestamp = utc_timestamp();
    if (!timestamp.has_value()) {
        return {};
    }

    const auto os = platform::linux_runtime::read_os_version();
    const auto kernel = platform::linux_runtime::read_kernel_version();
    const auto architecture = platform::linux_runtime::read_hardware_architecture();
    const auto system = platform::linux_diagnostics::collect_system_diagnostics();
    const auto network = platform::linux_diagnostics::collect_network_diagnostics();
    const auto detected_capabilities = platform_capabilities == nullptr
        ? platform::linux_runtime::detect_linux_platform_capabilities()
        : *platform_capabilities;

    const bool base_checks_pass =
        os.status == platform::linux_runtime::RuntimeValueStatus::Available &&
        system.uptime.status == DiagnosticValueStatus::Available &&
        network.interfaces.status == NetworkDiagnosticStatus::Available &&
        network.addresses.status == NetworkDiagnosticStatus::Available &&
        !network.addresses.values.empty() &&
        network.default_routes.status == NetworkDiagnosticStatus::Available &&
        !network.default_routes.values.empty();

    std::ostringstream output;
    output << "{\"schema_version\":\"1.0\",\"source\":\"LINUX_HOST\","
           << "\"collected_at_utc\":\"" << *timestamp << "\","
           << "\"summary\":{\"status\":\"WARN\",\"exit_code\":1,"
           << "\"message\":\""
           << (base_checks_pass
                   ? "Linux host diagnostics read; DNS and SSH service checks are not yet implemented"
                   : "One or more Linux host diagnostics are unavailable; DNS and SSH service checks are not yet implemented")
           << "\"},\"platform\":"
           << serialize_platform(os, kernel, architecture, system,
                                  detected_capabilities)
           << ",\"network\":" << serialize_network(network)
           << ",\"controller_telemetry\":"
           << serialize_controller_telemetry(controller_link_status, telemetry_snapshot)
           << ",\"scope\":{\"read_only\":true,"
           << "\"hardware_controller_connected\":"
           << (hardware_controller_connected ? "true" : "false") << ","
           << "\"physical_sensors_configured\":false,"
           << "\"note\":\"Linux host diagnostics are separate from controller link status; sensor measurements are not yet included.\"}}";
    return output.str();
}

std::string serialize_controller_link_status(
    const platform::linux_platform::ControllerLinkServiceStatus& status) {
    using platform::linux_platform::ControllerLinkServiceState;

    const char* state_name = "NOT_CONFIGURED";
    const char* status_name = "NOT_CONFIGURED";
    switch (status.state) {
        case ControllerLinkServiceState::NotConfigured:
            state_name = "NOT_CONFIGURED";
            status_name = "NOT_CONFIGURED";
            break;
        case ControllerLinkServiceState::TlsHandshake:
            state_name = "TLS_HANDSHAKE";
            status_name = "CONNECTING";
            break;
        case ControllerLinkServiceState::ProtocolHandshake:
            state_name = "PROTOCOL_HANDSHAKE";
            status_name = "CONNECTING";
            break;
        case ControllerLinkServiceState::Established:
            state_name = "ESTABLISHED";
            status_name = status.tls_authenticated && status.protocol_established
                ? "OK" : "ERROR";
            break;
        case ControllerLinkServiceState::LinkLost:
            state_name = "LINK_LOST";
            status_name = "LINK_LOST";
            break;
        case ControllerLinkServiceState::Error:
            state_name = "ERROR";
            status_name = "ERROR";
            break;
        case ControllerLinkServiceState::Stopped:
            state_name = "STOPPED";
            status_name = "NOT_CONFIGURED";
            break;
    }

    std::ostringstream output;
    output << "{\"status\":\"" << status_name
           << "\",\"state\":\"" << state_name
           << "\",\"tls_authenticated\":"
           << (status.tls_authenticated ? "true" : "false")
           << ",\"protocol_established\":"
           << (status.protocol_established ? "true" : "false")
           << ",\"telemetry_snapshots_received\":"
           << status.telemetry_snapshots_received
           << ",\"error_messages_received\":"
           << status.error_messages_received
           << ",\"rejected_frames\":" << status.rejected_frames
           << ",\"error_code\":\""
           << json_escape(status.error_code == nullptr
                              ? std::string{} : std::string{status.error_code})
           << "\"}";
    return output.str();
}

std::string serialize_error_response(const std::string& code,
                                     const std::string& message) {
    return "{\"error\":{\"code\":\"" + json_escape(code) +
           "\",\"message\":\"" + json_escape(message) + "\"}}";
}

}  // namespace dockstation::communications
