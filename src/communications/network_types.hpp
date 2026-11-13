#pragma once

#include <cstdint>
#include <string>

namespace dockstation::communications {

struct HttpRequest {
    std::string method{};
    std::string target{};
    std::string version{};
};

struct HttpParseResult {
    bool valid{false};
    bool too_large{false};
    HttpRequest request{};
    std::string error{};
};

struct ApiResponse {
    std::uint16_t status_code{500U};
    std::string reason_phrase{"Internal Server Error"};
    std::string body{};
    std::string allow{};
};

enum class LocalApiServeResult : std::uint8_t {
    Handled,
    TimedOut,
    Error
};

struct LocalApiServerStatus {
    bool running{false};
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{0U};
    std::string reason{};
};

}  // namespace dockstation::communications
