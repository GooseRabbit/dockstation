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
constexpr std::size_t kMaximumHeaderCount = 64U;

bool set_nonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wait_for(int descriptor, short events, int timeout_milliseconds) {
    struct pollfd item {};
    item.fd = descriptor;
    item.events = events;
    const int result = ::poll(&item, 1, timeout_milliseconds);
    if (result <= 0) {
        return false;
    }
    if ((item.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return true;
    }
    return (item.revents & events) != 0;
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

ApiResponse dispatch_api_request_impl(const HttpRequest& request) {
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
        const std::string body = serialize_system_diagnostics();
        if (body.empty()) {
            return {500U, "Internal Server Error",
                    serialize_error_response(
                        "DIAGNOSTICS_COLLECTION_FAILED",
                        "The diagnostics timestamp could not be created"),
                    {}};
        }
        return {200U, "OK", body, {}};
    }
    return {404U, "Not Found",
            serialize_error_response("NOT_FOUND", "API route not found"), {}};
}

bool read_request(int descriptor, std::string& request) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds{kClientIoTimeoutMilliseconds};
    char buffer[1024];
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int wait_ms = static_cast<int>(
            std::max<std::int64_t>(1, std::min<std::int64_t>(remaining.count(), 500)));
        if (!wait_for(descriptor, POLLIN, wait_ms)) {
            continue;
        }
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count == 0) {
            return false;
        }
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        request.append(buffer, static_cast<std::size_t>(count));
        if (request.size() > kMaximumHttpRequestBytes) {
            return true;
        }
        if (request.find("\r\n\r\n") != std::string::npos) {
            return true;
        }
    }
}

bool send_all(int descriptor, const std::string& response) {
    std::size_t offset = 0U;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds{kClientIoTimeoutMilliseconds};
    while (offset < response.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int wait_ms = static_cast<int>(
            std::max<std::int64_t>(1, std::min<std::int64_t>(remaining.count(), 500)));
        if (!wait_for(descriptor, POLLOUT, wait_ms)) {
            continue;
        }
        const ssize_t count = ::send(
            descriptor, response.data() + offset, response.size() - offset,
            MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
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

void process_client(int descriptor) {
    std::string raw_request;
    raw_request.reserve(1024U);
    ApiResponse response{};
    if (!read_request(descriptor, raw_request)) {
        response = {
            400U, "Bad Request",
            serialize_error_response("INVALID_HTTP_REQUEST",
                                     "HTTP request could not be read"),
            {}};
    } else {
        const HttpParseResult parsed = parse_http_request_impl(raw_request);
        if (!parsed.valid) {
            response = parsed.too_large
                ? ApiResponse{413U, "Payload Too Large",
                              serialize_error_response("REQUEST_TOO_LARGE",
                                                       "HTTP request exceeds the 8192-byte limit"),
                              {}}
                : ApiResponse{400U, "Bad Request",
                              serialize_error_response("INVALID_HTTP_REQUEST",
                                                       parsed.error),
                              {}};
        } else {
            response = dispatch_api_request_impl(parsed.request);
        }
    }
    (void)send_all(descriptor, build_http_response(response));
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

ApiResponse dispatch_api_request(const HttpRequest& request) {
    return dispatch_api_request_impl(request);
}

LocalApiServer::~LocalApiServer() {
    stop();
}

bool LocalApiServer::start(std::uint32_t port) {
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
    error_.clear();
    return true;
}

LocalApiServeResult LocalApiServer::serve_once(int timeout_milliseconds) {
    if (listen_descriptor_ < 0) {
        error_ = "Local API server is not running";
        return LocalApiServeResult::Error;
    }
    struct pollfd item {};
    item.fd = listen_descriptor_;
    item.events = POLLIN;
    const int poll_result =
        ::poll(&item, 1, std::max(0, timeout_milliseconds));
    if (poll_result == 0) {
        return LocalApiServeResult::TimedOut;
    }
    if (poll_result < 0) {
        if (errno == EINTR) {
            return LocalApiServeResult::TimedOut;
        }
        error_ = std::string{"API listener poll failed: "} + std::strerror(errno);
        return LocalApiServeResult::Error;
    }
    if ((item.revents & POLLIN) == 0) {
        error_ = "API listener reported an unexpected poll state";
        return LocalApiServeResult::Error;
    }

    struct sockaddr_in client_address {};
    socklen_t client_length = sizeof(client_address);
    const int client = ::accept(
        listen_descriptor_,
        reinterpret_cast<struct sockaddr*>(&client_address),
        &client_length);
    if (client < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return LocalApiServeResult::TimedOut;
        }
        error_ = std::string{"API accept failed: "} + std::strerror(errno);
        return LocalApiServeResult::Error;
    }
    if (!set_nonblocking(client)) {
        error_ = std::string{"Cannot configure API client socket: "} +
                 std::strerror(errno);
        ::close(client);
        return LocalApiServeResult::Error;
    }
    const int descriptor_flags = ::fcntl(client, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        ::fcntl(client, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        error_ = std::string{"Cannot set close-on-exec for API client socket: "} +
                 std::strerror(errno);
        ::close(client);
        return LocalApiServeResult::Error;
    }
    process_client(client);
    ::close(client);
    return LocalApiServeResult::Handled;
}

void LocalApiServer::stop() noexcept {
    if (listen_descriptor_ >= 0) {
        ::close(listen_descriptor_);
        listen_descriptor_ = -1;
    }
    bound_port_ = 0U;
}

LocalApiServerStatus LocalApiServer::status() const {
    return {listen_descriptor_ >= 0, "127.0.0.1", bound_port_, error_};
}

int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested) {
    if (shutdown_requested == nullptr) {
        (void)write_log(platform::logging::LogLevel::Error,
                        "Для локального API не передан флаг штатного завершения");
        return 1;
    }

    LocalApiServer server;
    if (!server.start(port)) {
        (void)write_log(platform::logging::LogLevel::Error,
                        "Не удалось запустить локальный read-only API",
                        {{"reason", server.status().reason},
                         {"bind_address", "127.0.0.1"},
                         {"port", std::to_string(port)}});
        return 1;
    }

    (void)write_log(platform::logging::LogLevel::Info,
                    "Локальный read-only API запущен",
                    {{"bind_address", "127.0.0.1"},
                     {"port", std::to_string(server.status().port)}});

    while (*shutdown_requested == 0) {
        const LocalApiServeResult result = server.serve_once(250);
        if (result == LocalApiServeResult::Error) {
            (void)write_log(platform::logging::LogLevel::Error,
                            "Ошибка локального API listener",
                            {{"reason", server.status().reason}});
            server.stop();
            return 1;
        }
    }

    server.stop();
    (void)write_log(platform::logging::LogLevel::Info,
                    "Локальный read-only API остановлен");
    return 0;
}

}  // namespace dockstation::communications
