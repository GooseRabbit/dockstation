#pragma once

#include <csignal>
#include <cstdint>
#include <string>

#include "communications/network_types.hpp"

namespace dockstation::communications {

[[nodiscard]] HttpParseResult parse_http_request(const std::string& raw_request);
[[nodiscard]] ApiResponse dispatch_api_request(const HttpRequest& request);

class LocalApiServer {
public:
    LocalApiServer() = default;
    LocalApiServer(const LocalApiServer&) = delete;
    LocalApiServer& operator=(const LocalApiServer&) = delete;
    ~LocalApiServer();

    // Port 0 requests an ephemeral port and is intended for host tests only.
    [[nodiscard]] bool start(std::uint32_t port);
    [[nodiscard]] LocalApiServeResult serve_once(int timeout_milliseconds);
    void stop() noexcept;
    [[nodiscard]] LocalApiServerStatus status() const;

private:
    int listen_descriptor_{-1};
    std::uint16_t bound_port_{0U};
    std::string error_{};
};

[[nodiscard]] int run_local_api_server(
    std::uint32_t port,
    const volatile std::sig_atomic_t* shutdown_requested);

}  // namespace dockstation::communications
