#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/linux/network_diagnostics.hpp"

using namespace dockstation::platform::linux_diagnostics;

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

template <typename Diagnostic>
void expect_status_consistent(const Diagnostic& diagnostic, const char* name) {
    expect((diagnostic.status == NetworkDiagnosticStatus::Available &&
            diagnostic.reason.empty()) ||
               (diagnostic.status == NetworkDiagnosticStatus::Unavailable &&
                !diagnostic.reason.empty()),
           name);
}

int create_local_listener(std::uint16_t& port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return -1;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(descriptor, 1) != 0) {
        ::close(descriptor);
        return -1;
    }

    socklen_t length = sizeof(address);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(descriptor);
        return -1;
    }
    port = ntohs(address.sin_port);
    return descriptor;
}

}  // namespace

int main() {
    const auto interfaces = read_network_interfaces();
    expect_status_consistent(interfaces, "interface list status and reason are consistent");
    if (interfaces.status == NetworkDiagnosticStatus::Available) {
        expect(!interfaces.values.empty(), "at least one local network interface is listed");
        bool has_loopback = false;
        for (const auto& item : interfaces.values) {
            expect(!item.name.empty(), "interface name is not empty");
            expect(item.index > 0, "interface index is positive");
            if (item.is_loopback) {
                has_loopback = true;
            }
            if (item.mtu_available) {
                expect(item.mtu > 0, "available interface MTU is positive");
            }
        }
        expect(has_loopback, "loopback interface is present");
    }

    const auto addresses = read_interface_addresses();
    expect_status_consistent(addresses, "interface address status and reason are consistent");
    if (addresses.status == NetworkDiagnosticStatus::Available) {
        bool has_ipv4_loopback = false;
        for (const auto& item : addresses.values) {
            expect(!item.interface_name.empty(), "address has an interface name");
            expect(!item.address.empty(), "interface address is not empty");
            expect(item.family == AddressFamily::IPv4 ||
                       item.family == AddressFamily::IPv6,
                   "interface address has a supported family");
            if (item.family == AddressFamily::IPv4 &&
                item.address == "127.0.0.1") {
                has_ipv4_loopback = true;
            }
            if (item.prefix_length_available) {
                expect(item.prefix_length <=
                           (item.family == AddressFamily::IPv4 ? 32 : 128),
                       "address prefix length is in range");
            }
        }
        expect(has_ipv4_loopback, "IPv4 loopback address is listed");
    }

    const auto routes = read_default_route();
    expect_status_consistent(routes, "default route status and reason are consistent");
    if (routes.status == NetworkDiagnosticStatus::Available) {
        for (const auto& item : routes.values) {
            expect(!item.interface_name.empty(), "default route has an interface name");
            expect(item.family == AddressFamily::IPv4 ||
                       item.family == AddressFamily::IPv6,
                   "default route has a supported address family");
        }
    }

    const auto dns = resolve_dns_name("localhost");
    expect_status_consistent(dns, "DNS status and reason are consistent");
    if (dns.status == NetworkDiagnosticStatus::Available) {
        expect(!dns.addresses.empty(), "localhost resolves to at least one address");
    }

    const auto ipv4_dns = resolve_dns_name("localhost", AddressFamily::IPv4);
    expect_status_consistent(ipv4_dns, "IPv4 DNS status and reason are consistent");
    if (ipv4_dns.status == NetworkDiagnosticStatus::Available) {
        expect(!ipv4_dns.addresses.empty(), "IPv4 localhost resolution returns addresses");
    }

    const auto invalid_dns = resolve_dns_name("");
    expect(invalid_dns.status == NetworkDiagnosticStatus::Unavailable,
           "empty DNS hostname is rejected");
    expect(!invalid_dns.reason.empty(), "empty DNS hostname has an explanation");

    const auto invalid_tcp = check_tcp_reachability(
        "127.0.0.1", 80, std::chrono::milliseconds{0});
    expect(invalid_tcp.status == NetworkDiagnosticStatus::Unavailable,
           "zero TCP timeout is rejected");

    const auto excessive_timeout = check_tcp_reachability(
        "127.0.0.1", 80, std::chrono::seconds{61});
    expect(excessive_timeout.status == NetworkDiagnosticStatus::Unavailable,
           "TCP timeout above 60 seconds is rejected");

    std::uint16_t local_port{0};
    const int listener = create_local_listener(local_port);
    expect(listener >= 0 && local_port > 0,
           "local TCP test listener is created");
    if (listener >= 0 && local_port > 0) {
        const auto reachable = check_tcp_reachability(
            "127.0.0.1", local_port, std::chrono::milliseconds{1500});
        expect(reachable.status == NetworkDiagnosticStatus::Available,
               "successful TCP probe has an available result");
        expect(reachable.reachable, "TCP probe reaches a local listening socket");
        expect(reachable.reason.empty(), "successful TCP probe has no error reason");
        ::close(listener);

        const auto refused = check_tcp_reachability(
            "127.0.0.1", local_port, std::chrono::milliseconds{1500});
        expect(refused.status == NetworkDiagnosticStatus::Available,
               "refused TCP probe is a completed diagnostic");
        expect(!refused.reachable, "TCP probe reports a closed local port as unreachable");
        expect(!refused.reason.empty(), "failed TCP probe explains the failure");
    } else if (listener >= 0) {
        ::close(listener);
    }

    const auto snapshot = collect_network_diagnostics();
    expect(snapshot.interfaces.status == interfaces.status,
           "network snapshot includes interface result");
    expect(snapshot.addresses.status == addresses.status,
           "network snapshot includes address result");
    expect(snapshot.default_routes.status == routes.status,
           "network snapshot includes default route result");

    std::cout << "Interfaces: " << interfaces.values.size() << '\n'
              << "Addresses: " << addresses.values.size() << '\n'
              << "Default routes: " << routes.values.size() << '\n'
              << "localhost addresses: " << dns.addresses.size() << '\n'
              << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
