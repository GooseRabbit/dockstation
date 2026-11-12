#include "platform/linux/network_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <limits>
#include <map>
#include <net/if.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <arpa/inet.h>

namespace dockstation::platform::linux_diagnostics {
namespace {

constexpr const char* kIpv4RoutesPath = "/proc/net/route";
constexpr const char* kIpv6RoutesPath = "/proc/net/ipv6_route";

template <typename Diagnostic>
void mark_unavailable(Diagnostic& diagnostic, std::string reason) {
    diagnostic.status = NetworkDiagnosticStatus::Unavailable;
    diagnostic.reason = std::move(reason);
}

template <typename Diagnostic>
void mark_available(Diagnostic& diagnostic) {
    diagnostic.status = NetworkDiagnosticStatus::Available;
    diagnostic.reason.clear();
}

bool parse_hex_u32(std::string_view text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_hex_bytes(std::string_view text, std::uint8_t* bytes, std::size_t count) {
    if (text.size() != count * 2) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t value{0};
        if (!parse_hex_u32(text.substr(i * 2, 2), value) || value > 0xFFU) {
            return false;
        }
        bytes[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

std::string ipv4_from_proc_route(std::string_view hex) {
    std::uint32_t native_value{0};
    if (!parse_hex_u32(hex, native_value)) {
        return {};
    }

    // /proc/net/route prints IPv4 addresses in host byte order.
    struct in_addr address {};
    static_assert(sizeof(address.s_addr) == sizeof(native_value));
    std::memcpy(&address.s_addr, &native_value, sizeof(native_value));

    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (::inet_ntop(AF_INET, &address, buffer.data(), buffer.size()) == nullptr) {
        return {};
    }
    return buffer.data();
}

std::string ipv6_from_proc_route(std::string_view hex) {
    std::array<std::uint8_t, 16> bytes{};
    if (!parse_hex_bytes(hex, bytes.data(), bytes.size())) {
        return {};
    }

    struct in6_addr address {};
    std::copy(bytes.begin(), bytes.end(), address.s6_addr);

    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (::inet_ntop(AF_INET6, &address, buffer.data(), buffer.size()) == nullptr) {
        return {};
    }
    return buffer.data();
}

bool is_zero_hex(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char ch) { return ch == '0'; });
}

bool prefix_length_from_netmask(const sockaddr* netmask,
                                std::uint8_t& prefix_length) {
    if (netmask == nullptr) {
        return false;
    }

    const std::uint8_t* bytes = nullptr;
    std::size_t count = 0;
    if (netmask->sa_family == AF_INET) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(netmask);
        bytes = reinterpret_cast<const std::uint8_t*>(&address->sin_addr);
        count = sizeof(address->sin_addr);
    } else if (netmask->sa_family == AF_INET6) {
        const auto* address = reinterpret_cast<const sockaddr_in6*>(netmask);
        bytes = reinterpret_cast<const std::uint8_t*>(&address->sin6_addr);
        count = sizeof(address->sin6_addr);
    } else {
        return false;
    }

    std::uint8_t prefix{0};
    bool zero_seen{false};
    for (std::size_t i = 0; i < count; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool set = (bytes[i] & static_cast<std::uint8_t>(1U << bit)) != 0;
            if (set && zero_seen) {
                return false;
            }
            if (set) {
                ++prefix;
            } else {
                zero_seen = true;
            }
        }
    }
    prefix_length = prefix;
    return true;
}

AddressFamily to_address_family(int family) {
    if (family == AF_INET) {
        return AddressFamily::IPv4;
    }
    if (family == AF_INET6) {
        return AddressFamily::IPv6;
    }
    return AddressFamily::Unspecified;
}

std::string address_to_string(const sockaddr* address) {
    if (address == nullptr) {
        return {};
    }

    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const void* source = nullptr;
    int family = address->sa_family;
    if (family == AF_INET) {
        source = &reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
    } else if (family == AF_INET6) {
        source = &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
    } else {
        return {};
    }

    if (::inet_ntop(family, source, buffer.data(), buffer.size()) == nullptr) {
        return {};
    }
    return buffer.data();
}

std::uint64_t elapsed_milliseconds(
    const std::chrono::steady_clock::time_point& start) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return elapsed.count() < 0 ? 0U : static_cast<std::uint64_t>(elapsed.count());
}

std::string socket_error_text(int error) {
    return std::string{std::strerror(error)};
}

}  // namespace

NetworkDiagnosticList<NetworkInterfaceInfo> read_network_interfaces() {
    NetworkDiagnosticList<NetworkInterfaceInfo> result{};
    struct if_nameindex* names = ::if_nameindex();
    if (names == nullptr) {
        mark_unavailable(result, "Не удалось получить список сетевых интерфейсов");
        return result;
    }

    const int descriptor = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    for (const struct if_nameindex* item = names;
         item->if_index != 0 && item->if_name != nullptr;
         ++item) {
        NetworkInterfaceInfo info{};
        info.name = item->if_name;
        info.index = item->if_index;

        if (descriptor >= 0) {
            struct ifreq request {};
            const std::size_t name_length =
                std::min(std::strlen(item->if_name), sizeof(request.ifr_name) - 1);
            std::memcpy(request.ifr_name, item->if_name, name_length);
            if (::ioctl(descriptor, SIOCGIFFLAGS, &request) == 0) {
                info.flags = static_cast<std::uint32_t>(
                    static_cast<unsigned short>(request.ifr_flags));
                info.is_up = (request.ifr_flags & IFF_UP) != 0;
                info.is_running = (request.ifr_flags & IFF_RUNNING) != 0;
                info.is_loopback = (request.ifr_flags & IFF_LOOPBACK) != 0;
                info.flags_available = true;
            }

            struct ifreq mtu_request {};
            const std::size_t mtu_name_length =
                std::min(std::strlen(item->if_name), sizeof(mtu_request.ifr_name) - 1);
            std::memcpy(mtu_request.ifr_name, item->if_name, mtu_name_length);
            if (::ioctl(descriptor, SIOCGIFMTU, &mtu_request) == 0 &&
                mtu_request.ifr_mtu >= 0) {
                info.mtu = static_cast<std::uint32_t>(mtu_request.ifr_mtu);
                info.mtu_available = true;
            }
        }
        result.values.push_back(std::move(info));
    }

    if (descriptor >= 0) {
        ::close(descriptor);
    }
    ::if_freenameindex(names);
    mark_available(result);
    return result;
}

NetworkDiagnosticList<InterfaceAddressInfo> read_interface_addresses() {
    NetworkDiagnosticList<InterfaceAddressInfo> result{};
    struct ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        mark_unavailable(result,
                         "Не удалось получить адреса интерфейсов: " +
                             socket_error_text(errno));
        return result;
    }

    for (const struct ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_name == nullptr || item->ifa_addr == nullptr) {
            continue;
        }
        const int family = item->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }

        InterfaceAddressInfo info{};
        info.interface_name = item->ifa_name;
        info.family = to_address_family(family);
        info.address = address_to_string(item->ifa_addr);
        info.prefix_length_available =
            prefix_length_from_netmask(item->ifa_netmask, info.prefix_length);
        if (!info.address.empty()) {
            result.values.push_back(std::move(info));
        }
    }

    ::freeifaddrs(interfaces);
    mark_available(result);
    return result;
}

NetworkDiagnosticList<DefaultRouteInfo> read_default_route() {
    NetworkDiagnosticList<DefaultRouteInfo> result{};

    std::ifstream ipv4_file{kIpv4RoutesPath};
    const bool ipv4_table_available = ipv4_file.is_open();
    if (ipv4_table_available) {
        std::string line;
        std::getline(ipv4_file, line);  // Header.
        while (std::getline(ipv4_file, line)) {
            std::istringstream row{line};
            std::string interface_name;
            std::string destination;
            std::string gateway;
            std::string flags_text;
            std::string reference_count;
            std::string use_count;
            std::string metric_text;
            std::string mask;
            if (!(row >> interface_name >> destination >> gateway >> flags_text >>
                  reference_count >> use_count >> metric_text >> mask)) {
                continue;
            }

            std::uint32_t flags{0};
            std::uint32_t metric{0};
            if (!parse_hex_u32(flags_text, flags) ||
                !parse_hex_u32(metric_text, metric) ||
                !is_zero_hex(destination) || !is_zero_hex(mask) ||
                (flags & static_cast<std::uint32_t>(RTF_UP)) == 0) {
                continue;
            }

            DefaultRouteInfo route{};
            route.family = AddressFamily::IPv4;
            route.interface_name = std::move(interface_name);
            route.gateway = is_zero_hex(gateway) ? std::string{} :
                                                    ipv4_from_proc_route(gateway);
            route.metric = metric;
            result.values.push_back(std::move(route));
        }
    }

    std::ifstream ipv6_file{kIpv6RoutesPath};
    const bool ipv6_table_available = ipv6_file.is_open();
    if (ipv6_table_available) {
        std::string line;
        while (std::getline(ipv6_file, line)) {
            std::istringstream row{line};
            std::string destination;
            std::string destination_prefix;
            std::string source;
            std::string source_prefix;
            std::string next_hop;
            std::string metric_text;
            std::string reference_count;
            std::string use_count;
            std::string flags_text;
            std::string interface_name;
            if (!(row >> destination >> destination_prefix >> source >> source_prefix >>
                  next_hop >> metric_text >> reference_count >> use_count >>
                  flags_text >> interface_name)) {
                continue;
            }

            std::uint32_t prefix{0};
            std::uint32_t source_prefix_value{0};
            std::uint32_t metric{0};
            std::uint32_t flags{0};
            if (!is_zero_hex(destination) || !is_zero_hex(source) ||
                !parse_hex_u32(destination_prefix, prefix) || prefix != 0 ||
                !parse_hex_u32(source_prefix, source_prefix_value) ||
                source_prefix_value != 0 ||
                !parse_hex_u32(metric_text, metric) ||
                !parse_hex_u32(flags_text, flags) ||
                (flags & static_cast<std::uint32_t>(RTF_UP)) == 0) {
                continue;
            }

            DefaultRouteInfo route{};
            route.family = AddressFamily::IPv6;
            route.interface_name = std::move(interface_name);
            route.gateway = is_zero_hex(next_hop) ? std::string{} :
                                                    ipv6_from_proc_route(next_hop);
            route.metric = metric;
            result.values.push_back(std::move(route));
        }
    }

    if (!ipv4_table_available && !ipv6_table_available) {
        mark_unavailable(result,
                         "Не удалось открыть таблицы маршрутов IPv4 и IPv6");
        return result;
    }

    mark_available(result);
    return result;
}

DnsResolutionDiagnostic resolve_dns_name(const std::string& hostname,
                                          AddressFamily family) {
    DnsResolutionDiagnostic result{};
    result.hostname = hostname;
    result.requested_family = family;
    if (hostname.empty()) {
        mark_unavailable(result, "Имя узла не задано");
        return result;
    }

    struct addrinfo hints {};
    hints.ai_family = family == AddressFamily::IPv4 ? AF_INET :
                      family == AddressFamily::IPv6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    const int status = ::getaddrinfo(hostname.c_str(), nullptr, &hints, &addresses);
    if (status != 0) {
        mark_unavailable(result,
                         "Системный resolver не разрешил имя: " +
                             std::string{::gai_strerror(status)});
        return result;
    }

    for (const struct addrinfo* item = addresses; item != nullptr; item = item->ai_next) {
        const std::string address = address_to_string(item->ai_addr);
        if (!address.empty() &&
            std::find(result.addresses.begin(), result.addresses.end(), address) ==
                result.addresses.end()) {
            result.addresses.push_back(address);
        }
    }
    ::freeaddrinfo(addresses);

    if (result.addresses.empty()) {
        mark_unavailable(result, "Resolver не вернул IPv4/IPv6 адресов");
        return result;
    }
    mark_available(result);
    return result;
}

TcpReachabilityDiagnostic check_tcp_reachability(const std::string& host,
                                                 std::uint16_t port,
                                                 std::chrono::milliseconds timeout) {
    TcpReachabilityDiagnostic result{};
    result.host = host;
    result.port = port;
    const auto start = std::chrono::steady_clock::now();

    if (host.empty() || port == 0 || timeout.count() <= 0 ||
        timeout > std::chrono::seconds{60}) {
        mark_unavailable(result,
                         "Для TCP-проверки нужны host, port > 0 и timeout от 1 мс до 60 с");
        return result;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int resolve_status =
        ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (resolve_status != 0) {
        mark_unavailable(result,
                         "Не удалось разрешить TCP-узел: " +
                             std::string{::gai_strerror(resolve_status)});
        result.elapsed_milliseconds = elapsed_milliseconds(start);
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last_error = "Нет доступных адресов для TCP-подключения";

    for (const struct addrinfo* item = addresses; item != nullptr; item = item->ai_next) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            last_error = "Истёк тайм-аут TCP-подключения";
            break;
        }

        const int descriptor = ::socket(item->ai_family,
                                        item->ai_socktype | SOCK_CLOEXEC,
                                        item->ai_protocol);
        if (descriptor < 0) {
            last_error = "Не удалось создать TCP-сокет: " + socket_error_text(errno);
            continue;
        }

        const int current_flags = ::fcntl(descriptor, F_GETFL, 0);
        if (current_flags < 0 ||
            ::fcntl(descriptor, F_SETFL, current_flags | O_NONBLOCK) < 0) {
            last_error = "Не удалось включить non-blocking режим: " +
                         socket_error_text(errno);
            ::close(descriptor);
            continue;
        }

        const int connect_status =
            ::connect(descriptor, item->ai_addr, item->ai_addrlen);
        if (connect_status == 0) {
            ::close(descriptor);
            result.reachable = true;
            result.elapsed_milliseconds = elapsed_milliseconds(start);
            mark_available(result);
            return result;
        }

        if (errno != EINPROGRESS) {
            last_error = "TCP connect завершился ошибкой: " + socket_error_text(errno);
            ::close(descriptor);
            continue;
        }

        bool connected{false};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto bounded = std::max<std::int64_t>(
                1, std::min<std::int64_t>(remaining.count(),
                                          std::numeric_limits<int>::max()));
            struct pollfd poll_descriptor {};
            poll_descriptor.fd = descriptor;
            poll_descriptor.events = POLLOUT;

            const int poll_status =
                ::poll(&poll_descriptor, 1, static_cast<int>(bounded));
            if (poll_status < 0 && errno == EINTR) {
                continue;
            }
            if (poll_status == 0) {
                last_error = "Истёк тайм-аут TCP-подключения";
                break;
            }
            if (poll_status < 0) {
                last_error = "Ошибка ожидания TCP-подключения: " +
                             socket_error_text(errno);
                break;
            }

            int socket_error{0};
            socklen_t error_length = sizeof(socket_error);
            if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
                             &socket_error, &error_length) != 0) {
                last_error = "Не удалось получить результат TCP connect: " +
                             socket_error_text(errno);
                break;
            }
            if (socket_error == 0) {
                connected = true;
                break;
            }
            last_error = "TCP connect завершился ошибкой: " +
                         socket_error_text(socket_error);
            break;
        }

        ::close(descriptor);
        if (connected) {
            ::freeaddrinfo(addresses);
            result.reachable = true;
            result.elapsed_milliseconds = elapsed_milliseconds(start);
            mark_available(result);
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            last_error = "Истёк тайм-аут TCP-подключения";
            break;
        }
    }

    ::freeaddrinfo(addresses);
    result.elapsed_milliseconds = elapsed_milliseconds(start);
    result.reachable = false;
    mark_available(result);
    result.reason = std::move(last_error);
    return result;
}

NetworkDiagnosticsSnapshot collect_network_diagnostics() {
    NetworkDiagnosticsSnapshot result{};
    result.interfaces = read_network_interfaces();
    result.addresses = read_interface_addresses();
    result.default_routes = read_default_route();
    return result;
}

}  // namespace dockstation::platform::linux_diagnostics
