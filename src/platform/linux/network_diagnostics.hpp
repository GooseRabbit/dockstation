#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace dockstation::platform::linux_diagnostics {

enum class NetworkDiagnosticStatus : std::uint8_t {
    Available,
    Unavailable
};

enum class AddressFamily : std::uint8_t {
    Unspecified,
    IPv4,
    IPv6
};

template <typename Value>
struct NetworkDiagnosticList {
    NetworkDiagnosticStatus status{NetworkDiagnosticStatus::Unavailable};
    std::vector<Value> values{};
    std::string reason{};
};

struct NetworkInterfaceInfo {
    std::string name{};
    std::uint32_t index{0};
    std::uint32_t flags{0};
    bool is_up{false};
    bool is_running{false};
    bool is_loopback{false};
    bool flags_available{false};
    bool mtu_available{false};
    std::uint32_t mtu{0};
};

struct InterfaceAddressInfo {
    std::string interface_name{};
    AddressFamily family{AddressFamily::Unspecified};
    std::string address{};
    std::uint8_t prefix_length{0};
    bool prefix_length_available{false};
};

struct DefaultRouteInfo {
    AddressFamily family{AddressFamily::Unspecified};
    std::string interface_name{};
    std::string gateway{};
    std::uint32_t metric{0};
};

struct DnsResolutionDiagnostic {
    NetworkDiagnosticStatus status{NetworkDiagnosticStatus::Unavailable};
    std::string hostname{};
    AddressFamily requested_family{AddressFamily::Unspecified};
    std::vector<std::string> addresses{};
    std::string reason{};
};

// AVAILABLE means the probe ran; reachable=false is a completed negative result.
// UNAVAILABLE means the probe could not be performed (invalid input/name resolution).
struct TcpReachabilityDiagnostic {
    NetworkDiagnosticStatus status{NetworkDiagnosticStatus::Unavailable};
    std::string host{};
    std::uint16_t port{0};
    bool reachable{false};
    std::uint64_t elapsed_milliseconds{0};
    std::string reason{};
};

struct NetworkDiagnosticsSnapshot {
    NetworkDiagnosticList<NetworkInterfaceInfo> interfaces{};
    NetworkDiagnosticList<InterfaceAddressInfo> addresses{};
    NetworkDiagnosticList<DefaultRouteInfo> default_routes{};
};

// Lists network interfaces and their administrative/operational flags.
// The function does not enable, disable, or reconfigure any interface.
[[nodiscard]] NetworkDiagnosticList<NetworkInterfaceInfo> read_network_interfaces();

// Lists IPv4 and IPv6 addresses assigned to local interfaces.
[[nodiscard]] NetworkDiagnosticList<InterfaceAddressInfo> read_interface_addresses();

// Reads IPv4 and IPv6 default routes from the Linux kernel route tables.
// An empty successful list means that no default route is currently configured.
[[nodiscard]] NetworkDiagnosticList<DefaultRouteInfo> read_default_route();

// Resolves a caller-supplied hostname using the system resolver (NSS/getaddrinfo).
// DNS/NSS resolution is synchronous and is not bounded by a timeout in this API.
[[nodiscard]] DnsResolutionDiagnostic resolve_dns_name(
    const std::string& hostname,
    AddressFamily family = AddressFamily::Unspecified);

// Checks TCP connectivity to a caller-supplied host and port.
// timeout must be between 1 ms and 60 s and bounds TCP connection attempts
// after synchronous name resolution.
[[nodiscard]] TcpReachabilityDiagnostic check_tcp_reachability(
    const std::string& host,
    std::uint16_t port,
    std::chrono::milliseconds timeout);

// Collects local interface, address and default-route information only.
// DNS and TCP probes are explicit operations and are not triggered automatically.
[[nodiscard]] NetworkDiagnosticsSnapshot collect_network_diagnostics();

}  // namespace dockstation::platform::linux_diagnostics
