#include "communications/api_serialization.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

#include "platform/linux/network_diagnostics.hpp"
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

std::string serialize_platform(
    const platform::linux_runtime::RuntimeValue& os,
    const platform::linux_runtime::RuntimeValue& kernel,
    const platform::linux_runtime::RuntimeValue& architecture,
    const platform::linux_diagnostics::SystemDiagnosticsSnapshot& system) {
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
           << "}}}";
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

std::string serialize_system_diagnostics() {
    const auto timestamp = utc_timestamp();
    if (!timestamp.has_value()) {
        return {};
    }

    const auto os = platform::linux_runtime::read_os_version();
    const auto kernel = platform::linux_runtime::read_kernel_version();
    const auto architecture = platform::linux_runtime::read_hardware_architecture();
    const auto system = platform::linux_diagnostics::collect_system_diagnostics();
    const auto network = platform::linux_diagnostics::collect_network_diagnostics();

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
           << serialize_platform(os, kernel, architecture, system)
           << ",\"network\":" << serialize_network(network)
           << ",\"scope\":{\"read_only\":true,"
           << "\"hardware_controller_connected\":false,"
           << "\"physical_sensors_configured\":false,"
           << "\"note\":\"C++ Gateway reports Linux host and network state only; ESP32 and physical station telemetry are not included.\"}}";
    return output.str();
}

std::string serialize_error_response(const std::string& code,
                                     const std::string& message) {
    return "{\"error\":{\"code\":\"" + json_escape(code) +
           "\",\"message\":\"" + json_escape(message) + "\"}}";
}

}  // namespace dockstation::communications
