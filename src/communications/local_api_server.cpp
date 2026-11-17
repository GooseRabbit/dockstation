#include "communications/local_api_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "communications/api_serialization.hpp"
#include "platform/linux/runtime_config.hpp"
#include "platform/logging/logger.hpp"

namespace dockstation::communications {
namespace {

constexpr std::size_t kMaximumHttpRequestBytes = 8192U;
constexpr int kClientIoTimeoutMilliseconds = 2500;
constexpr std::size_t kMaximumHttpClients = 8U;
constexpr std::size_t kMaximumHeaderCount = 64U;

bool set_nonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string trim_header_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return std::string{value};
}

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool valid_header_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char ch : name) {
        const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

HttpParseResult invalid_request(std::string reason, bool too_large = false) {
    HttpParseResult result{};
    result.too_large = too_large;
    result.error = std::move(reason);
    return result;
}

HttpParseResult parse_http_request_impl(const std::string& raw_request) {
    if (raw_request.size() > kMaximumHttpRequestBytes) {
        return invalid_request("HTTP request exceeds the 8192-byte limit", true);
    }
    const std::size_t separator = raw_request.find("\r\n\r\n");
    if (separator == std::string::npos) {
        return invalid_request("HTTP headers are incomplete");
    }
    if (separator + 4U != raw_request.size()) {
        return invalid_request("Request bodies are not supported");
    }

    const std::size_t request_line_end = raw_request.find("\r\n");
    if (request_line_end == std::string::npos || request_line_end > separator) {
        return invalid_request("HTTP request line is missing");
    }
    const std::string_view request_line{raw_request.data(), request_line_end};
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space =
        first_space == std::string_view::npos
            ? std::string_view::npos
            : request_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos ||
        request_line.find(' ', second_space + 1U) != std::string_view::npos) {
        return invalid_request("Malformed HTTP request line");
    }

    const std::string_view method = request_line.substr(0U, first_space);
    const std::string_view target =
        request_line.substr(first_space + 1U, second_space - first_space - 1U);
    const std::string_view version = request_line.substr(second_space + 1U);
    if (method.empty() || target.empty() ||
        (version != "HTTP/1.0" && version != "HTTP/1.1")) {
        return invalid_request("Unsupported or malformed HTTP request line");
    }
    for (const unsigned char ch : method) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) {
            return invalid_request("Invalid HTTP method token");
        }
    }
    if (target.front() != '/') {
        return invalid_request("Only origin-form request targets are supported");
    }
    for (const unsigned char ch : target) {
        if (ch <= 0x20U || ch == 0x7FU || ch >= 0x80U) {
            return invalid_request("Invalid HTTP request target");
        }
    }

    bool content_length_seen = false;
    std::size_t header_count = 0U;
    std::size_t offset = request_line_end + 2U;
    while (offset < separator) {
        const std::size_t line_end = raw_request.find("\r\n", offset);
        if (line_end == std::string::npos || line_end > separator) {
            return invalid_request("Malformed HTTP header line");
        }
        const std::string_view line{
            raw_request.data() + offset, line_end - offset};
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            return invalid_request("Empty or folded HTTP headers are not supported");
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos ||
            !valid_header_name(line.substr(0U, colon))) {
            return invalid_request("Malformed HTTP header name");
        }
        ++header_count;
        if (header_count > kMaximumHeaderCount) {
            return invalid_request("Too many HTTP headers");
        }
        const std::string name = lower_ascii(std::string{line.substr(0U, colon)});
        const std::string value = trim_header_value(line.substr(colon + 1U));
        if (name == "transfer-encoding") {
            return invalid_request("Transfer-Encoding is not supported");
        }
        if (name == "content-length") {
            if (content_length_seen || value != "0") {
                return invalid_request("Only an absent or zero Content-Length is supported");
            }
            content_length_seen = true;
        }
        for (const unsigned char ch : value) {
            if ((ch < 0x20U && ch != '\t') || ch == 0x7FU) {
                return invalid_request("Invalid control character in HTTP header");
            }
        }
        offset = line_end + 2U;
    }

    HttpParseResult result{};
    result.valid = true;
    result.request.method.assign(method.data(), method.size());
    result.request.target.assign(target.data(), target.size());
    result.request.version.assign(version.data(), version.size());
    return result;
}

ApiResponse dispatch_api_request_impl(
    const HttpRequest& request,
    const platform::linux_platform::ControllerLinkService* controller_link) {
    if (request.target.find('?') != std::string::npos ||
        request.target.find('#') != std::string::npos) {
        return {400U, "Bad Request",
                serialize_error_response(
                    "INVALID_REQUEST_TARGET",
                    "Query and fragment components are not supported"),
                {}};
    }
    if (request.method != "GET") {
        return {405U, "Method Not Allowed",
                serialize_error_response(
                    "METHOD_NOT_ALLOWED",
                    "HTTP method is not supported for this route"),
                "GET"};
    }
    if (request.target == "/api/v1/health") {
        return {200U, "OK", serialize_health_response(), {}};
    }
    if (request.target == "/api/v1/system/diagnostics") {
        const auto controller_status = controller_link == nullptr
            ? platform::linux_platform::ControllerLinkServiceStatus{}
            : controller_link->status();
        const auto* telemetry_snapshot = controller_link == nullptr
            ? nullptr : &controller_link->telemetry_snapshot();
        const std::string body = serialize_system_diagnostics(
            &controller_status, nullptr, telemetry_snapshot);
        if (body.empty()) {
            return {500U, "Internal Server Error",
                    serialize_error_response(
                        "DIAGNOSTICS_COLLECTION_FAILED",
                        "The diagnostics timestamp could not be created"),
                    {}};
        }
        return {200U, "OK", body, {}};
    }
    if (request.target == "/api/v1/controller/link") {
        const auto link_status = controller_link == nullptr
            ? platform::linux_platform::ControllerLinkServiceStatus{}
            : controller_link->status();
        return {200U, "OK", serialize_controller_link_status(link_status), {}};
    }
    return {404U, "Not Found",
            serialize_error_response("NOT_FOUND", "API route not found"), {}};
}

std::string build_http_response(const ApiResponse& response) {
    std::string result = "HTTP/1.1 " + std::to_string(response.status_code) +
        " " + response.reason_phrase + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(response.body.size()) + "\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n";
    if (!response.allow.empty()) {
        result += "Allow: " + response.allow + "\r\n";
    }
    result += "\r\n";
    result += response.body;
    return result;
}

bool write_log(platform::logging::LogLevel level,
               const std::string& message,
               const std::vector<platform::logging::LogField>& fields = {}) {
    return platform::logging::write_log_record(
        level, "local_api_server", message, fields).status ==
        platform::logging::LoggerStatus::Available;
}

}  // namespace

HttpParseResult parse_http_request(const std::string& raw_request) {
    return parse_http_request_impl(raw_request);
}

ApiResponse dispatch_api_request(
    const HttpRequest& request,
    const platform::linux_platform::ControllerLinkService* controller_link) {
    return dispatch_api_request_impl(request, controller_link);
}

LocalApiServer::~LocalApiServer() {
    stop();
}

bool LocalApiServer::start(
    std::uint32_t port,
    platform::linux_platform::ControllerLinkService* controller_link) {
    if (listen_descriptor_ >= 0) {
        error_ = "Local API server is already running";
        return false;
    }
    if (port > 65535U) {
        error_ = "API port is outside the valid range";
        return false;
    }

    const int descriptor =
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (descriptor < 0) {
        error_ = std::string{"Cannot create API socket: "} + std::strerror(errno);
        return false;
    }

    int reuse_address = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address)) != 0) {
        error_ = std::string{"Cannot configure API socket: "} + std::strerror(errno);
        ::close(descriptor);
        return false;
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, platform::linux_runtime::kGatewayBindAddress,
                    &address.sin_addr) != 1) {
        error_ = "Configured loopback bind address is invalid";
        ::close(descriptor);
        return false;
    }
    if (::bind(descriptor, reinterpret_cast<const struct sockaddr*>(&address),
               sizeof(address)) != 0) {
        error_ = std::string{"Cannot bind API socket to loopback: "} +
                 std::strerror(errno);
        ::close(descriptor);
        return false;
    }
    if (::listen(descriptor, 8) != 0 || !set_nonblocking(descriptor)) {
        error_ = std::string{"Cannot listen on API socket: "} + std::strerror(errno);
        ::close(descriptor);
        return false;
    }

    struct sockaddr_in actual_address {};
    socklen_t actual_length = sizeof(actual_address);
    if (::getsockname(descriptor,
                      reinterpret_cast<struct sockaddr*>(&actual_address),
                      &actual_length) != 0) {
        error_ = std::string{"Cannot read API socket address: "} + std::strerror(errno);
        ::close(descriptor);
        return false;
    }

    listen_descriptor_ = descriptor;
    bound_port_ = ntohs(actual_address.sin_port);
    controller_link_ = controller_link;
    error_.clear();
    return true;
}

LocalApiServeResult LocalApiServer::serve_once(int timeout_milliseconds) {
    if (listen_descriptor_ < 0) {
        error_ = "Local API server is not running";
        return LocalApiServeResult::Error;
    }

    // One poll call services the listener and every active client. No client
    // performs a blocking read/write, so the caller can poll ControllerLink
    // again on the next event-loop iteration.
    std::vector<struct pollfd> descriptors;
    descriptors.reserve(clients_.size() + 1U);
    descriptors.push_back({listen_descriptor_, POLLIN, 0});
    for (const Client& client : clients_) {
        descriptors.push_back({
            client.descriptor,
            static_cast<short>(client.responding ? POLLOUT : POLLIN),
            0});
    }

    const int poll_result = ::poll(
        descriptors.data(), static_cast<nfds_t>(descriptors.size()),
        std::max(0, timeout_milliseconds));
    if (poll_result < 0 && errno != EINTR) {
        error_ = std::string{"API event poll failed: "} + std::strerror(errno);
        return LocalApiServeResult::Error;
    }

    if (poll_result > 0 &&
        (descriptors.front().revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        error_ = "API listener reported an invalid poll state";
        return LocalApiServeResult::Error;
    }

    bool handled = false;
    if (poll_result > 0 && (descriptors.front().revents & POLLIN) != 0) {
        // Drain the accept queue, but never exceed the explicit active-client cap.
        while (clients_.size() < kMaximumHttpClients) {
            struct sockaddr_in client_address {};
            socklen_t client_length = sizeof(client_address);
            const int client_fd = ::accept(
                listen_descriptor_,
                reinterpret_cast<struct sockaddr*>(&client_address),
                &client_length);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                error_ = std::string{"API accept failed: "} + std::strerror(errno);
                return LocalApiServeResult::Error;
            }
            const int descriptor_flags = ::fcntl(client_fd, F_GETFD, 0);
            if (!set_nonblocking(client_fd) || descriptor_flags < 0 ||
                ::fcntl(client_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
                ::close(client_fd);
                continue;
            }
            Client accepted{};
            accepted.descriptor = client_fd;
            accepted.deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds{kClientIoTimeoutMilliseconds};
            accepted.request.reserve(1024U);
            clients_.push_back(std::move(accepted));
            handled = true;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    // Walk the polled clients backwards: erasing a completed connection then
    // cannot change the descriptor-to-client indices of earlier entries.
    const std::size_t polled_clients =
        std::min(clients_.size(), descriptors.size() - 1U);
    for (std::size_t poll_index = polled_clients; poll_index > 0U; --poll_index) {
        const std::size_t index = poll_index - 1U;
        Client& client = clients_[index];
        bool close_client = now >= client.deadline;
        const short events = descriptors[poll_index].revents;
        if (!close_client && (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close_client = true;
        }

        if (!close_client && !client.responding && (events & POLLIN) != 0) {
            char buffer[1024];
            for (;;) {
                const ssize_t count = ::recv(client.descriptor, buffer, sizeof(buffer), 0);
                if (count > 0) {
                    client.request.append(buffer, static_cast<std::size_t>(count));
                    if (client.request.size() > kMaximumHttpRequestBytes ||
                        client.request.find("\r\n\r\n") != std::string::npos) {
                        break;
                    }
                    continue;
                }
                if (count == 0) {
                    close_client = true;
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close_client = true;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }

            if (!close_client &&
                (client.request.size() > kMaximumHttpRequestBytes ||
                 client.request.find("\r\n\r\n") != std::string::npos)) {
                ApiResponse response{};
                if (client.request.size() > kMaximumHttpRequestBytes) {
                    response = {413U, "Payload Too Large",
                        serialize_error_response("REQUEST_TOO_LARGE",
                            "HTTP request exceeds the 8192-byte limit"), {}};
                } else {
                    const HttpParseResult parsed = parse_http_request_impl(client.request);
                    if (!parsed.valid) {
                        response = {400U, "Bad Request",
                            serialize_error_response("INVALID_HTTP_REQUEST", parsed.error), {}};
                    } else {
                        response = dispatch_api_request_impl(parsed.request, controller_link_);
                    }
                }
                client.response = build_http_response(response);
                client.responding = true;
                client.deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{kClientIoTimeoutMilliseconds};
                handled = true;
            }
        }

        if (!close_client && client.responding && (events & POLLOUT) != 0) {
            while (client.response_offset < client.response.size()) {
                const ssize_t count = ::send(
                    client.descriptor,
                    client.response.data() + client.response_offset,
                    client.response.size() - client.response_offset,
                    MSG_NOSIGNAL);
                if (count > 0) {
                    client.response_offset += static_cast<std::size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                close_client = true;
                break;
            }
            if (client.response_offset == client.response.size()) {
                close_client = true;
                handled = true;
            }
        }

        if (close_client) {
            ::close(client.descriptor);
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    return handled ? LocalApiServeResult::Handled
                   : LocalApiServeResult::TimedOut;
}

void LocalApiServer::stop() noexcept {
    for (const Client& client : clients_) {
        ::close(client.descriptor);
    }
    clients_.clear();
    if (listen_descriptor_ >= 0) {
        ::close(listen_descriptor_);
        listen_descriptor_ = -1;
    }
    bound_port_ = 0U;
    controller_link_ = nullptr;
}

LocalApiServerStatus LocalApiServer::status() const {
    return {listen_descriptor_ >= 0, "127.0.0.1", bound_port_, error_};
}

int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested,
    platform::linux_platform::ControllerLinkServiceConfig controller_config) {
    platform::linux_platform::ControllerLinkService controller_link;
    return run_local_api_server(
        port, shutdown_requested, controller_link, controller_config);
}

int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested,
    platform::linux_platform::ControllerLinkService& controller_link,
    platform::linux_platform::ControllerLinkServiceConfig controller_config) {
    if (shutdown_requested == nullptr) {
        (void)write_log(platform::logging::LogLevel::Error,
                        "Для локального API не передан флаг штатного завершения");
        return 1;
    }

    const auto controller_status = controller_link.start(controller_config);
    if (controller_status.state ==
        platform::linux_platform::ControllerLinkServiceState::Error) {
        (void)write_log(platform::logging::LogLevel::Warning,
                        "Внутренний контроллерный канал не запущен",
                        {{"error_code", controller_status.error_code == nullptr
                            ? "" : controller_status.error_code}});
    }

    LocalApiServer server;
    if (!server.start(port, &controller_link)) {
        (void)write_log(platform::logging::LogLevel::Error,
                        "Не удалось запустить локальный read-only API",
                        {{"reason", server.status().reason},
                         {"bind_address", "127.0.0.1"},
                         {"port", std::to_string(port)}});
        controller_link.stop();
        return 1;
    }

    (void)write_log(platform::logging::LogLevel::Info,
                    "Локальный read-only API запущен",
                    {{"bind_address", "127.0.0.1"},
                     {"port", std::to_string(server.status().port)}});

    while (*shutdown_requested == 0) {
        controller_link.poll();
        const LocalApiServeResult result = server.serve_once(250);
        if (result == LocalApiServeResult::Error) {
            (void)write_log(platform::logging::LogLevel::Error,
                            "Ошибка локального API listener",
                            {{"reason", server.status().reason}});
            server.stop();
            controller_link.stop();
            return 1;
        }
    }

    server.stop();
    controller_link.stop();
    (void)write_log(platform::logging::LogLevel::Info,
                    "Локальный read-only API остановлен");
    return 0;
}

}  // namespace dockstation::communications
